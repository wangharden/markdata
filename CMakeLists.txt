# md_gate: single TDF login process that writes latest market snapshots into shared memory.
cmake_minimum_required(VERSION 3.10)
project(md_gate CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/utf-8)
endif()

if(WIN32)
  set(SDK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/include_external/windows")
  set(SDK_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/windows")
else()
  set(SDK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/include_external/linux")
  set(SDK_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/lib/linux")
endif()

include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${SDK_INCLUDE_DIR}
)

link_directories(${SDK_LIB_DIR})

add_executable(md_gate
  md_gate_main.cpp
  src/shm_writer.cpp
  src/shm_reader.cpp
)

if(WIN32)
  target_link_libraries(md_gate
    TDFAPI30
    ws2_32
  )

  # Ensure runtime can find the DLL next to the executable.
  add_custom_command(TARGET md_gate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDK_LIB_DIR}/TDFAPI30.dll"
            "$<TARGET_FILE_DIR:md_gate>/TDFAPI30.dll"
  )
else()
  target_link_libraries(md_gate
    TDFAPI30
    pthread
    rt
  )

  set_target_properties(md_gate PROPERTIES
    BUILD_RPATH "${SDK_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib/linux:${SDK_LIB_DIR}"
    BUILD_RPATH_USE_ORIGIN ON
  )
endif()

message(STATUS "========================================")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "SDK Include: ${SDK_INCLUDE_DIR}")
message(STATUS "SDK Lib: ${SDK_LIB_DIR}")
message(STATUS "========================================")
