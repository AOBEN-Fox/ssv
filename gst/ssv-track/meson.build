ssv_track_deps = [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep]
ssv_track_cpp_args = ['-DPACKAGE="gstssvtrack"', '-DSSV_HAS_OPENCV=' + (opencv_dep.found() ? '1' : '0')]
if opencv_dep.found()
  ssv_track_deps += opencv_dep
endif

shared_library('gstssvtrack',
  'gstssvtrack.cpp',
  'botsort/botsort_kalman.cpp',
  'botsort/botsort_matching.cpp',
  'botsort/botsort_lapjv.cpp',
  'botsort/botsort_gmc.cpp',
  'botsort/botsort_tracker.cpp',
  dependencies : ssv_track_deps,
  cpp_args : ssv_track_cpp_args,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)
