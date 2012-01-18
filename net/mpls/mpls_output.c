/*****************************************************************************
 * MPLS - Multi Protocol Label Switching
 *
 *      An implementation of the MPLS architecture for Linux.
 *
 * Authors:
 *          James Leu        <jleu@mindspring.com>
 *          Ramon Casellas   <casellas@infres.enst.fr>
 *          Igor Maravić     <igorm@etf.rs> - Innovational Centre of School of Electrical Engineering, Belgrade
 *
 *   (c) 1999-2004   James Leu        <jleu@mindspring.com>
 *   (c) 2003-2004   Ramon Casellas   <casellas@infres.enst.fr>
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 ****************************************************************************/

#include <linux/ratelimit.h>
#include <net/ip.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/dst.h>
#include <net/ip_fib.h>
#include <net/mpls.h>
#include <linux/ip.h>
#include <net/dsfield.h>
#include <net/xfrm.h>

static inline int mpls_prepare_skb(
		struct sk_buff *skb, 
		unsigned int header_size,
		struct net_device *dev)
{
	secpath_reset(skb);
	skb->mac_header = skb->network_header;
	skb_reset_network_header(skb);

	if (!pskb_may_pull(skb, header_size))
		return -EINVAL;

	skb->pkt_type = PACKET_HOST;
	__skb_tunnel_rx(skb, dev);
	return NET_XMIT_SUCCESS;
}

static int mpls_dlv_ip(struct sk_buff *skb)
{
	if (ip_hdr(skb)->version == 4)
		skb->protocol = htons(ETH_P_IP);
	else if (ip_hdr(skb)->version == 6)
		skb->protocol = htons(ETH_P_IPV6);
	else {
		return -EINVAL;
	}

	netif_receive_skb(skb);

	MPLS_DEBUG("delivering\n");
	MPLS_EXIT;
	return NET_XMIT_SUCCESS;
}

static int mpls_dlv_recurse(struct sk_buff *skb)
{
	MPLSCB(skb)->recursion = 1;
	MPLS_DEBUG("recursion\n");

	netif_receive_skb(skb);

	MPLS_EXIT;
	return NET_XMIT_SUCCESS;
}

/**
 *	mpls_send - Send a labelled packet.
 *	@skb: Ready to go socket buffer.
 *
 *	Send the socket buffer to the next hop. It assumes that everything has
 *	been properly set up. In order to forward/send the packet it's using:
 *	a) neigh_output(neigh, skb);
 *
 *	Returns: NET_XMIT_SUCCESS/NET_XMIT_DROP/NET_XMIT_CN
 **/
static inline int mpls_send(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct neighbour *neigh;

	MPLS_DEBUG("output device = %s\n", dst->dev->name);

	/* Be paranoid, rather than too clever. */
	if (unlikely(skb_headroom(skb) < LL_RESERVED_SPACE(dst->dev)
			&& dst->dev->header_ops)) {
		struct sk_buff *skb2;

		skb2 = skb_realloc_headroom(skb, LL_RESERVED_SPACE(dst->dev));
		if (!skb2) {
			kfree_skb(skb);
			return -ENOMEM;
		}
		if (skb->sk)
			skb_set_owner_w(skb2, skb->sk);
		kfree_skb(skb);
		skb = skb2;
	}

	rcu_read_lock();
	neigh = dst_get_neighbour_noref(dst);
	if (neigh) {
		int res = neigh_output(neigh, skb);
		MPLS_EXIT;
		rcu_read_unlock();
		return res;
	}
	rcu_read_unlock();

	if (net_ratelimit())
		MPLS_DEBUG("No header cache and no neighbour!\n");

	kfree_skb(skb);
	MPLS_EXIT;
	return -EINVAL;
}

/**
 *	mpls_finish_output - Apply out segment to socket buffer
 *	@sbk: Socket buffer.
 *	@nhlfe: NHLFE object containing the list of opcodes to apply.
 *
 *	This function is either called by mpls_switch or mpls_output, and
 *	iterates the set of output opcodes that are configured for this NHLFE.
 **/

static int mpls_finish_output(
	struct sk_buff *skb,
	struct mpls_nhlfe *nhlfe)
{
	struct mpls_instr *mi;
	int ret = -EINVAL;
	int ready_to_tx = 0;
	unsigned int packet_length;
	struct net_device *dev = skb_dst(skb)->dev;

	MPLS_OUT_OPCODE_PROTOTYPE(*func);

	MPLS_ENTER;

	ready_to_tx = 0;

	if (skb_cow_head(skb, skb_dst(skb)->header_len) < 0)
		goto out_discard;

	/* Iterate all the opcodes for this NHLFE */
	for_each_instr(nhlfe->nhlfe_instr, mi) {
		int opcode = mi->mi_opcode;
		void *data = mi->mi_data;
		char *msg  = mpls_ops[opcode].msg;

		MPLS_DEBUG("opcode %s\n", msg);

		if (mpls_ops[opcode].extra)
			ready_to_tx = 1;
		func = mpls_ops[opcode].out;
		if (func) {
			switch (func(&skb, NULL, &nhlfe, data)) {
				case MPLS_RESULT_SUCCESS:
					/*
					 * it's ready to tx only if the opcode is SET
					 */
					if (ready_to_tx)
						goto send;
					break;
				case MPLS_RESULT_DROP:
					goto out_drop;
				case MPLS_RESULT_DLV:
					if (mpls_prepare_skb(skb, 
						sizeof(struct iphdr), dev))
						goto out_drop;			
					goto dlv;
				case MPLS_RESULT_RECURSE:
					if (mpls_prepare_skb(skb, 
						MPLS_HDR_LEN, dev))
						goto out_drop;
					goto recourse;
				case MPLS_RESULT_FWD:
					/* Invalid on OUTPUT */
					WARN_ON_ONCE(1);
					goto out_drop;
			}
		}
	}
	goto out_drop;
dlv:
	packet_length = skb->len;
	ret = mpls_dlv_ip(skb);
	if (ret)
		goto out_drop;

	goto stats;
recourse:
	packet_length = skb->len;
	ret = mpls_dlv_recurse(skb);
	if (ret)
		goto out_drop;

	goto stats;
send:
	if (skb->len > dev->mtu) {
		int mtu = dst_mtu(&nhlfe->dst);
		MPLS_DEBUG("packet size %d"
			" exceeded device MTU %d (%d)\n",
			skb->len, dev->mtu, mtu);
		ret = nhlfe->nhlfe_proto->mtu_exceeded(&skb, mtu);
		if (ret)
			goto out_drop;
		/* Otherwise prot->mtu_exceeded() has returned a
		 * modified skb that it wants to be forwarded
		 * down the LSP */
	}
	packet_length = skb->len;

	/* Send to the hardware */
	ret = mpls_send(skb);
stats:
	if (likely(ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN)) {
		MPLS_INC_STATS(dev_net(dev), MPLS_MIB_OUTPACKETS);
		MPLS_ADD_STATS(dev_net(dev), MPLS_MIB_OUTOCTETS,
			packet_length);
	} else
		MPLS_INC_STATS(dev_net(dev), MPLS_MIB_OUTERRORS);
out:

	MPLS_EXIT;
	return ret;

out_drop:
	/* kfree_skb() releases nhlfe entry
	 * No need to call mpls_nhlfe_release()
	 */
	kfree_skb(skb);
	MPLS_INC_STATS(dev_net(dev), MPLS_MIB_OUTERRORS);
	goto out;
out_discard:
	kfree_skb(skb);
	MPLS_INC_STATS(dev_net(dev), MPLS_MIB_OUTDISCARDS);
	goto out;
}



/**
 *	mpls_output - Send a packet using MPLS forwarding.
 *	@skb: socket buffer containing the packet to send.
 *
 *	This function is called by the upper layers, in order to
 *	forward a data packet using MPLS. It assumes that the buffer
 *	is ready, most notably, that skb->dst field is valid and
 *	is part of a valid NHLFE. After some error checking, calls
 *	mpls_output_shim.
 *
 *	NOTE: Please note that we *push* a label. A cross-connect (SWAP)
 *	is a ILM/POP + NHLFE/PUSH
 **/

int mpls_output(struct sk_buff *skb)
{
	struct mpls_skb_cb *cb = MPLSCB(skb);
	struct mpls_nhlfe *nhlfe = NULL;
	int ttl = sysctl_mpls_default_ttl;

	BUG_ON(!skb_dst(skb));
	
	if (unlikely(skb_dst(skb)->ops->protocol != htons(ETH_P_MPLS_UC))) {
		printk("MPLS: Not a MPLS dst in skb\n");
		goto mpls_output_drop;
	}

	nhlfe = container_of(skb_dst(skb), struct mpls_nhlfe, dst);

	if (unlikely(!nhlfe)) {
		printk(KERN_ERR "MPLS: unable to find NHLFE from dst\n");
		goto mpls_output_drop;
	}

	if (unlikely(skb->protocol !=
		nhlfe->nhlfe_proto->ethertype)) {
		printk_ratelimited(KERN_ERR "unable to find a protocol"
			" driver (0x%x)\n", htons(skb->protocol));
		goto mpls_output_drop;
	}

	/*
	 * JLEU: we only propagate the TTL if the SKB came from
	 * IP[46] _and_ nhlfe_propagate_ttl is set to 1, otherwise we
	 * set the TTL sysctl_mpls_default_ttl
	 */
	if (nhlfe->nhlfe_propagate_ttl)
		ttl = nhlfe->nhlfe_proto->get_ttl(skb);

	cb->prot = nhlfe->nhlfe_proto;
	cb->label = 0;
	cb->ttl = ttl;
	cb->exp = 0;
	cb->bos = 1;
	cb->flag = 0;
	cb->popped_bos = 1;

	return mpls_finish_output(skb, nhlfe);

mpls_output_drop:
	MPLS_INC_STATS(dev_net(skb_dst(skb)->dev),
		MPLS_MIB_OUTDISCARDS);
	kfree_skb(skb);
	MPLS_EXIT;
	return -EINVAL;
}

/**
 *	mpls_switch - Label switch a packet coming from mpls_input
 *	@skb: socket buffer containing the packet to send.
 *
 *	This function is called by mpls_input, in order to
 *	label switch a data packet. It assumes that the socket
 *	is ready, most notably, that skb->dst field is valid and
 *	is part of a valid NHLFE. After some error checking, calls
 *	mpls_finish_output.
 *	NOTE: Please note that we *push* a label. The current label was
 *	already poped in mpls_input.
 **/
int mpls_switch(struct sk_buff *skb)
{
	struct mpls_nhlfe *nhlfe = NULL;

	BUG_ON(!skb_dst(skb));

	if (unlikely(skb_dst(skb)->ops->protocol != htons(ETH_P_MPLS_UC))) {
		printk_ratelimited(KERN_ERR "MPLS: Not a MPLS dst in skb\n");
		goto mpls_switch_drop;
	}

	nhlfe = container_of(skb_dst(skb), struct mpls_nhlfe, dst);

	if (unlikely(!nhlfe)) {
		printk_ratelimited(KERN_ERR "MPLS: unable to find NHLFE from dst\n");
		goto mpls_switch_drop;
	}

	if (unlikely(skb->protocol != nhlfe->nhlfe_proto->ethertype
			&& skb->protocol != htons(ETH_P_MPLS_UC))) {
		printk_ratelimited(KERN_ERR "MPLS: unable to find a"
			" protocol driver (0x%x)\n", htons(skb->protocol));
		goto mpls_switch_drop;
	}
	return mpls_finish_output(skb, nhlfe);

mpls_switch_drop:
	MPLS_INC_STATS(dev_net(skb_dst(skb)->dev),
		MPLS_MIB_OUTDISCARDS);
	kfree_skb(skb);
	MPLS_EXIT;
	return -EINVAL;
}
