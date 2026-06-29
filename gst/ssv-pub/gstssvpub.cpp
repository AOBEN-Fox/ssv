#include "gstssvpub.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <optional>
#include <sstream>

GST_DEBUG_CATEGORY_STATIC(ssv_pub_debug);

struct _SsvPub {
    GstBaseTransform parent;

    gchar *redis_host;
    gint redis_port;
    gchar *stream_key;
    gboolean helmet_event_enabled;
    gchar *helmet_trigger_class;
    gboolean publish_detection_events;
    gchar *evidence_output_dir;

    redisContext *redis_ctx;
};

enum {
    PROP_0,
    PROP_REDIS_HOST,
    PROP_REDIS_PORT,
    PROP_STREAM_KEY,
    PROP_HELMET_EVENT_ENABLED,
    PROP_HELMET_TRIGGER_CLASS,
    PROP_PUBLISH_DETECTION_EVENTS,
    PROP_EVIDENCE_OUTPUT_DIR,
};

G_DEFINE_TYPE(SsvPub, ssv_pub, GST_TYPE_BASE_TRANSFORM)

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

std::string ssv_pub_sanitize_filename_component(const std::string &value) {
    std::string out;
    for (char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        out.push_back(ok ? ch : '_');
    }
    return out.empty() ? "unknown" : out;
}

std::string ssv_pub_build_evidence_frame_path(
    const std::string &output_dir,
    const SsvFrameDetections &det,
    std::int64_t timestamp_ms) {
    const std::string source = ssv_pub_sanitize_filename_component(det.source_id[0] ? det.source_id : "unknown");
    std::ostringstream path;
    path << output_dir << "/" << source << "/" << timestamp_ms << "-frame-" << det.frame_id << ".jpg";
    return path.str();
}

std::string
ssv_pub_build_event_payload(const SsvFrameDetections &det, std::int64_t timestamp_ms) {
    using json = nlohmann::json;

    json detections_arr = json::array();
    for (const auto &d : det.detections) {
        json det_obj = {
            {"class", d.class_name},
            {"class_id", d.class_id},
            {"confidence", d.confidence},
            {"bbox", {d.x1, d.y1, d.x2, d.y2}},
            {"track_id", d.track_id},
            {"track_state", d.track_state},
            {"occluded", d.occluded}
        };
        detections_arr.push_back(det_obj);
    }

    json msg = {
        {"type", "detection"},
        {"source", det.source_id},
        {"timestamp_ms", timestamp_ms},
        {"frame_id", det.frame_id},
        {"detections", detections_arr}
    };

    return msg.dump();
}

std::optional<std::string> ssv_pub_build_helmet_violation_payload(
    const SsvFrameDetections &det,
    std::int64_t timestamp_ms,
    const std::string &trigger_class,
    const char *frame_path,
    int frame_width,
    int frame_height,
    const char *missing_reason) {
    using json = nlohmann::json;

    json detections_arr = json::array();
    for (const auto &d : det.detections) {
        if (trigger_class != d.class_name)
            continue;
        detections_arr.push_back({
            {"class", d.class_name},
            {"class_id", d.class_id},
            {"confidence", d.confidence},
            {"bbox", {d.x1, d.y1, d.x2, d.y2}},
            {"track_id", d.track_id},
            {"track_state", d.track_state},
            {"occluded", d.occluded}
        });
    }

    if (detections_arr.empty())
        return std::nullopt;

    const std::string source = det.source_id[0] ? det.source_id : "unknown";
    std::ostringstream event_id;
    event_id << source << ":" << det.frame_id << ":" << timestamp_ms;

    json evidence = {
        {"frame_path", frame_path ? json(frame_path) : json(nullptr)},
        {"frame_format", "jpeg"},
        {"frame_width", frame_width},
        {"frame_height", frame_height},
        {"missing_reason", missing_reason ? json(missing_reason) : json(nullptr)}
    };

    json msg = {
        {"schema_version", "m4.helmet_violation.v1"},
        {"type", "helmet_violation"},
        {"event_id", event_id.str()},
        {"source", source},
        {"timestamp_ms", timestamp_ms},
        {"frame_id", det.frame_id},
        {"severity", "warning"},
        {"trigger_reason", "head_detected"},
        {"detections", detections_arr},
        {"evidence", evidence},
        {"agent_state", missing_reason ? "manual_review" : "pending"}
    };

    return msg.dump();
}

static gboolean
ssv_pub_redis_connect(SsvPub *self) {
    if (self->redis_ctx) {
        redisFree(self->redis_ctx);
        self->redis_ctx = nullptr;
    }

    struct timeval timeout = { 2, 0 };
    self->redis_ctx = redisConnectWithTimeout(self->redis_host, self->redis_port, timeout);

    if (!self->redis_ctx || self->redis_ctx->err) {
        if (self->redis_ctx) {
            GST_ERROR_OBJECT(self, "Redis connect failed: %s", self->redis_ctx->errstr);
            redisFree(self->redis_ctx);
            self->redis_ctx = nullptr;
        } else {
            GST_ERROR_OBJECT(self, "Redis connect failed: allocation error");
        }
        return FALSE;
    }

    GST_INFO_OBJECT(self, "connected to Redis at %s:%d", self->redis_host, self->redis_port);
    return TRUE;
}

static void
ssv_pub_redis_publish_payload(SsvPub *self, const std::string &payload) {
    if (!self->redis_ctx)
        return;

    auto *reply = (redisReply *)redisCommand(self->redis_ctx,
        "XADD %s * event %s",
        self->stream_key, payload.c_str());

    if (!reply) {
        GST_WARNING_OBJECT(self, "Redis XADD failed, reconnecting...");
        ssv_pub_redis_connect(self);
        return;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        GST_WARNING_OBJECT(self, "Redis error: %s", reply->str);
    }
    freeReplyObject(reply);
}

static bool
ssv_pub_write_jpeg_from_bgr_buffer(
    SsvPub *self,
    GstBuffer *buf,
    const std::string &path,
    int *frame_width,
    int *frame_height) {
    GstCaps *caps = gst_pad_get_current_caps(GST_BASE_TRANSFORM_SINK_PAD(self));
    if (!caps)
        return false;

    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, caps)) {
        gst_caps_unref(caps);
        return false;
    }
    gst_caps_unref(caps);

    *frame_width = GST_VIDEO_INFO_WIDTH(&info);
    *frame_height = GST_VIDEO_INFO_HEIGHT(&info);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src is-live=false format=time ! videoconvert ! jpegenc ! filesink name=sink",
        &err);
    if (!pipeline) {
        if (err)
            g_error_free(err);
        return false;
    }

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    GstCaps *src_caps = gst_video_info_to_caps(&info);
    g_object_set(src, "caps", src_caps, nullptr);
    g_object_set(sink, "location", path.c_str(), nullptr);
    gst_caps_unref(src_caps);

    GstBuffer *copy = gst_buffer_copy_deep(buf);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(src), copy);
    if (flow == GST_FLOW_OK)
        flow = gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    const bool ok = flow == GST_FLOW_OK && msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg)
        gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return ok;
}

static gboolean
ssv_pub_start(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    return ssv_pub_redis_connect(self);
}

static gboolean
ssv_pub_stop(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    if (self->redis_ctx) {
        redisFree(self->redis_ctx);
        self->redis_ctx = nullptr;
    }
    return TRUE;
}

static GstFlowReturn
ssv_pub_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    (void)buf;
    auto *self = SSV_PUB(trans);

    auto det = SsvDetectionStore::instance().take();
    if (det.detections.empty())
        return GST_FLOW_OK;

    const auto timestamp_ms = std::time(nullptr) * 1000LL;
    if (self->publish_detection_events) {
        ssv_pub_redis_publish_payload(self, ssv_pub_build_event_payload(det, timestamp_ms));
    }

    if (self->helmet_event_enabled) {
        std::string frame_path = ssv_pub_build_evidence_frame_path(self->evidence_output_dir, det, timestamp_ms);
        int width = 0;
        int height = 0;
        const bool saved = ssv_pub_write_jpeg_from_bgr_buffer(self, buf, frame_path, &width, &height);
        auto payload = ssv_pub_build_helmet_violation_payload(
            det,
            timestamp_ms,
            self->helmet_trigger_class ? self->helmet_trigger_class : "head",
            saved ? frame_path.c_str() : nullptr,
            width,
            height,
            saved ? nullptr : "write_failed");
        if (payload.has_value())
            ssv_pub_redis_publish_payload(self, *payload);
    }

    GST_DEBUG_OBJECT(self, "published frame %" G_GUINT64_FORMAT " with %zu detections",
        det.frame_id, det.detections.size());
    return GST_FLOW_OK;
}

static void
ssv_pub_set_property(GObject *object, guint prop_id,
                      const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_PUB(object);
    switch (prop_id) {
    case PROP_REDIS_HOST:
        g_free(self->redis_host);
        self->redis_host = g_value_dup_string(value);
        break;
    case PROP_REDIS_PORT:
        self->redis_port = g_value_get_int(value);
        break;
    case PROP_STREAM_KEY:
        g_free(self->stream_key);
        self->stream_key = g_value_dup_string(value);
        break;
    case PROP_HELMET_EVENT_ENABLED:
        self->helmet_event_enabled = g_value_get_boolean(value);
        break;
    case PROP_HELMET_TRIGGER_CLASS:
        g_free(self->helmet_trigger_class);
        self->helmet_trigger_class = g_value_dup_string(value);
        break;
    case PROP_PUBLISH_DETECTION_EVENTS:
        self->publish_detection_events = g_value_get_boolean(value);
        break;
    case PROP_EVIDENCE_OUTPUT_DIR:
        g_free(self->evidence_output_dir);
        self->evidence_output_dir = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_pub_get_property(GObject *object, guint prop_id,
                      GValue *value, GParamSpec *pspec) {
    auto *self = SSV_PUB(object);
    switch (prop_id) {
    case PROP_REDIS_HOST:
        g_value_set_string(value, self->redis_host);
        break;
    case PROP_REDIS_PORT:
        g_value_set_int(value, self->redis_port);
        break;
    case PROP_STREAM_KEY:
        g_value_set_string(value, self->stream_key);
        break;
    case PROP_HELMET_EVENT_ENABLED:
        g_value_set_boolean(value, self->helmet_event_enabled);
        break;
    case PROP_HELMET_TRIGGER_CLASS:
        g_value_set_string(value, self->helmet_trigger_class);
        break;
    case PROP_PUBLISH_DETECTION_EVENTS:
        g_value_set_boolean(value, self->publish_detection_events);
        break;
    case PROP_EVIDENCE_OUTPUT_DIR:
        g_value_set_string(value, self->evidence_output_dir);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
ssv_pub_finalize(GObject *object) {
    auto *self = SSV_PUB(object);
    g_free(self->redis_host);
    g_free(self->stream_key);
    g_free(self->helmet_trigger_class);
    g_free(self->evidence_output_dir);
    if (self->redis_ctx)
        redisFree(self->redis_ctx);
    G_OBJECT_CLASS(ssv_pub_parent_class)->finalize(object);
}

static void
ssv_pub_class_init(SsvPubClass *klass) {
    auto *gobject_class = G_OBJECT_CLASS(klass);
    auto *base_class = GST_BASE_TRANSFORM_CLASS(klass);
    auto *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->set_property = ssv_pub_set_property;
    gobject_class->get_property = ssv_pub_get_property;
    gobject_class->finalize = ssv_pub_finalize;

    g_object_class_install_property(gobject_class, PROP_REDIS_HOST,
        g_param_spec_string("redis-host", "Redis Host",
            "Redis server hostname",
            "localhost", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_REDIS_PORT,
        g_param_spec_int("redis-port", "Redis Port",
            "Redis server port",
            1, 65535, 6379,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_STREAM_KEY,
        g_param_spec_string("stream-key", "Stream Key",
            "Redis Stream key for detection events",
            "ssv:events", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_HELMET_EVENT_ENABLED,
        g_param_spec_boolean("helmet-event-enabled", "Helmet Event Enabled",
            "Enable helmet violation event publishing",
            TRUE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_HELMET_TRIGGER_CLASS,
        g_param_spec_string("helmet-trigger-class", "Helmet Trigger Class",
            "Class name that triggers helmet violation events",
            "head", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_PUBLISH_DETECTION_EVENTS,
        g_param_spec_boolean("publish-detection-events", "Publish Detection Events",
            "Continue publishing generic detection events",
            TRUE, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property(gobject_class, PROP_EVIDENCE_OUTPUT_DIR,
        g_param_spec_string("evidence-output-dir", "Evidence Output Directory",
            "Directory for JPEG evidence frames",
            "artifacts/evidence", (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class,
        "SSV Redis Publisher",
        "Generic/Video",
        "Publish detection events to Redis Streams",
        "site-safety-vision");

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    base_class->start = ssv_pub_start;
    base_class->stop = ssv_pub_stop;
    base_class->transform_ip = ssv_pub_transform_ip;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_pub_init(SsvPub *self) {
    self->redis_host = g_strdup("localhost");
    self->redis_port = 6379;
    self->stream_key = g_strdup("ssv:events");
    self->helmet_event_enabled = TRUE;
    self->helmet_trigger_class = g_strdup("head");
    self->publish_detection_events = TRUE;
    self->evidence_output_dir = g_strdup("artifacts/evidence");
    self->redis_ctx = nullptr;
}


static gboolean
ssv_pub_plugin_init(GstPlugin *plugin) {
    GST_DEBUG_CATEGORY_INIT(ssv_pub_debug, "ssvpub", 0, "SSV redis publisher");
    return gst_element_register(plugin, "ssvpub", GST_RANK_NONE, SSV_TYPE_PUB);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    ssvpub,
    "Publish detection and helmet violation events to Redis Streams",
    ssv_pub_plugin_init,
    "0.1.0",
    "LGPL",
    "site-safety-vision",
    "site-safety-vision")
