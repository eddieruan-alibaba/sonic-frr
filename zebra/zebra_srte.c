/* Zebra SR-TE code
 * Copyright (C) 2020  NetDEF, Inc.
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "lib/zclient.h"
#include "lib/lib_errors.h"
#include "lib/nexthop.h"

#include "zebra/zebra_router.h"
#include "zebra/zebra_srte.h"
#include "zebra/zebra_mpls.h"
#include "zebra/zebra_srv6.h"
#include "zebra/zebra_rnh.h"
#include "zebra/zapi_msg.h"
#include "zebra/debug.h"
#include "zebra/zebra_nhg.h"
#include "zebra/zebra_nhg_private.h"

DEFINE_MTYPE_STATIC(ZEBRA, ZEBRA_SR_POLICY, "SR Policy");

static void zebra_sr_policy_deactivate(struct zebra_sr_policy *policy);

struct hash *srte_table_hash = NULL;
#if 0
/* Generate rb-tree of SR Policy instances. */
static inline int
zebra_sr_policy_instance_compare(const struct zebra_sr_policy *a,
				 const struct zebra_sr_policy *b)
{
	return sr_policy_compare(&a->endpoint, &b->endpoint, a->color,
				 b->color);
}
RB_GENERATE(zebra_sr_policy_instance_head, zebra_sr_policy, entry,
	    zebra_sr_policy_instance_compare)

struct zebra_sr_policy_instance_head zebra_sr_policy_instances =
	RB_INITIALIZER(&zebra_sr_policy_instances);

struct zebra_sr_policy *zebra_sr_policy_add(uint32_t color,
					    struct ipaddr *endpoint, char *name)
{
	struct zebra_sr_policy *policy;

	policy = XCALLOC(MTYPE_ZEBRA_SR_POLICY, sizeof(*policy));
	policy->color = color;
	policy->endpoint = *endpoint;
	strlcpy(policy->name, name, sizeof(policy->name));
	policy->status = ZEBRA_SR_POLICY_DOWN;
	RB_INSERT(zebra_sr_policy_instance_head, &zebra_sr_policy_instances,
		  policy);

	return policy;
}

void zebra_sr_policy_del(struct zebra_sr_policy *policy)
{
	if (policy->status == ZEBRA_SR_POLICY_UP)
		zebra_sr_policy_deactivate(policy);
	RB_REMOVE(zebra_sr_policy_instance_head, &zebra_sr_policy_instances,
		  policy);
	XFREE(MTYPE_ZEBRA_SR_POLICY, policy);
}

struct zebra_sr_policy *zebra_sr_policy_find(uint32_t color,
					     struct ipaddr *endpoint)
{
	struct zebra_sr_policy policy = {};

	policy.color = color;
	policy.endpoint = *endpoint;
	return RB_FIND(zebra_sr_policy_instance_head,
		       &zebra_sr_policy_instances, &policy);
}

struct zebra_sr_policy *zebra_sr_policy_find_by_name(char *name)
{
	struct zebra_sr_policy *policy;

	// TODO: create index for policy names
	RB_FOREACH (policy, zebra_sr_policy_instance_head,
		    &zebra_sr_policy_instances) {
		if (strcmp(policy->name, name) == 0)
			return policy;
	}

	return NULL;
}
#endif

struct zebra_sr_policy *zebra_sr_policy_add_by_prefix(struct prefix *p, uint32_t color, char *name)
{
	struct route_node *rn;
	struct zebra_sr_policy *policy;
	struct srte_table_key srte_key = {0};
	struct srte_table_key *srte_key_table = NULL;
	srte_key.afi = family2afi(p->family);
	srte_key.color = color;
	srte_key_table = hash_get(srte_table_hash, &srte_key, srte_table_alloc);
	if (!srte_key_table || !srte_key_table->table) {
		return NULL;
	}
	apply_mask(p);
	rn = route_node_get(srte_key_table->table, p);
	policy = rn->info;
	if (!policy) {
		policy = XCALLOC(MTYPE_ZEBRA_SR_POLICY, sizeof(struct zebra_sr_policy));
		route_lock_node(rn);
		policy->node = rn;
		policy->color = color;
		strlcpy(policy->name, name, sizeof(policy->name));
		policy->status = ZEBRA_SR_POLICY_UP;
		rnh_list_init(&policy->nht);
		rn->info = policy;
	} 
	route_unlock_node(rn);
	return policy;
}
struct zebra_sr_policy *zebra_sr_policy_lookup_by_prefix(struct prefix *p, uint32_t color)
{
	struct route_node *rn;
	struct srte_table_key srte_key = {0};
	struct srte_table_key *srte_key_table = NULL;
	srte_key.afi = family2afi(p->family);
	srte_key.color = color;
	srte_key_table = hash_lookup(srte_table_hash, &srte_key);
	if (!srte_key_table || !srte_key_table->table)
		return NULL;
	apply_mask(p);
	rn = route_node_lookup(srte_key_table->table, p);
	if (!rn || !rn->info)
		return NULL;
	route_unlock_node(rn);
	return rn->info;
}
struct zebra_sr_policy *zebra_sr_policy_match_by_prefix(struct prefix *p, uint32_t color, struct route_node **prn)
{
	struct route_node *rn;
	struct srte_table_key srte_key = {0};
	struct zebra_sr_policy *policy = NULL;
	struct srte_table_key *srte_key_table = NULL;
	srte_key.afi = family2afi(p->family);
	srte_key.color = color;
	srte_key_table = hash_lookup(srte_table_hash, &srte_key);
	if (!srte_key_table || !srte_key_table->table)
		return NULL;
	apply_mask(p);
	rn = route_node_match(srte_key_table->table, p);
	if (!rn || !rn->info)
		return NULL;
	route_unlock_node(rn);
	while(rn) {
		policy = rn->info;
		if (policy && policy->status != ZEBRA_SR_POLICY_DOWN) {
			*prn = rn;
			return policy;
		}
		rn = rn->parent;
	}
	return NULL;
}
void zebra_free_sr_table(struct route_table *table)
{
	struct route_node *rn;
	struct srte_table_key *srte_key_table = NULL;
	struct srte_table_key srte_key = {0};
	struct zebra_sr_policy *policyRoot = NULL;
	rn = route_top(table);
	policyRoot = rn->info;
	if (!policyRoot) {
		zlog_err("error sr-te table node!");
		return;
	}
	if (route_table_count(table) == 1 && rnh_list_count(&policyRoot->nht) == 0)
	{
		rnh_list_fini(&policyRoot->nht);
		rn->info = NULL;
		srte_key.afi = family2afi(rn->p.family);
		srte_key.color = policyRoot->color;
		srte_key_table = hash_release(srte_table_hash, &srte_key);
		route_unlock_node(rn);
		route_table_finish(table);
		XFREE(MTYPE_ZEBRA_SR_POLICY, policyRoot);
		XFREE(MTYPE_ZEBRA_SR_POLICY, srte_key_table);
	}
}
void zebra_sr_policy_delete_by_prefix(struct zebra_sr_policy *policy)
{
	struct route_node *rn;
	struct route_table *table;
	if (policy->status == ZEBRA_SR_POLICY_UP)
		zebra_sr_policy_deactivate(policy);
	table = policy->node->table;
	if (!is_default_prefix(&policy->node->p))
	{
		rn = policy->node;
		rnh_list_fini(&policy->nht);
		rn->info = NULL;
		XFREE(MTYPE_ZEBRA_SR_POLICY, policy);
		route_unlock_node(rn);
	}
	zebra_free_sr_table(table);
}
struct zebra_sr_policy *zebra_sr_policy_find_by_rnh(struct rnh *rnh)
{
    return zebra_sr_policy_lookup_by_prefix(&rnh->node->p, rnh->srte_color);
}
static struct nhg_hash_entry *zebra_srv6_find_pic_nhe_by_policy(struct zebra_sr_policy *policy)
{
	vrf_id_t vrf_id = 0;
	bool ret = false;
	struct nexthop *nh = NULL;
	struct nhg_hash_entry lookup = {0};
	struct nhg_hash_entry *pic_nhe = NULL;
	vrf_id = policy->zvrf->vrf->vrf_id;
	lookup.type = ZEBRA_ROUTE_NHG;
	lookup.vrf_id = vrf_id;
	SET_FLAG(lookup.flags, NEXTHOP_GROUP_PIC_NHT);
	SET_FLAG(lookup.flags, NEXTHOP_GROUP_SEGMENTLIST);
	switch (policy->node->p.family) {
	case AF_INET:
		nh = nexthop_from_ipv4_segment_list(&policy->node->p.u.prefix4, vrf_id);
		lookup.afi = AFI_IP;
		break;
	case AF_INET6:
		nh = nexthop_from_ipv6_segment_list(&policy->node->p.u.prefix6, vrf_id);
		lookup.afi = AFI_IP6;
		break;
	default:
		return NULL;
	}
	nh->srte_color = policy->color;
	SET_FLAG(nh->flags, NEXTHOP_FLAG_ACTIVE);
	ret = nexthop_group_add_sorted_nodup(&lookup.nhg, nh);
	if (!ret) {
		nexthop_free(nh);
		return NULL;
	}
	pic_nhe = hash_lookup(zrouter.nhgs, &lookup);
	if (lookup.nhg.nexthop)
		nexthops_free(lookup.nhg.nexthop);
	return pic_nhe;
}
static struct nexthop *zebra_nhg_seg_update_nexthop(struct nexthop *nexthop,
	char *sidlist_name, bool add)
{
	struct nexthop *resolved_hop;
	struct nexthop *delete_hop;
	resolved_hop = nexthop_new();
	nexthop_copy_no_recurse(resolved_hop, nexthop, nexthop);
	memcpy(resolved_hop->sidlist_name, sidlist_name,
		SRTE_SEGMENTLIST_NAME_MAX_LENGTH);
	resolved_hop->flags = 0;
	SET_FLAG(resolved_hop->flags, NEXTHOP_FLAG_ACTIVE);
	if (add)
		_nexthop_add_sorted(&nexthop->resolved, resolved_hop);
	else {
		delete_hop = nexthop_exists_in_list(nexthop->resolved, resolved_hop);
		if (delete_hop)
			nexthop_del(&nexthop->resolved, resolved_hop);
		return delete_hop;
	}
	return resolved_hop;
}
static void zebra_nhg_seg_add_sidlist(struct nhg_hash_entry *nhe, struct zebra_sr_policy *policy,
	struct nexthop *nexthop, bool skip_update_depend)
{
	struct nexthop *add_hop = NULL;
	char *policy_sid_name = NULL;
	uint8_t path_num = 0;
	for(path_num = 0; path_num < policy->srv6_segment_list.path_num; path_num++) {
		if (!CHECK_FLAG(policy->srv6_segment_list.sidlists[path_num].type, SRV6_SID_LIST_ADD))
			continue;
		policy_sid_name = policy->srv6_segment_list.sidlists[path_num].sidlist_name;
		add_hop = zebra_nhg_seg_update_nexthop(nexthop, policy_sid_name, true);
		if (skip_update_depend) {
			if (IS_ZEBRA_DEBUG_NHG_DETAIL)
				zlog_debug("%s: nhe id %d add nexthop skip:%s", __func__,
					nhe->id, skip_update_depend ? "true":"false");
			continue;
		}
		if (add_hop->type == NEXTHOP_TYPE_IPV4_SEGMENTLIST)
			handle_recursive_segdepend(&nhe->nhg_segdepends, add_hop, AFI_IP, nhe->type, true);
		else
			handle_recursive_segdepend(&nhe->nhg_segdepends, add_hop, AFI_IP6, nhe->type, true);
		zebra_nhg_segment_depends(nhe, &nhe->nhg_segdepends);
	}
	return;
}
static void zebra_nhg_seg_del_sidlist(struct nhg_hash_entry *nhe, struct zebra_sr_policy *policy,
	struct nexthop *nexthop, bool skip_update_depend)
{
	struct nexthop *del_hop = NULL;
	struct nhg_segment *rb_node_dep = NULL;
	char *policy_sid_name = NULL;
	char *node_sid_name = NULL;
	uint8_t path_num = 0;
	for(path_num = 0; path_num < policy->srv6_segment_list.path_num_old; path_num++) {
		if (!CHECK_FLAG(policy->srv6_segment_list.sidlists_old[path_num].type, SRV6_SID_LIST_DEL))
			continue;
		policy_sid_name = policy->srv6_segment_list.sidlists_old[path_num].sidlist_name;
		del_hop = zebra_nhg_seg_update_nexthop(nexthop, policy_sid_name, false);
		if (del_hop == NULL)
			continue;
		if (IS_ZEBRA_DEBUG_NHG_DETAIL) {
			if (del_hop->type == NEXTHOP_TYPE_IPV4_SEGMENTLIST)
				zlog_debug("%s:nhe id %d delete nexthop %pI4 color %d", __func__, nhe->id,
					&del_hop->gate.ipv4, nexthop->srte_color);
			else
				zlog_debug("%s:nhe id %d delete nexthop %pI6 color %d", __func__, nhe->id,
					&del_hop->gate.ipv6, nexthop->srte_color);
		}
		if (skip_update_depend) {
			if (IS_ZEBRA_DEBUG_NHG_DETAIL)
				zlog_debug("%s: nhe id %d del nexthop skip:%s", __func__,
					nhe->id, skip_update_depend ? "true":"false");
			continue;
		}
		frr_each_safe(nhg_segment_tree, &nhe->nhg_segdepends, rb_node_dep) {
			node_sid_name = rb_node_dep->nhe->nhg.nexthop->sidlist_name;
			if (memcmp(node_sid_name, policy_sid_name, SRTE_SEGMENTLIST_NAME_MAX_LENGTH) == 0)
				zebra_nhg_seg_release(rb_node_dep->nhe);
		}
	}
	return;
}
static void zebra_nhg_seg_update_nhe(struct nhg_hash_entry *nhe,
	struct zebra_sr_policy *policy)
{
	struct nexthop *nexthop = NULL;
	bool skip_update_depend = false;
	int ret = 0;
	for (nexthop = nhe->nhg.nexthop; nexthop; nexthop = nexthop->next) {
		switch (policy->node->p.family) {
		case AF_INET:
			ret = memcmp(&nexthop->gate.ipv4, &policy->node->p.u.prefix, sizeof(struct in_addr));
			break;
		case AF_INET6:
			ret = memcmp(&nexthop->gate.ipv6, &policy->node->p.u.prefix, sizeof(struct in6_addr));
			break;
		default:
			continue;
		}
		if (ret != 0)
			continue;
		if (nexthop->next != NULL || nexthop->prev != NULL) {
			skip_update_depend = true;
		}
		if (IS_ZEBRA_DEBUG_NHG_DETAIL) {
			if (policy->node->p.family == AF_INET)
				zlog_debug("%s: update nhe id:%d gate:%pI4 color:%d", __func__, nhe->id,
					&nexthop->gate.ipv4, nexthop->srte_color);
			else
				zlog_debug("%s: update nhe id:%d gate:%pI6 color:%d", __func__, nhe->id,
					&nexthop->gate.ipv6, nexthop->srte_color);
		}
		zebra_nhg_seg_add_sidlist(nhe, policy, nexthop, skip_update_depend);
		zebra_nhg_seg_del_sidlist(nhe, policy, nexthop, skip_update_depend);
	}
}
static void zebra_nhg_install_nhe(struct nhg_hash_entry *nhe)
{
	UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
	zebra_nhg_seg_install_kernel(nhe);
}
static void zebra_nhe_seg_update(struct zebra_sr_policy *policy)
{
	struct nhg_hash_entry *picnhe = NULL;
	struct nhg_segment *rb_node_dep = NULL;
	if (policy == NULL)
		return;
	picnhe = zebra_srv6_find_pic_nhe_by_policy(policy);
	if (!picnhe) {
		return;
	}
	zebra_nhg_seg_update_nhe(picnhe, policy);
	zebra_nhg_install_nhe(picnhe);
	frr_each_safe(nhg_segment_tree, &picnhe->nhg_segdependents, rb_node_dep) {
		zebra_nhg_seg_update_nhe(rb_node_dep->nhe, policy);
		zebra_nhg_install_nhe(rb_node_dep->nhe);
	}
}
static void zebra_srv6_policy_down_update_pic_nhe(struct zebra_sr_policy *policy)
{
	struct nhg_hash_entry *picnhe = NULL;
	struct nhg_segment *rb_node_dep = NULL;
	picnhe = zebra_srv6_find_pic_nhe_by_policy(policy);
	if (!picnhe) {
		return;
	}
	if (IS_ZEBRA_DEBUG_NHG_DETAIL)
		zlog_debug("%s: nhe id=%d flags=0x%x", __func__,
			picnhe->id, picnhe->flags);
	UNSET_FLAG(picnhe->flags, NEXTHOP_GROUP_VALID);
	frr_each_safe(nhg_segment_tree, &picnhe->nhg_segdepends, rb_node_dep) {
		UNSET_FLAG(rb_node_dep->nhe->flags, NEXTHOP_GROUP_VALID);
	}
	frr_each_safe(nhg_segment_tree, &picnhe->nhg_segdependents, rb_node_dep) {
		zebra_nhg_install_nhe(rb_node_dep->nhe);
	}
}
int zebra_sr_policy_notify_update_client(struct rnh *rnh, struct zebra_sr_policy *policy,
						struct zserv *client)
{
	const struct zebra_nhlfe *nhlfe;
	struct stream *s;
	uint32_t message = 0;
	unsigned long nump = 0;
	uint8_t num;
	struct zapi_nexthop znh;
	int ret;
	struct nexthop nh = {0};
	struct route_node *rn;
	rn = rnh->node;

	/* Get output stream. */
	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, ZEBRA_NEXTHOP_UPDATE, rnh->vrf_id);

	/* Message flags. */
	SET_FLAG(message, ZAPI_MESSAGE_SRTE);
	stream_putl(s, message);

	stream_putw(s, rnh->safi);
	/*
	 * The prefix is copied twice because the ZEBRA_NEXTHOP_UPDATE
	 * code was modified to send back both the matched against
	 * as well as the actual matched.  There does not appear to
	 * be an equivalent here so just send the same thing twice.
	 */
	stream_putw(s, rn->p.family);
	stream_putc(s, rn->p.prefixlen);
	switch (rn->p.family) {
	case AF_INET:
		stream_put_in_addr(s, &rn->p.u.prefix4);
		break;
	case AF_INET6:
		stream_put(s, &rn->p.u.prefix6, IPV6_MAX_BYTELEN);
		break;
	default:
		flog_err(EC_ZEBRA_RNH_UNKNOWN_FAMILY,
			 "%s: Unknown family (%d) notification attempted",
			 __func__, rn->p.family);
		goto failure;
	}
	stream_putw(s, rnh->resolved_route.family);
	stream_putc(s, rnh->resolved_route.prefixlen);
	switch (rnh->resolved_route.family) {
	case AF_INET:
		stream_put_in_addr(s, &rnh->resolved_route.u.prefix4);
		break;
	case AF_INET6:
		stream_put(s, &rnh->resolved_route.u.prefix6, IPV6_MAX_BYTELEN);
		break;
	default:
		flog_err(EC_ZEBRA_RNH_UNKNOWN_FAMILY,
			 "%s: Unknown family (%d) notification attempted",
			 __func__, rn->p.family);
		goto failure;
	}
	stream_putl(s, rnh->srte_color);

	num = 0;
	if (policy && policy->type == ZEBRA_SR_POLICY_TYPE_LSP)
	{
	frr_each (nhlfe_list_const, &policy->lsp->nhlfe_list, nhlfe) {
		if (!CHECK_FLAG(nhlfe->flags, NHLFE_FLAG_SELECTED)
		    || CHECK_FLAG(nhlfe->flags, NHLFE_FLAG_DELETED))
			continue;

		if (num == 0) {
			stream_putc(s, re_type_from_lsp_type(nhlfe->type));
			stream_putw(s, 0); /* instance - not available */
			stream_putc(s, nhlfe->distance);
			stream_putl(s, 0); /* metric - not available */
			nump = stream_get_endp(s);
			stream_putc(s, 0);
		}

		zapi_nexthop_from_nexthop(&znh, nhlfe->nexthop);
		ret = zapi_nexthop_encode(s, &znh, 0, message);
		if (ret < 0)
			goto failure;

		num++;
	}
	stream_putc_at(s, nump, num);
	}
	else if (policy && policy->type == ZEBRA_SR_POLICY_TYPE_SRV6)
	{
		stream_putc(s, ZEBRA_ROUTE_SRTE);
		stream_putw(s, 0); /* instance - not available */
		stream_putc(s, 0);/* distance - not available */
		stream_putl(s, 0); /* metric - not available */
		if (policy->status == ZEBRA_SR_POLICY_UP) {
			stream_putc(s, 1);
			memset(&nh, 0, sizeof(struct nexthop));
			nh.vrf_id = policy->zvrf->vrf->vrf_id;
			switch (policy->node->p.family) {
				case IPADDR_V4:
					memcpy(&nh.gate.ipv4, &policy->node->p.u.prefix, sizeof(struct in_addr));
					nh.type = NEXTHOP_TYPE_IPV4_SEGMENTLIST;
					break;
				case IPADDR_V6:
					memcpy(&nh.gate.ipv6, &policy->node->p.u.prefix, sizeof(struct in6_addr));
					nh.type = NEXTHOP_TYPE_IPV6_SEGMENTLIST;
					break;
				default:
					flog_warn(EC_LIB_DEVELOPMENT,
						"%s: unknown policy endpoint address family: %u",
						__func__, policy->node->p.family);
					exit(1);
			}
			zapi_nexthop_from_nexthop(&znh, &nh);
			ret = zapi_nexthop_encode(s, &znh, 0, message);
			if (ret < 0)
				goto failure;
		}
		else
			stream_putc(s, 0);
	}
	else {
		stream_putc(s, ZEBRA_ROUTE_SRTE);
		stream_putw(s, 0); /* instance - not available */
		stream_putc(s, 0);/* distance - not available */
		stream_putl(s, 0); /* metric - not available */
		stream_putc(s, 0);
	}
	stream_putw_at(s, 0, stream_get_endp(s));

	client->nh_last_upd_time = monotime(NULL);
	return zserv_send_message(client, s);

failure:

	stream_free(s);
	return -1;
}

void zebra_sr_policy_notify_update(struct rnh *rnh, struct zebra_sr_policy *policy,
	struct zserv *zclient)
{
	struct listnode *node;
	struct zserv *client;
	if (zclient) {
		zebra_sr_policy_notify_update_client(rnh, policy, zclient);
	}
	else {
		for (ALL_LIST_ELEMENTS_RO(rnh->client_list, node, client)) {
			zebra_sr_policy_notify_update_client(rnh, policy, client);
		}
	}
}

int zebra_sr_policy_notify_unknown(struct rnh *rnh,
						struct zserv *client)
{
	struct stream *s;
	uint32_t message = 0;
    struct route_node *rn;

    rn = rnh->node;

	s = stream_new(ZEBRA_MAX_PACKET_SIZ);

	zclient_create_header(s, ZEBRA_NEXTHOP_UPDATE, rnh->vrf_id);

	SET_FLAG(message, ZAPI_MESSAGE_SRTE);
	stream_putl(s, message);
	stream_putw(s, rnh->safi);
	switch (rn->p.family) {
	case AF_INET:
		stream_putw(s, AF_INET);
		stream_putc(s, IPV4_MAX_BITLEN);
		stream_put_in_addr(s, &rn->p.u.prefix4);
		stream_putw(s, AF_INET);
		stream_putc(s, IPV4_MAX_BITLEN);
		stream_put_in_addr(s, &rn->p.u.prefix4);
		break;
	case AF_INET6:
		stream_putw(s, AF_INET6);
		stream_putc(s, IPV6_MAX_BITLEN);
		stream_put(s, &rn->p.u.prefix6, IPV6_MAX_BYTELEN);
		stream_putw(s, AF_INET6);
		stream_putc(s, IPV6_MAX_BITLEN);
		stream_put(s, &rn->p.u.prefix6, IPV6_MAX_BYTELEN);
		break;
	default:
		flog_warn(EC_LIB_DEVELOPMENT,
			  "%s: unknown policy endpoint address family: %u",
			  __func__, rn->p.family);
		exit(1);
	}

	stream_putl(s, rnh->srte_color);

    stream_putc(s, ZEBRA_ROUTE_SRTE);
	stream_putw(s, 0); /* instance - not available */
	stream_putc(s, 0);/* distance - not available */
	stream_putl(s, 0); /* metric - not available */
    stream_putc(s, 0);
			/* Fallback to the IGP shortest path. */
    stream_putw_at(s, 0, stream_get_endp(s));
	client->nh_last_upd_time = monotime(NULL);
	client->last_write_cmd = ZEBRA_NEXTHOP_UPDATE;
	return zserv_send_message(client, s);

}

static void zebra_sr_policy_activate(struct zebra_sr_policy *policy,
				     struct zebra_lsp *lsp)
{
	policy->status = ZEBRA_SR_POLICY_UP;
	policy->lsp = lsp;
	(void)zebra_sr_policy_bsid_install(policy);
	zsend_sr_policy_notify_status(policy->color, policy->node,
				      policy->name, ZEBRA_SR_POLICY_UP);
	zebra_srte_evaluate_rn_nexthops(policy, false);
}

static void zebra_sr_policy_update(struct zebra_sr_policy *policy,
				   struct zebra_lsp *lsp,
				   struct zapi_srte_tunnel *old_tunnel)
{
	bool bsid_changed;
	bool segment_list_changed;

	policy->lsp = lsp;

	bsid_changed =
		policy->segment_list.local_label != old_tunnel->local_label;
	segment_list_changed =
		policy->segment_list.label_num != old_tunnel->label_num
		|| memcmp(policy->segment_list.labels, old_tunnel->labels,
			  sizeof(mpls_label_t)
				  * policy->segment_list.label_num);

	/* Re-install label stack if necessary. */
	if (bsid_changed || segment_list_changed) {
		zebra_sr_policy_bsid_uninstall(policy, old_tunnel->local_label);
		(void)zebra_sr_policy_bsid_install(policy);
	}

	zsend_sr_policy_notify_status(policy->color, policy->node,
				      policy->name, ZEBRA_SR_POLICY_UP);

	/* Handle segment-list update. */
	if (segment_list_changed)
		zebra_srte_evaluate_rn_nexthops(policy, false);
}
static bool zebra_srv6_policy_set_sidlist_type(struct zapi_srv6te_tunnel *te_tunnel,
	char *sidlist_name, uint32_t weight)
{
	for (uint32_t i = 0; i < te_tunnel->path_num_old; i++) {
		if (sidlist_name != NULL && strcmp(te_tunnel->sidlists_old[i].sidlist_name, sidlist_name) == 0) {
			if (te_tunnel->sidlists_old[i].weight != weight)
				SET_FLAG(te_tunnel->sidlists_old[i].type, SRV6_SID_LIST_UPDATE);
			UNSET_FLAG(te_tunnel->sidlists_old[i].type, SRV6_SID_LIST_DEL);
			return true;
		}
	}
	return false;
}
static bool zebra_srv6_policy_check_update(struct zapi_srv6te_tunnel *new_tunnel)
{
	uint8_t path_num = 0;
	uint8_t path_num_old = 0;
	bool segment_list_changed = false;
	bool find = false;
	for (path_num = 0; path_num < new_tunnel->path_num; path_num++) {
		find = zebra_srv6_policy_set_sidlist_type(new_tunnel, new_tunnel->sidlists[path_num].sidlist_name,
			new_tunnel->sidlists[path_num].weight);
		if (find == false) {
			SET_FLAG(new_tunnel->sidlists[path_num].type, SRV6_SID_LIST_ADD);
			segment_list_changed = true;
		}
	}
	if (segment_list_changed)
		return true;
	for(path_num_old = 0; path_num_old < new_tunnel->path_num_old; path_num_old++) {
		if (new_tunnel->sidlists_old[path_num_old].type != 0)
			return true;
	}
	return segment_list_changed;
}
static void zebra_srv6_clear_old_sidlist(struct zapi_srv6te_tunnel *new_tunnel)
{
	if (new_tunnel == NULL)
		return;
	memset(&new_tunnel->sidlists_old, 0,
		ZEBRA_SID_LIST_MAX_NUM * sizeof(struct zapi_srv6_active_sidlist));
	new_tunnel->path_num_old = 0;
}
void zebra_srv6_policy_validate(struct zebra_sr_policy *policy,
			     struct zapi_srv6te_tunnel *new_tunnel, bool new)
{
	uint8_t path_num = 0;
	bool segment_list_changed = false;
	zebra_srv6_clear_old_sidlist(new_tunnel);
	if (new == false) {
		for (path_num = 0; path_num < policy->srv6_segment_list.path_num; path_num++) {
			strlcpy(new_tunnel->sidlists_old[path_num].sidlist_name, policy->srv6_segment_list.sidlists[path_num].sidlist_name,
				sizeof(policy->srv6_segment_list.sidlists[path_num].sidlist_name));
			new_tunnel->sidlists_old[path_num].weight = policy->srv6_segment_list.sidlists[path_num].weight;
			SET_FLAG(new_tunnel->sidlists_old[path_num].type, SRV6_SID_LIST_DEL);
		}
		new_tunnel->path_num_old = policy->srv6_segment_list.path_num;
		segment_list_changed = zebra_srv6_policy_check_update(new_tunnel);
		policy->srv6_segment_list = *new_tunnel;
		policy->type = ZEBRA_SR_POLICY_TYPE_SRV6;
		if (segment_list_changed)
			zebra_nhe_seg_update(policy);
		return;
	}
	policy->srv6_segment_list = *new_tunnel;
	policy->type = ZEBRA_SR_POLICY_TYPE_SRV6;
	zebra_srte_evaluate_rn_nexthops(policy, false);
}

static void zebra_sr_policy_deactivate(struct zebra_sr_policy *policy)
{
	if (is_default_prefix(&policy->node->p))
		policy->status = ZEBRA_SR_POLICY_INIT;
	else
	policy->status = ZEBRA_SR_POLICY_DOWN;
	policy->lsp = NULL;
    if (policy->type == ZEBRA_SR_POLICY_TYPE_LSP)
    {
        zebra_sr_policy_bsid_uninstall(policy, policy->segment_list.local_label);
    }
	if (policy->type == ZEBRA_SR_POLICY_TYPE_SRV6)
		zebra_srv6_policy_down_update_pic_nhe(policy);
	zsend_sr_policy_notify_status(policy->color, policy->node,
				      policy->name, ZEBRA_SR_POLICY_DOWN);
	zebra_srte_evaluate_rn_nexthops(policy, true);
}

int zebra_sr_policy_validate(struct zebra_sr_policy *policy,
			     struct zapi_srte_tunnel *new_tunnel)
{
	struct zapi_srte_tunnel old_tunnel = policy->segment_list;
	struct zebra_lsp *lsp;

	if (new_tunnel)
		policy->segment_list = *new_tunnel;

	/* Try to resolve the Binding-SID nexthops. */
	lsp = mpls_lsp_find(policy->zvrf, policy->segment_list.labels[0]);
	if (!lsp || !lsp->best_nhlfe
	    || lsp->addr_family != policy->node->p.family) {
		if (policy->status == ZEBRA_SR_POLICY_UP)
			zebra_sr_policy_deactivate(policy);
		return -1;
	}

	/* First label was resolved successfully. */
	if (policy->status == ZEBRA_SR_POLICY_DOWN)
		zebra_sr_policy_activate(policy, lsp);
	else
		zebra_sr_policy_update(policy, lsp, &old_tunnel);

	return 0;
}

int zebra_sr_policy_bsid_install(struct zebra_sr_policy *policy)
{
	struct zapi_srte_tunnel *zt = &policy->segment_list;
	struct zebra_nhlfe *nhlfe;

	if (zt->local_label == MPLS_LABEL_NONE)
		return 0;

	frr_each_safe (nhlfe_list, &policy->lsp->nhlfe_list, nhlfe) {
		uint8_t num_out_labels;
		mpls_label_t *out_labels;
		mpls_label_t null_label = MPLS_LABEL_IMPLICIT_NULL;

		if (!CHECK_FLAG(nhlfe->flags, NHLFE_FLAG_SELECTED)
		    || CHECK_FLAG(nhlfe->flags, NHLFE_FLAG_DELETED))
			continue;

		/*
		 * Don't push the first SID if the corresponding action in the
		 * LFIB is POP.
		 */
		if (!nhlfe->nexthop->nh_label
		    || !nhlfe->nexthop->nh_label->num_labels
		    || nhlfe->nexthop->nh_label->label[0]
			       == MPLS_LABEL_IMPLICIT_NULL) {
			if (zt->label_num > 1) {
				num_out_labels = zt->label_num - 1;
				out_labels = &zt->labels[1];
			} else {
				num_out_labels = 1;
				out_labels = &null_label;
			}
		} else {
			num_out_labels = zt->label_num;
			out_labels = zt->labels;
		}

		if (mpls_lsp_install(
			    policy->zvrf, zt->type, zt->local_label,
			    num_out_labels, out_labels, nhlfe->nexthop->type,
			    &nhlfe->nexthop->gate, nhlfe->nexthop->ifindex)
		    < 0)
			return -1;
	}

	return 0;
}

void zebra_sr_policy_bsid_uninstall(struct zebra_sr_policy *policy,
				    mpls_label_t old_bsid)
{
	struct zapi_srte_tunnel *zt = &policy->segment_list;

	mpls_lsp_uninstall_all_vrf(policy->zvrf, zt->type, old_bsid);
}

int zebra_srv6_sidlist_install(struct zapi_srv6_sidlist *sidlist)
{
	struct zebra_srv6_sidlist *sid_list = XCALLOC(MTYPE_ZEBRA_SRV6_SIDLIST, sizeof(struct zebra_srv6_sidlist));
	snprintf(sid_list->sidlist_name_, SRV6_SEGMENTLIST_NAME_MAX_LENGTH, "%s", sidlist->sidlist_name);
	sid_list->segment_count_ = sidlist->segment_count;
	for (uint32_t i = 0; i < sidlist->segment_count; i++) {
		sid_list->segments_[i].index_ = sidlist->segments[i].index;
		sid_list->segments_[i].srv6_sid_value_ = sidlist->segments[i].srv6_sid_value;
	}
	srv6_sidlist_install(sid_list);
	return 0;
}

void zebra_srv6_sidlist_uninstall(struct zapi_srv6_sidlist *sidlist)
{
	struct zebra_srv6_sidlist *sid_list = XCALLOC(MTYPE_ZEBRA_SRV6_SIDLIST, sizeof(struct zebra_srv6_sidlist));
	snprintf(sid_list->sidlist_name_, SRV6_SEGMENTLIST_NAME_MAX_LENGTH, "%s", sidlist->sidlist_name);
	sid_list->segment_count_ = sidlist->segment_count;
	for (uint32_t i = 0; i < sidlist->segment_count; i++) {
		sid_list->segments_[i].index_ = sidlist->segments[i].index;
		sid_list->segments_[i].srv6_sid_value_ = sidlist->segments[i].srv6_sid_value;
	}
	srv6_sidlist_uninstall(sid_list);
	return;
}

int zebra_sr_policy_label_update_walk(struct hash_bucket *hb, void *arg)
{
	struct zebra_sr_policy *policy;
	struct route_table *table;
	struct route_node *rn;
	struct zebra_sr_policy_label_para *para = arg;
	table = hb->data;
	if (!table) {
		return 0;
	}

	for (rn = route_top(table); rn; rn = route_next(rn)) {
		policy = rn->info;
		if (!policy)
			continue;
		mpls_label_t next_hop_label;

		next_hop_label = policy->segment_list.labels[0];
		if (next_hop_label != para->label)
			continue;

		switch (para->mode) {
		case ZEBRA_SR_POLICY_LABEL_CREATED:
		case ZEBRA_SR_POLICY_LABEL_UPDATED:
		case ZEBRA_SR_POLICY_LABEL_REMOVED:
			zebra_sr_policy_validate(policy, NULL);
			break;
		}
	}

	return 0;
}

int zebra_sr_policy_label_update(mpls_label_t label,
				 enum zebra_sr_policy_update_label_mode mode)
{
	struct zebra_sr_policy_label_para para = {0};
	para.label = label;
	para.mode = mode;
	hash_walk(srte_table_hash, zebra_sr_policy_label_update_walk, &para);

		return 0;
}

struct route_table *zebra_srte_table_create(afi_t afi, uint32_t color)
{
	struct route_node *rn;
	struct prefix p;
	struct route_table *table;
	struct zebra_sr_policy *policy;
	table = route_table_init();
	memset(&p, 0, sizeof(p));
	p.family = afi2family(afi);
	rn = route_node_get(table, &p);
	policy = rn->info;
	if (!policy) {
		policy = XCALLOC(MTYPE_ZEBRA_SR_POLICY, sizeof(struct zebra_sr_policy));
		route_lock_node(rn);
		policy->node = rn;
		policy->color = color;
		policy->status = ZEBRA_SR_POLICY_INIT;
		rnh_list_init(&policy->nht);
		rn->info = policy;
	} 
	return table;
}
static uint32_t srte_table_hash_key_make(const void *arg)
{
	const struct srte_table_key *srte_key = arg;
	uint32_t key = 0;
	key = jhash_1word(srte_key->afi, key);
	key = jhash_1word(srte_key->color, key);
	return key;
}
static bool srte_table_hash_same(const void *arg1, const void *arg2)
{
	const struct srte_table_key *srte_key1 = arg1;
	const struct srte_table_key *srte_key2 = arg2;
	if (srte_key1->afi != srte_key2->afi)
		return false;
	return (srte_key1->color == srte_key2->color);
}
void *srte_table_alloc(void *arg)
{
	struct route_table *srte_table;
	struct srte_table_key *srte_key = arg;
	struct srte_table_key *srte_key_table = NULL;
	srte_key_table = XCALLOC(MTYPE_ZEBRA_SR_POLICY, sizeof(struct srte_table_key));
	srte_table = zebra_srte_table_create(srte_key->afi, srte_key->color);
	srte_key_table->table = srte_table;
	srte_key_table->afi = srte_key->afi;
	srte_key_table->color = srte_key->color;
	return srte_key_table;
}
void zebra_srte_evaluate_rn_nexthops(struct zebra_sr_policy *policy, bool rt_delete)
{
	struct route_node *rn;
	struct rnh *rnh;
	struct zebra_sr_policy *policyNext = policy;
	rn = policy->node;
	while (rn) {
		if (!policyNext) {
			rn = rn->parent;
			if (rn)
				policyNext = rn->info;
			continue;
	}
		if (rt_delete && (!rnh_list_count(&policyNext->nht))) {
			if (IS_ZEBRA_DEBUG_NHT_DETAILED)
				zlog_debug("%pRN has no tracking NHTs. Bailing",
					   rn);
			break;
		}
		if (!rnh_list_count(&policyNext->nht)) {
			rn = rn->parent;
			if (rn)
				policyNext = rn->info;
			continue;
		}
		frr_each_safe(rnh_list, &policyNext->nht, rnh) {
			struct prefix *p = &rnh->node->p;
			zebra_evaluate_rnh_by_srte(family2afi(p->family), rnh);
		}
		rn = rn->parent;
		if (rn)
			policyNext = rn->info;
	}
}

void zebra_srte_init(void)
{
	srte_table_hash = hash_create(srte_table_hash_key_make, srte_table_hash_same,
				    "SRTE table Hash");
	srte_table_hash->max_size = 1000;
}
