cmake_minimum_required(VERSION 3.15)
project(pgn2pack CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# lc0 uses x86 BMI2 PEXT in its magic-bitboard path. Disable on non-x86.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i.86)$")
  add_compile_definitions(NO_PEXT NO_POPCNT=0)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -march=native)
  add_link_options(-flto)
  add_compile_options(-flto)
endif()

# libzstd: prefer find_package, fall back to pkg-config.
find_package(zstd QUIET)
if(NOT zstd_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ZSTD REQUIRED libzstd)
endif()

# All non-main sources, so tests can link against them.
add_library(pgn2pack_lib STATIC
  src/chess/bitboard.cc
  src/chess/board.cc
  src/chess/position.cc
  src/pgn/reader.cc
  src/pgn/san.cc
  src/pack/record.cc
)
target_include_directories(pgn2pack_lib PUBLIC src)
if(zstd_FOUND)
  target_link_libraries(pgn2pack_lib PUBLIC zstd::libzstd_shared)
else()
  target_include_directories(pgn2pack_lib PUBLIC ${ZSTD_INCLUDE_DIRS})
  target_link_libraries(pgn2pack_lib PUBLIC ${ZSTD_LIBRARIES})
  target_link_directories(pgn2pack_lib PUBLIC ${ZSTD_LIBRARY_DIRS})
endif()

add_executable(pgn2pack src/main.cc)
target_link_libraries(pgn2pack PRIVATE pgn2pack_lib)

option(PGN2PACK_BUILD_TESTS "Build unit tests" ON)
if(PGN2PACK_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.3
  )
  FetchContent_MakeAvailable(Catch2)

  enable_testing()
  add_executable(pgn2pack_tests
    tests/test_main.cc
    tests/test_record.cc
    tests/test_san.cc
    tests/test_reader.cc
  )
  target_link_libraries(pgn2pack_tests
    PRIVATE pgn2pack_lib Catch2::Catch2)
  target_compile_definitions(pgn2pack_tests PRIVATE
    PGN2PACK_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests")

  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(pgn2pack_tests)
endif()
