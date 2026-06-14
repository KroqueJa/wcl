# ---------------------------------------------------------------------------
# GoogleTest (vendored as a git submodule under thirdparty/googletest)
# ---------------------------------------------------------------------------
set( QWC_GTEST_DIR "${CMAKE_SOURCE_DIR}/thirdparty/googletest" )
if( NOT EXISTS "${QWC_GTEST_DIR}/CMakeLists.txt" )
  message( FATAL_ERROR
    "GoogleTest submodule is missing. Run:\n"
    "  git submodule update --init --recursive" )
endif()

# We only need googletest, not googlemock, and we don't want it installed.
set( BUILD_GMOCK   OFF CACHE BOOL "" FORCE )
set( INSTALL_GTEST OFF CACHE BOOL "" FORCE )
# On Windows, link gtest against the shared CRT to match consuming targets.
set( gtest_force_shared_crt ON CACHE BOOL "" FORCE )

# Out-of-tree source dir needs an explicit binary dir.
add_subdirectory( ${QWC_GTEST_DIR} ${CMAKE_BINARY_DIR}/thirdparty/googletest EXCLUDE_FROM_ALL )

# ---------------------------------------------------------------------------
# Test executable
# ---------------------------------------------------------------------------
# Reuse the same countlines implementation (and SIMD flags) the main build
# selected, so we test exactly what ships. QWC_COUNTLINES / QWC_SIMD_FLAGS are
# set by the top-level CMakeLists before this directory is added. processfile.cpp
# is linked directly so processFile() can be unit-tested as a function; main.cpp
# is exercised end-to-end via cli_test.cpp instead (gtest_main owns main()).
add_executable( qwc_tests
  count_test.cpp
  words_test.cpp
  chars_test.cpp
  maxlinelen_test.cpp
  processfile_test.cpp
  cli_test.cpp
  ${CMAKE_SOURCE_DIR}/src/processfile.cpp
  ${CMAKE_SOURCE_DIR}/${QWC_WORDS}
  ${CMAKE_SOURCE_DIR}/${QWC_CHARS}
  ${CMAKE_SOURCE_DIR}/${QWC_MAXLINE}
  ${CMAKE_SOURCE_DIR}/${QWC_COUNTLINES}
)
target_include_directories( qwc_tests PRIVATE ${CMAKE_SOURCE_DIR}/include )
target_compile_options( qwc_tests PRIVATE ${QWC_SIMD_FLAGS} )
target_link_libraries( qwc_tests PRIVATE GTest::gtest_main )

# cli_test.cpp drives the real binary; hand it the resolved path and make sure
# the binary is built first.
target_compile_definitions( qwc_tests PRIVATE QWC_BINARY="$<TARGET_FILE:qwc>" )
add_dependencies( qwc_tests qwc )

include( GoogleTest )
gtest_discover_tests( qwc_tests )
