#!/bin/bash
# scripts/pipeline.sh — 运行整套 GStreamer 链路 / 有界 smoke
#   用法:
#     ./ssv run             RTSP → 推理 → 跟踪 → Redis
#     ./ssv run --display   同一链路额外打开视频观察窗口
#     ./ssv test            由 scripts/test.sh 调用的有界 smoke 路径
set -euo pipefail
source "$(dirname "$0")/lib.sh"
source "$(dirname "$0")/deps.sh"
cd "$SSV_ROOT"

MODE="run"
SHOW_DISPLAY=false
DISPLAY_OVERLAY_OVERRIDE=false
DISPLAY_SINK_OVERRIDE=""
SKIP_BUILD=false

case "${1:-}" in
    "") ;;
    --run)
        MODE="run"
        shift
        ;;
    --smoke|--test)
        MODE="smoke"
        shift
        ;;
    --skip-build)
        SKIP_BUILD=true
        shift
        ;;
    *)
        ssv_error "未知 test/run 参数: $1"
        exit 1
        ;;
esac

while [ "$#" -gt 0 ]; do
    case "$1" in
        --display)
            SHOW_DISPLAY=true
            shift
            ;;
        --overlay)
            SHOW_DISPLAY=true
            DISPLAY_OVERLAY_OVERRIDE=true
            shift
            ;;
        --sink)
            if [ -z "${2:-}" ]; then
                ssv_error "--sink requires a sink name"
                exit 1
            fi
            SHOW_DISPLAY=true
            DISPLAY_SINK_OVERRIDE="$2"
            shift 2
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        *)
            ssv_error "未知 test/run 参数: $1"
            exit 1
            ;;
    esac
done

validate_boolean() {
    local key="$1"
    local value="$2"
    case "$value" in
        true|false) ;;
        *)
            ssv_error "$key 必须是 true 或 false: $value"
            exit 1
            ;;
    esac
}

DISPLAY_ENABLED="$(ssv_yaml_get display.enabled false)"
DISPLAY_OVERLAY_CONFIG="$(ssv_yaml_get display.overlay false)"
DISPLAY_FPS="$(ssv_yaml_get display.fps 30)"
DISPLAY_LATEST_FRAME="$(ssv_yaml_get display.latest_frame true)"
OVERLAY_FONT_FACE="$(ssv_yaml_get display.overlay_font.face regular)"
OVERLAY_FONT_SIZE="$(ssv_yaml_get display.overlay_font.size 7)"
MOTION_PREDICTION_ENABLED="$(ssv_yaml_get display.motion_prediction.enabled true)"
MOTION_PREDICTION_MAX_HORIZON_MS="$(ssv_yaml_get display.motion_prediction.max_horizon_ms 300)"
SOURCE_ID="$(ssv_yaml_get sources.0.name pipeline-0)"

FRAME_WIDTH="$(ssv_yaml_get pipeline.frame_width 640)"
FRAME_HEIGHT="$(ssv_yaml_get pipeline.frame_height 480)"
ANALYSIS_FPS="$(ssv_yaml_get pipeline.analysis_fps 5)"
CONF_THRESHOLD="$(ssv_yaml_get inference.confidence_threshold 0.5)"
INFER_RUNTIME="$(ssv_yaml_get inference.runtime auto)"
INFER_DEVICE="$(ssv_yaml_get inference.device auto)"
INFER_DEVICE_ID="$(ssv_yaml_get inference.device_id 0)"
INFER_PRECISION="$(ssv_yaml_get inference.precision auto)"
MODEL_FAMILY="$(ssv_yaml_get inference.model_family yolo)"
OUTPUT_FORMAT="$(ssv_yaml_get inference.output_format auto)"
RTSP_PROTOCOLS="$(ssv_yaml_get sources.0.protocols tcp)"
RTSP_LATENCY="$(ssv_yaml_get sources.0.latency_ms 200)"
REDIS_HOST="${REDIS_HOST:-$(ssv_yaml_get redis.host localhost)}"
REDIS_PORT="${REDIS_PORT:-$(ssv_yaml_get redis.port 6379)}"
REDIS_STREAM_KEY="$(ssv_yaml_get redis.stream_key ssv:events)"
CHECK_TIMEOUT="$(ssv_yaml_get pipeline.check_timeout 30s)"
GST_DEBUG_LEVEL="${GST_DEBUG:-$(ssv_yaml_get logging.cpp_debug_level "ssv*:4")}"
RTSP_URL="${SSV_RTSP_URL:-$(ssv_yaml_get sources.0.uri "")}"
MODEL="$(ssv_yaml_get inference.model_path models/yolov8n.onnx)"
TARGET_CLASS="$(ssv_yaml_get inference.target_class person)"
LABEL_MAP="$(ssv_yaml_get inference.label_map config/model-labels/coco80.txt)"

validate_boolean "display.enabled" "$DISPLAY_ENABLED"
validate_boolean "display.overlay" "$DISPLAY_OVERLAY_CONFIG"
validate_boolean "display.latest_frame" "$DISPLAY_LATEST_FRAME"
validate_boolean "display.motion_prediction.enabled" "$MOTION_PREDICTION_ENABLED"

if [[ ! "$DISPLAY_FPS" =~ ^[1-9][0-9]*$ ]]; then
    ssv_error "display.fps 必须是正整数: $DISPLAY_FPS"
    exit 1
fi
case "$OVERLAY_FONT_FACE" in
    regular|bold) ;;
    *)
        ssv_error "display.overlay_font.face 必须是 regular 或 bold: $OVERLAY_FONT_FACE"
        exit 1
        ;;
esac
if [[ ! "$OVERLAY_FONT_SIZE" =~ ^[0-9]+$ ]] ||
   [ "$OVERLAY_FONT_SIZE" -lt 7 ] ||
   [ "$OVERLAY_FONT_SIZE" -gt 64 ]; then
    ssv_error "display.overlay_font.size 必须是 7..64 的整数: $OVERLAY_FONT_SIZE"
    exit 1
fi
if [[ ! "$MOTION_PREDICTION_MAX_HORIZON_MS" =~ ^[0-9]+$ ]] ||
   [ "$MOTION_PREDICTION_MAX_HORIZON_MS" -lt 1 ] ||
   [ "$MOTION_PREDICTION_MAX_HORIZON_MS" -gt 300 ]; then
    ssv_error "display.motion_prediction.max_horizon_ms 必须是 1..300 的整数: $MOTION_PREDICTION_MAX_HORIZON_MS"
    exit 1
fi
if [ -z "$SOURCE_ID" ]; then
    ssv_error "sources.0.name 显式配置时不能为空"
    exit 1
fi
if [[ ! "$ANALYSIS_FPS" =~ ^[0-9]+$ ]]; then
    ssv_error "pipeline.analysis_fps 必须是非负整数: $ANALYSIS_FPS"
    exit 1
fi

DISPLAY_OVERLAY="$DISPLAY_OVERLAY_CONFIG"
if [ "$DISPLAY_OVERLAY_OVERRIDE" = true ]; then
    DISPLAY_OVERLAY=true
fi
if [ "$SHOW_DISPLAY" = false ] && [ "$DISPLAY_ENABLED" = true ]; then
    SHOW_DISPLAY=true
fi

if [ -z "$RTSP_URL" ]; then
    ssv_error "RTSP 视频源未配置"
    ssv_warn "在 ssv.yaml 设置 sources[0].uri，或临时设置 SSV_RTSP_URL"
    exit 1
fi

DISPLAY_MAX_LATENESS_NS="$((1000000000 / DISPLAY_FPS))"

ssv_header "检查 GStreamer Pipeline"

ssv_require_command "gst-launch-1.0" \
    "sudo apt-get install gstreamer1.0-tools" \
    "Debian/Ubuntu"

if [ "$MODE" = "smoke" ]; then
    ssv_require_command "timeout" \
        "sudo apt-get install coreutils" \
        "Debian/Ubuntu"
fi

if [ "$SHOW_DISPLAY" = true ]; then
    ssv_require_command "gst-inspect-1.0" \
        "sudo apt-get install gstreamer1.0-tools" \
        "Debian/Ubuntu"
fi

display_sink_supports_property() {
    local sink="$1"
    local property="$2"
    gst-inspect-1.0 "$sink" 2>/dev/null |
        grep -E "^[[:space:]]*${property}[[:space:]]*:" >/dev/null
}

display_sink_supports_latest_frame() {
    local sink="$1"
    display_sink_supports_property "$sink" sync &&
        display_sink_supports_property "$sink" max-lateness
}

validate_display_sink() {
    local sink="$1"
    if ! gst-inspect-1.0 "$sink" >/dev/null 2>&1; then
        ssv_error "display sink 不存在或无法检查: $sink"
        exit 1
    fi
    if [ "$DISPLAY_LATEST_FRAME" = true ] &&
       ! display_sink_supports_latest_frame "$sink"; then
        ssv_error "display sink $sink 不支持 latest-frame 所需属性: sync 和 max-lateness"
        exit 1
    fi
}

resolve_display_sink() {
    local configured_sink="$DISPLAY_SINK_OVERRIDE"
    if [ -z "$configured_sink" ]; then
        configured_sink="$(ssv_yaml_get display.sink "")"
    fi
    if [ -n "$configured_sink" ]; then
        validate_display_sink "$configured_sink"
        printf '%s\n' "$configured_sink"
        return 0
    fi

    local sink
    local candidates=()
    if [ -n "${DISPLAY:-}" ]; then
        candidates+=(gtksink ximagesink xvimagesink glimagesink)
    fi
    if [ -n "${WAYLAND_DISPLAY:-}" ]; then
        candidates+=(waylandsink)
    fi
    if [ "$DISPLAY_LATEST_FRAME" = false ]; then
        candidates+=(autovideosink)
    fi

    for sink in "${candidates[@]}"; do
        if ! gst-inspect-1.0 "$sink" >/dev/null 2>&1; then
            continue
        fi
        if [ "$DISPLAY_LATEST_FRAME" = false ] ||
           display_sink_supports_latest_frame "$sink"; then
            printf '%s\n' "$sink"
            return 0
        fi
    done

    if [ "$DISPLAY_LATEST_FRAME" = true ]; then
        ssv_error "找不到支持 sync 和 max-lateness 的具体视频 sink"
    else
        ssv_error "找不到可用的视频 sink"
    fi
    exit 1
}

DISPLAY_SINK=""
display_sink_args=()
if [ "$SHOW_DISPLAY" = true ]; then
    DISPLAY_SINK="$(resolve_display_sink)"
    display_sink_args=("$DISPLAY_SINK")
    if [ "$DISPLAY_LATEST_FRAME" = true ]; then
        display_sink_args+=("sync=true" "max-lateness=$DISPLAY_MAX_LATENESS_NS")
    elif [ "$DISPLAY_SINK" != "gtksink" ]; then
        display_sink_args+=("sync=false")
    fi
fi

if [ "$SKIP_BUILD" = false ]; then
    bash "$SSV_ROOT/scripts/build.sh"
fi

ssv_deps_load_runtime
export_ssv_plugin_path

if [ ! -f "$MODEL" ]; then
    ssv_error "模型文件不存在: $MODEL"
    ssv_warn "运行 ./ssv download-model 下载模型，或在 ssv.yaml 设置 inference.model_path"
    exit 1
fi

if [ -n "$LABEL_MAP" ] && [ ! -f "$LABEL_MAP" ]; then
    ssv_error "类别表文件不存在: $LABEL_MAP"
    ssv_warn "设置 inference.label_map，或使用默认 config/model-labels/coco80.txt"
    exit 1
fi

if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q '^ssv-redis$'; then
    ssv_warn "Redis 未运行，自动启动..."
    bash "$SSV_ROOT/scripts/redis.sh"
    sleep 2
fi

rtsp_decode_pipeline=(
    rtspsrc "location=$RTSP_URL" "protocols=$RTSP_PROTOCOLS" "latency=$RTSP_LATENCY"
    ! application/x-rtp,media=video
    ! decodebin
    ! clocksync "sync=true"
    ! queue "leaky=downstream" "max-size-buffers=2"
    ! videoconvert
)

infer_props=(
    ssvinfer
    "source-id=$SOURCE_ID"
    "runtime=$INFER_RUNTIME"
    "model-path=$MODEL"
    "conf-threshold=$CONF_THRESHOLD"
    "label-map=$LABEL_MAP"
    "device=$INFER_DEVICE"
    "device-id=$INFER_DEVICE_ID"
    "precision=$INFER_PRECISION"
    "model-family=$MODEL_FAMILY"
    "output-format=$OUTPUT_FORMAT"
    "async=false"
)
if [ -n "$TARGET_CLASS" ]; then
    infer_props+=("target-class=$TARGET_CLASS")
fi

track_props=(
    ssvtrack
    "source-id=$SOURCE_ID"
)

pub_props=(
    ssvpub
    "source-id=$SOURCE_ID"
    "redis-host=$REDIS_HOST"
    "redis-port=$REDIS_PORT"
    "stream-key=$REDIS_STREAM_KEY"
)

overlay_props=(
    ssvoverlay
    "source-id=$SOURCE_ID"
    "motion-prediction=$MOTION_PREDICTION_ENABLED"
    "max-horizon-ms=$MOTION_PREDICTION_MAX_HORIZON_MS"
    "font-face=$OVERLAY_FONT_FACE"
    "font-size=$OVERLAY_FONT_SIZE"
)

analysis_rate_pipeline=(
    ! videoscale
)
if [ "$ANALYSIS_FPS" -gt 0 ]; then
    analysis_rate_pipeline+=(
        ! videorate
        ! "video/x-raw,width=$FRAME_WIDTH,height=$FRAME_HEIGHT,framerate=$ANALYSIS_FPS/1,format=BGR"
    )
    ANALYSIS_FPS_LABEL="${ANALYSIS_FPS}fps"
else
    analysis_rate_pipeline+=(
        ! "video/x-raw,width=$FRAME_WIDTH,height=$FRAME_HEIGHT,format=BGR"
    )
    ANALYSIS_FPS_LABEL="不限流"
fi

analysis_pipeline=(
    "${analysis_rate_pipeline[@]}"
    ! ssvtemplate
    ! "${infer_props[@]}"
    ! "${track_props[@]}"
    ! "${pub_props[@]}"
)

analysis_queue_pipeline=(
    ! queue "leaky=downstream" "max-size-buffers=2"
)

if [ "$DISPLAY_LATEST_FRAME" = "true" ]; then
    display_queue_pipeline=(
        ! queue "max-size-buffers=1" "leaky=downstream"
    )
    display_source_pipeline=(
        ! videoscale
        ! videorate "drop-only=true" "max-rate=$DISPLAY_FPS"
    )
else
    display_queue_pipeline=(
        ! queue "leaky=downstream" "max-size-buffers=2"
    )
    display_source_pipeline=(
        ! videoscale
        ! videorate
        ! "video/x-raw,framerate=$DISPLAY_FPS/1"
    )
fi

ssv_info "输入: $RTSP_URL"
ssv_info "RTSP 网络缓冲: transport=$RTSP_PROTOCOLS, latency=${RTSP_LATENCY}ms"
ssv_info "显示帧率: ${DISPLAY_FPS}fps, 分析帧率: ${ANALYSIS_FPS_LABEL}"
ssv_info "感知 source-id: $SOURCE_ID"
ssv_info "模型: $MODEL"
ssv_info "推理运行时: $INFER_RUNTIME"
ssv_info "推理设备: $INFER_DEVICE (device-id=$INFER_DEVICE_ID, precision=$INFER_PRECISION)"
ssv_info "模型家族: $MODEL_FAMILY, 输出格式: $OUTPUT_FORMAT"
ssv_info "目标类别: ${TARGET_CLASS:-全部类别}"
ssv_info "类别表: ${LABEL_MAP:-内置 COCO}"
ssv_info "Redis Stream: $REDIS_STREAM_KEY"

if [ "$SHOW_DISPLAY" = true ]; then
    ssv_info "模式: 实时链路 + 视频观察窗口 (sink: $DISPLAY_SINK)"
    ssv_info "最新帧策略: latest_frame=$DISPLAY_LATEST_FRAME, max-lateness=${DISPLAY_MAX_LATENESS_NS}ns"
    ssv_info "展示运动预测: enabled=$MOTION_PREDICTION_ENABLED, max-horizon=${MOTION_PREDICTION_MAX_HORIZON_MS}ms"
    ssv_info "Overlay 字体: face=$OVERLAY_FONT_FACE, size=${OVERLAY_FONT_SIZE}px"
    ssv_info "关闭视频窗口即退出"
    if [ "$DISPLAY_OVERLAY" = true ]; then
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. "${display_queue_pipeline[@]}" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "video/x-raw,format=BGRx" ! "${overlay_props[@]}" ! videoconvert ! "video/x-raw,format=BGRx" ! "${display_sink_args[@]}" \
              t. "${analysis_queue_pipeline[@]}" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false async=false
    else
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            ! tee name=t \
              t. "${display_queue_pipeline[@]}" \
                 "${display_source_pipeline[@]}" \
                 ! videoconvert ! "video/x-raw,format=BGRx" ! "${display_sink_args[@]}" \
              t. "${analysis_queue_pipeline[@]}" \
                 "${analysis_pipeline[@]}" \
                 ! fakesink sync=false async=false
    fi
else
    if [ "$MODE" = "smoke" ]; then
        ssv_info "模式: 链路冒烟测试 (timeout: $CHECK_TIMEOUT)"
    else
        ssv_info "模式: 实时链路无头运行"
    fi
    if [ "$MODE" = "smoke" ]; then
        set +e
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        timeout --foreground "$CHECK_TIMEOUT" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            "${analysis_pipeline[@]}" \
            ! fakesink sync=false
        status=$?
        set -e
        if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
            exit "$status"
        fi
        ssv_info "链路冒烟测试完成"
    else
        GST_DEBUG="$GST_DEBUG_LEVEL" \
        gst-launch-1.0 \
            "${rtsp_decode_pipeline[@]}" \
            "${analysis_pipeline[@]}" \
            ! fakesink sync=false
    fi
fi

EVENT_COUNT=$(docker exec ssv-redis redis-cli XLEN "$REDIS_STREAM_KEY" 2>/dev/null || echo "?")
ssv_info "Redis $REDIS_STREAM_KEY 中累计 $EVENT_COUNT 条事件"
