#pragma once

#include "ssv_meta.hpp"

#include <gst/base/gstbasetransform.h>

#include <cstdint>
#include <optional>
#include <string>

std::string ssv_pub_build_event_payload(const SsvFrameDetections &det, std::int64_t timestamp_ms);
std::string ssv_pub_sanitize_filename_component(const std::string &value);
std::string ssv_pub_build_evidence_frame_path(
    const std::string &output_dir,
    const SsvFrameDetections &det,
    std::int64_t timestamp_ms);
std::optional<std::string> ssv_pub_build_helmet_violation_payload(
    const SsvFrameDetections &det,
    std::int64_t timestamp_ms,
    const std::string &trigger_class,
    const char *frame_path,
    int frame_width,
    int frame_height,
    const char *missing_reason);

G_BEGIN_DECLS

#define SSV_TYPE_PUB (ssv_pub_get_type())
G_DECLARE_FINAL_TYPE(SsvPub, ssv_pub, SSV, PUB, GstBaseTransform)

G_END_DECLS
