/*
 * Copyright (C) 2020  NetDEF, Inc.
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

#include "thread.h"
#include "log.h"
#include "lib_errors.h"
#include "if.h"
#include "prefix.h"
#include "zclient.h"
#include "network.h"
#include "stream.h"
#include "linklist.h"
#include "nexthop.h"
#include "vrf.h"
#include "typesafe.h"

#include "pathd/pathd.h"
#include "pathd/path_ted.h"
#include "pathd/path_zebra.h"
#include "lib/command.h"
#include "lib/link_state.h"

static int path_zebra_opaque_msg_handler(ZAPI_CALLBACK_ARGS);

struct zclient *zclient;
static struct zclient *zclient_sync;

/* Global Variables */
bool g_has_router_id_v4 = false;
bool g_has_router_id_v6 = false;
struct in_addr g_router_id_v4;
struct in6_addr g_router_id_v6;
pthread_mutex_t g_router_id_v4_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_router_id_v6_mtx = PTHREAD_MUTEX_INITIALIZER;

/**
 * Gives the IPv4 router ID received from Zebra.
 *
 * @param router_id The in_addr strucure where to store the router id
 * @return true if the router ID was available, false otherwise
 */
bool get_ipv4_router_id(struct in_addr *router_id)
{
	bool retval = false;
	assert(router_id != NULL);
	pthread_mutex_lock(&g_router_id_v4_mtx);
	if (g_has_router_id_v4) {
		memcpy(router_id, &g_router_id_v4, sizeof(*router_id));
		retval = true;
	}
	pthread_mutex_unlock(&g_router_id_v4_mtx);
	return retval;
}

/**
 * Gives the IPv6 router ID received from Zebra.
 *
 * @param router_id The in6_addr strucure where to store the router id
 * @return true if the router ID was available, false otherwise
 */
bool get_ipv6_router_id(struct in6_addr *router_id)
{
	bool retval = false;
	assert(router_id != NULL);
	pthread_mutex_lock(&g_router_id_v6_mtx);
	if (g_has_router_id_v6) {
		memcpy(router_id, &g_router_id_v6, sizeof(*router_id));
		retval = true;
	}
	pthread_mutex_unlock(&g_router_id_v6_mtx);
	return retval;
}

static void path_zebra_connected(struct zclient *zclient)
{
	struct srte_policy *policy;

	zclient_send_reg_requests(zclient, VRF_DEFAULT);
	zclient_send_router_id_update(zclient, ZEBRA_ROUTER_ID_ADD, AFI_IP6,
				      VRF_DEFAULT);

	RB_FOREACH (policy, srte_policy_head, &srte_policies) {
		struct srte_candidate *candidate;
		struct srte_segment_list *segment_list;

		candidate = policy->best_candidate;
		if (!candidate)
			continue;

		segment_list = candidate->lsp->segment_list;
		if (!segment_list)
			continue;

		path_zebra_add_sr_policy(policy, segment_list);
	}
}

static int path_zebra_sr_policy_notify_status(ZAPI_CALLBACK_ARGS)
{
	struct zapi_sr_policy zapi_sr_policy;
	struct srte_policy *policy;
	struct srte_candidate *best_candidate_path;

	if (zapi_sr_policy_notify_status_decode(zclient->ibuf, &zapi_sr_policy))
		return -1;

	policy = srte_policy_find(zapi_sr_policy.color,
				  &zapi_sr_policy.endpoint);
	if (!policy)
		return -1;

	best_candidate_path = policy->best_candidate;
	if (!best_candidate_path)
		return -1;

	srte_candidate_status_update(best_candidate_path,
				     zapi_sr_policy.status);

	return 0;
}

/* Router-id update message from zebra. */
static int path_zebra_router_id_update(ZAPI_CALLBACK_ARGS)
{
	struct prefix pref;
	const char *family;
	char buf[PREFIX2STR_BUFFER];
	zebra_router_id_update_read(zclient->ibuf, &pref);
	if (pref.family == AF_INET) {
		pthread_mutex_lock(&g_router_id_v4_mtx);
		memcpy(&g_router_id_v4, &pref.u.prefix4,
		       sizeof(g_router_id_v4));
		g_has_router_id_v4 = true;
		inet_ntop(AF_INET, &g_router_id_v4, buf, sizeof(buf));
		pthread_mutex_unlock(&g_router_id_v4_mtx);
		family = "IPv4";
	} else if (pref.family == AF_INET6) {
		pthread_mutex_lock(&g_router_id_v6_mtx);
		memcpy(&g_router_id_v6, &pref.u.prefix6,
		       sizeof(g_router_id_v6));
		g_has_router_id_v6 = true;
		inet_ntop(AF_INET6, &g_router_id_v6, buf, sizeof(buf));
		pthread_mutex_unlock(&g_router_id_v6_mtx);
		family = "IPv6";
	} else {
		zlog_warn("Unexpected router ID address family for vrf %u: %u",
			  vrf_id, pref.family);
		return 0;
	}
	zlog_info("%s Router Id updated for VRF %u: %s", family, vrf_id, buf);
	return 0;
}

/**
 * Adds a segment routing policy to Zebra.
 *
 * @param policy The policy to add
 * @param segment_list The segment list for the policy
 */
void path_zebra_add_sr_policy(struct srte_policy *policy,
			      struct srte_segment_list *segment_list)
{
	struct zapi_sr_policy zp = {};
	struct srte_segment_entry *segment;

	zp.color = policy->color;
	zp.endpoint = policy->endpoint;
	strlcpy(zp.name, policy->name, sizeof(zp.name));
	zp.segment_list.type = ZEBRA_LSP_SRTE;
	zp.segment_list.local_label = policy->binding_sid;
	zp.segment_list.label_num = 0;
	RB_FOREACH (segment, srte_segment_entry_head, &segment_list->segments)
		zp.segment_list.labels[zp.segment_list.label_num++] =
			segment->sid_value;
	policy->status = SRTE_POLICY_STATUS_GOING_UP;

	(void)zebra_send_sr_policy(zclient, ZEBRA_SR_POLICY_SET, &zp);
}

/**
 * Deletes a segment policy from Zebra.
 *
 * @param policy The policy to remove
 */
void path_zebra_delete_sr_policy(struct srte_policy *policy)
{
	struct zapi_sr_policy zp = {};

	zp.color = policy->color;
	zp.endpoint = policy->endpoint;
	strlcpy(zp.name, policy->name, sizeof(zp.name));
	zp.segment_list.type = ZEBRA_LSP_SRTE;
	zp.segment_list.local_label = policy->binding_sid;
	zp.segment_list.label_num = 0;
	policy->status = SRTE_POLICY_STATUS_DOWN;

	(void)zebra_send_sr_policy(zclient, ZEBRA_SR_POLICY_DELETE, &zp);
}

/**
 * Adds a segment routing policy to Zebra.
 *
 * @param policy The policy to add
 * @param segment_list The segment list for the policy
 */
void path_zebra_add_srv6_policy(struct srte_policy *policy,
			      struct srte_candidate_group *candidate_group)
{
	struct zapi_sr_policy zp = {};
	struct srte_candidate *candidate;
	uint32_t count = 0;
	struct srte_segment_entry *s_entry;
	zp.color = policy->color;
	zp.endpoint = policy->endpoint;
	strlcpy(zp.name, policy->name, sizeof(zp.name));
	zp.tunnel_type = SRTE_TUNNEL_TYPE_SRV6;
	char endpoint[46];
	prefix2str(&policy->endpoint, endpoint, sizeof(endpoint));
	RB_FOREACH (candidate, srte_candidate_pref_head, &candidate_group->candidate_paths) {
		if (candidate->segment_list == NULL ) 
		{
			continue;
		}
		if (CHECK_FLAG(policy->flags, F_POLICY_CONF_BFD) 
		    && candidate->status == SRTE_DETECT_DOWN)
		{
            continue;
		}
		if (candidate->bfd_name[0] && candidate->status == SRTE_DETECT_DOWN)
		{
			continue;
		}
		if (CHECK_FLAG(candidate->flags, F_CANDIDATE_DELETED))
		{
			continue;
		}
		if (count < candidate_group->up_cpath_num)
		{
			zlog_info("collect UP cpath, color:%u, endpoint:%s, group:%u, cpath:%s, sidlist:%s, bfd_name:%s",
				zp.color, endpoint, candidate_group->preference, candidate->name, candidate->segment_list->name, candidate->bfd_name);
			strlcpy(zp.srv6_tunnel.sidlists[count].sidlist_name, candidate->segment_list->name,
				sizeof(zp.srv6_tunnel.sidlists[count].sidlist_name));
			zp.srv6_tunnel.sidlists[count].weight = candidate->weight;
			count++;
		}
	}
    zp.srv6_tunnel.path_num = count;
	prefix2str(&policy->endpoint, endpoint, sizeof(endpoint));
	zlog_info("notify policy set to zebra, color:%u, endpoint:%s, name:%s, tunnel_type:%u, path_num:%u.",
		zp.color, endpoint, zp.name[0]?zp.name : "-",
		zp.tunnel_type, zp.srv6_tunnel.path_num);
	(void)zebra_send_sr_policy(zclient, ZEBRA_SRV6_POLICY_SET, &zp);
}
void path_zebra_delete_srv6_policy(struct srte_policy *policy)
{
	struct zapi_sr_policy zp = {};
	zp.color = policy->color;
	zp.endpoint = policy->endpoint;
	strlcpy(zp.name, policy->name, sizeof(zp.name));
	zp.tunnel_type = SRTE_TUNNEL_TYPE_SRV6;
	zp.srv6_tunnel.path_num = 0;
    char endpoint[60];
	prefix2str(&policy->endpoint, endpoint, sizeof(endpoint));
	zlog_info("notify policy del to zebra, color:%u, endpoint:%s, name:%s, tunnel_type:%u, path_num:%u.",
		zp.color, endpoint, zp.name[0]?zp.name : "-",
		zp.tunnel_type, zp.srv6_tunnel.path_num);
	(void)zebra_send_sr_policy(zclient, ZEBRA_SRV6_POLICY_DELETE, &zp);
}
void path_zebra_add_srv6_sidlist(struct srte_segment_list *segment_list)
{
	if (segment_list == NULL) {
		return;
	}
	struct srte_segment_entry *s_entry;
	struct zapi_srv6_sidlist sidlist = {};
	strlcpy(sidlist.sidlist_name, segment_list->name, sizeof(sidlist.sidlist_name));

	uint32_t segment_count = 0;
	RB_FOREACH (s_entry, srte_segment_entry_head, &segment_list->segments) {
		sidlist.segments[segment_count].index = s_entry->index;
		sidlist.segments[segment_count].sid_type = s_entry->sid_type;
		memcpy(&sidlist.segments[segment_count].srv6_sid_value, &s_entry->srv6_sid_value, sizeof(struct ipaddr));
		segment_count++;
	}
	sidlist.segment_count = segment_count;

	(void)zebra_send_srv6_sidlist(zclient, ZEBRA_SRV6_SIDLIST_SET, &sidlist);
}

/**
 * Deletes a segment list to Zebra.
 *
 * @param policy The segment list to delete
 */
void path_zebra_delete_srv6_sidlist(struct srte_segment_list *segment_list)
{
	struct srte_segment_entry *s_entry;
	struct zapi_srv6_sidlist sidlist = {};
	strlcpy(sidlist.sidlist_name, segment_list->name, sizeof(sidlist.sidlist_name));

	uint32_t segment_count = 0;
	RB_FOREACH (s_entry, srte_segment_entry_head, &segment_list->segments) {
		sidlist.segments[segment_count].index = s_entry->index;
		sidlist.segments[segment_count].sid_type = s_entry->sid_type;
		memcpy(&sidlist.segments[segment_count].srv6_sid_value, &s_entry->srv6_sid_value, sizeof(struct ipaddr));
		segment_count++;
	}
	sidlist.segment_count = segment_count;

	(void)zebra_send_srv6_sidlist(zclient, ZEBRA_SRV6_SIDLIST_DELETE, &sidlist);
}


/**
 * Allocates a label from Zebra's label manager.
 *
 * @param label the label to be allocated
 * @return 0 if the label has been allocated, -1 otherwise
 */
int path_zebra_request_label(mpls_label_t label)
{
	int ret;
	uint32_t start, end;

	ret = lm_get_label_chunk(zclient_sync, 0, label, 1, &start, &end);
	if (ret < 0) {
		zlog_warn("%s: error getting label range!", __func__);
		return -1;
	}

	return 0;
}

/**
 * Releases a previously allocated label from Zebra's label manager.
 *
 * @param label The label to release
 * @return 0 ifthe label has beel released, -1 otherwise
 */
void path_zebra_release_label(mpls_label_t label)
{
	int ret;

	ret = lm_release_label_chunk(zclient_sync, label, label);
	if (ret < 0)
		zlog_warn("%s: error releasing label range!", __func__);
}

static void path_zebra_label_manager_connect(void)
{
	/* Connect to label manager. */
	while (zclient_socket_connect(zclient_sync) < 0) {
		zlog_warn("%s: error connecting synchronous zclient!",
			  __func__);
		sleep(1);
	}
	set_nonblocking(zclient_sync->sock);

	/* Send hello to notify zebra this is a synchronous client */
	while (zclient_send_hello(zclient_sync) < 0) {
		zlog_warn("%s: Error sending hello for synchronous zclient!",
			  __func__);
		sleep(1);
	}

	while (lm_label_manager_connect(zclient_sync, 0) != 0) {
		zlog_warn("%s: error connecting to label manager!", __func__);
		sleep(1);
	}
}

static int path_zebra_opaque_msg_handler(ZAPI_CALLBACK_ARGS)
{
	int ret = 0;
	struct stream *s;
	struct zapi_opaque_msg info;

	s = zclient->ibuf;

	if (zclient_opaque_decode(s, &info) != 0)
		return -1;

	switch (info.type) {
	case LINK_STATE_UPDATE:
	case LINK_STATE_SYNC:
		/* Start receiving ls data so cancel request sync timer */
		path_ted_timer_sync_cancel();

		struct ls_message *msg = ls_parse_msg(s);

		if (msg) {
			zlog_debug("%s: [rcv ted] ls (%s) msg (%s)-(%s) !",
				   __func__,
				   info.type == LINK_STATE_UPDATE
					   ? "LINK_STATE_UPDATE"
					   : "LINK_STATE_SYNC",
				   LS_MSG_TYPE_PRINT(msg->type),
				   LS_MSG_EVENT_PRINT(msg->event));
		} else {
			zlog_err(
				"%s: [rcv ted] Could not parse LinkState stream message.",
				__func__);
			return -1;
		}

		ret = path_ted_rcvd_message(msg);
		ls_delete_msg(msg);
		/* Update local configuration after process update. */
		path_ted_segment_list_refresh();
		break;
	default:
		zlog_debug("%s: [rcv ted] unknown opaque event (%d) !",
			   __func__, info.type);
		break;
	}

	return ret;
}

static zclient_handler *const path_handlers[] = {
	[ZEBRA_SR_POLICY_NOTIFY_STATUS] = path_zebra_sr_policy_notify_status,
	[ZEBRA_ROUTER_ID_UPDATE] = path_zebra_router_id_update,
	[ZEBRA_OPAQUE_MESSAGE] = path_zebra_opaque_msg_handler,
};

/**
 * Initializes Zebra asynchronous connection.
 *
 * @param master The master thread
 */
void path_zebra_init(struct thread_master *master)
{
	struct zclient_options options = zclient_options_default;
	options.synchronous = true;

	/* Initialize asynchronous zclient. */
	zclient = zclient_new(master, &zclient_options_default, path_handlers,
			      array_size(path_handlers));
	zclient_init(zclient, ZEBRA_ROUTE_SRTE, 0, &pathd_privs);
	zclient->zebra_connected = path_zebra_connected;

	/* Initialize special zclient for synchronous message exchanges. */
	zclient_sync = zclient_new(master, &options, NULL, 0);
	zclient_sync->sock = -1;
	zclient_sync->redist_default = ZEBRA_ROUTE_SRTE;
	zclient_sync->instance = 1;
	zclient_sync->privs = &pathd_privs;

	/* Connect to the LM. */
	path_zebra_label_manager_connect();
}

void path_zebra_stop(void)
{
	zclient_stop(zclient);
	zclient_free(zclient);
}
