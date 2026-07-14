#include "gstssvtrack.hpp"
#include "botsort/botsort_tracker.hpp"
#include "botsort/botsort_coordinates.hpp"
#include "botsort/botsort_types.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/video/video.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

GST_DEBUG_CATEGORY_STATIC(ssv_track_debug);

static botsort::Detection
ssv_to_botsort_detection(const SsvDetection &src, int input_index) {
    botsort::Detection det;
    det.x1 = src.x1;
    det.y1 = src.y1;
    det.x2 = src.x2;
    det.y2 = src.y2;
    det.score = src.confidence;
    det.class_id = src.class_id;
    det.class_name = src.class_name;
    det.input_index = input_index;
    det.track_id = src.track_id;
    det.track_state = static_cast<botsort::TrackState>(src.track_state);
    det.occluded = src.occluded;
    return det;
}

static botsort::FrameDetections
ssv_to_botsort_detections(const std::vector<SsvDetection> &src, int width, int height) {
    botsort::FrameDetections out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        out.push_back(botsort::to_pixel_detection(
            ssv_to_botsort_detection(src[i], static_cast<int>(i)), width, height));
    }
    return out;
}

static void
apply_botsort_results(std::vector<SsvDetection> &dst, const botsort::FrameDetections &src) {
    for (const auto &det : src) {
        if (det.input_index < 0) continue;
        const std::size_t index = static_cast<std::size_t>(det.input_index);
        if (index >= dst.size()) continue;
        auto &out = dst[index];
        out.track_id = det.track_id;
        out.track_state = static_cast<int>(det.track_state);
        out.occluded = det.occluded;
    }
}

// ── GObject struct ─────────────────────────────────────────────────────
struct _SsvTrack {
    GstBaseTransform parent;

    gint frame_rate;
    gfloat track_thresh;
    gint track_buffer;
    gfloat match_thresh;
    gboolean mock_track;
    gfloat track_low_thresh;
    gfloat track_high_thresh;
    gfloat new_track_thresh;
    gchar *gmc_method;
    gint gmc_downscale;

    botsort::BoTSortTracker *tracker;
    gint mock_next_id;
};

static botsort::TrackerConfig
make_botsort_config(const SsvTrack *self) {
    botsort::TrackerConfig config;
    config.frame_rate = self->frame_rate;
    config.track_thresh = self->track_thresh;
    config.track_buffer = self->track_buffer;
    config.match_thresh = self->match_thresh;
    config.track_low_thresh = self->track_low_thresh;
    config.track_high_thresh = self->track_high_thresh;
    config.new_track_thresh = self->new_track_thresh;
    config.enable_score_fuse = true;
    config.enable_class_constraint = false;
    config.gmc_method = g_strcmp0(self->gmc_method, "none") == 0
        ? botsort::GmcMethod::kNone
        : botsort::GmcMethod::kSparseOptFlow;
    config.gmc_downscale = self->gmc_downscale;
    return config;
}

enum {
    PROP_0,
    PROP_FRAME_RATE,
    PROP_TRACK_THRESH,
    PROP_TRACK_BUFFER,
    PROP_MATCH_THRESH,
    PROP_MOCK_TRACK,
    PROP_TRACK_LOW_THRESH,
    PROP_TRACK_HIGH_THRESH,
    PROP_NEW_TRACK_THRESH,
    PROP_GMC_METHOD,
    PROP_GMC_DOWNSCALE,
};

G_DEFINE_TYPE(SsvTrack, ssv_track, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "video/x-raw, format=(string)BGR, "
        "width=(int)[1, MAX], height=(int)[1, MAX], "
        "framerate=(fraction)[0, MAX]"));

// ── GstBaseTransform callbacks ────────────────────────────────────────

static gboolean
ssv_track_start(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);

    if (self->mock_track) {
        self->mock_next_id = 1;
        GST_INFO_OBJECT(self, "mock-track enabled (sequential IDs)");
    } else {
        const auto config = make_botsort_config(self);
        self->tracker = new botsort::BoTSortTracker(config);
        GST_INFO_OBJECT(self, "BoT-SORT tracker started (buffer=%d, match_thresh=%.2f, track_thresh=%.2f)",
            self->track_buffer, self->match_thresh, self->track_thresh);
    }
    return TRUE;
}

static gboolean
ssv_track_stop(GstBaseTransform *trans) {
    auto *self = SSV_TRACK(trans);
    delete self->tracker;
    self->tracker = nullptr;
    return TRUE;
}

static GstFlowReturn
ssv_track_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    (void)buf;
    auto *self = SSV_TRACK(trans);

    auto det = SsvDetectionStore::instance().take_for_tracking();
    if (det.detections.empty() && det.frame_id == 0) {
        // No fresh detections, pass frame through
        return GST_FLOW_OK;
    }

    if (self->mock_track) {
        // Mock mode: assign sequential IDs, all marked as NEW
        for (auto &d : det.detections) {
            d.track_id = self->mock_next_id++;
            d.track_state = SSV_TRACK_NEW;
            d.occluded = false;
        }
    } else if (self->tracker) {
        GstVideoInfo info;
        gst_video_info_init(&info);
        GstCaps *caps = gst_pad_get_current_caps(trans->sinkpad);
        const bool have_info = caps && gst_video_info_from_caps(&info, caps);
        const int frame_width = have_info ? GST_VIDEO_INFO_WIDTH(&info) : 0;
        const int frame_height = have_info ? GST_VIDEO_INFO_HEIGHT(&info) : 0;
        auto input = ssv_to_botsort_detections(det.detections, frame_width, frame_height);
        botsort::UpdateResult result;
        bool frame_mapped = false;
        GstVideoFrame frame;
        if (g_strcmp0(self->gmc_method, "none") != 0 && have_info &&
            gst_video_frame_map(&frame, &info, buf, GST_MAP_READ)) {
            botsort::FrameView frame_view;
            frame_view.data = static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            frame_view.width = GST_VIDEO_FRAME_WIDTH(&frame);
            frame_view.height = GST_VIDEO_FRAME_HEIGHT(&frame);
            frame_view.stride = static_cast<std::size_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0));
            result = self->tracker->update(input, frame_view);
            frame_mapped = true;
        } else {
            if (g_strcmp0(self->gmc_method, "none") != 0) {
                GST_WARNING_OBJECT(self, "GMC frame unavailable, falling back to no-frame update");
            }
            result = self->tracker->update(input);
        }
        if (frame_mapped) {
            gst_video_frame_unmap(&frame);
        }
        if (caps) {
            gst_caps_unref(caps);
        }
        for (auto &tracked : result.detections) {
            tracked = botsort::to_normalized_detection(tracked, frame_width, frame_height);
        }
        apply_botsort_results(det.detections, result.detections);
        if (!det.detections.empty()) {
            GST_DEBUG_OBJECT(self, "frame %" G_GUINT64_FORMAT ": %zu tracked detections",
                det.frame_id, det.detections.size());
        }
    }

    SsvDetectionStore::instance().set_tracked(std::move(det));
    return GST_FLOW_OK;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_track_set_property(GObject *object, guint prop_id,
                        const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_FRAME_RATE:
        self->frame_rate = g_value_get_int(value);
        break;
    case PROP_TRACK_THRESH:
        self->track_thresh = g_value_get_float(value);
        break;
    case PROP_TRACK_BUFFER:
        self->track_buffer = g_value_get_int(value);
        break;
    case PROP_MATCH_THRESH:
        self->match_thresh = g_value_get_float(value);
        break;
    case PROP_MOCK_TRACK:
        self->mock_track = g_value_get_boolean(value);
        break;
    case PROP_TRACK_LOW_THRESH:
        self->track_low_thresh = g_value_get_float(value);
        break;
    case PROP_TRACK_HIGH_THRESH:
        self->track_high_thresh = g_value_get_float(value);
        break;
    case PROP_NEW_TRACK_THRESH:
        self->new_track_thresh = g_value_get_float(value);
        break;
    case PROP_GMC_METHOD:
        g_free(self->gmc_method);
        self->gmc_method = g_value_dup_string(value);
        break;
    case PROP_GMC_DOWNSCALE:
        self->gmc_downscale = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_track_get_property(GObject *object, guint prop_id,
                        GValue *value, GParamSpec *pspec) {
    auto *self = SSV_TRACK(object);
    switch (prop_id) {
    case PROP_FRAME_RATE:
        g_value_set_int(value, self->frame_rate);
        break;
    case PROP_TRACK_THRESH:
        g_value_set_float(value, self->track_thresh);
        break;
    case PROP_TRACK_BUFFER:
        g_value_set_int(value, self->track_buffer);
        break;
    case PROP_MATCH_THRESH:
        g_value_set_float(value, self->match_thresh);
        break;
    case PROP_MOCK_TRACK:
        g_value_set_boolean(value, self->mock_track);
        break;
    case PROP_TRACK_LOW_THRESH:
        g_value_set_float(value, self->track_low_thresh);
        break;
    case PROP_TRACK_HIGH_THRESH:
        g_value_set_float(value, self->track_high_thresh);
        break;
    case PROP_NEW_TRACK_THRESH:
        g_value_set_float(value, self->new_track_thresh);
        break;
    case PROP_GMC_METHOD:
        g_value_set_string(value, self->gmc_method);
        break;
    case PROP_GMC_DOWNSCALE:
        g_value_set_int(value, self->gmc_downscale);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_track_finalize(GObject *object) {
    auto *self = SSV_TRACK(object);
    delete self->tracker;
    self->tracker = nullptr;
    g_free(self->gmc_method);
    self->gmc_method = nullptr;
    G_OBJECT_CLASS(ssv_track_parent_class)->finalize(object);
}

static void
ssv_track_class_init(SsvTrackClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_track_set_property;
    gobject_class->get_property = ssv_track_get_property;
    gobject_class->finalize = ssv_track_finalize;

    g_object_class_install_property(gobject_class, PROP_FRAME_RATE,
        g_param_spec_int("frame-rate", "Frame Rate",
            "Pipeline frame rate",
            1, 120, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_THRESH,
        g_param_spec_float("track-thresh", "Track Threshold",
            "Tracking confidence threshold",
            0.0f, 1.0f, 0.5f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_BUFFER,
        g_param_spec_int("track-buffer", "Track Buffer",
            "Frames to retain lost tracks",
            1, 300, 30,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MATCH_THRESH,
        g_param_spec_float("match-thresh", "Match Threshold",
            "BoT-SORT matching threshold",
            0.0f, 1.0f, 0.8f,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_MOCK_TRACK,
        g_param_spec_boolean("mock-track", "Mock Track",
            "Assign sequential IDs without real tracking",
            FALSE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_LOW_THRESH,
        g_param_spec_float("track-low-thresh", "Track Low Threshold",
            "Low-confidence detection threshold used by BoT-SORT",
            0.0f, 1.0f, 0.1f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_TRACK_HIGH_THRESH,
        g_param_spec_float("track-high-thresh", "Track High Threshold",
            "High-confidence detection threshold used by BoT-SORT",
            0.0f, 1.0f, 0.6f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_NEW_TRACK_THRESH,
        g_param_spec_float("new-track-thresh", "New Track Threshold",
            "Minimum confidence required to spawn a new BoT-SORT track",
            0.0f, 1.0f, 0.7f,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_GMC_METHOD,
        g_param_spec_string("gmc-method", "GMC Method",
            "Global motion compensation mode",
            "sparse-opt-flow",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_GMC_DOWNSCALE,
        g_param_spec_int("gmc-downscale", "GMC Downscale",
            "Downscale factor used for GMC estimation",
            1, 8, 2,
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV BoT-SORT Tracker",
        "Filter/Effect/Video",
        "Multi-object tracking using BoT-SORT",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_track_start;
    base_class->stop = ssv_track_stop;
    base_class->transform_ip = ssv_track_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_track_init(SsvTrack *self) {
    self->frame_rate = 30;
    self->track_thresh = 0.5f;
    self->track_buffer = 30;
    self->match_thresh = 0.8f;
    self->mock_track = FALSE;
    self->track_low_thresh = 0.1f;
    self->track_high_thresh = 0.6f;
    self->new_track_thresh = 0.7f;
    self->gmc_method = g_strdup("sparse-opt-flow");
    self->gmc_downscale = 2;
    self->tracker = nullptr;
    self->mock_next_id = 1;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_track, "ssvtrack",
    GST_RANK_NONE, SSV_TYPE_TRACK)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_track_debug, "ssv-track");
    return GST_ELEMENT_REGISTER(ssv_track, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvtrack,
    "SSV BoT-SORT Tracker Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
