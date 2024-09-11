/*
 * Zebra SRv6 VTY functions
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

#include <zebra.h>

#include "memory.h"
#include "if.h"
#include "prefix.h"
#include "command.h"
#include "table.h"
#include "rib.h"
#include "nexthop.h"
#include "vrf.h"
#include "srv6.h"
#include "lib/json.h"

#include "zebra/zserv.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_srv6.h"
#include "zebra/zebra_srv6_vty.h"
#include "zebra/zebra_rnh.h"
#include "zebra/redistribute.h"
#include "zebra/zebra_routemap.h"
#include "zebra/zebra_dplane.h"
#include "zebra/zapi_msg.h"

#include "zebra/zebra_srv6_vty_clippy.c"

static int zebra_sr_config(struct vty *vty);

static struct cmd_node sr_node = {
	.name = "sr",
	.node = SEGMENT_ROUTING_NODE,
	.parent_node = CONFIG_NODE,
	.prompt = "%s(config-sr)# ",
	.config_write = zebra_sr_config,
};

static struct cmd_node srv6_node = {
	.name = "srv6",
	.node = SRV6_NODE,
	.parent_node = SEGMENT_ROUTING_NODE,
	.prompt = "%s(config-srv6)# ",

};

static struct cmd_node srv6_locs_node = {
	.name = "srv6-locators",
	.node = SRV6_LOCS_NODE,
	.parent_node = SRV6_NODE,
	.prompt = "%s(config-srv6-locators)# ",
};

static struct cmd_node srv6_loc_node = {
	.name = "srv6-locator",
	.node = SRV6_LOC_NODE,
	.parent_node = SRV6_LOCS_NODE,
	.prompt = "%s(config-srv6-locator)# "
};

static bool seg6local_act_contain_sidact(enum seg6local_action_t action,
					 enum seg6local_action_t sidaction)
{
	switch (action) {
	case ZEBRA_SEG6_LOCAL_ACTION_END:
	case ZEBRA_SEG6_LOCAL_ACTION_END_T:
	case ZEBRA_SEG6_LOCAL_ACTION_END_DX2:
	case ZEBRA_SEG6_LOCAL_ACTION_END_DX6:
	case ZEBRA_SEG6_LOCAL_ACTION_END_DX4:
	case ZEBRA_SEG6_LOCAL_ACTION_END_B6:
	case ZEBRA_SEG6_LOCAL_ACTION_END_B6_ENCAP:
	case ZEBRA_SEG6_LOCAL_ACTION_END_BM:
	case ZEBRA_SEG6_LOCAL_ACTION_END_S:
	case ZEBRA_SEG6_LOCAL_ACTION_END_AS:
	case ZEBRA_SEG6_LOCAL_ACTION_END_AM:
		if (action == sidaction)
			return true;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT46:
		if (sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT4 ||
		    sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT6 ||
		    sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT46)
			return true;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT4:
		if (sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT4 ||
		    sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT46)
			return true;
		break;
	case ZEBRA_SEG6_LOCAL_ACTION_END_DT6:
		if (sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT46 ||
		    sidaction == ZEBRA_SEG6_LOCAL_ACTION_END_DT6)
			return true;
		break;
	default:
		return false;
	}
	return false;
}

static struct seg6_sid *
sid_lookup_by_vrf_action(struct srv6_locator *loc, const char *vrfname,
			 enum seg6local_action_t sidaction)
{
	struct seg6_sid *sid = NULL;
	struct listnode *node, *nnode;

	if (!vrfname)
		return NULL;

	for (ALL_LIST_ELEMENTS(loc->sids, node, nnode, sid)) {
		if (strcmp(sid->vrfName, vrfname) == 0 &&
		    seg6local_act_contain_sidact(sid->sidaction, sidaction))
			return sid;
	}
	return NULL;
}

DEFUN (show_srv6_locator,
       show_srv6_locator_cmd,
       "show segment-routing srv6 locator [json]",
       SHOW_STR
       "Segment Routing\n"
       "Segment Routing SRv6\n"
       "Locator Information\n"
       JSON_STR)
{
	const bool uj = use_json(argc, argv);
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *locator;
	struct listnode *node;
	char str[256];
	int id;
	json_object *json = NULL;
	json_object *json_locators = NULL;
	json_object *json_locator = NULL;

	if (uj) {
		json = json_object_new_object();
		json_locators = json_object_new_array();
		json_object_object_add(json, "locators", json_locators);

		for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator)) {
			json_locator = srv6_locator_json(locator);
			if (!json_locator)
				continue;
			json_object_array_add(json_locators, json_locator);

		}

		vty_json(vty, json);
	} else {
		vty_out(vty, "Locator:\n");
		vty_out(vty, "Name                 ID      Prefix                   Status\n");
		vty_out(vty, "-------------------- ------- ------------------------ -------\n");

		id = 1;
		for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator)) {
			prefix2str(&locator->prefix, str, sizeof(str));
			vty_out(vty, "%-20s %7d %-24s %s\n",
				locator->name, id, str,
				locator->status_up ? "Up" : "Down");
			++id;
		}
		vty_out(vty, "\n");
	}

	return CMD_SUCCESS;
}

DEFUN (show_srv6_locator_detail,
       show_srv6_locator_detail_cmd,
       "show segment-routing srv6 locator NAME detail [json]",
       SHOW_STR
       "Segment Routing\n"
       "Segment Routing SRv6\n"
       "Locator Information\n"
       "Locator Name\n"
       "Detailed information\n"
       JSON_STR)
{
	const bool uj = use_json(argc, argv);
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *locator;
	struct listnode *node;
	struct listnode *sidnode;
	struct seg6_sid *sid = NULL;
	char str[256];
	char buf[256];
	const char *locator_name = argv[4]->arg;
	json_object *json_locator = NULL;

	if (uj) {
		locator = zebra_srv6_locator_lookup(locator_name);
		if (!locator)
			return CMD_WARNING;

		json_locator = srv6_locator_detailed_json(locator);
		vty_json(vty, json_locator);
		return CMD_SUCCESS;
	}

	for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator)) {
		struct listnode *node;
		struct srv6_locator_chunk *chunk;

		if (strcmp(locator->name, locator_name) != 0)
			continue;

		prefix2str(&locator->prefix, str, sizeof(str));
		vty_out(vty, "Name: %s\n", locator->name);
		vty_out(vty, "Prefix: %s\n", str);
		vty_out(vty, "Block-Bit-Len: %u\n", locator->block_bits_length);
		vty_out(vty, "Node-Bit-Len: %u\n", locator->node_bits_length);
		vty_out(vty, "Function-Bit-Len: %u\n",
			locator->function_bits_length);
		vty_out(vty, "Argument-Bit-Len: %u\n",
			locator->argument_bits_length);

		if (CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID))
			vty_out(vty, "Behavior: uSID\n");

		vty_out(vty, "Chunks:\n");
		for (ALL_LIST_ELEMENTS_RO((struct list *)locator->chunks, node,
					  chunk)) {
			prefix2str(&chunk->prefix, str, sizeof(str));
			vty_out(vty, "- prefix: %s, owner: %s\n", str,
				zebra_route_string(chunk->proto));
		}
		vty_out(vty, "  sids:\n");
		for (ALL_LIST_ELEMENTS_RO(locator->sids, sidnode, sid)) {
			prefix2str(&sid->ipv6Addr, buf, sizeof(buf));
			vty_out(vty, "   -opcode %s\n", buf);
			vty_out(vty, "    sidaction %s\n",
				seg6local_action2str(sid->sidaction));
			vty_out(vty, "    vrf %s\n", sid->vrfName);
		}
	}


	return CMD_SUCCESS;
}

DEFUN_NOSH (segment_routing,
            segment_routing_cmd,
            "segment-routing",
            "Segment Routing\n")
{
	vty->node = SEGMENT_ROUTING_NODE;
	return CMD_SUCCESS;
}

DEFUN_NOSH (srv6,
            srv6_cmd,
            "srv6",
            "Segment Routing SRv6\n")
{
	vty->node = SRV6_NODE;
	return CMD_SUCCESS;
}

DEFUN (no_srv6,
       no_srv6_cmd,
       "no srv6",
       NO_STR
       "Segment Routing SRv6\n")
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct srv6_locator *locator;
	struct listnode *node, *nnode;

	for (ALL_LIST_ELEMENTS(srv6->locators, node, nnode, locator))
		zebra_srv6_locator_delete(locator);
	return CMD_SUCCESS;
}

DEFUN_NOSH (srv6_locators,
            srv6_locators_cmd,
            "locators",
            "Segment Routing SRv6 locators\n")
{
	vty->node = SRV6_LOCS_NODE;
	return CMD_SUCCESS;
}

DEFUN_NOSH(srv6_locator_sid, srv6_locator_cmd,
	   "locator WORD [ prefix X:X::X:X/M$prefix \
         [block-len (16-64)$block_bit_len] [node-len (16-64)$node_bit_len] [func-bits (16-80)$func_bit_len] [argu-bits (16-80)$argu_bit_len] ]",
	   "Segment Routing SRv6 locator\n"
	   "Specify locator-name\n"
	   "Configure SRv6 locator prefix\n"
	   "Specify SRv6 locator prefix\n"
	   "Configure SRv6 locator block length in bits\n"
	   "Specify SRv6 locator block length in bits\n"
	   "Configure SRv6 locator node length in bits\n"
	   "Specify SRv6 locator node length in bits\n"
	   "Configure SRv6 locator argument length in bits\n"
	   "Specify SRv6 locator argument length in bits\n"
	   "Configure SRv6 locator function length in bits\n"
	   "Specify SRv6 locator function length in bits\n")
{
	struct srv6_locator *locator_sid = NULL;
	char *prefix = NULL;
	int ret = 0;
	int idx = 0;
	int block_bit_len = 0;
	int node_bit_len = 0;
	int func_bit_len = 0;
	int args_bit_len = 0;

	locator_sid = zebra_srv6_locator_lookup(argv[1]->arg);
	if (locator_sid) {
		VTY_PUSH_CONTEXT(SRV6_LOC_NODE, locator_sid);
		locator_sid->status_up = true;
		return CMD_SUCCESS;
	}

	locator_sid = srv6_locator_alloc(argv[1]->arg);
	if (!locator_sid) {
		vty_out(vty, "%% Alloc failed\n");
		return CMD_WARNING_CONFIG_FAILED;
	}
	locator_sid->status_up = true;

	prefix = argv[3]->arg;
	ret = str2prefix_ipv6(prefix, &locator_sid->prefix);
	apply_mask_ipv6(&locator_sid->prefix);
	if (!ret) {
		srv6_locator_del(locator_sid);
		vty_out(vty, "Malformed IPv6 prefix\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (argv_find(argv, argc, "block-len", &idx)) {
		block_bit_len = strtoul(argv[idx + 1]->arg, NULL, 10);
	}
	if (argv_find(argv, argc, "node-len", &idx)) {
		node_bit_len = strtoul(argv[idx + 1]->arg, NULL, 10);
	}
	if (argv_find(argv, argc, "func-bits", &idx)) {
		func_bit_len = strtoul(argv[idx + 1]->arg, NULL, 10);
	}
	if (argv_find(argv, argc, "argu-bits", &idx)) {
		args_bit_len = strtoul(argv[idx + 1]->arg, NULL, 10);
	}

	if (block_bit_len == 0 && node_bit_len == 0) {
		block_bit_len = block_bit_len
					? block_bit_len
					: locator_sid->prefix.prefixlen - 24;
		node_bit_len = node_bit_len ? node_bit_len : 24;
	} else if (block_bit_len == 0) {
		block_bit_len = locator_sid->prefix.prefixlen - node_bit_len;
	} else if (node_bit_len == 0) {
		node_bit_len = locator_sid->prefix.prefixlen - block_bit_len;
	} else {
		if (block_bit_len + node_bit_len !=
		    locator_sid->prefix.prefixlen) {
			srv6_locator_del(locator_sid);
			vty_out(vty,
				"%% block-bits + node-bits must be equal to the prefix length\n");
			return CMD_WARNING_CONFIG_FAILED;
		}
	}

	/*
	 * TODO(slankdev): please support variable node-bit-length.
	 * In draft-ietf-bess-srv6-services-05#section-3.2.1.
	 * Locator block length and Locator node length are defined.
	 * Which are defined as "locator-len == block-len + node-len".
	 * In current implementation, node bits length is hardcoded as 24.
	 * It should be supported various val.
	 *
	 * Cisco IOS-XR support only following pattern.
	 *  (1) Teh locator length should be 64-bits long.
	 *  (2) The SID block portion (MSBs) cannot exceed 40 bits.
	 *      If this value is less than 40 bits,
	 *      user should use a pattern of zeros as a filler.
	 *  (3) The Node Id portion (LSBs) cannot exceed 24 bits.
	 */
	locator_sid->block_bits_length = block_bit_len;
	locator_sid->node_bits_length = node_bit_len;
	locator_sid->function_bits_length = func_bit_len;
	locator_sid->argument_bits_length = args_bit_len;

	if (!zebra_srv6_local_sid_get_format(locator_sid)) {
		vty_out(vty, "%% Malformed locator sid format\n");
		srv6_locator_del(locator_sid);
		return CMD_WARNING_CONFIG_FAILED;
	}

	zebra_srv6_locator_add(locator_sid);

	VTY_PUSH_CONTEXT(SRV6_LOC_NODE, locator_sid);
	vty->node = SRV6_LOC_NODE;
	return CMD_SUCCESS;
}

DEFUN(no_srv6_locator_sid, no_srv6_locator_cmd, "no locator WORD",
      NO_STR
      "Segment Routing SRv6 locator\n"
      "Specify locator-name\n")
{
	struct srv6_locator *locator = zebra_srv6_locator_lookup(argv[2]->arg);
	if (!locator) {
		vty_out(vty, "%% Can't find SRv6 locator\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	zebra_srv6_locator_delete(locator); // need pick
	return CMD_SUCCESS;
}

DEFPY(locator_prefix, locator_prefix_cmd,
      "opcode WORD <end | end-dt46 vrf VIEWVRFNAME | end-dt4 vrf VIEWVRFNAME | end-dt6 vrf VIEWVRFNAME>",
      "Configure SRv6 locator prefix\n"
      "Specify SRv6 locator hex opcode\n"
      "Apply the code to an End SID\n"
      "Apply the code to an End.DT46 SID\n"
      "vrf\n"
      "vrf\n"
      "Apply the code to an End.DT4 SID\n"
      "vrf\n"
      "vrf\n"
      "Apply the code to an End.DT6 SID\n"
      "vrf\n"
      "vrf\n")
{
	VTY_DECLVAR_CONTEXT(srv6_locator, locator);
	struct seg6_sid *sid = NULL;
	struct listnode *node = NULL;
	enum seg6local_action_t sidaction = ZEBRA_SEG6_LOCAL_ACTION_UNSPEC;
	int idx = 0;
	char *vrfName = NULL;
	char *prefix = NULL;
	int ret = 0;
	struct prefix_ipv6 ipv6prefix = {0};
	struct in6_addr nexthop = {};
	struct zserv *client;
	struct listnode *client_node;
	struct vrf *vrf = NULL;

	if (argv_find(argv, argc, "end", &idx))
		sidaction = ZEBRA_SEG6_LOCAL_ACTION_END;
	else if (argv_find(argv, argc, "end-dt46", &idx)) {
		sidaction = ZEBRA_SEG6_LOCAL_ACTION_END_DT46;
		vrfName = argv[idx + 2]->arg;
	} else if (argv_find(argv, argc, "end-dt4", &idx)) {
		sidaction = ZEBRA_SEG6_LOCAL_ACTION_END_DT4;
		vrfName = argv[idx + 2]->arg;
	} else if (argv_find(argv, argc, "end-dt6", &idx)) {
		sidaction = ZEBRA_SEG6_LOCAL_ACTION_END_DT6;
		vrfName = argv[idx + 2]->arg;
	}
	prefix = argv[1]->arg;
	ret = str2prefix_ipv6(prefix, &ipv6prefix);
	apply_mask_ipv6(&ipv6prefix);
	if (!ret) {
		vty_out(vty, "Malformed IPv6 prefix\n");
		return CMD_WARNING_CONFIG_FAILED;
	}
	for (ALL_LIST_ELEMENTS_RO(locator->sids, node, sid)) {
		if (IPV6_ADDR_SAME(&sid->ipv6Addr.prefix, &ipv6prefix.prefix)) {
			vty_out(vty,
				"Prefix %s is already exist,please delete it first. \n",
				argv[1]->arg);
			return CMD_WARNING;
		}
	}

	sid = sid_lookup_by_vrf_action(locator, vrfName, sidaction);
	if (sid) {
		vty_out(vty,
			"VRF %s is already exist,please delete it first. \n",
			vrfName);
		return CMD_WARNING;
	}

	sid = srv6_locator_sid_alloc();
	sid->sidaction = sidaction;

	if (vrfName != NULL)
		strlcpy(sid->vrfName, vrfName, VRF_ALIASNAMESIZ);

	sid->ipv6Addr = ipv6prefix;
	strncpy(sid->sidstr, prefix, PREFIX_STRLEN);

	sid->ifname[0] = '\0';
	memcpy(&sid->nexthop, &nexthop, sizeof(struct in6_addr));
	if (!zebra_srv6_local_sid_format_valid(locator, sid)) {
		vty_out(vty, "%% Malformed locator sid opcode format\n");
		srv6_locator_sid_free(sid);
		return CMD_WARNING_CONFIG_FAILED;
	}

	listnode_add(locator->sids, sid);
	zebra_srv6_local_sid_add(locator, sid);

	for (ALL_LIST_ELEMENTS_RO(zrouter.client_list, client_node, client)) {

		zsend_srv6_manager_get_locator_sid_response(
			client, VRF_DEFAULT, locator, sid); // need to pick
	}
	return CMD_SUCCESS;
}

DEFPY(no_locator_prefix, no_locator_prefix_cmd, "no opcode WORD",
      NO_STR
      "Configure SRv6 locator prefix\n"
      "Specify SRv6 locator hex opcode\n")
{
	VTY_DECLVAR_CONTEXT(srv6_locator, locator);
	struct seg6_sid *sid = NULL;
	struct listnode *node, *next;
	char *prefix = NULL;
	int ret = 0;
	struct prefix_ipv6 ipv6prefix = {0};
	struct zserv *client;
	struct listnode *client_node;

	prefix = argv[2]->arg;
	ret = str2prefix_ipv6(prefix, &ipv6prefix);
	if (!ret) {
		vty_out(vty, "Malformed IPv6 prefix\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	for (ALL_LIST_ELEMENTS(locator->sids, node, next, sid)) {
		if (IPV6_ADDR_SAME(&sid->ipv6Addr.prefix, &ipv6prefix.prefix)) {
			for (ALL_LIST_ELEMENTS_RO(zrouter.client_list,
						  client_node, client)) {

				zsend_srv6_manager_del_sid(client, VRF_DEFAULT,
							   locator, sid);
			}
			zebra_srv6_local_sid_del(locator, sid);

			listnode_delete(locator->sids, sid);
			srv6_locator_sid_free(sid);
			return CMD_SUCCESS;
		}
	}
	return CMD_SUCCESS;
}

DEFPY (locator_behavior,
       locator_behavior_cmd,
       "[no] behavior usid",
       NO_STR
       "Configure SRv6 behavior\n"
       "Specify SRv6 behavior uSID\n")
{
	VTY_DECLVAR_CONTEXT(srv6_locator, locator);

	if (no && !CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID))
		/* SRv6 locator uSID flag already unset, nothing to do */
		return CMD_SUCCESS;

	if (!no && CHECK_FLAG(locator->flags, SRV6_LOCATOR_USID))
		/* SRv6 locator uSID flag already set, nothing to do */
		return CMD_SUCCESS;

	/* Remove old locator from zclients */
	zebra_notify_srv6_locator_delete(locator);

	/* Set/Unset the SRV6_LOCATOR_USID */
	if (no)
		UNSET_FLAG(locator->flags, SRV6_LOCATOR_USID);
	else
		SET_FLAG(locator->flags, SRV6_LOCATOR_USID);

	/* Notify the new locator to zclients */
	zebra_notify_srv6_locator_add(locator);

	return CMD_SUCCESS;
}

static int zebra_sr_config(struct vty *vty)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct listnode *node, *opcodenode;
	struct srv6_locator *locator;
	struct seg6_sid *sid;
	char str[256];
	char buf[INET6_ADDRSTRLEN];

	vty_out(vty, "!\n");
	if (zebra_srv6_is_enable()) {
		vty_out(vty, "segment-routing\n");
		vty_out(vty, " srv6\n");
		vty_out(vty, "  locators\n");
		for (ALL_LIST_ELEMENTS_RO(srv6->locators, node, locator)) {
			inet_ntop(AF_INET6, &locator->prefix.prefix,
				  str, sizeof(str));
			vty_out(vty, "   locator %s", locator->name);
			vty_out(vty, " prefix %s/%u", str,
				locator->prefix.prefixlen);
			if (locator->node_bits_length)
				vty_out(vty, " block-len %u",
					locator->block_bits_length);
			if (locator->function_bits_length)
				vty_out(vty, " node-len %u",
					locator->node_bits_length);
			if (locator->block_bits_length)
				vty_out(vty, " func-bits %u",
					locator->function_bits_length);
			if (locator->argument_bits_length)
				vty_out(vty, " argu-bits %u",
					locator->argument_bits_length);
			vty_out(vty, "\n");
			for (ALL_LIST_ELEMENTS_RO(locator->sids, opcodenode,
						  sid)) {
				vty_out(vty, "    opcode %s", sid->sidstr);
				if (sid->sidaction ==
				    ZEBRA_SEG6_LOCAL_ACTION_END)
					vty_out(vty, " end");
				else if (sid->sidaction ==
					 ZEBRA_SEG6_LOCAL_ACTION_END_DT4) {
					vty_out(vty, " end-dt4");
					vty_out(vty, " vrf %s", sid->vrfName);
				} else if (sid->sidaction ==
					   ZEBRA_SEG6_LOCAL_ACTION_END_DT6) {
					vty_out(vty, " end-dt6");
					vty_out(vty, " vrf %s", sid->vrfName);
				} else if (sid->sidaction ==
					   ZEBRA_SEG6_LOCAL_ACTION_END_DT46) {
					vty_out(vty, " end-dt46");
					vty_out(vty, " vrf %s", sid->vrfName);
				} else if (sid->sidaction ==
					   ZEBRA_SEG6_LOCAL_ACTION_END_X) {
					vty_out(vty, " end-x");
					vty_out(vty, " interface %s",
						sid->ifname);
					vty_out(vty, " nexthop %s",
						inet_ntop(AF_INET6,
							  &sid->nexthop, buf,
							  sizeof(buf)));
				}
				vty_out(vty, "\n");
			}
			vty_out(vty, "\n");
			vty_out(vty, "   exit\n");
			vty_out(vty, "   !\n");
		}
		vty_out(vty, "  exit\n");
		vty_out(vty, "  !\n");
		vty_out(vty, " !\n");
		vty_out(vty, "!\n");
	}
	return 0;
}

void zebra_srv6_vty_init(void)
{
	/* Install nodes and its default commands */
	install_node(&sr_node);
	install_node(&srv6_node);
	install_node(&srv6_locs_node);
	install_node(&srv6_loc_node);
	install_default(SEGMENT_ROUTING_NODE);
	install_default(SRV6_NODE);
	install_default(SRV6_LOCS_NODE);
	install_default(SRV6_LOC_NODE);

	/* Command for change node */
	install_element(CONFIG_NODE, &segment_routing_cmd);
	install_element(SEGMENT_ROUTING_NODE, &srv6_cmd);
	install_element(SEGMENT_ROUTING_NODE, &no_srv6_cmd);
	install_element(SRV6_NODE, &srv6_locators_cmd);
	install_element(SRV6_LOCS_NODE, &srv6_locator_cmd);
	install_element(SRV6_LOCS_NODE, &no_srv6_locator_cmd);

	/* Command for configuration */
	install_element(SRV6_LOC_NODE, &locator_prefix_cmd);
	install_element(SRV6_LOC_NODE, &no_locator_prefix_cmd);
	install_element(SRV6_LOC_NODE, &locator_behavior_cmd);

	/* Command for operation */
	install_element(VIEW_NODE, &show_srv6_locator_cmd);
	install_element(VIEW_NODE, &show_srv6_locator_detail_cmd);
}
