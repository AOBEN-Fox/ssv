# T3/T4 DeerFlow 安全帽复验链路实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**目标：** 在不进入逐帧 Python 检测链路的前提下，新增 `ssvevent`、证据归档和基于 DeerFlow 原始模型 factory 的 OpenAI-compatible/Anthropic 安全帽复验闭环。

**架构：** T3 仅从 `ssvtrack` 的 immutable tracked history 读取 `head` 轨迹和 BGR buffer，生成事件目录及 `ssv:review-candidates`；T4 仅消费候选与 manifest，以 SQLite/outbox 保证幂等，然后通过固定工具顺序调用一个配置好的视觉模型。DeerFlow 原样子树只服务于配置、反射、factory 和 tracing，`deerflow_adapter.py` 是 SSV 领域代码唯一的框架耦合点。

**技术栈：** C++20、GStreamer、GLib、libjpeg、hiredis、nlohmann/json、Python 3.12、Pydantic v2、SQLite、Redis Streams、LangChain、DeerFlow commit `b68e1c686a0cb5a3780089d27354354533451d8e`。

---

本计划实现 [T3/T4 DeerFlow 安全帽复验链路设计](../specs/2026-07-29-T3T4-DeerFlow安全帽复验链路-spec.md)。

实现期间不提交、不推送、不创建 PR；每项完成后保留工作区改动和命令输出。默认 SQLite 账本路径定为 `artifacts/agent-review.sqlite3`，通过新增配置 `agent.ledger_path` 覆盖；它与 `artifacts.events_root` 分离，避免把全局账本置入单一事件目录。

## 最终文件结构与边界

| 文件或目录 | 动作 | 唯一责任 |
| --- | --- | --- |
| `gst/ssv-common/include/ssv_redis_stream.hpp`、`gst/ssv-common/ssv_redis_stream.cpp` | 新增 | 同步 Redis Streams `event` 字段发布器，供 `ssvpub` 和 `ssvevent` 复用。 |
| `gst/ssv-common/include/ssv_config.hpp`、`gst/ssv-common/ssv_config.cpp` | 修改 | 读取候选/结果 Stream 与共享事件根目录；不包含规则和 Agent 状态。 |
| `gst/ssv-pub/gstssvpub.cpp`、`gst/ssv-pub/gstssvpub.hpp` | 修改 | 改用公共发布器，保持 `ssv:events` 的 JSON 和语义逐字兼容。 |
| `gst/ssv-event/` | 新增 | `ssvevent` 插件、轨迹片段聚合、帧缓存、确定性取帧、JPEG/manifest 写入、候选本地待发布和发布重试。 |
| `gst/tests/test_ssv_event_*.cpp`、`gst/tests/test_ssv_redis_stream.cpp` | 新增 | 通过纯 C++ seam 测试规则、UUID、取帧、裁剪、队列和 JSON 契约。 |
| `gst/meson.build`、`gst/ssv-common/meson.build`、`gst/tests/meson.build`、`meson.build` | 修改 | 加入 `ssv-event`、libjpeg 与新增测试目标；保留现有插件链接方式。 |
| `scripts/pipeline.sh` | 修改 | 在 `ssvtrack ! ssvpub` 之间接入 `ssvevent`，从 YAML 传入 source、Redis、规则和共享证据根。 |
| `config/ssv.example.yaml`、`config/agent-rules/head_without_helmet_v1.yaml`、`agent/prompts/vision_review_v1.md` | 修改/新增 | 版本化运行配置、候选/取帧阈值和中文视觉复验提示词。 |
| `agent/src/deerflow/` | 新增 | 从指定 DeerFlow commit 原样迁入的最小闭包；禁止 import `ssv_agent`。 |
| `agent/src/deerflow/UPSTREAM_MANIFEST.json`、`docs/DeerFlow迁移说明.md` | 新增 | 每个迁入文件的来源相对路径、SHA-256、commit、MIT 许可和升级/校验流程。 |
| `agent/src/ssv_agent/review/` | 新增 | T4 领域契约、ports、状态机、SQLite、outbox、四个工具、应用服务和唯一 DeerFlow adapter。 |
| `agent/src/ssv_agent/config.py`、`event_consumer.py`、`service.py`、`cli.py`、`pyproject.toml` | 修改 | 解析新配置、只消费候选流、按终态输出中文摘要并加入运行依赖。 |
| `agent/tests/review/` | 新增 | Pydantic/Redis 契约、工具、adapter、状态机、SQLite/outbox、CLI 与 mock provider 的自动化测试。 |

`ssv_meta`、`ssvinfer`、`ssvtrack` 不增加业务规则或 LLM 字段。`ssvevent` 对每个输入 buffer 只调用 `latest_tracked_at_or_before(GST_BUFFER_PTS(buf))` 读取 immutable snapshot，不调用 `consume_tracked()`，因此 `ssvpub` 仍是唯一消费 tracked publication slot 的插件。

## TDD seam

- C++：`HeadEpisodeEngine`、`EvidenceSelector`、`EvidenceWriter`、`CandidatePublisher`、`ssv_event_build_candidate_payload()`、`ssv_redis_stream_publish()` 和 `ssvevent` GObject 属性。
- Python：`ReviewCandidate`、`EvidenceManifest`、`ReviewDecision`、`ReviewStateMachine`、`ReviewStore`、`ResultPublisher`、四个固定工具及 `ReviewService.process()`。
- 跨语言：Redis `XADD <key> * event <JSON>` 外壳、`schema_version: 1`、候选/结果 fixture、相对 manifest 路径和 `primary_reason_code`。
- 不测试私有 mutex、线程循环、JPEG scanline 缓冲或 SQLite SQL 字符串；这些只能经上述公开 seam 和集成 fixture 验证。

## 实施步骤

### 1. 先冻结配置、规则和跨语言 JSON fixture

**文件：**

- 修改：`config/ssv.example.yaml`、`agent/src/ssv_agent/config.py`、`agent/tests/test_config.py`
- 新增：`config/agent-rules/head_without_helmet_v1.yaml`、`agent/prompts/vision_review_v1.md`、`agent/tests/review/fixtures/review-candidate-v1.json`、`agent/tests/review/fixtures/review-result-v1.json`
- 修改：`gst/ssv-common/include/ssv_config.hpp`、`gst/ssv-common/ssv_config.cpp`、`gst/tests/test_ssv_config.cpp`

- [ ] 先为 Python 与 C++ 配置解析写失败测试，断言默认值为：
  `redis.review_candidate_stream=ssv:review-candidates`、
  `redis.review_result_stream=ssv:review-results`、
  `artifacts.events_root=artifacts/events`、
  `agent.rules_path=config/agent-rules`、
  `agent.ledger_path=artifacts/agent-review.sqlite3`。
- [ ] 在同一失败测试中验证 `agent.review_model` 仅接受 `agent.models[].name` 中的一个名称；每个模型项保留 `name`、`use`、`model` 和额外 provider 参数，不能以 `provider` 枚举替代 `use`。
- [ ] 实现 C++ `SsvConfig` 和 Python Pydantic 配置字段；保留 `redis.stream_key` 与 `redis.consumer_group` 的旧默认值，确保旧 `ssvpub` 链路可独立运行。
- [ ] 将规则 YAML 固定为 `head_without_helmet_v1`：`minimum_duration_ms: 1000`、`minimum_observation_count: 3`、`close_after_missing_ms: 2000`、`automatic_decision_min_confidence: 0.80`、五锚点目标、±250ms、质量权重 `0.30/0.25/0.20/0.15/0.10` 和裁剪 padding `0.25`。
- [ ] 将候选 fixture 固定为单字段 JSON 内容：`type: review_candidate`、`schema_version: 1`、UUIDv5 `event_id`、两类时间字段、相对 manifest 路径和证据覆盖数量；结果 fixture 固定为 `type: review_result`、`result_revision: 1`、合法 reason code 与已存在的 `evidence_anchor_ids`。
- [ ] 运行 `meson test -C build ssv-config --print-errorlogs` 与 `cd agent && uv run --extra dev pytest tests/test_config.py -q`；新增断言必须先失败、实现后通过。

### 2. 提取 Redis Streams 公共发布器并保持 `ssvpub` 兼容

**文件：**

- 新增：`gst/ssv-common/include/ssv_redis_stream.hpp`、`gst/ssv-common/ssv_redis_stream.cpp`、`gst/tests/test_ssv_redis_stream.cpp`
- 修改：`gst/ssv-common/meson.build`、`gst/ssv-pub/gstssvpub.cpp`、`gst/ssv-pub/meson.build`、`gst/tests/meson.build`

- [ ] 写 `ssv_redis_stream_publish(context, stream_key, event_json)` 的失败测试；用 fake command seam 断言它只形成 `XADD <stream> * event <json>`，拒绝空 stream key 或空 JSON，且 Redis 错误返回 `false` 不抛到实时 transform。
- [ ] 实现 RAII `SsvRedisStreamPublisher`：连接、发布、连接错误后显式重连、析构释放 `redisContext`；不在该类实现业务重试、候选状态或 JSON 构造。
- [ ] 将 `ssvpub` 的连接和 `redisCommand("XADD ...")` 替换为该发布器，保留 `ssv_pub_build_event_payload()` 的五个现有字段、`type: detection`、空检测跳过和原插件属性。
- [ ] 扩展 `test_ssv_pub_payload.cpp`，逐字段比较迁移前后 payload；新增公共发布器测试用 fake seam 验证唯一 `event` 字段。
- [ ] 运行 `meson test -C build ssv-pub-payload ssv-redis-stream --print-errorlogs`，随后运行全部 `meson test -C build --print-errorlogs`。

### 3. 以纯 C++ 方式实现 `head` 片段与确定性选帧

**文件：**

- 新增：`gst/ssv-event/ssv_event_types.hpp`、`gst/ssv-event/head_episode_engine.hpp`、`gst/ssv-event/head_episode_engine.cpp`、`gst/ssv-event/evidence_selector.hpp`、`gst/ssv-event/evidence_selector.cpp`、`gst/tests/test_ssv_event_engine.cpp`、`gst/tests/test_ssv_evidence_selector.cpp`

- [ ] 用合成 `SsvTrackedFrame` 写红灯测试：同一 `source + generation + track_id` 的 `head` 在 1000ms 前不能触发、第三次有效观测且达到 1000ms 时恰好触发一次、连续 2000ms 未见 `head` 或 `SSV_TRACK_DEAD` 时关闭 episode、再次出现形成新 episode。
- [ ] 在测试中证明 `helmet` 不参与几何配对：同帧有无 `helmet`、两框是否重叠都不能改变 `head` episode 触发。
- [ ] 实现 `HeadEpisodeEngine::observe(frame, wall_clock_ms)`，仅接收 `class_name == "head"`、有效 `track_id`、当前 generation 的对象；公开 `EpisodeTriggered` 只包含 source、generation、track、开始/触发 PTS、观测数与目标框。
- [ ] 实现 UUIDv5 helper：用 GLib SHA-1 计算固定 namespace 下的
  `source_id + pipeline_generation + track_id + episode_started_pts_ns + rule_id + rule_version`，再设置 UUID version/variant 位；测试固定输入得到固定 RFC 4122 字符串。
- [ ] 为 `EvidenceSelector` 写红灯测试：`trigger` 返回真实触发帧；其余目标为 `-1000/-500/+500/+1000ms` 并仅接受 ±250ms；无候选时缺失且不复用帧；等质量时用较早 PTS 决胜。
- [ ] 实现质量归一化和权重：`sharpness=0.30`、`box_area=0.25`、`confidence=0.20`、`track_stability=0.15`、`target_proximity=0.10`；固定五锚点即时间多样性，不计算图像相似度。
- [ ] 运行两个新测试，随后运行 `meson test -C build --print-errorlogs`，确认现有 meta/track/pub 测试未受影响。

### 4. 实现证据 JPEG、manifest、有限异步写入和本地待发布记录

**文件：**

- 新增：`gst/ssv-event/evidence_writer.hpp`、`gst/ssv-event/evidence_writer.cpp`、`gst/ssv-event/candidate_publisher.hpp`、`gst/ssv-event/candidate_publisher.cpp`、`gst/tests/test_ssv_evidence_writer.cpp`
- 修改：`meson.build`、`gst/ssv-event/meson.build`、`gst/tests/meson.build`

- [ ] 写失败测试，将 BGR fixture 写到临时 `artifacts/events/<event_id>.tmp`，断言最终原子目录包含 `candidate.json`、`evidence-manifest.json`、`evidence/<anchor_id>-full.jpg` 与 `evidence/<anchor_id>-head.jpg`，并且 manifest SHA-256 可复算。
- [ ] 写裁剪红灯测试：normalized `head` 框向四边扩展 25%、钳制图像边界、从原始像素写 JPEG、不放大；任一边小于 64 像素时 manifest 的 `low_resolution` 为真。
- [ ] 用 `libjpeg` 实现 BGR→JPEG encoder；单图限制 1MiB，总图限制 10MiB。超过预算时按已冻结质量顺序裁减非 trigger 图，并在 manifest 写明裁减原因；trigger 图不能被裁减。
- [ ] 实现 `EvidenceWriteQueue`：入队前深拷贝像素，上限 8 个任务或 64MiB；满队、编码异常和磁盘异常立即释放实时线程资源，并生成带 `evidence_unavailable` 的最小 `candidate.json`。
- [ ] 实现 `CandidatePublisher`：从完整目录读取 candidate JSON，使用公共 Redis 发布器写候选流；Redis 不可用时保留目录中的发布状态并以有限退避重试，成功后原子标记已发布。发布器不阻塞 `transform_ip`。
- [ ] 将 libjpeg 作为 Meson 必需依赖并为 `ssv-event` 单独链接；运行 writer/queue/publisher 定向测试和全量 Meson 测试。

### 5. 将 `ssvevent` 接入 GStreamer、配置与 pipeline

**文件：**

- 新增：`gst/ssv-event/gstssvevent.hpp`、`gst/ssv-event/gstssvevent.cpp`、`gst/ssv-event/meson.build`、`gst/tests/test_gst_ssv_event.cpp`
- 修改：`gst/meson.build`、`gst/tests/meson.build`、`scripts/pipeline.sh`、`config/ssv.example.yaml`、`tests/ssv_cli_test.sh`

- [ ] 为 `ssvevent` 写 GStreamer 红灯测试：注册 `source-id`、`redis-host`、`redis-port`、`stream-key`、`events-root`、`rules-path` 属性；将受控 BGR buffer 和 tracked history 推入 `appsrc`，断言插件不消费 publication slot，`ssvpub` 仍能发布同一 tracked snapshot。
- [ ] 实现薄 GObject adapter：`start` 加载固定规则、创建 engine/writer/publisher；`transform_ip` 以当前 buffer PTS 从 `latest_tracked_at_or_before` 读取 snapshot，map BGR buffer，交给 engine 与有限队列；`stop/finalize` 有界停止后台线程并释放资源。
- [ ] 在 `scripts/pipeline.sh` 读取 `redis.review_candidate_stream`、`artifacts.events_root`、`agent.rules_path`，构造 `ssvtrack ! ssvevent ! ssvpub`；`ssvpub` 仍使用 `redis.stream_key`。
- [ ] 扩展 shell CLI 测试，断言最终 pipeline 含一个 `ssvevent`，所有三个插件使用同一 `source-id`，且 `ssv:events` 和 `ssv:review-candidates` 不混用。
- [ ] 运行 `bash -n scripts/pipeline.sh`、定向 GstCheck、`./ssv build`、`meson test -C build --print-errorlogs`；真实 RTSP/Redis 不可用时只记录外部阻塞，不放宽测试契约。

### 6. 原样迁入 DeerFlow 最小闭包并建立可审计来源

**文件：**

- 新增：`agent/src/deerflow/__init__.py`、`agent/src/deerflow/config/`、`agent/src/deerflow/models/`、`agent/src/deerflow/reflection/`、`agent/src/deerflow/tracing/`、`agent/src/deerflow/trace_context.py`、`agent/src/deerflow/UPSTREAM_MANIFEST.json`、`agent/tests/review/test_deerflow_upstream.py`、`docs/DeerFlow迁移说明.md`
- 修改：`agent/pyproject.toml`、`agent/uv.lock`

- [ ] 从 `/mnt/work/deer-flow` 的 commit `b68e1c686a0cb5a3780089d27354354533451d8e` 复制 factory 所需闭包，保持上游相对路径、文件内容和 `deerflow.*` import 不变。闭包包括完整 `config/`、`reflection/`、`tracing/`、`trace_context.py`、`models/factory.py` 及其 import 到的 models 文件；不得迁入通用 tools loader、Sandbox provider、MCP、Skills、Sub-Agent 或业务代码。
- [ ] 生成 `UPSTREAM_MANIFEST.json`，每项包含 `source_path`、`destination_path`、`sha256`、`upstream_commit` 与 `license: MIT`；测试逐个读取 destination 并重算 SHA-256。
- [ ] 在 `docs/DeerFlow迁移说明.md` 记录上游仓库 `/mnt/work/deer-flow`、commit、MIT 许可、迁入范围、禁止迁入范围、校验命令和升级规则；不在仓库根目录新增 notices 文件。
- [ ] 将 hatch wheel 包包含 `src/deerflow`；加入与上游兼容的最小运行依赖：`langchain>=1.2.15`、`langchain-openai>=1.2.1`、`langchain-anthropic>=1.4.1`，并锁定 `uv.lock`。
- [ ] 写 import 红灯测试：`from deerflow.config.model_config import ModelConfig`、`from deerflow.models.factory import create_chat_model`、`from deerflow.reflection import resolve_class` 都可导入；扫描原样子树确认不存在 `ssv_agent` 字符串。
- [ ] 运行 `cd agent && uv lock && uv sync --extra dev && uv run --extra dev pytest tests/review/test_deerflow_upstream.py -q`。若依赖下载被环境阻止，记录阻止原因但不替换为自写 factory。

### 7. 建立 T4 领域契约、规则/证据工具和归档 renderer

**文件：**

- 新增：`agent/src/ssv_agent/review/contracts.py`、`ports.py`、`tools/__init__.py`、`tools/resolve_rule.py`、`tools/read_evidence.py`、`tools/render_review_artifact.py`
- 新增：`agent/tests/review/test_contracts.py`、`test_resolve_rule.py`、`test_read_evidence.py`、`test_render_review_artifact.py`

- [ ] 写 Pydantic 红灯测试，固定 `ReviewCandidate`、`EvidenceManifest`、`EvidenceAnchor`、`ReviewDecision`、`ReviewResult` 的字段、整数时间单位、三种 decision、reason-code 映射、`schema_version == 1` 和 `result_revision == 1`。
- [ ] 实现 `resolve_rule(candidate)`：只从 `agent.rules_path/<rule_id>.yaml` 读取，规则 ID/version 与候选不一致即拒绝；不让模型传入任意路径或选择规则。
- [ ] 实现 `read_evidence(candidate)`：只接受 `<event_id>/evidence-manifest.json`，`resolve()` 后仍必须位于 `artifacts.events_root`，事件目录必须等于 JSON `event_id`；校验 manifest 中每个已选文件的相对路径、SHA-256、图片数量和总预算。
- [ ] 在测试中覆盖绝对路径、`..`、软链接逃逸、错误 event 目录、hash 不符、缺少文件与 `evidence_unavailable`；这些情况产生可发布的 `needs_human_review` 所需上下文，而不是读取本机任意文件。
- [ ] 实现 `render_review_artifact`：原子写 `result/review-result.json` 和中文 `result/review-summary.md`；Markdown 只呈现 decision、置信度、主原因、已引用锚点、证据摘要和建议，不保存隐藏推理。
- [ ] 运行 `cd agent && uv run --extra dev pytest tests/review/test_contracts.py tests/review/test_resolve_rule.py tests/review/test_read_evidence.py tests/review/test_render_review_artifact.py -q`。

### 8. 通过唯一 DeerFlow adapter 实现视觉模型复验

**文件：**

- 新增：`agent/src/ssv_agent/review/deerflow_adapter.py`、`tools/review_vision.py`、`agent/tests/review/test_deerflow_adapter.py`、`agent/tests/review/test_review_vision.py`
- 修改：`agent/src/ssv_agent/review/tools/__init__.py`

- [ ] 写红灯测试，证明 SSV 中只有 `deerflow_adapter.py` import `deerflow.*`；其余 `ssv_agent/review` 文件只能依赖 ports/contracts。
- [ ] 在 adapter 中以原样 `ModelConfig`、`reflection.resolve_class()` 和 `create_chat_model(name=..., app_config=..., attach_tracing=False)` 构建唯一模型。用 `AppConfig.model_validate({"models": models, "sandbox": {"use": "deerflow.sandbox.local:LocalSandboxProvider"}, "tools": []})` 构造 factory 所需的原样配置；Sandbox class path 仅满足未使用的配置字段，首版绝不实例化 Sandbox，也不得改写 `use` provider class path。
- [ ] 用 copied `AppConfig.resolve_env_variables()` 解析 `$SSV_AGENT_OPENAI_API_KEY`、`$SSV_AGENT_OPENAI_BASE_URL`、`$SSV_AGENT_ANTHROPIC_API_KEY`，随后校验选中模型 `supports_vision is true`。provider/model/endpoint 只在服务启动解析一次。
- [ ] 将 `EvidenceBundle` 转成 LangChain `HumanMessage`：中文规则与检测摘要为文本，已校验 JPEG 为 data URL 图片；请求不含本机路径、SQLite、Redis 字段或密钥。
- [ ] 使用 Pydantic structured output 得到 `ReviewDecision`；模型输出必须引用 manifest 中存在的 `evidence_anchor_ids`。第一次结构化校验失败时发送一次仅要求修复 JSON 的请求，第二次失败映射为 `needs_human_review + invalid_model_output`。
- [ ] 用 OpenAI-compatible 与 Anthropic fake chat model 分别测试图片消息、配置 class path、单 provider 调用、低于 0.80 强制人工复核、provider 不可用映射 `provider_unavailable`；测试不得访问网络。

### 9. 实现 SQLite 账本、outbox、状态机和候选 consumer

**文件：**

- 新增：`agent/src/ssv_agent/review/state_machine.py`、`store.py`、`outbox.py`、`service.py`
- 修改：`agent/src/ssv_agent/event_consumer.py`、`agent/src/ssv_agent/service.py`
- 新增：`agent/tests/review/test_state_machine.py`、`test_store.py`、`test_outbox.py`、`test_review_service.py`、`test_review_consumer.py`

- [ ] 用临时 SQLite 写红灯测试：`event_id` 原子领取、已完成事件不会再次调用 `review_vision`、outbox result 与终态同一事务写入、重复发布保持 `result_revision: 1`。
- [ ] 实现状态机 `pending → claimed → loading_evidence → calling_model → validating_result → completed|manual_review`；`failed` 只保存当前运行期可恢复故障，不发布业务结论且不 ACK。
- [ ] 实现错误映射：429/5xx/超时有限退避后进入 `manual_review`；模型证据不足、低置信度、不可用、无效 JSON 都是 `needs_human_review`；SQLite、配置或结果归档不可用为未 ACK 的 `failed`。outbox Redis 发送失败不回滚已持久化结果。
- [ ] 将现有 detection consumer 替换为 candidate consumer：只读取 `redis.review_candidate_stream` 的 `event` 字段，首次建组使用 `$`，consumer 名为 `<hostname>-<pid>`，单次只分派一个事件给单 worker。
- [ ] 实现实时消费边界：启动前未读消息与旧 pending 仅写 `expired` 审计并 ACK；本次运行期间的新事件按 Stream 顺序等待，不设模型/排队时限。只为过期 ACK 而认领旧 pending，绝不复验它们。
- [ ] outbox loop 发布 `type: review_result` 到 `redis.review_result_stream`；成功持久化终态和 outbox 后 ACK 输入，Redis 暂不可用时稍后重发结果且不重新调用模型。
- [ ] 运行上述定向 Python 测试，覆盖 malformed `event` 不 ACK、旧 detection 流绝不订阅、终态重复输入、旧 pending 过期和 outbox 重发。

### 10. 接入 CLI、运行日志和端到端 mock 验收

**文件：**

- 修改：`agent/src/ssv_agent/cli.py`、`agent/src/ssv_agent/logging.py`、`agent/src/ssv_agent/service.py`、`scripts/agent.sh`
- 新增：`agent/tests/review/test_cli.py`、`agent/tests/review/test_end_to_end.py`
- 修改：`agent/tests/test_service.py`、`agent/tests/test_event_consumer.py`

- [ ] 写 CLI 红灯测试：`INFO` 只输出 `completed` 或 `manual_review` 的中文单行摘要；`failed` 是 `ERROR` 的可恢复故障信息；`DEBUG` 才输出领取、证据、模型、发布、候选到达间隔、调用耗时、积压量和最老待处理候选年龄。
- [ ] 让 `./ssv agent`/`scripts/agent.sh` 继续使用现有 `ssv-agent` entry point，只启动候选 consumer；不得打印 `ssv:events` 的逐帧 detection 内容。
- [ ] 写完整 mock fixture：候选 JSON、五锚点 JPEG、manifest、规则和 fake provider，断言生成 SQLite 账本、`result/review-result.json`、`result/review-summary.md`、单字段 `review_result` Stream 消息与一次 INFO 摘要。
- [ ] 增加缺证据、低置信度、provider unavailable、无效 JSON 修复失败、outbox 短暂失败与已完成候选重复到达的集成断言。
- [ ] 运行 `cd agent && uv run --extra dev pytest -q`、`cd agent && uv run --extra dev ruff check src tests`；真实 API key 缺失时不发网络请求。

### 11. 完成文档、全量验证和回滚检查

**文件：**

- 修改：`docs/specs/2026-07-29-T3T4-DeerFlow安全帽复验链路-spec.md`、本计划
- 新增或修改：`docs/DeerFlow迁移说明.md`

- [ ] 对照 spec 的目标、非范围、T2/T3/T4 边界、五锚点、证据预算、实时消费、状态机、provider、工具、归档、CLI、回滚逐项勾验；任何跨语言字段差异同时修改 fixture、Pydantic 和 C++ payload 测试。
- [ ] 运行 `./ssv build`、`meson test -C build --print-errorlogs`、`cd agent && uv run --extra dev pytest -q`、`cd agent && uv run --extra dev ruff check src tests`、`bash tests/ssv_cli_test.sh` 与 `bash -n scripts/pipeline.sh`。
- [ ] 仅当 Redis、RTSP、模型和密钥均存在时运行 `./ssv test` 与真实 provider smoke；默认 CI/mocked tests 不调用外部 API。缺失外部条件时在验证记录写明原因，不降级为假通过。
- [ ] 扫描 `agent/src/deerflow` 的 manifest/hash、`ssv_agent` 对 `deerflow.*` 的唯一 import 边界、禁止的逐帧 Agent 路径、文档占位标记、`git diff --check` 和 `git status --short`。
- [ ] 回滚演练仅从 pipeline 删除 `ssvevent` 并停止 Agent；确认 `ssvinfer → ssvtrack → ssvpub` 和 `ssv:events` 仍正常，既有事件目录、SQLite 和结果 Stream 不被自动删除。

## 完成定义

- `head` 连续轨迹只在 1 秒/3 次有效观测后生成一次 UUIDv5 候选；`helmet` 不参与几何匹配。
- T3 在不阻塞实时 buffer 的条件下归档至多十张经 SHA-256 校验的证据，候选只发布到 `ssv:review-candidates`。
- T4 只处理在线期的新候选，固定单 worker、单 provider、固定四工具顺序；所有可发布结果均有 schema v1、revision 1、固定 reason code、有效锚点引用和 JSON/Markdown/CLI/Redis 四种呈现。
- DeerFlow 原样迁入文件可由 manifest 回溯到指定 MIT commit，SSV 没有复制其通用 Sandbox/MCP/Skills/Sub-Agent 工具系统。
- 全部适用单元、契约、集成、格式和构建验证有明确结果；外部依赖验证的阻塞原因被准确记录。
