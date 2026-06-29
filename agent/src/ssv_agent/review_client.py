from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Any

from ssv_agent.review_context import ReviewContext, ReviewResult
from ssv_agent.review_prompt import build_review_prompt

VALID_DECISIONS = {"violation_confirmed", "no_violation", "manual_review"}


def manual_review_result(ctx: ReviewContext, error: str) -> ReviewResult:
    return ReviewResult(
        event_id=ctx.event_id,
        final_decision="manual_review",
        confidence=0.0,
        reasoning_summary="复核未完成，需要人工处理。",
        evidence_description="证据不可用或模型调用失败。",
        recommended_action="请人工复核该事件。",
        provider=ctx.provider,
        model=ctx.model,
        error=error,
    )


def parse_review_response(event_id: str, content: str, provider: str, model: str) -> ReviewResult:
    try:
        data = json.loads(content)
    except json.JSONDecodeError:
        return ReviewResult(
            event_id=event_id,
            final_decision="manual_review",
            confidence=0.0,
            reasoning_summary="模型返回非 JSON。",
            evidence_description="无法解析模型输出。",
            recommended_action="请人工复核该事件。",
            provider=provider,
            model=model,
            error="invalid_json",
        )
    decision = data.get("final_decision")
    if decision not in VALID_DECISIONS:
        data["final_decision"] = "manual_review"
        data["error"] = "invalid_decision"
    return ReviewResult(
        event_id=event_id,
        final_decision=data["final_decision"],
        confidence=float(data.get("confidence", 0.0)),
        reasoning_summary=str(data.get("reasoning_summary", "模型未给出说明。")),
        evidence_description=str(data.get("evidence_description", "模型未给出画面描述。")),
        recommended_action=str(data.get("recommended_action", "请人工复核该事件。")),
        provider=provider,
        model=model,
        error=data.get("error"),
    )


class MockVisionReviewClient:
    def review(self, ctx: ReviewContext) -> ReviewResult:
        return ReviewResult(
            event_id=ctx.event_id,
            final_decision="violation_confirmed",
            confidence=0.80,
            reasoning_summary="mock provider 根据 head 触发事件返回违规确认。",
            evidence_description="mock provider 未读取真实图片内容。",
            recommended_action="建议现场提醒并复查该时间点视频。",
            provider="mock",
            model=ctx.model,
            error=None,
        )


class RightCodesVisionReviewClient:
    def __init__(self, base_url: str, model: str, api_key_env: str, timeout_seconds: int) -> None:
        self._base_url = base_url.rstrip("/")
        self._model = model
        self._api_key_env = api_key_env
        self._timeout_seconds = timeout_seconds

    def review(self, ctx: ReviewContext) -> ReviewResult:
        api_key = os.environ.get(self._api_key_env)
        if not api_key:
            return manual_review_result(ctx, "missing_api_key")
        try:
            prompt = build_review_prompt(ctx)
            body: dict[str, Any] = {
                "model": self._model,
                "messages": [
                    {"role": "system", "content": prompt.system},
                    {
                        "role": "user",
                        "content": [
                            {"type": "text", "text": prompt.user_text},
                            {"type": "image_url", "image_url": {"url": prompt.image_data_url}},
                        ],
                    },
                ],
                "temperature": 0,
            }
            req = urllib.request.Request(
                f"{self._base_url}/chat/completions",
                data=json.dumps(body).encode("utf-8"),
                headers={
                    "Authorization": f"Bearer {api_key}",
                    "Content-Type": "application/json",
                },
                method="POST",
            )
            with urllib.request.urlopen(req, timeout=self._timeout_seconds) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
            content = payload["choices"][0]["message"]["content"]
            return parse_review_response(ctx.event_id, content, "right_codes", self._model)
        except (OSError, urllib.error.URLError, KeyError, TypeError, json.JSONDecodeError):
            return manual_review_result(ctx, "provider_error")
