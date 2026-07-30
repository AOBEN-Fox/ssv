#include "ssv_preprocessor.hpp"
#include "ssv_meta.hpp"
#include "ssv_yolo_parser.hpp"

#include <gst/check/gstcheck.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

extern void run_overlay_renderer_contract_tests();

static void assert_element_factory(const char *name) {
    GstElement *element = gst_element_factory_make(name, nullptr);
    fail_unless(element != nullptr, "missing element factory: %s", name);
    gst_object_unref(element);
}

static SsvDetectionFrame make_plugin_detection_frame(
    const char *source_id,
    std::uint64_t frame_id,
    GstClockTime pts,
    std::uint64_t generation)
{
    SsvDetectionFrame frame;
    frame.frame_id = frame_id;
    frame.source_id = source_id;
    frame.timing = {pts, GST_SECOND / 30, generation};
    SsvDetection object;
    std::snprintf(object.class_name, sizeof(object.class_name), "person");
    object.confidence = 0.9F;
    object.x1 = 0.1F;
    object.y1 = 0.1F;
    object.x2 = 0.4F;
    object.y2 = 0.4F;
    object.class_id = 0;
    frame.detections.push_back(object);
    return frame;
}

static GstBuffer *make_plugin_bgr_buffer(GstClockTime pts)
{
    GstBuffer *buffer = gst_buffer_new_allocate(
        nullptr, 64 * 48 * 3, nullptr);
    fail_unless(buffer != nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    return buffer;
}

GST_START_TEST(test_ssv_plugin_factories_are_registered) {
    assert_element_factory("ssvtemplate");
    assert_element_factory("ssvinfer");
    assert_element_factory("ssvtrack");
    assert_element_factory("ssvpub");
    assert_element_factory("ssvoverlay");
}
GST_END_TEST

GST_START_TEST(test_ssvpub_review_properties_default_to_disabled) {
    GstElement *pub = gst_element_factory_make("ssvpub", nullptr);
    fail_unless(pub != nullptr);
    gboolean review_enabled = TRUE;
    gchar *review_stream_key = nullptr;
    gchar *events_root = nullptr;
    g_object_get(pub,
        "review-enabled", &review_enabled,
        "review-stream-key", &review_stream_key,
        "events-root", &events_root,
        nullptr);
    fail_unless(!review_enabled);
    fail_unless(std::string(review_stream_key) == "ssv:review-candidates");
    fail_unless(std::string(events_root) == "artifacts/events");
    g_free(review_stream_key);
    g_free(events_root);
    gst_object_unref(pub);
}
GST_END_TEST

GST_START_TEST(test_infer_track_preserve_controlled_buffer_timing) {
    const char *source_id = "plugin-timing-test";
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! "
        "video/x-raw,format=BGR,width=64,height=48,framerate=30/1 ! "
        "ssvinfer source-id=plugin-timing-test mock-detect=true async=false ! "
        "ssvtrack source-id=plugin-timing-test mock-track=true gmc-method=none ! "
        "fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);

    auto meta = ssv_meta(source_id);
    const auto generation = meta->generation();
    auto snapshot = meta->latest_tracked_at_or_before(GST_SECOND);
    fail_unless(snapshot != nullptr);
    fail_unless(snapshot->frame_id == 0);
    fail_unless(snapshot->source_id == source_id);
    fail_unless(snapshot->timing.pts == 0);
    fail_unless(snapshot->timing.duration == GST_SECOND / 30);
    fail_unless(snapshot->timing.generation == generation);
    fail_unless(snapshot->objects.size() == 1);
    fail_unless(snapshot->objects.front().track_id == 1);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_rejects_wrong_source_observation) {
    const char *bound_source = "plugin-wrong-source-a";
    auto meta = ssv_meta(bound_source);
    SsvTimelineCursor timeline(meta);
    const auto update = timeline.on_segment({0, 0, 0, 1.0});

    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGR,width=64,height=48,framerate=30/1 ! "
        "ssvtrack name=track source-id=plugin-wrong-source-a "
        "mock-track=true gmc-method=none ! fakesink sync=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *track = gst_bin_get_by_name(GST_BIN(pipeline), "track");
    fail_unless(src != nullptr);
    fail_unless(track != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    g_object_set(track, "source-id", "plugin-wrong-source-b", nullptr);
    gchar *configured_source = nullptr;
    g_object_get(track, "source-id", &configured_source, nullptr);
    fail_unless(configured_source != nullptr);
    fail_unless(std::string(configured_source) == "plugin-wrong-source-b");
    g_free(configured_source);

    auto detection = make_plugin_detection_frame(
        bound_source, 0, GST_SECOND, update.generation);
    fail_unless(meta->publish_detection(std::move(detection)) ==
        SsvMetaResult::Published);

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_plugin_bgr_buffer(GST_SECOND)) == GST_FLOW_OK);
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(message != nullptr);
    fail_unless(GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR);

    gst_message_unref(message);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(track);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_resets_mock_state_for_consumed_generation) {
    const char *source_id = "plugin-track-generation-test";
    auto meta = ssv_meta(source_id);
    SsvTimelineCursor timeline(meta);
    auto update = timeline.on_segment({0, 0, 0, 1.0});

    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGR,width=64,height=48,framerate=30/1 ! "
        "ssvtrack source-id=plugin-track-generation-test "
        "mock-track=true gmc-method=none ! "
        "appsink name=sink sync=false emit-signals=false",
        nullptr);
    fail_unless(pipeline != nullptr);
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    auto publish_and_process = [&](std::uint64_t frame_id,
                                   GstClockTime pts,
                                   std::uint64_t generation) {
        auto detection = make_plugin_detection_frame(
            source_id, frame_id, pts, generation);
        fail_unless(meta->publish_detection(std::move(detection)) ==
            SsvMetaResult::Published);

        fail_unless(gst_app_src_push_buffer(
            GST_APP_SRC(src), make_plugin_bgr_buffer(pts)) == GST_FLOW_OK);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        fail_unless(sample != nullptr);
        gst_sample_unref(sample);

        auto tracked = meta->consume_tracked();
        fail_unless(tracked.result == SsvMetaResult::Consumed);
        fail_unless(tracked.frame != nullptr);
        fail_unless(tracked.frame->timing.generation == generation);
        fail_unless(tracked.frame->objects.size() == 1);
        fail_unless(tracked.frame->objects.front().track_id == 1);
    };

    publish_and_process(1, GST_SECOND, update.generation);
    update = timeline.on_lifecycle_reset();
    publish_and_process(2, 2 * GST_SECOND, update.generation);

    gst_app_src_end_of_stream(GST_APP_SRC(src));
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_perception_plugins_share_source_id_contract) {
    const char *plugin_names[] = {"ssvinfer", "ssvtrack", "ssvpub", "ssvoverlay"};
    for (const char *plugin_name : plugin_names) {
        GstElement *element = gst_element_factory_make(plugin_name, nullptr);
        fail_unless(element != nullptr);
        gchar *source_id = nullptr;
        g_object_get(element, "source-id", &source_id, nullptr);
        fail_unless(source_id != nullptr);
        fail_unless(std::string(source_id) == "pipeline-0");
        g_free(source_id);

        g_object_set(element, "source-id", "camera-01", nullptr);
        g_object_get(element, "source-id", &source_id, nullptr);
        fail_unless(source_id != nullptr);
        fail_unless(std::string(source_id) == "camera-01");
        g_free(source_id);
        gst_object_unref(element);
    }

    GstElement *overlay = gst_element_factory_make("ssvoverlay", nullptr);
    gboolean motion_prediction = FALSE;
    guint max_horizon_ms = 0;
    gchar *font_face = nullptr;
    guint font_size = 0;
    g_object_get(overlay,
        "motion-prediction", &motion_prediction,
        "max-horizon-ms", &max_horizon_ms,
        "font-face", &font_face,
        "font-size", &font_size,
        nullptr);
    fail_unless(motion_prediction);
    fail_unless(max_horizon_ms == 300);
    fail_unless(font_face != nullptr);
    fail_unless(std::string(font_face) == "regular");
    fail_unless(font_size == 7);
    g_free(font_face);
    g_object_set(overlay,
        "motion-prediction", FALSE,
        "max-horizon-ms", 125U,
        "font-face", "bold",
        "font-size", 14U,
        nullptr);
    g_object_get(overlay,
        "motion-prediction", &motion_prediction,
        "max-horizon-ms", &max_horizon_ms,
        "font-face", &font_face,
        "font-size", &font_size,
        nullptr);
    fail_unless(!motion_prediction);
    fail_unless(max_horizon_ms == 125);
    fail_unless(font_face != nullptr);
    fail_unless(std::string(font_face) == "bold");
    fail_unless(font_size == 14);
    g_free(font_face);
    gst_object_unref(overlay);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_label_map_property) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *target_class = nullptr;
    g_object_get(element, "target-class", &target_class, nullptr);
    fail_unless(target_class != nullptr);
    fail_unless(std::string(target_class).empty());
    g_free(target_class);

    g_object_set(element, "label-map", "config/model-labels/coco80.txt", nullptr);
    gchar *label_map = nullptr;
    g_object_get(element, "label-map", &label_map, nullptr);
    fail_unless(label_map != nullptr);
    fail_unless(std::string(label_map) == "config/model-labels/coco80.txt");

    g_free(label_map);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_exposes_runtime_properties) {
    GstElement *element = gst_element_factory_make("ssvinfer", nullptr);
    fail_unless(element != nullptr);

    gchar *runtime = nullptr;
    gchar *device = nullptr;
    gchar *precision = nullptr;
    gchar *model_family = nullptr;
    gchar *output_format = nullptr;
    gint device_id = -1;
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        "device-id", &device_id,
        "precision", &precision,
        "model-family", &model_family,
        "output-format", &output_format,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(std::string(runtime) == "auto");
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "auto");
    fail_unless(device_id == 0);
    fail_unless(precision != nullptr);
    fail_unless(std::string(precision) == "auto");
    fail_unless(model_family != nullptr);
    fail_unless(std::string(model_family) == "yolo");
    fail_unless(output_format != nullptr);
    fail_unless(std::string(output_format) == "auto");
    g_free(runtime);
    g_free(device);
    g_free(precision);
    g_free(model_family);
    g_free(output_format);

    g_object_set(element,
        "runtime", "onnxruntime",
        "device", "gpu",
        "device-id", 1,
        "precision", "fp32",
        "model-family", "yolo",
        "output-format", "yolov8",
        nullptr);
    g_object_get(element,
        "runtime", &runtime,
        "device", &device,
        "device-id", &device_id,
        "precision", &precision,
        "model-family", &model_family,
        "output-format", &output_format,
        nullptr);
    fail_unless(runtime != nullptr);
    fail_unless(std::string(runtime) == "onnxruntime");
    fail_unless(device != nullptr);
    fail_unless(std::string(device) == "gpu");
    fail_unless(device_id == 1);
    fail_unless(precision != nullptr);
    fail_unless(std::string(precision) == "fp32");
    fail_unless(model_family != nullptr);
    fail_unless(std::string(model_family) == "yolo");
    fail_unless(output_format != nullptr);
    fail_unless(std::string(output_format) == "yolov8");

    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cuda-device-id") == nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cuda-required") == nullptr);

    g_free(runtime);
    g_free(device);
    g_free(precision);
    g_free(model_family);
    g_free(output_format);
    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_preprocessor_outputs_nchw_rgb_tensor) {
    ssv::infer::SsvVideoFrame frame;
    frame.frame_id = 1;
    frame.source_id = "unit-test";
    frame.width = 1;
    frame.height = 1;
    frame.stride = 3;
    frame.bgr = {10, 20, 30};

    ssv::infer::TensorSpec input;
    input.name = "images";
    input.shape = {1, 3, 1, 1};
    input.layout = ssv::infer::TensorLayout::Nchw;

    ssv::infer::Preprocessor preprocessor;
    auto result = preprocessor.run(frame, input);

    fail_unless(result.input.host_data.size() == 3);
    fail_unless(std::fabs(result.input.host_data[0] - (30.0f / 255.0f)) < 0.0001f);
    fail_unless(std::fabs(result.input.host_data[1] - (20.0f / 255.0f)) < 0.0001f);
    fail_unless(std::fabs(result.input.host_data[2] - (10.0f / 255.0f)) < 0.0001f);
}
GST_END_TEST

GST_START_TEST(test_ssvinfer_yolo_parser_parses_nx6_output) {
    ssv::infer::InferenceConfig config;
    config.output_format = ssv::infer::OutputFormat::YoloNx6;
    config.confidence_threshold = 0.5f;
    config.target_class = "head";

    ssv::infer::ModelMetadata metadata;
    ssv::infer::TensorSpec output_spec;
    output_spec.name = "output0";
    output_spec.shape = {1, 2, 6};
    metadata.outputs.push_back(output_spec);

    ssv::infer::YoloOutputParser parser;
    parser.configure(config, metadata, {"helmet", "head"});

    ssv::infer::Tensor output;
    output.spec = output_spec;
    output.host_data = {
        0.1f, 0.2f, 0.4f, 0.8f, 0.9f, 1.0f,
        0.2f, 0.2f, 0.5f, 0.8f, 0.4f, 1.0f,
    };

    ssv::infer::PreprocessResult preprocess;
    preprocess.original_width = 640;
    preprocess.original_height = 640;
    preprocess.model_width = 640;
    preprocess.model_height = 640;

    auto detections = parser.parse({output}, preprocess);

    fail_unless(detections.size() == 1);
    fail_unless(detections[0].class_id == 1);
    fail_unless(std::string(detections[0].class_name) == "head");
    fail_unless(std::fabs(detections[0].confidence - 0.9f) < 0.0001f);
}
GST_END_TEST

GST_START_TEST(test_ssvtrack_exposes_botsort_properties) {
    GstElement *element = gst_element_factory_make("ssvtrack", nullptr);
    fail_unless(element != nullptr);

    gfloat match_thresh = 0.0f;
    g_object_get(element, "match-thresh", &match_thresh, nullptr);
    fail_unless(match_thresh == 0.8f);

    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "track-low-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "track-high-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "new-track-thresh") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "gmc-method") != nullptr);
    fail_unless(g_object_class_find_property(G_OBJECT_GET_CLASS(element), "gmc-downscale") != nullptr);

    gchar *gmc_method = nullptr;
    g_object_get(element, "gmc-method", &gmc_method, nullptr);
    fail_unless(gmc_method != nullptr);
    fail_unless(std::string(gmc_method) == "sparse-opt-flow");
    g_free(gmc_method);

    g_object_set(element,
        "frame-rate", 25,
        "track-thresh", 0.55f,
        "track-buffer", 45,
        "match-thresh", 0.85f,
        "track-low-thresh", 0.15f,
        "track-high-thresh", 0.65f,
        "new-track-thresh", 0.75f,
        "gmc-method", "none",
        "gmc-downscale", 3,
        nullptr);

    gint frame_rate = 0;
    gfloat track_thresh = 0.0f;
    gint track_buffer = 0;
    gfloat configured_match_thresh = 0.0f;
    gfloat track_low_thresh = 0.0f;
    gfloat track_high_thresh = 0.0f;
    gfloat new_track_thresh = 0.0f;
    gint gmc_downscale = 0;
    g_object_get(element,
        "frame-rate", &frame_rate,
        "track-thresh", &track_thresh,
        "track-buffer", &track_buffer,
        "match-thresh", &configured_match_thresh,
        "track-low-thresh", &track_low_thresh,
        "track-high-thresh", &track_high_thresh,
        "new-track-thresh", &new_track_thresh,
        "gmc-method", &gmc_method,
        "gmc-downscale", &gmc_downscale,
        nullptr);
    fail_unless(frame_rate == 25);
    fail_unless(fabsf(track_thresh - 0.55f) < 0.0001f);
    fail_unless(track_buffer == 45);
    fail_unless(fabsf(configured_match_thresh - 0.85f) < 0.0001f);
    fail_unless(fabsf(track_low_thresh - 0.15f) < 0.0001f);
    fail_unless(fabsf(track_high_thresh - 0.65f) < 0.0001f);
    fail_unless(fabsf(new_track_thresh - 0.75f) < 0.0001f);
    fail_unless(gmc_method != nullptr);
    fail_unless(std::string(gmc_method) == "none");
    fail_unless(gmc_downscale == 3);
    g_free(gmc_method);

    gst_object_unref(element);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_rgb_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=RGB,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_runs_on_bgrx_buffer) {
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=1 ! video/x-raw,format=BGRx,width=64,height=48 ! "
        "ssvoverlay ! fakesink", nullptr);
    fail_unless(pipeline != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND,
        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    fail_unless(msg != nullptr);
    fail_unless(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_draws_latest_detection) {
    const char *source_id = "overlay-draw-test";
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGRx,width=64,height=48,framerate=30/1 ! "
        "ssvoverlay source-id=overlay-draw-test motion-prediction=false ! "
        "appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    fail_unless(ret != GST_STATE_CHANGE_FAILURE);

    auto make_black_buffer = [](GstClockTime pts) {
        constexpr gsize frame_size = 64 * 48 * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
        fail_unless(buffer != nullptr);
        gst_buffer_memset(buffer, 0, 0, frame_size);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
        return buffer;
    };

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_black_buffer(0)) == GST_FLOW_OK);
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    fail_unless(sample != nullptr);
    gst_sample_unref(sample);

    auto meta = ssv_meta(source_id);
    const auto generation = meta->generation();
    fail_unless(generation > 0);

    SsvDetectionFrame observation;
    observation.frame_id = 1;
    observation.source_id = source_id;
    observation.timing = {GST_SECOND / 30, GST_SECOND / 30, generation};
    SsvTrackedObject object;
    std::snprintf(
        object.detection.class_name,
        sizeof(object.detection.class_name),
        "person");
    object.detection.confidence = 0.9F;
    object.detection.x1 = 0.1F;
    object.detection.y1 = 0.1F;
    object.detection.x2 = 0.4F;
    object.detection.y2 = 0.4F;
    object.detection.class_id = 0;
    object.track_id = 7;
    object.track_state = SSV_TRACK_MATCHED;
    fail_unless(meta->publish_tracked(
                    std::move(observation), {std::move(object)}) ==
        SsvMetaResult::Published);

    fail_unless(gst_app_src_push_buffer(
        GST_APP_SRC(src), make_black_buffer(GST_SECOND / 30)) == GST_FLOW_OK);
    sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    fail_unless(sample != nullptr);

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    gst_video_info_from_caps(&info, caps);
    GstVideoFrame frame;
    fail_unless(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ));
    const uint8_t *px = static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    int x = 6;
    int y = 10;
    const uint8_t *p = px + y * stride + x * 4;
    fail_unless(p[0] == 0 && p[1] == 255 && p[2] == 0, "overlay did not draw green pixel");
    gst_video_frame_unmap(&frame);

    gst_sample_unref(sample);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

GST_START_TEST(test_ssvoverlay_tracks_controlled_timeline_events) {
    const char *source_id = "overlay-timeline-test";
    GstElement *pipeline = gst_parse_launch(
        "appsrc name=src format=time is-live=false block=true ! "
        "video/x-raw,format=BGRx,width=64,height=48,framerate=30/1 ! "
        "ssvoverlay source-id=overlay-timeline-test ! "
        "appsink name=sink sync=false emit-signals=false max-buffers=1 drop=false",
        nullptr);
    fail_unless(pipeline != nullptr);

    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    fail_unless(src != nullptr);
    fail_unless(sink != nullptr);
    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);

    auto push_and_pull = [&](GstClockTime pts, bool discontinuity) {
        constexpr gsize frame_size = 64 * 48 * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);
        fail_unless(buffer != nullptr);
        gst_buffer_memset(buffer, 0, 0, frame_size);
        GST_BUFFER_PTS(buffer) = pts;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
        if (discontinuity)
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        fail_unless(gst_app_src_push_buffer(
            GST_APP_SRC(src), buffer) == GST_FLOW_OK);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        fail_unless(sample != nullptr);
        gst_sample_unref(sample);
    };

    push_and_pull(100 * GST_MSECOND, false);
    auto meta = ssv_meta(source_id);
    const auto initial_generation = meta->generation();
    fail_unless(initial_generation > 0);

    push_and_pull(GST_CLOCK_TIME_NONE, false);
    fail_unless(meta->generation() == initial_generation);

    push_and_pull(200 * GST_MSECOND, true);
    const auto discontinuity_generation = meta->generation();
    fail_unless(discontinuity_generation == initial_generation + 1);

    push_and_pull(300 * GST_MSECOND, false);
    push_and_pull(250 * GST_MSECOND, false);
    const auto rollback_generation = meta->generation();
    fail_unless(rollback_generation == discontinuity_generation + 1);

    GstPad *src_pad = gst_element_get_static_pad(src, "src");
    fail_unless(src_pad != nullptr);
    fail_unless(gst_pad_push_event(src_pad, gst_event_new_flush_start()));
    fail_unless(gst_pad_push_event(src_pad, gst_event_new_flush_stop(TRUE)));
    gst_object_unref(src_pad);
    const auto flush_generation = meta->generation();
    fail_unless(flush_generation == rollback_generation + 1);

    fail_unless(gst_element_set_state(pipeline, GST_STATE_NULL) !=
        GST_STATE_CHANGE_FAILURE);
    const auto stopped_generation = meta->generation();
    fail_unless(stopped_generation == flush_generation + 1);

    fail_unless(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE);
    push_and_pull(400 * GST_MSECOND, false);
    fail_unless(meta->generation() >= stopped_generation);

    gst_object_unref(src);
    gst_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
GST_END_TEST

static Suite *ssv_gst_suite() {
    Suite *suite = suite_create("ssv-gst");
    TCase *tc = tcase_create("plugins");
    tcase_add_test(tc, test_ssv_plugin_factories_are_registered);
    tcase_add_test(tc, test_ssvpub_review_properties_default_to_disabled);
    tcase_add_test(tc, test_infer_track_preserve_controlled_buffer_timing);
    tcase_add_test(tc, test_ssvtrack_rejects_wrong_source_observation);
    tcase_add_test(tc, test_ssvtrack_resets_mock_state_for_consumed_generation);
    tcase_add_test(tc, test_perception_plugins_share_source_id_contract);
    tcase_add_test(tc, test_ssvinfer_exposes_label_map_property);
    tcase_add_test(tc, test_ssvinfer_exposes_runtime_properties);
    tcase_add_test(tc, test_ssvinfer_preprocessor_outputs_nchw_rgb_tensor);
    tcase_add_test(tc, test_ssvinfer_yolo_parser_parses_nx6_output);
    tcase_add_test(tc, test_ssvtrack_exposes_botsort_properties);
    tcase_add_test(tc, test_ssvoverlay_runs_on_rgb_buffer);
    tcase_add_test(tc, test_ssvoverlay_runs_on_bgrx_buffer);
    tcase_add_test(tc, test_ssvoverlay_draws_latest_detection);
    tcase_add_test(tc, test_ssvoverlay_tracks_controlled_timeline_events);
    suite_add_tcase(suite, tc);
    return suite;
}

int main(int argc, char **argv) {
    gst_check_init(&argc, &argv);
    run_overlay_renderer_contract_tests();

    Suite *suite = ssv_gst_suite();
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed == 0 ? 0 : 1;
}
