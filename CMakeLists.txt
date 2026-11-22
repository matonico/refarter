cmake_minimum_required(VERSION 3.24)
project(Refarter VERSION 1.3.0 LANGUAGES CXX)

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
# The running version's release notes, embedded for the "what's new" page. A version without notes yet (between
# releases, or the public source, which has no docs/) embeds an empty file and the page says so.
set(REFARTER_NOTES ${CMAKE_CURRENT_SOURCE_DIR}/docs/releases/${REFARTER_VERSION_FULL}.md)
# Watch the set of notes files, including an initially absent folder. A directory itself is not a valid MSBuild
# file dependency (MSB8064), whereas CONFIGURE_DEPENDS detects additions/removals through CMake's glob check.
file(GLOB REFARTER_NOTES_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/releases/*.md")
set(REFARTER_NOTES_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/release-notes.md)
# What is embedded is made safe to ship first: the popup shows a link's text only, so link targets and bare
# addresses are removed here, not just left undisplayed (the notes of a private release link the private
# repository). The release_notes_resource test checks the built exe for every address in the notes.
set(notes "")
if(EXISTS ${REFARTER_NOTES})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${REFARTER_NOTES})
  file(READ ${REFARTER_NOTES} notes)
  # Inline links (a target may hold one pair of parentheses), reference definitions, then reference links.
  string(REGEX REPLACE "\\[([^]\n]*)\\]\\(([^()\n]|\\([^()\n]*\\))*\\)" "\\1" notes "${notes}")
  string(REGEX REPLACE "(^|\n)[ \t]*\\[[^]\n]*\\]:[^\n]*" "\\1" notes "${notes}")
  string(REGEX REPLACE "\\[([^]\n]*)\\]\\[[^]\n]*\\]" "\\1" notes "${notes}")
  string(REGEX REPLACE "<?[A-Za-z][A-Za-z0-9+.-]*://[^ \t\r\n<>)]*>?" "" notes "${notes}")
  string(REGEX REPLACE "www\\.[^ \t\r\n<>)]*" "" notes "${notes}")
  # A list item that was only an address would be an empty bullet.
  # A pass skips every other one of several in a row, so until none is left.
  while(notes MATCHES "\n[ \t]*[-*+][ \t]*\r?(\n|$)")
    string(REGEX REPLACE "\n[ \t]*[-*+][ \t]*\r?(\n|$)" "\\1" notes "${notes}")
  endwhile()
  if(notes MATCHES "://|www\\.")
    message(FATAL_ERROR "${REFARTER_NOTES} still holds an address after removing links; it must not be embedded")
  endif()
endif()
# Written only when it changed, so configuring again does not rebuild the resources.
file(WRITE ${REFARTER_NOTES_RESOURCE}.new "${notes}")
configure_file(${REFARTER_NOTES_RESOURCE}.new ${REFARTER_NOTES_RESOURCE} COPYONLY)
file(REMOVE ${REFARTER_NOTES_RESOURCE}.new)

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
  src/capture/downscaler.cpp
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
# The downscaler's shaders, compiled by fxc into byte arrays in generated/shaders, so nothing compiles at run time.
# Visual Studio runs fxc through its shader properties, which other generators (Ninja, NMake) ignore; those run it
# as a custom command, finding fxc on PATH (a developer prompt) or in the Windows SDK.
set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  cmake_host_system_information(RESULT kits_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
  file(GLOB fxc_dirs LIST_DIRECTORIES true "${kits_root}bin/10.*/x64")
  list(SORT fxc_dirs COMPARE NATURAL ORDER DESCENDING) # The newest SDK first.
  find_program(REFARTER_FXC fxc REQUIRED HINTS "$ENV{WindowsSdkVerBinPath}x64" ${fxc_dirs})
endif()
foreach(shader scale_vs:Vertex:vs:scaleVertexShader scale_ps:Pixel:ps:scalePixelShader)
  string(REPLACE ":" ";" shader "${shader}")
  list(GET shader 0 name)
  list(GET shader 1 type)
  list(GET shader 2 profile)
  list(GET shader 3 variable)
  set(source ${CMAKE_CURRENT_SOURCE_DIR}/src/capture/shaders/${name}.hlsl)
  set(header ${shader_dir}/${name}.h)
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_sources(refarter_capture PRIVATE ${source})
    set_source_files_properties(${source} PROPERTIES VS_SHADER_TYPE ${type} VS_SHADER_MODEL 4.0 VS_SHADER_ENTRYPOINT main
      VS_SHADER_OUTPUT_HEADER_FILE ${header} VS_SHADER_VARIABLE_NAME ${variable} VS_SHADER_FLAGS "/O3")
  else()
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
      COMMAND ${REFARTER_FXC} /nologo /T ${profile}_4_0 /E main /O3 /Vn ${variable} /Fh ${header} ${source}
      DEPENDS ${source} VERBATIM)
    target_sources(refarter_capture PRIVATE ${header})
  endif()
endforeach()
target_include_directories(refarter_capture PRIVATE ${shader_dir})
target_link_libraries(refarter_capture PUBLIC refarter_diagnostics d3d11 dxgi dwmapi mfplat mfuuid mfreadwrite wmcodecdspuuid windowsapp shell32 ole32 avrt mmdevapi windowscodecs PRIVATE rnnoise)

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
  src/player/clip_preview.cpp
  src/player/clip_trim.cpp
  src/player/clip_combine.cpp
  src/player/clip_export.cpp
  src/player/media.cpp
  src/player/player_view.cpp
  src/player/renderer.cpp
  src/recycle_bin.cpp)
refarter_defaults(refarter_player)
target_link_libraries(refarter_player PUBLIC refarter_capture ffmpeg d2d1 dwrite dcomp)
add_executable(clip_combine_check tools/clip_combine_check.cpp)
refarter_defaults(clip_combine_check)
target_link_libraries(clip_combine_check PRIVATE refarter_player psapi)

enable_language(RC)
# The app without its entry point and resources, compiled once: Refarter links these objects, and so does the test
# that drives the real App against hidden controls (app_game_settings_test).
add_library(refarter_app OBJECT src/app.cpp src/app_layout.cpp src/app_paint.cpp src/app_controls.cpp src/app_menus.cpp src/app_settings.cpp src/app_storage.cpp src/app_library.cpp src/app_meters.cpp src/app_updates.cpp src/notes_view.cpp src/update_client.cpp src/update_download.cpp src/https_get.cpp src/auto_session.cpp src/chime.cpp src/tray_icon.cpp src/compression_job.cpp src/recording_controller.cpp
  src/clip_grid.cpp src/clip_library.cpp src/clip_sources.cpp src/game_icons.cpp src/favourites.cpp src/config.cpp src/startup.cpp src/jump_list.cpp src/auto_capture.cpp src/auto_capture_card.cpp src/game_settings.cpp src/game_names.cpp src/tray_flyout.cpp src/value_bubble.cpp
  src/popup_menu.cpp src/message_box.cpp src/window_target.cpp src/app_lifecycle.cpp src/leftover_files.cpp src/crash_dumps.cpp
  src/combine_dialog.cpp src/combine_job.cpp)
refarter_defaults(refarter_app)
# assets: icon, save chime and failed-save sound; config: the refarter.ini template embedded for first launch;
# generated: version.h and the release notes.
target_include_directories(refarter_app PUBLIC src assets config ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(refarter_app PUBLIC gdiplus dwmapi comctl32 ole32 msimg32 propsys winmm uxtheme windowscodecs advapi32 winhttp bcrypt refarter_capture refarter_player)
add_executable(Refarter WIN32 src/main.cpp src/resources.rc)
refarter_defaults(Refarter)
target_include_directories(Refarter PRIVATE assets config ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(Refarter PRIVATE refarter_app)
# The shipped exe names its PDB without the build machine's path.
if(MSVC)
  target_link_options(Refarter PRIVATE $<$<CONFIG:Release>:/PDBALTPATH:$<TARGET_PDB_FILE_NAME:Refarter>>)
endif()

add_executable(refarter-record tools/refarter_record.cpp src/clip_library.cpp src/recycle_bin.cpp src/clip_sources.cpp src/game_icons.cpp src/favourites.cpp)
refarter_defaults(refarter-record)
target_link_libraries(refarter-record PRIVATE refarter_capture pdh psapi windowscodecs winmm)

include(CTest)
if(BUILD_TESTING)
  foreach(test replay_duration capture_logic clip_list clip_selection hotkey config settings chime timing_trace source_names startup favourites auto_capture game_settings game_names auto_session auto_session_wiring accent_colour clip_preview player_timeline player_zoom export_plan power_resume leftover_files save_errors startup_retry diagnostic_log compression_diagnostics trim_follow_up storage_limit audio_device_state player_view value_bubble volume_input temporary_clip trim_cancel long_path update_check update_download release_notes mp4_tracks track_workers meter_rules clip_writer codec_choice processor_step)
    add_executable(${test}_test tests/${test}_test.cpp)
    refarter_defaults(${test}_test)
    target_include_directories(${test}_test PRIVATE src)
    add_test(NAME ${test} COMMAND ${test}_test)
  endforeach()
  target_link_libraries(processor_step_test PRIVATE windowsapp)
  target_sources(leftover_files_test PRIVATE src/leftover_files.cpp)
  target_sources(config_test PRIVATE src/config.cpp) # Config::loadFrom/set on temporary files.
  # The real downloader with httpsGet scripted by the test itself, so every answer and failure path runs offline.
  target_sources(update_download_test PRIVATE src/update_download.cpp)
  target_link_libraries(update_download_test PRIVATE bcrypt user32)
  add_executable(progress_dialog_test tests/progress_dialog_test.cpp)
  refarter_defaults(progress_dialog_test)
  target_include_directories(progress_dialog_test PRIVATE src)
  target_link_libraries(progress_dialog_test PRIVATE user32 gdi32 comctl32 gdiplus dwmapi ole32 oleacc)
  add_test(NAME progress_dialog COMMAND progress_dialog_test)
  set_tests_properties(progress_dialog PROPERTIES TIMEOUT 15)
  # The real App, its window and controls created hidden, with its files in a temporary folder and a stand-in recorder:
  # the per-game settings page, start and quit paths (see the test's header).
  add_executable(app_game_settings_test tests/app_game_settings_test.cpp)
  refarter_defaults(app_game_settings_test)
  target_link_libraries(app_game_settings_test PRIVATE refarter_app)
  add_test(NAME app_game_settings COMMAND app_game_settings_test)
  set_tests_properties(app_game_settings PROPERTIES TIMEOUT 60)
  # The same, for sign-in replay: launches, sources, the single retry and what cancels it.
  add_executable(app_sign_in_replay_test tests/app_sign_in_replay_test.cpp)
  refarter_defaults(app_sign_in_replay_test)
  target_link_libraries(app_sign_in_replay_test PRIVATE refarter_app)
  add_test(NAME app_sign_in_replay COMMAND app_sign_in_replay_test)
  set_tests_properties(app_sign_in_replay PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77) # 77: no display monitor.
  target_compile_definitions(chime_test PRIVATE REFARTER_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
  target_compile_definitions(settings_test PRIVATE REFARTER_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
  # Crashes copies of itself: each must leave one minidump.
  add_executable(crash_dumps_test tests/crash_dumps_test.cpp src/crash_dumps.cpp)
  refarter_defaults(crash_dumps_test)
  target_include_directories(crash_dumps_test PRIVATE src)
  add_test(NAME crash_dumps COMMAND crash_dumps_test)
  # The built exe, not just the popup's output: no address from the release notes may be embedded.
  add_test(NAME release_notes_resource COMMAND ${CMAKE_COMMAND} -DNOTES=${REFARTER_NOTES} -DRESOURCE=${REFARTER_NOTES_RESOURCE}
    -DEXE=$<TARGET_FILE:Refarter> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_notes_resource.cmake)
  target_link_libraries(diagnostic_log_test PRIVATE refarter_diagnostics)
  target_link_libraries(compression_diagnostics_test PRIVATE refarter_player)
  target_sources(compression_diagnostics_test PRIVATE src/compression_job.cpp)
  add_executable(clip_combine_test tests/clip_combine_test.cpp src/compression_job.cpp src/combine_job.cpp)
  refarter_defaults(clip_combine_test)
  target_include_directories(clip_combine_test PRIVATE src)
  target_compile_definitions(clip_combine_test PRIVATE REFARTER_COMBINE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/combine")
  target_link_libraries(clip_combine_test PRIVATE refarter_player)
  add_test(NAME clip_combine COMMAND clip_combine_test)
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
  # Media Foundation's software H.264 encoder and MP4 sink; fails rather than hangs when a save stalls.
  target_link_libraries(clip_writer_test PRIVATE refarter_capture)
  set_tests_properties(clip_writer PROPERTIES TIMEOUT 120)
  target_link_libraries(clip_selection_test PRIVATE user32 shell32 ole32)
  target_sources(startup_test PRIVATE src/startup.cpp)
  target_link_libraries(startup_test PRIVATE advapi32 shell32)
  target_sources(favourites_test PRIVATE src/favourites.cpp src/clip_sources.cpp src/game_icons.cpp)
  target_link_libraries(favourites_test PRIVATE shell32 ole32 windowscodecs windowsapp)
  target_sources(auto_capture_test PRIVATE src/auto_capture.cpp)
  target_link_libraries(auto_capture_test PRIVATE shell32 ole32)
  target_sources(game_settings_test PRIVATE src/game_settings.cpp)
  target_link_libraries(game_settings_test PRIVATE shell32 ole32)
  target_sources(game_names_test PRIVATE src/game_names.cpp)
  target_link_libraries(game_names_test PRIVATE shell32 ole32)
  target_sources(auto_session_test PRIVATE src/auto_capture.cpp)
  target_link_libraries(auto_session_test PRIVATE shell32 ole32)
  # The real AutoSession against a stand-in App: starts, stops, sleep and process exit.
  target_sources(auto_session_wiring_test PRIVATE src/auto_session.cpp src/auto_capture.cpp src/auto_capture_card.cpp src/window_target.cpp)
  target_link_libraries(auto_session_wiring_test PRIVATE refarter_diagnostics shell32 ole32 dwmapi gdiplus)
  # Manual opt-in: requires a desktop and hardware encoder, unlike CTest units.
  add_executable(recorder_lifecycle_check tests/recorder_lifecycle_check.cpp)
  refarter_defaults(recorder_lifecycle_check)
  target_link_libraries(recorder_lifecycle_check PRIVATE refarter_capture)
  # Manual opt-in: decodes, seeks and trims a real clip on this PC's GPU.
  add_executable(player_media_check tests/player_media_check.cpp)
  refarter_defaults(player_media_check)
  target_link_libraries(player_media_check PRIVATE refarter_player)
  # Manual opt-in: times one hover preview (12 keyframes at tile size) on a real clip.
  add_executable(clip_preview_check tests/clip_preview_check.cpp)
  refarter_defaults(clip_preview_check)
  target_link_libraries(clip_preview_check PRIVATE refarter_player psapi)
  # Manual opt-in: creates disposable copies; needs a hardware H.264 encoder.
  add_executable(clip_export_check tests/clip_export_check.cpp)
  refarter_defaults(clip_export_check)
  target_link_libraries(clip_export_check PRIVATE refarter_player psapi)
  # Manual opt-in: Shell metadata and Recycle Bin, using disposable scratch copies only.
  add_executable(clip_library_check tests/clip_library_check.cpp src/clip_library.cpp src/recycle_bin.cpp src/clip_sources.cpp src/game_icons.cpp src/favourites.cpp)
  refarter_defaults(clip_library_check)
  target_link_libraries(clip_library_check PRIVATE refarter_capture windowscodecs)
  # Manual opt-in: starts a built Refarter.exe, waits for its window, quits it and looks for crash dumps.
  add_executable(launch_check tests/launch_check.cpp)
  refarter_defaults(launch_check)
endif()
