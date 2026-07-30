#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() {
    echo "test failed: $*" >&2
    exit 1
}

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

assert_invalid_runtime_config() {
    local name="$1"
    local expected="$2"
    local yaml="$3"
    local config_path="$TMP_DIR/$name.yaml"
    local output

    printf '%s\n' "$yaml" >"$config_path"
    if output="$(SSV_CONFIG_PATH="$config_path" bash scripts/pipeline.sh --run --skip-build 2>&1)"; then
        fail "$name should reject invalid runtime config"
    fi
    grep -Fq "$expected" <<<"$output" || {
        printf '%s\n' "$output" >&2
        fail "$name did not report the expected validation error: $expected"
    }
    if grep -Fq '检查 GStreamer Pipeline' <<<"$output"; then
        fail "$name was not rejected before external pipeline preparation"
    fi
}

assert_arg_sequence() {
    local capture_path="$1"
    local description="$2"
    shift 2
    local -a expected=("$@")
    local matched=0
    local arg

    while IFS= read -r arg; do
        if [ "$arg" = "${expected[$matched]}" ]; then
            matched=$((matched + 1))
            if [ "$matched" -eq "${#expected[@]}" ]; then
                return 0
            fi
        elif [ "$arg" = "${expected[0]}" ]; then
            matched=1
        else
            matched=0
        fi
    done <"$capture_path"

    fail "$description"
}

help_output="$(./ssv --help)"

grep -q "  clean" <<<"$help_output" || fail "help does not list clean"
grep -q "  run" <<<"$help_output" || fail "help does not list run"
grep -q "run --display" <<<"$help_output" || fail "help does not list run --display"
grep -q -- "--overlay" <<<"$help_output" || fail "help does not list --overlay"
grep -q -- "--sink" <<<"$help_output" || fail "help does not list --sink"
grep -q "  test" <<<"$help_output" || fail "help does not list test"
grep -q "运行代码测试和链路冒烟测试后退出" <<<"$help_output" || fail "help does not describe test as exit-style"

for legacy in "--m2" "--m2-mock" "--m3" "--m3-mock" "check" "all"; do
    if grep -q -- "$legacy" <<<"$help_output"; then
        fail "help still lists legacy command: $legacy"
    fi
done

grep -q 'SSV_RTSP_URL' .env.example || fail ".env.example does not document SSV_RTSP_URL"
grep -q 'config/ssv.yaml' scripts/lib.sh || fail "scripts/lib.sh does not search config/ssv.yaml"
grep -q 'config/ssv.yaml' .gitignore || fail ".gitignore does not ignore local config/ssv.yaml"

loader_root="$TMP_DIR/config-loader"
mkdir -p "$loader_root/scripts" "$loader_root/config"
cp scripts/lib.sh "$loader_root/scripts/lib.sh"
printf '%s\n' $'redis:\n  host: example-only-host' >"$loader_root/config/ssv.example.yaml"
loader_result="$(env -u SSV_CONFIG_PATH bash -c \
    'source "$1"; printf "%s|%s" "$SSV_CONFIG" "$(ssv_yaml_get redis.host builtin-host)"' \
    _ "$loader_root/scripts/lib.sh")"
[ "$loader_result" = '|builtin-host' ] ||
    fail "scripts/lib.sh uses config/ssv.example.yaml as a default runtime config"

if rg -n 'python -m ssv_agent --config "\$SSV_CONFIG"' scripts/agent.sh; then
    fail "scripts/agent.sh always passes an empty default config path"
fi
if rg -n 'SSV_RTSP_PROTOCOLS|SSV_RTSP_LATENCY|SSV_CHECK_TIMEOUT|SSV_DISPLAY_OVERLAY' .env.example >/tmp/ssv-env-runtime-matches.txt; then
    cat /tmp/ssv-env-runtime-matches.txt >&2
    fail ".env.example should not document YAML-owned runtime overrides"
fi
grep -q 'rtspsrc' scripts/pipeline.sh || fail "pipeline script does not use explicit rtspsrc"
grep -q 'protocols=\$RTSP_PROTOCOLS' scripts/pipeline.sh || fail "pipeline script does not pass RTSP transport"
grep -q 'application/x-rtp,media=video' scripts/pipeline.sh || fail "pipeline script does not filter RTSP video stream"
grep -q 'videorate' scripts/pipeline.sh || fail "pipeline script does not normalize RTSP framerate"
grep -q 'ssv_yaml_get pipeline.analysis_fps' scripts/pipeline.sh || fail "pipeline script does not read analysis fps from YAML"
grep -q 'ssv_yaml_get sources.0.protocols' scripts/pipeline.sh || fail "pipeline script does not read RTSP transport from YAML"
grep -q 'ssv_yaml_get sources.0.latency_ms' scripts/pipeline.sh || fail "pipeline script does not read RTSP latency from YAML"
grep -q 'ssv_yaml_get sources.0.name pipeline-0' scripts/pipeline.sh || fail "pipeline script does not default source id to pipeline-0"
grep -q 'ssv_yaml_get display.latest_frame true' scripts/pipeline.sh || fail "pipeline script does not read latest-frame mode"
grep -q 'ssv_yaml_get display.overlay_font.face regular' scripts/pipeline.sh || fail "pipeline script does not read overlay font face"
grep -q 'ssv_yaml_get display.overlay_font.size 7' scripts/pipeline.sh || fail "pipeline script does not read overlay font size"
grep -q 'ssv_yaml_get display.motion_prediction.enabled true' scripts/pipeline.sh || fail "pipeline script does not read motion prediction switch"
grep -q 'ssv_yaml_get display.motion_prediction.max_horizon_ms 300' scripts/pipeline.sh || fail "pipeline script does not read motion prediction horizon"
grep -q '不参与默认配置搜索' config/ssv.example.yaml || fail "example YAML does not declare its template-only boundary"
grep -q 'latest_frame: true' config/ssv.example.yaml || fail "example YAML does not enable latest-frame mode by default"
grep -q 'overlay_font:' config/ssv.example.yaml || fail "example YAML does not contain overlay font config"
grep -q 'face: regular' config/ssv.example.yaml || fail "example YAML does not use the regular overlay font"
grep -q 'size: 7' config/ssv.example.yaml || fail "example YAML does not preserve the existing overlay font size"
grep -q 'motion_prediction:' config/ssv.example.yaml || fail "example YAML does not contain motion prediction config"
grep -q 'max_horizon_ms: 300' config/ssv.example.yaml || fail "example YAML does not use the confirmed prediction horizon"
grep -q 'sink: ""' config/ssv.example.yaml || fail "example YAML does not leave display sink on compatible auto selection"
grep -q 'display_source_pipeline' scripts/pipeline.sh || fail "display mode does not split before inference"
grep -q 'display_queue_pipeline' scripts/pipeline.sh || fail "display mode does not use one shared display queue definition"
grep -q 'analysis_queue_pipeline' scripts/pipeline.sh || fail "analysis branch does not keep an independent queue definition"
grep -q 'DISPLAY_OVERLAY' scripts/pipeline.sh || fail "display overlay is not controlled by an explicit switch"
if rg -n '^DISPLAY=' scripts/pipeline.sh >/tmp/ssv-display-var-matches.txt; then
    cat /tmp/ssv-display-var-matches.txt >&2
    fail "pipeline script must not overwrite the desktop DISPLAY environment variable"
fi
grep -q 'ssvoverlay' scripts/pipeline.sh || fail "display overlay mode does not enable detection overlay"
grep -q 'video/x-raw,format=BGRx' scripts/pipeline.sh || fail "display overlay branch does not use display-friendly BGRx format"
grep -q '"${overlay_props\[@\]}"' scripts/pipeline.sh || fail "display overlay branch does not use the shared overlay properties"
grep -q 'display_source_pipeline.*' scripts/pipeline.sh || fail "display source pipeline missing"
grep -q 'exec bash "$SCRIPTS_DIR/pipeline.sh" --run "$@"' ssv || fail "ssv does not pass run arguments through to pipeline script"
grep -q 'exec bash "$SCRIPTS_DIR/test.sh"' ssv || fail "ssv does not dispatch test command to the test orchestrator"
grep -q 'DISPLAY_SINK_OVERRIDE' scripts/pipeline.sh || fail "pipeline script does not use explicit display sink override"
grep -q 'candidates+=(gtksink' scripts/pipeline.sh || fail "pipeline script does not prefer a GTK window sink"
grep -q 'display_sink_supports_property' scripts/pipeline.sh || fail "pipeline script does not probe sink properties"
grep -q 'max-lateness' scripts/pipeline.sh || fail "pipeline script does not enforce sink max-lateness"
if rg -n 'gtksink 使用单链路显示|\$DISPLAY_SINK" = "gtksink"' scripts/pipeline.sh >/tmp/ssv-gtksink-single-chain-matches.txt; then
    cat /tmp/ssv-gtksink-single-chain-matches.txt >&2
    fail "gtksink should use the same tee display path as other sinks"
fi
grep -q 'leaky=downstream' scripts/pipeline.sh || fail "display mode queues are not configured as leaky"
grep -q '"max-size-buffers=1"' scripts/pipeline.sh || fail "latest-frame display queue is not bounded to one waiting frame"
grep -q '"drop-only=true"' scripts/pipeline.sh || fail "latest-frame display videorate can still duplicate old frames"
grep -q '"sync=true"' scripts/pipeline.sh || fail "latest-frame display sink is not clock synchronized"
grep -q 'DISPLAY_MAX_LATENESS_NS="\$((1000000000 / DISPLAY_FPS))"' scripts/pipeline.sh || fail "display max-lateness is not one configured frame period"
grep -q 'DISPLAY_LATEST_FRAME" = "true"' scripts/pipeline.sh || fail "pipeline script does not preserve the latest-frame rollback branch"
grep -q 'fakesink sync=false async=false' scripts/pipeline.sh || fail "display analysis branch fakesink should be async=false"
grep -q -- '--smoke' scripts/pipeline.sh || fail "pipeline script does not accept smoke mode"
grep -q -- '--skip-build' scripts/pipeline.sh || fail "pipeline script does not support skipping build for tests"
grep -q '#include <thread>' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not use a worker thread"
grep -q 'PROP_ASYNC_INFER' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not expose async inference"
grep -q 'latest_frame' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer does not keep latest frame for async inference"
grep -q '"async=false"' scripts/pipeline.sh || fail "pipeline script does not keep inference and tracking ordered"
if rg -n '"async=true"' scripts/pipeline.sh; then
    fail "pipeline script must not decouple inference from the following tracker"
fi
grep -q 'ssv_yaml_get' scripts/lib.sh || fail "scripts/lib.sh does not expose YAML config reader"
grep -q 'ssv_yaml_get inference.model_path' scripts/pipeline.sh || fail "pipeline script does not read model path from YAML"
grep -q 'ssv_yaml_get pipeline.analysis_fps' scripts/pipeline.sh || fail "pipeline script does not read analysis fps from YAML"
grep -q 'analysis_rate_pipeline' scripts/pipeline.sh || fail "pipeline script does not build analysis rate branch"
grep -q 'ANALYSIS_FPS_LABEL="不限流"' scripts/pipeline.sh || fail "pipeline script does not support unlimited analysis fps"
grep -q 'framerate=\$ANALYSIS_FPS/1' scripts/pipeline.sh || fail "pipeline script does not keep capped analysis fps path"
grep -q 'ssv_yaml_get redis.stream_key' scripts/pipeline.sh || fail "pipeline script does not read Redis stream key from YAML"
if rg -n 'SSV_MODEL_PATH|SSV_TARGET_CLASS|SSV_LABEL_MAP|SSV_FRAME_WIDTH|SSV_FRAME_HEIGHT|SSV_ANALYSIS_FPS|SSV_CONF_THRESHOLD|SSV_INFER_RUNTIME|SSV_INFER_DEVICE|SSV_INFER_DEVICE_ID|SSV_INFER_PRECISION|SSV_MODEL_FAMILY|SSV_OUTPUT_FORMAT|SSV_RTSP_PROTOCOLS|SSV_RTSP_LATENCY|SSV_CHECK_TIMEOUT|SSV_DISPLAY_OVERLAY|SSV_DISPLAY_SINK|SSV_REDIS_STREAM_KEY|SSV_DISPLAY_FPS|SSV_CUDA_DEVICE_ID|SSV_CUDA_REQUIRED|cuda-device-id|cuda-required' scripts/pipeline.sh >/tmp/ssv-runtime-env-matches.txt; then
    cat /tmp/ssv-runtime-env-matches.txt >&2
    fail "pipeline script must not expose YAML-owned runtime environment overrides"
fi
if rg -n 'SSV_CUDA_DEVICE_ID|SSV_CUDA_REQUIRED|cuda-device-id|cuda-required' scripts/pipeline.sh >/tmp/ssv-cuda-config-matches.txt; then
    cat /tmp/ssv-cuda-config-matches.txt >&2
    fail "pipeline script must not expose CUDA-specific inference config"
fi
grep -q 'runtime=\$INFER_RUNTIME' scripts/pipeline.sh || fail "pipeline script does not pass runtime to ssvinfer"
grep -q 'device=\$INFER_DEVICE' scripts/pipeline.sh || fail "pipeline script does not pass device to ssvinfer"
grep -q 'device-id=\$INFER_DEVICE_ID' scripts/pipeline.sh || fail "pipeline script does not pass device id to ssvinfer"
grep -q 'precision=\$INFER_PRECISION' scripts/pipeline.sh || fail "pipeline script does not pass precision to ssvinfer"
grep -q 'model-family=\$MODEL_FAMILY' scripts/pipeline.sh || fail "pipeline script does not pass model family to ssvinfer"
grep -q 'output-format=\$OUTPUT_FORMAT' scripts/pipeline.sh || fail "pipeline script does not pass output format to ssvinfer"
grep -q 'source-id=\$SOURCE_ID' scripts/pipeline.sh || fail "pipeline script does not pass the configured source id"
grep -q 'motion-prediction=\$MOTION_PREDICTION_ENABLED' scripts/pipeline.sh || fail "pipeline script does not pass the motion prediction switch"
grep -q 'max-horizon-ms=\$MOTION_PREDICTION_MAX_HORIZON_MS' scripts/pipeline.sh || fail "pipeline script does not pass the motion prediction horizon"
grep -q 'font-face=\$OVERLAY_FONT_FACE' scripts/pipeline.sh || fail "pipeline script does not pass the overlay font face"
grep -q 'font-size=\$OVERLAY_FONT_SIZE' scripts/pipeline.sh || fail "pipeline script does not pass the overlay font size"
for props in infer_props track_props pub_props overlay_props; do
    grep -A20 "^${props}=(" scripts/pipeline.sh | grep -q 'source-id=\$SOURCE_ID' ||
        fail "$props does not use the shared source id"
done
grep -q 'source "$(dirname "$0")/deps.sh"' scripts/build.sh || fail "build script does not use the unified dependency entry point"
grep -q 'SSV_ONNXRUNTIME_VERSION=1.25.1-gpu' .env.example || fail ".env.example does not document the ONNX Runtime GPU version suffix"
grep -q 'managed 当前固定使用 OpenCV 4.10.0' .env.example || fail ".env.example does not document the managed OpenCV version"
grep -q 'opencv-managed.sh 的包来源、模块 SONAME 和运行库闭包' .env.example || fail ".env.example does not document managed OpenCV upgrade requirements"
grep -q 'SSV_TENSORRT_URL' .env.example || fail ".env.example does not document explicit TensorRT URL configuration"
grep -q 'SSV_TENSORRT_ARCHIVE' .env.example || fail ".env.example does not document TensorRT archive configuration"
grep -q 'SSV_DEPS_TENSORRT_MESON_MODE' scripts/build.sh || fail "build script does not pass the resolved TensorRT status to Meson"
if rg -n 'TensorRT-Enterprise|default_url' scripts/deps/tensorrt-managed.sh >/tmp/ssv-tensorrt-default-url-matches.txt; then
    cat /tmp/ssv-tensorrt-default-url-matches.txt >&2
    fail "build script must not choose a default TensorRT SDK URL"
fi
grep -q 'ssv_deps_load_runtime' scripts/pipeline.sh || fail "pipeline does not load the successful dependency snapshot"
grep -q 'ssv_deps_load_runtime' scripts/inspect.sh || fail "inspect does not load the successful dependency snapshot"
if rg -n 'site-packages|nvidia/.*/lib|nvidia/\*/lib' scripts/lib.sh; then
    fail "runtime script must not scan Python NVIDIA wheel paths"
fi
grep -q 'if \[ -n "\$TARGET_CLASS" \]' scripts/pipeline.sh || fail "pipeline script does not omit empty target class"
grep -q 'infer_props+=("target-class=\$TARGET_CLASS")' scripts/pipeline.sh || fail "pipeline script does not pass non-empty target class to ssvinfer"
grep -q 'label-map=\$LABEL_MAP' scripts/pipeline.sh || fail "pipeline script does not pass label map to ssvinfer"
grep -q 'review-enabled=\$REVIEW_ENABLED' scripts/pipeline.sh || fail "pipeline script does not pass review enabled"
grep -q 'review-stream-key=\$REVIEW_CANDIDATE_STREAM' scripts/pipeline.sh || fail "pipeline script does not pass review candidate stream"
grep -q 'events-root=\$EVENTS_ROOT' scripts/pipeline.sh || fail "pipeline script does not pass events root"
grep -q 'models/comp-2-freeze10.onnx' config/ssv.example.yaml || fail "YAML config does not default to helmet model"
grep -q 'config/model-labels/helmet.txt' config/ssv.example.yaml || fail "YAML config does not default to helmet labels"
grep -q 'scripts/model/verify_helmet_models.py' docs/安全帽模型验证说明.md || fail "helmet verification docs do not use scripts/model path"
grep -q 'Run YOLO ONNX inference on video frames' gst/ssv-infer/gstssvinfer.cpp || fail "ssvinfer metadata changed unexpectedly"
grep -q 'class SsvSourceMeta' gst/ssv-common/include/ssv_meta.hpp || fail "source-scoped perception metadata contract is missing"
grep -q 'std::make_shared<const SsvTrackedFrame>' gst/ssv-common/ssv_meta.cpp || fail "immutable tracked snapshot sharing is missing"
for legacy_contract in \
    gst/ssv-common/include/ssv_frame_types.hpp \
    gst/ssv-common/include/ssv_timeline.hpp \
    gst/ssv-common/include/ssv_result_channels.hpp \
    gst/ssv-common/include/ssv_result_exchange.hpp \
    gst/ssv-common/ssv_result_channels.cpp \
    gst/ssv-common/ssv_result_exchange.cpp; do
    [ ! -e "$legacy_contract" ] || fail "legacy perception metadata contract still exists: $legacy_contract"
done
grep -q "subdir('ssv-overlay')" gst/meson.build || fail "overlay plugin is not included in Meson"
grep -q 'GST_ELEMENT_REGISTER_DEFINE(ssv_overlay, "ssvoverlay"' gst/ssv-overlay/gstssvoverlay.cpp || fail "ssvoverlay plugin is missing"

if rg -n 'builddir' ssv scripts README.md .env.example >/tmp/ssv-builddir-matches.txt; then
    cat /tmp/ssv-builddir-matches.txt >&2
    fail "scripts or docs still reference builddir"
fi

grep -q 'SSV_BUILD_DIR.*build' scripts/lib.sh || fail "scripts/lib.sh does not define SSV_BUILD_DIR"
grep -q 'rm -rf.*SSV_BUILD_DIR' scripts/clean.sh || fail "scripts/clean.sh does not remove SSV_BUILD_DIR"

grep -q 'ssv_deps_prepare' scripts/build.sh || fail 'build script does not prepare dependencies through deps.sh'
awk '
    /rm -rf -- "\$SSV_BUILD_DIR"/ { cleanup = NR }
    /^[[:space:]]*ssv_deps_prepare$/ { prepare = NR }
    END { exit !(cleanup && prepare && cleanup < prepare) }
' scripts/build.sh || fail 'build script must reset an invalid Meson directory before writing the pending dependency snapshot'
grep -q 'downloads/opencv' scripts/deps/opencv-managed.sh || fail 'OpenCV packages are not cached by its provider'
grep -q 'SSV_OPENCV_PACKAGE_REVISION' scripts/deps/opencv-managed.sh || fail 'OpenCV package details are not private to its provider'
grep -q 'pc_dir/opencv4.pc' scripts/deps/opencv-managed.sh || fail 'OpenCV provider does not generate opencv4.pc'
if rg -n 'dpkg-deb' scripts/deps/opencv-managed.sh; then fail 'OpenCV provider must use ar and tar, not dpkg-deb'; fi
grep -q "option('opencv_mode'" meson.options || fail 'Meson does not expose the unified OpenCV mode'
grep -q "opencv_mode = get_option('opencv_mode')" meson.build || fail 'Meson does not read the unified OpenCV mode'
grep -q 'opencv_enabled' meson.build || fail 'Meson does not gate OpenCV discovery by build mode'
grep -q 'opencv_enabled' gst/ssv-track/meson.build || fail 'track does not gate GMC on OpenCV mode'
grep -q 'opencv_enabled' gst/tests/meson.build || fail 'tests do not gate OpenCV on build mode'

if rg -n 'SSV_ONNXRUNTIME_FLAVOR|SSV_TENSORRT_VERSION|SSV_OPENCV=|SSV_TENSORRT=' scripts .env.example README.md; then
    fail 'legacy dependency variable names remain in active scripts or user docs'
fi
if rg -n "option\('(opencv|tensorrt)'|-D(opencv|tensorrt)=" meson.options meson.build gst scripts README.md .env.example; then
    fail 'legacy Meson dependency options remain'
fi
if rg -n "method *: *'cmake'|find_library|\.deps.*onnxruntime|TensorRT-[0-9].*/lib" meson.build gst/tests/meson.build; then
    fail 'Meson still contains SDK discovery fallbacks or fixed paths'
fi

assert_invalid_runtime_config \
    latest-frame-boolean \
    'display.latest_frame 必须是 true 或 false' \
    $'display:\n  latest_frame: "invalid"'
assert_invalid_runtime_config \
    motion-prediction-boolean \
    'display.motion_prediction.enabled 必须是 true 或 false' \
    $'display:\n  motion_prediction:\n    enabled: "invalid"'
assert_invalid_runtime_config \
    display-fps \
    'display.fps 必须是正整数' \
    $'display:\n  fps: 0'
assert_invalid_runtime_config \
    prediction-horizon \
    'display.motion_prediction.max_horizon_ms 必须是 1..300 的整数' \
    $'display:\n  motion_prediction:\n    max_horizon_ms: 301'
assert_invalid_runtime_config \
    overlay-font-face \
    'display.overlay_font.face 必须是 regular 或 bold' \
    $'display:\n  overlay_font:\n    face: serif'
assert_invalid_runtime_config \
    overlay-font-size \
    'display.overlay_font.size 必须是 7..64 的整数' \
    $'display:\n  overlay_font:\n    size: 65'
assert_invalid_runtime_config \
    empty-source-name \
    'sources.0.name 显式配置时不能为空' \
    $'sources:\n  - name: ""'

compatible_config="$TMP_DIR/compatible.yaml"
printf '%s\n' $'sources: []\ndisplay:\n  latest_frame: true\n  fps: 30\n  motion_prediction:\n    enabled: true\n    max_horizon_ms: 300\ninference:\n  model_path: /bin/true\n  label_map: ""' >"$compatible_config"
if sink_output="$(SSV_CONFIG_PATH="$compatible_config" SSV_RTSP_URL='rtsp://127.0.0.1/test' \
    bash scripts/pipeline.sh --run --display --sink identity --skip-build 2>&1)"; then
    fail "latest-frame mode should reject a sink without sync and max-lateness"
fi
grep -Fq 'display sink identity 不支持 latest-frame 所需属性' <<<"$sink_output" || {
    printf '%s\n' "$sink_output" >&2
    fail "incompatible display sink did not fail with a capability error"
}

mock_build="$TMP_DIR/mock-build"
(
    source "$ROOT/scripts/deps.sh"
    for key in "${SSV_DEPS_ENV_KEYS[@]}"; do
        printf -v "$key" '%s' ''
    done
    ssv_deps_write_env "$mock_build/ssv-deps.env"
)

    mock_bin="$TMP_DIR/bin"
    mkdir -p "$mock_bin"
    printf '%s\n' \
        '#!/bin/bash' \
        ': "${SSV_CAPTURE_PATH:?}"' \
        'printf "%s\n" "$@" >"$SSV_CAPTURE_PATH"' \
        >"$mock_bin/gst-launch-1.0"
    printf '%s\n' \
        '#!/bin/bash' \
        'if [ "${1:-}" = fakesink ]; then' \
        '    printf "  sync : synchronize on the clock\n  max-lateness : lateness in nanoseconds\n"' \
        '    for ((i = 0; i < 8192; i++)); do' \
        '        printf "  trailing-property-%05d : filler\n" "$i"' \
        '    done' \
        '    exit 0' \
        'fi' \
        'exit 1' \
        >"$mock_bin/gst-inspect-1.0"
    printf '%s\n' \
        '#!/bin/bash' \
        'case "${1:-}" in' \
        '    ps) printf "ssv-redis\n" ;;' \
        '    exec) printf "0\n" ;;' \
        '    *) exit 1 ;;' \
        'esac' \
        >"$mock_bin/docker"
    chmod +x "$mock_bin/gst-launch-1.0" "$mock_bin/gst-inspect-1.0" "$mock_bin/docker"

    latest_config="$TMP_DIR/latest.yaml"
    printf '%s\n' $'sources: []\ndisplay:\n  latest_frame: true\n  overlay: false\n  fps: 60\n  sink: fakesink\n  overlay_font:\n    face: bold\n    size: 14\n  motion_prediction:\n    enabled: true\n    max_horizon_ms: 300\ninference:\n  model_path: /bin/true\n  label_map: ""' >"$latest_config"

    no_overlay_capture="$TMP_DIR/no-overlay.args"
    PATH="$mock_bin:$PATH" SSV_BUILD_DIR="$mock_build" \
        SSV_CONFIG_PATH="$latest_config" \
        SSV_RTSP_URL='rtsp://127.0.0.1/test' SSV_CAPTURE_PATH="$no_overlay_capture" \
        bash scripts/pipeline.sh --run --display --skip-build >/dev/null
    grep -Fxq 'max-size-buffers=1' "$no_overlay_capture" || fail "no-overlay latest-frame queue is not capacity one"
    grep -Fxq 'leaky=downstream' "$no_overlay_capture" || fail "no-overlay latest-frame queue is not downstream leaky"
    grep -Fxq 'drop-only=true' "$no_overlay_capture" || fail "no-overlay latest-frame videorate can duplicate frames"
    grep -Fxq 'sync=true' "$no_overlay_capture" || fail "no-overlay latest-frame sink is not synchronized"
    grep -Fxq 'max-lateness=16666666' "$no_overlay_capture" || fail "no-overlay max-lateness is not one frame period"
    grep -Fxq 'async=false' "$no_overlay_capture" || fail "analysis inference is no longer ordered"
    if grep -Fxq 'ssvoverlay' "$no_overlay_capture"; then
        fail "no-overlay display path unexpectedly creates ssvoverlay"
    fi

    overlay_capture="$TMP_DIR/overlay.args"
    PATH="$mock_bin:$PATH" SSV_BUILD_DIR="$mock_build" \
        SSV_CONFIG_PATH="$latest_config" \
        SSV_RTSP_URL='rtsp://127.0.0.1/test' SSV_CAPTURE_PATH="$overlay_capture" \
        bash scripts/pipeline.sh --run --display --overlay --skip-build >/dev/null
    assert_arg_sequence \
        "$overlay_capture" \
        "RTSP decoder output is not clock-paced before the first leaky queue" \
        decodebin '!' clocksync sync=true '!' queue leaky=downstream max-size-buffers=2
    grep -Fxq 'max-rate=60' "$overlay_capture" ||
        fail "latest-frame videorate does not treat display fps as a drop-only upper bound"
    if grep -Fxq 'video/x-raw,framerate=60/1' "$overlay_capture"; then
        fail "latest-frame display requires exact 60/1 caps and cannot accept a 60000/1001 source"
    fi
    grep -Fxq 'ssvoverlay' "$overlay_capture" || fail "overlay display path does not create ssvoverlay"
    grep -Fxq 'motion-prediction=true' "$overlay_capture" || fail "overlay path does not enable configured prediction"
    grep -Fxq 'max-horizon-ms=300' "$overlay_capture" || fail "overlay path does not pass configured prediction horizon"
    grep -Fxq 'font-face=bold' "$overlay_capture" || fail "overlay path does not pass configured font face"
    grep -Fxq 'font-size=14' "$overlay_capture" || fail "overlay path does not pass configured font size"
    [ "$(grep -Fxc 'source-id=pipeline-0' "$overlay_capture")" -eq 4 ] ||
        fail "four perception plugins do not share the default source id"
    for latest_arg in max-size-buffers=1 leaky=downstream drop-only=true sync=true max-lateness=16666666; do
        grep -Fxq "$latest_arg" "$overlay_capture" ||
            fail "overlay latest-frame path is missing $latest_arg"
    done

    rollback_config="$TMP_DIR/rollback.yaml"
    printf '%s\n' $'sources: []\ndisplay:\n  latest_frame: false\n  overlay: false\n  fps: 30\n  sink: fakesink\n  motion_prediction:\n    enabled: false\n    max_horizon_ms: 300\ninference:\n  model_path: /bin/true\n  label_map: ""' >"$rollback_config"
    rollback_capture="$TMP_DIR/rollback.args"
    PATH="$mock_bin:$PATH" SSV_BUILD_DIR="$mock_build" \
        SSV_CONFIG_PATH="$rollback_config" \
        SSV_RTSP_URL='rtsp://127.0.0.1/test' SSV_CAPTURE_PATH="$rollback_capture" \
        bash scripts/pipeline.sh --run --display --skip-build >/dev/null
    grep -Fxq 'max-size-buffers=2' "$rollback_capture" || fail "rollback display queue does not preserve the compatible capacity"
    grep -Fxq 'sync=false' "$rollback_capture" || fail "rollback sink does not preserve the compatible sync behavior"
    if grep -Fxq 'drop-only=true' "$rollback_capture" ||
       grep -Fq 'max-lateness=' "$rollback_capture"; then
        fail "rollback display path still applies latest-frame-only properties"
    fi
