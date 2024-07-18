include_guard(GLOBAL)

if(userver_kafka_FOUND)
  return()
endif()

find_package(userver REQUIRED COMPONENTS
    core
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")
include("${USERVER_CMAKE_DIR}/SetupRdKafka.cmake")

set(userver_kafka_FOUND TRUE)
