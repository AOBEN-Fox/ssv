#pragma once

#include "ssv_frame_evidence.hpp"
#include "ssv_meta.hpp"

#include <array>
#include <cstdint>
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
std::string ssv_review_event_dir_name(
    std::string_view source, std::uint64_t generation, int track_id,
    std::int64_t timestamp_ms);
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

enum class SsvReviewPublishResult {
    Published,
    SkippedDuplicate,
    Failed,
};

using SsvReviewPublishFn =
    std::function<bool(std::string_view stream, std::string_view payload)>;

class SsvReviewDeduplicator {
public:
    void reset_for_generation(std::uint64_t generation);
    bool already_published(std::string_view key) const;
    void mark_published(std::string key);

private:
    std::uint64_t generation_ = 0;
    std::unordered_set<std::string> published_;
};

SsvReviewPublishResult ssv_review_try_publish(
    const std::filesystem::path &events_root,
    std::string_view stream_key,
    const SsvReviewCandidate &candidate,
    const SsvFrameEvidence &evidence,
    SsvReviewDeduplicator &deduplicator,
    const SsvReviewPublishFn &publish,
    std::string *error);
