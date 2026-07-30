# DeerFlow 最小迁移说明

本 Demo 从本机上游仓库 `/mnt/work/deer-flow` 的固定 commit
`b68e1c686a0cb5a3780089d27354354533451d8e` 迁入最小 model factory
import 闭包。迁入文件使用 MIT 许可，许可原文位于
`agent/src/deerflow/LICENSE`。

同步范围仅包括 `config/`、`models/`、`reflection/`、`tracing/` 及其
必要顶层模块；不迁入 `agents/`、`runtime/`、`sandbox/`、`tools/`、
`community/`、`skills/` 或缓存文件。`UPSTREAM_MANIFEST.json` 记录每个
迁入文件的源路径、commit、SHA-256 与许可。

SSV 业务代码只能在 `agent/src/ssv_agent/review/deerflow_adapter.py` 中直接
import `deerflow.*`。升级时先更新 commit 与 allowlist，再运行：

```bash
cd agent
uv run python scripts/sync_deerflow.py \
  --upstream /mnt/work/deer-flow \
  --commit b68e1c686a0cb5a3780089d27354354533451d8e
uv run --extra dev pytest tests/review/test_deerflow_upstream.py -q
```
