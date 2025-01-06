#!/usr/bin/env python

#
# <template>.py
# Part of NetDEF Topology Tests
#
# Copyright (c) 2017 by
# Network Device Education Foundation, Inc. ("NetDEF")
#
# Permission to use, copy, modify, and/or distribute this software
# for any purpose with or without fee is hereby granted, provided
# that the above copyright notice and this permission notice appear
# in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND NETDEF DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL NETDEF BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
# DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
# WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
# ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
# OF THIS SOFTWARE.
#

"""
<template>.py: Test <template>.
"""

import os
import sys
import pytest
import json
import re
import time
import pdb
from functools import partial

# Save the Current Working Directory to find configuration files.
CWD = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(CWD, '../'))

# pylint: disable=C0413
# Import topogen and topotest helpers
from lib import topotest
from lib.topogen import Topogen, TopoRouter, get_topogen
from lib.topolog import logger

"""
test_sbfd_topo1.py: test simple sbfd with IPv6 encap. RT1 is sbfd Initiator, RT2 is sbfd Reflector

 +----+----+        +----+----+
 |         |        |         |
 |   RT1   |   1    |   RT2   |
 |         +--------+         |
 | 2001::10|        | 2001::20|
 +----+----+        +----+----+

"""
pytestmark = [pytest.mark.bfdd, pytest.mark.sbfd]

def show_bfd_check(router, status, type='echo', encap=None):
    output = router.cmd("vtysh -c 'show bfd peers'")
    if encap:
        # check encap data if any
        pattern1 = re.compile(r'encap-data {}'.format(encap))
        ret = pattern1.findall(output)
        assert len(ret) > 0, output

    # check  status
    pattern2 = re.compile(r'Status: {}'.format(status))
    ret = pattern2.findall(output)
    assert len(ret) > 0, output

    # check type
    pattern3 = re.compile(r'Peer Type: {}'.format(type))
    ret = pattern3.findall(output)
    assert len(ret) > 0, output

def build_topo(tgen):
    "Test topology builder"

    # This function only purpose is to define allocation and relationship
    # between routers, switches and hosts.
    #
    # Example
    #
    # Create 2 routers
    for routern in range(1, 3):
        tgen.add_router('r{}'.format(routern))

    # Create a switch with just one router connected to it to simulate a
    # empty network.
    switch = tgen.add_switch('s1')
    switch.add_link(tgen.gears['r1'])
    switch.add_link(tgen.gears['r2'])

def setup_module(mod):
    "Sets up the pytest environment"
    # This function initiates the topology build with Topogen...
    tgen = Topogen(build_topo, mod.__name__)
    # ... and here it calls Mininet initialization functions.
    tgen.start_topology()

    # This is a sample of configuration loading.
    router_list = tgen.routers()

    for rname, router in router_list.items():
        router.load_config(
            TopoRouter.RD_ZEBRA,
            os.path.join(CWD, '{}/zebra.conf'.format(rname))
        )
        router.load_config(
            TopoRouter.RD_BFD,
            os.path.join(CWD, '{}/bfdd.conf'.format(rname))
        )

    # After loading the configurations, this function loads configured daemons.
    tgen.start_router()

    # Verify that we are using the proper version and that the BFD
    # daemon exists.
    for router in router_list.values():
        # Check for Version
        if router.has_version('<', '5.1'):
            tgen.set_error('Unsupported FRR version')
            break

def teardown_module(mod):
    "Teardown the pytest environment"
    tgen = get_topogen()
    # This function tears down the whole topology.
    tgen.stop_topology()


# step 1 : config sbfd Initiator and reflector
def test_sbfd_config_check():
    "Assert that config sbfd and check sbfd status."
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    # config sbfd
    r1 = tgen.net['r1']
    r1.cmd("ping -c 5 2001::20")
    time.sleep(5)
    r1.cmd("vtysh -c 'config t' -c 'bfd' -c 'peer 2001::20 bfd-mode sbfd-init bfd-name 2-44 local-address 2001::10 remote-discr 1234'")

    r2 = tgen.net['r2']
    r2.cmd("vtysh -c 'config t' -c 'bfd' -c 'sbfd reflector source-address 2001::20 discriminator 1234'")

    logger.info('waiting 5 sec ... for sbfd up')
    time.sleep(5)
    show_bfd_check(r1, 'up', type='sbfd initiator')

# step 2: shutdown if and no shutdown if then check sbfd status
def test_sbfd_updown_interface():
    "Assert that updown interface then check sbfd status."
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    r1 = tgen.net['r1']
    r2 = tgen.net['r2']

    # shutdown interface
    r2.cmd("vtysh -c 'config t' -c 'interface r2-eth0' -c 'shutdown'")
    time.sleep(5)
    show_bfd_check(r1, 'down', type='sbfd initiator')

    # up interface
    r2.cmd("vtysh -c 'config t' -c 'interface r2-eth0' -c 'no shutdown'")
    logger.info('waiting 5 sec ... for sbfd up after no shutdown')
    time.sleep(5)
    show_bfd_check(r1, 'up', type='sbfd initiator')

# step 3: change transmit-interval and check sbfd status according to the interval time
def test_sbfd_change_transmit_interval():
    "Assert that sbfd status changes align with transmit-interval."
    tgen = get_topogen()
    if tgen.routers_have_failure():
        pytest.skip(tgen.errors)

    r1 = tgen.net['r1']
    r2 = tgen.net['r2']

    r1.cmd("vtysh -c 'config t' -c 'bfd' -c 'peer 2001::20 bfd-mode sbfd-init bfd-name 2-44 local-address 2001::10 remote-discr 1234' -c 'transmit-interval 3000'")
    #wait sometime for polling finish
    time.sleep(3)

    # shutdown interface
    r2.cmd("vtysh -c 'config t' -c 'interface r2-eth0' -c 'shutdown'")
    logger.info('waiting 5 sec ... for sbfd still up after shutdown')
    time.sleep(5)
    #sbfd is still up
    show_bfd_check(r1, 'up', type='sbfd initiator')
    #wait enough time for timeout
    logger.info('waiting 10 sec ... for sbfd down after shutdown')
    time.sleep(10)
    show_bfd_check(r1, 'down', type='sbfd initiator')

    r2.cmd("vtysh -c 'config t' -c 'interface r2-eth0' -c 'no shutdown'")
    logger.info('waiting 5 sec ... for sbfd up after no shutdown')
    time.sleep(5)
    show_bfd_check(r1, 'up', type='sbfd initiator')

# Memory leak test template
def test_memory_leak():
    "Run the memory leak test and report results."
    tgen = get_topogen()
    if not tgen.is_memleak_enabled():
        pytest.skip('Memory leak test/report is disabled')

    tgen.report_memory_leaks()

if __name__ == '__main__':
    args = ["-s"] + sys.argv[1:]
    sys.exit(pytest.main(args))
