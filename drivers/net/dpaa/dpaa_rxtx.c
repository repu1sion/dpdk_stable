/*-
 *   BSD LICENSE
 *
 *   Copyright 2016 Freescale Semiconductor, Inc. All rights reserved.
 *   Copyright 2017 NXP.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of  Freescale Semiconductor, Inc nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* System headers */
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <sched.h>
#include <pthread.h>

#include <rte_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_ring.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>

#include "dpaa_ethdev.h"
#include "dpaa_rxtx.h"
#include <rte_dpaa_bus.h>
#include <dpaa_mempool.h>

#include <fsl_usd.h>
#include <fsl_qman.h>
#include <fsl_bman.h>
#include <of.h>
#include <netcfg.h>

#define DPAA_MBUF_TO_CONTIG_FD(_mbuf, _fd, _bpid) \
	do { \
		(_fd)->cmd = 0; \
		(_fd)->opaque_addr = 0; \
		(_fd)->opaque = QM_FD_CONTIG << DPAA_FD_FORMAT_SHIFT; \
		(_fd)->opaque |= ((_mbuf)->data_off) << DPAA_FD_OFFSET_SHIFT; \
		(_fd)->opaque |= (_mbuf)->pkt_len; \
		(_fd)->addr = (_mbuf)->buf_physaddr; \
		(_fd)->bpid = _bpid; \
	} while (0)

static inline struct rte_mbuf *dpaa_eth_fd_to_mbuf(struct qm_fd *fd,
							uint32_t ifid)
{
	struct dpaa_bp_info *bp_info = DPAA_BPID_TO_POOL_INFO(fd->bpid);
	struct rte_mbuf *mbuf;
	void *ptr;
	uint16_t offset =
		(fd->opaque & DPAA_FD_OFFSET_MASK) >> DPAA_FD_OFFSET_SHIFT;
	uint32_t length = fd->opaque & DPAA_FD_LENGTH_MASK;

	DPAA_DP_LOG(DEBUG, " FD--->MBUF");

	/* Ignoring case when format != qm_fd_contig */
	ptr = rte_dpaa_mem_ptov(fd->addr);
	/* Ignoring case when ptr would be NULL. That is only possible incase
	 * of a corrupted packet
	 */

	mbuf = (struct rte_mbuf *)((char *)ptr - bp_info->meta_data_size);
	/* Prefetch the Parse results and packet data to L1 */
	rte_prefetch0((void *)((uint8_t *)ptr + DEFAULT_RX_ICEOF));
	rte_prefetch0((void *)((uint8_t *)ptr + offset));

	mbuf->data_off = offset;
	mbuf->data_len = length;
	mbuf->pkt_len = length;

	mbuf->port = ifid;
	mbuf->nb_segs = 1;
	mbuf->ol_flags = 0;
	mbuf->next = NULL;
	rte_mbuf_refcnt_set(mbuf, 1);

	return mbuf;
}

uint16_t dpaa_eth_queue_rx(void *q,
			   struct rte_mbuf **bufs,
			   uint16_t nb_bufs)
{
	struct qman_fq *fq = q;
	struct qm_dqrr_entry *dq;
	uint32_t num_rx = 0, ifid = ((struct dpaa_if *)fq->dpaa_intf)->ifid;
	int ret;

	ret = rte_dpaa_portal_init((void *)0);
	if (ret) {
		DPAA_PMD_ERR("Failure in affining portal");
		return 0;
	}

	ret = qman_set_vdq(fq, (nb_bufs > DPAA_MAX_DEQUEUE_NUM_FRAMES) ?
				DPAA_MAX_DEQUEUE_NUM_FRAMES : nb_bufs);
	if (ret)
		return 0;

	do {
		dq = qman_dequeue(fq);
		if (!dq)
			continue;
		bufs[num_rx++] = dpaa_eth_fd_to_mbuf(&dq->fd, ifid);
		qman_dqrr_consume(fq, dq);
	} while (fq->flags & QMAN_FQ_STATE_VDQCR);

	return num_rx;
}

static void *dpaa_get_pktbuf(struct dpaa_bp_info *bp_info)
{
	int ret;
	uint64_t buf = 0;
	struct bm_buffer bufs;

	ret = bman_acquire(bp_info->bp, &bufs, 1, 0);
	if (ret <= 0) {
		DPAA_PMD_WARN("Failed to allocate buffers %d", ret);
		return (void *)buf;
	}

	DPAA_DP_LOG(DEBUG, "got buffer 0x%lx from pool %d",
		    (uint64_t)bufs.addr, bufs.bpid);

	buf = (uint64_t)rte_dpaa_mem_ptov(bufs.addr) - bp_info->meta_data_size;
	if (!buf)
		goto out;

out:
	return (void *)buf;
}

static struct rte_mbuf *dpaa_get_dmable_mbuf(struct rte_mbuf *mbuf,
					     struct dpaa_if *dpaa_intf)
{
	struct rte_mbuf *dpaa_mbuf;

	/* allocate pktbuffer on bpid for dpaa port */
	dpaa_mbuf = dpaa_get_pktbuf(dpaa_intf->bp_info);
	if (!dpaa_mbuf)
		return NULL;

	memcpy((uint8_t *)(dpaa_mbuf->buf_addr) + mbuf->data_off, (void *)
		((uint8_t *)(mbuf->buf_addr) + mbuf->data_off), mbuf->pkt_len);

	/* Copy only the required fields */
	dpaa_mbuf->data_off = mbuf->data_off;
	dpaa_mbuf->pkt_len = mbuf->pkt_len;
	dpaa_mbuf->ol_flags = mbuf->ol_flags;
	dpaa_mbuf->packet_type = mbuf->packet_type;
	dpaa_mbuf->tx_offload = mbuf->tx_offload;
	rte_pktmbuf_free(mbuf);
	return dpaa_mbuf;
}

/* Handle mbufs which are not segmented (non SG) */
static inline void
tx_on_dpaa_pool_unsegmented(struct rte_mbuf *mbuf,
			    struct dpaa_bp_info *bp_info,
			    struct qm_fd *fd_arr)
{
	struct rte_mbuf *mi = NULL;

	if (RTE_MBUF_DIRECT(mbuf)) {
		if (rte_mbuf_refcnt_read(mbuf) > 1) {
			/* In case of direct mbuf and mbuf being cloned,
			 * BMAN should _not_ release buffer.
			 */
			DPAA_MBUF_TO_CONTIG_FD(mbuf, fd_arr, 0xff);
			/* Buffer should be releasd by EAL */
			rte_mbuf_refcnt_update(mbuf, -1);
		} else {
			/* In case of direct mbuf and no cloning, mbuf can be
			 * released by BMAN.
			 */
			DPAA_MBUF_TO_CONTIG_FD(mbuf, fd_arr, bp_info->bpid);
		}
	} else {
		/* This is data-containing core mbuf: 'mi' */
		mi = rte_mbuf_from_indirect(mbuf);
		if (rte_mbuf_refcnt_read(mi) > 1) {
			/* In case of indirect mbuf, and mbuf being cloned,
			 * BMAN should _not_ release it and let EAL release
			 * it through pktmbuf_free below.
			 */
			DPAA_MBUF_TO_CONTIG_FD(mbuf, fd_arr, 0xff);
		} else {
			/* In case of indirect mbuf, and no cloning, core mbuf
			 * should be released by BMAN.
			 * Increate refcnt of core mbuf so that when
			 * pktmbuf_free is called and mbuf is released, EAL
			 * doesn't try to release core mbuf which would have
			 * been released by BMAN.
			 */
			rte_mbuf_refcnt_update(mi, 1);
			DPAA_MBUF_TO_CONTIG_FD(mbuf, fd_arr, bp_info->bpid);
		}
		rte_pktmbuf_free(mbuf);
	}
}

/* Handle all mbufs on dpaa BMAN managed pool */
static inline uint16_t
tx_on_dpaa_pool(struct rte_mbuf *mbuf,
		struct dpaa_bp_info *bp_info,
		struct qm_fd *fd_arr)
{
	DPAA_DP_LOG(DEBUG, "BMAN offloaded buffer, mbuf: %p", mbuf);

	if (mbuf->nb_segs == 1) {
		/* Case for non-segmented buffers */
		tx_on_dpaa_pool_unsegmented(mbuf, bp_info, fd_arr);
	} else {
		DPAA_PMD_DEBUG("Number of Segments not supported");
		return 1;
	}

	return 0;
}

/* Handle all mbufs on an external pool (non-dpaa) */
static inline uint16_t
tx_on_external_pool(struct qman_fq *txq, struct rte_mbuf *mbuf,
		    struct qm_fd *fd_arr)
{
	struct dpaa_if *dpaa_intf = txq->dpaa_intf;
	struct rte_mbuf *dmable_mbuf;

	DPAA_DP_LOG(DEBUG, "Non-BMAN offloaded buffer."
		    "Allocating an offloaded buffer");
	dmable_mbuf = dpaa_get_dmable_mbuf(mbuf, dpaa_intf);
	if (!dmable_mbuf) {
		DPAA_DP_LOG(DEBUG, "no dpaa buffers.");
		return 1;
	}

	DPAA_MBUF_TO_CONTIG_FD(mbuf, fd_arr, dpaa_intf->bp_info->bpid);

	return 0;
}

uint16_t
dpaa_eth_queue_tx(void *q, struct rte_mbuf **bufs, uint16_t nb_bufs)
{
	struct rte_mbuf *mbuf, *mi = NULL;
	struct rte_mempool *mp;
	struct dpaa_bp_info *bp_info;
	struct qm_fd fd_arr[MAX_TX_RING_SLOTS];
	uint32_t frames_to_send, loop, i = 0;
	uint16_t state;
	int ret;

	ret = rte_dpaa_portal_init((void *)0);
	if (ret) {
		DPAA_PMD_ERR("Failure in affining portal");
		return 0;
	}

	DPAA_DP_LOG(DEBUG, "Transmitting %d buffers on queue: %p", nb_bufs, q);

	while (nb_bufs) {
		frames_to_send = (nb_bufs >> 3) ? MAX_TX_RING_SLOTS : nb_bufs;
		for (loop = 0; loop < frames_to_send; loop++, i++) {
			mbuf = bufs[i];
			if (RTE_MBUF_DIRECT(mbuf)) {
				mp = mbuf->pool;
			} else {
				mi = rte_mbuf_from_indirect(mbuf);
				mp = mi->pool;
			}

			bp_info = DPAA_MEMPOOL_TO_POOL_INFO(mp);
			if (likely(mp->ops_index == bp_info->dpaa_ops_index)) {
				state = tx_on_dpaa_pool(mbuf, bp_info,
							&fd_arr[loop]);
				if (unlikely(state)) {
					/* Set frames_to_send & nb_bufs so
					 * that packets are transmitted till
					 * previous frame.
					 */
					frames_to_send = loop;
					nb_bufs = loop;
					goto send_pkts;
				}
			} else {
				state = tx_on_external_pool(q, mbuf,
							    &fd_arr[loop]);
				if (unlikely(state)) {
					/* Set frames_to_send & nb_bufs so
					 * that packets are transmitted till
					 * previous frame.
					 */
					frames_to_send = loop;
					nb_bufs = loop;
					goto send_pkts;
				}
			}
		}

send_pkts:
		loop = 0;
		while (loop < frames_to_send) {
			loop += qman_enqueue_multi(q, &fd_arr[loop],
					frames_to_send - loop);
		}
		nb_bufs -= frames_to_send;
	}

	DPAA_DP_LOG(DEBUG, "Transmitted %d buffers on queue: %p", i, q);

	return i;
}

uint16_t dpaa_eth_tx_drop_all(void *q  __rte_unused,
			      struct rte_mbuf **bufs __rte_unused,
		uint16_t nb_bufs __rte_unused)
{
	DPAA_DP_LOG(DEBUG, "Drop all packets");

	/* Drop all incoming packets. No need to free packets here
	 * because the rte_eth f/w frees up the packets through tx_buffer
	 * callback in case this functions returns count less than nb_bufs
	 */
	return 0;
}
