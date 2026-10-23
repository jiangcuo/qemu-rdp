# AGENTS.md — QEMU fork（RDP 补丁）

这是自家 QEMU fork 的 QEMU 11 分支：基线 v11.1.1，RDP 相关补丁在分支 `rdp-11` 上
（QEMU 10.0 的版本在分支 `rdp`，基线 v10.0.10）。两条分支的 RDP 补丁功能保持一致，改一边要同步另一边；
QEMU 11 的接口差异主要在音频后端（QOM 类型 `audio-rdp`、`audio_be_*`）和控制台（`qemu_console_*`）。

**开始任务前必须阅读：**
1. [../vds/AGENTS.md](../vds/AGENTS.md) —— 通用规则（锁定决策、范围纪律、禁止事项、完成前检查、提交规则），本仓库同样适用
2. [../vds/docs/spec/c-qemu-style.md](../vds/docs/spec/c-qemu-style.md) —— 本仓库的代码规范
3. [../vds/docs/spec/logging.md](../vds/docs/spec/logging.md) 第 3 节 —— C 侧日志规则

要点：
- 遵守 QEMU 自身风格（`docs/devel/style.rst`），改动必须通过 `scripts/checkpatch.pl`。
- 新功能写在新文件（`ui/rdp-*.c`、`audio/rdpaudio.c`、`chardev/char-rdp.c`），已有文件只加挂钩。
- 不使用 D-Bus 显示（`-display dbus`）作为 RDP 的实现途径。
- 不修改与 RDP 无关的 QEMU 代码。
- 只有人类明确要求时才提交；提交规则见 `../vds/AGENTS.md` 第 7 节。
