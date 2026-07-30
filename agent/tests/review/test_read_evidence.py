from __future__ import annotations

import hashlib
import json
from pathlib import Path

import pytest
from PIL import Image

from ssv_agent.review.contracts import ReviewCandidate
from ssv_agent.review.tools.read_evidence import EvidenceUnavailableError, read_evidence


FIXTURE = Path(__file__).parent / "fixtures/review-candidate-v1.json"


def make_candidate(events_root: Path) -> ReviewCandidate:
    data = json.loads(FIXTURE.read_text())
    event_dir = events_root / data["event_id"]
    event_dir.mkdir(parents=True)
    path = event_dir / "evidence.jpg"
    Image.new("RGB", (640, 480), color="red").save(path, format="JPEG")
    data["evidence_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return ReviewCandidate.model_validate(data)


def test_reads_valid_jpeg_with_matching_hash(tmp_path: Path) -> None:
    candidate = make_candidate(tmp_path)
    evidence = read_evidence(candidate, tmp_path)
    assert evidence.media_type == "image/jpeg"
    assert (evidence.width, evidence.height) == (640, 480)


@pytest.mark.parametrize("path", ["/etc/passwd", "../evidence.jpg"])
def test_rejects_unsafe_evidence_path(tmp_path: Path, path: str) -> None:
    candidate = make_candidate(tmp_path)
    with pytest.raises(EvidenceUnavailableError):
        read_evidence(candidate.model_copy(update={"evidence_path": path}), tmp_path)


def test_rejects_hash_or_jpeg_dimension_mismatch(tmp_path: Path) -> None:
    candidate = make_candidate(tmp_path)
    with pytest.raises(EvidenceUnavailableError):
        read_evidence(candidate.model_copy(update={"evidence_sha256": "0" * 64}), tmp_path)
    with pytest.raises(EvidenceUnavailableError):
        read_evidence(candidate.model_copy(update={"evidence_width": 1}), tmp_path)
