# Route-Event-Based NHG Rebuild — Design

This project changes how Nexthop Group (NHG) information travels the
zebra → dplane → FPM → fpmsyncd pipeline: instead of standalone NHG dplane
events, **route events** become the single carrier. Stock upstream zebra
already deep-copies the full recursive nexthop tree into the route dplane ctx;
with PR #19252 each recursive nexthop additionally carries its `resolved_via`
NHG id and resolving prefix. The dplane_fpm_sonic plugin decomposes that tree
into a deduplicated set of dplane NHG objects (Merkle-hash keyed internally,
plugin-allocated uint32 ids on the wire) and delivers them to fpmsyncd by
reusing `RTM_NEWNHGFIB`/`RTM_DELNHGFIB` and `RTA_NH_ID` — so the existing
fpmsyncd code is reused essentially unchanged. fpmsyncd rebuilds the NHG/PIC
objects and maintains a "resolving prefix → impacted NHG" binding table
(analogous to zebra's NHT database), using resolving-prefix delete/add events
to drive PIC-core-style fast convergence.

Repositories involved:
- FRR: `/Users/eddie/community/frr` (upstream `master` +
  [PR #19252](https://github.com/FRRouting/frr/pull/19252) carried as patches)
- dplane plugin: `src/sonic-frr/dplane_fpm_sonic` in sonic-buildimage
  (changes on top of the existing ribfib_2 version)
- fpmsyncd: sonic-swss (existing ribfib_2 code reused; minimal delta)
- JSON schema: `src/libraries/sonic-fib` (libnexthopgroup, optional fields)

Full details: `docs/alinos/specs/2026-07-29-route-event-nhg-rebuild-design.md`.

## Core Mechanism: Route Events Carry NHG Information

**Motivation** (from discussion with Mark Stapp and Donald Sharp, equally
weighted):

1. Minimize zebra changes / upstreamability — stock `dplane_ctx_route_init()`
   already copies the full recursive tree; received and recursive NHGs are
   derivable in the FPM plugin, so no fork-specific zebra plumbing is needed.
   The only zebra delta is PR #19252.
2. Ordering correctness — a single route event atomically carries route + NHG
   information, eliminating ordering/batching races between NHG events and
   route events (cf. the skip-kernel premature-flush bug, commit `2bdd2f84a6`).

**Key decisions (D1–D13):**

| # | Decision |
|---|----------|
| D1 | Upstream zebra keeps sending `DPLANE_OP_NH_*` for kernel NHGs; in nhg-fib mode the plugin ignores them for FPM (`fnc->use_nhg` unset). Fork-only PIC-context / NHGFIB-from-NHG-event paths do not exist on the upstream base |
| D2 | NHG identity: canonical 64-bit Merkle hash is the plugin-internal dedupe key; the **wire id is a plugin-allocated `uint32_t` dplane NHG id**, so `RTA_NH_ID` and fpmsyncd `ribID` stay 32-bit |
| D3 | Hybrid split: plugin decomposes each route ctx tree into a 3-level dplane NHG hierarchy and allocates ids; NHG rebuild, SONiC ID allocation, and binding table live in fpmsyncd |
| D4 | Reuse `RTM_NEWNHGFIB`/`RTM_DELNHGFIB` (JSON format fpmsyncd already parses) with dplane ids; NEW precedes the first referencing route, DEL on refcount 0; routes carry `RTA_NH_ID = top dplane id` |
| D5 | Plugin-driven lifecycle: per-route map + refcounts; first ref → NEW, last deref → DEL + id freed; fpmsyncd follows messages, no mirrored refcount; cleared on FPM (re)connect |
| D6 | `resolved_via` + `resolved_addr/resolved_len` live in `struct nexthop` exactly as PR #19252 defines them |
| D7 | Bidirectional convergence trigger: resolving-prefix delete → repair (drop dead members); add → restore from stored json; `repaired` flag tracks degraded state |
| D8 | Uniform scope: in nhg-fib mode every route (including non-recursive) references a dplane NHG id |
| D9 | Binding covers all recursion depths (e.g. BGP → static → IGP) |
| D10 | Mode selection moves into the plugin: new `fpm use-nhg-fib` vty command (modeled on `use-next-hop-groups`, mutually exclusive with it) replaces the fork's zebra `--nhg-fib` global |
| D11 | FRR base = upstream master + PR #19252 only; all other ribfib_2 zebra changes dropped; plugin removes fork-only zebra API usage so it compiles against upstream headers |
| D12 | fpmsyncd reuses existing ribfib_2 code (`onNextHopGroupFullMsg`, `NHGMgr`, `RTA_NH_ID` route path unchanged); delta limited to the binding-table module, optional JSON fields `resolved_prefix`/`vrf_id`, and the unknown-id protocol-error path |
| D13 | Observability: `show fpm nhg-fib [id <id>] [json]` dumps the dplane NHG table and the **rib→dplane NHG mapping** (zebra NHG ids recorded from referencing route events); counters in `show fpm status` |

**Wire protocol.** Route messages carry the existing `RTA_NH_ID` attribute
filled with the plugin-allocated top-level dplane id (appended by the plugin
in its own buffer — no zebra encoder change). NHGFIB JSON objects form a
3-level hierarchy: L-A received group (RECEIVED flag) → L-B resolved group per
recursive NH (RECURSIVE flag, new optional `resolved_prefix` + `vrf_id`
fields, any depth) → L-C leaf singletons; id/depends entries are dplane ids.
SRv6 VPN routes keep their two existing id attributes, filled from the derived
objects. Each NHGFIB message gets its own FPM frame (64KB cap, asserted at
encode time). The Merkle hash never appears on the wire.

## Per-Repository Changes

- **FRR (upstream + PR #19252)**: cherry-pick PR #19252 (lib nexthop
  resolved-via fields, `get_resolving_info()`, show output,
  `NHA_FPM_RESOLVED_VIA`). No other zebra change; kernel NHG programming
  untouched. Risk tracked: the PR is open ("do not merge") and its final
  shape may change — this design needs both the id and the prefix fields.
- **dplane_fpm_sonic**: new `fpm use-nhg-fib` vty command; removal of
  fork-only zebra API usage (PIC-context dispatch, `nhe_received`,
  `nh_grp_full`/depends/dependents getters); the derivation engine —
  post-order DFS over `dplane_ctx_get_ng()`, Merkle-hash dedupe, uint32 id
  allocator, `dplane_nhg_table` + `route_nhg_map`, refcount lifecycle with
  children-before-parent NEW and parent-before-child DEL emission, reconnect
  flush without DELs; `RTA_NH_ID` appended after route encode; tree-input
  variants of `build_c_nexthopgroupfull_multi/_singleton`; new show commands
  (D13).
- **fpmsyncd**: existing code reused (D12). New: parse optional
  `resolved_prefix`/`vrf_id` into `RIBNHGEntry`; binding-table module with
  repair/restore triggers; unknown `RTA_NH_ID` → protocol error → close FPM
  socket to force zebra full replay.
- **sonic-fib (libnexthopgroup)**: `NextHopGroupFull.json` schema gains
  optional `resolved_prefix` (string) and `vrf_id` (integer) fields;
  `fib::from_json` tolerates absence.

## Binding Table and Convergence (NHT-like, in fpmsyncd)

`(vrf, afi, resolving prefix) → set<{ribID, node-in-group}>`, populated at NHG
registration from every recursive member's `resolved_prefix` at any depth. On
`RTM_DELROUTE(P)` (exact match): drop that node's resolved leaf subtree from
the NHG members — one NHG update repairs all dependent routes
(PIC-core-style), ahead of per-route re-convergence; set `repaired`. On
`RTM_NEWROUTE(P)`: restore members from the stored JSON and clear `repaired`
(covers the IGP-flap case where the plugin dedupes an identical re-resolution
and no new NHGFIB arrives). A modify of P is not a trigger: zebra re-resolves
and re-sends routes; changed trees produce new dplane objects and old ones
drain via DELNHGFIB.

## Warm Reboot and Error Handling

- Warm reboot: parity with existing ribfib_2 behavior — dplane ids are not
  stable across a zebra restart (same as zebra NHG ids today); replay
  re-registers NHGs and `WarmStartHelper` reconciles ROUTE_TABLE (only).
  Content-based sonic-ID preservation is follow-up work.
- Route referencing an unknown dplane id → close the FPM socket → zebra
  reconnects and replays everything. Merkle collisions are plugin-internal
  and handled by comparing subtrees on hash hit (correctness never depends on
  hash uniqueness). FPM `msg_len` is uint16 → 64KB cap, asserted at encode
  time.

## Test Design (Summary)

- **UT**: upstream FRR `make check` (`tests/lib/test_nexthop.c` additions) +
  upstream topotests (`fpm_testing_topo1`, `zebra_nhg_check`,
  `zebra_recursive_nhg_installed`); fpmsyncd gtest (`tests_fpmsyncd` in
  `tests/mock_tests/`: existing `nhgmgr_ut.cpp` as regression, new
  `nhgbinding_ut.cpp`, `test_routesync.cpp` additions). dplane_fpm_sonic has
  no UT harness; covered via fpmsyncd decode UTs on captured message streams,
  fpm topotest, and on-device `show fpm nhg-fib` (recorded exception).
- **IT**: 5 cases covering 8 test points designed
  (TC-NHGROUTE-FUNC/CONV/RESYNC/WARM/SCALE-001; assertions include
  NEWNHGFIB-exactly-once, DELNHGFIB reclamation, fast repair on prefix
  delete, warm-reboot parity, 512-ECMP scale). **IT implementation is
  Deferred by user decision** — no test repository confirmed;
  `/alinos.writing-plans` must not generate repository-specific IT code
  tasks.

## Change Log

| Date | Feature | Change |
|------|---------|--------|
| 2026-07-29 | route-event-nhg-rebuild | Initial design: Approach C (hybrid), content-addressed NHGs, binding-table fast convergence |
| 2026-07-30 | route-event-nhg-rebuild | Grill-Me revisions: bidirectional trigger, all-depth binding, standalone tree message, idempotent warm-reboot write-through; user corrections D1 (keep DPLANE_OP_NH_*), D10 (--nhg-fib path selection), D4/D5 (reuse RTM_NEWNHGFIB/DELNHGFIB, plugin-driven lifecycle) |
| 2026-07-30 | route-event-nhg-rebuild | D3 code-level detail (spec §4.2.1): 3-level dplane NHG hierarchy, Merkle hashing, refcount cascade, post-order DFS emission order; route_nhg_map memory analysis |
| 2026-07-30 | route-event-nhg-rebuild | Document translated to English for SONiC community publication |
| 2026-07-30 | route-event-nhg-rebuild | D1 scope reduction: PIC-context removal deferred to a separate cleanup PR |
| 2026-07-30 | route-event-nhg-rebuild | **Base pivot (D11–D13)**: FRR base = upstream master + PR #19252 only; wire id = plugin-allocated uint32 (hash internal); `fpm use-nhg-fib` vty replaces zebra `--nhg-fib`; fpmsyncd reused unchanged except binding table + schema fields; new `show fpm nhg-fib` rib→dplane mapping |
