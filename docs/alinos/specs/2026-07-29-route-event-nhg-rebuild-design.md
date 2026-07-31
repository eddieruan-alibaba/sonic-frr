# Route-Event-Based NHG Rebuild Design (route-event-nhg-rebuild)

- Date: 2026-07-29 (revised 2026-07-30: rebased onto upstream FRR)
- Repos in scope:
  - FRR: `/Users/eddie/community/frr` (upstream `master` + [PR #19252](https://github.com/FRRouting/frr/pull/19252) carried as patches — the upstream version of ribfib_2's resolved-via WIP)
  - dplane plugin: `/Users/eddie/community/sonic-buildimage/src/sonic-frr/dplane_fpm_sonic` (changes on top of the existing ribfib_2 version)
  - fpmsyncd: `/Users/eddie/community/sonic-swss/fpmsyncd` (existing ribfib_2 code reused; minimal delta)
  - JSON schema library: `/Users/eddie/community/sonic-buildimage/src/libraries/sonic-fib` (libnexthopgroup — schema extension only)
- Reference (previous fork-based iteration): `/Users/eddie/community/sonic-frr` branch `ribfib_2`
- Status: design approved section-by-section in brainstorming session

## 1. Motivation

Two equally weighted drivers (from discussion with Mark Stapp and Donald Sharp):

1. **Minimize zebra changes / upstreamability.** Route dplane events in **stock
   upstream FRR** already deep-copy the full recursive nexthop tree
   (`dplane_ctx_route_init()`: `copy_nexthops(&(ctx->u.rinfo.zd_ng.nexthop),
   re->nhe->nhg.nexthop, NULL)` — "recursive info is included too"). Received
   NHG and recursive NHG information can be derived from that tree in the FPM
   plugin, so **none** of the ribfib_2 fork's zebra additions are needed
   (`--nhg-fib` global, `nhe_received` dplane plumbing,
   `nh_grp_full`/`depends`/`dependents` ctx arrays, skip-kernel ctxs,
   `INSTALLED_FPM_ONLY`, `DPLANE_OP_PIC_CONTEXT_*`). The only zebra delta is
   PR #19252 (resolved-via id + resolving prefix on `struct nexthop`).
2. **Ordering correctness.** One route event atomically carries route + full NHG info,
   eliminating NHG-event vs route-event ordering/batching races (cf. the skip-kernel
   premature-flush bug fixed in `2bdd2f84a6`).

## 2. Decisions

| # | Decision | Choice |
|---|----------|--------|
| D1 | NHG events | Upstream zebra keeps sending `DPLANE_OP_NH_*` for kernel NHGs (untouched); in nhg-fib mode the plugin **ignores them for FPM** (`fnc->use_nhg` unset → existing gating skips them). Fork-only `DPLANE_OP_PIC_CONTEXT_*` / NHGFIB-from-NHG-event paths do not exist on the upstream base; the plugin's uses of fork-only zebra APIs are removed/guarded (D11) |
| D2 | NHG identity | **Content-based dedupe, compact wire id**: canonical 64-bit Merkle hash of the (sub)tree is the plugin-internal dedupe key; the **wire id is a plugin-allocated `uint32_t` dplane NHG id** (private id space), so `RTA_NH_ID` and fpmsyncd's `ribID` stay 32-bit and fpmsyncd is reused unchanged |
| D3 | Rebuild/state location | dplane_fpm_sonic decomposes each route ctx tree into a 3-level dplane NHG object hierarchy with Merkle-style hash dedupe and allocates dplane ids (code-level detail in §4.2.1). Resolved-via prefix/id are **plugin-internal identity input only** — never emitted. fpmsyncd keeps NHG rebuild + SONiC ID allocation, unchanged |
| D4 | Tree transmission | **Reuse `RTM_NEWNHGFIB`/`RTM_DELNHGFIB`** (same JSON format fpmsyncd already parses) — id fields carry plugin-allocated dplane ids. `RTM_NEWNHGFIB` sent before the first referencing route (once per object per connection); `RTM_DELNHGFIB` when the plugin refcount hits 0. Route messages carry `RTA_NH_ID = top-level dplane id` (stream order guarantees define-before-reference) |
| D5 | NHG lifecycle | **Plugin-driven**: plugin `route → top id` map refcounts route references; first ref → emit NEW, last deref → emit DEL and evict (id returned to the allocator). fpmsyncd follows the messages — no mirrored refcount. Cleared on FPM (re)connect; zebra full replay rebuilds |
| D6 | Resolving prefix carrier | `resolved_via` + `resolved_addr/resolved_len` in `struct nexthop`, exactly as PR #19252 defines them — deep-copied for free by `nexthop_dup` |
| D7 | Convergence trigger | **Out of scope for the plugin (§5).** No binding table, no repair, no `repaired` state in dplane_fpm_sonic: the plugin cannot express a multi→single/empty NHG update through the unchanged `RIBNHGEntry` path. Convergence relies on zebra re-resolution; the fast-repair optimisation is a follow-up fpmsyncd change |
| D8 | Scope | **Uniform**: in nhg-fib mode every route (including simple non-recursive ones) goes through the derivation path and references a dplane NHG id |
| D9 | Binding depth | Resolving prefixes of RECURSIVE nodes at **all depths** are bound (e.g. BGP→static→IGP); binding value identifies the node within the tree |
| D10 | Mode selection | **Moves entirely into the plugin**: new `fpm use-nhg-fib` vty command (modeled on `fpm use-next-hop-groups`, `dplane_fpm_sonic.c:415`) sets `fnc->use_nhg_fib`, replacing the fork's `zebra --nhg-fib` global (`gfnc->use_nhg_fib = zebra_nhg_fib_enabled` at :4117). Mutually exclusive with `use-next-hop-groups`. Legacy modes behave exactly as today |
| D11 | FRR base | **Upstream `/Users/eddie/community/frr` master + PR #19252 only** (lib nexthop resolved-via fields, `get_resolving_info()`, show output, `NHA_FPM_RESOLVED_VIA` FPM encode). All other ribfib_2 zebra changes are dropped. The plugin removes/guards its uses of fork-only zebra APIs (`dplane_ctx_get_nhe_received_id`, `dplane_ctx_get_nhe_ng/nh_grp_full/depends/dependents`, `DPLANE_OP_PIC_CONTEXT_*`) so it compiles against upstream headers |
| D12 | fpmsyncd reuse | **Zero fpmsyncd change.** `onNextHopGroupFullMsg` → `NHGMgr::addNHGFull/delNHGFull` and the `RTA_NH_ID` route path work unchanged with plugin ids (`ribID` stays `uint32_t`). No binding module, no schema change, no new handler — the plugin absorbs all new behaviour. Corollary (D14): the JSON the plugin emits must match what `NHGMgr` already consumes, byte-semantics included |
| D14 | JSON member encoding | The `nh_grp_full` array must be the **flattened, all-depths** list the existing `NHGMgr` expects (group nodes carry their direct-child count; leaves carry `num_direct = 0`), replicating `zebra_nhg_nhe2grp_full_internal`; `getResolvedGroupFromNHGFull` keeps only `num_direct == 0` entries. The `RECEIVED` flag means "pre-resolution group, do not program directly" (`NHGMgr` suppresses the SONiC NHG object for NORMAL+RECEIVED and creates a PIC context for SRv6+RECEIVED), so the plain-IP L-A a route points at must **not** carry it |
| D13 | Observability | New plugin show commands: `show fpm nhg-fib [id <id>] [json]` dumps the dplane NHG table (id, hash, level, flags, refcount, children, resolving prefix) and the **rib→dplane NHG mapping** — each object records the zebra NHG ids (`dplane_ctx_get_nhe_id`) seen on referencing route events; counters added to `show fpm status` |

## 3. Architecture and Data Flow

```
zebra (upstream + PR #19252) rib_process
  └─ route dplane ctx (full recursive tree; per-nexthop resolved_via NHG id
     + resolving prefix)   [kernel NHG events still exist; plugin ignores
      them for FPM in nhg-fib mode]
       └─ dplane_fpm_sonic  (fpm use-nhg-fib):
            decompose tree → dplane NHG objects (dedupe by Merkle hash)
            first ref   → allocate uint32 dplane id,
                          send RTM_NEWNHGFIB {id, json} before the route
            route msg  += RTA_NH_ID = top-level dplane id   (always)
            last deref  → send RTM_DELNHGFIB {id}, free id
       └─ fpmsyncd (existing ribfib_2 code):
            RTM_NEWNHGFIB → NHGMgr::addNHGFull: register by ribID(=dplane id),
                       allocate SONiC IDs, write NEXTHOP_GROUP_TABLE
                       (+ PIC_CONTEXT_TABLE),
                       add bindings: resolving-prefix → {dplane id, node}  [new]
            RTM_DELNHGFIB → delNHGFull: free SONiC IDs, delete objects,
                       remove bindings
            ROUTE_TABLE entry references sonic NHG id (via RTA_NH_ID lookup)
```

### 3.1 Wire protocol (FPM)

| Message / Attribute | When | Content |
|---|---|---|
| `RTA_NH_ID` (existing attr, existing fpmsyncd parse via `rtnl_route_get_nh_id`, `routesync.cpp:2676`) | every RTM_NEWROUTE in nhg-fib mode | **plugin-allocated uint32 dplane NHG id** of the route's top-level (L-A) object — written by the plugin into its own buffer after `netlink_route_multipath_msg_encode`, so no zebra encoder change |
| `RTM_NEWNHGFIB`/`RTM_DELNHGFIB` (**reused**, msg types 5000/5001, existing fpmsyncd handler `onNextHopGroupFullMsg`) | NEW: first reference to each dplane NHG object per FPM connection, children emitted before parents, all before the first referencing route. DEL: plugin refcount hits 0 (cascading child de-refs) | One message per dplane NHG object, **exactly today's JSON schema — no new fields** (D12/D14); id and depends/nh_grp_full entries carry **plugin-allocated uint32 dplane ids**, and `nh_grp_full` is the flattened all-depths member list. Objects form a 3-level hierarchy (§4.2.1): L-A group (RECEIVED **not** set for plain IP) → L-B per-recursive-NH resolved group (flag RECURSIVE, at any depth per D9) → L-C leaf singletons. Resolving prefix/id stay plugin-internal |
| `RTM_NEWNEXTHOP`/`RTM_DELNEXTHOP` to FPM | not sent in nhg-fib mode (`fnc->use_nhg` unset → existing gating); legacy modes unchanged | — |
| `NHA_FPM_RESOLVED_VIA` (from PR #19252) | kernel-NHG FPM encodes in legacy mode | unused by the nhg-fib path (resolving info travels in NHGFIB JSON) |

The Merkle hash never appears on the wire — it is the plugin's dedupe key
only (D2). This keeps `RTA_NH_ID` and fpmsyncd's `ribID` at 32 bits and the
existing fpmsyncd parse paths byte-compatible. Each NHGFIB message still gets
its own FPM frame (64KB budget, `fpm_msg_hdr_t.msg_len` uint16), asserted at
encode time.

Applies uniformly to **all** routes in nhg-fib mode (D8), including
non-recursive ones (single-level tree). Derivation in fpmsyncd: received NHG =
top-level members; resolved NHG = leaf members (existing
`RIBNHGEntry::getResolvedGroupFromNHGFull`, `nhgmgr.cpp:1061`).

### 3.2 SRv6 VPN routes

Resolving-prefix/id are never encoded (D3): they exist only inside the plugin to
drive the binding table (§5).

`netlink_srv6_vpn_route_msg_encode` keeps its existing two attributes —
`FPM_ROUTE_ENCAP_SRV6_NH_RECEIVED_ID` and `FPM_ROUTE_ENCAP_SRV6_NH_ID` — so
the existing fpmsyncd `onSrv6VpnRouteMsg` path (`routesync.cpp:1921-1975`)
is reused unchanged. In nhg-fib mode the plugin fills them with **dplane ids
from the derived objects** (received id = L-A object id; nh id = its resolved
view) instead of the fork's `dplane_ctx_get_nhe_received_id`/`nhe_id` values.
PIC-context creation is driven, as today, by the RECEIVED flag inside the
NHGFIB JSON (`checkNeedCreateSonicPICObj`).

## 4. Per-Repository Changes

### 4.1 FRR (`/Users/eddie/community/frr`, branch off upstream `master`)

Apply **PR #19252 only** (cherry-picked; it is open with a "do not merge"
label, so we carry it as patches and track upstream evolution):
1. `lib/nexthop.h/.c`: `resolved_via` + `resolved_addr`/`resolved_len` on
   `struct nexthop`; copied in `nexthop_copy_no_recurse`; json/show output.
2. `zebra/zebra_nhg.c`: `get_resolving_info()` called from `nexthop_active()`
   after `nexthop_set_resolved()`.
3. `zebra/kernel_netlink.h` + `zebra/rt_netlink.c`: `NHA_FPM_RESOLVED_VIA`
   FPM-only attribute encode.

**No other zebra change.** Dropped relative to ribfib_2: `--nhg-fib` global,
`nhe_received` dplane plumbing, `nh_grp_full`/`depends`/`dependents` ctx
arrays + getters, skip-kernel ctxs, `INSTALLED_FPM_ONLY`/`REINSTALL_FPM_ONLY`,
`DPLANE_OP_PIC_CONTEXT_*`, NHGFIB emission from NHG events. Stock
`dplane_ctx_route_init()` tree copy is the entire zebra→plugin contract.
Kernel NHG programming is untouched.

Risk to track: PR #19252's author is still debating id-vs-prefix shape; this
design needs **both** fields (currently both are in the PR).

### 4.2 dplane_fpm_sonic (sonic-buildimage, on top of the ribfib_2 version)

- **New vty command `fpm use-nhg-fib`** (+ `no` form; modeled on
  `fpm_use_nhg_cmd` at :267-292/415): sets `fnc->use_nhg_fib`, persisted in
  config write; replaces `gfnc->use_nhg_fib = zebra_nhg_fib_enabled` (:4117).
  Rejected when `use-next-hop-groups` is set (and vice versa).
- **Compile against upstream FRR headers**: remove/guard all fork-only API
  usage — `DPLANE_OP_PIC_CONTEXT_*` dispatch (:3288-3312) and
  `netlink_pic_context_msg_encode` (:2317), `dplane_ctx_get_nhe_received_id`
  (:1730), `dplane_ctx_get_nhe_ng/nhg_flags/nh_grp_full(_count)/depends/
  dependents` (:1220-1256, :2759-2811), NHGFIB-from-NHG-event branch
  (:3216-3242). The route-derivation engine below replaces them all.
- **Derivation engine** (§4.2.1): 3-level decomposition from
  `dplane_ctx_get_ng(ctx)`, Merkle-hash dedupe, per-object **uint32 dplane id
  allocator** (monotonic with free-list, private space), refcount lifecycle,
  NEW/DEL NHGFIB emission via tree-input variants of
  `build_c_nexthopgroupfull_multi/_singleton`.
- **Route encode**: call `netlink_route_multipath_msg_encode(...)` with
  nexthop-group emission off, then append `RTA_NH_ID = top dplane id` to the
  assembled message in the plugin's own `nl_buf` (adjust `nlmsg_len`) — zero
  zebra encoder change. SRv6 VPN route encode fills its two existing id
  attributes from the derived objects (§3.2).
- **Show commands** (D13): `show fpm nhg-fib [id <id>] [json]` — per object:
  dplane id, hash (hex), level, flags, refcount, children (id/weight),
  resolving prefix + vrf, and the **rib NHG ids** observed on referencing
  route events (recorded from `dplane_ctx_get_nhe_id(ctx)` at map-upsert
  time) → answers "how does rib NHG X map to dplane NHG(s)". Counters
  (objects created/deleted, NHGFIB sent, dedupe hits) added to
  `show fpm status`.

#### 4.2.1 Dplane NHG derivation from the route ctx tree (code-level)

**Input.** `dplane_ctx_get_ng(ctx)->nexthop` — the deep-copied tree from
`dplane_ctx_route_init()` (`zebra_dplane.c:4123-4126`). Top-level chain =
nexthops as received (recursive parents keep `NEXTHOP_FLAG_RECURSIVE`); each
recursive parent's `->resolved` chain holds its resolution (possibly nested);
each recursive nexthop carries `resolved_via` + `resolved_addr/resolved_len`
(PR #19252). None of the fork-only NHG-event ctx getters are used — only
stock `dplane_ctx_get_ng()` and `dplane_ctx_get_nhe_id()` (the latter solely
for the rib→dplane show mapping, D13).

**Object model.** The tree decomposes into a 3-level hierarchy mirroring
zebra's NHE model, so fpmsyncd's `NHGMgr` semantics port unchanged:

- **L-C singleton**: one leaf `struct nexthop` (no `->resolved`).
  Emitted via the existing `build_c_nexthopgroupfull_singleton()` shape
  (`dplane_fpm_sonic.c:1270`) with `id = its dplane id`.
- **L-B resolved group**: one recursive nexthop's resolution — depends = the
  dplane ids of its leaf singletons (or nested L-B for multi-level recursion,
  D9). JSON carries `nhg_flags |= NEXTHOP_GROUP_RECURSIVE`, the recursive
  NH's `gate`/`type` (as `build_c_nexthopgroupfull_multi()` does today at
  `dplane_fpm_sonic.c:1230-1238`), plus its `resolved_prefix` and
  `vrf_id` (new optional JSON fields — feeds the fpmsyncd binding table).
- **L-A received group**: the whole top-level chain — depends = per-member
  dplane ids (L-B for recursive members, L-C for direct members). JSON carries
  `nhg_flags |= NEXTHOP_GROUP_RECEIVED` when any member is recursive →
  drives `checkNeedCreateSonicPICObj()` in fpmsyncd exactly as today.
  A single-member non-recursive route degenerates to L-A == L-C (one
  singleton object, no depends).

**Hashing (Merkle-style, bottom-up).**

```c
/* leaf: canonical field encode into a flat buffer, then hash */
h(leaf) = H(vrf_id, type, gate|bh_type, ifindex, src, rmap_src,
            NEXTHOP_FLAGS_HASHED bits, nh_label_type, label stack,
            srv6 seg list, seg6local action + context (memberwise —
            the source struct has interior padding), encap_behavior)
/* rule: every field emitted in the NHGFIB JSON must be in the leaf key,
 * else leaves differing only there would dedupe and one leaf's value
 * would be programmed for both. */

/* groups: fold sorted (child_hash, weight) pairs; weight is an edge
 * property of the parent, so it lives in the parent's encoding, and
 * member order from zebra never changes identity */
h(group) = H(level_tag, nhg_flags-subset,
             [ (h(child_i), weight_i as u16 BE) sorted by
               (h(child_i), weight_i) ],
             /* L-B only: */ resolved_via prefix, vrf_id)
```

`H` = SHA-256 truncated to 64 bits (`lib/sha256.h` — deterministic, no
seed). The hash is **internal only** (D2): on first sight of a hash the
plugin allocates a `uint32_t` dplane id from its allocator, and that id is
what appears in NHGFIB JSON, depends lists, and `RTA_NH_ID`. Identical
subtrees anywhere → identical hash → shared object:
100k BGP routes with the same received set share one L-A; two different
L-A sets sharing one BGP NH share that NH's L-B and its L-C children.

**Plugin state (per `fpm_nl_ctx`).**

```c
struct fpm_dplane_nhg {
    uint64_t hash;              /* internal dedupe key (Merkle) */
    uint32_t dplane_id;         /* wire id: NHGFIB id / depends / RTA_NH_ID */
    uint32_t refcount;          /* parent objects + routes */
    uint8_t  level;             /* L_A / L_B / L_C */
    uint32_t nhg_flags;         /* RECEIVED / RECURSIVE subset for JSON */
    struct nexthop *nh;         /* nexthop_dup of the defining nexthop
                                   (leaf fields, or recursive parent for L-B) */
    struct prefix resolved_prefix;  /* L-B: binding prefix */
    vrf_id_t vrf_id;
    uint32_t rib_nhg_ids[4];    /* last zebra NHG ids seen referencing this
                                   L-A (dplane_ctx_get_nhe_id) — show only */
    uint16_t num_children;
    struct { struct fpm_dplane_nhg *obj; uint16_t weight; } children[]; /* sorted by (obj->hash, weight) */
};
/* fnc->dplane_nhg_table    : hash -> fpm_dplane_nhg   (lib hash_create) */
/* fnc->dplane_nhg_by_id    : dplane_id -> fpm_dplane_nhg  (show/debug)  */
/* fnc->dplane_nhg_id_alloc : uint32 allocator (monotonic + free list)  */
/* fnc->route_nhg_map       : (table,afi,prefix) -> top fpm_dplane_nhg* */
```

**Object lifecycle — the central problem.** Route UPDATE events make deletion
tracking non-obvious: an update carries only the *new* tree, a delete carries
*no* tree at all, and `use_route_replace` (`dplane_fpm_sonic.c:3128`) means the
wire never shows an explicit old-route removal. The design therefore never
diffs old vs new trees. Two pieces of bookkeeping fully determine deletion:

1. `route_nhg_map : (table_id, afi, prefix) → top L-A object` — remembers,
   per route key, which L-A object the route currently references. This is
   the *only* memory of "old"; the old tree itself is never needed.
2. `refcount` on every object — held by (a) each route whose map entry points
   at an L-A, and (b) each parent object on each of its children (taken once
   at parent creation, released once at parent free).

An object is deleted exactly when its refcount reaches 0; there are no
timers, no GC pass, no tree diffing.

**Memory footprint of `route_nhg_map`.** One entry per installed route key:
1M routes → 1M entries at ~48–64B each (key = prefix ≤17B + table 4B +
afi 1B, value = pointer, plus hashtable overhead) ≈ **50–64MB** in the zebra
process. Accepted because (a) zebra's RIB itself is GB-scale at 1M routes and
fpmsyncd/redis hold per-route entries regardless, and (b) the map cannot be
replaced by ctx-carried old-tree info on Linux: `zd_old_ng` is populated only
under `#ifndef HAVE_NETLINK` (`zebra_dplane.c:5043-5047`) and only when
`old_re != re` — the common same-`re` re-resolution update never captures the
old tree. The DELETE ctx does carry the route's tree, but map lookup is used
there too: it is exact, while recomputing the hash assumes the delete-time
tree is bit-identical to the install-time one (and the upstream DELETE ctx
may not carry nexthops at all). `dplane_nhg_table` itself is small — it
scales with unique NHG objects (thousands), not routes.

**Creation.** Only inside `fpm_nhg_build()`, post-order DFS over
`dplane_ctx_get_ng(ctx)` on `DPLANE_OP_ROUTE_INSTALL`/`ROUTE_UPDATE`:

```c
static uint64_t fpm_nhg_build(struct fpm_nl_ctx *fnc,
                              const struct nexthop *chain, bool top,
                              struct fpm_msg_batch *staging)
{
    /* 1. per member: if RECURSIVE -> child = fpm_nhg_build(resolved chain)
     *                else        -> child = lookup_or_create_singleton(nh)
     * 2. sort (child->hash, weight); compute this level's Merkle hash
     * 3. obj = hash_lookup(fnc->dplane_nhg_table, hash);
     *    if (!obj) { create; obj->dplane_id = id_alloc(); refcount = 0;
     *                foreach child: fpm_nhg_ref(child);   // parent holds refs
     *                append RTM_NEWNHGFIB{id=obj->dplane_id, json} to staging; }
     * 4. return obj;   // caller refs the top-level object only
     */
}
```

A cache hit at any level (step 3) stops recursion for that subtree — no
message, no new refs below it (the existing object already holds its
children). So an update that only reorders members or touches one leaf
creates only the objects along the changed path.

**Deletion primitive.**

```c
static void fpm_nhg_unref(struct fpm_nl_ctx *fnc, struct fpm_dplane_nhg *obj,
                          struct fpm_msg_batch *staging)
{
    if (--obj->refcount > 0)
        return;
    append RTM_DELNHGFIB{id=obj->dplane_id} to staging; /* parent DEL first */
    for (i = 0; i < obj->num_children; i++)
        fpm_nhg_unref(fnc, obj->children[i].obj, staging);  /* then children */
    id_free(obj->dplane_id); hash_release + free(obj);
}
```

Parent-before-child DEL order mirrors define-before-reference on creation:
fpmsyncd never sees a live object whose depends have been deleted.

**Per-op handling in `fpm_nl_enqueue()` (new path).** All messages for one
ctx are staged, then flushed to `fnc->obuf` in this exact order:

```
ROUTE_INSTALL / ROUTE_UPDATE (key = table_id/afi/prefix from ctx):
    new_top = fpm_nhg_build(ctx nexthop tree, staging_new)
    encode route msg via netlink_route_multipath_msg_encode, then append
        RTA_NH_ID = new_top->dplane_id in the plugin buffer
        -> on encode failure: rollback (drop staging_new, unref any objects
           created in this build), keep map unchanged, log; nothing emitted
    record dplane_ctx_get_nhe_id(ctx) into new_top->rib_nhg_ids  (show, D13)
    old_top = route_nhg_map[key]        /* NULL if absent */
    route_nhg_map[key] = new_top; fpm_nhg_ref(new_top)
    if (old_top) fpm_nhg_unref(old_top, staging_del)
    flush: staging_new (NEWs, children-first)  ->  route msg  ->  staging_del (DELs)

ROUTE_DELETE (ctx may carry no nexthops -- the map supplies the object):
    encode RTM_DELROUTE
    old_top = route_nhg_map.pop(key)
    if (old_top) fpm_nhg_unref(old_top, staging_del)
    flush: route del msg  ->  staging_del
```

Why this order matters: DELs must follow the route message. On an update
where the prefix moves from NHG A to NHG B, fpmsyncd processes
`NEW(B-children) → NEW(B) → route-replace(P→B) → DEL(A...)`; at every point
each APPL_DB route references a live NHG. Emitting `DEL(A)` before the
replace would leave the still-installed route pointing at a deleted NHG.

**Update corner cases (all fall out of the two bookkeeping rules):**

- *Same tree re-sent* (unrelated route attr change, or zebra re-install after
  NHT re-eval with identical resolution): `new_top == old_top` → ref then
  unref nets to zero, staging_del empty → wire shows only the route replace.
  No NHG churn.
- *Partial overlap* (one BGP NH's resolution changed): new L-A and the
  changed L-B/L-C path are created; shared subtrees are cache hits. Unref of
  old L-A cascades, but shared children's refcounts stay >0 (the new parents
  hold them) → only the truly orphaned objects emit DEL.
- *INSTALL for an already-mapped prefix* (zebra implicit replace): identical
  to UPDATE — the map upsert is idempotent by construction.
- *Rapid flap A→B→A within one batch*: second transition finds A's objects
  either still alive (refcount) or re-creates them; stream order keeps every
  reference valid at its point in the stream.
- *Two routes sharing one L-A, one deleted*: unref drops L-A refcount 2→1;
  no DEL — exactly the shared-NHG semantics we want.

**What never triggers deletion**: zebra NHG dplane events (ignored on this
path), fpmsyncd-side binding repair/restore (invisible to the plugin), and
FPM disconnect — on reconnect (`fpm_connect` path) both tables are flushed
*without* emitting DELs, because fpmsyncd treats a new connection as full
resync and zebra's replay rebuilds everything.

**Invariant** (checkable in UT via captured message streams): at any point in
the FPM byte stream, every dplane id referenced by a route message or a
depends list points to an object with NEW emitted and no DEL emitted since.
Freed ids are not reused until their DEL has been flushed.

**JSON emit reuse**: `build_c_nexthopgroupfull_multi/_singleton` gain
tree-input variants taking `struct fpm_dplane_nhg *` instead of fork ctx
getters: `id`/`depends[]`/`nh_grp_full_list[]` come from `obj->dplane_id` and
`obj->children[]` (`nh_grp_full.id = child dplane id, weight, num_direct =
child's num_children`), nexthop-detail fields from `obj->nh`, and new
optional fields `resolved_prefix`/`vrf_id`. `dependents[]` is emitted empty —
fpmsyncd derives reverse edges from depends on registration.

### 4.3 fpmsyncd (sonic-swss) — existing ribfib_2 code reused (D12)

Unchanged (reused as-is):
- `onNextHopGroupFullMsg` (`routesync.cpp:3022`) → `NHGMgr::addNHGFull` /
  `delNHGFull` — plugin dplane ids land in `ribID` (`uint32_t`, no widening).
- `onRouteMsg` nhg_fib path: `rtnl_route_get_nh_id()` (`routesync.cpp:2676`)
  → `getRIBNHGEntryByRIBID` → ROUTE_TABLE `nexthop_group` field.
- `onSrv6VpnRouteMsg` and PIC-context creation via the RECEIVED flag.
- CONFIG_DB `nhg_fib` gate (`fpmsyncd.cpp:127`), WarmStartHelper (ROUTE_TABLE).

New (the only fpmsyncd delta):
- Parse optional `resolved_prefix`/`vrf_id` JSON fields into `RIBNHGEntry`.
- **Binding-table module** (§5) + repair/restore triggers hooked into route
  delete/add handling.
- Unknown `RTA_NH_ID` on a route in nhg_fib mode → protocol error → close FPM
  socket to force zebra full replay (§6).

### 4.4 sonic-fib / libnexthopgroup

**Unchanged.** No schema or capi change: the plugin emits only fields the
existing `NextHopGroupFull` schema already defines (D12).

## 5. Binding Table and Convergence — deferred to fpmsyncd

**Not implemented in the plugin (scope decision, 2026-07-31).** The plugin keeps
`resolved_prefix`/`resolved_via` as **internal L-B identity input only** (they
participate in the Merkle hash so a group is distinguished by what it resolved
through) and does not emit them, does not maintain a binding table, and does not
attempt NHG repair.

Why the plugin cannot do the repair with fpmsyncd unchanged: repair means
re-emitting the same dplane id with fewer members, and the existing
`RIBNHGEntry` path cannot represent a multi→single or multi→empty transition —
`m_resolvedGroup.size() <= 1` sets `m_is_single`, which makes
`needCreateSonicObject()` false so `updateExistingNHGFull` skips the write
(stale group keeps the dead nexthop); and an empty member list fails
`syncFvVector` with "unsupported address family 0" after `setEntry` has already
clobbered the entry, which can later leak an empty nexthop into ROUTE_TABLE.

Consequence: convergence after a resolving-prefix loss relies on zebra's own
re-resolution (today's behaviour), which produces new trees → new dplane
objects → normal NHGFIB + route updates. The PIC-core fast-repair optimisation,
its binding table, and the `repaired` state belong to a **follow-up fpmsyncd
change** that owns the member fixup end to end.

## 6. Warm Reboot and Error Handling

Warm reboot:
- Behavior is **parity with existing ribfib_2**: dplane ids are not stable
  across a zebra/plugin restart (same as zebra NHG ids today), so after warm
  restart the replay re-registers NHGs under new ids and ROUTE_TABLE entries
  are reconciled by `WarmStartHelper` (ROUTE_TABLE-only, as today,
  `onWarmStartEnd`, `routesync.cpp:3764`). Content-based sonic-ID
  preservation (matching restored APPL_DB json against re-registered json to
  avoid NHG churn) is noted as follow-up work, not in this change.

Error handling:
- **Route references an unknown dplane id** (desync): log + close FPM socket →
  zebra reconnects and replays everything (existing mechanism).
- **Merkle hash collision** (plugin-internal): on a hash hit, compare the
  candidate subtree against `obj->nh`/children before reuse — collision falls
  back to allocating a distinct object (hash chained), so correctness never
  depends on hash uniqueness.
- **Message size budget**: FPM `fpm_msg_hdr_t.msg_len` is uint16 → 64KB hard
  cap. Each NHGFIB message gets its own frame; size asserted at encode time.
- **Id allocator exhaustion** (uint32 private space): practically unreachable
  (scales with unique NHG objects); allocation failure logs and drops to
  protocol-error → resync.

## 7. GBrain 参考

无相关历史记录（本会话 GBrain MCP 未连接）。

## 8. 测试设计

### Test Framework Discovery

| Test Level | Framework | Repository Evidence | Test Location | Run Command | Environment / Dependencies |
|---|---|---|---|---|---|
| UT (FRR lib) | FRR make check (C unit tests + pytest driver) | `tests/lib/test_nexthop.c` (`main()`+assert, driven by `test_nexthop.py` frrtest wrapper) | `/Users/eddie/community/frr` `tests/lib/` | `make check` | build host |
| UT (FRR zebra E2E) | topotests (pytest/mininet) | upstream `tests/topotests/fpm_testing_topo1/` (uses `zebra/fpm_listener.c`), `zebra_nhg_check`, `zebra_recursive_nhg_installed` | `/Users/eddie/community/frr` `tests/topotests/` | `pytest` in topotest env | docker/mininet |
| UT (fpmsyncd) | gtest mock_tests | `sonic-swss/tests/mock_tests/fpmsyncd/{nhgmgr_ut.cpp,test_routesync.cpp,test_fpmlink.cpp,ut_helpers_fpmsyncd.*}` | same directory | mock_tests gtest binary | build host |
| UT (dplane_fpm_sonic) | none — single `.c` in sonic-buildimage without a harness | `src/sonic-frr/dplane_fpm_sonic/` | — | — | exception: covered indirectly via fpmsyncd decode UTs + upstream fpm topotest + on-device show commands (see table below) |
| IT | — (implementation **Deferred**, see §9) | — | — | — | — |

### Unit Test Design — function coverage

| New / Modified Function | Change | Test Scenarios | Framework / Test File | Coverage Status |
|---|---|---|---|---|
| PR #19252 fields (`get_resolving_info()`, `nexthop_copy_no_recurse` resolved fields) | cherry-pick | recursive NH gets resolving prefix + via id; copy/dup with `resolved_addr/len` set and unset; upstream regressions green | upstream `tests/lib/test_nexthop.c` (add cases) + topotests `zebra_nhg_check`/`zebra_recursive_nhg_installed`/`fpm_testing_topo1`; `make check` | Covered |
| `fpm use-nhg-fib` vty + mode gating (dplane_fpm_sonic) | new | set/unset persists in config write; mutual exclusion with `use-next-hop-groups`; legacy modes byte-identical | Exception: no UT harness in sonic-buildimage; verified via fpm topotest run of the plugin + config save/restore on device | Exception explained |
| Derivation engine: `fpm_nhg_build`/`fpm_nhg_unref`/id allocator/`route_nhg_map` (dplane_fpm_sonic) | new | Merkle hash stability; NHGFIB-once per object; DELNHGFIB at refcount 0; children-before-parent NEW order; update old/new swap; reconnect flush | Exception: no UT harness in sonic-buildimage; verified via fpmsyncd decode UTs on captured message streams (ut_helpers_fpmsyncd) + fpm topotest | Exception explained |
| `RTA_NH_ID` plugin-side append after route encode (dplane_fpm_sonic) | new | attr present with top dplane id; nlmsg_len consistent; libnl parse on fpmsyncd side | fpmsyncd gtest `test_routesync.cpp` on captured route messages | Covered |
| `show fpm nhg-fib` + counters (dplane_fpm_sonic) | new | table dump matches injected state; rib→dplane mapping shows recorded zebra NHG ids; json form valid | Exception: no UT harness; verified on device (vtysh) in deploy_verify | Exception explained |
| `NHGMgr::addNHGFull`/`delNHGFull` with plugin ids | reused (regression) | existing `nhgmgr_ut.cpp` suite green unchanged; new case: `resolved_prefix`/`vrf_id` JSON fields parsed into `RIBNHGEntry` | gtest `tests_fpmsyncd` (`tests/mock_tests`) | Covered |
| Binding table insert/lookup/remove + `RTM_DELROUTE` repair / `RTM_NEWROUTE` restore | new | registration populates bindings; delete rewrites NHG members + sets `repaired`; add restores + clears; no-match no-op; cleanup at NHG delete | gtest new `nhgbinding_ut.cpp` in `tests/mock_tests/fpmsyncd/` | Covered |
| Unknown `RTA_NH_ID` protocol error path | new | unknown id → socket close/resync request; known id → normal | gtest `test_routesync.cpp` | Covered |

### Integration Test Design Decision

IT case design: **Requested by user and completed below.**
IT implementation: **Deferred by user** (no test repository confirmed). See §9.

### Integration Test Design

#### Test Point Coverage Matrix

| Test Point ID | Test Point | Risk / Scenario | Covered By | Status |
|---|---|---|---|---|
| TP-001 | Route-event-derived NHG build: recursive BGP routes → NEXTHOP_GROUP_TABLE/PIC_CONTEXT_TABLE/ROUTE_TABLE correct; no RTM_NEWNEXTHOP; PIC events (if present) ignored by new path; NHGFIB derived from route ctx only | Core pipeline broken → no routes programmed | TC-NHGROUTE-FUNC-001 | Covered |
| TP-002 | Dedupe: N routes sharing one NHG → one NHG object; RTM_NEWNHGFIB sent once, routes carry RTA_NH_ID afterwards | Duplicate NHGs / message bloat | TC-NHGROUTE-FUNC-001 | Covered |
| TP-003 | Resolving-prefix delete → binding lookup → NHG member repair before routing re-convergence | PIC-core repair fails, blackhole window | TC-NHGROUTE-CONV-001 | Covered |
| TP-004 | Refcount lifecycle: withdraw all → NHG/PIC/bindings removed; re-add → tree re-sent, objects recreated | Leaks or stale APPL_DB entries | TC-NHGROUTE-FUNC-001 | Covered |
| TP-005 | Re-resolution: after IGP change routes repoint to a new dplane NHG, old NHG drains via DELNHGFIB | Stale members / drain failure | TC-NHGROUTE-CONV-001 | Covered |
| TP-006 | FPM reconnect: fpmsyncd restart → zebra full replay rebuilds identical state | Desync after replay | TC-NHGROUTE-RESYNC-001 | Covered |
| TP-007 | Warm reboot: replay re-registers NHGs; ROUTE_TABLE reconciled by WarmStartHelper; behavior parity with existing ribfib_2 (sonic id churn accepted) | Warm reboot regression | TC-NHGROUTE-WARM-001 | Covered |
| TP-008 | Scale: 512-ECMP tree within FPM framing; 100k routes sharing NHGs stable | 64KB msg cap / memory / CPU | TC-NHGROUTE-SCALE-001 | Covered |
| TP-009 | Unknown-RTA_NH_ID desync → forced resync | Cache desync | — | Out of scope: needs fault injection inside the plugin; covered by fpmsyncd UT |

#### Integration Test Case Minimization Summary

| Test Points | Test Cases | Merged Test-Point Groups | Cases Kept Separate and Why |
|---|---|---|---|
| 9 (8 in scope) | 5 | TP-001+TP-002+TP-004 → FUNC-001 (one add/verify/withdraw/re-add lifecycle flow); TP-003+TP-005 → CONV-001 (one delete-triggered convergence flow) | RESYNC-001 (process restart, destructive); WARM-001 (warm-reboot state, destructive + special config); SCALE-001 (needs scale topology/route generator) |

#### TC-NHGROUTE-FUNC-001

1. **用例编号** `TC-NHGROUTE-FUNC-001`
2. **测试名称** 路由事件重建 NHG 的安装、去重与生命周期回收
3. **测试目的** TP-001（仅凭路由事件派生的 NHGFIB+路由消息正确生成 NHG/PIC/ROUTE 表项，无 RTM_NEWNEXTHOP，PIC 事件被新路径忽略）、TP-002（多路由共享单一 NHG 对象、RTM_NEWNHGFIB 仅发送一次）、TP-004（引用计数归零回收与重建）
4. **前置条件** DUT 运行含本设计的 FRR（upstream+PR#19252）/dplane_fpm_sonic/fpmsyncd；BGP 邻居经 IGP 递归解析；插件配置 `fpm use-nhg-fib`；可注入 ≥100 条共享同一组 BGP NH 的路由
5. **测试步骤**
   1. 建立 IGP 与 BGP 会话，注入 100 条共享同一 BGP NH 集合的前缀
   2. 检查 APPL_DB 的 NEXTHOP_GROUP_TABLE、PIC_CONTEXT_TABLE、ROUTE_TABLE
   3. 统计 FPM 消息（fpmsyncd 计数/日志）中 RTM_NEWNEXTHOP 与 RTM_NEWNHGFIB 出现次数
   4. 撤销全部 100 条路由，检查 APPL_DB
   5. 重新注入其中 1 条路由，检查 APPL_DB 与 RTM_NEWNHGFIB 重发
6. **预期结果**
   1. 会话建立，路由学到
   2. 仅 1 个 NHG 对象（含正确 resolved 成员与 PIC context），100 条 ROUTE_TABLE 项引用同一 sonic NHG id
   3. FPM 无 RTM_NEWNEXTHOP；RTM_NEWNHGFIB（id=dplane id）恰好 1 次，路由消息携带 RTA_NH_ID=顶层 dplane id
   4. 插件发出 RTM_DELNHGFIB，NHG/PIC 表项与绑定关系全部删除，无残留
   5. RTM_NEWNHGFIB 重新发送 1 次，NHG/PIC 对象重建，路由正确引用

#### TC-NHGROUTE-CONV-001

1. **用例编号** `TC-NHGROUTE-CONV-001`
2. **测试名称** 解析前缀删除触发 NHG 快速修复与重新收敛
3. **测试目的** TP-003（前缀删除经绑定表触发成员修复，先于协议收敛）、TP-005（重新解析后路由指向新 dplane NHG，旧 NHG 排空）
4. **前置条件** 同 FUNC-001 完成注入；BGP NH 经两条 IGP 路径解析（存在备用路径），已记录当前 NHG 的 sonic id 与成员
5. **测试步骤**
   1. 使其中一个 BGP NH 的解析前缀（IGP 路由）被删除（如关闭对应 IGP 邻居接口）
   2. 立即（协议重收敛前窗口内）检查 NEXTHOP_GROUP_TABLE 成员与数据面丢包
   3. 等待 zebra/BGP 重新收敛，检查 ROUTE_TABLE 引用与新旧 NHG 对象
6. **预期结果**
   1. IGP 路由删除事件到达 fpmsyncd
   2. 受影响 NHG 的死亡成员被移除（表项更新时间早于路由重发），流量切换到存活成员，丢包窗口远小于逐路由收敛
   3. 路由指向新 dplane NHG；旧 NHG 引用计数归零后 DELNHGFIB 删除，绑定表同步清理

#### TC-NHGROUTE-RESYNC-001

1. **用例编号** `TC-NHGROUTE-RESYNC-001`
2. **测试名称** FPM 重连全量重放一致性
3. **测试目的** TP-006（fpmsyncd 重启后 zebra 重放，重建状态与重启前一致）
4. **前置条件** FUNC-001 状态就绪（100 条路由 + 1 个共享 NHG）；记录 APPL_DB 快照
5. **测试步骤**
   1. 重启 fpmsyncd 进程（非 warm-restart 模式）
   2. 等待 FPM 重连与 zebra 全量重放完成
   3. 比对 APPL_DB（ROUTE/NHG/PIC 表）与重启前快照；检查 RTM_NEWNHGFIB 重发计数
6. **预期结果**
   1. FPM 连接断开后自动重连
   2. 重放完成，无错误日志（无路由引用未知 dplane id 的错误）
   3. 表内容语义一致（dplane/sonic id 允许变化但引用关系正确），RTM_NEWNHGFIB 每唯一 NHG 恰好重发 1 次

#### TC-NHGROUTE-WARM-001

1. **用例编号** `TC-NHGROUTE-WARM-001`
2. **测试名称** Warm reboot 下 NHG 状态恢复无扰动
3. **测试目的** TP-007（warm restart 重放重新注册 NHG，ROUTE_TABLE 经 WarmStartHelper 对账，行为与既有 ribfib_2 持平）
4. **前置条件** FUNC-001 状态就绪；warm-restart 已启用（bgp docker）；可观测 orchagent/SAI 操作计数
5. **测试步骤**
   1. 触发 bgp 容器 warm restart（fpmsyncd 随之 warm 启动）
   2. 恢复期间与 EOIU 后检查 APPL_DB 与 orchagent 操作
   3. 恢复完成后删除一个解析前缀，验证绑定表功能
6. **预期结果**
   1. warm restart 正常完成
   2. ROUTE_TABLE 经对账无闪断；NHG 重注册行为与既有 ribfib_2 基线一致，无错误日志
   3. 修复触发仍然生效（绑定表已恢复）

#### TC-NHGROUTE-SCALE-001

1. **用例编号** `TC-NHGROUTE-SCALE-001`
2. **测试名称** 512 ECMP 与 10 万路由规模稳定性
3. **测试目的** TP-008（512 成员树消息不超过 FPM 帧上限；大规模共享 NHG 下内存/CPU/收敛可接受）
4. **前置条件** 支持 512 ECMP 的拓扑或路由生成器；DUT 可注入 100k 前缀共享少量 NHG
5. **测试步骤**
   1. 构造 512 个 resolved 成员的递归 NHG，注入引用它的路由
   2. 注入 100k 前缀（共享若干 NHG），记录 fpmsyncd CPU/内存与安装完成时间
   3. 删除一个解析前缀，测量修复完成时间
6. **预期结果**
   1. RTM_NEWNHGFIB 编码成功（小于 64KB 帧上限），无截断/错误日志，表项正确
   2. 全部路由安装成功，资源占用与基线（NHG 事件方案）相当或更优
   3. 修复时间与路由规模无关（单次 NHG 更新完成修复）

## 9. Integration Test Implementation Authorization

- Repository: not confirmed (user chose to defer)
- Target branch: not confirmed
- Target test files: not confirmed
- Testcase names / registration: designed above (TC-NHGROUTE-*), not registered anywhere
- Code location confirmation: not obtained
- Exploration evidence: no IT repository was explored (deferral chosen before exploration)
- Proposal: none — to be produced when a repository is confirmed
- Status: `Deferred`
- Approval record: user selected "Defer IT implementation" when asked where the 5 IT cases should be implemented (2026-07-29 brainstorming session)

`/alinos.writing-plans` must NOT create repository-specific IT code tasks; the
approved case design above is retained for future implementation.

## Grill-Me 审查记录

**审查时间：** 2026-07-30
**审查目标：** docs/alinos/specs/2026-07-29-route-event-nhg-rebuild-design.md

### 关键决策
- **范围统一**：hash+tree 方案适用于**所有**路由（含非递归简单路由），fpmsyncd 单一解析路径；不保留 legacy RTA_MULTIPATH 回退路径。
- **绑定表双向触发**（修订 D7）：解析前缀 delete 触发修复（摘除死成员）；解析前缀 **add** 触发恢复（从存储的 tree json 还原成员）；NHG 条目增加 "repaired" 标志，恢复触发或新 hash 引用时清除。
- **绑定覆盖全部递归层级**：树中任意深度的 RECURSIVE 节点的解析前缀均登记（如 BGP→static→IGP 两级场景）；绑定值标识树内节点路径，修复摘除该节点的叶子子树。
- **树独立消息承载**（修订 §3.1）：新增私有消息 `RTM_NEWNHGTREE {hash, tree JSON}`，在首个引用该 hash 的路由之前于同一 TCP 流发送（流序保证 tree-before-reference）；路由消息仅携带 `RTA_FPM_NHG_HASH`。理由：nl_buf 64KB（dplane_fpm_sonic.c:3084）、FPM msg_len uint16、netlink rta_len uint16 三重上限与路由其他属性共享，512 成员树内联有溢出风险。编码时仍需 size assert。
- **Warm reboot 修订 §6**：不将 WarmStartHelper 扩展到 NHG/PIC 表；启动时从 APPL_DB 恢复 hash→sonicID 与绑定表，NHG/PIC 保持幂等直写（同 hash → 同 sonicID → 内容一致无扰动），EOIU 后引用计数清扫未被重新引用的 NHG；仅 ROUTE_TABLE 走 WarmStartHelper（维持现状）。
- **PIC 事件一并取消**：`DPLANE_OP_PIC_CONTEXT_*` 不再发往 FPM，PIC context 仅由树的顶层 received 成员推导（`checkNeedCreateSonicPICObj` 逻辑改为树输入）；SRv6 VPN 路由消息以单一 `FPM_ROUTE_ENCAP_SRV6_NHG_HASH` 属性替换 `NH_RECEIVED_ID`/`NH_ID` 两个 zebra id 属性；zebra 侧 PIC-context dplane op 相关管道随 NHG 事件管道一并移除。

### 发现的问题
- [x] §3.1 需修订：`RTA_FPM_NHG_TREE` 内联属性改为独立 `RTM_NEWNHGTREE` 消息（私有 5000 系列风格编号）。
- [x] D7/§5 需修订：delete-only 触发改为双向触发（delete=修复，add=恢复），并补充 "repaired" 标志语义；原设计下 IGP 闪断会导致共享 NHG 永久降级（静默转发能力损失）。
- [x] §5 绑定值 "top-level member index" 措辞隐含单层假设，需改为任意深度节点标识。
- [x] §6 warm reboot "WarmStartHelper reconciliation extended to NHG tables" 与结论矛盾，需改为幂等直写 + EOIU 后清扫。
- [x] §4.1 FRR 变更清单需补充：移除 zebra 侧 PIC-context dplane 事件生成管道。
- [x] 树 JSON 每节点需携带 vrf_id（绑定表按 (vrf, afi, prefix) 键控，解析路由位于 nexthop 的 vrf）。
- [x] fpmsyncd 需自维护 prefix→hash 映射：route update 换 hash 时对旧 hash 减引用（delete 消息不携带 hash）。

### 设计确认
- 统一 hash+tree、内容寻址 + fpmsyncd 持有 ID/状态（方案 C）经受住压力测试。
- 插件 prefix→hash 引用计数缓存（软状态，重连清零）与 fpmsyncd 镜像引用计数的驱逐协议自洽：归零驱逐后复用会重发树，无失配窗口。
- 64 位规范化哈希 + 树到达时 json 比对的碰撞检测方案成立。
- hash-without-tree 断连重放恢复机制复用既有 FPM 重连全量重放，成立。
- 内核 NHG 事件不受影响；FPM 侧删除 NHG/PIC 事件后 zebra 可移除 skip-kernel、`INSTALLED_FPM_ONLY`、`nh_grp_full/depends/dependents` 等管道，方向确认。

### 修订（2026-07-30，用户更正）
- **D1 更正**：保留 `DPLANE_OP_NH_*`（为既有 NHG 用户保持 backwalk 兼容），仅移除 `DPLANE_OP_PIC_CONTEXT_*`。新部署不设 `fpm use-next-hop-groups`（`fnc->use_nhg`），借既有门控跳过 NHG 事件；zebra 侧 skip-kernel / `INSTALLED_FPM_ONLY` / `nh_grp_full` 等管道**保留不删**（上文"可移除"结论作废）。
- **新增 D10**：路由更新处理以 `--nhg-fib`（`zebra_nhg_fib_enabled`）选择新树路径；legacy 部署（`use_nhg` 置位且无 `--nhg-fib`）行为与今天完全一致。
- **D1 再更正（PIC 范围）**：`DPLANE_OP_PIC_CONTEXT_*` 在本变更中**保留不删**，其移除作为独立清理 PR 处理；新路径下 fpmsyncd 忽略 PIC 事件、仅从树推导 PIC context。
- **D4/D5 更正**：不新增 `RTM_NEWNHGTREE`，**复用 `RTM_NEWNHGFIB`/`RTM_DELNHGFIB`**。dplane 插件维护由路由 ctx 树派生的 dplane NHG 集合：首次引用发 NEWNHGFIB（先于首条引用路由），引用归零发 DELNHGFIB。fpmsyncd 生命周期改为**插件驱动**（复用 `onNextHopGroupFullMsg` 路径，无需镜像引用计数）。IT 用例中 "树 TLV" 断言已同步改为 NEWNHGFIB/DELNHGFIB 断言。

### 修订（2026-07-30，二次基线切换：upstream FRR）
- **D11 新增**：FRR 基线改为 upstream `/Users/eddie/community/frr` master + PR #19252（即 ribfib_2 resolved-via WIP 的上游版本），其余 ribfib_2 zebra 改动全部放弃；插件移除对 fork-only zebra API 的依赖（PIC ops、nhe_received、nh_grp_full getters）。
- **D2/D4 更正**：Merkle hash 仅作插件内部去重键；线上 id 为**插件分配的 uint32 dplane NHG id**，经既有 `RTA_NH_ID` 与 NHGFIB JSON id 字段传递 → fpmsyncd `ribID` 无需拓宽为 64 位，既有解析路径字节兼容。
- **D10 更正**：nhg_fib 开关从 zebra `--nhg-fib` 移入插件 vty `fpm use-nhg-fib`（仿 `use-next-hop-groups`），与 `use-next-hop-groups` 互斥。
- **D12 新增**：fpmsyncd 复用既有 ribfib_2 代码；增量仅限绑定表模块、JSON 可选字段 `resolved_prefix`/`vrf_id` 解析、未知 id 协议错误路径。schema 扩展落在 sonic-fib（libnexthopgroup）。
- **D13 新增**：插件新增 `show fpm nhg-fib`（dplane NHG 表 + rib→dplane NHG 映射，记录引用路由事件上的 `dplane_ctx_get_nhe_id`）与 `show fpm status` 计数。
- **§6 更正**：warm reboot 目标改为与既有 ribfib_2 行为持平（id 跨重启不稳定，与 zebra NHG id 一致）；基于内容匹配保留 sonic id 列为后续工作。
