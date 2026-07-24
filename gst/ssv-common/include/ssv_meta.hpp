#pragma once

#include <gst/gst.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum SsvTrackState : int {
    SSV_TRACK_NEW = 0,
    SSV_TRACK_MATCHED = 1,
    SSV_TRACK_LOST = 2,
    SSV_TRACK_DEAD = 3,
};

struct SsvFrameTiming {
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    std::uint64_t generation = 0;
};

inline bool operator==(const SsvFrameTiming &left, const SsvFrameTiming &right)
{
    return left.pts == right.pts && left.duration == right.duration &&
        left.generation == right.generation;
}

struct SsvDetection {
    char class_name[32] = {};
    float confidence = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    int class_id = -1;
};

struct SsvDetectionFrame {
    std::uint64_t frame_id = 0;
    std::string source_id;
    SsvFrameTiming timing;
    std::vector<SsvDetection> detections;
};

struct SsvTrackedObject {
    SsvDetection detection;
    int track_id = -1;
    SsvTrackState track_state = SSV_TRACK_NEW;
    bool occluded = false;
};

struct SsvTrackedFrame {
    std::uint64_t frame_id = 0;
    std::string source_id;
    SsvFrameTiming timing;
    std::vector<SsvTrackedObject> objects;
};

struct SsvOverlayBox {
    SsvDetection detection;
    int track_id = -1;
    SsvTrackState track_state = SSV_TRACK_NEW;
    bool occluded = false;
    bool predicted = false;
};

struct SsvOverlayFrame {
    std::string source_id;
    SsvFrameTiming observation_timing;
    SsvFrameTiming display_timing;
    std::vector<SsvOverlayBox> boxes;
};

enum class SsvMetaResult {
    Published,
    Consumed,
    Empty,
    NoPts,
    Occupied,
    WrongSource,
    WrongGeneration,
    DuplicatePts,
    StalePts,
};

struct SsvMetaStats {
    std::uint64_t published = 0;
    std::uint64_t consumed = 0;
    std::uint64_t empty = 0;
    std::uint64_t no_pts = 0;
    std::uint64_t occupied = 0;
    std::uint64_t wrong_source = 0;
    std::uint64_t wrong_generation = 0;
    std::uint64_t duplicate_pts = 0;
    std::uint64_t stale_pts = 0;
    std::uint64_t generation_resets = 0;
    std::size_t max_history_depth = 0;
};

struct SsvDetectionConsumeResult {
    SsvMetaResult result = SsvMetaResult::Empty;
    std::optional<SsvDetectionFrame> frame;
};

struct SsvTrackedConsumeResult {
    SsvMetaResult result = SsvMetaResult::Empty;
    std::shared_ptr<const SsvTrackedFrame> frame;
};

struct SsvTimelineSegment {
    GstClockTime start = 0;
    GstClockTime time = 0;
    GstClockTime base = 0;
    double rate = 1.0;

    bool operator==(const SsvTimelineSegment &) const = default;
};

struct SsvTimelineUpdate {
    std::uint64_t generation = 0;
    bool reset = false;
};

class SsvTimelineCursor;

class SsvSourceMeta final {
public:
    explicit SsvSourceMeta(std::string_view source_id);
    ~SsvSourceMeta();

    SsvSourceMeta(const SsvSourceMeta &) = delete;
    SsvSourceMeta &operator=(const SsvSourceMeta &) = delete;

    std::uint64_t generation() const;

    SsvMetaResult publish_detection(SsvDetectionFrame &&frame);
    SsvDetectionConsumeResult consume_detection();

    SsvMetaResult publish_tracked(
        SsvDetectionFrame &&observation,
        std::vector<SsvTrackedObject> objects);
    SsvTrackedConsumeResult consume_tracked();

    std::shared_ptr<const SsvTrackedFrame> latest_tracked_at_or_before(
        GstClockTime display_pts) const;
    std::size_t history_depth() const;
    SsvMetaStats stats() const;

private:
    friend class SsvTimelineCursor;

    struct Impl;

    std::uint64_t observe_segment(
        const SsvTimelineSegment &segment,
        std::uint64_t expected_generation,
        bool coalesce_reset);
    std::uint64_t request_reset(std::uint64_t expected_generation);

    std::unique_ptr<Impl> impl_;
};

class SsvTimelineCursor final {
public:
    explicit SsvTimelineCursor(std::shared_ptr<SsvSourceMeta> source);

    SsvTimelineUpdate on_segment(const SsvTimelineSegment &segment);
    SsvTimelineUpdate on_flush_stop(bool reset_time);
    SsvTimelineUpdate on_buffer(GstClockTime pts, bool discontinuity);
    SsvTimelineUpdate on_lifecycle_reset();

    std::uint64_t generation() const { return generation_; }

private:
    enum class ResetKind {
        None,
        Flush,
        Discontinuity,
        PtsRollback,
        Lifecycle,
    };

    SsvTimelineUpdate synchronize();
    SsvTimelineUpdate reset_once(ResetKind kind);

    std::shared_ptr<SsvSourceMeta> source_;
    std::uint64_t generation_ = 0;
    GstClockTime last_pts_ = GST_CLOCK_TIME_NONE;
    ResetKind last_reset_kind_ = ResetKind::None;
};

std::shared_ptr<SsvSourceMeta> ssv_meta(std::string_view source_id);
