// SPDX-License-Identifier: GPL-2.0-only
/*
 *  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2020 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 *		James Yonan <james@openvpn.net>
 */


#include "pktid.h"

#include <linux/atomic.h>
#include <linux/jiffies.h>

void ovpn_pktid_xmit_init(struct ovpn_pktid_xmit *pid)
{
	atomic64_set(&pid->seq_num, (__u64)0);
	pid->tcp_linear = NULL;
}

void ovpn_pktid_recv_init(struct ovpn_pktid_recv *pr)
{
	memset(pr, 0, sizeof(*pr));
	spin_lock_init(&pr->lock);
}

#if ENABLE_REPLAY_PROTECTION

/*
 * Packet replay detection.
 * Allows ID backtrack of up to REPLAY_WINDOW_SIZE - 1.
 */
static int __ovpn_pktid_recv(struct ovpn_pktid_recv *pr, __u32 pkt_id, __u32 pkt_time)
{
	const unsigned long now = jiffies;

	/* expire backtracks at or below pr->id after PKTID_RECV_EXPIRE time */
	if (unlikely(time_after_eq(now, pr->expire)))
		pr->id_floor = pr->id;

        /* ID must not be zero */
	if (unlikely(pkt_id == 0))
		return -OVPN_ERR_PKTID_ID_ZERO;

	/* time changed? */
	if (unlikely(pkt_time != pr->time)) {
		if (pkt_time > pr->time) {
			/* time moved forward, accept */
			pr->base = 0;
			pr->extent = 0;
			pr->id = 0;
			pr->time = pkt_time;
			pr->id_floor = 0;
		} else {
			/* time moved backward, reject */
			return -OVPN_ERR_PKTID_TIME_BACKTRACK;
		}
	}

	if (likely(pkt_id == pr->id + 1)) {
		/* well-formed ID sequence (incremented by 1) */
		pr->base = REPLAY_INDEX(pr->base, -1);
		pr->history[pr->base / 8] |= (1 << (pr->base % 8));
		if (pr->extent < REPLAY_WINDOW_SIZE)
			++pr->extent;
		pr->id = pkt_id;
	} else if (pkt_id > pr->id) {
		/* ID jumped forward by more than one */
		const unsigned int delta = pkt_id - pr->id;
		if (delta < REPLAY_WINDOW_SIZE) {
			unsigned int i;
			pr->base = REPLAY_INDEX(pr->base, -delta);
			pr->history[pr->base / 8] |= (1 << (pr->base % 8));
			pr->extent += delta;
			if (pr->extent > REPLAY_WINDOW_SIZE)
				pr->extent = REPLAY_WINDOW_SIZE;
			for (i = 1; i < delta; ++i) {
				const unsigned int newbase = REPLAY_INDEX(pr->base, i);
				pr->history[newbase / 8] &= ~(1 << (newbase % 8));
			}
		} else {
			pr->base = 0;
			pr->extent = REPLAY_WINDOW_SIZE;
			memset(pr->history, 0, sizeof(pr->history));
			pr->history[0] = 1;
		}
		pr->id = pkt_id;
	} else {
		/* ID backtrack */
		const unsigned int delta = pr->id - pkt_id;
		if (delta > pr->max_backtrack)
			pr->max_backtrack = delta;
		if (delta < pr->extent) {
			if (pkt_id > pr->id_floor) {
				const unsigned int ri = REPLAY_INDEX(pr->base, delta);
				__u8 *p = &pr->history[ri / 8];
				const __u8 mask = (1 << (ri % 8));
				if (*p & mask)
					return -OVPN_ERR_PKTID_REPLAY;
				*p |= mask;
			} else
				return -OVPN_ERR_PKTID_EXPIRE;
		} else
			return -OVPN_ERR_PKTID_ID_BACKTRACK;
	}

	pr->expire = now + PKTID_RECV_EXPIRE;
	return 0;
}

#endif

/*
 * Packet replay detection with locking.
 */
int ovpn_pktid_recv(struct ovpn_pktid_recv *pr, __u32 pkt_id, __u32 pkt_time)
{
#if ENABLE_REPLAY_PROTECTION
	int ret;
	//ovpn_debug(OVPN_KERN_INFO, "*** PKT ID recv id=%u time=%u\n", pkt_id, pkt_time);
	spin_lock(&pr->lock);
	ret = __ovpn_pktid_recv(pr, pkt_id, pkt_time);
	spin_unlock(&pr->lock);
	return ret;
#else
	return 0;
#endif
}