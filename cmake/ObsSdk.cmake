include_guard(GLOBAL)

set(OBS_SDK_VERSION "32.2.1")
set(OBS_DEPS_VERSION "2026-07-15")

set(_obs_source_sha256 "0cc1bd46a3d60c8f4317b38c27414fc0472e04609f4e67ad2142ed1598ef5462")
set(_obs_workspace "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
set(_obs_downloads "${_obs_workspace}/downloads")
set(_obs_sources "${_obs_workspace}/sources")
set(_obs_source_dir "${_obs_sources}/obs-studio-${OBS_SDK_VERSION}")
set(_obs_deps_dir "${_obs_source_dir}/.deps/obs-deps-${OBS_DEPS_VERSION}-x64")
set(_obs_build_dir "${_obs_workspace}/obs-studio-${OBS_SDK_VERSION}-build-x64")
set(_obs_install_dir "${_obs_workspace}/obs-sdk-${OBS_SDK_VERSION}-x64")
set(_obs_sdk_stamp "${_obs_install_dir}/.obs-sync-replay-sdk-complete")

function(_obs_sync_replay_download url output_path expected_sha256)
  if(EXISTS "${output_path}")
    file(SHA256 "${output_path}" actual_sha256)
    if(actual_sha256 STREQUAL expected_sha256)
      return()
    endif()
    file(REMOVE "${output_path}")
  endif()

  get_filename_component(output_directory "${output_path}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  message(STATUS "Downloading ${url}")
  file(
    DOWNLOAD "${url}" "${output_path}"
    EXPECTED_HASH "SHA256=${expected_sha256}"
    STATUS download_status
    TLS_VERIFY ON
  )
  list(GET download_status 0 download_code)
  list(GET download_status 1 download_message)
  if(NOT download_code EQUAL 0)
    file(REMOVE "${output_path}")
    message(FATAL_ERROR "Download failed: ${download_message}")
  endif()
endfunction()

if(NOT EXISTS "${_obs_sdk_stamp}")
  set(_obs_source_archive "${_obs_downloads}/obs-studio-${OBS_SDK_VERSION}.zip")
  _obs_sync_replay_download(
    "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_SDK_VERSION}.zip"
    "${_obs_source_archive}"
    "${_obs_source_sha256}"
  )
  if(NOT EXISTS "${_obs_source_dir}/CMakeLists.txt")
    file(MAKE_DIRECTORY "${_obs_sources}")
    file(ARCHIVE_EXTRACT INPUT "${_obs_source_archive}" DESTINATION "${_obs_sources}")
  endif()
  set(
    _obs_configure_command
    "${CMAKE_COMMAND}"
    --fresh
    -S "${_obs_source_dir}"
    -B "${_obs_build_dir}"
    -G "${CMAKE_GENERATOR}"
  )
  if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _obs_configure_command -A "${CMAKE_GENERATOR_PLATFORM}")
  endif()
  list(
    APPEND
    _obs_configure_command
    "-DOBS_CMAKE_VERSION:STRING=3.0.0"
    "-DOBS_VERSION_OVERRIDE:STRING=${OBS_SDK_VERSION}"
    "-DENABLE_FRONTEND:BOOL=OFF"
    "-DENABLE_PLUGINS:BOOL=OFF"
    "-DENABLE_BROWSER:BOOL=OFF"
    "-DCMAKE_INSTALL_PREFIX:PATH=${_obs_install_dir}"
  )

  message(STATUS "Configuring the pinned OBS ${OBS_SDK_VERSION} libobs SDK")
  execute_process(COMMAND ${_obs_configure_command} COMMAND_ERROR_IS_FATAL ANY)

  foreach(configuration IN ITEMS Debug Release)
    message(STATUS "Building the libobs SDK (${configuration})")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${_obs_build_dir}" --target obs-frontend-api --config
              "${configuration}" --parallel
      COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --install "${_obs_build_dir}" --component Development
              --config "${configuration}" --prefix "${_obs_install_dir}"
      COMMAND_ERROR_IS_FATAL ANY
    )
  endforeach()

  file(WRITE "${_obs_sdk_stamp}" "OBS ${OBS_SDK_VERSION}; obs-deps ${OBS_DEPS_VERSION}\n")
endif()

list(PREPEND CMAKE_PREFIX_PATH "${_obs_install_dir}" "${_obs_deps_dir}")
find_package(libobs CONFIG REQUIRED)
