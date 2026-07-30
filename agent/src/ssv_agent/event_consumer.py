from __future__ import annotations

import os
import signal
import time

import structlog
from redis import Redis

from ssv_agent.config import SsvConfig
from ssv_agent.review.contracts import ReviewCandidate
from ssv_agent.review.processor import ReviewProcessor

logger = structlog.get_logger()


class EventConsumer:
    """串行消费已归档的复验候选。"""

    def __init__(self, config: SsvConfig, processor: ReviewProcessor, redis_client: Redis | None = None) -> None:
        self._stream = config.redis.review_candidate_stream
        self._group = config.redis.consumer_group
        self._processor = processor
        self._running = False
        self._redis = redis_client or Redis(
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
            # Group already exists — that's fine.
            pass

    def start(self) -> None:
        """Blocking consumer loop.  Returns on SIGINT/SIGTERM."""
        self._running = True
        self._ensure_group()
        consumer_name = f"{os.uname().nodename}-{os.getpid()}"

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
                    count=1,
                    block=1000,  # 1 s
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
            candidate = ReviewCandidate.model_validate_json(raw)
        except Exception:
            logger.warning("复验候选无效", msg_id=msg_id)
            return
        if self._processor.process(candidate) is True:
            self._redis.xack(self._stream, self._group, msg_id)


def run_consumer(config: SsvConfig, processor: ReviewProcessor, redis_client: Redis | None = None) -> None:
    """Run the event consumer with graceful shutdown."""
    consumer = EventConsumer(config, processor, redis_client)

    def _shutdown(sig: int, _frame: object) -> None:
        logger.info("received signal, stopping consumer", signal=sig)
        consumer.stop()

    signal.signal(signal.SIGINT, _shutdown)
    signal.signal(signal.SIGTERM, _shutdown)

    consumer.start()
