#!/usr/bin/env python
# SPDX-License-Identifier: ISC

#
# test_zebra_nhg_partial_inactive.py
#
# Copyright (c) 2026
#

"""
test_zebra_nhg_partial_inactive.py

Reproduce a zebra nexthop-group that has one INACTIVE member and one ACTIVE
(recursive) member while the group as a whole stays Valid/Installed.

Topology (eBGP):

           AS 64601                AS 64600               AS 64602
          +-------+               +-------+              +-------+
          |  PE1  |--fc00:13::/64-|  PE3  |-fc00:23::/64-|  PE2  |
          | :100::1d|             | :300::1f|            | :200::1e|
          +-------+               +-------+              +-------+

  - PE1 advertises its loopback 2064:100::1d/128 to PE3 over eBGP.
  - PE2 advertises its loopback 2064:200::1e/128 to PE3 over eBGP.
  - On PE3 a static route exists:
        ipv6 route 1::1/128 2064:100::1d
        ipv6 route 1::1/128 2064:200::1e
    This forms a 2-member recursive ECMP nexthop-group on PE3.

Test:
  1. Both members resolve -> group has 2 active recursive members.
  2. Withdraw PE1's loopback (shut PE1 BGP advertisement). 2064:100::1d/128
     is removed from PE3's RIB, so the member recursing through it can no
     longer be resolved -> it becomes INACTIVE, while the member through
     2064:200::1e stays ACTIVE (recursive). The group stays Valid/Installed.
"""

import os
import sys
import json
import pytest
import functools
import re

CWD = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(CWD, "../"))

# pylint: disable=C0413
from lib import topotest
from lib.topogen import Topogen, TopoRouter, get_topogen
from lib.topolog import logger
from lib.common_config import step

pytestmark = [pytest.mark.bgpd, pytest.mark.staticd]


def build_topo(tgen):
    "Build a 3-node topology: PE1--PE3--PE2"

    for rname in ("pe1", "pe2", "pe3"):
        tgen.add_router(rname)

    # PE1 <-> PE3
    sw13 = tgen.add_switch("s-pe1-pe3")
    sw13.add_link(tgen.gears["pe1"], nodeif="pe1-eth-pe3")
    sw13.add_link(tgen.gears["pe3"], nodeif="pe3-eth-pe1")

    # PE2 <-> PE3
    sw23 = tgen.add_switch("s-pe2-pe3")
    sw23.add_link(tgen.gears["pe2"], nodeif="pe2-eth-pe3")
    sw23.add_link(tgen.gears["pe3"], nodeif="pe3-eth-pe2")


def setup_module(mod):
    tgen = Topogen(build_topo, mod.__name__)
    tgen.start_topology()

    for rname, router in tgen.routers().items():
        router.load_frr_config(
            os.path.join(CWD, "{}/frr.conf".format(rname)),
            [
                (TopoRouter.RD_ZEBRA, None),
                (TopoRouter.RD_BGP, None),
                (TopoRouter.RD_STATIC, None),
            ],
        )

    tgen.start_router()


def teardown_module(mod):
    tgen = get_topogen()
    tgen.stop_topology()


def _get_route(router, prefix):
    output = router.vtysh_cmd("show ipv6 route {} json".format(prefix))
    return json.loads(output)


def test_underlays_learned():
    "Both PE loopbacks must be learned by PE3 over eBGP before we start."
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    pe3 = tgen.gears["pe3"]
    step("Verify PE3 learned both underlay loopbacks via BGP")

    def _check_underlays():
        for nh in ("2064:100::1d/128", "2064:200::1e/128"):
            j = _get_route(pe3, nh)
            if nh not in j:
                return "{} not in PE3 RIB".format(nh)
            if not j[nh][0].get("installed", False):
                return "{} not installed".format(nh)
        return None

    test_func = functools.partial(_check_underlays)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)
    assert result is None, "PE3 did not learn underlays: {}".format(result)


def test_nhg_two_active_members():
    "1::1/128 should resolve recursively over BOTH members (both active)."
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    pe3 = tgen.gears["pe3"]
    step("Verify 1::1/128 has two active recursive members")

    def _check_two_active():
        j = _get_route(pe3, "1::1/128")
        if "1::1/128" not in j:
            return "1::1/128 not present"
        route = j["1::1/128"][0]
        if not route.get("installed", False):
            return "1::1/128 not installed"

        # Collect top-level (non-resolver) nexthops keyed by gateway.
        actives = {}
        for nh in route.get("nexthops", []):
            if nh.get("resolver"):
                continue
            ip = nh.get("ip")
            actives[ip] = nh.get("active", False)

        for ip in ("2064:100::1d", "2064:200::1e"):
            if ip not in actives:
                return "member {} missing".format(ip)
            if not actives[ip]:
                return "member {} not active".format(ip)
        return None

    test_func = functools.partial(_check_two_active)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)
    assert result is None, "Initial NHG not fully active: {}".format(result)


def test_nhg_one_inactive_one_active():
    """
    Withdraw PE1's loopback. The member recursing through 2064:100::1d must
    become INACTIVE while 2064:200::1e stays ACTIVE; group stays installed.
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    pe1 = tgen.gears["pe1"]
    pe3 = tgen.gears["pe3"]

    step("Withdraw PE1 loopback advertisement (remove 2064:100::1d underlay)")
    pe1.vtysh_cmd(
        "configure terminal\n"
        "router bgp 64601\n"
        " address-family ipv6 unicast\n"
        "  no network 2064:100::1d/128\n"
        " exit-address-family\n"
        "end\n"
    )

    step("Verify 2064:100::1d/128 underlay is gone from PE3")

    def _check_underlay_gone():
        j = _get_route(pe3, "2064:100::1d/128")
        if "2064:100::1d/128" in j:
            return "2064:100::1d/128 still present"
        return None

    test_func = functools.partial(_check_underlay_gone)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)
    assert result is None, "Underlay not withdrawn: {}".format(result)

    step("Verify 1::1/128 NHG now has 2064:100::1d inactive, 2064:200::1e active")

    def _check_partial_inactive():
        j = _get_route(pe3, "1::1/128")
        if "1::1/128" not in j:
            return "1::1/128 not present"
        route = j["1::1/128"][0]

        # Group must still be selected/installed (Valid).
        if not route.get("installed", False):
            return "1::1/128 no longer installed (group went invalid)"

        state = {}
        recursive = {}
        for nh in route.get("nexthops", []):
            if nh.get("resolver"):
                continue
            ip = nh.get("ip")
            state[ip] = nh.get("active", False)
            recursive[ip] = nh.get("recursive", False)

        if "2064:100::1d" not in state:
            return "2064:100::1d member missing (should be retained inactive)"
        if state.get("2064:100::1d"):
            return "2064:100::1d should be INACTIVE but is active"
        if "2064:200::1e" not in state:
            return "2064:200::1e member missing"
        if not state.get("2064:200::1e"):
            return "2064:200::1e should be ACTIVE but is inactive"
        if not recursive.get("2064:200::1e"):
            return "2064:200::1e should be recursive"
        return None

    test_func = functools.partial(_check_partial_inactive)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)

    logger.info(
        "PE3 1::1 route:\n%s", pe3.vtysh_cmd("show ipv6 route 1::1/128")
    )
    logger.info(
        "PE3 nexthop-groups:\n%s", pe3.vtysh_cmd("show nexthop-group rib")
    )

    assert result is None, "Did not reach one-inactive/one-active state: {}".format(
        result
    )


def test_fib_excludes_inactive_member():
    """
    Dataplane proof: although the RIB still lists the inactive member, the
    installed (FIB) nexthop-group must contain ONLY the active member's
    resolved underlay. The inactive member (2064:100::1d, resolving via the
    PE1-facing link pe3-eth-pe1) must NOT appear in the kernel install group;
    only the PE2-facing link pe3-eth-pe2 may appear.

    Depends on test_nhg_one_inactive_one_active having withdrawn PE1's loopback
    (tests run sequentially within a file).
    """
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    pe3 = tgen.gears["pe3"]
    pe1_link = "pe3-eth-pe1"
    pe2_link = "pe3-eth-pe2"

    step("FIB JSON: no installed nexthop may use the PE1-facing link")

    def _check_fib_json():
        j = _get_route(pe3, "1::1/128")
        if "1::1/128" not in j:
            return "1::1/128 not present"
        route = j["1::1/128"][0]
        if not route.get("installed", False):
            return "1::1/128 not installed"

        fib_ifaces = set()
        for nh in route.get("nexthops", []):
            if not nh.get("fib"):
                continue
            iface = nh.get("interfaceName")
            if iface:
                fib_ifaces.add(iface)

        if not fib_ifaces:
            return "no FIB nexthops found for 1::1/128"
        if pe1_link in fib_ifaces:
            return "inactive member leaked into FIB via {}".format(pe1_link)
        if pe2_link not in fib_ifaces:
            return "active member not in FIB (expected via {})".format(pe2_link)
        return None

    test_func = functools.partial(_check_fib_json)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)
    assert result is None, "FIB JSON check failed: {}".format(result)

    step("Kernel netlink: install group must exclude the inactive member")

    def _check_kernel_group():
        # Resolve the kernel route -> nexthop-group id for 1::1/128.
        route_out = pe3.run("ip -6 route show 1::1/128")
        if not route_out or "1::1" not in route_out:
            return "1::1/128 not in kernel route table"

        # Build the set of kernel nexthop lines relevant to this route. The
        # route may render members inline ("nexthop via ... dev ...") or via a
        # separate nhid group; cover both by also expanding any nhid.
        kernel_text = route_out

        m = re.search(r"nhid (\d+)", route_out)
        if m:
            nhid = m.group(1)
            # Expand the group and any of its members.
            grp = pe3.run("ip -6 nexthop show id {}".format(nhid))
            kernel_text += "\n" + (grp or "")
            for mid in re.findall(r"\b(\d+)\b", grp or ""):
                kernel_text += "\n" + (pe3.run("ip -6 nexthop show id {}".format(mid)) or "")

        if pe1_link in kernel_text:
            return "inactive member present in kernel install group ({}):\n{}".format(
                pe1_link, kernel_text
            )
        if pe2_link not in kernel_text:
            return "active member missing from kernel install group ({}):\n{}".format(
                pe2_link, kernel_text
            )
        return None

    test_func = functools.partial(_check_kernel_group)
    _, result = topotest.run_and_expect(test_func, None, count=60, wait=1)

    logger.info(
        "PE3 kernel route 1::1:\n%s", pe3.run("ip -6 route show 1::1/128")
    )
    logger.info("PE3 kernel nexthops:\n%s", pe3.run("ip -6 nexthop show"))

    assert result is None, "Kernel install group check failed: {}".format(result)


if __name__ == "__main__":
    args = ["-s"] + sys.argv[1:]
    sys.exit(pytest.main(args))
