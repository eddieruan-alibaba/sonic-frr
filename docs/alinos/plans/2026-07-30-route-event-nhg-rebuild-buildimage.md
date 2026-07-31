# Route-Event-Based NHG Rebuild — sonic-buildimage Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** In sonic-buildimage, move NHG derivation out of forked zebra and into the `dplane_fpm_sonic` plugin: rebase the FRR package onto upstream FRR + PR #19252, and have the plugin derive deduplicated dplane NHG objects from route dplane ctx trees, emitting them over the existing `RTM_NEWNHGFIB`/`RTM_DELNHGFIB` + `RTA_NH_ID` wire so fpmsyncd is reused unchanged.

**Architecture:** Stock `dplane_ctx_route_init()` deep-copies the full recursive nexthop tree; PR #19252 adds `resolved_via` + resolving prefix per nexthop. The plugin decomposes each tree into a 3-level object hierarchy (L-A received group → L-B per-recursive-NH resolved group → L-C leaf singletons), dedupes objects by a Merkle hash, allocates a `uint32_t` dplane id per object, and drives object lifetime purely from a per-route map plus refcounts. NHGFIB messages are emitted children-before-parent ahead of the referencing route; DELs follow the route. A new `fpm use-nhg-fib` vty command replaces the fork's zebra `--nhg-fib` global.

**Tech Stack:** C (FRR dplane plugin module, FRR `lib` hash/prefix/sha256 APIs, libnl-style netlink encoding), JSON Schema + Jinja2 codegen (`sonic-fib`/libnexthopgroup), quilt/stg patch series (`src/sonic-frr/patch`), automake, sonic-buildimage `make target/debs/...`.

**Scope:** sonic-buildimage only — `src/sonic-frr` (FRR base + patch series + `dplane_fpm_sonic`) and `src/libraries/sonic-fib`. The fpmsyncd delta (binding table, `resolved_prefix` parsing, unknown-id error path) is a **separate follow-up plan** in sonic-swss; this plan's device verification therefore validates plugin-side behavior and existing-fpmsyncd compatibility, not the new convergence trigger.

Design source of truth: `docs/alinos/specs/2026-07-29-route-event-nhg-rebuild-design.md`
(decisions D1–D13; code-level derivation detail in §4.2.1).

---

## File Structure

**`/Users/eddie/community/frr`** (new FRR base repo, branch `sonic-nhgfib-base`)
- Create branch off upstream `master`; cherry-pick PR #19252 (3 commits worth of change across `lib/nexthop.[ch]`, `zebra/zebra_nhg.c`, `zebra/zebra_vty.c`, `zebra/kernel_netlink.h`, `zebra/rt_netlink.c`).
- Export as patch file(s) for the buildimage patch series.

**`sonic-buildimage/rules/frr.mk`**
- Modify: `FRR_VERSION`/`FRR_TAG`/`FRR_BRANCH` to the new upstream base.

**`sonic-buildimage/src/sonic-frr/patch/`**
- Create: `0100-Upstream-PR19252-store-resolving-nexthop-group-id.patch`
- Modify: `series` (append the new patch)
- Audit: `0012-SONiC-ONLY-build-dplane-fpm-sonic-module.patch` still applies on the new base.

**`sonic-buildimage/src/sonic-frr/dplane_fpm_sonic/`**
- Create: `fpm_nhg.h` — dplane NHG object model, table/allocator/map API (new, self-contained).
- Create: `fpm_nhg.c` — hash, id allocator, object table, `fpm_nhg_build`/`ref`/`unref`, show helpers.
- Modify: `dplane_fpm_sonic.c` — `use_nhg_fib` config + vty, fork-API removal, route-op integration, `RTA_NH_ID` append, JSON emit from objects, show commands, reconnect flush.

**`sonic-buildimage/src/sonic-frr/patch/0012-...-build-dplane-fpm-sonic-module.patch`**
- Modify: add `fpm_nhg.c` to `zebra_dplane_fpm_sonic_la_SOURCES`.

**`sonic-buildimage/src/sonic-frr/Makefile`**
- Modify: `DPLANE_FPM_SONIC_MODULE` copy step must copy the new files too.

**`sonic-buildimage/src/libraries/sonic-fib/schema/NextHopGroupFull.json`**
- Modify: add optional `resolved_addr` (ip_address) + `resolved_len` (integer). `vrf_id` already exists (line 74) — no change needed.

---

## Test Implementation Plan

### Approved test design status

From the design spec `## 8. 测试设计`:

- UT (FRR lib): upstream `make check`, `tests/lib/test_nexthop.c` — **in `/Users/eddie/community/frr`**, covered by Task 2.
- UT (FRR zebra E2E): upstream topotests `fpm_testing_topo1`, `zebra_nhg_check`, `zebra_recursive_nhg_installed` — covered by Task 2/Task 11.
- UT (dplane_fpm_sonic): **recorded exception** — "none — single `.c` in sonic-buildimage without a harness"; covered indirectly via fpmsyncd decode UTs on captured message streams, upstream fpm topotest, and on-device `show fpm nhg-fib`.
- UT (fpmsyncd): `tests_fpmsyncd` gtest — **sonic-swss, out of this plan's scope** (follow-up plan).

Because the plugin has no unit-test harness (approved exception), plugin tasks use
**verification-first steps**: capture the expected FPM message stream / show output
first (via `fpm_listener` or the plugin's own log), then implement until observed
output matches. TDD Red-Green applies literally only to Task 2 (FRR lib UT).

> Deviation flag for the reviewer: adding a host-side unit harness for the pure
> logic in `fpm_nhg.c` (hash canonicalization, id allocator, refcount cascade)
> would be a *new* framework adoption not present in the approved design. It is
> **not** included here; see "Follow-up" at the end.

### Integration Test Implementation Authorization (copied verbatim from the design)

```
- Repository: not confirmed (user chose to defer)
- Target branch: not confirmed
- Target test files: not confirmed
- Testcase names / registration: designed above (TC-NHGROUTE-*), not registered anywhere
- Code location confirmation: not obtained
- Exploration evidence: no IT repository was explored (deferral chosen before exploration)
- Proposal: none — to be produced when a repository is confirmed
- Status: `Deferred`
- Approval record: user selected "Defer IT implementation" when asked where the 5 IT cases should be implemented (2026-07-29 brainstorming session)
```

Status is `Deferred` → **this plan creates no integration-test code tasks.** The
approved TC-NHGROUTE-FUNC/CONV/RESYNC/WARM/SCALE-001 case design is retained in
the design spec for future implementation.

---

## Task 1: Create the FRR base branch with PR #19252

**Files:**
- Repo: `/Users/eddie/community/frr` (branch `sonic-nhgfib-base` off `origin/master`)
- Create: `/tmp/pr19252/*.patch` (exported patches)

- [ ] **Step 1: Fetch the PR and create the base branch**
  ```bash
  cd /Users/eddie/community/frr
  git fetch origin master
  git fetch origin pull/19252/head:pr19252
  git checkout -b sonic-nhgfib-base origin/master
  ```
- [ ] **Step 2: Inspect what PR #19252 contains**
  ```bash
  git log --oneline origin/master..pr19252
  git diff --stat origin/master...pr19252
  ```
  Expected files: `lib/nexthop.h`, `lib/nexthop.c`, `zebra/zebra_nhg.c`,
  `zebra/zebra_vty.c`, `zebra/kernel_netlink.h`, `zebra/rt_netlink.c`.
  **Stop and report** if `resolved_addr`/`resolved_len` are absent (the design
  requires both the id and the prefix — see spec §4.1 risk note).
- [ ] **Step 3: Cherry-pick onto the base branch**
  ```bash
  git cherry-pick $(git rev-list --reverse origin/master..pr19252)
  ```
  Resolve conflicts by keeping upstream master structure; do not add anything
  beyond the PR.
- [ ] **Step 4: Build to confirm the base compiles**
  ```bash
  ./bootstrap.sh && ./configure --enable-dev-build && make -j$(nproc) 2>&1 | tail -20
  ```
  Expect no errors.
- [ ] **Step 5: Export patches for the buildimage series**
  ```bash
  mkdir -p /tmp/pr19252
  git format-patch -o /tmp/pr19252 origin/master..HEAD
  ls /tmp/pr19252
  ```
- [ ] **Step 6: Commit nothing in buildimage yet; record the base commit SHA**
  ```bash
  git rev-parse origin/master; git rev-parse HEAD
  ```

## Task 2: Verify PR #19252 behavior with upstream unit tests

**Files:**
- Modify: `/Users/eddie/community/frr/tests/lib/test_nexthop.c`
- Test: same file (framework: `main()` + `assert()`, driven by `tests/lib/test_nexthop.py` frrtest wrapper printing `"Simple test passed."`)

- [ ] **Step 1: Write the failing test** — append to `test_nexthop.c` before `main()`:
  ```c
  static void test_resolved_via_copy(void)
  {
      struct nexthop *nh, *copy;
      struct in_addr via;

      nh = nexthop_from_ifindex(11, 0);
      nh->resolved_via = 4242;
      inet_pton(AF_INET, "10.0.0.1", &via);
      nh->resolved_addr.ipa_type = IPADDR_V4;
      nh->resolved_addr.ipaddr_v4 = via;
      nh->resolved_len = 32;

      copy = nexthop_dup(nh, NULL);
      assert(copy->resolved_via == 4242);
      assert(copy->resolved_len == 32);
      assert(copy->resolved_addr.ipaddr_v4.s_addr == via.s_addr);
      nexthop_free(copy);

      /* unset fields must copy as zero */
      nh->resolved_via = 0;
      nh->resolved_len = 0;
      memset(&nh->resolved_addr, 0, sizeof(nh->resolved_addr));
      copy = nexthop_dup(nh, NULL);
      assert(copy->resolved_via == 0);
      assert(copy->resolved_len == 0);
      nexthop_free(copy);

      nexthop_free(nh);
  }
  ```
  and call `test_resolved_via_copy();` from `main()` before the `printf`.
- [ ] **Step 2: Run it to make sure it fails** if the PR is missing the prefix fields
  ```bash
  cd /Users/eddie/community/frr && make tests/lib/test_nexthop && ./tests/lib/test_nexthop
  ```
  With PR #19252 applied this should already pass (the test guards the fields we
  depend on). If it fails to **compile**, the PR shape changed → stop and report.
- [ ] **Step 3: Run the full lib unit suite**
  ```bash
  make check TESTS='tests/lib/test_nexthop tests/lib/test_nexthop_iter' 2>&1 | tail -20
  ```
- [ ] **Step 4: Run the FPM/NHG topotests as regression**
  ```bash
  cd tests/topotests && sudo pytest fpm_testing_topo1 zebra_nhg_check zebra_recursive_nhg_installed 2>&1 | tail -20
  ```
  Expect all passed.
- [ ] **Step 5: Commit** (`test: cover resolved-via field copy in nexthop_dup`)

## Task 3: Point the buildimage FRR package at the new base

**Files:**
- Modify: `sonic-buildimage/rules/frr.mk:3-6`
- Create: `sonic-buildimage/src/sonic-frr/patch/0100-Upstream-PR19252-store-resolving-nexthop-group-id.patch`
- Modify: `sonic-buildimage/src/sonic-frr/patch/series`

- [ ] **Step 1: Copy the exported patch into the series directory**
  ```bash
  cd /Users/eddie/community/sonic-buildimage/src/sonic-frr
  cat /tmp/pr19252/*.patch > patch/0100-Upstream-PR19252-store-resolving-nexthop-group-id.patch
  ```
- [ ] **Step 2: Append it to `patch/series`** (last line):
  ```
  0100-Upstream-PR19252-store-resolving-nexthop-group-id.patch
  ```
- [ ] **Step 3: Update `rules/frr.mk`** — set the upstream base:
  ```make
  FRR_VERSION = 10.5.4
  FRR_SUBVERSION = 0
  FRR_TAG = <upstream tag/SHA recorded in Task 1 Step 6>
  FRR_BRANCH = sonic-nhgfib-base
  ```
- [ ] **Step 4: Dry-run the patch series against the new base**
  ```bash
  cd frr && git checkout sonic-nhgfib-base && \
    for p in $(grep -v '^#' ../patch/series); do \
      git apply --check ../patch/$p || echo "FAILS: $p"; done
  ```
- [ ] **Step 5: Fix or drop fork-only patches that no longer apply** — for each
  `FAILS:` entry, decide: rebase the patch, or drop it if the change is now
  upstream / belonged to the ribfib_2 NHG-event design (record the decision in
  the commit message). Re-run Step 4 until clean.
- [ ] **Step 6: Commit** (`frr: rebase package onto upstream base + PR 19252`)

## Task 4: Extend the NextHopGroupFull JSON schema

**Files:**
- Modify: `sonic-buildimage/src/libraries/sonic-fib/schema/NextHopGroupFull.json`

- [ ] **Step 1: Add the two optional fields** to `properties` (after `rmap_src`, before `nh_srv6`):
  ```json
      "resolved_addr": {
        "position" : 5,
        "$ref": "#/$defs/ip_address",
        "description": "Resolving route prefix address for a recursive group (optional)"
      },
      "resolved_len": {
        "type": "integer",
        "position" : 3,
        "minimum": 0,
        "maximum": 128,
        "default_value" : "0",
        "description": "Resolving route prefix length for a recursive group (optional)"
      },
  ```
  Do **not** add them to `required` — absence must stay valid so mixed
  component versions interoperate (design D12).
- [ ] **Step 2: Regenerate and inspect the generated code**
  ```bash
  cd /Users/eddie/community/sonic-buildimage/src/libraries/sonic-fib
  ./autogen.sh && ./configure && make -j$(nproc) 2>&1 | tail -20
  grep -n "resolved_addr\|resolved_len" src/c_nexthopgroupfull.h src/nexthopgroupfull.h
  ```
  Expect both fields present in the generated C struct and C++ class.
- [ ] **Step 3: Verify JSON round-trip tolerance** — parse a group JSON *without*
  the new fields using the generated `from_json` (use the library's existing
  `tests/` entry point if present, else a 15-line scratch program):
  ```bash
  ls tests/ && make check 2>&1 | tail -10
  ```
  Expect no failure on legacy JSON.
- [ ] **Step 4: Commit** (`sonic-fib: add optional resolved_addr/resolved_len to NextHopGroupFull`)

## Task 5: Add the `fpm use-nhg-fib` vty command

**Files:**
- Modify: `dplane_fpm_sonic/dplane_fpm_sonic.c` (new DEFUNs near `fpm_use_nhg_cmd:415`; `install_element` near `:4138`; init near `:4117`)

- [ ] **Step 1: Replace the zebra-global init** at `:4117`:
  ```c
  /* was: gfnc->use_nhg_fib = zebra_nhg_fib_enabled; */
  gfnc->use_nhg_fib = false;
  ```
- [ ] **Step 2: Add the DEFUNs** after `no_fpm_use_nhg_cmd` (`:445`):
  ```c
  DEFUN(fpm_use_nhg_fib, fpm_use_nhg_fib_cmd,
        "fpm use-nhg-fib",
        FPM_STR
        "Derive next hop groups from route events (RIB/FIB mode).\n")
  {
  	if (gfnc->use_nhg_fib)
  		return CMD_SUCCESS;

  	if (gfnc->use_nhg) {
  		vty_out(vty,
  			"%% cannot enable use-nhg-fib while use-next-hop-groups is set\n");
  		return CMD_WARNING_CONFIG_FAILED;
  	}

  	gfnc->use_nhg_fib = true;
  	/* Reconnect so the peer gets a clean, fully re-derived view. */
  	event_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
  			FNE_INTERNAL_RECONNECT, &gfnc->t_event);
  	return CMD_SUCCESS;
  }

  DEFUN(no_fpm_use_nhg_fib, no_fpm_use_nhg_fib_cmd,
        "no fpm use-nhg-fib",
        NO_STR
        FPM_STR
        "Derive next hop groups from route events (RIB/FIB mode).\n")
  {
  	if (!gfnc->use_nhg_fib)
  		return CMD_SUCCESS;

  	gfnc->use_nhg_fib = false;
  	event_add_event(gfnc->fthread->master, fpm_process_event, gfnc,
  			FNE_INTERNAL_RECONNECT, &gfnc->t_event);
  	return CMD_SUCCESS;
  }
  ```
- [ ] **Step 3: Guard the reverse direction** in `fpm_use_nhg` (`:419`), right
  after the "already enabled" check:
  ```c
  	if (gfnc->use_nhg_fib) {
  		vty_out(vty,
  			"%% cannot enable use-next-hop-groups while use-nhg-fib is set\n");
  		return CMD_WARNING_CONFIG_FAILED;
  	}
  ```
- [ ] **Step 4: Register and persist** — add to `fpm_nl_init`'s command
  installation block (near `:4138`):
  ```c
  	install_element(CONFIG_NODE, &fpm_use_nhg_fib_cmd);
  	install_element(CONFIG_NODE, &no_fpm_use_nhg_fib_cmd);
  ```
  and in the plugin's config-write function emit `fpm use-nhg-fib` when
  `gfnc->use_nhg_fib` is set (mirror how `use_nhg` is written).
- [ ] **Step 5: Verify vty behavior** (after Task 10 build, or with a local FRR build):
  `vtysh -c 'conf t' -c 'fpm use-nhg-fib'` → accepted; then
  `vtysh -c 'conf t' -c 'fpm use-next-hop-groups'` → must print the mutual
  exclusion warning; `show running-config | grep use-nhg-fib` → present.
- [ ] **Step 6: Commit** (`dplane_fpm_sonic: add fpm use-nhg-fib mode command`)

## Task 6: Add the dplane NHG object model, hash and id allocator

**Files:**
- Create: `dplane_fpm_sonic/fpm_nhg.h`
- Create: `dplane_fpm_sonic/fpm_nhg.c`
- Modify: `src/sonic-frr/patch/0012-SONiC-ONLY-build-dplane-fpm-sonic-module.patch` (add `fpm_nhg.c` to `zebra_dplane_fpm_sonic_la_SOURCES`)
- Modify: `src/sonic-frr/Makefile` (`DPLANE_FPM_SONIC_MODULE` copy list)

- [ ] **Step 1: Write `fpm_nhg.h`** — object model per spec §4.2.1:
  ```c
  #ifndef _FPM_NHG_H
  #define _FPM_NHG_H

  #include "lib/prefix.h"
  #include "lib/nexthop.h"
  #include "lib/hash.h"

  enum fpm_nhg_level { FPM_NHG_L_C = 0, FPM_NHG_L_B, FPM_NHG_L_A };

  #define FPM_NHG_RIB_ID_TRACK 4

  struct fpm_dplane_nhg;

  struct fpm_nhg_child {
  	struct fpm_dplane_nhg *obj;
  	uint8_t weight;
  };

  struct fpm_dplane_nhg {
  	uint64_t hash;        /* internal Merkle dedupe key */
  	uint32_t dplane_id;   /* wire id: NHGFIB id / depends / RTA_NH_ID */
  	uint32_t refcount;    /* parents + routes */
  	uint8_t level;        /* enum fpm_nhg_level */
  	uint32_t nhg_flags;   /* RECEIVED / RECURSIVE subset for JSON */
  	struct nexthop *nh;   /* defining nexthop (dup'd) */
  	struct prefix resolved_prefix; /* L-B only */
  	vrf_id_t vrf_id;
  	uint32_t rib_nhg_ids[FPM_NHG_RIB_ID_TRACK]; /* show only */
  	uint8_t rib_nhg_id_count;
  	uint16_t num_children;
  	struct fpm_nhg_child *children; /* sorted by obj->hash */
  };

  struct fpm_nhg_tables {
  	struct hash *by_hash;
  	struct hash *by_id;
  	struct hash *route_map;
  	uint32_t next_id;
  	uint32_t *free_ids;
  	uint32_t free_id_count, free_id_cap;
  	/* counters */
  	uint64_t obj_created, obj_deleted, nhgfib_sent, dedupe_hits;
  };

  void fpm_nhg_tables_init(struct fpm_nhg_tables *t);
  void fpm_nhg_tables_flush(struct fpm_nhg_tables *t);
  uint32_t fpm_nhg_id_alloc(struct fpm_nhg_tables *t);
  void fpm_nhg_id_free(struct fpm_nhg_tables *t, uint32_t id);
  uint64_t fpm_nhg_hash_leaf(const struct nexthop *nh);
  uint64_t fpm_nhg_hash_group(uint8_t level, uint32_t nhg_flags,
  			    const struct fpm_nhg_child *children,
  			    uint16_t count, const struct prefix *resolved);
  #endif
  ```
- [ ] **Step 2: Implement hashing in `fpm_nhg.c`** — canonical encode + SHA-256
  truncated to 64 bits (`lib/sha256.h`: `SHA256_Init/Update/Final`):
  ```c
  static uint64_t fpm_nhg_digest(const void *buf, size_t len)
  {
  	unsigned char d[32];
  	SHA256_CTX ctx;
  	uint64_t out = 0;

  	SHA256_Init(&ctx);
  	SHA256_Update(&ctx, buf, len);
  	SHA256_Final(d, &ctx);
  	for (int i = 0; i < 8; i++)
  		out = (out << 8) | d[i];
  	return out;
  }

  uint64_t fpm_nhg_hash_leaf(const struct nexthop *nh)
  {
  	struct { vrf_id_t vrf; uint8_t type, bh; int32_t ifindex;
  		 union g_addr gate; uint8_t label_type, nlabels;
  		 mpls_label_t labels[MPLS_MAX_LABELS]; uint8_t nseg;
  		 struct in6_addr segs[SRV6_MAX_SIDS]; } k;

  	memset(&k, 0, sizeof(k));           /* deterministic padding */
  	k.vrf = nh->vrf_id;
  	k.type = nh->type;
  	k.ifindex = nh->ifindex;
  	if (nh->type == NEXTHOP_TYPE_BLACKHOLE)
  		k.bh = nh->bh_type;
  	else
  		k.gate = nh->gate;
  	k.label_type = nh->nh_label_type;
  	if (nh->nh_label) {
  		k.nlabels = nh->nh_label->num_labels;
  		memcpy(k.labels, nh->nh_label->label,
  		       k.nlabels * sizeof(mpls_label_t));
  	}
  	if (nh->nh_srv6 && nh->nh_srv6->seg6_segs) {
  		k.nseg = nh->nh_srv6->seg6_segs->num_segs;
  		memcpy(k.segs, nh->nh_srv6->seg6_segs->seg,
  		       k.nseg * sizeof(struct in6_addr));
  	}
  	return fpm_nhg_digest(&k, sizeof(k));
  }
  ```
  `fpm_nhg_hash_group()` folds `level`, the RECEIVED/RECURSIVE flag subset, each
  `(child->hash, weight)` pair **in sorted order**, and (L-B only) the resolving
  prefix + vrf into one buffer, then calls `fpm_nhg_digest()`. `memset` before
  fill is mandatory — struct padding must not leak into the digest.
- [ ] **Step 3: Implement the id allocator** — monotonic `next_id` starting at 1
  with a LIFO free list; `fpm_nhg_id_free()` pushes, `fpm_nhg_id_alloc()` pops
  when non-empty. Never hand back an id whose DEL has not been flushed: the
  caller (Task 8) frees ids only after staging the DEL, and staging is flushed
  before the next ctx is processed.
- [ ] **Step 4: Implement `fpm_nhg_tables_init/flush`** — three `hash_create()`
  tables (`by_hash` keyed on `hash`, `by_id` keyed on `dplane_id`, `route_map`
  keyed on `(table_id, afi, prefix)`); `flush` frees all objects and resets
  counters + allocator without emitting any message (used on reconnect).
- [ ] **Step 5: Wire the build** — add `fpm_nhg.c` to
  `zebra_dplane_fpm_sonic_la_SOURCES` in patch `0012`, and extend the
  `DPLANE_FPM_SONIC_MODULE` copy in `src/sonic-frr/Makefile` so both
  `fpm_nhg.c` and `fpm_nhg.h` land in `frr/zebra/`.
- [ ] **Step 6: Compile-check the new files standalone**
  ```bash
  cd /Users/eddie/community/frr && cp <plugin files> zebra/ && \
    gcc -fsyntax-only -I. -Ilib -Izebra $(pkg-config --cflags libyang) zebra/fpm_nhg.c
  ```
  Expect no errors (functional verification comes in Task 10/11).
- [ ] **Step 7: Commit** (`dplane_fpm_sonic: add dplane NHG object model, hash and id allocator`)

## Task 7: Implement tree decomposition (`fpm_nhg_build`)

**Files:**
- Modify: `dplane_fpm_sonic/fpm_nhg.c`, `dplane_fpm_sonic/fpm_nhg.h`

- [ ] **Step 1: Add the staging + build API to `fpm_nhg.h`**
  ```c
  struct fpm_nhg_staging {
  	struct fpm_dplane_nhg **objs;  /* objects needing RTM_NEWNHGFIB */
  	uint16_t count, cap;
  };

  struct fpm_dplane_nhg *fpm_nhg_build(struct fpm_nhg_tables *t,
  				     const struct nexthop *chain,
  				     enum fpm_nhg_level level,
  				     struct fpm_nhg_staging *newq);
  void fpm_nhg_ref(struct fpm_dplane_nhg *obj);
  void fpm_nhg_rollback(struct fpm_nhg_tables *t, struct fpm_nhg_staging *newq);
  ```
- [ ] **Step 2: Implement post-order DFS `fpm_nhg_build()`** per spec §4.2.1:
  1. Walk `chain`; for each member `nh`:
     - `CHECK_FLAG(nh->flags, NEXTHOP_FLAG_RECURSIVE)` → `child =
       fpm_nhg_build(t, nh->resolved, FPM_NHG_L_B, newq)` and record
       `nh->resolved_addr/resolved_len` into the child's `resolved_prefix` +
       `vrf_id`, setting `NEXTHOP_GROUP_RECURSIVE` in the child's `nhg_flags`.
     - else → `child = lookup_or_create_singleton(t, nh, newq)` (level `L_C`).
  2. Collect `(child, nh->weight)` pairs, sort by `child->hash`.
  3. `nhg_flags = (level == FPM_NHG_L_A && any_recursive) ? NEXTHOP_GROUP_RECEIVED : 0`
     (plus RECURSIVE for L-B); `hash = fpm_nhg_hash_group(...)`.
  4. `obj = hash_lookup(t->by_hash, &hash)`; on hit → `t->dedupe_hits++`, return it.
  5. On miss → allocate object, `obj->dplane_id = fpm_nhg_id_alloc(t)`,
     `refcount = 0`, `nh = nexthop_dup(defining nh, NULL)`, `fpm_nhg_ref()`
     every child, insert into `by_hash` + `by_id`, push onto `newq`,
     `t->obj_created++`.
  6. Return `obj`.
  A single non-recursive member at top level degenerates to one `L_C` object
  (L-A == L-C) — do not synthesize an extra wrapper.
- [ ] **Step 3: Handle the hash-hit correctness check** — on a `by_hash` hit,
  compare `num_children` and each `(child->hash, weight)` (and `nh` basics via
  `nexthop_cmp_basic`) before reuse; on mismatch (collision) chain a distinct
  object under a perturbed key so correctness never depends on hash uniqueness
  (spec §6).
- [ ] **Step 4: Implement `fpm_nhg_rollback()`** — pop every object pushed onto
  `newq` in reverse, undo child refs, free ids, remove from both tables. Used
  when route encode fails (Task 8).
- [ ] **Step 5: Verify decomposition by log inspection** — add a
  `fib_frr_debug`-level log per created object (`level, id, hash, children`),
  then after Task 10's build, install one recursive BGP route on the device and
  confirm the log shows exactly `L_C(s) → L_B → L_A` in that order with the
  expected child ids.
- [ ] **Step 6: Commit** (`dplane_fpm_sonic: derive dplane NHG objects from route ctx trees`)

## Task 8: Implement lifecycle (`unref`, route map upsert, per-op integration)

**Files:**
- Modify: `dplane_fpm_sonic/fpm_nhg.c`, `dplane_fpm_sonic/fpm_nhg.h`
- Modify: `dplane_fpm_sonic/dplane_fpm_sonic.c` — `fpm_nl_enqueue` route ops (`:3139-3200`), `fpm_reconnect` (`:735-786`)

- [ ] **Step 1: Add lifecycle API to `fpm_nhg.h`**
  ```c
  void fpm_nhg_unref(struct fpm_nhg_tables *t, struct fpm_dplane_nhg *obj,
  		   struct fpm_nhg_staging *delq);
  struct fpm_dplane_nhg *fpm_nhg_route_get(struct fpm_nhg_tables *t,
  					 uint32_t table_id, uint8_t afi,
  					 const struct prefix *p);
  void fpm_nhg_route_set(struct fpm_nhg_tables *t, uint32_t table_id,
  			uint8_t afi, const struct prefix *p,
  			struct fpm_dplane_nhg *obj);
  struct fpm_dplane_nhg *fpm_nhg_route_pop(struct fpm_nhg_tables *t,
  					 uint32_t table_id, uint8_t afi,
  					 const struct prefix *p);
  ```
- [ ] **Step 2: Implement `fpm_nhg_unref()`** exactly as spec §4.2.1:
  decrement; return if still >0; else push onto `delq` (**parent before
  children**), recurse into children, `fpm_nhg_id_free()`, remove from
  `by_hash`/`by_id`, free `nh` and the object, `t->obj_deleted++`.
- [ ] **Step 3: Integrate into `fpm_nl_enqueue()` route ops** — inside the
  `use_nhg_fib` branch, replacing the fork's NHG-event assumptions:
  ```c
  /* ROUTE_INSTALL / ROUTE_UPDATE */
  struct fpm_nhg_staging newq = {}, delq = {};
  struct fpm_dplane_nhg *new_top, *old_top;

  new_top = fpm_nhg_build(&fnc->nhg_tables, dplane_ctx_get_ng(ctx)->nexthop,
  			FPM_NHG_L_A, &newq);
  if (!new_top) { /* build failure */ fpm_nhg_rollback(...); break; }

  /* encode route; on failure roll back and emit nothing */
  if (!encode_route_with_nh_id(ctx, new_top->dplane_id, nl_buf, ...)) {
  	fpm_nhg_rollback(&fnc->nhg_tables, &newq);
  	break;
  }

  fpm_nhg_record_rib_id(new_top, dplane_ctx_get_nhe_id(ctx));   /* show only */
  old_top = fpm_nhg_route_get(&fnc->nhg_tables, table_id, afi, p);
  fpm_nhg_route_set(&fnc->nhg_tables, table_id, afi, p, new_top);
  fpm_nhg_ref(new_top);
  if (old_top)
  	fpm_nhg_unref(&fnc->nhg_tables, old_top, &delq);

  /* flush order: NEWs (children-first) -> route msg -> DELs */
  ```
  ```c
  /* ROUTE_DELETE — ctx may carry no nexthops; the map supplies the object */
  old_top = fpm_nhg_route_pop(&fnc->nhg_tables, table_id, afi, p);
  /* encode RTM_DELROUTE first, then: */
  if (old_top)
  	fpm_nhg_unref(&fnc->nhg_tables, old_top, &delq);
  /* flush order: route del msg -> DELs */
  ```
  Note `use_route_replace` (`:3128`) already converts UPDATE→INSTALL; the map
  upsert is idempotent so both land on the same path.
- [ ] **Step 4: Flush staging in the correct order** — write `newq` NHGFIB
  messages (in push order = children before parents) into `fnc->obuf`, then the
  route message, then `delq` NHGFIB deletes. Assert each NHGFIB message length
  against `DPLANE_FPM_NL_BUF_SIZE` (`:3084`) before writing.
- [ ] **Step 5: Flush tables on reconnect** — call
  `fpm_nhg_tables_flush(&fnc->nhg_tables)` in `fpm_reconnect()` (`:764-770`,
  under `obuf_mutex`, next to `stream_reset(fnc->obuf)`), emitting no DELs.
- [ ] **Step 6: Verify lifecycle on-device** (after Task 10) with `fpm_listener`
  or fpmsyncd logs: install 2 routes sharing one NHG → exactly one NEWNHGFIB
  per object; withdraw one → **no** DEL; withdraw the second → DELs appear
  parent-first; re-add → NEWs reappear with fresh ids.
- [ ] **Step 7: Commit** (`dplane_fpm_sonic: drive dplane NHG lifecycle from route events`)

## Task 9: Emit NHGFIB JSON from objects; drop fork-only zebra API usage

**Files:**
- Modify: `dplane_fpm_sonic/dplane_fpm_sonic.c` — `build_c_nexthopgroupfull_multi` (`:1210`), `_singleton` (`:1270`), `netlink_nexthopgroupfull_msg_encode` (`:2700`), NH/PIC dispatch (`:3215-3312`), SRv6 VPN encode (`:1729-1730`)

- [ ] **Step 1: Add tree-input builders** — new functions
  `build_c_nhgfull_from_obj_multi/_singleton(struct C_NextHopGroupFull *,
  const struct fpm_dplane_nhg *)` that set:
  `id = obj->dplane_id`; `key = (uint32_t)obj->hash`;
  `nhg_flags = obj->nhg_flags`; `depends[i] = obj->children[i].obj->dplane_id`;
  `nh_grp_full_list[i] = {child dplane_id, child weight, child->num_children}`;
  nexthop detail fields from `obj->nh` (mirroring the existing
  `_singleton` field list at `:1270-1330`); `resolved_addr`/`resolved_len`
  from `obj->resolved_prefix`; `vrf_id = obj->vrf_id`; `dependents` left empty.
- [ ] **Step 2: Add an object-input encoder** —
  `netlink_nhgfull_obj_msg_encode(int cmd, const struct fpm_dplane_nhg *obj,
  uint8_t *buf, size_t buflen)` modeled on `netlink_nexthopgroupfull_msg_encode`
  (`:2700`), calling `nexthopgroupfull_json_from_c_nhg_multi/_singleton`
  (`:2791`, `:2811`) with counts taken from `obj->num_children` instead of the
  fork ctx getters. Used by Task 8's flush for `RTM_NEWNHGFIB`/`RTM_DELNHGFIB`.
- [ ] **Step 3: Remove fork-only API usage** so the plugin compiles against the
  upstream base:
  - delete the `DPLANE_OP_PIC_CONTEXT_*` cases (`:3288-3312`) and
    `netlink_pic_context_msg_encode` (`:2317`);
  - delete the `use_nhg_fib` branches that called
    `netlink_nexthopgroupfull_msg_encode` from NH events (`:3216-3218`,
    `:3240-3242`) — in nhg-fib mode NH events are simply skipped;
  - delete `build_c_nexthopgroupfull_multi/_singleton`'s ctx-getter bodies
    (`dplane_ctx_get_nhe_ng/nhg_flags/nh_grp_full/depends/dependents`,
    `:1220-1256`) once Step 1's replacements are in place;
  - in SRv6 VPN encode, replace `dplane_ctx_get_nhe_received_id(ctx)`
    (`:1730`) with the derived L-A object's `dplane_id` (and its resolved view
    for `NH_ID`).
- [ ] **Step 4: Grep-verify no fork-only symbol remains**
  ```bash
  cd /Users/eddie/community/sonic-buildimage/src/sonic-frr/dplane_fpm_sonic
  grep -n "dplane_ctx_get_nhe_received_id\|dplane_ctx_get_nhe_ng\|nh_grp_full\|dplane_ctx_get_nhe_depends\|dplane_ctx_get_nhe_dependents\|PIC_CONTEXT" dplane_fpm_sonic.c
  ```
  Expect no matches (except unrelated comments).
- [ ] **Step 5: Compile the module against the new FRR base**
  ```bash
  cd /Users/eddie/community/frr && cp <plugin files> zebra/ && \
    make -j$(nproc) zebra/dplane_fpm_sonic.la 2>&1 | tail -20
  ```
  Expect a clean build (this is the real gate for Step 3).
- [ ] **Step 6: Commit** (`dplane_fpm_sonic: emit NHGFIB from derived objects, drop fork-only zebra APIs`)

## Task 10: Add `show fpm nhg-fib` and counters

**Files:**
- Modify: `dplane_fpm_sonic/dplane_fpm_sonic.c` — new DEFUNs near `fpm_show_counters_cmd` (`:573`), `install_element` (`:4131-4135`), `fpm_show_status` (`:501`)
- Modify: `dplane_fpm_sonic/fpm_nhg.c` (dump helpers)

- [ ] **Step 1: Add the show DEFUNs**
  ```c
  DEFUN(fpm_show_nhg_fib, fpm_show_nhg_fib_cmd,
        "show fpm nhg-fib [id (1-4294967295)] [json]",
        SHOW_STR FPM_STR
        "Dplane next hop groups derived from route events\n"
        "Filter by dplane next hop group id\n" "Identifier\n" JSON_STR)
  ```
  Non-JSON output, one block per object (walk `by_id` sorted):
  ```
  Dplane NHG 12 (L-A) flags 0x1000 refcount 100 hash 0x9f3a...c1
    children: 13(w1) 14(w1)
    rib nhg ids: 84 85
  Dplane NHG 13 (L-B) flags 0x0008 refcount 1 hash 0x22b1...07
    resolved via 10.0.0.1/32 vrf 0
    children: 15(w1)
  ```
  JSON form mirrors the same fields (`dplaneId`, `level`, `flags`, `refcount`,
  `hash`, `children[]`, `resolvedVia`, `vrfId`, `ribNhgIds[]`).
- [ ] **Step 2: Implement `fpm_nhg_record_rib_id()`** — ring-buffer insert into
  `obj->rib_nhg_ids` (dedup, cap `FPM_NHG_RIB_ID_TRACK`), called from Task 8
  Step 3. This is what answers "how does rib NHG X map to dplane NHG(s)":
  ```
  vtysh -c 'show fpm nhg-fib' | grep -B3 'rib nhg ids:.*\b84\b'
  ```
- [ ] **Step 3: Extend `fpm_show_status`** (`:501`) with the derivation counters:
  `dplane NHG objects: created N, deleted M, live L`, `NHGFIB sent: N`,
  `dedupe hits: N`, and the current mode (`nhg-fib` / `next-hop-groups` / plain).
- [ ] **Step 4: Register the commands** next to `:4131`:
  ```c
  	install_element(ENABLE_NODE, &fpm_show_nhg_fib_cmd);
  ```
- [ ] **Step 5: Verify output shape on-device** (after Task 11 deploy):
  `vtysh -c 'show fpm nhg-fib'` — object count matches
  `redis-cli -n 0 KEYS 'NEXTHOP_GROUP_TABLE:*' | wc -l`; every L-B block shows a
  `resolved via` line; `show fpm nhg-fib json` parses under `python3 -m json.tool`.
- [ ] **Step 6: Commit** (`dplane_fpm_sonic: add show fpm nhg-fib and derivation counters`)

---

## Build & Device Verification

### Repo classification

| Repo / path | 判定 | 理由 |
|---|---|---|
| `sonic-buildimage/src/sonic-frr` (FRR base switch, patch series, `dplane_fpm_sonic`) | **需编译** | Ships in the device image as `frr_*.deb` (`rules/frr.mk`), including `dplane_fpm_sonic.so` |
| `sonic-buildimage/src/libraries/sonic-fib` | **需编译** | Ships as `libnexthopgroup` (`rules/sonic-fib.mk`), linked by both the plugin and fpmsyncd |
| `sonic-buildimage/rules/frr.mk`, `src/sonic-frr/Makefile` | **需编译** | Build definitions for the above |
| `/Users/eddie/community/frr` (upstream base branch) | **不需要**（本仓库不进镜像） | Source of the exported patch only; its own `make`/`make check` is a Task 1/2 gate, not an image build |
| `docs/alinos/**` | **不需要** | Documentation |
| fpmsyncd (sonic-swss) | out of scope | Separate follow-up plan |

Any repo 需编译 → the two closing tasks below are mandatory.

### Task 11: 编译验证 (loop_build)

- [ ] **Step 1: 确认代码已 push 到远程分支** — push the sonic-buildimage branch
  (with `rules/frr.mk`, `src/sonic-frr/patch/*`, `dplane_fpm_sonic/*`,
  `src/libraries/sonic-fib/schema/*`) and the FRR base branch
  `sonic-nhgfib-base` to its remote, since the build clones the FRR branch named
  in `FRR_BRANCH`.
- [ ] **Step 2: 调用 `/alinos.loop_build`** — 单包 deb 快速模式, two packages:
  `target/debs/bookworm/libnexthopgroup*.deb` first (the plugin links it), then
  `target/debs/bookworm/frr_*.deb`. Rationale: only these two packages changed;
  a full image build is unnecessary until device verification needs an image.
- [ ] **Step 3: 编译至 `SUCCESS`** — on failure apply loop_build P6 attribution:
  patch-apply failures against the new upstream base → fix/drop the offending
  patch (Task 3 Step 5); plugin compile errors on fork-only symbols → finish
  Task 9 Step 3; codegen errors → Task 4. Fix, re-push, rebuild.

### Task 12: 设备功能验证 (deploy_verify)

- [ ] **Step 1: 调用 `/alinos.deploy_verify`**，传入 Task 11 的 BUILD_URL
- [ ] **Step 2: 目标设备 `<user-provided-device>`** — collect via AskUserQuestion
  at execution time; no device IP is hardcoded here.
- [ ] **Step 3: 验证点清单**

  | # | 验证点 | 命令 | 期望值 |
  |---|---|---|---|
  | 1 | Plugin loaded on the new FRR base | `docker exec bgp vtysh -c 'show fpm status'` | Connected; no module-load error in `/var/log/frr/frr.log` |
  | 2 | Mode command works and persists | `docker exec bgp vtysh -c 'conf t' -c 'fpm use-nhg-fib'`; `vtysh -c 'show running-config' \| grep use-nhg-fib` | Command accepted; line present in running-config |
  | 3 | Mutual exclusion enforced | `docker exec bgp vtysh -c 'conf t' -c 'fpm use-next-hop-groups'` | Prints `% cannot enable use-next-hop-groups while use-nhg-fib is set` |
  | 4 | Objects derived from routes | `docker exec bgp vtysh -c 'show fpm nhg-fib'` | ≥1 `L-A` block; each recursive BGP NH has an `L-B` block with `resolved via <prefix>/<len> vrf <id>` |
  | 5 | rib→dplane mapping visible | `docker exec bgp vtysh -c 'show ip route <bgp-prefix> json' \| jq '.[][0].nexthops[0].resolvedVia'` then `vtysh -c 'show fpm nhg-fib' \| grep 'rib nhg ids'` | The zebra NHG id reported for the route appears in some object's `rib nhg ids` list |
  | 6 | fpmsyncd accepted the NHGFIB stream | `redis-cli -n 0 KEYS 'NEXTHOP_GROUP_TABLE:*' \| wc -l`; `docker logs bgp 2>&1 \| grep -ci 'fail\|invalid json'` | Count > 0 and equal to the live-object count from `show fpm status`; zero parse/failure lines |
  | 7 | Routes reference the NHG | `redis-cli -n 0 HGETALL 'ROUTE_TABLE:<bgp-prefix>'` | Contains `nexthop_group` equal to the sonic id of the referenced group |
  | 8 | Dedupe across routes | inject/observe N prefixes sharing one BGP NH set; `vtysh -c 'show fpm status' \| grep 'dedupe hits'` | One L-A object for all N routes; `dedupe hits` grows with N |
  | 9 | Lifecycle reclaim | withdraw all routes using that NHG, then `vtysh -c 'show fpm nhg-fib'` and `redis-cli -n 0 KEYS 'NEXTHOP_GROUP_TABLE:*'` | Objects gone from both; `deleted` counter incremented; no leftover keys |
  | 10 | No NHG events on the wire | `docker logs bgp 2>&1 \| grep -c RTM_NEWNEXTHOP` | 0 while in `use-nhg-fib` mode |
  | 11 | Reconnect resync | restart fpmsyncd (`docker exec bgp supervisorctl restart fpmsyncd`), wait for replay | `show fpm nhg-fib` repopulates; no "unknown id" errors in fpmsyncd log; Redis counts return to pre-restart values |
  | 12 | Legacy mode regression | `no fpm use-nhg-fib` + `fpm use-next-hop-groups`, re-check routes | Behaves as the pre-change baseline; routes still programmed |

---

## Self-Review

1. **Spec coverage:** D11→Tasks 1-3; D6→Task 2; D12 schema part→Task 4; D10→Task 5; D2→Task 6; D3/§4.2.1 creation→Task 7; D5 lifecycle + D4 emission order→Task 8; D4 JSON/D1 fork-API removal + §3.2 SRv6→Task 9; D13→Task 10. D7/D9 binding table and D12's fpmsyncd delta are explicitly out of scope (follow-up plan) — stated in Scope.
2. **Placeholder scan:** no TBD/TODO; every step has a command, code, or a concrete expected value.
3. **Type consistency:** `struct fpm_dplane_nhg`/`fpm_nhg_child`/`fpm_nhg_tables`/`fpm_nhg_staging` and the function signatures are declared once in Task 6 and reused verbatim in Tasks 7-10; `dplane_id` is `uint32_t` everywhere (matches `RTA_NH_ID` and fpmsyncd `ribID`).
4. **UT implementation coverage:** the one Red-Green-capable item (PR #19252 fields) has literal test code and a run command (Task 2). Plugin items follow the design's recorded no-harness exception with concrete log/show/wire verification steps instead.
5. **IT applicability:** IT cases exist in the design; the authorization block is copied verbatim with status `Deferred`, so no IT code tasks were created. Preserved, not blocking.
6. **IT authorization:** exactly one status (`Deferred`), copied unedited.
7. **IT coverage:** not applicable while deferred.
8. **Test design fidelity:** frameworks, TC IDs, and case boundaries unchanged; nothing renumbered or merged.
9. **Build & device verification:** present, with a per-repo 需编译/不需要 table and the loop_build + deploy_verify closing tasks carrying 12 concrete verification points (commands + expected values).

## Follow-up (not in this plan)

1. **fpmsyncd plan** (sonic-swss): binding table module + `nhgbinding_ut.cpp`,
   `resolved_addr`/`resolved_len` parsing into `RIBNHGEntry`, repair/restore
   triggers, unknown-`RTA_NH_ID` protocol-error path.
2. **Optional plugin UT harness** — a host-side test for `fpm_nhg.c`'s pure
   logic (hash canonicalization, id allocator, refcount cascade). This would be
   a new test framework not in the approved design; it needs design re-approval
   before planning.
3. **Warm-reboot sonic-id preservation** by content matching (spec §6 notes this
   as follow-up; current behavior is parity with ribfib_2).
