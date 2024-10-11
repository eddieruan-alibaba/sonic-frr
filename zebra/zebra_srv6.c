/*
 * Zebra SRv6 definitions
 * Copyright (C) 2020  Hiroki Shirokura, LINE Corporation
 * Copyright (C) 2020  Masakazu Asama
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "network.h"
#include "prefix.h"
#include "stream.h"
#include "srv6.h"
#include "zebra/debug.h"
#include "zebra/zapi_msg.h"
#include "zebra/zserv.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_srv6.h"
#include "zebra/zebra_errors.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "zebra/zebra_srte.h"

DEFINE_MGROUP(SRV6_MGR, "SRv6 Manager");
DEFINE_MTYPE_STATIC(SRV6_MGR, SRV6M_CHUNK, "SRv6 Manager Chunk");

/* define hooks for the basic API, so that it can be specialized or served
 * externally
 */

DEFINE_HOOK(srv6_manager_client_connect,
	    (struct zserv *client, vrf_id_t vrf_id),
	    (client, vrf_id));
DEFINE_HOOK(srv6_manager_client_disconnect,
	    (struct zserv *client), (client));
DEFINE_HOOK(srv6_manager_get_chunk,
	    (struct srv6_locator **loc,
	     struct zserv *client,
	     const char *locator_name,
	     vrf_id_t vrf_id),
	    (loc, client, locator_name, vrf_id));
DEFINE_HOOK(srv6_manager_release_chunk,
	    (struct zserv *client,
	     const char *locator_name,
	     vrf_id_t vrf_id),
	    (client, locator_name, vrf_id));

DEFINE_HOOK(srv6_manager_get_sid,
	    (struct srv6_locator **loc,
	     struct zserv *client,
	     const char *locator_name,
	     vrf_id_t vrf_id),
	    (loc, client, locator_name, vrf_id));
DEFINE_HOOK(srv6_manager_release_sid,
	    (struct zserv *client,
	     const char *locator_name,
	     vrf_id_t vrf_id),
	    (client, locator_name, vrf_id));
DEFINE_HOOK(srv6_manager_get_locator_sid_all,
	    (struct zserv *client,
	     vrf_id_t vrf_id),
	    (client, vrf_id));

int srv6_sidlist_install(struct zebra_srv6_sidlist *sid_list)
{
	dplane_sidlist_add(sid_list);
	return 0;
}

void srv6_sidlist_uninstall(struct zebra_srv6_sidlist *sid_list)
{
	dplane_sidlist_delete(sid_list);
	return;
}


/* define wrappers to be called in zapi_msg.c (as hooks must be called in
 * source file where they were defined)
 */

void srv6_manager_client_connect_call(struct zserv *client, vrf_id_t vrf_id)
{
	hook_call(srv6_manager_client_connect, client, vrf_id);
}

void srv6_manager_get_locator_chunk_call(struct srv6_locator **loc,
					 struct zserv *client,
					 const char *locator_name,
					 vrf_id_t vrf_id)
{
	hook_call(srv6_manager_get_chunk, loc, client, locator_name, vrf_id);
}

void srv6_manager_release_locator_chunk_call(struct zserv *client,
					     const char *locator_name,
					     vrf_id_t vrf_id)
{
	hook_call(srv6_manager_release_chunk, client, locator_name, vrf_id);
}

void srv6_manager_get_locator_sid_call(struct srv6_locator **loc,
					 struct zserv *client,
					 const char *locator_name,
					 vrf_id_t vrf_id)
{
	hook_call(srv6_manager_get_sid, loc, client, locator_name, vrf_id);
}

void srv6_manager_get_locator_all_call(struct zserv *client,
					 vrf_id_t vrf_id)
{
	hook_call(srv6_manager_get_locator_sid_all, client, vrf_id);
}

int srv6_manager_client_disconnect_cb(struct zserv *client)
{
	hook_call(srv6_manager_client_disconnect, client);
	return 0;
}

static int zebra_srv6_cleanup(struct zserv *client)
{
	return 0;
}

void zebra_srv6_locator_add(struct srv6_locator *locator)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *tmp;
	struct listnode *node;
	struct zserv *client;

	tmp = zebra_srv6_locator_lookup(locator->name);
	if (!tmp)
		listnode_add(srv6->locators, locator);

	/*
	 * Notify new locator info to zclients.
	 *
	 * The srv6 locators and their prefixes are managed by zserv(zebra).
	 * And an actual configuration the srv6 sid in the srv6 locator is done
	 * by zclient(bgpd, isisd, etc). The configuration of each locator
	 * allocation and specify it by zserv and zclient should be
	 * asynchronous. For that, zclient should be received the event via
	 * ZAPI when a srv6 locator is added on zebra.
	 * Basically, in SRv6, adding/removing SRv6 locators is performed less
	 * frequently than adding rib entries, so a broad to all zclients will
	 * not degrade the overall performance of FRRouting.
	 */
	for (ALL_LIST_ELEMENTS_RO(zrouter.client_list, node, client))
		zsend_zebra_srv6_locator_add(client, locator);
}

void zebra_srv6_prefix_delete(struct srv6_locator *locator)
{
	struct listnode *n, *nnode;
	struct srv6_locator_chunk *c;
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct seg6_sid *sid = NULL;
	struct zserv *client;
	struct listnode *client_node;

	for (ALL_LIST_ELEMENTS(locator->sids, n, nnode, sid))
	{
		for (ALL_LIST_ELEMENTS_RO(zrouter.client_list,
			client_node, client)) {

			zsend_srv6_manager_del_sid(client, VRF_DEFAULT, locator, sid);
		}
		zebra_srv6_local_sid_del(locator, sid);
		listnode_delete(locator->sids, sid);
		srv6_locator_sid_free(sid);
	}
	
	memset(&locator->prefix, 0, sizeof(struct prefix_ipv6));
	locator->block_bits_length = 0;
	locator->node_bits_length = 0;
	locator->function_bits_length = 0;
	locator->argument_bits_length = 0;

	for (ALL_LIST_ELEMENTS_RO(zrouter.client_list, client_node, client))
		zsend_zebra_srv6_locator_delete(client, locator);
	
	locator->status_up = false;
}

void zebra_srv6_locator_delete(struct srv6_locator *locator)
{
	struct listnode *n, *nnode;
	struct srv6_locator_chunk *c;
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct seg6_sid *sid = NULL;
	struct zserv *client;
	struct listnode *client_node;

	for (ALL_LIST_ELEMENTS(locator->sids, n, nnode, sid))
	{
		for (ALL_LIST_ELEMENTS_RO(zrouter.client_list,
			client_node, client)) {

			zsend_srv6_manager_del_sid(client, VRF_DEFAULT, locator, sid);
		}
		zebra_srv6_local_sid_del(locator, sid);
		listnode_delete(locator->sids, sid);
		srv6_locator_sid_free(sid);
	}	

	for (ALL_LIST_ELEMENTS_RO(zrouter.client_list, client_node, client))
		zsend_zebra_srv6_locator_delete(client, locator);

	listnode_delete(srv6->locators, locator);
	srv6_locator_free(locator);
}


extern bool zebra_srv6_local_sid_format_valid(struct srv6_locator *locator, struct seg6_sid *sid)
{
	struct in6_addr result_sid = {0};
	uint16_t sid_masklen = 0;
	combine_sid(locator, &sid->ipv6Addr.prefix, &result_sid);

	sid_masklen = locator->block_bits_length + locator->node_bits_length + locator->function_bits_length;
	// Logic is the same as la_vrf_impl::verify_srv6_endpoint
	// addr_msb
	//uint32_t addr_0 = result_sid.s6_addr32[0];
	uint32_t addr_1 = result_sid.s6_addr32[1];

	// addr_lsb
	uint32_t addr_2 = result_sid.s6_addr32[2];
	uint32_t addr_3 = result_sid.s6_addr32[3];

	if (locator->format == SRV6_FORMAT_F1) {
		if (sid_masklen == 128) {
			// Verify that bits [39:0] are zero
			if ((addr_2 & 0xff000000) == 0 &&
				(addr_3 & 0xffffffff) == 0) {
				return true;
			}
		}
		return false;
	}

	if (locator->format != SRV6_FORMAT_USID_3216) {
		return false;
	}

	// Check for valid prefix lengths. /48, /64, /80.
	if (sid_masklen == 48) {
		// Make sure that bits [95:80] are not zero.
		if ((addr_1 & 0xffff) != 0) {
			return true;
		}
		return false;
	}

	if (sid_masklen == 64) {
		// Make sure that bits [79:64] are not zero.
		if ((addr_1 & 0xffff0000) != 0) {
			return true;
		}
		return false;
	}

	if (sid_masklen == 80) {
		// WLIB format
		// Make sure that [79:64] == 0xfff_0xxx
		if ((addr_1 & 0xf8ff0000) != 0xf0ff0000) {
			return false;
		}
		// Make sure that bits [63:48] are not zero.
		if ((addr_2 & 0xffff) != 0) {
			return true;
		}
	}

	return false;
}

struct srv6_locator *zebra_srv6_locator_lookup(const char *name)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *locator;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator))
		if (!strncmp(name, locator->name, SRV6_LOCNAME_SIZE))
			return locator;
	return NULL;
}

void zebra_notify_srv6_locator_add(struct srv6_locator *locator)
{
	struct listnode *node;
	struct zserv *client;

	/*
	 * Notify new locator info to zclients.
	 *
	 * The srv6 locators and their prefixes are managed by zserv(zebra).
	 * And an actual configuration the srv6 sid in the srv6 locator is done
	 * by zclient(bgpd, isisd, etc). The configuration of each locator
	 * allocation and specify it by zserv and zclient should be
	 * asynchronous. For that, zclient should be received the event via
	 * ZAPI when a srv6 locator is added on zebra.
	 * Basically, in SRv6, adding/removing SRv6 locators is performed less
	 * frequently than adding rib entries, so a broad to all zclients will
	 * not degrade the overall performance of FRRouting.
	 */
	for (ALL_LIST_ELEMENTS_RO(zrouter.client_list, node, client))
		zsend_zebra_srv6_locator_add(client, locator);
}

void zebra_notify_srv6_locator_delete(struct srv6_locator *locator)
{
	struct listnode *n;
	struct srv6_locator_chunk *c;
	struct zserv *client;

	/*
	 * Notify deleted locator info to zclients if needed.
	 *
	 * zclient(bgpd,isisd,etc) allocates a sid from srv6 locator chunk and
	 * uses it for its own purpose. For example, in the case of BGP L3VPN,
	 * the SID assigned to vpn unicast rib will be given.
	 * And when the locator is deleted by zserv(zebra), those SIDs need to
	 * be withdrawn. The zclient must initiate the withdrawal of the SIDs
	 * by ZEBRA_SRV6_LOCATOR_DELETE, and this notification is sent to the
	 * owner of each chunk.
	 */
	for (ALL_LIST_ELEMENTS_RO((struct list *)locator->chunks, n, c)) {
		if (c->proto == ZEBRA_ROUTE_SYSTEM)
			continue;
		client = zserv_find_client(c->proto, c->instance);
		if (!client) {
			zlog_warn("Not found zclient(proto=%u, instance=%u).",
				  c->proto, c->instance);
			continue;
		}
		zsend_zebra_srv6_locator_delete(client, locator);
	}
}

struct zebra_srv6 *zebra_srv6_get_default(void)
{
	static struct zebra_srv6 srv6;
	static bool first_execution = true;

	if (first_execution) {
		first_execution = false;
		srv6.locators = list_new();
	}
	return &srv6;
}

/**
 * Core function, assigns srv6-locator chunks
 *
 * It first searches through the list to check if there's one available
 * (previously released). Otherwise it creates and assigns a new one
 *
 * @param proto Daemon protocol of client, to identify the owner
 * @param instance Instance, to identify the owner
 * @param session_id SessionID of client
 * @param name Name of SRv6-locator
 * @return Pointer to the assigned srv6-locator chunk,
 *         or NULL if the request could not be satisfied
 */
static struct srv6_locator *
assign_srv6_locator_chunk(uint8_t proto,
			  uint16_t instance,
			  uint32_t session_id,
			  const char *locator_name)
{
	bool chunk_found = false;
	struct listnode *node = NULL;
	struct srv6_locator *loc = NULL;
	struct srv6_locator_chunk *chunk = NULL;

	loc = zebra_srv6_locator_lookup(locator_name);
	if (!loc) {
		zlog_info("%s: locator %s was not found",
			  __func__, locator_name);
		return NULL;
	}

	for (ALL_LIST_ELEMENTS_RO((struct list *)loc->chunks, node, chunk)) {
		if (chunk->proto != NO_PROTO && chunk->proto != proto)
			continue;
		chunk_found = true;
		break;
	}

	if (!chunk_found) {
		zlog_info("%s: locator is already owned", __func__);
		return NULL;
	}

	chunk->proto = proto;
	chunk->instance = instance;
	chunk->session_id = session_id;
	return loc;
}

extern bool zebra_srv6_local_sid_get_format(struct srv6_locator *locator)
{
	// Logic is the same as sai_srv6_handler::get_la_sid_format
	if (locator->block_bits_length == 32 &&
		locator->node_bits_length == 16 &&
		locator->function_bits_length == 0) {
		// prefix len = 48
		locator->format = SRV6_FORMAT_USID_3216;
		return true;
	} else if (locator->block_bits_length == 32 &&
		locator->node_bits_length == 0 &&
		locator->function_bits_length == 16) {
		// prefix len = 48
		locator->format = SRV6_FORMAT_USID_3216;
		return true;
	} else if (locator->block_bits_length == 32 &&
		locator->node_bits_length == 16 &&
		locator->function_bits_length == 16) {
		// prefix len = 64
		locator->format = SRV6_FORMAT_USID_3216;
		return true;
	} else if (locator->block_bits_length == 32 &&
		locator->node_bits_length == 16 &&
		locator->function_bits_length == 32) {
		// prefix len = 80
		locator->format = SRV6_FORMAT_USID_3216;
		return true;
	} else if (locator->block_bits_length == 40 &&
		locator->node_bits_length == 24 &&
		locator->function_bits_length == 16 &&
		locator->argument_bits_length == 8) {
		locator->format = SRV6_FORMAT_F1;
		return true;
	}

	return false;
}

void zebra_srv6_local_sid_add(struct srv6_locator *locator, struct seg6_sid *sid)
{
	enum seg6local_action_t act;
	struct seg6local_context ctx = {};
	struct in6_addr result_sid = {0};
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

    combine_sid(locator, &sid->ipv6Addr.prefix, &result_sid);

	vrf = vrf_lookup_by_name(sid->vrfName);
	if (!vrf)
		return;

	zvrf = vrf->info;
    ctx.table = zvrf->table_id;
	act = sid->sidaction;
    ctx.block_bits_length = locator->block_bits_length;
    ctx.node_bits_length = locator->node_bits_length;
    ctx.function_bits_length = locator->function_bits_length;
    ctx.argument_bits_length = locator->argument_bits_length;
    strncpy(ctx.vrfName, sid->vrfName, VRF_NAMSIZ + 1);

    if (CHECK_FLAG(vrf->status, VRF_ACTIVE)) {
		zebra_route_add(&result_sid, vrf, act, &ctx);
	}

}

void zebra_srv6_local_sid_del(struct srv6_locator *locator, struct seg6_sid *sid)
{
	enum seg6local_action_t act;
	struct seg6local_context ctx = {};
	struct in6_addr result_sid = {0};
	struct vrf *vrf;
	struct zebra_vrf *zvrf;

	combine_sid(locator, &sid->ipv6Addr.prefix, &result_sid);

	vrf = vrf_lookup_by_name(sid->vrfName);
	if (!vrf)
		return;
	
	zvrf = vrf->info;
    ctx.table = zvrf->table_id;
	ctx.block_bits_length = locator->block_bits_length;
	ctx.node_bits_length = locator->node_bits_length;
	ctx.function_bits_length = locator->function_bits_length;
	ctx.argument_bits_length = locator->argument_bits_length;
	act = sid->sidaction;

    zebra_route_del(&result_sid, vrf, act, &ctx);

}

int zebra_route_add(struct in6_addr *result_sid, struct vrf *vrf, enum seg6local_action_t act, struct seg6local_context *ctx)
{
	afi_t afi;
	struct prefix_ipv6 *src_p = NULL;
	struct route_entry *re;
	struct nexthop_group *ng = NULL;
	int ret = 0;
	struct nhg_hash_entry nhe, *n;
    struct zebra_vrf *zvrf;
    struct vrf *def_vrf = NULL;
    struct prefix p = {};
    struct nexthop *nexthop;

    p.family = AF_INET6;
    p.prefixlen = ctx->block_bits_length + ctx->node_bits_length + ctx->function_bits_length;
    p.u.prefix6 = *result_sid;

    def_vrf = vrf_lookup_by_name(VRF_DEFAULT_NAME);
    zvrf = zebra_vrf_lookup_by_id(def_vrf->vrf_id);
    if (!zvrf) {
        return ret;
    }

	/* Allocate new route. */
	re = XCALLOC(MTYPE_RE, sizeof(struct route_entry));
	re->type = ZEBRA_ROUTE_STATIC;
	re->instance = 0;
    SET_FLAG(re->flags, ZEBRA_FLAG_ALLOW_RECURSION);
    SET_FLAG(re->flags, ZEBRA_FLAG_LOCAL_SID_ROUTE);
    SET_FLAG(re->status, ROUTE_ENTRY_INSTALLED);
	re->uptime = monotime(NULL);
	re->vrf_id = VRF_DEFAULT;

	re->table = zvrf->table_id;

    ng = nexthop_group_new();
    /*
	 * TBD should _all_ of the nexthop add operations use
	 * api_nh->vrf_id instead of re->vrf_id ? I only changed
	 * for cases NEXTHOP_TYPE_IPV4 and NEXTHOP_TYPE_IPV6.
	 */

	/* Convert zapi nexthop */
    nexthop = nexthop_from_ifindex(vrf->vrf_id, 0);

	if (!nexthop) {
		if (ng)
			nexthop_group_delete(&ng);
		return ret;
	}

	zlog_debug("%s: adding seg6local action %s",
		   __func__,
		   seg6local_action2str(act));

	nexthop_add_srv6_seg6local(nexthop, act, ctx);

	if (ng) {
		/* Add new nexthop to temporary list. This list is
		 * canonicalized - sorted - so that it can be hashed
		 * later in route processing. We expect that the sender
		 * has sent the list sorted, and the zapi client api
		 * attempts to enforce that, so this should be
		 * inexpensive - but it is necessary to support shared
		 * nexthop-groups.
		 */
		nexthop_group_add_sorted(ng, nexthop);
	}

	afi = family2afi(AF_INET6);

	/*
	 * If we have an ID, this proto owns the NHG it sent along with the
	 * route, so we just send the ID into rib code with it.
	 *
	 * Havent figured out how to handle backup NHs with this yet, so lets
	 * keep that separate.
	 * Include backup info with the route. We use a temporary nhe here;
	 * if this is a new/unknown nhe, a new copy will be allocated
	 * and stored.
	 */
	if (!re->nhe_id) {
		zebra_nhe_init(&nhe, afi, ng->nexthop);
		nhe.nhg.nexthop = ng->nexthop;
	}

	n = zebra_nhe_copy(&nhe, 0);
	ret = rib_add_multipath_nhe(afi, SAFI_UNICAST, &p, src_p,
				    re, n, false);

	/* At this point, these allocations are not needed: 're' has been
	 * retained or freed, and if 're' still exists, it is using
	 * a reference to a shared group object.
	 */
	nexthop_group_delete(&ng);
    return ret;

}

int zebra_route_del(struct in6_addr *result_sid, struct vrf *vrf, enum seg6local_action_t act, struct seg6local_context *ctx)
{
	afi_t afi;
	struct prefix_ipv6 *src_p = NULL;
	uint32_t table_id;
    struct zebra_vrf *zvrf;
    struct vrf *def_vrf = NULL;
    int ret = 0;
    uint32_t flags = 0;

    struct prefix p = {};

    p.family = AF_INET6;
    p.prefixlen = ctx->block_bits_length + ctx->node_bits_length + ctx->function_bits_length;
    p.u.prefix6 = *result_sid;

    def_vrf = vrf_lookup_by_name(VRF_DEFAULT_NAME);
    zvrf = zebra_vrf_lookup_by_id(def_vrf->vrf_id);
    if (!zvrf) {
        return ret;
    }

	afi = family2afi(AF_INET6);

	table_id = zvrf->table_id;
	SET_FLAG(flags, ZEBRA_FLAG_ALLOW_RECURSION);
	SET_FLAG(flags, ZEBRA_FLAG_LOCAL_SID_ROUTE);

	rib_delete(afi, SAFI_UNICAST, zvrf_id(zvrf), ZEBRA_ROUTE_STATIC, 0,
		   flags, &p, src_p, NULL, 0, table_id, 0,
		   0, false);
    return 0;

}

static int zebra_srv6_manager_get_locator_chunk(struct srv6_locator **loc,
						struct zserv *client,
						const char *locator_name,
						vrf_id_t vrf_id)
{
	int ret = 0;

	*loc = assign_srv6_locator_chunk(client->proto, client->instance,
					 client->session_id, locator_name);

	if (!*loc)
		zlog_err("Unable to assign locator chunk to %s instance %u",
			 zebra_route_string(client->proto), client->instance);
	else if (IS_ZEBRA_DEBUG_PACKET)
		zlog_info("Assigned locator chunk %s to %s instance %u",
			  (*loc)->name, zebra_route_string(client->proto),
			  client->instance);

	if (*loc && (*loc)->status_up)
		ret = zsend_srv6_manager_get_locator_chunk_response(client,
								    vrf_id,
								    *loc);
	return ret;
}

/**
 * Core function, assigns srv6-locator chunks
 *
 * It first searches through the list to check if there's one available
 * (previously released). Otherwise it creates and assigns a new one
 *
 * @param proto Daemon protocol of client, to identify the owner
 * @param instance Instance, to identify the owner
 * @param session_id SessionID of client
 * @param name Name of SRv6-locator
 * @return Pointer to the assigned srv6-locator chunk,
 *         or NULL if the request could not be satisfied
 */
static int zebra_srv6_manager_get_locator_sid(struct srv6_locator **loc,
						struct zserv *client,
						const char *locator_name,
						vrf_id_t vrf_id)
{
	int ret = 0;

    *loc = zebra_srv6_locator_lookup(locator_name);

	if (!*loc)
		zlog_err("Unable to assign locator chunk to %s instance %u",
			 zebra_route_string(client->proto), client->instance);
	else if (IS_ZEBRA_DEBUG_PACKET)
		zlog_info("Assigned locator chunk %s to %s instance %u",
			  (*loc)->name, zebra_route_string(client->proto),
			  client->instance);

	if (*loc && (*loc)->status_up)
		ret = zsend_srv6_manager_get_locator_sid_response(client,
								    vrf_id,
								    *loc,
								    NULL);
	return ret;
}

static int zebra_srv6_manager_get_locator_all(struct zserv *client,
						vrf_id_t vrf_id)
{
	int ret = 0;
    struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *locator;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator))
	{
        if (locator->status_up)
    		ret = zsend_srv6_manager_get_locator_sid_response(client,
    								    vrf_id,
    								    locator,
    								    NULL);
	}

	return ret;
}

/**
 * Core function, release no longer used srv6-locator chunks
 *
 * @param proto Daemon protocol of client, to identify the owner
 * @param instance Instance, to identify the owner
 * @param session_id Zclient session ID, to identify the zclient session
 * @param locator_name SRv6-locator name, to identify the actual locator
 * @return 0 on success, -1 otherwise
 */
static int release_srv6_locator_chunk(uint8_t proto, uint16_t instance,
				      uint32_t session_id,
				      const char *locator_name)
{
	int ret = -1;
	struct listnode *node;
	struct srv6_locator_chunk *chunk;
	struct srv6_locator *loc = NULL;

	loc = zebra_srv6_locator_lookup(locator_name);
	if (!loc)
		return -1;

	if (IS_ZEBRA_DEBUG_PACKET)
		zlog_debug("%s: Releasing srv6-locator on %s", __func__,
			   locator_name);

	for (ALL_LIST_ELEMENTS_RO((struct list *)loc->chunks, node, chunk)) {
		if (chunk->proto != proto ||
		    chunk->instance != instance ||
		    chunk->session_id != session_id)
			continue;
		chunk->proto = NO_PROTO;
		chunk->instance = 0;
		chunk->session_id = 0;
		chunk->keep = 0;
		ret = 0;
		break;
	}

	if (ret != 0)
		flog_err(EC_ZEBRA_SRV6M_UNRELEASED_LOCATOR_CHUNK,
			 "%s: SRv6 locator chunk not released", __func__);

	return ret;
}

static int zebra_srv6_manager_release_locator_chunk(struct zserv *client,
						    const char *locator_name,
						    vrf_id_t vrf_id)
{
	if (vrf_id != VRF_DEFAULT) {
		zlog_err("SRv6 locator doesn't support vrf");
		return -1;
	}

	return release_srv6_locator_chunk(client->proto, client->instance,
					  client->session_id, locator_name);
}

/**
 * Core function, release no longer used srv6-locator chunks
 *
 * @param proto Daemon protocol of client, to identify the owner
 * @param instance Instance, to identify the owner
 * @param session_id Zclient session ID, to identify the zclient session
 * @param locator_name SRv6-locator name, to identify the actual locator
 * @return 0 on success, -1 otherwise
 */
static int zebra_srv6_manager_release_locator_sid(struct zserv *client,
						    const char *locator_name,
						    vrf_id_t vrf_id)
{
	if (vrf_id != VRF_DEFAULT) {
		zlog_err("SRv6 locator doesn't support vrf");
		return -1;
	}

	return release_srv6_locator_chunk(client->proto, client->instance,
					  client->session_id, locator_name);
}

/**
 * Release srv6-locator chunks from a client.
 *
 * Called on client disconnection or reconnection. It only releases chunks
 * with empty keep value.
 *
 * @param proto Daemon protocol of client, to identify the owner
 * @param instance Instance, to identify the owner
 * @return Number of chunks released
 */
int release_daemon_srv6_locator_chunks(struct zserv *client)
{
	int ret;
	int count = 0;
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct listnode *loc_node;
	struct listnode *chunk_node;
	struct srv6_locator *loc;
	struct srv6_locator_chunk *chunk;

	if (IS_ZEBRA_DEBUG_PACKET)
		zlog_debug("%s: Releasing chunks for client proto %s, instance %d, session %u",
			   __func__, zebra_route_string(client->proto),
			   client->instance, client->session_id);

	for (ALL_LIST_ELEMENTS_RO(srv6->locators, loc_node, loc)) {
		for (ALL_LIST_ELEMENTS_RO(loc->chunks, chunk_node, chunk)) {
			if (chunk->proto == client->proto &&
			    chunk->instance == client->instance &&
			    chunk->session_id == client->session_id &&
			    chunk->keep == 0) {
				ret = release_srv6_locator_chunk(
						chunk->proto, chunk->instance,
						chunk->session_id, loc->name);
				if (ret == 0)
					count++;
			}
		}
	}

	if (IS_ZEBRA_DEBUG_PACKET)
		zlog_debug("%s: Released %d srv6-locator chunks",
			   __func__, count);

	return count;
}

void zebra_srv6_init(void)
{
	hook_register(zserv_client_close, zebra_srv6_cleanup);
	hook_register(srv6_manager_get_chunk,
		      zebra_srv6_manager_get_locator_chunk);
	hook_register(srv6_manager_release_chunk,
		      zebra_srv6_manager_release_locator_chunk);
	hook_register(srv6_manager_get_sid,
		      zebra_srv6_manager_get_locator_sid);
    hook_register(srv6_manager_get_locator_sid_all,
		      zebra_srv6_manager_get_locator_all);
}

bool zebra_srv6_is_enable(void)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();

	return listcount(srv6->locators);
}

int zebra_srv6_vrf_enable(struct zebra_vrf *zvrf)
{
    struct zebra_srv6 *srv6 = zebra_srv6_get_default();
    struct listnode *node, *opcodenode;
    struct srv6_locator *locator;
    struct seg6_sid *sid;
    struct vrf *vrf;

    for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator)) {
        for (ALL_LIST_ELEMENTS_RO(locator->sids, opcodenode, sid)) {
            if (sid->vrfName == NULL)
                continue;
            vrf = vrf_lookup_by_name(sid->vrfName);
            if (zvrf->vrf != vrf)
                continue;
            if (CHECK_FLAG(vrf->status, VRF_ACTIVE))
            {
                zebra_srv6_local_sid_add(locator, sid);
            }
        }
    }
    return 0;
}