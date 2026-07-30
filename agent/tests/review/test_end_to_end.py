from __future__ import annotations

import hashlib
import json
from pathlib import Path

from PIL import Image
from pydantic import Field
from langchain_core.language_models.chat_models import BaseChatModel
from langchain_core.messages import AIMessage, BaseMessage
from langchain_core.outputs import ChatGeneration, ChatResult

from ssv_agent.config import SsvConfig
from ssv_agent.review.contracts import EvidenceBundle, ReviewCandidate, ReviewResult
from ssv_agent.review.mock_provider import MockVisionChatModel
from ssv_agent.review.processor import ReviewModelRuntime, ReviewProcessor
from ssv_agent.review.tools.review_vision import review_vision


class FakeRedis:
    def __init__(self) -> None:
        self.added: list[tuple[str, dict[str, str]]] = []

    def xadd(self, stream: str, fields: dict[str, str]) -> None:
        self.added.append((stream, fields))


class CountingMockVisionChatModel(MockVisionChatModel):
    call_count: int = 0

    def _generate(self, messages, **kwargs):
        self.call_count += 1
        return super()._generate(messages, **kwargs)


class SequencedVisionChatModel(BaseChatModel):
    responses: list[str]
    call_kwargs: list[dict[str, object]] = Field(default_factory=list)

    @property
    def _llm_type(self) -> str:
        return "openai_compatible_test_vision"

    def _generate(self, messages: list[BaseMessage], **kwargs: object) -> ChatResult:
        self.call_kwargs.append(kwargs)
        return ChatResult(generations=[ChatGeneration(message=AIMessage(content=self.responses.pop(0)))])


def _evidence_bundle(tmp_path: Path) -> EvidenceBundle:
    data = json.loads((Path(__file__).parent / "fixtures/review-candidate-v1.json").read_text())
    image_path = tmp_path / "evidence.jpg"
    Image.new("RGB", (640, 480), "red").save(image_path, format="JPEG")
    jpeg_bytes = image_path.read_bytes()
    data["evidence_sha256"] = hashlib.sha256(jpeg_bytes).hexdigest()
    return EvidenceBundle(
        candidate=ReviewCandidate.model_validate(data),
        jpeg_bytes=jpeg_bytes,
        media_type="image/jpeg",
        width=640,
        height=480,
    )


def test_review_vision_uses_strict_schema_and_repairs_one_invalid_response(tmp_path: Path) -> None:
    model = SequencedVisionChatModel(responses=[
        '{"decision":"no_helmet","review_confidence":"high"}',
        '{"decision":"confirmed_no_helmet","review_confidence":0.94,'
        '"primary_reason_code":"no_helmet_visible",'
        '"evidence_summary":"未见安全帽。",'
        '"recommended_action":"生成未佩戴安全帽告警。"}',
    ])

    result = review_vision(_evidence_bundle(tmp_path), model)

    assert result.decision == "confirmed_no_helmet"
    assert result.review_confidence == 0.94
    assert len(model.call_kwargs) == 2
    response_format = model.call_kwargs[0]["response_format"]
    assert response_format["type"] == "json_schema"
    assert response_format["json_schema"]["name"] == "helmet_review"
    assert response_format["json_schema"]["strict"] is True
    assert response_format["json_schema"]["schema"]["additionalProperties"] is False
    assert response_format["json_schema"]["schema"]["allOf"]


def test_review_vision_maps_two_invalid_responses_to_human_review(tmp_path: Path) -> None:
    model = SequencedVisionChatModel(responses=["not json", "still not json"])

    result = review_vision(_evidence_bundle(tmp_path), model)

    assert result.decision == "needs_human_review"
    assert result.review_confidence == 0.0
    assert result.primary_reason_code == "invalid_model_output"
    assert len(model.call_kwargs) == 2


def test_single_frame_review_is_idempotent(tmp_path: Path) -> None:
    data = json.loads((Path(__file__).parent / "fixtures/review-candidate-v1.json").read_text())
    event_dir = tmp_path / data["event_id"]
    event_dir.mkdir()
    evidence = event_dir / "evidence.jpg"
    Image.new("RGB", (640, 480), "red").save(evidence, format="JPEG")
    data["evidence_sha256"] = hashlib.sha256(evidence.read_bytes()).hexdigest()
    candidate = ReviewCandidate.model_validate(data)
    config = SsvConfig.model_validate({"artifacts": {"events_root": str(tmp_path)}})
    redis = FakeRedis()
    model = CountingMockVisionChatModel()
    processor = ReviewProcessor(config, ReviewModelRuntime(model, "mock", "demo-vision"), redis, lambda: 1785400001200)

    assert processor.process(candidate) is True
    result_path = event_dir / "review-result.json"
    result = ReviewResult.model_validate_json(result_path.read_text())
    assert result.decision == "confirmed_no_helmet"
    assert model.call_count == 1
    assert redis.added[0][0] == "ssv:review-results"

    assert processor.process(candidate) is True
    assert model.call_count == 1


def test_result_is_saved_in_human_readable_evidence_directory(tmp_path: Path) -> None:
    data = json.loads((Path(__file__).parent / "fixtures/review-candidate-v1.json").read_text())
    event_dir_name = "20260730T164214.927+0800_camera-01_g3_t12_4816f729"
    data.update({
        "evidence_path": f"{event_dir_name}/evidence.jpg",
    })
    event_dir = tmp_path / event_dir_name
    event_dir.mkdir()
    evidence = event_dir / "evidence.jpg"
    Image.new("RGB", (640, 480), "red").save(evidence, format="JPEG")
    data["evidence_sha256"] = hashlib.sha256(evidence.read_bytes()).hexdigest()
    candidate = ReviewCandidate.model_validate(data)
    config = SsvConfig.model_validate({"artifacts": {"events_root": str(tmp_path)}})
    processor = ReviewProcessor(
        config, ReviewModelRuntime(MockVisionChatModel(), "mock", "demo-vision"),
        FakeRedis(), lambda: 1785400001200,
    )

    assert processor.process(candidate) is True
    assert (event_dir / "review-result.json").exists()
    assert not (tmp_path / str(candidate.event_id) / "review-result.json").exists()
