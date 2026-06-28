cmake_minimum_required(VERSION 3.16)
project(ocli LANGUAGES CXX)

# --- C++20 -----------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# --- dependencies ----------------------------------------------------------
# libcurl: HTTP + SSE/NDJSON streaming.
find_package(CURL REQUIRED)

# threads (pthread): std::thread / producer-consumer streaming, Spinner, interrupter.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- sources ---------------------------------------------------------------
# Build target 'ocli' from every translation unit in src/.
file(GLOB OCLI_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

add_executable(ocli ${OCLI_SOURCES})

target_include_directories(ocli PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/third_party"   # json.hpp
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ocli PRIVATE
  CURL::libcurl
  Threads::Threads
)

# macOS: realpath/getaddrinfo/termios/pty are in libc/libSystem — nothing extra needed.
