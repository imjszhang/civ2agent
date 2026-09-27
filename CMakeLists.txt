cmake_minimum_required(VERSION 3.15)
project(civ2agent CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "civ2agent must be built as 32-bit (cmake -A Win32)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  string(TOUPPER ${cfg} CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

add_library(civ2agent SHARED
  dll/dllmain.cpp
  dll/game.cpp
  dll/server.cpp
)
target_include_directories(civ2agent PRIVATE dll)
target_compile_options(civ2agent PRIVATE /W3 /EHa /utf-8)
target_link_libraries(civ2agent PRIVATE advapi32 user32 kernel32)
set_target_properties(civ2agent PROPERTIES OUTPUT_NAME civ2agent)

add_executable(civ2agent-launch launch/main.cpp)
target_compile_definitions(civ2agent-launch PRIVATE UNICODE _UNICODE)
target_compile_options(civ2agent-launch PRIVATE /W3 /utf-8)
target_link_options(civ2agent-launch PRIVATE /ENTRY:wmainCRTStartup)
target_link_libraries(civ2agent-launch PRIVATE kernel32 user32)
