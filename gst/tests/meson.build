gst_test_deps = [gst_deps, gst_base_dep, gst_app_dep, gst_video_dep, gst_check_dep, ssv_common_dep, onnx_dep]
if opencv_enabled
  gst_test_deps += opencv_dep
endif

gst_unit_tests = executable('gst-unit-tests',
  'test_overlay_renderer_contract.cpp',
  'test_gst_plugins.cpp',
  '../ssv-infer/ssv_inference_config.cpp',
  '../ssv-infer/ssv_preprocessor.cpp',
  '../ssv-infer/ssv_yolo_parser.cpp',
  '../ssv-track/botsort/botsort_kalman.cpp',
  '../ssv-track/botsort/botsort_matching.cpp',
  '../ssv-track/botsort/botsort_lapjv.cpp',
  '../ssv-track/botsort/botsort_gmc.cpp',
  '../ssv-track/botsort/botsort_tracker.cpp',
  '../ssv-track/botsort/botsort_processor.cpp',
  include_directories : [
    include_directories('../ssv-infer'),
    include_directories('../ssv-overlay'),
  ],
  dependencies : gst_test_deps,
  cpp_args : ['-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')],
  link_with : [ssv_pub_lib, ssv_overlay_lib],
)

ssv_meta_types_test = executable('ssv-meta-types-test',
  'test_ssv_meta_types.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_inference_timing_test = executable('ssv-inference-timing-test',
  'test_ssv_inference_timing.cpp',
  include_directories : [include_directories('../ssv-infer')],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_infer_lib],
)

ssv_botsort_processor_contract_test = executable('ssv-botsort-processor-contract-test',
  'test_botsort_processor_contract.cpp',
  include_directories : [include_directories('../ssv-track')],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_track_lib],
)

ssv_pub_payload_test = executable('ssv-pub-payload-test',
  'test_ssv_pub_payload.cpp',
  include_directories : [include_directories('../ssv-pub')],
  dependencies : [gst_deps, ssv_common_dep, json_dep],
  link_with : [ssv_pub_lib],
)

ssv_review_candidate_test = executable('ssv-review-candidate-test',
  'test_ssv_review_candidate.cpp',
  '../ssv-pub/ssv_review_candidate.cpp',
  '../ssv-pub/ssv_frame_evidence.cpp',
  include_directories : [include_directories('../ssv-pub')],
  dependencies : [gst_deps, ssv_common_dep, json_dep, jpeg_dep],
)

ssv_frame_evidence_test = executable('ssv-frame-evidence-test',
  'test_ssv_frame_evidence.cpp',
  '../ssv-pub/ssv_frame_evidence.cpp',
  include_directories : [include_directories('../ssv-pub')],
  dependencies : [gst_deps, ssv_common_dep, jpeg_dep],
)

ssv_overlay_motion_predictor_test = executable('ssv-overlay-motion-predictor-test',
  'test_overlay_motion_predictor.cpp',
  '../ssv-overlay/overlay_motion_predictor.cpp',
  include_directories : [include_directories('../ssv-overlay')],
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_overlay_runtime_test = executable('ssv-overlay-runtime-test',
  'test_overlay_runtime.cpp',
  '../ssv-overlay/overlay_motion_predictor.cpp',
  '../ssv-overlay/overlay_renderer.cpp',
  '../ssv-overlay/overlay_runtime.cpp',
  include_directories : [include_directories('../ssv-overlay')],
  dependencies : [gst_deps, gst_video_dep, ssv_common_dep],
)

ssv_meta_timeline_test = executable('ssv-meta-timeline-test',
  'test_ssv_meta_timeline.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_meta_test = executable('ssv-meta-test',
  'test_ssv_meta.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_config_test = executable('ssv-config-test',
  'test_ssv_config.cpp',
  dependencies : [ssv_common_dep],
)

test('ssv-meta-types', ssv_meta_types_test)
test('ssv-inference-timing', ssv_inference_timing_test)
test('ssv-botsort-processor-contract', ssv_botsort_processor_contract_test)
test('ssv-pub-payload', ssv_pub_payload_test)
test('ssv-review-candidate', ssv_review_candidate_test)
test('ssv-frame-evidence', ssv_frame_evidence_test)
test('ssv-overlay-motion-predictor', ssv_overlay_motion_predictor_test)
test('ssv-overlay-runtime', ssv_overlay_runtime_test)
test('ssv-meta-timeline', ssv_meta_timeline_test)
test('ssv-meta', ssv_meta_test)
test('ssv-config', ssv_config_test)

test('gst-unit-tests', gst_unit_tests,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-test-registry.bin',
    'GST_PLUGIN_PATH' : meson.project_build_root() / 'gst' / 'ssv-template'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-infer'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-track'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-pub'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-overlay',
    'LD_LIBRARY_PATH' : (deps_runtime_path == '' ? '' : deps_runtime_path + ':')
      + meson.project_build_root() / 'gst' / 'ssv-common',
  },
)
