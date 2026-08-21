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
  src/plugin/plugin-main.cpp
  src/timeline/master-frame-coordinator.cpp
  src/rendering/scene-renderer.cpp
  src/rendering/synchronized-scene-renderer.cpp
)
target_link_libraries(
  obs-sync-replay
  PRIVATE OBS::libobs obs-sync-replay-rendering-coordination obs-sync-replay-pipeline
)
target_compile_features(obs-sync-replay PRIVATE cxx_std_17)
target_compile_definitions(obs-sync-replay PRIVATE OBS_SYNC_REPLAY_VERSION="${PROJECT_VERSION}")
target_include_directories(obs-sync-replay PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

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

add_test(
  NAME plugin-artifact
  COMMAND
    "${CMAKE_COMMAND}" -DPLUGIN_FILE=$<TARGET_FILE:obs-sync-replay> -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPluginArtifact.cmake"
)
