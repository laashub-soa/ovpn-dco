// SPDX-License-Identifier: GPL-2.0-only
/*  OpenVPN data channel accelerator
 *
 *  Copyright (C) 2019-2020 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "main.h"
#include "bind.h"
#include "netlink.h"
#include "sock.h"
#include "peer.h"
#include "stats_counters.h"
#include "proto.h"
#include "crypto.h"
#include "work.h"
#include "skb.h"

#include <net/route.h>
#include <net/ip6_route.h>
#include <net/ip6_checksum.h>
#include <net/udp_tunnel.h>
#include <uapi/linux/if_ether.h>

int ovpn_struct_init(struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	int err;

	ovpn->dev = dev;

	err = ovpn_netlink_init(ovpn);
	if (err < 0)
		return err;

	spin_lock_init(&ovpn->lock);
	RCU_INIT_POINTER(ovpn->peer, NULL);

	err = security_tun_dev_alloc_security(&ovpn->security);
	if (err < 0)
		return err;

	/* kernel -> userspace tun queue length */
	ovpn->max_tun_queue_len = OVPN_MAX_TUN_QUEUE_LEN;

	return 0;
}

/* Called after decrypt to write IP packet to tun netdev.
 * This method is expected to manage/free skb.
 */
static int tun_netdev_write(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
			    struct sk_buff *skb)
{
	unsigned int rx_stats_size;
	int ret;

	rcu_read_lock();

	/* note event of authenticated packet received for keepalive */
	ovpn_peer_update_keepalive_expire(peer);

	/* increment RX stats */
	rx_stats_size = OVPN_SKB_CB(skb)->rx_stats_size;
	ovpn_peer_stats_increment_rx(peer, rx_stats_size);

	/* verify IP header size, set skb->protocol,
	 * set skb network header, and possibly stash shim
	 */
	ret = ovpn_ip_header_probe(skb, OVPN_PROBE_SET_SKB);
	if (unlikely(ret < 0)) {
		/* check if null packet */
		if (unlikely(!pskb_may_pull(skb, 1))) {
			ret = -EINVAL;
			goto drop;
		}

		/* check if special OpenVPN message */
		if (ovpn_is_keepalive(skb)) {
#if DEBUG_PING
			ovpn_dbg_ping_received(skb, ovpn, peer);
#endif
			/* openvpn keepalive - not an error */
			ret = 0;
		}

		goto drop;
	}

#if DEBUG_IN
	ovpn_dbg_kovpn_in(skb, peer);
#endif

	/* packet integrity was verified on the VPN layer - no need to perform
	 * any additional check along the stack
	 */
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->csum_level = ~0;

	/* skb hash for transport packet no longer valid after decapsulation */
	skb_clear_hash(skb);

	/* post-decrypt scrub -- prepare to inject encapsulated packet onto tun
	 * interface, based on __skb_tunnel_rx() in dst.h
	 */
	skb->dev = ovpn->dev;
	skb_set_queue_mapping(skb, 0);
	skb_scrub_packet(skb, true);

	/* set transport header */
	skb->transport_header = 0;
	skb_probe_transport_header(skb);

	rcu_read_unlock();

	/* cause packet to be "received" by tun interface */
	netif_rx(skb);
	return 0;

drop:
	if (ret < 0)
		kfree_skb(skb);
	else
		consume_skb(skb);
	rcu_read_unlock();
	return ret;
}

static void post_decrypt(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
			 struct ovpn_crypto_context *cc, struct sk_buff *skb,
			 int err, struct ovpn_work *work)
{
	/* free workspace */
	kfree(work);

	/* test decrypt status */
	if (unlikely(err)) {
		/* decryption failed */
		kfree_skb(skb);
		goto error;
	}

	/* successful decryption */
	tun_netdev_write(ovpn, peer, skb);

error:
	ovpn_crypto_context_put(cc);
	ovpn_peer_put(peer);
}

static void post_decrypt_callback(struct sk_buff *skb, int err)
{
	struct ovpn_work *work = OVPN_SKB_CB(skb)->work;

	post_decrypt(work->cc->peer->ovpn, work->cc->peer, work->cc, skb, err,
		     work);
}

/* Lookup ovpn_peer using incoming encrypted transport packet.
 * This is for looking up transport -> ovpn packets.
 */
static struct ovpn_peer *
ovpn_lookup_peer_via_transport(struct ovpn_struct *ovpn,
			       struct sk_buff *skb)
{
	struct ovpn_peer *peer;
	struct ovpn_bind *bind;

	rcu_read_lock();
	peer = ovpn_peer_get(ovpn);
	if (!peer)
		goto err;

	bind = rcu_dereference(peer->bind);
	if (!bind)
		goto err;

	/* only one peer is supported at the moment. check if it's the one the
	 * skb was received from and return it
	 */
	if (!ovpn_bind_skb_match(bind, skb))
		goto err;

	rcu_read_unlock();
	return peer;

err:
	ovpn_peer_put(peer);
	rcu_read_unlock();
	return NULL;
}

static int ovpn_transport_to_userspace(struct ovpn_struct *ovpn,
				       struct sk_buff *skb)
{
	int ret;

	ret = skb_linearize(skb);
	if (ret < 0)
		return ret;

	ret = ovpn_netlink_send_packet(ovpn, skb->data, skb->len);
	if (ret < 0)
		return ret;

	consume_skb(skb);
	return 0;
}

/* Receive an encrypted packet from transport (UDP or TCP).
 * Should be called with rcu_read_lock held, but will be released
 * before return.  Takes ownership of skb.
 */
static void ovpn_recv_crypto(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
			     const unsigned int op, struct sk_buff *skb)
{
	struct ovpn_crypto_context *cc;
	int key_id;
	int ret;

	/* save original packet size for stats accounting */
	OVPN_SKB_CB(skb)->rx_stats_size = skb->len;

	/* we only handle OVPN_DATA_Vx packets from known peers here.
	 *
	 * all other packets are sent to userspace via netlink
	 */
	if (unlikely(!peer || !ovpn_opcode_is_data(op))) {
		ret = ovpn_transport_to_userspace(ovpn, skb);
		if (ret < 0)
			goto drop;

		if (peer)
			ovpn_peer_put(peer);
		return;
	}

	/* get the crypto context */
	key_id = ovpn_key_id_extract(op);
	cc = ovpn_crypto_context_from_state(&peer->crypto, key_id);
	if (unlikely(!cc))
		goto drop;

	/* decrypt */
	ret = cc->ops->decrypt(cc, skb, key_id, op, post_decrypt_callback);
	if (likely(ret != -EINPROGRESS))
		post_decrypt(ovpn, peer, cc, skb, ret, OVPN_SKB_CB(skb)->work);

	return;

drop:
	if (peer)
		ovpn_peer_put(peer);
	kfree_skb(skb);
}

/* Dispatch received transport packet (UDP or TCP)
 * to the appropriate handler (crypto or relay).
 * Should be called with rcu_read_lock held, but will be released
 * before return.  Takes ownership of skb.
 */
static void ovpn_recv(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
		      const unsigned int op, struct sk_buff *skb)
{
	ovpn_recv_crypto(ovpn, peer, op, skb);
}

/* UDP encapsulation receive handler.  See net/ipv[46]/udp.c.
 * Here we look at an incoming OpenVPN UDP packet.  If we are able
 * to process it, we will send it directly to tun interface.
 * Otherwise, send it up to userspace.
 * Called in softirq context.
 *
 * Return codes:
 *  0 : we consumed or dropped packet
 * >0 : skb should be passed up to userspace as UDP (packet not consumed)
 * <0 : skb should be resubmitted as proto -N (packet not consumed)
 */
int ovpn_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct ovpn_struct *ovpn;
	struct ovpn_peer *peer;
	unsigned int op;

	/* ensure accurate L4 hash for packets assembled from IP fragments */
	skb_clear_hash_if_not_l4(skb);

	/* pre-decrypt scrub */
	/* TODO */

	/* pop off outer UDP header */
	__skb_pull(skb, sizeof(struct udphdr));

	ovpn = ovpn_from_udp_sock(sk);
	if (!ovpn)
		goto drop;

	/* get opcode */
	op = ovpn_op32_from_skb(skb, NULL);

	/* lookup peer */
	peer = ovpn_lookup_peer_via_transport(ovpn, skb);

	ovpn_recv(ovpn, peer, op, skb);
	return 0;

drop:
	kfree_skb(skb);
	return 0;
}

static int ovpn_udp4_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			    struct sock *sk, struct sk_buff *skb)
{
	struct flowi4 fl = {
		.saddr = bind->sapair.local.u.in4.sin_addr.s_addr,
		.daddr = bind->sapair.remote.u.in4.sin_addr.s_addr,
		.fl4_sport = bind->sapair.local.u.in4.sin_port,
		.fl4_dport = bind->sapair.remote.u.in4.sin_port,
		.flowi4_proto = sk->sk_protocol,
		.flowi4_mark = sk->sk_mark,
		.flowi4_oif = sk->sk_bound_dev_if,
	};
	struct rtable *rt;

	rt = ip_route_output_flow(sock_net(sk), &fl, sk);
	if (IS_ERR(rt)) {
		net_dbg_ratelimited("%s: no route to host %pISpc\n",
				    ovpn->dev->name,
				    &bind->sapair.remote.u.in4);
		return -EHOSTUNREACH;
	}

	udp_tunnel_xmit_skb(rt, sk, skb, fl.saddr, fl.daddr, 0,
			    ip4_dst_hoplimit(&rt->dst), 0, fl.fl4_sport,
			    fl.fl4_dport, false, sk->sk_no_check_tx);
	return 0;
}

#if IS_ENABLED(CONFIG_IPV6)
static int ovpn_udp6_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			    struct sock *sk, struct sk_buff *skb)
{
	struct dst_entry *dst;
	int ret;

	struct flowi6 fl = {
		.saddr = bind->sapair.local.u.in6.sin6_addr,
		.daddr = bind->sapair.remote.u.in6.sin6_addr,
		.fl6_sport = bind->sapair.local.u.in6.sin6_port,
		.fl6_dport = bind->sapair.remote.u.in6.sin6_port,
		.flowi6_proto = sk->sk_protocol,
		.flowi6_mark = sk->sk_mark,
		.flowi6_oif = sk->sk_bound_dev_if,
	};

	/* based on scope ID usage from net/ipv6/udp.c */
	if (bind->sapair.remote.u.in6.sin6_scope_id &&
	    __ipv6_addr_needs_scope_id(__ipv6_addr_type(&fl.daddr)))
		fl.flowi6_oif = bind->sapair.remote.u.in6.sin6_scope_id;

	dst = ip6_route_output(sock_net(sk), sk, &fl);
	if (unlikely(dst->error < 0)) {
		ret = dst->error;
		dst_release(dst);
		return ret;
	}

	udp_tunnel6_xmit_skb(dst, sk, skb, skb->dev, &fl.saddr, &fl.daddr, 0,
			     ip6_dst_hoplimit(dst), 0, fl.fl6_sport,
			     fl.fl6_dport, udp_get_no_check6_tx(sk));
	return 0;
}
#endif

/* Prepend UDP transport and IP headers to skb (using
 * address/ports from binding) and transmit the packet
 * using ip_local_out.
 *
 * rcu_read_lock should be held on entry.
 * On return, the skb is consumed and rcu_read_lock
 * is released, even on error return.
 */
static int ovpn_udp_output(struct ovpn_struct *ovpn, struct ovpn_bind *bind,
			   struct sock *sk, struct sk_buff *skb)
{
	int ret;

	ovpn_rcu_lockdep_assert_held();

	/* set sk to null if skb is already orphaned */
	if (!skb->destructor)
		skb->sk = NULL;

	switch (bind->sapair.local.family) {
	case AF_INET:
		ret = ovpn_udp4_output(ovpn, bind, sk, skb);
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case AF_INET6:
		ret = ovpn_udp6_output(ovpn, bind, sk, skb);
		break;
#endif
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

/* Called after encrypt to write IP packet to UDP port.
 * This method is expected to manage/free skb.
 */
static void ovpn_udp_write(struct ovpn_struct *ovpn, struct ovpn_peer *peer,
			   struct sk_buff *skb)
{
	struct ovpn_bind *bind;
	struct socket *sock;
	int ret = -1;

	skb->dev = ovpn->dev;

	/* get socket info */
	sock = peer->sock;
	if (unlikely(!sock))
		goto out;

	rcu_read_lock();
	/* get binding */
	bind = rcu_dereference(peer->bind);
	if (unlikely(!bind))
		goto out_unlock;

	/* note event of authenticated packet xmit for keepalive */
	ovpn_peer_update_keepalive_xmit(peer);

	/* crypto layer -> transport (UDP) */
	ret = ovpn_udp_output(ovpn, bind, sock->sk, skb);

out_unlock:
	rcu_read_unlock();
out:
	if (ret < 0)
		kfree_skb(skb);
}

int ovpn_udp_send_data(struct ovpn_struct *ovpn, const u8 *data, size_t len)
{
	struct ovpn_peer *peer;
	struct sk_buff *skb;
	int ret = 0;

	peer = ovpn_peer_get(ovpn);
	if (!peer) {
		pr_debug("no peer to send data to\n");
		return -EHOSTUNREACH;
	}

	skb = alloc_skb(SKB_HEADER_LEN + len, GFP_ATOMIC);
	if (unlikely(!skb)) {
		ret = -ENOMEM;
		goto out;
	}

	skb_reserve(skb, SKB_HEADER_LEN);
	skb_put_data(skb, data, len);

	ovpn_udp_write(ovpn, peer, skb);
out:
	ovpn_peer_put(peer);
	return ret;
}

static void post_encrypt(struct ovpn_struct *ovpn,
			 struct ovpn_peer *peer, struct ovpn_crypto_context *cc,
			 struct sk_buff *skb, int err, struct ovpn_work *work)
{
	/* free workspace */
	kfree(work);

	/* test encrypt status */
	if (unlikely(err)) {
		kfree_skb(skb);
		goto error;
	}

	/* successful encryption */
	ovpn_udp_write(ovpn, peer, skb);

error:
	/* release our reference to crypto context */
	ovpn_crypto_context_put(cc);
	ovpn_peer_put(peer);
}

static void post_encrypt_callback(struct sk_buff *skb, int err)
{
	struct ovpn_crypto_context *cc;
	struct ovpn_struct *ovpn;
	struct ovpn_work *work;
	struct ovpn_peer *peer;

	work = OVPN_SKB_CB(skb)->work;
	cc = work->cc;
	peer = cc->peer;

	ovpn = peer->ovpn;

	post_encrypt(ovpn, peer, cc, skb, err, work);
}

/* rcu_read_lock must be held on entry.
 * On success, 0 is returned, skb ownership is transferred,
 * On error, a value < 0 is returned, the skb is not owned/released.
 */
static int do_ovpn_net_xmit(struct ovpn_struct *ovpn, struct sk_buff *skb,
			    const bool is_ip_packet)
{
	struct ovpn_crypto_context *cc;
	struct ovpn_peer *peer;
	struct ovpn_bind *bind;
	unsigned int headroom;
	int key_id;
	int ret = -1;

	peer = ovpn_peer_get(ovpn);
	if (unlikely(!peer))
		return -ENOLINK;

	rcu_read_lock();
	bind = rcu_dereference(peer->bind);
	if (unlikely(!bind)) {
		ret = -ENOENT;
		goto drop;
	}

	/* set minimum encapsulation headroom for encrypt */
	headroom = ovpn_bind_udp_encap_overhead(bind, ETH_HLEN);
	if (unlikely(headroom < 0))
		goto drop;

	/* get crypto context */
	cc = ovpn_crypto_context_primary(&peer->crypto, &key_id);
	if (unlikely(!cc)) {
		ret = -ENODEV;
		goto drop;
	}
	rcu_read_unlock();

	/* init packet ID to undef in case we err before setting real value */
	OVPN_SKB_CB(skb)->pktid = 0;

	/* encrypt */
	ret = cc->ops->encrypt(cc, skb, headroom, key_id,
			       post_encrypt_callback);
	if (likely(ret != -EINPROGRESS))
		post_encrypt(ovpn, peer, cc, skb, ret,
			     OVPN_SKB_CB(skb)->work);

	return 0;

drop:
	rcu_read_unlock();
	ovpn_peer_put(peer);
	return ret;
}

/* Net device start xmit
 */
netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	int ret;

	/* reset netfilter state */
	nf_reset_ct(skb);
	/* verify IP header size in network packet */
	ret = ovpn_ip_header_probe(skb, 0);
	if (unlikely(ret < 0))
		goto drop;

	skb_reset_network_header(skb);

	ret = do_ovpn_net_xmit(ovpn, skb, true);
	if (unlikely(ret < 0))
		goto drop;

	return NETDEV_TX_OK;

drop:
	skb_tx_error(skb);
	kfree_skb(skb);
	return NET_XMIT_DROP;
}

/* Encrypt and transmit a special message to peer, such as keepalive
 * or explicit-exit-notify.  Called from softirq context.
 * Assumes that caller holds a reference to peer.
 */
void ovpn_xmit_special(struct ovpn_peer *peer, const void *data,
		       const unsigned int len)
{
	struct ovpn_struct *ovpn;
	struct sk_buff *skb;
	int err;

	ovpn = peer->ovpn;
	if (unlikely(!ovpn))
		return;

	skb = alloc_skb(256 + len, GFP_ATOMIC);
	if (unlikely(!skb))
		return;

	skb_reserve(skb, 128);
	skb->priority = TC_PRIO_BESTEFFORT;
	memcpy(__skb_put(skb, len), data, len);

	err = do_ovpn_net_xmit(ovpn, skb, false);
	if (likely(err < 0))
		kfree_skb(skb);
}
