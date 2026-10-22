# AGENTS.md — QEMU fork（RDP 补丁）

这是自家 QEMU fork，基线 v10.0.10，RDP 相关补丁在分支 `rdp` 上。

**开始任务前必须阅读：**
1. [../vds/AGENTS.md](../vds/AGENTS.md) —— 通用规则（范围纪律、禁止事项、完成前检查、报告格式），本仓库同样适用
2. [../vds/docs/decisions.md](../vds/docs/decisions.md) —— 锁定的架构决策
3. [../vds/docs/spec/c-qemu-style.md](../vds/docs/spec/c-qemu-style.md) —— 本仓库的代码规范
4. [../vds/docs/spec/logging.md](../vds/docs/spec/logging.md) 第 3 节 —— C 侧日志规则

要点：
- 遵守 QEMU 自身风格（`docs/devel/style.rst`），改动必须通过 `scripts/checkpatch.pl`。
- 新功能写在新文件（`ui/rdp-*.c`、`audio/rdpaudio.c`、`chardev/char-rdp.c`），已有文件只加挂钩。
- 不使用 D-Bus 显示（`-display dbus`）作为 RDP 的实现途径。
- 不修改与 RDP 无关的 QEMU 代码。
- 只有人类明确要求时才提交；提交信息遵循 `../vds/docs/spec/git-workflow.md` 第 4 节。
