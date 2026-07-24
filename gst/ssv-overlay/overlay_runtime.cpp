#include "overlay_runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

OverlayRuntime::OverlayRuntime(
    std::string source_id,
    bool motion_prediction,
    std::uint32_t max_horizon_ms,
    std::string font_face,
    std::uint32_t font_size)
    : source_id_(std::move(source_id)),
      meta_(ssv_meta(source_id_)),
      timeline_(meta_),
      predictor_(motion_prediction, max_horizon_ms),
      renderer_(font_face, font_size),
      last_summary_at_(std::chrono::steady_clock::now())
{
    if (source_id_.empty())
        throw std::invalid_argument("source_id must not be empty");
}

void OverlayRuntime::reset_display_state()
{
    predictor_.reset();
    last_observed_.reset();
}

SsvTimelineUpdate OverlayRuntime::on_segment(const SsvTimelineSegment &segment)
{
    const auto update = timeline_.on_segment(segment);
    if (update.reset)
        reset_display_state();
    return update;
}

SsvTimelineUpdate OverlayRuntime::on_flush_stop(bool reset_time)
{
    const auto update = timeline_.on_flush_stop(reset_time);
    if (update.reset)
        reset_display_state();
    return update;
}

SsvFrameTiming OverlayRuntime::on_buffer(
    GstClockTime pts,
    GstClockTime duration,
    bool discontinuity)
{
    const auto update = timeline_.on_buffer(pts, discontinuity);
    if (update.reset)
        reset_display_state();
    ++stats_.display_frames;
    if (pts == GST_CLOCK_TIME_NONE)
        ++stats_.no_pts_frames;
    return {pts, duration, update.generation};
}

SsvTimelineUpdate OverlayRuntime::stop()
{
    const auto update = timeline_.on_lifecycle_reset();
    reset_display_state();
    return update;
}

SsvOverlayFrame OverlayRuntime::frame_for_display(
    const SsvFrameTiming &display_timing)
{
    SsvOverlayFrame output;
    prepare_frame_for_display(display_timing, output);
    return output;
}

void OverlayRuntime::prepare_frame_for_display(
    const SsvFrameTiming &display_timing,
    SsvOverlayFrame &output)
{
    output.source_id = source_id_;
    output.observation_timing = {};
    output.display_timing = display_timing;
    output.boxes.clear();
    if (display_timing.pts == GST_CLOCK_TIME_NONE)
        return;

    auto snapshot = meta_->latest_tracked_at_or_before(display_timing.pts);
    if (!snapshot) {
        ++stats_.history_misses;
        return;
    }
    if (snapshot->source_id != source_id_ ||
        snapshot->timing.generation != display_timing.generation ||
        snapshot->timing.pts == GST_CLOCK_TIME_NONE ||
        snapshot->timing.pts > display_timing.pts) {
        ++stats_.future_matches;
        return;
    }
    ++stats_.history_hits;

    if (!last_observed_ || last_observed_.get() != snapshot.get()) {
        if (predictor_.observe(snapshot))
            ++stats_.observation_snapshots;
        last_observed_ = snapshot;
    }
    if (meta_->generation() != display_timing.generation) {
        return;
    }

    predictor_.predict_into(source_id_, display_timing, output);
    const auto predictor_stats = predictor_.take_stats();
    stats_.timed_out_boxes += predictor_stats.timed_out_boxes;
    stats_.clipped_boxes += predictor_stats.clipped_boxes;
    stats_.invalid_boxes += predictor_stats.invalid_boxes;
    for (const auto &box : output.boxes) {
        if (box.predicted)
            ++stats_.predicted_boxes;
        else
            ++stats_.observed_boxes;
    }
    if (!output.boxes.empty()) {
        const auto age = display_timing.pts - snapshot->timing.pts;
        stats_.prediction_age_total_ns += age;
        stats_.max_prediction_age_ns = std::max(
            stats_.max_prediction_age_ns, age);
    }
    stats_.max_predictor_states = std::max(
        stats_.max_predictor_states, predictor_.state_count());
}

OverlayRenderStats OverlayRuntime::render(
    GstVideoFrame &video_frame,
    const SsvFrameTiming &display_timing)
{
    prepare_frame_for_display(display_timing, reusable_overlay_frame_);
    auto render_stats = renderer_.render(video_frame, reusable_overlay_frame_);
    stats_.invalid_boxes += render_stats.skipped_small_boxes;
    return render_stats;
}

bool OverlayRuntime::should_log_summary()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_summary_at_ < std::chrono::seconds(5))
        return false;
    last_summary_at_ = now;
    return true;
}
