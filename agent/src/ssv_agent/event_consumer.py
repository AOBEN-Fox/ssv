from __future__ import annotations

import json
import signal
import time
from pathlib import Path

import structlog
from redis import Redis

from ssv_agent.config import SsvConfig
from ssv_agent.review_client import MockVisionReviewClient, RightCodesVisionReviewClient, manual_review_result
from ssv_agent.review_context import ReviewContext, ReviewEventError
from ssv_agent.review_output import write_review_outputs

logger = structlog.get_logger()
PROJECT_ROOT = Path(__file__).resolve().parents[3]


class EventConsumer:
    """Minimal Redis Streams consumer that reads detection events and logs them.

    M2 scope: consume and print.  Full state-machine orchestration arrives in M6.
    """

    def __init__(self, config: SsvConfig) -> None:
        self._config = config
        self._stream = config.redis.stream_key
        self._group = config.redis.consumer_group
        self._running = False
        self._redis = Redis(
            host=config.redis.host,
            port=config.redis.port,
            db=config.redis.db,
            decode_responses=True,
        )

    def _ensure_group(self) -> None:
        """Create the consumer group if it does not exist."""
        try:
            self._redis.xgroup_create(self._stream, self._group, id="0", mkstream=True)
            logger.info("created consumer group", group=self._group, stream=self._stream)
        except Exception:
            pass

    def _review_client(self):
        model_cfg = self._config.agent.model
        if model_cfg.provider == "right_codes":
            return RightCodesVisionReviewClient(
                base_url=model_cfg.base_url,
                model=model_cfg.model,
                api_key_env=model_cfg.api_key_env,
                timeout_seconds=model_cfg.timeout_seconds,
            )
        return MockVisionReviewClient()

    def _resolve_repo_path(self, value: str | None) -> Path | None:
        if not value:
            return None
        path = Path(value)
        if path.is_absolute():
            return path
        return PROJECT_ROOT / path

    def _handle_helmet_violation(self, event: dict) -> Path:
        model_cfg = self._config.agent.model
        ctx = ReviewContext.from_event(event, provider=model_cfg.provider, model=model_cfg.model)
        frame_path = self._resolve_repo_path(ctx.frame_path)
        if ctx.missing_reason or frame_path is None or not frame_path.is_file():
            result = manual_review_result(ctx, "missing_evidence")
        else:
            ctx = ctx.model_copy(update={"frame_path": str(frame_path)})
            result = self._review_client().review(ctx)
        written = write_review_outputs(
            output_dir=self._resolve_repo_path(self._config.reviews.output_dir) or self._config.reviews.output_dir,
            result=result,
            source=ctx.source,
            frame_id=ctx.frame_id,
            trigger_reason=ctx.trigger_reason,
            frame_path=str(frame_path) if frame_path is not None else ctx.frame_path,
        )
        return written.markdown_path

    def start(self) -> None:
        self._running = True
        self._ensure_group()
        consumer_name = "ssv-agent-0"

        logger.info(
            "event consumer started",
            stream=self._stream,
            group=self._group,
            consumer=consumer_name,
        )

        while self._running:
            try:
                entries = self._redis.xreadgroup(
                    self._group,
                    consumer_name,
                    {self._stream: ">"},
                    count=10,
                    block=1000,
                )
            except Exception as exc:
                logger.warning("redis read error", error=str(exc))
                time.sleep(2)
                continue

            for _stream_name, messages in entries:
                for msg_id, fields in messages:
                    self._handle_event(msg_id, fields)

    def stop(self) -> None:
        self._running = False

    def _handle_event(self, msg_id: str, fields: dict[str, str]) -> None:
        raw = fields.get("event", "{}")
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            logger.warning("malformed event", msg_id=msg_id, raw=raw)
            return

        if event.get("type") == "helmet_violation":
            try:
                markdown_path = self._handle_helmet_violation(event)
            except (ReviewEventError, OSError) as exc:
                logger.warning("helmet violation review failed", msg_id=msg_id, error=str(exc))
                return
            logger.info(
                "helmet violation reviewed",
                msg_id=msg_id,
                event_id=event.get("event_id"),
                review=str(markdown_path),
            )
            self._redis.xack(self._stream, self._group, msg_id)
            return

        detections = event.get("detections", [])
        det_summary = ", ".join(
            f"{d['class']}({d['confidence']:.2f}"
            + (f", track={d['track_id']}" if d.get("track_id", -1) >= 0 else "")
            + ")"
            for d in detections
        )

        logger.info(
            "detection event",
            msg_id=msg_id,
            source=event.get("source", "?"),
            frame_id=event.get("frame_id"),
            detections=det_summary,
            count=len(detections),
        )

        self._redis.xack(self._stream, self._group, msg_id)


def run_consumer(config: SsvConfig) -> None:
    consumer = EventConsumer(config)

    def _shutdown(sig: int, _frame: object) -> None:
        logger.info("received signal, stopping consumer", signal=sig)
        consumer.stop()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    consumer.start()
