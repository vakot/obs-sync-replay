if(NOT DEFINED PLUGIN_FILE OR NOT EXISTS "${PLUGIN_FILE}")
  message(FATAL_ERROR "Plugin artifact does not exist: ${PLUGIN_FILE}")
endif()

file(SIZE "${PLUGIN_FILE}" plugin_size)
if(plugin_size EQUAL 0)
  message(FATAL_ERROR "Plugin artifact is empty: ${PLUGIN_FILE}")
endif()

message(STATUS "Verified plugin artifact: ${PLUGIN_FILE} (${plugin_size} bytes)")
