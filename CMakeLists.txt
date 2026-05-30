cmake_minimum_required(VERSION 3.23)

project(HttpWebServer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Default to debug build." FORCE)
endif()

add_compile_options(
  $<$<CONFIG:Debug>:-g3>
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  $<$<CONFIG:Debug>:-fno-omit-frame-pointer>

  $<$<CONFIG:Release>:-g>
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-march=native>
  $<$<CONFIG:Release>:-fno-math-errno>
  $<$<CONFIG:Release>:-fstack-protector-strong>
  $<$<CONFIG:Release>:-fno-rtti>
)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

set(ENABLE_PGO "OFF" CACHE STRING "Enable Profile Guided Optimization")
set_property(CACHE ENABLE_PGO PROPERTY STRINGS "OFF" "GENERATE" "CS_GENERATE" "USE")

if(ENABLE_PGO STREQUAL "GENERATE")
  add_compile_options(-fprofile-generate)
  add_link_options(-fprofile-generate)
elseif(ENABLE_PGO STREQUAL "CS_GENERATE")
  add_compile_options(-fcs-profile-generate)
  add_link_options(-fcs-profile-generate)
elseif(ENABLE_PGO STREQUAL "USE")
  add_compile_options(-fprofile-use=${CMAKE_SOURCE_DIR}/pgo.profdata)
  add_link_options(-fprofile-use=${CMAKE_SOURCE_DIR}/pgo.profdata)
endif()

find_path(JEMALLOC_INCLUDE_DIR jemalloc/jemalloc.h)
find_library(JEMALLOC_LIBRARY jemalloc)
if (NOT JEMALLOC_INCLUDE_DIR OR NOT JEMALLOC_LIBRARY)
  message(FATAL_ERROR "jemalloc not found! (e.g. run `brew install jemalloc`)")
endif()

add_compile_definitions(
  $<$<CONFIG:Debug>:_GLIBCXX_DEBUG>
  $<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>
)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined> $<$<CONFIG:Release>:-Wl,-dead_strip>)

add_executable(server)
target_include_directories(server PRIVATE src ${JEMALLOC_INCLUDE_DIR})
target_compile_options(server PRIVATE
  -Wall
  -Wextra
  -Wshadow
  -Wpedantic
  -Wconversion
  -Wformat=2
  -Werror
)
target_sources(server PRIVATE
  src/main.cpp
  src/server/server.hpp src/server/server.cpp
  src/server/server_worker.hpp src/server/server_worker.cpp
  src/request/enums.hpp src/request/request.hpp src/request/request.cpp
  src/utils/sys.hpp
)

target_link_libraries(server ${CMAKE_THREAD_LIBS_INIT} ${JEMALLOC_LIBRARY})
