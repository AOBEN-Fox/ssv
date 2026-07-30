#include "gstssvpub.hpp"
#include "ssv_logging.hpp"
#include "ssv_meta.hpp"
#include "ssv_review_candidate.hpp"

#include <gst/video/video.h>
#include <hiredis/hiredis.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <chrono>
#include <cstring>
#include <ctime>
#include <utility>

GST_DEBUG_CATEGORY_STATIC(ssv_pub_debug);

struct _SsvPub {
    GstBaseTransform parent;

    gchar *source_id;
    gchar *redis_host;
    gint redis_port;
    gchar *stream_key;
    gboolean review_enabled;
    gchar *review_stream_key;
    gchar *events_root;
    GstVideoInfo video_info;
    gboolean have_video_info;
    SsvReviewDeduplicator *review_deduplicator;

    redisContext *redis_ctx;
    SsvSourceMeta *meta;
};

enum {
    PROP_0,
    PROP_SOURCE_ID,
    PROP_REDIS_HOST,
    PROP_REDIS_PORT,
    PROP_STREAM_KEY,
    PROP_REVIEW_ENABLED,
    PROP_REVIEW_STREAM_KEY,
    PROP_EVENTS_ROOT,
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

// ── Redis helpers ──────────────────────────────────────────────────────

std::string
ssv_pub_build_event_payload(const SsvTrackedFrame &frame, std::int64_t timestamp_ms) {
    using json = nlohmann::json;

    json detections_arr = json::array();
    for (const auto &object : frame.objects) {
        const auto &d = object.detection;
        json det_obj = {
            {"class", d.class_name},
            {"class_id", d.class_id},
            {"confidence", d.confidence},
            {"bbox", {d.x1, d.y1, d.x2, d.y2}},
            {"track_id", object.track_id},
            {"track_state", object.track_state},
            {"occluded", object.occluded}
        };
        detections_arr.push_back(det_obj);
    }

    json msg = {
        {"type", "detection"},
        {"source", frame.source_id},
        {"timestamp_ms", timestamp_ms},
        {"frame_id", frame.frame_id},
        {"detections", detections_arr}
    };

    return msg.dump();
}

bool
ssv_pub_snapshot_is_current(
    std::string_view source_id,
    const SsvTrackedFrame &frame)
{
    return !source_id.empty() && frame.source_id == source_id &&
        ssv_meta(source_id)->generation() == frame.timing.generation;
}

bool
ssv_pub_review_snapshot_matches_buffer(
    std::string_view source_id,
    const SsvTrackedFrame &frame,
    GstClockTime buffer_pts)
{
    return ssv_pub_snapshot_is_current(source_id, frame) &&
        buffer_pts != GST_CLOCK_TIME_NONE && frame.timing.pts == buffer_pts;
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

static bool
ssv_pub_redis_publish_json(
    redisContext *context, std::string_view stream, std::string_view payload,
    SsvPub *self, bool reconnect_on_failure) {
    if (!context)
        return false;
    auto *reply = (redisReply *)redisCommand(context,
        "XADD %s * event %s",
        std::string(stream).c_str(), std::string(payload).c_str());

    if (!reply) {
        GST_WARNING_OBJECT(self, "Redis XADD failed, reconnecting...");
        if (reconnect_on_failure)
            ssv_pub_redis_connect(self);
        return false;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        GST_WARNING_OBJECT(self, "Redis error: %s", reply->str);
        freeReplyObject(reply);
        return false;
    }
    freeReplyObject(reply);
    return true;
}

static void
ssv_pub_redis_publish(SsvPub *self, const SsvTrackedFrame &frame) {
    const auto payload = ssv_pub_build_event_payload(frame, std::time(nullptr) * 1000LL);
    if (!ssv_pub_redis_publish_json(self->redis_ctx, self->stream_key, payload, self, true))
        return;

    GST_DEBUG_OBJECT(self, "published frame %" G_GUINT64_FORMAT " with %zu detections",
        frame.frame_id, frame.objects.size());
}

static void
ssv_pub_publish_review_candidates(SsvPub *self, const SsvTrackedFrame &snapshot, GstBuffer *buffer)
{
    if (!self->have_video_info || !self->review_deduplicator ||
        !ssv_pub_review_snapshot_matches_buffer(self->source_id, snapshot, GST_BUFFER_PTS(buffer)))
        return;
    self->review_deduplicator->reset_for_generation(snapshot.timing.generation);
    bool has_candidate = false;
    for (const auto &object : snapshot.objects) {
        if (ssv_review_object_is_eligible(object) && !self->review_deduplicator->already_published(
                ssv_review_dedup_key(snapshot.source_id, snapshot.timing.generation, object.track_id))) {
            has_candidate = true;
            break;
        }
    }
    if (!has_candidate)
        return;
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &self->video_info, buffer, GST_MAP_READ)) {
        GST_WARNING_OBJECT(self, "无法映射 BGR 复验证据帧");
        return;
    }
    std::string error;
    const auto evidence = ssv_encode_bgr_jpeg(
        static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
        GST_VIDEO_INFO_WIDTH(&self->video_info),
        GST_VIDEO_INFO_HEIGHT(&self->video_info), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), 90, &error);
    gst_video_frame_unmap(&frame);
    if (!evidence) {
        GST_WARNING_OBJECT(self, "复验证据编码失败: %s", error.c_str());
        return;
    }
    const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto &object : snapshot.objects) {
        const auto candidate = ssv_review_make_candidate(
            snapshot, object, timestamp_ms, evidence->sha256, evidence->width, evidence->height);
        if (!candidate || self->review_deduplicator->already_published(
                ssv_review_dedup_key(snapshot.source_id, snapshot.timing.generation, object.track_id)))
            continue;
        const auto result = ssv_review_try_publish(
            self->events_root, self->review_stream_key, *candidate, *evidence,
            *self->review_deduplicator,
            [self](std::string_view stream, std::string_view payload) {
                return ssv_pub_redis_publish_json(self->redis_ctx, stream, payload, self, true);
            }, &error);
        if (result == SsvReviewPublishResult::Failed)
            GST_WARNING_OBJECT(self, "复验候选发布失败: %s", error.c_str());
    }
}

// ── GstBaseTransform callbacks ────────────────────────────────────────

static gboolean
ssv_pub_start(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    if (!self->source_id || self->source_id[0] == '\0') {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS,
            ("source-id must not be empty"), (nullptr));
        return FALSE;
    }
    self->meta = ssv_meta(self->source_id).get();
    return ssv_pub_redis_connect(self);
}

static gboolean
ssv_pub_stop(GstBaseTransform *trans) {
    auto *self = SSV_PUB(trans);
    if (self->redis_ctx) {
        redisFree(self->redis_ctx);
        self->redis_ctx = nullptr;
    }
    self->meta = nullptr;
    return TRUE;
}

static GstFlowReturn
ssv_pub_transform_ip(GstBaseTransform *trans, GstBuffer *buf) {
    auto *self = SSV_PUB(trans);

    if (!self->meta)
        return GST_FLOW_OK;
    auto consumed = self->meta->consume_tracked();
    if (consumed.result != SsvMetaResult::Consumed || !consumed.frame)
        return GST_FLOW_OK;

    const auto snapshot = std::move(consumed.frame);
    if (snapshot->objects.empty())
        return GST_FLOW_OK;
    if (!ssv_pub_snapshot_is_current(self->source_id, *snapshot)) {
        return GST_FLOW_OK;
    }
    ssv_pub_redis_publish(self, *snapshot);
    if (self->review_enabled)
        ssv_pub_publish_review_candidates(self, *snapshot, buf);

    return GST_FLOW_OK;
}

static gboolean
ssv_pub_set_caps(GstBaseTransform *trans, GstCaps *in_caps, GstCaps *) {
    auto *self = SSV_PUB(trans);
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (!gst_video_info_from_caps(&info, in_caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGR)
        return FALSE;
    self->video_info = info;
    self->have_video_info = TRUE;
    return TRUE;
}

// ── Properties ─────────────────────────────────────────────────────────

static void
ssv_pub_set_property(GObject *object, guint prop_id,
                      const GValue *value, GParamSpec *pspec) {
    auto *self = SSV_PUB(object);
    switch (prop_id) {
    case PROP_SOURCE_ID:
        g_free(self->source_id);
        self->source_id = g_value_dup_string(value);
        break;
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
    case PROP_REVIEW_ENABLED:
        self->review_enabled = g_value_get_boolean(value);
        break;
    case PROP_REVIEW_STREAM_KEY:
        g_free(self->review_stream_key);
        self->review_stream_key = g_value_dup_string(value);
        break;
    case PROP_EVENTS_ROOT:
        g_free(self->events_root);
        self->events_root = g_value_dup_string(value);
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
    case PROP_SOURCE_ID:
        g_value_set_string(value, self->source_id);
        break;
    case PROP_REDIS_HOST:
        g_value_set_string(value, self->redis_host);
        break;
    case PROP_REDIS_PORT:
        g_value_set_int(value, self->redis_port);
        break;
    case PROP_STREAM_KEY:
        g_value_set_string(value, self->stream_key);
        break;
    case PROP_REVIEW_ENABLED:
        g_value_set_boolean(value, self->review_enabled);
        break;
    case PROP_REVIEW_STREAM_KEY:
        g_value_set_string(value, self->review_stream_key);
        break;
    case PROP_EVENTS_ROOT:
        g_value_set_string(value, self->events_root);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

// ── Class / instance init ──────────────────────────────────────────────

static void
ssv_pub_finalize(GObject *object) {
    auto *self = SSV_PUB(object);
    g_free(self->source_id);
    g_free(self->redis_host);
    g_free(self->stream_key);
    g_free(self->review_stream_key);
    g_free(self->events_root);
    delete self->review_deduplicator;
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

    g_object_class_install_property(gobject_class, PROP_SOURCE_ID,
        g_param_spec_string("source-id", "Source ID",
            "Perception metadata source identifier",
            "pipeline-0",
            (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY |
                          G_PARAM_STATIC_STRINGS)));

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
    g_object_class_install_property(gobject_class, PROP_REVIEW_ENABLED,
        g_param_spec_boolean("review-enabled", "Review Enabled",
            "Archive and publish first head frame per track", FALSE,
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(gobject_class, PROP_REVIEW_STREAM_KEY,
        g_param_spec_string("review-stream-key", "Review Stream Key",
            "Redis Stream key for review candidates", "ssv:review-candidates",
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(gobject_class, PROP_EVENTS_ROOT,
        g_param_spec_string("events-root", "Events Root",
            "Root directory for review event artifacts", "artifacts/events",
            (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

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
    base_class->set_caps = ssv_pub_set_caps;
    base_class->passthrough_on_same_caps = TRUE;
}

static void
ssv_pub_init(SsvPub *self) {
    self->source_id = g_strdup("pipeline-0");
    self->redis_host = g_strdup("localhost");
    self->redis_port = 6379;
    self->stream_key = g_strdup("ssv:events");
    self->review_enabled = FALSE;
    self->review_stream_key = g_strdup("ssv:review-candidates");
    self->events_root = g_strdup("artifacts/events");
    gst_video_info_init(&self->video_info);
    self->have_video_info = FALSE;
    self->review_deduplicator = new SsvReviewDeduplicator();
    self->redis_ctx = nullptr;
    self->meta = nullptr;
}

// ── Plugin registration ────────────────────────────────────────────────

GST_ELEMENT_REGISTER_DEFINE(ssv_pub, "ssvpub",
    GST_RANK_NONE, SSV_TYPE_PUB)

static gboolean
plugin_init(GstPlugin *plugin) {
    SSV_GST_DEBUG_INIT(ssv_pub_debug, "ssv-pub");
    return GST_ELEMENT_REGISTER(ssv_pub, plugin);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    ssvpub,
    "SSV Redis Publisher Plugin",
    plugin_init,
    "0.1.0", "LGPL",
    "site-safety-vision",
    "https://github.com/site-safety-vision/site-safety-vision")
