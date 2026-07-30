from __future__ import annotations

import base64
import json
from pathlib import Path
from typing import Any

from langchain_core.messages import HumanMessage, SystemMessage
from langchain_core.language_models.chat_models import BaseChatModel

from ssv_agent.review.contracts import EvidenceBundle, ReviewDecision


PROMPT_PATH = Path(__file__).resolve().parents[4] / "prompts/vision_review_v1.md"


def _strict_review_response_format() -> dict[str, Any]:
    schema = ReviewDecision.model_json_schema()
    schema["additionalProperties"] = False
    schema["allOf"] = [
        {
            "if": {"properties": {"decision": {"const": "confirmed_no_helmet"}}, "required": ["decision"]},
            "then": {"properties": {"primary_reason_code": {"const": "no_helmet_visible"}}, "required": ["primary_reason_code"]},
        },
        {
            "if": {"properties": {"decision": {"const": "rejected"}}, "required": ["decision"]},
            "then": {"properties": {"primary_reason_code": {"const": "helmet_visible"}}, "required": ["primary_reason_code"]},
        },
        {
            "if": {"properties": {"decision": {"const": "needs_human_review"}}, "required": ["decision"]},
            "then": {
                "properties": {
                    "primary_reason_code": {
                        "enum": ["low_confidence", "evidence_unavailable", "provider_unavailable", "invalid_model_output"],
                    }
                },
                "required": ["primary_reason_code"],
            },
        },
    ]
    return {
        "type": "json_schema",
        "json_schema": {"name": "helmet_review", "strict": True, "schema": schema},
    }


def _vision_message(text: str, image_url: str) -> HumanMessage:
    return HumanMessage(content=[
        {"type": "text", "text": text},
        {"type": "image_url", "image_url": {"url": image_url}},
    ])


def _parse_review_decision(content: object) -> ReviewDecision:
    if not isinstance(content, str):
        raise ValueError("模型输出不是文本 JSON")
    return ReviewDecision.model_validate(json.loads(content))


def review_vision(evidence: EvidenceBundle, model: BaseChatModel) -> ReviewDecision:
    prompt = PROMPT_PATH.read_text(encoding="utf-8")
    image_url = "data:image/jpeg;base64," + base64.b64encode(evidence.jpeg_bytes).decode("ascii")
    response_format = _strict_review_response_format()
    messages = [
        SystemMessage(content=prompt),
        _vision_message("请根据这一张原始视频帧输出 JSON。", image_url),
    ]
    try:
        return _parse_review_decision(model.invoke(messages, response_format=response_format).content)
    except (ValueError, json.JSONDecodeError):
        repair_messages = [
            SystemMessage(content=prompt),
            _vision_message("上一份输出未通过 JSON Schema 校验。只输出符合 schema 的 JSON，不得输出 Markdown 或解释。", image_url),
        ]
        try:
            return _parse_review_decision(model.invoke(repair_messages, response_format=response_format).content)
        except (ValueError, json.JSONDecodeError):
            return ReviewDecision(
                decision="needs_human_review",
                review_confidence=0.0,
                primary_reason_code="invalid_model_output",
                evidence_summary="视觉模型未返回符合复验契约的结构化结果。",
                recommended_action="人工复验该单帧证据。",
            )
