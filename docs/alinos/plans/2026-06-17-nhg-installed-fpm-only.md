# NHG INSTALLED_FPM_ONLY Implementation Plan

> **For agentic workers:** Use /alinos.subagent-dev (recommended) or /alinos.executing-plans to implement this plan task-by-task.

**Goal:** Introduce `NEXTHOP_GROUP_INSTALLED_FPM_ONLY` flag to separate FPM-only install state from real kernel install, fixing production EINVAL when inactive NHE members leak into kernel NHG programming.

**Architecture:** New flag (bit 12) set exclusively in `zebra_nhg_dplane_result()` SUCCESS branch when nhg_fib mode identifies RECEIVED/RECURSIVE NHE. All existing INSTALLED check points get `|| (nhg_fib && INSTALLED_FPM_ONLY)` guard. All UNSET INSTALLED points synchronously UNSET the new flag.

**Tech Stack:** C (FRR zebra daemon), Linux kernel netlink NHG programming, SONiC FPM datapath.

---

## Task 1: Flag Definition + mark_received Guard

**Files:**
- Modify: `zebra/zebra_nhg.h:191` (after REINSTALL_FPM_ONLY)
- Modify: `zebra/zebra_rib.c:479-480`

- [ ] **Step 1: Add flag definition to zebra_nhg.h**

  After line 191 (`#define NEXTHOP_GROUP_REINSTALL_FPM_ONLY (1 << 11)`), before the closing `};`, add:
  ```c
  /*
   * NHG delivered to FPM only, not actually installed in kernel.
   * Used in RIBFIB (--nhg-fib) mode for RECEIVED or RECURSIVE NHEs
   * that go through skip_kernel path. Community INSTALLED semantics unaffected.
   */
  #define NEXTHOP_GROUP_INSTALLED_FPM_ONLY (1 << 12)
  ```

- [ ] **Step 2: Add nhg_fib guard to mark_received_flag call in zebra_rib.c**

  In `zebra/zebra_rib.c`, function `route_entry_update_original_nhe()`, change:
  ```c
  re->nhe_received = nhe;
  zebra_nhg_mark_received_flag(nhe);
  ```
  To:
  ```c
  re->nhe_received = nhe;

  if (zebra_nhg_fib_enabled)
      zebra_nhg_mark_received_flag(nhe);
  ```

- [ ] **Step 3: Compile check**

  Run: `make -C zebra zebra_nhg.o zebra_rib.o 2>&1 | grep -i error` (or equivalent build command for this repo)

- [ ] **Step 4: Commit**

  Message: `zebra: Add NEXTHOP_GROUP_INSTALLED_FPM_ONLY flag and mark_received guard`

---

## Task 2: Set-Flag Entry Point (zebra_nhg_dplane_result)

**Files:**
- Modify: `zebra/zebra_nhg.c` (~line 3893-3896, `zebra_nhg_dplane_result()` SUCCESS branch)

- [ ] **Step 1: Modify the SUCCESS branch**

  In `zebra_nhg_dplane_result()`, locate:
  ```c
  case ZEBRA_DPLANE_REQUEST_SUCCESS:
      SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
      zebra_nhg_handle_install(nhe, true);
  ```

  Replace with:
  ```c
  case ZEBRA_DPLANE_REQUEST_SUCCESS:
      if (zebra_nhg_fib_enabled &&
          (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECEIVED) ||
           CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECURSIVE))) {
          SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
      } else {
          SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
      }
      zebra_nhg_handle_install(nhe, true);
  ```

- [ ] **Step 2: Compile check**

- [ ] **Step 3: Commit**

  Message: `zebra: Set INSTALLED_FPM_ONLY instead of INSTALLED for skip_kernel NHEs`

---

## Task 3: Read Points in zebra_nhg.c (Part 1 — install/uninstall flow)

**Files:**
- Modify: `zebra/zebra_nhg.c` (lines 165, 193, 1221, 1816, 3795, 3832)

- [ ] **Step 1: nhg_connected_tree_del_nhe (line 165)**

  Change:
  ```c
  if (zebra_nhg_fib_enabled && CHECK_FLAG(depend->flags, NEXTHOP_GROUP_INSTALLED)) {
  ```
  To:
  ```c
  if (zebra_nhg_fib_enabled &&
      (CHECK_FLAG(depend->flags, NEXTHOP_GROUP_INSTALLED) ||
       CHECK_FLAG(depend->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY))) {
  ```

- [ ] **Step 2: nhg_connected_tree_add_nhe (line 193)**

  Same pattern as Step 1.

- [ ] **Step 3: zebra_nhg_handle_install (line 1221)**

  Change:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) &&
      CHECK_FLAG(rb_node_dep->nhe->flags, NEXTHOP_GROUP_RECURSIVE)) {
  ```
  To:
  ```c
  if ((CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
       (zebra_nhg_fib_enabled &&
        CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY))) &&
      CHECK_FLAG(rb_node_dep->nhe->flags, NEXTHOP_GROUP_RECURSIVE)) {
  ```

- [ ] **Step 4: zebra_nhg_decrement_ref keep-around (line 1816)**

  必做。改前：RECEIVED/RECURSIVE NHE 经 skip_kernel SUCCESS 后被打 INSTALLED → refcnt=0 时享受 `nhg_keep` 秒 keep-around 保护。
  本次改动后这些 NHE 改为 INSTALLED_FPM_ONLY；若此处不改，refcnt=0 时既无 INSTALLED 也无 QUEUED，会跳过 keep-around 直接 uninstall，**导致行为退化**（丢失原有的快速复用保护）。

  Change:
  ```c
  if (!zebra_router_in_shutdown() && nhe->refcnt <= 0 &&
      (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
       CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)) &&
  ```
  To:
  ```c
  if (!zebra_router_in_shutdown() && nhe->refcnt <= 0 &&
      (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
       (zebra_nhg_fib_enabled &&
        CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)) ||
       CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)) &&
  ```

- [ ] **Step 5: zebra_nhg_install_kernel (line 3795)**

  Change:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_VALID) &&
      (!CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
  ```
  To:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_VALID) &&
      (!(CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
         (zebra_nhg_fib_enabled &&
          CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY))) ||
  ```

- [ ] **Step 6: zebra_nhg_uninstall_kernel (line 3832)**

  Change:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
      CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)) {
  ```
  To:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
      (zebra_nhg_fib_enabled &&
       CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)) ||
      CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)) {
  ```

  注：interface-up handler（zebra_nhg.c:4430）的 INSTALLED 检查**不改**——该处处理的 singleton NHE（绑定到接口、非 RECURSIVE 也非 RECEIVED）永远不可能被打 INSTALLED_FPM_ONLY，改了等于死代码。

- [ ] **Step 7: Compile check**

- [ ] **Step 8: Commit**

  Message: `zebra: Add INSTALLED_FPM_ONLY to install/uninstall flow check points`

---

## Task 4: Read Points in zebra_nhg.c (Part 2 — nhe2grp_full_internal)

**Files:**
- Modify: `zebra/zebra_nhg.c` (lines 3564, 3611)

- [ ] **Step 1: Main loop filter (line 3564)**

  Change:
  ```c
  && !(CHECK_FLAG(curr_node->flags, NEXTHOP_GROUP_INSTALLED) ||
       CHECK_FLAG(curr_node->flags, NEXTHOP_GROUP_QUEUED)))
  ```
  To:
  ```c
  && !(CHECK_FLAG(curr_node->flags, NEXTHOP_GROUP_INSTALLED) ||
       (zebra_nhg_fib_enabled &&
        CHECK_FLAG(curr_node->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)) ||
       CHECK_FLAG(curr_node->flags, NEXTHOP_GROUP_QUEUED)))
  ```

- [ ] **Step 2: Sub-depends filter (line 3611)**

  Change:
  ```c
  && !CHECK_FLAG(sub_rb_node->nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
  ```
  To:
  ```c
  && !(CHECK_FLAG(sub_rb_node->nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
       (zebra_nhg_fib_enabled &&
        CHECK_FLAG(sub_rb_node->nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)))) {
  ```

- [ ] **Step 3: Compile check**

- [ ] **Step 4: Commit**

  Message: `zebra: Include INSTALLED_FPM_ONLY NHEs in FPM nh_grp_full array`

---

## Task 5: Read Points in zebra_dplane.c

**Files:**
- Modify: `zebra/zebra_dplane.c` (lines 3672, 4730)

- [ ] **Step 1: Route dplane queue check (line 3672)**

  Change:
  ```c
  !CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) &&
  !CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)))
  ```
  To:
  ```c
  !CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) &&
  !(zebra_nhg_fib_enabled &&
    CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)) &&
  !CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_QUEUED)))
  ```

- [ ] **Step 2: REINSTALL_FPM_ONLY skip_kernel decision (line 4730)**

  Change:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
      dplane_ctx_set_skip_kernel(ctx);
  }
  ```
  To:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED) ||
      (zebra_nhg_fib_enabled &&
       CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY))) {
      dplane_ctx_set_skip_kernel(ctx);
  }
  ```

- [ ] **Step 3: Compile check**

- [ ] **Step 4: Commit**

  Message: `zebra: Handle INSTALLED_FPM_ONLY in dplane route/nhg queue logic`

---

## Task 6: Cleanup Points (UNSET synchronization)

**Files:**
- Modify: `zebra/zebra_nhg.c` (lines 1094, 1250, 3780, 3847, 4016)

- [ ] **Step 1: zebra_nhg_set_valid (line 1094)**

  After `UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);` add:
  ```c
  UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
  ```

- [ ] **Step 2: zebra_nhg_handle_kernel_state_change (line 1250)**

  After `UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);` add:
  ```c
  UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
  ```

- [ ] **Step 3: zebra_nhg_install_kernel INITIAL_DELAY (line 3780)**

  After `UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);` add:
  ```c
  UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
  ```

- [ ] **Step 4: zebra_nhg_uninstall_kernel SUCCESS (line 3847)**

  After `UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);` add:
  ```c
  UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
  ```

- [ ] **Step 5: zebra_nhg_mark_keep_entry / NHE reset (line 4016)**

  After `UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);` add:
  ```c
  UNSET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY);
  ```

- [ ] **Step 6: Compile check**

- [ ] **Step 7: Commit**

  Message: `zebra: Sync UNSET INSTALLED_FPM_ONLY at all cleanup points`

---

## Task 7: Display (show nexthop-group rib)

**Files:**
- Modify: `zebra/zebra_vty.c` (~line 1135)

- [ ] **Step 1: Add FPM-only display**

  After the existing block:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
  ```
  Add an else-if (or adjust the existing block) to show "Installed (FPM only)" when only INSTALLED_FPM_ONLY is set:
  ```c
  if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED)) {
      // existing "Installed" output
  } else if (zebra_nhg_fib_enabled &&
             CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED_FPM_ONLY)) {
      // output "Installed (FPM only)"
  }
  ```

- [ ] **Step 2: Compile check**

- [ ] **Step 3: Commit**

  Message: `zebra: Display 'Installed (FPM only)' in show nexthop-group rib`

---

## Task 8: Final Verification

- [ ] **Step 1: Full grep audit**

  Run: `grep -n NEXTHOP_GROUP_INSTALLED zebra/` and verify every hit is either:
  - (a) Social community code untouched (nhe2grp_internal, nhg_ctx_process_new)
  - (b) Already has INSTALLED_FPM_ONLY paired
  - (c) Interface-up handler (line 4430) — explicitly不改，singleton NHE 不会有 FPM_ONLY

- [ ] **Step 2: Build full zebra binary**

  Confirm zero warnings/errors.

- [ ] **Step 3: Verify with topotest (if environment available)**

  Run `test_zebra_nhg_inactive_skip.py` with `--nhg-fib` mode.

注：本测试分支保留所有现有 `zlog_err` 调试日志，不删除（与 master/上游合并时再清理）。

---

## Self-Review Checklist

| Spec Requirement | Covered By |
|------------------|-----------|
| §5.1 Flag definition | Task 1 Step 1 |
| §5.2 Set-flag entry point | Task 2 Step 1 |
| §5.3 Read points (zebra_nhg.c install/uninstall flow) | Task 3 |
| §5.3 Read points (zebra_nhg.c nhe2grp_full) | Task 4 |
| §5.3 Read points (zebra_dplane.c) | Task 5 |
| §5.4 Cleanup points (5 locations) | Task 6 |
| §5.5 mark_received guard | Task 1 Step 2 |
| §7 show display | Task 7 |
| Final audit | Task 8 |
