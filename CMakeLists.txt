cmake_minimum_required(VERSION 3.24)
project(Refarter VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Refarter currently targets Windows only.")
endif()

# Pre-release suffix shown beside the version (e.g. "beta" for 1.1.0-beta); empty, the default since 1.0, for a plain release.
set(REFARTER_VERSION_SUFFIX "" CACHE STRING "Pre-release suffix shown beside the version")
set(REFARTER_VERSION_FULL "${PROJECT_VERSION}")
if(REFARTER_VERSION_SUFFIX)
  string(APPEND REFARTER_VERSION_FULL "-${REFARTER_VERSION_SUFFIX}")
endif()
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Release builds also write a PDB beside each binary (never shipped) so crash dumps can be read.
# /DEBUG would switch off these link optimisations, so they are named again.
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

function(refarter_defaults target)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  endif()
endfunction()

# RNNoise (BSD-3, Xiph.Org) for microphone noise suppression: a pinned
# release including its trained model, downloaded once at configure time.
enable_language(C)
include(FetchContent)
FetchContent_Declare(rnnoise
  URL https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
  URL_HASH SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR no-cmake-build) # Autotools project: populate only, targets below.
FetchContent_MakeAvailable(rnnoise)
set(RNNOISE_SOURCES denoise rnn pitch kiss_fft celt_lpc nnet nnet_default parse_lpcnet_weights rnnoise_data rnnoise_tables)
list(TRANSFORM RNNOISE_SOURCES PREPEND ${rnnoise_SOURCE_DIR}/src/)
list(TRANSFORM RNNOISE_SOURCES APPEND .c)
add_library(rnnoise STATIC ${RNNOISE_SOURCES})
target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include PRIVATE ${rnnoise_SOURCE_DIR}/src)
# Every x64 CPU has SSE2; MSVC just does not announce it. Selects RNNoise's
# vectorised path (the plain-C fallback needs a header the release omits).
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS __SSE2__)
if(MSVC)
  target_compile_options(rnnoise PRIVATE /W0)
endif()

# Capture pipeline: Windows Graphics Capture, D3D11 video processing,
# Media Foundation hardware encoding. Windows libraries plus RNNoise.
add_library(refarter_diagnostics STATIC src/diagnostic_log.cpp)
refarter_defaults(refarter_diagnostics)
target_include_directories(refarter_diagnostics PUBLIC src)

add_library(refarter_capture STATIC
  src/capture/audio_capture.cpp
  src/capture/clip_folder.cpp
  src/capture/clip_writer.cpp
  src/capture/codec_probe.cpp
  src/capture/discord_audio.cpp
  src/capture/frame_converter.cpp
  src/capture/gpu.cpp
  src/capture/graphics_capture.cpp
  src/capture/mic_filters.cpp
  src/capture/recorder.cpp
  src/capture/screenshot.cpp
  src/capture/surface_pool.cpp
  src/capture/video_encoder.cpp)
refarter_defaults(refarter_capture)
target_include_directories(refarter_capture PUBLIC src)
target_link_libraries(refarter_capture PUBLIC refarter_diagnostics d3d11 dxgi dwmapi mfplat mfuuid mfreadwrite windowsapp shell32 ole32 avrt mmdevapi windowscodecs PRIVATE rnnoise)

# In-app player and trimming: a minimal static FFmpeg (MP4, H.264/HEVC/AV1/AAC,
# D3D11VA) and dav1d, built once by scripts/build-ffmpeg.ps1 into .deps/ffmpeg.
set(REFARTER_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ffmpeg" CACHE PATH "Static FFmpeg from scripts/build-ffmpeg.ps1")
if(NOT EXISTS "${REFARTER_FFMPEG_DIR}/lib/avcodec.lib")
  message(FATAL_ERROR "FFmpeg is missing: run scripts/build-ffmpeg.ps1 first (scripts/build.ps1 does this).")
endif()
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg SYSTEM INTERFACE "${REFARTER_FFMPEG_DIR}/include")
foreach(lib avformat avcodec swscale swresample avutil dav1d)
  target_link_libraries(ffmpeg INTERFACE "${REFARTER_FFMPEG_DIR}/lib/${lib}.lib")
endforeach()
target_link_libraries(ffmpeg INTERFACE bcrypt ole32 user32 mfplat mfuuid mfreadwrite strmiids)

add_library(refarter_player STATIC
  src/player/audio_output.cpp
  src/player/clip_player.cpp
  src/player/clip_trim.cpp
  src/player/clip_export.cpp
  src/player/media.cpp
  src/player/player_view.cpp
  src/player/renderer.cpp
  src/recycle_bin.cpp)
refarter_defaults(refarter_player)
target_link_libraries(refarter_player PUBLIC refarter_capture ffmpeg d2d1 dwrite dcomp)

enable_language(RC)
add_executable(Refarter WIN32 src/main.cpp src/app.cpp src/app_layout.cpp src/app_paint.cpp src/app_controls.cpp src/app_menus.cpp src/app_settings.cpp src/app_storage.cpp src/app_library.cpp src/app_meters.cpp src/app_updates.cpp src/update_client.cpp src/auto_session.cpp src/chime.cpp src/tray_icon.cpp src/compression_job.cpp src/recording_controller.cpp
  src/clip_grid.cpp src/clip_library.cpp src/clip_sources.cpp src/favourites.cpp src/config.cpp src/startup.cpp src/auto_capture.cpp src/auto_capture_card.cpp src/tray_flyout.cpp src/value_bubble.cpp
  src/popup_menu.cpp src/message_box.cpp src/window_target.cpp src/app_lifecycle.cpp src/leftover_files.cpp src/crash_dumps.cpp src/resources.rc)
refarter_defaults(Refarter)
# assets: icon, save chime and failed-save sound; config: the refarter.ini template embedded for first launch.
target_include_directories(Refarter PRIVATE assets config ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(Refarter PRIVATE gdiplus dwmapi comctl32 ole32 winmm uxtheme windowscodecs advapi32 winhttp refarter_capture refarter_player)
# The shipped exe names its PDB without the build machine's path.
if(MSVC)
  target_link_options(Refarter PRIVATE $<$<CONFIG:Release>:/PDBALTPATH:$<TARGET_PDB_FILE_NAME:Refarter>>)
endif()

add_executable(refarter-record tools/refarter_record.cpp src/clip_library.cpp src/recycle_bin.cpp src/clip_sources.cpp src/favourites.cpp)
refarter_defaults(refarter-record)
target_link_libraries(refarter-record PRIVATE refarter_capture pdh psapi windowscodecs winmm)

include(CTest)
if(BUILD_TESTING)
  foreach(test replay_duration capture_logic clip_list clip_selection hotkey config settings chime timing_trace source_names startup favourites auto_capture auto_session player_timeline export_plan power_resume leftover_files save_errors startup_retry diagnostic_log compression_diagnostics trim_follow_up storage_limit audio_device_state player_view value_bubble volume_input temporary_clip trim_cancel long_path update_check)
    add_executable(${test}_test tests/${test}_test.cpp)
    refarter_defaults(${test}_test)
    target_include_directories(${test}_test PRIVATE src)
    add_test(NAME ${test} COMMAND ${test}_test)
  endforeach()
  target_sources(leftover_files_test PRIVATE src/leftover_files.cpp)
  target_sources(config_test PRIVATE src/config.cpp) # Config::loadFrom/set on temporary files.
  target_compile_definitions(chime_test PRIVATE REFARTER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
  target_compile_definitions(settings_test PRIVATE REFARTER_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
  # Crashes copies of itself: each must leave one minidump.
  add_executable(crash_dumps_test tests/crash_dumps_test.cpp src/crash_dumps.cpp)
  refarter_defaults(crash_dumps_test)
  target_include_directories(crash_dumps_test PRIVATE src)
  add_test(NAME crash_dumps COMMAND crash_dumps_test)
  target_link_libraries(diagnostic_log_test PRIVATE refarter_diagnostics)
  target_link_libraries(compression_diagnostics_test PRIVATE refarter_player)
  target_sources(compression_diagnostics_test PRIVATE src/compression_job.cpp)
  target_link_libraries(player_view_test PRIVATE refarter_player)
  target_link_libraries(trim_cancel_test PRIVATE refarter_player gdiplus)
  # Exhaust the encoder list without activating an encoder or submitting frames.
  add_executable(mf_encoder_cleanup_test tests/mf_encoder_cleanup_test.cpp)
  refarter_defaults(mf_encoder_cleanup_test)
  target_link_libraries(mf_encoder_cleanup_test PRIVATE ffmpeg)
  add_test(NAME mf_encoder_cleanup COMMAND mf_encoder_cleanup_test)
  set_tests_properties(mf_encoder_cleanup PROPERTIES SKIP_RETURN_CODE 77)
  target_sources(save_errors_test PRIVATE tests/clip_folder_preflight_cases.cpp)
  target_link_libraries(save_errors_test PRIVATE shell32 ole32)
  target_link_libraries(capture_logic_test PRIVATE refarter_capture)
  target_link_libraries(clip_selection_test PRIVATE user32 shell32 ole32)
  target_sources(startup_test PRIVATE src/startup.cpp)
  target_link_libraries(startup_test PRIVATE advapi32 shell32)
  target_sources(favourites_test PRIVATE src/favourites.cpp src/clip_sources.cpp)
  target_link_libraries(favourites_test PRIVATE shell32 ole32)
  target_sources(auto_capture_test PRIVATE src/auto_capture.cpp)
  target_link_libraries(auto_capture_test PRIVATE shell32 ole32)
  target_sources(auto_session_test PRIVATE src/auto_capture.cpp)
  target_link_libraries(auto_session_test PRIVATE shell32 ole32)
  # Manual opt-in: requires a desktop and hardware encoder, unlike CTest units.
  add_executable(recorder_lifecycle_check tests/recorder_lifecycle_check.cpp)
  refarter_defaults(recorder_lifecycle_check)
  target_link_libraries(recorder_lifecycle_check PRIVATE refarter_capture)
  # Manual opt-in: decodes, seeks and trims a real clip on this PC's GPU.
  add_executable(player_media_check tests/player_media_check.cpp)
  refarter_defaults(player_media_check)
  target_link_libraries(player_media_check PRIVATE refarter_player)
  # Manual opt-in: creates disposable copies; needs a hardware H.264 encoder.
  add_executable(clip_export_check tests/clip_export_check.cpp)
  refarter_defaults(clip_export_check)
  target_link_libraries(clip_export_check PRIVATE refarter_player psapi)
  # Manual opt-in: Shell metadata and Recycle Bin, using disposable scratch copies only.
  add_executable(clip_library_check tests/clip_library_check.cpp src/clip_library.cpp src/recycle_bin.cpp src/clip_sources.cpp src/favourites.cpp)
  refarter_defaults(clip_library_check)
  target_link_libraries(clip_library_check PRIVATE refarter_capture windowscodecs)
endif()
