/*
 * Zebra SRv6 definitions
 * Copyright (C) 2020  Hiroki Shirokura, LINE Corporation
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

#ifndef _ZEBRA_SRV6_H
#define _ZEBRA_SRV6_H

#include <zebra.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "qobj.h"
#include "prefix.h"
#include <pthread.h>
#include <plist.h>

/* SRv6 instance structure. */
struct zebra_srv6 {
	struct list *locators;
};

#define SRV6_SID_LIST_TABLE "SRV6_SID_LIST_TABLE"
// DEFINE_MTYPE_STATIC(ZEBRA, ZEBRA_SRV6_SIDLIST, "Zebra Srv6 SidList");

#define SRV6_SEGMENTLIST_NAME_MAX_LENGTH 64
#define SRV6_SID_INDEX_MAX_NUM 8

struct zebra_srv6_segment_entry {
	uint32_t index_;
	struct ipaddr srv6_sid_value_;
};

struct zebra_srv6_sidlist {
	char sidlist_name_[SRV6_SEGMENTLIST_NAME_MAX_LENGTH];
	uint32_t segment_count_;
	struct zebra_srv6_segment_entry segments_[SRV6_SID_INDEX_MAX_NUM];
};

/* declare hooks for the basic API, so that it can be specialized or served
 * externally. Also declare a hook when those functions have been registered,
 * so that any external module wanting to replace those can react
 */

DECLARE_HOOK(srv6_manager_client_connect,
	    (struct zserv *client, vrf_id_t vrf_id),
	    (client, vrf_id));
DECLARE_HOOK(srv6_manager_client_disconnect,
	     (struct zserv *client), (client));
DECLARE_HOOK(srv6_manager_get_chunk,
	     (struct srv6_locator **loc,
	      struct zserv *client,
	      const char *locator_name,
	      vrf_id_t vrf_id),
	     (mc, client, keep, size, base, vrf_id));
DECLARE_HOOK(srv6_manager_release_chunk,
	     (struct zserv *client,
	      const char *locator_name,
	      vrf_id_t vrf_id),
	     (client, locator_name, vrf_id));

DECLARE_HOOK(srv6_manager_get_sid,
	     (struct srv6_locator **loc,
	      struct zserv *client,
	      const char *locator_name,
	      vrf_id_t vrf_id),
	     (loc, client, locator_name, vrf_id));
DECLARE_HOOK(srv6_manager_release_sid,
	     (struct zserv *client,
	      const char *locator_name,
	      vrf_id_t vrf_id),
	     (client, locator_name, vrf_id));
DECLARE_HOOK(srv6_manager_get_locator_sid_all,
	     (struct zserv *client,
	      vrf_id_t vrf_id),
	     (client, vrf_id));

int srv6_sidlist_install(struct zebra_srv6_sidlist *sid_list);
void srv6_sidlist_uninstall(struct zebra_srv6_sidlist *sid_list);

extern void zebra_srv6_locator_add(struct srv6_locator *locator);
extern void zebra_srv6_locator_delete(struct srv6_locator *locator);
extern void zebra_srv6_prefix_delete(struct srv6_locator *locator);
extern struct srv6_locator *zebra_srv6_locator_lookup(const char *name);

extern int zebra_route_add(struct in6_addr *result_sid, struct vrf *vrf, enum seg6local_action_t act, struct seg6local_context *ctx);
extern int zebra_route_del(struct in6_addr *result_sid, struct vrf *vrf, enum seg6local_action_t act, struct seg6local_context *ctx);

void zebra_notify_srv6_locator_add(struct srv6_locator *locator);
void zebra_notify_srv6_locator_delete(struct srv6_locator *locator);
extern void zebra_srv6_local_sid_add(struct srv6_locator *locator, struct seg6_sid *sid);
extern void zebra_srv6_local_sid_del(struct srv6_locator *locator, struct seg6_sid *sid);

extern void zebra_srv6_init(void);
extern struct zebra_srv6 *zebra_srv6_get_default(void);
extern bool zebra_srv6_is_enable(void);

extern void srv6_manager_client_connect_call(struct zserv *client,
					     vrf_id_t vrf_id);
extern void srv6_manager_get_locator_chunk_call(struct srv6_locator **loc,
						struct zserv *client,
						const char *locator_name,
						vrf_id_t vrf_id);
extern void srv6_manager_release_locator_chunk_call(struct zserv *client,
						    const char *locator_name,
						    vrf_id_t vrf_id);

extern void srv6_manager_release_locator_sid_call(struct zserv *client,
						    const char *locator_name,
						    vrf_id_t vrf_id);
extern void srv6_manager_get_locator_sid_call(struct srv6_locator **loc,
						struct zserv *client,
						const char *locator_name,
						vrf_id_t vrf_id);
extern void srv6_manager_get_locator_all_call(struct zserv *client,
					 vrf_id_t vrf_id);

extern int srv6_manager_client_disconnect_cb(struct zserv *client);
extern bool zebra_srv6_local_sid_format_valid(struct srv6_locator *locator, struct seg6_sid *sid);
extern int release_daemon_srv6_locator_chunks(struct zserv *client);
extern bool zebra_srv6_local_sid_get_format(struct srv6_locator *locator);
extern int zebra_srv6_vrf_enable(struct zebra_vrf *zvrf);

#endif /* _ZEBRA_SRV6_H */
