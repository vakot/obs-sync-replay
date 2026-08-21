cmake_minimum_required(VERSION 3.28...3.31)

project(obs-sync-replay VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "OBS Sync Replay currently supports Windows only")
endif()

option(OBS_SYNC_REPLAY_WARNINGS_AS_ERRORS "Treat plugin compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ObsSdk)

add_library(obs-sync-replay-timeline STATIC src/timeline/master-frame-timeline.cpp)
target_compile_features(obs-sync-replay-timeline PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-timeline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(obs-sync-replay-rendering-coordination STATIC src/rendering/scene-render-pair-tracker.cpp)
target_link_libraries(obs-sync-replay-rendering-coordination PUBLIC obs-sync-replay-timeline)
target_compile_features(obs-sync-replay-rendering-coordination PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-rendering-coordination PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(
  obs-sync-replay-recording
  STATIC
  src/recording/encoded-packet-buffer.cpp
  src/recording/synchronized-recording-session.cpp
  src/sync/common-packet-range.cpp
)
target_compile_features(obs-sync-replay-recording PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-recording PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(obs-sync-replay-muxing STATIC src/muxing/mkv-packet-writer.cpp)
target_link_libraries(obs-sync-replay-muxing PUBLIC obs-sync-replay-recording)
target_compile_features(obs-sync-replay-muxing PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-muxing PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(obs-sync-replay-muxing PRIVATE "${_obs_deps_dir}/include")
target_link_libraries(
  obs-sync-replay-muxing
  PUBLIC
    "${_obs_deps_dir}/lib/avcodec.lib"
    "${_obs_deps_dir}/lib/avformat.lib"
    "${_obs_deps_dir}/lib/avutil.lib"
)

add_library(obs-sync-replay-capture STATIC src/capture/synchronized-capture-session.cpp)
target_link_libraries(obs-sync-replay-capture PUBLIC obs-sync-replay-recording)
target_compile_features(obs-sync-replay-capture PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(
  obs-sync-replay-replay
  STATIC
  src/recording/synchronized-recording-consumer.cpp
  src/replay/synchronized-replay-consumer.cpp
)
target_link_libraries(obs-sync-replay-replay PUBLIC obs-sync-replay-capture obs-sync-replay-muxing)
target_compile_features(obs-sync-replay-replay PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-replay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(obs-sync-replay-replay PRIVATE "${_obs_deps_dir}/include")

add_library(
  obs-sync-replay-pipeline
  STATIC
  src/pipeline/synchronized-frame-queue.cpp
  src/pipeline/synchronized-frame-pipeline.cpp
)
target_link_libraries(obs-sync-replay-pipeline PUBLIC obs-sync-replay-timeline PRIVATE OBS::libobs)
target_compile_features(obs-sync-replay-pipeline PUBLIC cxx_std_17)
target_include_directories(obs-sync-replay-pipeline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(
  obs-sync-replay
  MODULE
  src/bootstrap/deterministic-test-environment.cpp
  src/experiment/stock-encoder-timeline-probe.cpp
  src/experiment/three-stream-capture-poc.cpp
  src/plugin/plugin-main.cpp
  src/recording/obs-synchronized-recording.cpp
  src/timeline/master-frame-coordinator.cpp
  src/rendering/scene-renderer.cpp
  src/rendering/synchronized-scene-renderer.cpp
)
target_link_libraries(
  obs-sync-replay
  PRIVATE OBS::libobs OBS::obs-frontend-api obs-sync-replay-rendering-coordination obs-sync-replay-pipeline
          obs-sync-replay-recording obs-sync-replay-muxing obs-sync-replay-capture obs-sync-replay-replay
)
target_compile_features(obs-sync-replay PRIVATE cxx_std_17)
target_compile_definitions(obs-sync-replay PRIVATE OBS_SYNC_REPLAY_VERSION="${PROJECT_VERSION}")
target_include_directories(obs-sync-replay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(obs-sync-replay PRIVATE "${_obs_deps_dir}/include")
target_link_libraries(
  obs-sync-replay
  PRIVATE
    "${_obs_deps_dir}/lib/avcodec.lib"
    "${_obs_deps_dir}/lib/avformat.lib"
    "${_obs_deps_dir}/lib/avutil.lib"
    "${_obs_deps_dir}/lib/swresample.lib"
    "${_obs_deps_dir}/lib/swscale.lib"
)

if(MSVC)
  target_compile_options(obs-sync-replay-timeline PRIVATE /W4 /permissive- /Zc:__cplusplus)
  target_compile_options(obs-sync-replay-pipeline PRIVATE /W4 /permissive- /Zc:__cplusplus)
  target_compile_options(
    obs-sync-replay
    PRIVATE /W4 /permissive- /Zc:__cplusplus
            $<$<BOOL:${OBS_SYNC_REPLAY_WARNINGS_AS_ERRORS}>:/WX>
  )
endif()

set_target_properties(obs-sync-replay PROPERTIES PREFIX "" OUTPUT_NAME "obs-sync-replay")

install(
  TARGETS obs-sync-replay
  RUNTIME DESTINATION obs-sync-replay/bin/64bit
  LIBRARY DESTINATION obs-sync-replay/bin/64bit
)
install(DIRECTORY data/ DESTINATION obs-sync-replay/data)

enable_testing()
add_executable(
  master-frame-timeline-test
  tests/timeline/master-frame-timeline-test.cpp
)
target_link_libraries(master-frame-timeline-test PRIVATE obs-sync-replay-timeline)
target_compile_features(master-frame-timeline-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(master-frame-timeline-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME master-frame-timeline COMMAND master-frame-timeline-test)

add_executable(
  synchronized-scene-renderer-test
  tests/rendering/synchronized-scene-renderer-test.cpp
)
target_link_libraries(synchronized-scene-renderer-test PRIVATE obs-sync-replay-rendering-coordination)
target_compile_features(synchronized-scene-renderer-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(synchronized-scene-renderer-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME synchronized-scene-renderer COMMAND synchronized-scene-renderer-test)

add_executable(
  synchronized-frame-queue-test
  tests/pipeline/synchronized-frame-queue-test.cpp
)
target_link_libraries(synchronized-frame-queue-test PRIVATE obs-sync-replay-pipeline)
target_compile_features(synchronized-frame-queue-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(synchronized-frame-queue-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME synchronized-frame-queue COMMAND synchronized-frame-queue-test)

add_executable(synchronized-recording-session-test tests/recording/synchronized-recording-session-test.cpp)
target_link_libraries(synchronized-recording-session-test PRIVATE obs-sync-replay-recording)
target_compile_features(synchronized-recording-session-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(synchronized-recording-session-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME synchronized-recording-session COMMAND synchronized-recording-session-test)

add_executable(synchronized-capture-session-test tests/capture/synchronized-capture-session-test.cpp)
target_link_libraries(synchronized-capture-session-test PRIVATE obs-sync-replay-capture)
target_compile_features(synchronized-capture-session-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(synchronized-capture-session-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME synchronized-capture-session COMMAND synchronized-capture-session-test)

add_executable(synchronized-replay-consumer-test tests/replay/synchronized-replay-consumer-test.cpp)
target_link_libraries(synchronized-replay-consumer-test PRIVATE obs-sync-replay-replay)
target_compile_features(synchronized-replay-consumer-test PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(synchronized-replay-consumer-test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
add_test(NAME synchronized-replay-consumer COMMAND synchronized-replay-consumer-test)
set_tests_properties(synchronized-replay-consumer PROPERTIES ENVIRONMENT "PATH=${_obs_deps_dir}/bin")

add_test(
  NAME plugin-artifact
  COMMAND
    "${CMAKE_COMMAND}" -DPLUGIN_FILE=$<TARGET_FILE:obs-sync-replay> -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPluginArtifact.cmake"
)
