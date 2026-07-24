#pragma once

#include "overlay_motion_predictor.hpp"
#include "overlay_renderer.hpp"
#include "ssv_meta.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct OverlayRuntimeStats {
    std::uint64_t display_frames = 0;
    std::uint64_t no_pts_frames = 0;
    std::uint64_t history_hits = 0;
    std::uint64_t history_misses = 0;
    std::uint64_t future_matches = 0;
    std::uint64_t observation_snapshots = 0;
    std::uint64_t observed_boxes = 0;
    std::uint64_t predicted_boxes = 0;
    std::uint64_t timed_out_boxes = 0;
    std::uint64_t clipped_boxes = 0;
    std::uint64_t invalid_boxes = 0;
    std::uint64_t prediction_age_total_ns = 0;
    GstClockTime max_prediction_age_ns = 0;
    std::size_t max_predictor_states = 0;
};

class OverlayRuntime {
public:
    OverlayRuntime(
        std::string source_id,
        bool motion_prediction,
        std::uint32_t max_horizon_ms,
        std::string font_face = "regular",
        std::uint32_t font_size = 7);

    SsvTimelineUpdate on_segment(const SsvTimelineSegment &segment);
    SsvTimelineUpdate on_flush_stop(bool reset_time);
    SsvFrameTiming on_buffer(
        GstClockTime pts,
        GstClockTime duration,
        bool discontinuity);
    SsvTimelineUpdate stop();

    SsvOverlayFrame frame_for_display(const SsvFrameTiming &display_timing);
    OverlayRenderStats render(
        GstVideoFrame &video_frame,
        const SsvFrameTiming &display_timing);

    OverlayRuntimeStats stats() const { return stats_; }
    SsvMetaStats meta_stats() const { return meta_->stats(); }
    bool should_log_summary();

private:
    void reset_display_state();
    void prepare_frame_for_display(
        const SsvFrameTiming &display_timing,
        SsvOverlayFrame &output);

    std::string source_id_;
    std::shared_ptr<SsvSourceMeta> meta_;
    SsvTimelineCursor timeline_;
    OverlayMotionPredictor predictor_;
    OverlayRenderer renderer_;
    SsvOverlayFrame reusable_overlay_frame_;
    std::shared_ptr<const SsvTrackedFrame> last_observed_;
    OverlayRuntimeStats stats_;
    std::chrono::steady_clock::time_point last_summary_at_;
};
