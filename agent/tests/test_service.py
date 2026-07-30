from __future__ import annotations

from ssv_agent.config import SsvConfig
from ssv_agent import service


def test_run_does_nothing_when_review_disabled(monkeypatch) -> None:
    calls: list[SsvConfig] = []
    monkeypatch.setattr(service, "run_consumer", lambda *args: calls.append(args))
    cfg = SsvConfig()

    service.run(cfg)

    assert calls == []
