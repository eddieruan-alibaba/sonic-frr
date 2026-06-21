# NHG INSTALLED_FPM_ONLY 项目设计

本项目修复 RIBFIB（`--nhg-fib`）模式下 zebra NHG 下发 kernel 时因 inactive member 被错误纳入导致 EINVAL 的生产 bug。核心手段是引入新标志位 `NEXTHOP_GROUP_INSTALLED_FPM_ONLY`，将"FPM-only 路径假成功"与"kernel 真装了"在状态层彻底分离。

## NHG INSTALLED_FPM_ONLY 标志位

### 架构决策

- 新 flag `NEXTHOP_GROUP_INSTALLED_FPM_ONLY (1 << 12)`，仅在 RIBFIB 模式下使用
- 置位入口唯一：`zebra_nhg_dplane_result()` SUCCESS 分支
- 识别条件：`zebra_nhg_fib_enabled && (CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECEIVED) || CHECK_FLAG(nhe->flags, NEXTHOP_GROUP_RECURSIVE))`
- 社区 `nhe2grp_internal()` 一行不改，靠 INSTALLED 缺位天然过滤 inactive member
- RIBFIB 私有 `nhe2grp_full_internal()` 把 INSTALLED_FPM_ONLY 也算"已装"，FPM 路径不丢成员

### 数据模型

NHE flags 域扩展一位（bit 12）。语义：
- `INSTALLED`：kernel 真实确认已装
- `INSTALLED_FPM_ONLY`：仅 FPM 收到，kernel 未装（RIBFIB 模式私有）

### 接口与约束

- 所有"已装"判断统一为：`INSTALLED || (zebra_nhg_fib_enabled && INSTALLED_FPM_ONLY)`
- 所有 UNSET INSTALLED 的点同步 UNSET INSTALLED_FPM_ONLY
- RECURSIVE / RECEIVED 标志在 NHE 生命周期中不可变（代码验证无 UNSET 路径）
- 默认编译（无 `--nhg-fib`）下 INSTALLED_FPM_ONLY 永为 0，行为与社区零差异

### 附带改动

`zebra_rib.c:480` 的 `zebra_nhg_mark_received_flag()` 调用加 `if (zebra_nhg_fib_enabled)` 守卫，与社区 finalize PR 对齐。保证默认编译下不出现 RECEIVED NHE，使识别条件表达式取值等价。

## 变更记录

| 日期 | 功能 | 变更说明 |
|------|------|---------|
| 2026-06-17 | NHG INSTALLED_FPM_ONLY | 新增设计，修复 RIBFIB skip_kernel 路径 INSTALLED 残留导致 kernel EINVAL |
