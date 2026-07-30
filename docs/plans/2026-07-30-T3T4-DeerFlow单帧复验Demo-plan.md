# T3/T4 DeerFlow 单帧复验 Demo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不新增 `ssvevent`、不让 Python 进入逐帧检测链路的前提下，用真实安全帽模型产生的 `head` 轨迹触发一次完整原始帧证据，并通过 DeerFlow 原始 model factory 跑通候选消费、视觉复验和结果落盘 Demo。

**Architecture:** T3 直接扩展现有 `ssvpub`：继续原样发布 `ssv:events` detection，同时按 `source + pipeline_generation + track_id + rule_id + rule_version` 去重，把首次有效 `head` 对应的当前 BGR 完整帧编码为 JPEG，并发布独立 `review_candidate`。T4 只消费候选，通过唯一 `deerflow_adapter.py` 构造模型，按固定 `read_evidence → review_vision → save_review_result` 顺序执行；`AppConfig.tools=[]`，模型看不到文件、Redis 或其他工具。

**Tech Stack:** C++20、GStreamer、GLib checksum、libjpeg、hiredis、nlohmann/json、Meson、Python 3.12、Pydantic v2、Pillow、Redis Streams、LangChain、DeerFlow commit `b68e1c686a0cb5a3780089d27354354533451d8e`。

---

## 实施边界

本计划实现 [DeerFlow 单帧安全帽复验 Demo 设计](../specs/2026-07-30-T3T4-DeerFlow单帧复验Demo-spec.md)，属于 T3/T4 跨主线改动，并由 T5 补齐构建、契约测试和 Demo 验收。

以下文件是用户已有的独立完整方案文档，本计划不得修改、重写、暂存或回退：

- `docs/specs/2026-07-29-T3T4-DeerFlow安全帽复验链路-spec.md`
- `docs/plans/2026-07-29-T3T4-DeerFlow安全帽复验链路-plan.md`

本 Demo 固定遵守以下约束：

- 检测输入使用 `models/comp-2-freeze10.onnx`、`config/model-labels/helmet.txt` 和空 `inference.target_class`。
- 候选只接受 `class_name == "head"`；`helmet` 仍保留在 detection JSON，但不参与候选触发和几何配对。
- 不使用 `person`、`mock-detect`、`mock-track` 代替真实业务输入。
- 每个事件只保存当前分析 buffer 的一张完整 JPEG；不裁剪、不缩放、不绘框。
- 不新增 GStreamer 插件，不实现多帧、episode、SQLite、outbox、LangGraph、MCP、Skills、Sub-Agent 或模型自主 tool calling。
- 默认自动化闭环使用经 DeerFlow factory 创建的 `MockVisionChatModel`；真实 OpenAI-compatible 请求仅作为显式配置后的 smoke。

## 文件结构与职责

| 文件 | 操作 | 单一职责 |
|---|---|---|
| `gst/ssv-pub/ssv_review_candidate.hpp`、`gst/ssv-pub/ssv_review_candidate.cpp` | 新增 | `head` 候选筛选、UUIDv5、去重键、candidate JSON、归档后发布语义。 |
| `gst/ssv-pub/ssv_frame_evidence.hpp`、`gst/ssv-pub/ssv_frame_evidence.cpp` | 新增 | BGR 完整帧 JPEG 编码、SHA-256 和原子文件写入。 |
| `gst/ssv-pub/gstssvpub.hpp`、`gst/ssv-pub/gstssvpub.cpp` | 修改 | 将当前 tracked snapshot 与当前 BGR buffer 接入单帧候选，不改变 detection payload。 |
| `gst/tests/test_ssv_review_candidate.cpp`、`gst/tests/test_ssv_frame_evidence.cpp` | 新增 | T3 纯函数、JPEG 和失败后重试测试。 |
| `gst/tests/test_ssv_pub_payload.cpp`、`gst/tests/test_gst_plugins.cpp` | 修改 | 冻结旧 detection JSON，并验证新增插件属性。 |
| `meson.build`、`gst/ssv-pub/meson.build`、`gst/tests/meson.build`、`scripts/build.sh` | 修改 | 接入 libjpeg、两个实现文件和测试目标。 |
| `config/ssv.example.yaml`、`scripts/pipeline.sh`、`tests/ssv_cli_test.sh` | 修改 | 冻结 review 配置、真实安全帽模型示例和 `ssvpub` 属性传递。 |
| `agent/scripts/sync_deerflow.py` | 新增 | 从固定 commit 按 allowlist 原样迁入 DeerFlow import 闭包并生成 manifest。 |
| `agent/src/deerflow/`、`agent/src/deerflow/UPSTREAM_MANIFEST.json` | 新增 | 未修改的 DeerFlow model/config/reflection/tracing 最小闭包与可审计来源。 |
| `agent/src/ssv_agent/review/contracts.py` | 新增 | candidate、decision、result 和 evidence bundle 的 Pydantic 契约。 |
| `agent/src/ssv_agent/review/deerflow_adapter.py` | 新增 | SSV 中唯一允许 import `deerflow.*` 的业务适配层。 |
| `agent/src/ssv_agent/review/mock_provider.py` | 新增 | 可控、无网络、仍由 DeerFlow factory 创建的视觉 chat model。 |
| `agent/src/ssv_agent/review/tools/read_evidence.py` | 新增 | 受限路径、JPEG 可读性和 SHA-256 校验。 |
| `agent/src/ssv_agent/review/tools/review_vision.py` | 新增 | 构造单图 vision message、结构化输出和失败映射。 |
| `agent/src/ssv_agent/review/tools/save_review_result.py` | 新增 | 原子写 `review-result.json`，结果 Stream best effort。 |
| `agent/src/ssv_agent/review/processor.py` | 新增 | 固定串联三个内部能力，并执行轻量幂等。 |
| `agent/src/ssv_agent/event_consumer.py`、`agent/src/ssv_agent/service.py` | 修改 | 从 detection 日志消费者切换为 review candidate 消费者。 |
| `agent/prompts/vision_review_v1.md` | 新增 | 固定中文单帧安全帽复验规则，不保存隐藏推理。 |
| `agent/tests/review/` | 新增 | DeerFlow 来源、契约、工具、幂等和端到端测试。 |
| `agent/pyproject.toml`、`agent/uv.lock` | 修改 | 打包 `deerflow` 子树并加入最小 LangChain/Pillow 依赖。 |
| `docs/DeerFlow迁移说明.md` | 新增 | 记录上游 commit、许可、allowlist、升级和校验命令。 |

## Agent 工具边界

实现完成后，模型可见工具必须严格为零：

```python
app_config = AppConfig.model_validate(
    {
        "models": resolved_models,
        "sandbox": {"use": "deerflow.sandbox.local:LocalSandboxProvider"},
        "tools": [],
    }
)
```

`read_evidence`、`review_vision`、`save_review_result` 是 Agent 进程内的确定性函数，不注册为 LangChain tool，不允许模型选择顺序、参数路径、Redis key 或落盘位置。

### Task 1: 冻结配置和跨语言 JSON fixture

**Files:**

- Modify: `config/ssv.example.yaml`
- Modify: `agent/src/ssv_agent/config.py`
- Modify: `agent/tests/test_config.py`
- Create: `agent/tests/review/fixtures/review-candidate-v1.json`
- Create: `agent/tests/review/fixtures/review-result-v1.json`

- [ ] **Step 1: 先写 Python 配置红灯测试**

在 `agent/tests/test_config.py` 增加默认值、真实安全帽模型示例和启用 review 时模型选择校验：

```python
def test_review_defaults_are_disabled() -> None:
    cfg = SsvConfig()
    assert cfg.redis.review_candidate_stream == "ssv:review-candidates"
    assert cfg.redis.review_result_stream == "ssv:review-results"
    assert cfg.artifacts.events_root == Path("artifacts/events")
    assert cfg.review.enabled is False
    assert cfg.review.automatic_decision_min_confidence == 0.80
    assert cfg.agent.review_model == ""
    assert cfg.agent.models == []


def test_enabled_review_requires_one_vision_model() -> None:
    with pytest.raises(ValueError, match="agent.review_model"):
        SsvConfig.model_validate({"review": {"enabled": True}})

    cfg = SsvConfig.model_validate(
        {
            "review": {"enabled": True},
            "agent": {
                "review_model": "demo-vision",
                "models": [
                    {
                        "name": "demo-vision",
                        "use": "ssv_agent.review.mock_provider:MockVisionChatModel",
                        "model": "demo-vision",
                        "supports_vision": True,
                    }
                ],
            },
        }
    )
    assert cfg.agent.models[0].supports_vision is True
```

- [ ] **Step 2: 运行配置测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/test_config.py -q`

Expected: FAIL，错误包含 `RedisConfig has no attribute 'review_candidate_stream'` 或 `SsvConfig has no attribute 'review'`。

- [ ] **Step 3: 实现最小配置模型和跨字段校验**

在 `agent/src/ssv_agent/config.py` 增加以下模型；保留现有 `state_machine_timeout` 和 `max_retries` 字段以兼容旧 YAML，但本 Demo 不使用它们：

```python
from pydantic import BaseModel, ConfigDict, Field, model_validator


class ReviewModelConfig(BaseModel):
    model_config = ConfigDict(extra="allow")

    name: str
    use: str
    model: str
    supports_vision: bool = False


class ArtifactsConfig(BaseModel):
    events_root: Path = Path("artifacts/events")


class ReviewConfig(BaseModel):
    enabled: bool = False
    automatic_decision_min_confidence: float = Field(default=0.80, ge=0.0, le=1.0)


class AgentConfig(BaseModel):
    state_machine_timeout: int = 300
    max_retries: int = 3
    review_model: str = ""
    models: list[ReviewModelConfig] = Field(default_factory=list)


class SsvConfig(BaseModel):
    # 保留已有字段
    artifacts: ArtifactsConfig = Field(default_factory=ArtifactsConfig)
    review: ReviewConfig = Field(default_factory=ReviewConfig)

    @model_validator(mode="after")
    def validate_review_model(self) -> "SsvConfig":
        if not self.review.enabled:
            return self
        selected = [model for model in self.agent.models if model.name == self.agent.review_model]
        if len(selected) != 1:
            raise ValueError("review.enabled=true 时 agent.review_model 必须唯一匹配 agent.models[].name")
        if not selected[0].supports_vision:
            raise ValueError("agent.review_model 必须配置 supports_vision=true")
        return self
```

同时给 `RedisConfig` 增加：

```python
review_candidate_stream: str = "ssv:review-candidates"
review_result_stream: str = "ssv:review-results"
```

- [ ] **Step 4: 更新示例 YAML，明确真实安全帽模型和关闭态默认值**

在 `config/ssv.example.yaml` 固定以下内容：

```yaml
redis:
  host: "localhost"
  port: 6379
  db: 0
  stream_key: "ssv:events"
  review_candidate_stream: "ssv:review-candidates"
  review_result_stream: "ssv:review-results"
  consumer_group: "ssv-agent"

inference:
  model_path: "models/comp-2-freeze10.onnx"
  target_class: ""
  label_map: "config/model-labels/helmet.txt"

artifacts:
  events_root: "artifacts/events"

review:
  enabled: false
  automatic_decision_min_confidence: 0.80

agent:
  state_machine_timeout: 300
  max_retries: 3
  review_model: ""
  models: []
```

不要在示例 YAML 中写 API key。真实 provider 只允许写 `$SSV_AGENT_OPENAI_API_KEY` 和 `$SSV_AGENT_OPENAI_BASE_URL` 引用。

- [ ] **Step 5: 写入 schema v1 fixture**

`review-candidate-v1.json` 使用固定 UUIDv5 样例：

```json
{
  "type": "review_candidate",
  "schema_version": 1,
  "event_id": "4816f729-74d2-5eba-8f2d-3f2ad285f53e",
  "source": "camera-01",
  "pipeline_generation": 3,
  "frame_id": 128,
  "media_pts_ns": 6200000000,
  "timestamp_ms": 1785400000000,
  "track_id": 12,
  "rule_id": "head_without_helmet_single_frame",
  "rule_version": 1,
  "candidate_class": "head",
  "detection_confidence": 0.91,
  "bbox": [0.20, 0.10, 0.38, 0.32],
  "evidence_path": "4816f729-74d2-5eba-8f2d-3f2ad285f53e/evidence.jpg",
  "evidence_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "evidence_width": 640,
  "evidence_height": 480
}
```

`review-result-v1.json` 固定为：

```json
{
  "type": "review_result",
  "schema_version": 1,
  "event_id": "4816f729-74d2-5eba-8f2d-3f2ad285f53e",
  "source": "camera-01",
  "track_id": 12,
  "rule_id": "head_without_helmet_single_frame",
  "rule_version": 1,
  "decision": "confirmed_no_helmet",
  "review_confidence": 0.93,
  "primary_reason_code": "no_helmet_visible",
  "evidence_summary": "目标头部清晰可见，未观察到安全帽。",
  "recommended_action": "生成未佩戴安全帽复验记录。",
  "provider": "mock",
  "model": "demo-vision",
  "completed_at_ms": 1785400001200
}
```

- [ ] **Step 6: 运行配置测试并提交**

Run: `cd agent && uv run --extra dev pytest tests/test_config.py -q`

Expected: PASS。

```bash
git add config/ssv.example.yaml agent/src/ssv_agent/config.py agent/tests/test_config.py agent/tests/review/fixtures
git commit -m "feat: freeze single-frame review config"
```

### Task 2: 实现 `head` 候选、UUIDv5 和 generation 内去重

**Files:**

- Create: `gst/ssv-pub/ssv_review_candidate.hpp`
- Create: `gst/ssv-pub/ssv_review_candidate.cpp`
- Create: `gst/tests/test_ssv_review_candidate.cpp`
- Modify: `gst/ssv-pub/meson.build`
- Modify: `gst/tests/meson.build`

- [ ] **Step 1: 写候选纯函数红灯测试并接入 Meson 目标**

测试至少包含以下断言：

```cpp
assert(ssv_review_event_id("camera-01", 3, 12) ==
       "4816f729-74d2-5eba-8f2d-3f2ad285f53e");
assert(ssv_review_canonical_name("camera-01", 3, 12) ==
       "ssv://review/camera-01/3/12/head_without_helmet_single_frame/1");

auto head = make_object("head", 12, SSV_TRACK_NEW, 0.91F,
                        0.20F, 0.10F, 0.38F, 0.32F);
assert(ssv_review_object_is_eligible(head));
assert(!ssv_review_object_is_eligible(
    make_object("helmet", 12, SSV_TRACK_NEW, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));
assert(!ssv_review_object_is_eligible(
    make_object("head", -1, SSV_TRACK_NEW, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));
assert(!ssv_review_object_is_eligible(
    make_object("head", 12, SSV_TRACK_LOST, 0.91F, 0.20F, 0.10F, 0.38F, 0.32F)));

SsvReviewDeduplicator dedup;
dedup.reset_for_generation(3);
const auto key = ssv_review_dedup_key("camera-01", 3, 12);
assert(!dedup.already_published(key));
dedup.mark_published(key);
assert(dedup.already_published(key));
dedup.reset_for_generation(4);
assert(!dedup.already_published(ssv_review_dedup_key("camera-01", 4, 12)));

auto candidate = ssv_review_make_candidate(
    frame, head, 1785400000000LL,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    640, 480);
assert(candidate.has_value());
const auto payload = nlohmann::json::parse(
    ssv_review_candidate_json(*candidate));
assert(payload.size() == 18);
assert(payload["pipeline_generation"] == 3);
assert(payload["media_pts_ns"] == frame.timing.pts);
assert(payload["candidate_class"] == "head");
```

Meson 目标名固定为 `ssv-review-candidate-test`，测试名固定为 `ssv-review-candidate`。

- [ ] **Step 2: 编译并确认链接先失败**

Run: `meson setup build --reconfigure && meson compile -C build ssv-review-candidate-test`

Expected: FAIL，错误为缺少 `ssv_review_candidate.hpp` 或链接错误 `undefined reference to ssv_review_event_id`。

- [ ] **Step 3: 定义稳定 C++ 接口**

`ssv_review_candidate.hpp` 暴露以下接口，后续任务不得改名：

```cpp
#include "ssv_meta.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

inline constexpr std::string_view SSV_REVIEW_RULE_ID =
    "head_without_helmet_single_frame";
inline constexpr int SSV_REVIEW_RULE_VERSION = 1;

struct SsvReviewCandidate {
    std::string event_id;
    std::string source;
    std::uint64_t pipeline_generation = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t media_pts_ns = 0;
    std::int64_t timestamp_ms = 0;
    int track_id = -1;
    float detection_confidence = 0.0F;
    std::array<float, 4> bbox = {};
    std::string evidence_path;
    std::string evidence_sha256;
    std::uint32_t evidence_width = 0;
    std::uint32_t evidence_height = 0;
};

std::string ssv_review_canonical_name(
    std::string_view source, std::uint64_t generation, int track_id);
std::string ssv_review_event_id(
    std::string_view source, std::uint64_t generation, int track_id);
std::string ssv_review_dedup_key(
    std::string_view source, std::uint64_t generation, int track_id);
bool ssv_review_object_is_eligible(const SsvTrackedObject &object);
std::optional<SsvReviewCandidate> ssv_review_make_candidate(
    const SsvTrackedFrame &frame,
    const SsvTrackedObject &object,
    std::int64_t timestamp_ms,
    std::string evidence_sha256,
    std::uint32_t evidence_width,
    std::uint32_t evidence_height);
std::string ssv_review_candidate_json(const SsvReviewCandidate &candidate);

class SsvReviewDeduplicator {
public:
    void reset_for_generation(std::uint64_t generation);
    bool already_published(std::string_view key) const;
    void mark_published(std::string key);

private:
    std::uint64_t generation_ = 0;
    std::unordered_set<std::string> published_;
};
```

- [ ] **Step 4: 实现 URL namespace UUIDv5、筛选和 JSON**

UUIDv5 使用标准 URL namespace `6ba7b811-9dad-11d1-80b4-00c04fd430c8`。实现步骤固定为：namespace 原始 16 bytes + canonical UTF-8 bytes → GLib SHA-1 → 取前 16 bytes → 设置 version nibble 为 `5`、variant 为 `10xx` → RFC 4122 小写字符串。

`ssv_review_dedup_key()` 直接返回同一个 canonical name，因此 key 已完整包含 `source/generation/track_id/rule_id/rule_version`，不会再维护第二套拼接格式。

`ssv_review_object_is_eligible()` 必须同时检查：

```cpp
return class_name == "head" &&
    object.track_id >= 0 &&
    (object.track_state == SSV_TRACK_NEW ||
     object.track_state == SSV_TRACK_MATCHED) &&
    std::isfinite(d.x1) && std::isfinite(d.y1) &&
    std::isfinite(d.x2) && std::isfinite(d.y2) &&
    0.0F <= d.x1 && d.x1 < d.x2 && d.x2 <= 1.0F &&
    0.0F <= d.y1 && d.y1 < d.y2 && d.y2 <= 1.0F;
```

candidate JSON 必须恰好包含 spec 中的 18 个字段，不加入 `track_state`、`occluded` 或绝对路径。

`ssv_review_make_candidate()` 在 source 为空、PTS 为 `GST_CLOCK_TIME_NONE`、object 不合格、SHA-256 不是 64 位小写十六进制或证据尺寸为 0 时返回 `std::nullopt`；否则从 frame/object 生成 `event_id`、`<event_id>/evidence.jpg`、generation、frame/PTS、置信度和 bbox。

- [ ] **Step 5: 运行候选测试**

Run: `meson compile -C build ssv-review-candidate-test && meson test -C build ssv-review-candidate --print-errorlogs`

Expected: PASS，固定 UUID 为 `4816f729-74d2-5eba-8f2d-3f2ad285f53e`。

- [ ] **Step 6: 提交候选契约**

```bash
git add gst/ssv-pub/ssv_review_candidate.* gst/tests/test_ssv_review_candidate.cpp gst/ssv-pub/meson.build gst/tests/meson.build
git commit -m "feat: add single-frame review candidate contract"
```

### Task 3: 编码完整 BGR 原始帧并原子写 JPEG

**Files:**

- Create: `gst/ssv-pub/ssv_frame_evidence.hpp`
- Create: `gst/ssv-pub/ssv_frame_evidence.cpp`
- Create: `gst/tests/test_ssv_frame_evidence.cpp`
- Modify: `meson.build`
- Modify: `gst/ssv-pub/meson.build`
- Modify: `gst/tests/meson.build`
- Modify: `scripts/build.sh`

- [ ] **Step 1: 写完整帧 JPEG 红灯测试**

测试构造 `4x3` BGR 图，stride 固定为 `16`，每行末尾 4 bytes padding；这样可以证明实现按 stride 读取，而不是把 buffer 错当连续 `width * 3`：

```cpp
std::array<std::uint8_t, 48> pixels = make_bgr_fixture_with_padding();
std::string error;
auto evidence = ssv_encode_bgr_jpeg(
    pixels.data(), 4, 3, 16, 90, &error);
assert(evidence.has_value());
assert(evidence->width == 4);
assert(evidence->height == 3);
assert(evidence->sha256.size() == 64);

const auto output = temp_dir / "evidence.jpg";
assert(ssv_write_atomic_bytes(output, evidence->jpeg_bytes, &error));
assert(std::filesystem::exists(output));
for (const auto &entry : std::filesystem::directory_iterator(temp_dir))
    assert(entry.path().filename().string().rfind(".evidence.jpg.tmp-", 0) != 0);
assert(decode_jpeg_dimensions(output) == std::pair(4, 3));
```

同时断言 quality 固定为 `90`、空指针、非正尺寸、`stride < width * 3` 时返回 `std::nullopt`。

- [ ] **Step 2: 编译并确认先失败**

Run: `meson setup build --reconfigure && meson compile -C build ssv-frame-evidence-test`

Expected: FAIL，错误包含 `ssv_frame_evidence.hpp: No such file` 或未定义符号。

- [ ] **Step 3: 加入 libjpeg 构建依赖**

`meson.build` 增加：

```meson
jpeg_dep = dependency('libjpeg', required : true)
```

`scripts/build.sh` 的基础依赖检查列表加入 `libjpeg`。`gst/ssv-pub/meson.build` 和新测试目标只链接 `jpeg_dep`，不把 OpenCV 变为 `ssvpub` 的必需依赖。

- [ ] **Step 4: 实现固定接口**

`ssv_frame_evidence.hpp` 定义：

```cpp
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct SsvFrameEvidence {
    std::vector<std::uint8_t> jpeg_bytes;
    std::string sha256;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::optional<SsvFrameEvidence> ssv_encode_bgr_jpeg(
    const std::uint8_t *data,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t stride,
    int quality,
    std::string *error);

std::string ssv_sha256_hex(std::span<const std::uint8_t> bytes);

bool ssv_write_atomic_bytes(
    const std::filesystem::path &final_path,
    std::span<const std::uint8_t> bytes,
    std::string *error);
```

编码器为每条 scanline 按 `B,G,R → R,G,B` 写入临时行缓冲，再以标准 `JCS_RGB` 交给 libjpeg；只做 JPEG 所需的颜色通道转换，不改变尺寸或像素位置。不得调用 resize、crop、overlay 或颜色框绘制。原子写采用同目录 `.<filename>.tmp-<pid>`，完整写入并关闭后用 `std::filesystem::rename` 替换最终文件。

- [ ] **Step 5: 运行 JPEG 测试**

Run: `meson compile -C build ssv-frame-evidence-test && meson test -C build ssv-frame-evidence --print-errorlogs`

Expected: PASS，解码尺寸仍为 `4x3`，SHA-256 可由最终文件字节复算。

- [ ] **Step 6: 提交证据编码**

```bash
git add meson.build scripts/build.sh gst/ssv-pub/ssv_frame_evidence.* gst/ssv-pub/meson.build gst/tests/test_ssv_frame_evidence.cpp gst/tests/meson.build
git commit -m "feat: archive full-frame jpeg evidence"
```

### Task 4: 把单帧候选接入现有 `ssvpub`、pipeline 和 CLI 测试

**Files:**

- Modify: `gst/ssv-pub/ssv_review_candidate.hpp`
- Modify: `gst/ssv-pub/ssv_review_candidate.cpp`
- Modify: `gst/ssv-pub/gstssvpub.hpp`
- Modify: `gst/ssv-pub/gstssvpub.cpp`
- Modify: `gst/tests/test_ssv_review_candidate.cpp`
- Modify: `gst/tests/test_ssv_pub_payload.cpp`
- Modify: `gst/tests/test_gst_plugins.cpp`
- Modify: `scripts/pipeline.sh`
- Modify: `tests/ssv_cli_test.sh`
- Modify: `ssv`

- [ ] **Step 1: 写发布失败可重试和插件属性红灯测试**

给 `test_ssv_review_candidate.cpp` 增加 fake publisher：

```cpp
SsvReviewDeduplicator dedup;
dedup.reset_for_generation(3);
int calls = 0;
auto fail_publish = [&](std::string_view stream, std::string_view payload) {
    ++calls;
    assert(stream == "ssv:review-candidates");
    assert(nlohmann::json::parse(payload)["event_id"] == candidate.event_id);
    return false;
};
assert(ssv_review_try_publish(
    temp_root, "ssv:review-candidates", candidate, evidence,
    dedup, fail_publish, &error) == SsvReviewPublishResult::Failed);
assert(!dedup.already_published(ssv_review_dedup_key("camera-01", 3, 12)));

auto ok_publish = [&](std::string_view, std::string_view) {
    ++calls;
    return true;
};
assert(ssv_review_try_publish(
    temp_root, "ssv:review-candidates", candidate, evidence,
    dedup, ok_publish, &error) == SsvReviewPublishResult::Published);
assert(ssv_review_try_publish(
    temp_root, "ssv:review-candidates", candidate, evidence,
    dedup, ok_publish, &error) == SsvReviewPublishResult::SkippedDuplicate);
assert(calls == 2);
```

给 `test_gst_plugins.cpp` 增加 `ssvpub` 默认属性断言：

```cpp
gboolean review_enabled = TRUE;
gchar *review_stream_key = nullptr;
gchar *events_root = nullptr;
g_object_get(pub,
    "review-enabled", &review_enabled,
    "review-stream-key", &review_stream_key,
    "events-root", &events_root,
    nullptr);
fail_unless(!review_enabled);
fail_unless(std::string(review_stream_key) == "ssv:review-candidates");
fail_unless(std::string(events_root) == "artifacts/events");
```

将 `test_ssv_pub_payload.cpp` 现有 generation reset 测试扩展为 snapshot/buffer 对齐断言：

```cpp
assert(ssv_pub_snapshot_is_current(race_source, *snapshot));
assert(ssv_pub_review_snapshot_matches_buffer(
    race_source, *snapshot, GST_SECOND));
assert(!ssv_pub_review_snapshot_matches_buffer(
    race_source, *snapshot, 2 * GST_SECOND));
const auto reset = timeline.on_lifecycle_reset();
assert(reset.generation != snapshot->timing.generation);
assert(!ssv_pub_snapshot_is_current(race_source, *snapshot));
assert(!ssv_pub_review_snapshot_matches_buffer(
    race_source, *snapshot, GST_SECOND));
```

- [ ] **Step 2: 运行定向测试并确认先失败**

Run: `meson test -C build ssv-review-candidate gst-unit-tests --print-errorlogs`

Expected: FAIL，缺少 `ssv_review_try_publish` 或 `review-enabled` property。

- [ ] **Step 3: 实现归档后发布的确定性 seam**

在 `ssv_review_candidate.hpp` 增加：

```cpp
#include "ssv_frame_evidence.hpp"

enum class SsvReviewPublishResult {
    Published,
    SkippedDuplicate,
    Failed,
};

using SsvReviewPublishFn =
    std::function<bool(std::string_view stream, std::string_view payload)>;

SsvReviewPublishResult ssv_review_try_publish(
    const std::filesystem::path &events_root,
    std::string_view stream_key,
    const SsvReviewCandidate &candidate,
    const SsvFrameEvidence &evidence,
    SsvReviewDeduplicator &deduplicator,
    const SsvReviewPublishFn &publish,
    std::string *error);
```

函数顺序固定为：

1. 如果去重键已发布，返回 `SkippedDuplicate`；
2. 创建 `<events_root>/<event_id>/`；
3. 原子写 `evidence.jpg`；
4. 原子写 `candidate.json`；
5. 调用 `publish(review_stream_key, candidate_json)`；
6. 仅第 5 步成功后调用 `mark_published()` 并返回 `Published`。

任一步失败返回 `Failed`；已写事件目录保留，下一帧使用相同 `event_id` 覆盖重试。

- [ ] **Step 4: 扩展 `SsvPub` 状态和属性**

在 `gstssvpub.hpp` 暴露：

```cpp
bool ssv_pub_review_snapshot_matches_buffer(
    std::string_view source_id,
    const SsvTrackedFrame &frame,
    GstClockTime buffer_pts);
```

它返回 `ssv_pub_snapshot_is_current(source_id, frame) && buffer_pts != GST_CLOCK_TIME_NONE && frame.timing.pts == buffer_pts`，供纯测试和 `transform_ip` 共用。

`_SsvPub` 增加以下成员：

```cpp
gboolean review_enabled;
gchar *review_stream_key;
gchar *events_root;
GstVideoInfo video_info;
gboolean have_video_info;
SsvReviewDeduplicator *review_deduplicator;
```

注册属性：

```cpp
g_param_spec_boolean(
    "review-enabled", "Review Enabled",
    "Archive and publish first head frame per track",
    FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
g_param_spec_string(
    "review-stream-key", "Review Stream Key",
    "Redis Stream key for review candidates",
    "ssv:review-candidates", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
g_param_spec_string(
    "events-root", "Events Root",
    "Root directory for review event artifacts",
    "artifacts/events", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
```

实现 `set_caps`，仅接受 `GST_VIDEO_FORMAT_BGR`，把 negotiated width、height、stride 存入 `video_info`。`stop/finalize` 释放字符串和 deduplicator。

- [ ] **Step 5: 在 `transform_ip` 中接入候选，同时保持 detection 逻辑原样**

顺序固定为：

```cpp
auto consumed = self->meta->consume_tracked();
if (consumed.result != SsvMetaResult::Consumed || !consumed.frame)
    return GST_FLOW_OK;
const auto snapshot = std::move(consumed.frame);
if (snapshot->objects.empty() ||
    !ssv_pub_snapshot_is_current(self->source_id, *snapshot))
    return GST_FLOW_OK;

ssv_pub_redis_publish_detection(self, *snapshot);  // 旧 payload 不变
if (self->review_enabled)
    ssv_pub_publish_review_candidates(self, *snapshot, buf);
```

`ssv_pub_publish_review_candidates()` 必须：

- 调用 `ssv_pub_review_snapshot_matches_buffer(self->source_id, snapshot, GST_BUFFER_PTS(buf))`，再次确认 source、PTS 和 generation 对齐；
- generation 变化时调用 `reset_for_generation()`；
- 先收集未发布的有效 `head`；
- 当前帧至少有一个未发布 `head` 时只 map 和编码一次 BGR buffer；
- 每个 buffer 只读取一次 `std::chrono::system_clock`，把同一 `timestamp_ms` 传给该帧所有 `ssv_review_make_candidate()`；
- 多个首次 `head track_id` 复用同一 JPEG bytes/hash，但分别写入各自事件目录；
- Redis `XADD` 继续使用单字段 `event` 外壳；
- review 失败只记录 `GST_WARNING_OBJECT`，不让 pipeline 返回 `GST_FLOW_ERROR`。

生产发布 callback 固定为：

```cpp
auto publish_candidate = [&](std::string_view stream,
                             std::string_view payload) {
    return ssv_pub_redis_publish_json(
        self->redis_ctx, stream, payload, self,
        /*reconnect_on_failure=*/true);
};
```

`ssv_pub_redis_publish_json()` 在 hiredis reply 为空或 `REDIS_REPLY_ERROR` 时返回 `false`，成功 reply 时释放 reply 并返回 `true`；detection 路径继续调用同一底层函数但忽略 review-specific 状态。

- [ ] **Step 6: 冻结原 detection payload 不变**

扩展 `test_ssv_pub_payload.cpp`，继续断言：

```cpp
assert(message.size() == 5);
assert(message["type"] == "detection");
assert(!message.contains("event_id"));
assert(!message.contains("pipeline_generation"));
assert(!message.contains("evidence_path"));
```

不得把 review 字段加入 `ssv_pub_build_event_payload()`。

- [ ] **Step 7: 把 YAML 配置传给 `ssvpub`**

`scripts/pipeline.sh` 增加：

```bash
REVIEW_ENABLED="$(ssv_yaml_get review.enabled false)"
REVIEW_CANDIDATE_STREAM="$(ssv_yaml_get redis.review_candidate_stream ssv:review-candidates)"
EVENTS_ROOT="$(ssv_yaml_get artifacts.events_root artifacts/events)"
validate_boolean "review.enabled" "$REVIEW_ENABLED"

pub_props=(
    ssvpub
    "source-id=$SOURCE_ID"
    "redis-host=$REDIS_HOST"
    "redis-port=$REDIS_PORT"
    "stream-key=$REDIS_STREAM_KEY"
    "review-enabled=$REVIEW_ENABLED"
    "review-stream-key=$REVIEW_CANDIDATE_STREAM"
    "events-root=$EVENTS_ROOT"
)
```

日志明确打印 detection Stream、review 开关、candidate Stream 和 events root。`tests/ssv_cli_test.sh` 增加对应 `ssv_yaml_get` 和 property 断言，并把 example YAML 的模型断言更新为 `comp-2-freeze10.onnx + helmet.txt + target_class: ""`。

`ssv` 帮助中的 agent 描述改为“消费安全帽复验候选并输出结构化结果”，不再描述逐帧 detection 打印。

- [ ] **Step 8: 运行 T3、CLI 和全量 C++ 验证**

Run:

```bash
bash -n scripts/pipeline.sh
bash tests/ssv_cli_test.sh
./ssv build
meson test -C build --print-errorlogs
```

Expected: 全部 PASS；`review.enabled=false` 时不会创建 `artifacts/events`，原 `ssv:events` payload 测试逐字段不变。

- [ ] **Step 9: 提交 `ssvpub` 集成**

```bash
git add gst/ssv-pub gst/tests scripts/pipeline.sh tests/ssv_cli_test.sh ssv
git commit -m "feat: publish first head frame review candidates"
```

### Task 5: 原样迁入 DeerFlow 最小 import 闭包并建立来源审计

**Files:**

- Create: `agent/scripts/sync_deerflow.py`
- Create: `agent/src/deerflow/`
- Create: `agent/src/deerflow/UPSTREAM_MANIFEST.json`
- Create: `agent/src/deerflow/LICENSE`
- Create: `agent/tests/review/test_deerflow_upstream.py`
- Create: `docs/DeerFlow迁移说明.md`
- Modify: `agent/pyproject.toml`
- Modify: `agent/uv.lock`

- [ ] **Step 1: 写来源和 import 红灯测试**

`test_deerflow_upstream.py` 必须验证：

```python
UPSTREAM_COMMIT = "b68e1c686a0cb5a3780089d27354354533451d8e"

def test_manifest_hashes_and_license() -> None:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert manifest["upstream_commit"] == UPSTREAM_COMMIT
    for item in manifest["files"]:
        destination = AGENT_ROOT / item["destination_path"]
        assert hashlib.sha256(destination.read_bytes()).hexdigest() == item["sha256"]
        assert item["license"] == "MIT"


def test_factory_entrypoints_import() -> None:
    from deerflow.config.app_config import AppConfig
    from deerflow.config.model_config import ModelConfig
    from deerflow.models.factory import create_chat_model
    from deerflow.reflection import resolve_class

    assert AppConfig and ModelConfig and create_chat_model and resolve_class


def test_upstream_subtree_has_no_ssv_business_import() -> None:
    for path in (AGENT_ROOT / "src/deerflow").rglob("*.py"):
        assert "ssv_agent" not in path.read_text(encoding="utf-8")
```

- [ ] **Step 2: 运行测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/review/test_deerflow_upstream.py -q`

Expected: FAIL，`ModuleNotFoundError: No module named 'deerflow'`。

- [ ] **Step 3: 实现固定 allowlist 同步脚本**

`agent/scripts/sync_deerflow.py` 必须从 `git -C /mnt/work/deer-flow show <commit>:<source_path>` 读取 bytes，不能从未锁定的工作树复制。allowlist 固定为：

```python
SOURCE_ROOT = "backend/packages/harness/deerflow"

CONFIG_FILES = [
    "__init__.py", "acp_config.py", "agents_api_config.py", "app_config.py",
    "auth_config.py", "authorization_config.py", "channel_connections_config.py",
    "checkpointer_config.py", "database_config.py", "extensions_config.py",
    "guardrails_config.py", "input_polish_config.py", "loop_detection_config.py",
    "memory_config.py", "model_config.py", "paths.py",
    "read_before_write_config.py", "reload_boundary.py", "run_events_config.py",
    "run_ownership_config.py", "runtime_paths.py",
    "safety_finish_reason_config.py", "sandbox_config.py", "scheduler_config.py",
    "skill_evolution_config.py", "skill_scan_config.py", "skills_config.py",
    "stream_bridge_config.py", "subagents_config.py", "suggestions_config.py",
    "summarization_config.py", "title_config.py", "token_budget_config.py",
    "token_usage_config.py", "tool_config.py", "tool_output_config.py",
    "tool_progress_config.py", "tool_search_config.py", "tracing_config.py",
]

FILES = [
    "__init__.py",
    "constants.py",
    "trace_context.py",
    *[f"config/{name}" for name in CONFIG_FILES],
    "models/__init__.py",
    "models/factory.py",
    "models/openai_codex_provider.py",
    "models/credential_loader.py",
    "reflection/__init__.py",
    "reflection/resolvers.py",
    "tracing/__init__.py",
    "tracing/factory.py",
    "tracing/metadata.py",
    "tracing/monocle.py",
]
```

脚本以 `f"{SOURCE_ROOT}/{relative_path}"` 作为 `git show` 和 manifest 的 `source_path`，以 `src/deerflow/<relative_path>` 作为 `destination_path`。每项写入 `source_path`、`destination_path`、`upstream_commit`、`sha256`、`license: MIT`；禁止复制 `agents/`、`runtime/`、`sandbox/`、`tools/`、`community/`、`skills/` 和 `__pycache__`。

- [ ] **Step 4: 生成子树、manifest 和 MIT 许可**

Run:

```bash
cd agent
uv run python scripts/sync_deerflow.py \
  --upstream /mnt/work/deer-flow \
  --commit b68e1c686a0cb5a3780089d27354354533451d8e
```

Expected: 生成 allowlist 中的文件、`UPSTREAM_MANIFEST.json` 和从上游根 `LICENSE` 原样复制的 `agent/src/deerflow/LICENSE`。

- [ ] **Step 5: 更新 Python 依赖和 wheel 包**

`agent/pyproject.toml` 加入：

```toml
dependencies = [
    "httpx>=0.28.0",
    "langchain>=1.2.15",
    "langchain-openai>=1.2.1",
    "pillow>=11.0",
    "pydantic>=2.12.5",
    "pyyaml>=6.0.3",
    "structlog>=24.0",
    "redis>=5.0",
    "python-dotenv>=1.0",
]

[tool.hatch.build.targets.wheel]
packages = ["src/ssv_agent", "src/deerflow"]
```

不加入 Anthropic、MCP、LangGraph、Sandbox provider 或社区工具依赖。运行 `uv lock` 更新 `uv.lock`。

- [ ] **Step 6: 写中文迁移说明**

`docs/DeerFlow迁移说明.md` 明确记录：

- 上游仓库 `/mnt/work/deer-flow`；
- commit `b68e1c686a0cb5a3780089d27354354533451d8e`；
- MIT 许可和 `agent/src/deerflow/LICENSE`；
- allowlist 与禁止迁入目录；
- `deerflow_adapter.py` 是唯一业务 import 边界；
- 升级时先更新 commit/allowlist，再运行同步脚本、hash 测试和全量 Agent 测试。

- [ ] **Step 7: 运行来源测试和 import 测试**

Run:

```bash
cd agent
uv lock
uv sync --extra dev
uv run --extra dev pytest tests/review/test_deerflow_upstream.py -q
```

Expected: PASS。若依赖下载被网络策略阻止，保留 `pyproject.toml` 的上游兼容依赖并记录阻止原因，不用自写 factory 代替。

- [ ] **Step 8: 提交 DeerFlow 子树**

```bash
git add agent/scripts/sync_deerflow.py agent/src/deerflow agent/tests/review/test_deerflow_upstream.py agent/pyproject.toml agent/uv.lock docs/DeerFlow迁移说明.md
git commit -m "feat: vendor deerflow model factory closure"
```

### Task 6: 建立 Python candidate/result 契约和 `read_evidence`

**Files:**

- Create: `agent/src/ssv_agent/review/__init__.py`
- Create: `agent/src/ssv_agent/review/contracts.py`
- Create: `agent/src/ssv_agent/review/tools/__init__.py`
- Create: `agent/src/ssv_agent/review/tools/read_evidence.py`
- Create: `agent/tests/review/test_contracts.py`
- Create: `agent/tests/review/test_read_evidence.py`

- [ ] **Step 1: 写 Pydantic schema 红灯测试**

使用 Task 1 fixture，至少验证：

```python
candidate = ReviewCandidate.model_validate_json(CANDIDATE_FIXTURE.read_text())
assert candidate.event_id.version == 5
assert candidate.candidate_class == "head"
assert candidate.evidence_path == f"{candidate.event_id}/evidence.jpg"

with pytest.raises(ValidationError):
    ReviewCandidate.model_validate(
        {**candidate.model_dump(mode="json"), "candidate_class": "person"}
    )

with pytest.raises(ValidationError):
    ReviewDecision(
        decision="confirmed_no_helmet",
        review_confidence=0.93,
        primary_reason_code="helmet_visible",
        evidence_summary="冲突",
        recommended_action="人工确认",
    )
```

- [ ] **Step 2: 写路径、hash 和 JPEG 红灯测试**

`test_read_evidence.py` 用 Pillow 生成临时 `640x480` JPEG，并覆盖：

- 合法 `<event_id>/evidence.jpg`；
- 绝对路径；
- 包含 `..`；
- 软链接逃逸 events root；
- 父目录名与 `event_id` 不一致；
- SHA-256 不一致；
- JPEG 损坏；
- 实际尺寸与 candidate 宽高不一致。

非法情况统一抛 `EvidenceUnavailableError`，不得返回任意文件 bytes。

- [ ] **Step 3: 运行测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/review/test_contracts.py tests/review/test_read_evidence.py -q`

Expected: FAIL，`ModuleNotFoundError: No module named 'ssv_agent.review'`。

- [ ] **Step 4: 实现固定领域类型**

`contracts.py` 定义：

```python
Decision = Literal["confirmed_no_helmet", "rejected", "needs_human_review"]
ReasonCode = Literal[
    "no_helmet_visible",
    "helmet_visible",
    "low_confidence",
    "evidence_unavailable",
    "provider_unavailable",
    "invalid_model_output",
]

class ReviewCandidate(BaseModel):
    type: Literal["review_candidate"]
    schema_version: Literal[1]
    event_id: UUID5
    source: str
    pipeline_generation: int = Field(ge=0)
    frame_id: int = Field(ge=0)
    media_pts_ns: int = Field(ge=0)
    timestamp_ms: int = Field(ge=0)
    track_id: int = Field(ge=0)
    rule_id: Literal["head_without_helmet_single_frame"]
    rule_version: Literal[1]
    candidate_class: Literal["head"]
    detection_confidence: float = Field(ge=0.0, le=1.0)
    bbox: tuple[float, float, float, float]
    evidence_path: str
    evidence_sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    evidence_width: int = Field(gt=0)
    evidence_height: int = Field(gt=0)

    @model_validator(mode="after")
    def validate_identity_and_geometry(self) -> "ReviewCandidate":
        x1, y1, x2, y2 = self.bbox
        if not (0.0 <= x1 < x2 <= 1.0 and 0.0 <= y1 < y2 <= 1.0):
            raise ValueError("bbox 必须是有效归一化坐标")
        if self.evidence_path != f"{self.event_id}/evidence.jpg":
            raise ValueError("evidence_path 必须指向当前 event_id/evidence.jpg")
        if not self.source.strip():
            raise ValueError("source 不得为空")
        return self

class ReviewDecision(BaseModel):
    decision: Decision
    review_confidence: float = Field(ge=0.0, le=1.0)
    primary_reason_code: ReasonCode
    evidence_summary: str = Field(min_length=1)
    recommended_action: str = Field(min_length=1)

    @model_validator(mode="after")
    def validate_reason_mapping(self) -> "ReviewDecision":
        allowed = {
            "confirmed_no_helmet": {"no_helmet_visible"},
            "rejected": {"helmet_visible"},
            "needs_human_review": {
                "low_confidence",
                "evidence_unavailable",
                "provider_unavailable",
                "invalid_model_output",
            },
        }
        if self.primary_reason_code not in allowed[self.decision]:
            raise ValueError("decision 与 primary_reason_code 不匹配")
        return self

class ReviewResult(ReviewDecision):
    type: Literal["review_result"] = "review_result"
    schema_version: Literal[1] = 1
    event_id: UUID5
    source: str
    track_id: int
    rule_id: Literal["head_without_helmet_single_frame"]
    rule_version: Literal[1]
    provider: str
    model: str
    completed_at_ms: int

@dataclass(frozen=True)
class EvidenceBundle:
    candidate: ReviewCandidate
    jpeg_bytes: bytes
    media_type: Literal["image/jpeg"]
    width: int
    height: int
```

`ReviewDecision` 的 model validator 固定映射：

- `confirmed_no_helmet → no_helmet_visible`；
- `rejected → helmet_visible`；
- `needs_human_review → low_confidence|evidence_unavailable|provider_unavailable|invalid_model_output`。

- [ ] **Step 5: 实现受限 `read_evidence`**

接口固定为：

```python
from PIL import Image, UnidentifiedImageError

class EvidenceUnavailableError(RuntimeError):
    pass

def read_evidence(candidate: ReviewCandidate, events_root: Path) -> EvidenceBundle:
    relative = PurePosixPath(candidate.evidence_path)
    expected = PurePosixPath(str(candidate.event_id)) / "evidence.jpg"
    if relative.is_absolute() or ".." in relative.parts or relative != expected:
        raise EvidenceUnavailableError("evidence_path 非法")

    root = events_root.resolve()
    try:
        path = (root / Path(*relative.parts)).resolve(strict=True)
    except OSError as exc:
        raise EvidenceUnavailableError("证据文件不存在或不可访问") from exc
    if not path.is_relative_to(root) or path.parent.name != str(candidate.event_id):
        raise EvidenceUnavailableError("evidence_path 越界")

    try:
        data = path.read_bytes()
    except OSError as exc:
        raise EvidenceUnavailableError("证据文件读取失败") from exc
    if hashlib.sha256(data).hexdigest() != candidate.evidence_sha256:
        raise EvidenceUnavailableError("evidence_sha256 不一致")

    try:
        with Image.open(BytesIO(data)) as image:
            image.verify()
        with Image.open(BytesIO(data)) as image:
            if image.format != "JPEG" or image.size != (
                candidate.evidence_width,
                candidate.evidence_height,
            ):
                raise EvidenceUnavailableError("JPEG 元数据不一致")
            width, height = image.size
    except EvidenceUnavailableError:
        raise
    except (OSError, UnidentifiedImageError) as exc:
        raise EvidenceUnavailableError("JPEG 不可解码") from exc

    return EvidenceBundle(candidate, data, "image/jpeg", width, height)
```

`width, height` 必须在 Pillow image 仍打开时保存，`with` 结束后只使用整数副本。

- [ ] **Step 6: 运行契约与证据测试并提交**

Run: `cd agent && uv run --extra dev pytest tests/review/test_contracts.py tests/review/test_read_evidence.py -q`

Expected: PASS。

```bash
git add agent/src/ssv_agent/review agent/tests/review/test_contracts.py agent/tests/review/test_read_evidence.py
git commit -m "feat: validate review candidates and evidence"
```

### Task 7: 通过唯一 DeerFlow adapter 创建 mock/真实视觉模型并实现 `review_vision`

**Files:**

- Create: `agent/src/ssv_agent/review/deerflow_adapter.py`
- Create: `agent/src/ssv_agent/review/mock_provider.py`
- Create: `agent/src/ssv_agent/review/tools/review_vision.py`
- Create: `agent/prompts/vision_review_v1.md`
- Create: `agent/tests/review/test_deerflow_adapter.py`
- Create: `agent/tests/review/test_review_vision.py`

- [ ] **Step 1: 写唯一 import 边界和 factory 红灯测试**

`test_deerflow_adapter.py` 用 AST 扫描：

```python
imports = []
for path in (SRC / "ssv_agent").rglob("*.py"):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    if any(
        isinstance(node, (ast.Import, ast.ImportFrom))
        and (
            getattr(node, "module", "") == "deerflow"
            or getattr(node, "module", "").startswith("deerflow.")
            or any(alias.name == "deerflow" or alias.name.startswith("deerflow.")
                   for alias in getattr(node, "names", []))
        )
        for node in ast.walk(tree)
    ):
        imports.append(path.relative_to(SRC).as_posix())
assert imports == ["ssv_agent/review/deerflow_adapter.py"]
```

同一测试构造 `demo-vision` 配置，断言返回模型是 `MockVisionChatModel`，`supports_vision` 为真，且传入 DeerFlow 的 `AppConfig.tools == []`。

- [ ] **Step 2: 写单图 message、低置信度和异常映射红灯测试**

`test_review_vision.py` 覆盖：

- HumanMessage 只有一段候选摘要文本和一张 `data:image/jpeg;base64,<encoded-bytes>` 图片；
- prompt 明确“`head` 是候选而非最终事实”和“只依据当前完整帧”；
- `confirmed_no_helmet + 0.79` 被归一化为 `needs_human_review + low_confidence`；
- provider 抛异常时返回 `needs_human_review + provider_unavailable`；
- structured output 校验失败时返回 `needs_human_review + invalid_model_output`；
- 不产生 tool calls。

- [ ] **Step 3: 运行测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/review/test_deerflow_adapter.py tests/review/test_review_vision.py -q`

Expected: FAIL，缺少 `build_review_model` 和 `MockVisionChatModel`。

- [ ] **Step 4: 实现唯一 `deerflow_adapter.py`**

接口固定为：

```python
@dataclass(frozen=True)
class ReviewModelRuntime:
    chat_model: BaseChatModel
    provider: str
    model_name: str
    app_config: AppConfig


def build_review_model(config: SsvConfig) -> ReviewModelRuntime:
    raw_models = [model.model_dump(exclude_none=True) for model in config.agent.models]
    resolved_models = AppConfig.resolve_env_variables(raw_models)
    app_config = AppConfig.model_validate(
        {
            "models": resolved_models,
            "sandbox": {"use": "deerflow.sandbox.local:LocalSandboxProvider"},
            "tools": [],
        }
    )
    selected = app_config.get_model_config(config.agent.review_model)
    if selected is None or not selected.supports_vision:
        raise ValueError("review model 不存在或不支持 vision")
    resolve_class(selected.use, BaseChatModel)
    chat_model = create_chat_model(
        name=selected.name,
        app_config=app_config,
        attach_tracing=False,
    )
    provider = "mock" if selected.use.startswith(
        "ssv_agent.review.mock_provider:"
    ) else "openai-compatible"
    return ReviewModelRuntime(chat_model, provider, selected.model, app_config)
```

该文件是 `ssv_agent` 中唯一 import `ModelConfig/AppConfig/create_chat_model/resolve_class` 的位置。固定 Sandbox class path 只满足 `AppConfig` 必填字段，不解析、不 import、不实例化 Sandbox。

- [ ] **Step 5: 实现可控 `MockVisionChatModel`**

`MockVisionChatModel` 继承 `BaseChatModel`，实现 `_llm_type`、`_generate` 和 `with_structured_output`。`with_structured_output(ReviewDecision, include_raw=True)` 必须返回包含 `raw`、`parsed`、`parsing_error` 的 Runnable 结果，与真实 LangChain provider 的调用形状一致。默认输出固定为：

```json
{
  "decision": "confirmed_no_helmet",
  "review_confidence": 0.93,
  "primary_reason_code": "no_helmet_visible",
  "evidence_summary": "目标头部清晰可见，未观察到安全帽。",
  "recommended_action": "生成未佩戴安全帽复验记录。"
}
```

模型保存最近一次 messages 和调用次数到 Pydantic `PrivateAttr`，只供测试断言；不得读取本地路径或连接 Redis。

- [ ] **Step 6: 写固定中文 prompt**

`agent/prompts/vision_review_v1.md` 必须包含以下规则，且不要求 chain-of-thought：

```text
你是安全帽佩戴视觉复验器。
输入中的 head 只是检测候选，不是“未佩戴安全帽”的最终事实。
你只能依据当前提供的一张完整原始帧判断，不得假设其他时间点、其他视角或未显示区域。
若头部或安全帽不可可靠辨认，返回 needs_human_review。
只输出 ReviewDecision 结构化字段，不输出隐藏推理过程。
```

- [ ] **Step 7: 实现 `review_vision`**

接口固定为：

```python
def review_vision(
    evidence: EvidenceBundle,
    runtime: ReviewModelRuntime,
    automatic_decision_min_confidence: float,
) -> ReviewDecision:
    candidate = evidence.candidate
    messages = [
        SystemMessage(content=load_prompt()),
        HumanMessage(
            content=[
                {
                    "type": "text",
                    "text": (
                        f"source={candidate.source}, track_id={candidate.track_id}, "
                        f"bbox={candidate.bbox}, detection_confidence="
                        f"{candidate.detection_confidence:.3f}"
                    ),
                },
                {
                    "type": "image_url",
                    "image_url": {
                        "url": "data:image/jpeg;base64,"
                        + base64.b64encode(evidence.jpeg_bytes).decode("ascii")
                    },
                },
            ]
        ),
    ]
    try:
        structured = runtime.chat_model.with_structured_output(
            ReviewDecision, include_raw=True
        )
        response = structured.invoke(messages)
    except Exception:
        return provider_unavailable_decision()
    if (
        not isinstance(response, dict)
        or response.get("parsing_error") is not None
        or response.get("parsed") is None
    ):
        return invalid_model_output_decision()
    try:
        decision = ReviewDecision.model_validate(response["parsed"])
    except ValidationError:
        return invalid_model_output_decision()
    return normalize_confidence(decision, automatic_decision_min_confidence)
```

`normalize_confidence()` 只重写低于阈值的 `confirmed_no_helmet` 或 `rejected`；模型已经返回 `needs_human_review` 时保留其合法 reason code。

同一文件定义下列确定性 helper，避免异常分支出现自由文本漂移：

```python
PROMPT_PATH = Path(__file__).resolve().parents[4] / "prompts/vision_review_v1.md"

def load_prompt() -> str:
    return PROMPT_PATH.read_text(encoding="utf-8").strip()

def provider_unavailable_decision() -> ReviewDecision:
    return ReviewDecision(
        decision="needs_human_review",
        review_confidence=0.0,
        primary_reason_code="provider_unavailable",
        evidence_summary="视觉模型服务当前不可用。",
        recommended_action="保留证据并由人工复验。",
    )

def invalid_model_output_decision() -> ReviewDecision:
    return ReviewDecision(
        decision="needs_human_review",
        review_confidence=0.0,
        primary_reason_code="invalid_model_output",
        evidence_summary="视觉模型未返回合法结构化结果。",
        recommended_action="保留证据并由人工复验。",
    )

def normalize_confidence(
    decision: ReviewDecision, threshold: float
) -> ReviewDecision:
    if (
        decision.decision in {"confirmed_no_helmet", "rejected"}
        and decision.review_confidence < threshold
    ):
        return decision.model_copy(
            update={
                "decision": "needs_human_review",
                "primary_reason_code": "low_confidence",
                "recommended_action": "模型置信度不足，转人工复验。",
            }
        )
    return decision
```

- [ ] **Step 8: 运行 adapter/vision 测试并提交**

Run: `cd agent && uv run --extra dev pytest tests/review/test_deerflow_adapter.py tests/review/test_review_vision.py -q`

Expected: PASS，mock 模型由 `create_chat_model()` 返回，`AppConfig.tools` 为空。

```bash
git add agent/src/ssv_agent/review/deerflow_adapter.py agent/src/ssv_agent/review/mock_provider.py agent/src/ssv_agent/review/tools/review_vision.py agent/prompts/vision_review_v1.md agent/tests/review/test_deerflow_adapter.py agent/tests/review/test_review_vision.py
git commit -m "feat: review one frame through deerflow factory"
```

### Task 8: 实现 `save_review_result` 和轻量幂等 processor

**Files:**

- Create: `agent/src/ssv_agent/review/tools/save_review_result.py`
- Create: `agent/src/ssv_agent/review/processor.py`
- Create: `agent/tests/review/test_save_review_result.py`
- Create: `agent/tests/review/test_processor.py`
- Modify: `agent/src/ssv_agent/review/tools/__init__.py`

- [ ] **Step 1: 写落盘、best effort 和重复候选红灯测试**

测试必须证明：

- `review-result.json` 通过同目录临时文件原子替换；
- 文件写成功、结果 Stream `xadd` 抛异常时仍返回可 ACK；
- 文件写失败时不调用 `xadd` 且不可 ACK；
- 第二次处理同一 candidate 时模型调用次数保持 `1`；
- 已有 result 的 `event_id/source/track_id/rule_id/rule_version` 不一致时不调用模型、不覆盖文件、不可 ACK；
- 已有 result JSON 损坏时不可 ACK。

- [ ] **Step 2: 运行测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/review/test_save_review_result.py tests/review/test_processor.py -q`

Expected: FAIL，缺少 `save_review_result` 和 `ReviewProcessor`。

- [ ] **Step 3: 实现原子结果写入和结果 Stream best effort**

接口固定为：

```python
def publish_review_result_best_effort(
    result: ReviewResult,
    redis_client: Redis,
    result_stream: str,
) -> None:
    try:
        redis_client.xadd(result_stream, {"event": result.model_dump_json()})
    except Exception as exc:
        logger.warning(
            "复验结果 Stream 发布失败",
            event_id=str(result.event_id),
            error=str(exc),
        )


def save_review_result(
    result: ReviewResult,
    events_root: Path,
    redis_client: Redis,
    result_stream: str,
) -> bool:
    event_dir = events_root.resolve() / str(result.event_id)
    final_path = event_dir / "review-result.json"
    temp_path = event_dir / ".review-result.json.tmp"
    payload = result.model_dump_json(indent=2) + "\n"
    try:
        event_dir.mkdir(parents=True, exist_ok=True)
        temp_path.write_text(payload, encoding="utf-8")
        temp_path.replace(final_path)
    except OSError as exc:
        try:
            temp_path.unlink(missing_ok=True)
        except OSError:
            pass
        logger.error(
            "复验结果文件写入失败",
            event_id=str(result.event_id),
            error=str(exc),
        )
        return False
    publish_review_result_best_effort(result, redis_client, result_stream)
    return True
```

文件写入失败时不调用 `xadd`。不得删除旧的合法 `review-result.json`。

- [ ] **Step 4: 实现固定三段流程**

先定义已有结果状态、身份投影和证据失败结论：

```python
from enum import Enum
from pathlib import Path
from typing import Callable
import time

from pydantic import ValidationError

class ExistingResult(Enum):
    MISSING = "missing"
    VALID = "valid"
    CORRUPT = "corrupt"
    MISMATCHED = "mismatched"


def candidate_identity(candidate: ReviewCandidate) -> dict[str, object]:
    return {
        "event_id": candidate.event_id,
        "source": candidate.source,
        "track_id": candidate.track_id,
        "rule_id": candidate.rule_id,
        "rule_version": candidate.rule_version,
    }


def evidence_unavailable_decision() -> ReviewDecision:
    return ReviewDecision(
        decision="needs_human_review",
        review_confidence=0.0,
        primary_reason_code="evidence_unavailable",
        evidence_summary="单帧证据不存在、越界、损坏或校验失败。",
        recommended_action="检查证据归档后由人工复验。",
    )


def load_existing_result(
    candidate: ReviewCandidate,
    events_root: Path,
) -> tuple[ExistingResult, ReviewResult | None]:
    path = events_root.resolve() / str(candidate.event_id) / "review-result.json"
    if not path.exists():
        return ExistingResult.MISSING, None
    try:
        result = ReviewResult.model_validate_json(path.read_text(encoding="utf-8"))
    except (OSError, ValidationError):
        return ExistingResult.CORRUPT, None
    expected = candidate_identity(candidate)
    actual = {key: getattr(result, key) for key in expected}
    if actual != expected:
        return ExistingResult.MISMATCHED, result
    return ExistingResult.VALID, result
```

`ReviewProcessor` 使用完整实现形状：

```python
class ReviewProcessor:
    def __init__(
        self,
        config: SsvConfig,
        runtime: ReviewModelRuntime,
        redis_client: Redis,
        clock_ms: Callable[[], int] = lambda: time.time_ns() // 1_000_000,
    ) -> None:
        self._config = config
        self._runtime = runtime
        self._redis = redis_client
        self._clock_ms = clock_ms

    def process(self, candidate: ReviewCandidate) -> bool:
        events_root = self._config.artifacts.events_root
        status, existing = load_existing_result(candidate, events_root)
        if status is ExistingResult.VALID:
            assert existing is not None
            publish_review_result_best_effort(
                existing,
                self._redis,
                self._config.redis.review_result_stream,
            )
            return True
        if status in {ExistingResult.CORRUPT, ExistingResult.MISMATCHED}:
            return False

        try:
            evidence = read_evidence(candidate, events_root)
        except EvidenceUnavailableError:
            decision = evidence_unavailable_decision()
        else:
            decision = review_vision(
                evidence,
                self._runtime,
                self._config.review.automatic_decision_min_confidence,
            )

        result = ReviewResult(
            **candidate_identity(candidate),
            **decision.model_dump(),
            provider=self._runtime.provider,
            model=self._runtime.model_name,
            completed_at_ms=self._clock_ms(),
        )
        return save_review_result(
            result,
            events_root,
            self._redis,
            self._config.redis.review_result_stream,
        )
```

这就是唯一处理顺序：`read_evidence → review_vision → save_review_result`。证据不可用时跳过模型，但仍由确定性代码生成 `needs_human_review + evidence_unavailable` 并进入 `save_review_result`。

- [ ] **Step 5: 运行 processor 测试并提交**

Run: `cd agent && uv run --extra dev pytest tests/review/test_save_review_result.py tests/review/test_processor.py -q`

Expected: PASS，重复候选不会重复调用模型。

```bash
git add agent/src/ssv_agent/review/tools agent/src/ssv_agent/review/processor.py agent/tests/review/test_save_review_result.py agent/tests/review/test_processor.py
git commit -m "feat: persist idempotent review results"
```

### Task 9: 将 Agent consumer、service 和 CLI 切换到复验候选

**Files:**

- Modify: `agent/src/ssv_agent/event_consumer.py`
- Modify: `agent/src/ssv_agent/service.py`
- Modify: `agent/tests/test_event_consumer.py`
- Modify: `agent/tests/test_service.py`
- Modify: `scripts/agent.sh`
- Modify: `ssv`
- Modify: `tests/ssv_cli_test.sh`

- [ ] **Step 1: 写 candidate consumer ACK 红灯测试**

Fake Redis 记录 `xgroup_create`、`xack` 和构造参数；测试：

```python
processor = FakeProcessor(result=True)
consumer, redis = make_consumer(config, processor)
expected = ReviewCandidate.model_validate_json(
    CANDIDATE_FIXTURE.read_text(encoding="utf-8")
)
consumer._handle_event(
    "123-0",
    {"event": CANDIDATE_FIXTURE.read_text(encoding="utf-8")},
)
assert processor.calls == [expected]
assert redis.acked == [
    ("ssv:review-candidates", "ssv-agent", "123-0")
]
```

malformed JSON、错误 `type`、缺失身份字段和 `processor.process() == False` 均不 ACK。

- [ ] **Step 2: 写 service 组装红灯测试**

测试 `review.enabled=false` 时不构建模型、不启动 consumer；启用时调用顺序为：

```python
runtime = build_review_model(config)
processor = ReviewProcessor(config, runtime, redis_client)
run_consumer(config, processor, redis_client)
```

- [ ] **Step 3: 运行测试并确认先失败**

Run: `cd agent && uv run --extra dev pytest tests/test_event_consumer.py tests/test_service.py -q`

Expected: FAIL，consumer 仍读取 `ssv:events` 或仍把 payload 当 detection。

- [ ] **Step 4: 改造 `EventConsumer`**

构造函数改为：

```python
class EventConsumer:
    def __init__(
        self,
        config: SsvConfig,
        processor: ReviewProcessor,
        redis_client: Redis | None = None,
    ) -> None:
        self._stream = config.redis.review_candidate_stream
        self._group = config.redis.consumer_group
        self._processor = processor
        self._redis = redis_client or Redis(
            host=config.redis.host,
            port=config.redis.port,
            db=config.redis.db,
            decode_responses=True,
        )
```

consumer group 首次创建使用 `id="0"`，确保 Agent 在 candidate 之后启动时仍可完成 Demo；consumer name 使用 `<hostname>-<pid>`；`xreadgroup` 固定 `count=1`，本阶段单 worker 串行处理。

`_handle_event()` 只读取 `fields["event"]`，通过 `ReviewCandidate.model_validate_json` 校验，`processor.process(candidate) is True` 时才 `xack`。不得再解析 `detections` 或输出逐帧 detection 摘要。

- [ ] **Step 5: 改造 service 组装**

`service.run()`：

```python
def run(config: SsvConfig) -> None:
    if not config.review.enabled:
        logger.info("安全帽视觉复验未启用", review_enabled=False)
        return

    redis_client = Redis(
        host=config.redis.host,
        port=config.redis.port,
        db=config.redis.db,
        decode_responses=True,
    )
    runtime = build_review_model(config)
    processor = ReviewProcessor(config, runtime, redis_client)
    logger.info(
        "安全帽复验 Agent 启动",
        stream=config.redis.review_candidate_stream,
        result_stream=config.redis.review_result_stream,
        provider=runtime.provider,
        model=runtime.model_name,
        model_visible_tools=0,
    )
    run_consumer(config, processor, redis_client)
```

处理成功后只输出一条中文 INFO 摘要：`event_id`、`source`、`track_id`、`decision`、`review_confidence` 和 `primary_reason_code`；不得记录 JPEG base64、API key、base URL 或隐藏推理。

- [ ] **Step 6: 更新 shell/CLI 描述**

`scripts/agent.sh` 的标题改为“启动安全帽复验 Agent”，`ssv` 帮助和 `tests/ssv_cli_test.sh` 同步改为 candidate/result 语义。命令入口仍保持 `./ssv agent` 和 `python -m ssv_agent`，不新增第二套 Agent CLI。

- [ ] **Step 7: 运行 Agent 与 CLI 测试并提交**

Run:

```bash
cd agent
uv run --extra dev pytest tests/test_event_consumer.py tests/test_service.py -q
cd ..
bash tests/ssv_cli_test.sh
```

Expected: PASS；默认关闭态不订阅 `ssv:events`，启用态只订阅 `ssv:review-candidates`。

```bash
git add agent/src/ssv_agent/event_consumer.py agent/src/ssv_agent/service.py agent/tests/test_event_consumer.py agent/tests/test_service.py scripts/agent.sh ssv tests/ssv_cli_test.sh
git commit -m "feat: consume review candidates in agent"
```

### Task 10: 跑通默认 mock 闭环、真实安全帽模型 Demo 和最终验证

**Files:**

- Create: `agent/tests/review/test_end_to_end.py`
- Verify only: `docs/specs/2026-07-30-T3T4-DeerFlow单帧复验Demo-spec.md`
- Do not modify: `docs/specs/2026-07-29-T3T4-DeerFlow安全帽复验链路-spec.md`
- Do not modify: `docs/plans/2026-07-29-T3T4-DeerFlow安全帽复验链路-plan.md`

- [ ] **Step 1: 写无网络端到端测试**

`test_end_to_end.py` 使用临时 events root、真实 JPEG bytes、Task 1 candidate fixture、Fake Redis 和经 DeerFlow factory 创建的 `MockVisionChatModel`，断言：

```python
assert processor.process(candidate) is True
assert runtime.chat_model.call_count == 1
result_path = events_root / str(candidate.event_id) / "review-result.json"
result = ReviewResult.model_validate_json(result_path.read_text())
assert result.decision == "confirmed_no_helmet"
assert fake_redis.added[0][0] == "ssv:review-results"

assert processor.process(candidate) is True
assert runtime.chat_model.call_count == 1
```

另加 `provider_unavailable`、`invalid_model_output`、`evidence_unavailable` 和结果 Stream 失败断言。

- [ ] **Step 2: 运行全量 Python 质量门**

Run:

```bash
cd agent
uv run --extra dev pytest -q
uv run --extra dev ruff check src tests scripts
```

Expected: PASS；默认测试不访问网络。

- [ ] **Step 3: 运行全量 C++、脚本和格式验证**

Run:

```bash
./ssv build
meson test -C build --print-errorlogs
bash tests/ssv_cli_test.sh
bash -n scripts/pipeline.sh
git diff --check
```

Expected: PASS。

- [ ] **Step 4: 准备真实安全帽模型 + mock vision 的本地 Demo 配置**

从 `config/ssv.example.yaml` 复制本地 `config/ssv.yaml`，写入真实 RTSP 地址并启用：

```yaml
sources:
  - name: "camera-01"
    type: "rtsp"
    uri: "rtsp://<本地可用地址>"
    protocols: "tcp"
    latency_ms: 200

inference:
  runtime: "onnxruntime"
  model_path: "models/comp-2-freeze10.onnx"
  label_map: "config/model-labels/helmet.txt"
  target_class: ""
  device: "auto"
  output_format: "auto"

review:
  enabled: true
  automatic_decision_min_confidence: 0.80

agent:
  review_model: "demo-vision"
  models:
    - name: "demo-vision"
      use: "ssv_agent.review.mock_provider:MockVisionChatModel"
      model: "demo-vision"
      supports_vision: true
```

RTSP 内容必须能让真实安全帽模型产生至少一个 `head`；不得用 `mock-detect` 或 `person` 验收。

- [ ] **Step 5: 启动 Demo 并验证六个可观察结果**

分别在终端运行：

```bash
./ssv redis
./ssv build
./ssv agent
./ssv run
```

检查：

```bash
docker exec ssv-redis redis-cli XLEN ssv:events
docker exec ssv-redis redis-cli XRANGE ssv:review-candidates - +
docker exec ssv-redis redis-cli XRANGE ssv:review-results - +
find artifacts/events -maxdepth 2 -type f \( -name 'candidate.json' -o -name 'evidence.jpg' -o -name 'review-result.json' \)
```

成功标准：

1. `ssv:events` 继续出现逐帧 detection；
2. 同一 `source + generation + head track_id` 只有一个业务 `event_id`；
3. 事件目录只有一张完整 `evidence.jpg`，尺寸等于配置的分析分辨率；
4. `candidate.json` 的 SHA-256 与最终 JPEG bytes 一致；
5. Agent 日志出现一次中文结果摘要，`review-result.json` 已生成；
6. `ssv:review-results` 出现同一 `event_id` 的结果。

- [ ] **Step 6: 可选运行真实 OpenAI-compatible smoke**

只有 endpoint 和凭据已配置时，将本地模型配置切换为：

```yaml
agent:
  review_model: "openai-vision"
  models:
    - name: "openai-vision"
      use: "langchain_openai:ChatOpenAI"
      model: "gpt-4.1-mini"
      supports_vision: true
      api_key: "$SSV_AGENT_OPENAI_API_KEY"
      base_url: "$SSV_AGENT_OPENAI_BASE_URL"
```

重新启动 `./ssv agent`，只验证一个候选。缺少 endpoint 或 API key 时明确跳过，不把 mock 结果描述为真实 provider 结果。

- [ ] **Step 7: 执行边界和旧文档保护扫描**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

tokens = ["TO" + "DO", "T" + "BD", "待" + "补充"]
paths = [
    Path("docs/plans/2026-07-30-T3T4-DeerFlow单帧复验Demo-plan.md"),
    Path("agent/src/ssv_agent/review"),
    Path("gst/ssv-pub"),
]
for token in tokens:
    for path in paths:
        files = [path] if path.is_file() else path.rglob("*")
        for file in files:
            if (
                file.is_file()
                and file.suffix in {".md", ".py", ".cpp", ".hpp"}
                and token in file.read_text(encoding="utf-8", errors="ignore")
            ):
                raise SystemExit(f"placeholder token {token!r} in {file}")
PY
if rg -n "mock-detect|mock-track|candidate_class.*person|ssvevent" \
  agent/src/ssv_agent/review gst/ssv-pub; then
    exit 1
fi
rg -n "from deerflow|import deerflow" agent/src/ssv_agent
git diff -- docs/specs/2026-07-29-T3T4-DeerFlow安全帽复验链路-spec.md
git diff -- docs/plans/2026-07-29-T3T4-DeerFlow安全帽复验链路-plan.md
git status --short
```

Expected:

- 占位标记脚本和禁止实现扫描均无输出并以 0 退出；
- DeerFlow import 只命中 `agent/src/ssv_agent/review/deerflow_adapter.py`；
- 两个 2026-07-29 文档的 working-tree diff 为空，原有 staged 状态保持不变；
- 新增/修改文件仅属于本计划列出的 T3/T4/T5 范围。

- [ ] **Step 8: 演练关闭态回滚**

停止 Agent，将本地配置的 `review.enabled` 改回 `false`，重新运行：

```bash
./ssv run
```

确认 `ssvinfer → ssvtrack → ssvpub` 和 `ssv:events` 继续工作，既有 `artifacts/events` 文件不被删除，Agent 不再启动模型调用。

- [ ] **Step 9: 提交端到端验收**

```bash
git add agent/tests/review/test_end_to_end.py
git commit -m "test: verify deerflow single-frame review demo"
```

## 完成定义

- `review.enabled=false` 时，`ssvpub` 不写事件目录、不发布候选，原 detection payload 和旧 Stream 不变。
- 使用真实安全帽模型、`helmet.txt` 和空 `target_class` 后，只有有效 `head` 轨迹能触发候选。
- 同一去重键只成功发布一次；归档或 Redis candidate 发布失败时下一帧可用同一 `event_id` 重试。
- `evidence.jpg` 是触发 buffer 的完整分析帧，quality 90，不裁剪、不缩放、不绘框，hash 可复算。
- DeerFlow 子树可回溯到固定 MIT commit，SSV 业务只通过 `deerflow_adapter.py` 使用 `ModelConfig/AppConfig/resolve_class/create_chat_model`。
- 模型可见工具数为 0，Agent 固定执行 `read_evidence → review_vision → save_review_result`。
- 重复 candidate 不重复调用模型；合法结果文件成功落盘即可 ACK，结果 Stream 发布为 best effort。
- 默认 mock 闭环、全部自动化测试和真实安全帽模型 Demo 均有明确验证结果；真实 provider 未配置时被明确标记为未执行。

### Task 11: 约束真实视觉模型的 JSON Schema 输出

**Files:**

- Modify: `agent/src/ssv_agent/review/tools/review_vision.py`
- Modify: `agent/prompts/vision_review_v1.md`
- Modify: `agent/tests/review/test_end_to_end.py`
- Modify: `docs/specs/2026-07-30-T3T4-DeerFlow单帧复验Demo-spec.md`

- [ ] **Step 1: 写红灯测试**

用记录调用参数的 fake vision model 验证首次调用携带
`response_format.type=json_schema`、schema 名 `helmet_review` 和 `strict=true`；
再让首次返回 `decision: no_helmet`、第二次返回合法 `ReviewDecision`，断言发生两次调用且
最终使用第二次结果。让两次都返回非法值，断言仅产生
`needs_human_review + invalid_model_output`。

- [ ] **Step 2: 实现严格 schema 调用和一次格式修复**

从 `ReviewDecision.model_json_schema()` 生成 response format，在首次 vision invoke 的 kwargs
中绑定该格式。Pydantic 校验失败时，以同一图片、相同 response format 和仅修复 JSON 的
文本提示调用一次；不把模型原始枚举猜测映射为业务结论。mock provider 保持原单次调用，
以便默认离线 Demo 不依赖 provider 的 JSON Schema 支持。

- [ ] **Step 3: 纠正提示词并验证**

提示词逐字列出三个 decision、六个 reason code、0 至 1 的数值
`review_confidence` 与组合约束；删除把 `low_confidence` 写入数值字段的表述。

Run:

```bash
cd agent
uv run --extra dev pytest tests/review/test_end_to_end.py -q
uv run --extra dev pytest -q
uv run --extra dev ruff check src tests scripts
```

Expected: PASS，测试不访问网络。真实 smoke 仅在配置 endpoint 和 API key 后运行一个候选，
并检查结果不再是 `invalid_model_output`。

### Task 12: 使用人可读事件目录保存单帧复验证据

**Files:**

- Modify: `gst/ssv-pub/ssv_review_candidate.hpp`
- Modify: `gst/ssv-pub/ssv_review_candidate.cpp`
- Modify: `gst/tests/test_ssv_review_candidate.cpp`
- Modify: `agent/src/ssv_agent/review/contracts.py`
- Modify: `agent/src/ssv_agent/review/tools/read_evidence.py`
- Modify: `agent/src/ssv_agent/review/tools/save_review_result.py`
- Modify: `agent/src/ssv_agent/review/processor.py`
- Modify: `agent/tests/review/test_contracts.py`
- Modify: `agent/tests/review/test_end_to_end.py`

- [ ] **Step 1: 写 C++ 和 Python 红灯测试**

固定 `timestamp_ms=1785400934927`，断言新目录为
`20260730T164214.927+0800_camera-01_g3_t12_4816f729`，candidate JSON 保持 schema_version `1`，
不包含额外目录字段，且 evidence_path 为 `<事件目录名>/evidence.jpg`。Python 断言通过
evidence_path 的安全父目录读写结果，旧 UUID 目录 fixture 继续可用。

- [ ] **Step 2: 实现目录名和 v2 路径契约**

T3 使用 UTC 时间加固定 8 小时格式化触发时间；source 仅保留安全文件名字符；UUIDv5
只取前 8 位展示。T4 从 candidate.evidence_path 的安全父目录定位 evidence、existing result
和 result 写入目录；目录名不作为 candidate、result 或结果 Stream 的独立字段。

- [ ] **Step 3: 运行跨语言验证**

Run:

```bash
./ssv build
meson test -C build --print-errorlogs
cd agent && uv run --extra dev pytest -q
git diff --check
```

Expected: PASS；不删除既有 `artifacts/events`，不提交或暂存任何文件。
