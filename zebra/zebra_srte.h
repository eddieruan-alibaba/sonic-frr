/* Zebra's client header.
 * Copyright (C) 2020 Netdef, Inc.
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
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

#ifndef _ZEBRA_SRTE_H
#define _ZEBRA_SRTE_H

#include "zebra/zebra_mpls.h"

#include "lib/zclient.h"
#include "lib/srte.h"
#include "lib/table.h"

#ifdef __cplusplus
extern "C" {
#endif

enum zebra_sr_policy_update_label_mode {
	ZEBRA_SR_POLICY_LABEL_CREATED = 1,
	ZEBRA_SR_POLICY_LABEL_UPDATED = 2,
	ZEBRA_SR_POLICY_LABEL_REMOVED = 3,
};

#define ZEBRA_SR_POLICY_TYPE_LSP      1
#define ZEBRA_SR_POLICY_TYPE_SRV6     2
#define SRV6_SID_LIST_TABLE "SRV6_SID_LIST_TABLE"
DEFINE_MTYPE_STATIC(ZEBRA, ZEBRA_SRV6_SIDLIST, "Zebra Srv6 SidList");
struct zebra_sr_policy {
	RB_ENTRY(zebra_sr_policy) entry;
	uint32_t color;
	struct ipaddr endpoint;
	uint8_t type;
	char name[SRTE_POLICY_NAME_MAX_LENGTH];
	enum zebra_sr_policy_status status;
	struct zapi_srte_tunnel segment_list;
	struct zapi_srv6te_tunnel srv6_segment_list;
	struct zebra_lsp *lsp;
	struct zebra_vrf *zvrf;
	int sock;
	struct route_node *node;
	struct rnh_list_head nht;
};
#if 0
RB_HEAD(zebra_sr_policy_instance_head, zebra_sr_policy);
RB_PROTOTYPE(zebra_sr_policy_instance_head, zebra_sr_policy, entry,
	     zebra_sr_policy_instance_compare)

extern struct zebra_sr_policy_instance_head zebra_sr_policy_instances;
#endif

struct srte_table_key {
	afi_t afi;
	uint32_t color;
	struct route_table *table;
};
struct zebra_sr_policy_label_para {
	mpls_label_t label;
	enum zebra_sr_policy_update_label_mode mode;
};
extern struct hash *srte_table_hash;
struct zebra_sr_policy *
zebra_sr_policy_add(uint32_t color, struct ipaddr *endpoint, char *name);
void zebra_sr_policy_del(struct zebra_sr_policy *policy);
struct zebra_sr_policy *zebra_sr_policy_find(uint32_t color,
					     struct ipaddr *endpoint);
struct zebra_sr_policy *zebra_sr_policy_find_by_name(char *name);
struct zebra_sr_policy *zebra_sr_policy_find_by_rnh(struct rnh *rnh);
int zebra_sr_policy_validate(struct zebra_sr_policy *policy,
			     struct zapi_srte_tunnel *new_tunnel);
int zebra_sr_policy_bsid_install(struct zebra_sr_policy *policy);
void zebra_sr_policy_bsid_uninstall(struct zebra_sr_policy *policy,
				    mpls_label_t old_bsid);
int zebra_srv6_sidlist_install(struct zapi_srv6_sidlist *sidlist);
void zebra_srv6_sidlist_uninstall(struct zapi_srv6_sidlist *sidlist);
void zebra_srte_init(void);
int zebra_sr_policy_label_update(mpls_label_t label,
				 enum zebra_sr_policy_update_label_mode mode);

extern int zebra_sr_policy_notify_update_client(struct rnh *rnh, struct zebra_sr_policy *policy,
                            struct zserv *client);
extern void zebra_sr_policy_notify_update(struct rnh *rnh, struct zebra_sr_policy *policy, struct zserv *zclient);
extern int zebra_sr_policy_notify_unknown(struct rnh *rnh, struct zserv *client);
extern void zebra_srv6_policy_validate(struct zebra_sr_policy *policy,
                     struct zapi_srv6te_tunnel *new_tunnel, bool new);
extern struct zebra_sr_policy *zebra_sr_policy_add_by_prefix(struct prefix *p, uint32_t color, char *name);
extern struct zebra_sr_policy *zebra_sr_policy_lookup_by_prefix(struct prefix *p, uint32_t color);
extern struct zebra_sr_policy *zebra_sr_policy_match_by_prefix(struct prefix *p, uint32_t color, struct route_node **prn);
extern void zebra_sr_policy_delete_by_prefix(struct zebra_sr_policy *policy);
extern void *srte_table_alloc(void *arg);
extern void zebra_srte_evaluate_rn_nexthops(struct zebra_sr_policy *policy, bool rt_delete);
extern int zebra_sr_policy_label_update_walk(struct hash_bucket *hb, void *arg);
extern void zebra_free_sr_table(struct route_table *table);
extern struct route_table *zebra_srte_table_create(afi_t afi, uint32_t color);
#ifdef __cplusplus
}
#endif

#endif /* _ZEBRA_SRTE_H */
