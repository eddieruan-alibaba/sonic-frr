# NHG INSTALLED_FPM_ONLY 标志位设计

- 日期: 2026-06-16
- 作者: Yuqing Zhao
- 分支: rib_fib
- 关联生产现象: Jenkins job 962, NHG 294 / NHE 259, kernel EINVAL

## 1. 背景与问题

### 1.1 生产现象

SONiC 生产环境中，zebra 向 kernel 下发 NHG（RTM_NEWNEXTHOP）时被 kernel 以 EINVAL 拒绝：

```
Failed to install Nexthop (294) into the kernel
```

NHG 294 的 depends 列表包含 NHE 259（递归 nexthop 2064:100::1d），但 NHE 259 实际已经 inactive。

### 1.2 直接原因

`zebra_nhg_nhe2grp_internal()`（社区原生函数）构建 `nh_grp[]` 数组的纳入条件是：
```c
VALID && (INSTALLED || QUEUED)
```

NHE 259 残留 `VALID + INSTALLED` 标志，被错误纳入 nh_grp[]，kernel 校验失败返回 EINVAL。

### 1.3 根因：RIBFIB 私有 skip_kernel 路径污染了 INSTALLED 语义

社区原生闭环：
- zebra 通过 dplane 真实下发 NHE 到 kernel → kernel 回 SUCCESS → 设 INSTALLED
- NHE 不再被使用时 → zebra 调用 `dplane_nexthop_delete()` → kernel 删除 → 清 INSTALLED

RIBFIB 在 `zebra/zebra_dplane.c:3878` 引入了 skip_kernel 旁路：
```c
if (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECEIVED) ||
    (zebra_nhg_fib_enabled && CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECURSIVE)))
    dplane_ctx_set_skip_kernel(ctx);
```

skip_kernel 的 ctx 在 kernel provider（rt_netlink/kernel_socket/kernel_netlink）会直接 return SUCCESS，回调到 `zebra_nhg_dplane_result()` 的 SUCCESS 分支同样会 SET INSTALLED。

闭环被打断的后果：
1. NHE 假装下发成功 → 打 INSTALLED
2. underlying nexthop 变 inactive 时，没有真实的 kernel 交互来清 INSTALLED
3. INSTALLED 残留 → 社区 `nhe2grp_internal()` 仍然把 inactive NHE 压进 nh_grp[]
4. kernel 收到带 inactive member 的 NHG → EINVAL

### 1.4 master 上无此问题

社区 master 没有 skip_kernel 旁路，全部 NHE 真实经过 kernel；INSTALLED 状态由 kernel 反馈驱动，不会残留。`VALID` 在两个分支上都保留是正常的（只看 depends 链），**两个分支的唯一差异就是 INSTALLED**。

## 2. 设计目标

1. 修复生产 EINVAL：让 RIBFIB 模式下 inactive 的 RECEIVED / RECURSIVE NHE 不再被压进社区 `nhe2grp_internal()` 构建的 nh_grp[]
2. 不修改任何社区原生代码，避免 upstream merge 风险
3. RIBFIB 私有的 FPM 路径（`nhe2grp_full_internal`）依然能正确压缩这些 NHE
4. 默认编译（无 `--nhg-fib`）行为与社区完全一致，零差异
5. 新引入的状态有完整的清理路径，不留 stale 隐患

## 3. 核心思路

把 "kernel 真装了" 和 "FPM-only 路径假成功" 在状态层彻底分离：

- 引入新 flag：`NEXTHOP_GROUP_INSTALLED_FPM_ONLY`
- skip_kernel 路径回调时 → 置 INSTALLED_FPM_ONLY，**不再**置 INSTALLED
- 社区 `nhe2grp_internal()` 一行不改，靠 INSTALLED 缺位天然过滤
- RIBFIB 私有 `nhe2grp_full_internal()` 在 RIBFIB 模式下把 INSTALLED_FPM_ONLY 也算 "已装"，FPM 路径压缩不丢成员

## 4. 已锁定的设计决策

| 维度 | 决策 |
|------|------|
| 新 flag 编号 | `NEXTHOP_GROUP_INSTALLED_FPM_ONLY (1 << 12)` |
| 置位入口 | 仅 `zebra_nhg_dplane_result()` SUCCESS 分支，依据 nhe 当前 flag 二选一 |
| 识别条件 | `RECEIVED \|\| (zebra_nhg_fib_enabled && RECURSIVE)`（与 `zebra_dplane.c:3878` skip_kernel 入队条件严格对称） |
| kernel 上送路径（`nhg_ctx_process_new` zebra_nhg.c:1322） | **不动**，保持 `SET_FLAG(INSTALLED)` |
| INITIAL_DELAY 旁路（`zebra_dplane.c:4720`） | **不动**，保持 `SET_FLAG(INSTALLED)` |
| 社区 `nhe2grp_internal()`（zebra_nhg.c:3381） | **不动**，靠 INSTALLED 缺位天然过滤 |
| "已装" 统一语义 | `INSTALLED \|\| INSTALLED_FPM_ONLY`（读取点不需要 mode check — flag 唯一 SET 入口已有 `zebra_nhg_fib_enabled` 守卫，非 nhg-fib 模式下 flag 恒为 0） |
| 清理路径 | 现有所有 `UNSET INSTALLED` 的点同步 `UNSET INSTALLED_FPM_ONLY` |
| 作用域守卫 | 仅 SET 入口加 `zebra_nhg_fib_enabled &&` 守卫（单点控制）；读取点直接 check flag 值；社区默认行为零差异 |

### 4.1 为什么用 RECEIVED + (nhg_fib && RECURSIVE) 识别，不用 `dplane_ctx_is_skip_kernel(ctx)`

`skip_kernel` 是社区原生的通用 dplane 机制（`zebra_dplane.h:480-484` 注释）：
> Providers running before the kernel can control whether a kernel update should be done.

任何 dplane provider plugin 都可能设置它（不限于 RIBFIB）。如果用 ctx 上的 skip_kernel 判断，将来社区新增的 skip_kernel 场景会被误吃进 INSTALLED_FPM_ONLY 语义。

而 `nhg_fib && (RECEIVED || RECURSIVE)` 这个条件本身就锚定 RIBFIB 私有语义，与 `zebra_dplane.c:3878` 设置 skip_kernel 的入队条件**在所有取值下等价**（详见 §4.2），闭环干净，未来 upstream 无关联耦合。

### 4.2 附带改动：`zebra_rib.c:480` 加 nhg_fib 守卫（与 finalize PR 对齐）

为了让识别条件 `nhg_fib && (RECEIVED || RECURSIVE)` 与 sonic-frr 入队条件 `RECEIVED || (nhg_fib && RECURSIVE)` 在所有取值上等价，必须保证 **`nhg_fib == false && RECEIVED == true` 这个组合不存在**。

#### 现状

当前 sonic-frr 仓库 `zebra/zebra_rib.c:479-480`：
```c
re->nhe_received = nhe;
zebra_nhg_mark_received_flag(nhe);   // 裸调用，无 nhg_fib 守卫
```

→ 默认编译（无 `--nhg-fib`）下也会出现 `RECEIVED` NHE，导致两个表达式在 `nhg_fib==false && RECEIVED==true` 这个域上不等价。

#### 改动

与社区 finalize PR（`frr_yuqing_dev` 仓库 ribfib 分支 commit `34bbb5758f`）对齐，加上守卫：
```c
re->nhe_received = nhe;

/*
 * We only mark the protocol-received flag in nhg-fib mode
 * to pass the full NHG to FPM.
 * In normal mode, we skip this to avoid breaking other features.
 */
if (zebra_nhg_fib_enabled)
    zebra_nhg_mark_received_flag(nhe);
```

#### 必要性

加上这个守卫后：
- 默认编译下 `RECEIVED` NHE 永不出现 → `nhg_fib==false && RECEIVED==true` 域空集
- 因此 `RECEIVED || (nhg_fib && RECURSIVE)` 与 `nhg_fib && (RECEIVED || RECURSIVE)` 在所有可达取值上等价
- 入队、回调、读取守卫三处都可以使用更直观的 `nhg_fib && (RECEIVED || RECURSIVE)` 形式，可读性更好（一眼就能看出 RIBFIB 才走 FPM-only 路径）

#### 决策表更新

| 维度 | 最终决策 |
|------|---------|
| 识别条件统一为 | `zebra_nhg_fib_enabled && (RECEIVED \|\| RECURSIVE)` |
| 入队条件 | 保持现状 `RECEIVED \|\| (nhg_fib && RECURSIVE)`（与上面等价；改与不改都行，本次保持现状减少改动面） |
| `zebra_rib.c:480` mark_received_flag | **加 `if (zebra_nhg_fib_enabled)` 守卫**（本次新增） |

## 5. 详细改动清单

所有改动只在 `zebra/` 目录下；不接触社区原生函数 `zebra_nhg_nhe2grp_internal()`。

本次 spec 包含两组相关改动：
1. 主改动：引入 `INSTALLED_FPM_ONLY` flag（§5.1 - §5.4）
2. 附带改动：`zebra_rib.c:480` 加 nhg_fib 守卫（§5.5，详见 §4.2）

### 5.1 Flag 定义（`zebra/zebra_nhg.h`）

在 line 191 `NEXTHOP_GROUP_REINSTALL_FPM_ONLY (1 << 11)` 后追加：
```c
/*
 * NHG 已经下发到 FPM but 没有真正装入 kernel.
 * 仅在 RIBFIB（--nhg-fib）模式下，对 RECEIVED 或 RECURSIVE NHE 走 skip_kernel
 * 路径假装成功时使用。社区原生 INSTALLED 语义不受影响。
 */
#define NEXTHOP_GROUP_INSTALLED_FPM_ONLY (1 << 12)
```

### 5.2 置位入口（唯一一处）

**`zebra/zebra_nhg.c` `zebra_nhg_dplane_result()` SUCCESS 分支**（约 line 3895）

当前：
```c
case ZEBRA_DPLANE_REQUEST_SUCCESS:
    SET_FLAG(nhe->flags, NEXTHOP_GROUP_INSTALLED);
    zebra_nhg_handle_install(nhe, true);
```

改为：
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

注：识别条件 `nhg_fib && (RECEIVED || RECURSIVE)` 与入队条件 `RECEIVED || (nhg_fib && RECURSIVE)` 在 §4.2 守卫到位后取值等价。本写法可读性更好。

### 5.3 读取点（直接 check INSTALLED_FPM_ONLY）

所有"是否已装/不需重发/可触发 uninstall"的判断点，统一改成：
```c
CHECK_FLAG(.., INSTALLED) || CHECK_FLAG(.., INSTALLED_FPM_ONLY)
```

读取点无需 `zebra_nhg_fib_enabled &&` 守卫——`INSTALLED_FPM_ONLY` 的唯一 SET 入口已有 mode 守卫，非 nhg-fib 模式下该 flag 恒为 0，CHECK_FLAG 自然返回 false。

| 文件:行 | 函数/上下文 | 当前判断 | 改动后 |
|--------|-----------|---------|--------|
| `zebra_nhg.c:165` | `nhg_connected_tree_del_nhe` | INSTALLED → set REINSTALL_FPM_ONLY | + INSTALLED_FPM_ONLY 也触发 |
| `zebra_nhg.c:193` | `nhg_connected_tree_add_nhe` | 同上 | 同上 |
| `zebra_nhg.c:1221` | `zebra_nhg_handle_install` recursive dependent install | INSTALLED 才推 | + INSTALLED_FPM_ONLY 也推 |
| `zebra_nhg.c:1816` | `zebra_nhg_decrement_ref` keep-around | refcnt=0 + (INSTALLED \|\| QUEUED) → 保留 nhg_keep 秒 | + INSTALLED_FPM_ONLY 也保留，**保持改前 keep-around 行为不退化** |
| `zebra_nhg.c:3564` | `nhe2grp_full_internal` 主循环 | INSTALLED \|\| QUEUED 才纳入 | + INSTALLED_FPM_ONLY 也纳入 |
| `zebra_nhg.c:3611` | `nhe2grp_full_internal` 子节点过滤 | INSTALLED 才纳入 | + INSTALLED_FPM_ONLY 也纳入 |
| `zebra_nhg.c:3795` | `zebra_nhg_install_kernel` 决定是否重下 | `!INSTALLED` 才重下 | + 已 INSTALLED_FPM_ONLY 也算已装、不重下 |
| `zebra_nhg.c:3832` | `zebra_nhg_uninstall_kernel` 入口 | INSTALLED \|\| QUEUED → 发 DELETE | + INSTALLED_FPM_ONLY 也触发 DELETE |
| `zebra_dplane.c:3672` | dplane queue 判断 | `!INSTALLED` | + 已 INSTALLED_FPM_ONLY 也算已装 |
| `zebra_dplane.c:4730` | REINSTALL_FPM_ONLY 分支 → set skip_kernel | INSTALLED 才 skip | + INSTALLED_FPM_ONLY 也 skip |
| `zebra_vty.c:1135` | `show nexthop-group rib` 输出 | "Installed" | 增加 "Installed (FPM only)" 显示 |

**不改的相关位置**：

| 文件:行 | 上下文 | 不改原因 |
|--------|------|---------|
| `zebra_nhg.c:4430` | interface-up handler `if_nbr_address_install` 的 INSTALLED 检查 | 该处处理 singleton NHE（绑定到接口、非 RECURSIVE 非 RECEIVED），永远不可能被打 INSTALLED_FPM_ONLY，改了是死代码 |

### 5.4 清理点（与 INSTALLED 一一对齐）

所有现存 `UNSET_FLAG(.., INSTALLED)` 的点同步 `UNSET_FLAG(.., INSTALLED_FPM_ONLY)`：

| 文件:行 | 上下文 |
|--------|------|
| `zebra_nhg.c:1094` | `zebra_nhg_handle_kernel_state_change` |
| `zebra_nhg.c:1250` | 同上另一处分支 |
| `zebra_nhg.c:3780` | INITIAL_DELAY 切换（保险对齐） |
| `zebra_nhg.c:3847` | `zebra_nhg_uninstall_kernel` SUCCESS 后 |
| `zebra_nhg.c:4016` | NHE reset 入口 |

### 5.5 附带改动：`zebra_rib.c` mark_received_flag 加 nhg_fib 守卫

详见 §4.2。

`zebra/zebra_rib.c:479-480` 由：
```c
re->nhe_received = nhe;
zebra_nhg_mark_received_flag(nhe);
```

改为：
```c
re->nhe_received = nhe;

/*
 * We only mark the protocol-received flag in nhg-fib mode
 * to pass the full NHG to FPM.
 * In normal mode, we skip this to avoid breaking other features.
 */
if (zebra_nhg_fib_enabled)
    zebra_nhg_mark_received_flag(nhe);
```

此改动已在社区 PR finalize 版本（`frr_yuqing_dev` 仓库 ribfib 分支 commit `34bbb5758f`）中存在，本次同步带入 sonic-frr 仓库。

副作用：
- 默认编译（无 `--nhg-fib`）下不再有 NHE 被打 `RECEIVED` 标志
- `zebra_dplane.c:3878` 入队条件中 `RECEIVED` 那一支在默认编译下永远不成立 → 默认编译下 RECEIVED NHE 走真实 kernel 下发（与社区行为一致）
- 与 INSTALLED_FPM_ONLY 改动配合，使三处条件取值等价

## 6. 状态机推演（生产 case 验证）

### 6.1 NHG 294 / NHE 259（生产现场）

NHE 259：via 2064:100::1d 递归地址，无 interface 关联
NHG 294：包含 NHE 259（recursive）+ NHE 264

#### NHE 259 产生路径

| 步骤 | 行为 | 结果 |
|------|------|------|
| 1 | BGP 路由插入触发 `route_entry_update_original_nhe()`（zebra_rib.c:477） | NHE 259 创建并被 `re->nhe_received` 引用 |
| 2 | `zebra_nhg_mark_received_flag()` | 打 `VALID + RECEIVED` |
| 3 | 后续 `rib_install_kernel()` 触发 `zebra_nhg_install_kernel(re->nhe_received)` | 入 dplane |
| 4 | dplane 入队条件命中 RECEIVED → `set skip_kernel(ctx)` | kernel provider return SUCCESS |
| 5 | `zebra_nhg_dplane_result()` SUCCESS 分支 | **当前** SET INSTALLED → 0x403 |
| 6 | 2064:100::1d 不可达 | INSTALLED 残留（无人清） |
| 7 | NHG 294 重建，遍历 depends 命中社区 `nhe2grp_internal` 的 `VALID && INSTALLED` | NHE 259 被压进 nh_grp[] |
| 8 | kernel 校验失败 | **EINVAL** |

NHE 259 路径完全经过 dplane → 完全在本次 INSTALLED_FPM_ONLY 改动覆盖范围内。

#### 改前 RIBFIB

| 时间点 | NHE 259 flags | NHG 294 行为 |
|-------|--------------|------------|
| T1: BGP 路由插入触发 mark_received + dplane SUCCESS | `0x403` (VALID \| INSTALLED \| RECEIVED) | — |
| T2: 2064:100::1d 不可达 | `0x403`（INSTALLED 残留） | — |
| T3: NHG 294 重建 | NHE 259 仍 INSTALLED → 被纳入 nh_grp[] | kernel **EINVAL** |

#### 改后 RIBFIB（本次改动）

| 时间点 | NHE 259 flags | NHG 294 行为 |
|-------|--------------|------------|
| T1: BGP 路由插入触发 mark_received + dplane SUCCESS（识别条件命中 RECEIVED） | `0x401` (VALID \| RECEIVED \| INSTALLED_FPM_ONLY) | — |
| T2: 2064:100::1d 不可达 | INSTALLED_FPM_ONLY 残留（INSTALLED 始终未设） | — |
| T3: NHG 294 重建，社区 `nhe2grp_internal()` 看 INSTALLED == 0 | NHE 259 **不**进 nh_grp[] | kernel SUCCESS |
| | RIBFIB `nhe2grp_full_internal()` 看 INSTALLED_FPM_ONLY 算已装 | NHE 259 进 nh_grp_full[]（FPM 路径完整） |

EINVAL 消除；FPM 路径数据完整。

### 6.2 RECURSIVE NHE 通过 dplane skip_kernel 路径（本次改动直接覆盖）

例：递归 NHE，依赖 nexthop 1.1.1.1，nhg_fib 模式

#### 改前 RIBFIB

| 时间点 | flags | 后果 |
|-------|-------|-----|
| T1: 创建并下发 dplane | `VALID \| RECURSIVE` | skip_kernel SUCCESS 回调 |
| T2: 回调 SET INSTALLED | `VALID \| INSTALLED \| RECURSIVE` | — |
| T3: 1.1.1.1 不可达 | INSTALLED 残留 | 包含它的 NHG 进 nh_grp[] → EINVAL 风险 |

#### 改后 RIBFIB

| 时间点 | flags | 后果 |
|-------|-------|-----|
| T1: 创建并下发 dplane | `VALID \| RECURSIVE` | skip_kernel SUCCESS 回调 |
| T2: 回调识别 nhg_fib && RECURSIVE → SET INSTALLED_FPM_ONLY | `VALID \| RECURSIVE \| INSTALLED_FPM_ONLY` | — |
| T3: 1.1.1.1 不可达 | INSTALLED_FPM_ONLY 残留（INSTALLED 始终未设） | 社区 `nhe2grp_internal()` 看 INSTALLED == 0 → **跳过** |
| | | RIBFIB `nhe2grp_full_internal()` 看 INSTALLED_FPM_ONLY → 纳入 FPM nh_grp_full[] |

kernel 不会再收到 inactive member，EINVAL 消除；FPM 路径数据完整，业务不受影响。

### 6.3 默认编译（无 --nhg-fib）

- 入队条件 `zebra_dplane.c:3878` 的 `(zebra_nhg_fib_enabled && RECURSIVE)` 整段 false
- RECEIVED 也不会被设（RIBFIB 才 mark）
- → skip_kernel 永远不被设 → 走真实 kernel → 行为与 master 完全一致
- 所有读取点的 `(zebra_nhg_fib_enabled && INSTALLED_FPM_ONLY)` 也恒 false → 不读新 flag
- INSTALLED_FPM_ONLY 永远是 0

## 7. 测试方案

### 7.1 单元/topotest

复用现有 `test_zebra_nhg_inactive_skip.py`（见根仓 `zebra_nhg_stale_flags_verification.md` 附录 B）：

```
r1 ---eth0--- s1
r1 ---eth1--- s2
ip route 172.16.1.1/32 192.168.1.2
ip route 172.16.2.1/32 192.168.2.2
ip route 10.0.0.0/24 172.16.1.1
ip route 10.0.0.0/24 172.16.2.1
```

测试断言：删除 `172.16.2.1/32` resolve route 后，对应递归 NHE 的 `INSTALLED` flag 应被清除。

| 环境 | 预期 |
|------|------|
| master | PASS（INSTALLED 清除） |
| RIBFIB 改前 | FAIL（INSTALLED 残留） |
| RIBFIB 改后 | PASS（INSTALLED 始终未设；INSTALLED_FPM_ONLY 路径独立） |

### 7.2 生产场景回归

复现 Jenkins job 962 拓扑，确认：
- 不再出现 `Failed to install Nexthop (294) into the kernel` EINVAL
- FPM 收到的 NHG depends list 完整、无丢成员
- show nexthop-group rib 输出可读，`Installed (FPM only)` 标识清晰

### 7.3 默认编译回归

无 `--nhg-fib` 启动 zebra：
- 所有 NHE 走真实 kernel 下发
- INSTALLED_FPM_ONLY 永不出现
- 行为与 master diff 为零

## 8. 风险与边界

| 风险 | 缓解措施 |
|------|---------|
| 漏改某个 INSTALLED 检查点，导致 FPM_ONLY NHE 在该点被当成"未装" | §5.3 表格穷举；review 时对照 `grep NEXTHOP_GROUP_INSTALLED zebra/` 全量核对 |
| 漏改某个 UNSET 点，导致 INSTALLED_FPM_ONLY 残留 | §5.4 表格穷举；新增清理时与 INSTALLED 一一对齐 |
| 入队识别条件与回调识别条件不同步演化 | 两处条件代码注释互相引用；任一处变更必须同步 |
| 入队后回调前 nhe flag 被改写（极小） | RECURSIVE / RECEIVED 在 NHE 生命周期里非常稳定；理论上有概率不一致，但不会造成功能错误（最差表现为 NHE 仍按 INSTALLED 处理，与改前等价） |
| INITIAL_DELAY + RECURSIVE 同时成立 | INITIAL_DELAY 旁路在置位前就 return，不进 SUCCESS 回调，不冲突 |
| RECURSIVE/RECEIVED 标志运行时变迁导致"已装"守卫语义失配 | 经代码确认（全库 grep）：RECURSIVE 仅在 NHE 创建时 SET（zebra_nhg.c:830/878），RECEIVED 仅在 mark_received_flag 中 SET；全代码无任何 UNSET 路径。两者在 NHE 生命周期中**不可变**。若未来引入 UNSET 路径，必须配套清除 INSTALLED_FPM_ONLY |

## 9. 后续工作（不在本次范围）

1. **RECEIVED NHE 在 underlying nexthop 变 inactive 时的清理路径加强**：现状依赖 `zebra_nhg_uninstall_kernel()` 的引用计数清理，未来可考虑显式 stale-detection
2. **`nhg_ctx_process_new()` 路径（`zebra_nhg.c:1322`）的语义梳理**：这条路径由 zebra 启动时 kernel netlink dump 触发，处理 RTM_NEWNEXTHOP 上送，目前直接打 VALID+INSTALLED。本次保持现状（与 sonic-frr / RIBFIB 路径无交叉），未来如有从 kernel dump 与 nhg_fib 状态机交互的场景，再单独评估

## 10. 不做的事

- 不改任何社区原生函数的内部逻辑（`nhe2grp_internal`、`zebra_nhg_check_valid` 等）
- 不为本 bug 添加 INSTALLED 标志的"清除补丁"作为兜底（治标不治根）
- 不改变 `--nhg-fib` 默认值（仍然默认关闭）
- 不改 `nhg_ctx_process_new()`（kernel netlink 上送路径，与本 spec 无交叉）

## 11. GBrain 参考

无相关历史记录（MCP 未连接）。

## Grill-Me 审查记录

**审查时间：** 2026-06-17
**审查目标：** docs/alinos/specs/2026-06-16-nhg-installed-fpm-only-design.md

### 关键决策

- 识别条件最终定为 `zebra_nhg_fib_enabled && (RECEIVED || RECURSIVE)`（而非 `dplane_ctx_is_skip_kernel(ctx)`），可读性更好，锚定 RIBFIB 私有语义
- 为使该表达式与 sonic-frr 入队条件 `RECEIVED || (nhg_fib && RECURSIVE)` 在所有取值上等价，必须同步带入 `zebra_rib.c:480` 的 `if (zebra_nhg_fib_enabled)` 守卫（与 finalize PR 对齐）
- 生产 NHE 259 路径经核实完全走 dplane skip_kernel（BGP 路由 → mark_received → dplane SUCCESS），本次改动**直接覆盖**，非"后续工作"

### 发现的问题

- [x] §6.1 原文错误地称 NHE 259 走 `nhg_ctx_process_new`（kernel 上送），实际走 dplane skip_kernel — **已修正**
- [x] §9/§10 原文将 fpmsyncd 上送路径列为"后续工作/不做的事"，实际不成立 — **已修正**
- [x] 原 spec 识别条件 `RECEIVED || (nhg_fib && RECURSIVE)` 与用户最终希望的"美观"写法 `nhg_fib && (RECEIVED || RECURSIVE)` 在 sonic-frr 当前状态下不等价 — **通过带入 mark_received 守卫解决，spec §4.2 已补充**
- [x] RECURSIVE/RECEIVED 标志不可变假设未显式记录 — **已补入 §8 风险表**

### 设计确认

- 置位入口唯一（`zebra_nhg_dplane_result()` SUCCESS 分支）：经压力测试确认合理
- 社区 `nhe2grp_internal()` 一行不改、靠 INSTALLED 缺位天然过滤：验证通过
- 统一"已装"语义 `INSTALLED || (nhg_fib && INSTALLED_FPM_ONLY)` 适用于所有读取点：经确认 RECURSIVE/RECEIVED 运行时不变，不存在语义失配风险
- 清理点与 INSTALLED 一一对齐：5 处 UNSET 全部覆盖
- 生产 NHG 294 / NHE 259 EINVAL 在新状态机下被根治（§6.1 推演验证通过）
- 默认编译（无 --nhg-fib）行为与社区完全一致、零差异（§6.3 验证通过）

## 12. 设备验证记录

### 12.1 验证环境

| 维度 | 信息 |
|------|------|
| 设备 | PE3 / 10.250.0.53（KVM 虚拟设备，cisco-8101-p4-32x100-vs） |
| Image | SONiC.rib_fib.170-dirty-20260622.063444 |
| zebra cmdline | `/usr/lib/frr/zebra ... --nhg-fib`（RIBFIB 模式启用） |
| 拓扑 | PE3 通过 Ethernet4 连 P2（fc08::2，BGP 邻居 up），通过 Ethernet12 连 P4（fc06::2，BGP 邻居 down） |

### 12.2 验证场景

复用 ribfib_route_convergence.md §7.1 的 Test Topology 1 静态路由配置（递归依赖 BGP 学到的 nexthop），通过 shutdown Ethernet4 让所有 BGP NH 变 inactive，模拟生产 Jenkins 962 的 EINVAL 触发条件。

#### 配置

```
vtysh -c 'configure terminal' \
  -c 'ipv6 route 1::1/128 2064:100::1d' \
  -c 'ipv6 route 1::1/128 2064:200::1e' \
  -c 'ipv6 route 2::2/128 2064:200::1e' \
  -c 'ipv6 route 3::3/128 1::1' \
  -c 'ipv6 route 3::3/128 2::2' \
  -c 'ipv6 route 4::4/128 1::1' -c 'end'
```

#### 触发

```
sudo config interface shutdown Ethernet4
```

### 12.3 关键证据

#### 证据 1：INSTALLED_FPM_ONLY 在使用，且与 inactive 状态共存

```
admin@PE3:~$ vtysh -c 'show nexthop-group rib' | grep -B 4 'fc08::2.*inactive' | head -10
     Uptime: 04:23:51
     VRF: default(IPv6)
     Nexthop Count: 1
     Valid, Installed (FPM only)            ← 关键：不是 "Valid, Installed"
           via fc08::2 (vrf default) inactive, weight 1
```

NHE flags = `0x1401`（`VALID | RECEIVED | INSTALLED_FPM_ONLY`）—— **未携带** INSTALLED 位。

#### 证据 2：NHE 状态量化（shutdown 后稳定态）

| 类别 | 计数 |
|------|------|
| 总 NHE | 235 |
| `Valid, Installed`（真装 kernel） | 16 |
| `Valid, Installed (FPM only)`（FPM-only 假装成功） | 12 |
| 含 inactive nexthop 的 NHE | 117 |

#### 证据 3：生产 EINVAL 0 次复现

```
admin@PE3:~$ sudo grep -ic 'Failed to install Nexthop' /var/log/frr/zebra.log
0
```

生产 Jenkins 962 同模型场景下该错误反复出现（`Failed to install Nexthop (294[259/264])`）。改后场景压力更大（整个 Ethernet4 down 导致全部 BGP NH 失效），仍然 0 次。

#### 证据 4：bug 修复路径闭环验证

推理链：
1. inactive 递归 NHE 显示 `Valid, Installed (FPM only)` → INSTALLED 位 = 0
2. 社区 `nhe2grp_internal()` 纳入条件 `VALID && (INSTALLED || QUEUED)` → INSTALLED == 0 → **跳过该 NHE**
3. kernel 收到的 NHG 不带 inactive 成员 → 不会校验失败
4. 因此 zebra.log 无 `Failed to install Nexthop` 错误

`Failed to install Nexthop = 0` 即"inactive NHE 没被压进 kernel nh_grp[]"的等价证明（如果被压进去，kernel 必然 EINVAL，必然记录该错误）。

### 12.4 旁证：另一类瞬态错误（与本 spec 无关）

shutdown Ethernet4 瞬间日志中出现 4 条：
```
ERR netlink-dp (NS 0) error: Invalid argument, type=RTM_NEWROUTE(24), seq=1039
WARNING staticd: Route 1::1/128 failed to install for table: 254
```

这是 **RTM_NEWROUTE**（路由层）瞬态错误——所有 nexthop 全 inactive 时 kernel 拒绝路由。staticd 也正确报 failed。**不是** 本 spec 修复的 RTM_NEWNEXTHOP NHG 错误，属预期行为。

### 12.5 验证结论

- INSTALLED_FPM_ONLY flag 在 RIBFIB 模式下按设计生效
- 生产 EINVAL 场景在改后零复现
- show 命令 `Installed (FPM only)` 显示工作正常，便于运维识别状态

## 13. 环境拉起踩坑记录

本节记录验证过程中遇到的环境问题，供后续验证场景参考。

### 13.1 现象

Jenkins build 的 "Set up Pytest ENV VM" 阶段 40 分钟 timeout 失败（exit code 143），`1_setup_pytest_vm.txt` 日志 0 字节或极小。

### 13.2 根因

镜像服务器 `30.57.186.117` 带宽不稳定（200~700 KB/s），下载 1.4GB 的 `sonic-vs_daily.img.gz` 需要 50~90 分钟，超过 setup 阶段的 40 分钟 timeout。

### 13.3 排查路径

1. SSH 到 Jenkins node（`11.165.122.19`），检查 VM 状态：`virsh list --all`
2. Ping pytest VM：`ping 192.168.0.2` → 不通
3. 检查 vnet RX packets：`ip -s link show vnet0` → RX=0
4. 初步误判为 VM 内部 OS 未启动 → 重启 host 后问题复现
5. 观察 `pstree` 发现 setup 进程子进程卡在 `wget`
6. 测速确认 server 带宽瓶颈（非 VM 问题）

### 13.4 解决方案：预下载镜像 + skip 模式 + hardlink 注入

#### 前提条件

- `pytest_mgmt.py` 支持 `vsonic_image=skip` 参数（跳过下载，`sleep 120s`）
- setup 脚本的 `local_cache_dir` 是动态路径：`/tmp/local_cache/{时间戳}/`

#### Step 1：预下载镜像到 host（一次性，或镜像更新时重做）

```bash
ssh root@11.165.122.19
mkdir -p /root/img_cache
nohup wget -c -O /root/img_cache/sonic-vs_daily.img.gz \
  http://30.57.186.117/ribfib/latest/sonic-vs_daily.img.gz \
  > /root/img_cache/wget.log 2>&1 &

# 查看进度
tail -f /root/img_cache/wget.log

# 下载完成后验证
gzip -t /root/img_cache/sonic-vs_daily.img.gz && echo "OK"
```

#### Step 2：触发 Jenkins build 前，在 host 上启动监听循环

```bash
ssh root@11.165.122.19

# 一次性监听脚本：检测到新 build 的临时目录后自动 hardlink 镜像进去
while true; do
  d=$(ls -td /tmp/local_cache/*/ 2>/dev/null | head -1)
  if [ -n "$d" ] && [ ! -f "$d/sonic-vs.img.gz" ]; then
    ln /root/img_cache/sonic-vs_daily.img.gz "$d/sonic-vs.img.gz"
    echo "[$(date)] LINKED into $d"
    ls -la "$d/sonic-vs.img.gz"
    break
  fi
  sleep 1
done
```

#### Step 3：触发 Jenkins build

- 在 Jenkins 参数页面，把 `vsonic_image` 改为 `skip`
- 其他参数不变，正常触发

#### Step 4：验证

监听循环输出 `LINKED into ...` 即表示注入成功，setup 阶段会在几分钟内完成（对比正常下载需要 50+ 分钟）。

### 13.5 恢复正常模式

当镜像服务器带宽恢复后，Jenkins build 触发时不填 `skip`，保持默认 URL 即可。无需清理 `/root/img_cache/`。

### 13.6 注意事项

| 项目 | 说明 |
|------|------|
| hardlink 时机 | 必须在 `download_sonic_vs_img()` 的 `time.sleep(120)` 结束前完成，窗口充裕 |
| 镜像更新 | skip 模式使用的是预下载的镜像，非实时最新；需测最新 daily 时重新下载或改回 URL |
| 磁盘占用 | hardlink 不额外占空间，build 结束后 `/tmp/local_cache/` 会被脚本自动清理 |
| 适用范围 | 仅当镜像下载速度不足以在 40min timeout 内完成时使用 |
