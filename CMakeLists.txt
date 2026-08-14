cmake_minimum_required(VERSION 3.28...3.31)

project(obs-sync-replay VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "OBS Sync Replay currently supports Windows only")
endif()

option(OBS_SYNC_REPLAY_WARNINGS_AS_ERRORS "Treat plugin compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ObsSdk)

add_library(obs-sync-replay MODULE src/plugin-main.cpp)
target_link_libraries(obs-sync-replay PRIVATE OBS::libobs)
target_compile_features(obs-sync-replay PRIVATE cxx_std_17)
target_compile_definitions(obs-sync-replay PRIVATE OBS_SYNC_REPLAY_VERSION="${PROJECT_VERSION}")

if(MSVC)
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
add_test(
  NAME plugin-artifact
  COMMAND
    "${CMAKE_COMMAND}" -DPLUGIN_FILE=$<TARGET_FILE:obs-sync-replay> -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPluginArtifact.cmake"
)
