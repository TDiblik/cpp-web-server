cmake_minimum_required(VERSION 3.23)

project(HttpWebServer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Default to debug build." FORCE)
endif()

add_compile_options(
  -Wall
  -Wextra
  -Wshadow
  -Wpedantic
  -Wconversion
  -Wformat=2
  -Werror

  $<$<CONFIG:Debug>:-g3>
  $<$<CONFIG:Debug>:-O0>
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  $<$<CONFIG:Debug>:-fno-omit-frame-pointer>

  $<$<CONFIG:Release>:-g>
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-march=native>
  $<$<CONFIG:Release>:-fno-math-errno>
  $<$<CONFIG:Release>:-fstack-protector-strong>
)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_DEBUG>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

add_executable(server)
target_include_directories(server PRIVATE src)
target_sources(server PRIVATE
  src/main.cpp
  src/server/server.hpp src/server/server.cpp
  src/request/enums.hpp src/request/request.hpp src/request/request.cpp
)

target_link_libraries(server ${CMAKE_THREAD_LIBS_INIT})
