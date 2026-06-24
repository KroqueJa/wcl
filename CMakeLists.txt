cmake_minimum_required( VERSION 3.16 )
project( qwc LANGUAGES CXX )


# ---------------------------------------------------------------------------
# Build type
# ---------------------------------------------------------------------------
# Default to an optimized build (the old Makefile always passed -O3).
if( NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES )
  set( CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE )
  set_property( CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel )
endif()

# CMake's stock Release is -O2 -DNDEBUG. Every benchmark in this project was
# measured at -O3, and -O2 vs -O3 can change the autovectorization, so pin it.
# This affects Release and RelWithDebInfo; Debug stays -O0 -g as intended.
set( CMAKE_CXX_FLAGS_RELEASE        "-O3 -DNDEBUG" )
set( CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Force scalar countlines for the main `qwc` target (old: `make SCALAR=1`).
option( QWC_SCALAR "Use the portable scalar countlines for qwc" OFF )

# Sanitizers. ASan+UBSan and TSan are mutually exclusive (can't combine in one
# binary), so they're separate switches. Use these in dedicated CLion profiles
# with their own build dirs, never in the benchmark profile.
option( QWC_ASAN "Build with AddressSanitizer + UBSan" OFF )
option( QWC_TSAN "Build with ThreadSanitizer"          OFF )

# Link-time optimization on Release builds. Lets the linker inline across TUs;
# the qwc dispatch in src/processfile.cpp calls into separately-compiled kernel
# TUs once per buffer, so this matters at the call sites. Opt-out kept so a
# bench can isolate the LTO contribution if a regression is suspected. Gated
# below on CMAKE_BUILD_TYPE being Release-like and on no sanitizer being
# active (LTO + ASan/UBSan/TSan can confuse the sanitizer instrumentation).
option( QWC_LTO "Enable IPO/LTO on Release builds" ON )

# NEON-only: the words movemask uses the vshrn nibble trick (16-byte blocks,
# u64 nibble masks) instead of the vaddv horizontal-reduction construction.
# Default ON -- it wins ~20% on the common UTF-8 word-count path and ~4% on the
# C path, at the cost of a regression on dense 2-byte scripts (see Finding 15).
# Build with -DQWC_NEON_NIBBLE=OFF for the vaddv fallback (the A/B baseline for
# the open dense-2-byte investigation). No effect on the AVX2 build.
option( QWC_NEON_NIBBLE "NEON words: vshrn nibble-mask movemask (OFF = vaddv fallback)" ON )

# NEON-only: validate-csv Phase 2 (quote-aware) builds the in-quote mask with a
# bit-per-byte movemask + prefix-XOR (vmull_p64/PMULL or a shift-XOR ladder)
# instead of the per-byte scalar walk. Default ON. Build -DQWC_CSV_NEON_PHASE2=OFF
# for the scalar-Phase-2 A/B baseline (see Finding 19). No effect on AVX2.
option( QWC_CSV_NEON_PHASE2 "NEON validate-csv Phase 2: prefix-XOR in-quote mask (OFF = scalar walk)" ON )

# Output-name suffix so multiple profiles can coexist without clobbering ./qwc.
# Leave empty for the normal benchmark binary (-> ./qwc). For a sweep, set e.g.
#   -DQWC_SUFFIX=la2   ->   ./qwc-la2
set( QWC_SUFFIX "" CACHE STRING "Suffix for the qwc binary name (e.g. la2 -> qwc-la2)" )

# Generic hook for sweeping compile-time parameters from a profile, mirroring
# how you passed -DADVISE_LOOKAHEAD=N on the command line. Pass a semicolon- or
# space-separated list, e.g.
#   -DQWC_DEFINES=ADVISE_LOOKAHEAD=2;ADVISE_WINDOW_MIB=8
set( QWC_DEFINES "" CACHE STRING "Extra compile definitions (NAME=VALUE list) for sweeps" )

# Embedded version string for --version. Normally LEAVE THIS EMPTY: the build
# derives it from `git describe --tags --always --dirty` at BUILD time (not
# configure time, so it never goes stale between rebuilds), falling back to
# "local" without git. Set it only to override, e.g. -DQWC_VERSION=v9.9.9-rc1.
set( QWC_VERSION "" CACHE STRING "Override the embedded version string (default: git describe)" )

# The generated header lives in the build tree, one per build dir, so sweep
# profiles and the WSL/native trees never fight over it.
set( QWC_VERSION_HEADER_DIR ${CMAKE_BINARY_DIR}/generated )
add_custom_target( qwc_version_header
  COMMAND ${CMAKE_COMMAND}
    -DQWC_VERSION=${QWC_VERSION}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DOUTPUT_FILE=${QWC_VERSION_HEADER_DIR}/qwc_version.h
    -P ${CMAKE_SOURCE_DIR}/cmake/GenerateVersionHeader.cmake
  BYPRODUCTS ${QWC_VERSION_HEADER_DIR}/qwc_version.h
  COMMENT "Refreshing qwc_version.h" )

# Build the GoogleTest-based unit tests (under tests/). On by default for the
# top-level build; harmless to leave on for benchmark profiles since the test
# target is only built by `ctest`/explicit `--target qwc_tests`.
option( QWC_BUILD_TESTS "Build unit tests (requires the tests/googletest submodule)" ON )

# Opt-in clang-tidy gate. When ON, clang-tidy runs on qwc's own translation
# units during the build (never the vendored googletest) and fails the build on
# any finding. Off by default so normal and benchmark builds stay fast and
# unencumbered. The check list lives in .clang-tidy; CI flips this on.
option( QWC_CLANG_TIDY "Run clang-tidy on qwc sources during the build (gate)" OFF )
if( QWC_CLANG_TIDY )
  find_program( QWC_CLANG_TIDY_EXE NAMES clang-tidy )
  if( NOT QWC_CLANG_TIDY_EXE )
    message( FATAL_ERROR "QWC_CLANG_TIDY=ON but clang-tidy was not found on PATH." )
  endif()
  message( STATUS "qwc: clang-tidy gate enabled (${QWC_CLANG_TIDY_EXE})" )
endif()

# Always emit compile_commands.json so clang-tidy, clangd and other tooling can
# find the exact flags for each translation unit. Costs nothing at build time.
set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

# ---------------------------------------------------------------------------
# Architecture dispatch (mirrors the Makefile's uname -m logic)
# ---------------------------------------------------------------------------
if( QWC_SCALAR )
  set( QWC_COUNTLINES src/count_scalar.cpp)
  set( QWC_WORDS src/words_scalar.cpp)
  set( QWC_MAXLINE src/maxlinelen_scalar.cpp)
  set( QWC_CHARS src/chars_scalar.cpp)
  set( QWC_SIMD_FLAGS )
elseif( CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" )
  set( QWC_COUNTLINES src/count_neon.cpp)
  set( QWC_WORDS src/words_neon.cpp)
  set( QWC_MAXLINE src/maxlinelen_neon.cpp)
  set( QWC_CHARS src/chars_neon.cpp)
  # Bare like the old Makefile: Apple clang targets the native Apple Silicon
  # baseline (NEON included) with no flag. If you ever benchmarked with an
  # explicit -mcpu=apple-mN or -mcpu=native, add it here to match.
  set( QWC_SIMD_FLAGS )
elseif( CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64" )
  set( QWC_COUNTLINES src/count_avx2.cpp)
  set( QWC_WORDS src/words_avx2.cpp)
  set( QWC_MAXLINE src/maxlinelen_avx2.cpp)
  set( QWC_CHARS src/chars_avx2.cpp)
  # The hand-written kernels use AVX2 (256-bit) intrinsics, so AVX2 is the floor.
  # Default to the x86-64-v3 microarchitecture level -- AVX2 + FMA + BMI1/2 +
  # LZCNT + POPCNT, the standard "modern x86-64" baseline (Haswell / Excavator,
  # ~2015 onward) -- rather than bare -mavx2. It is a strict superset of -mavx2,
  # also sets matching tuning, and lets the compiler autovectorize the scalar
  # frontend TUs (and the count reductions) more aggressively. The trade-off is
  # the minimum CPU: a v3 binary will SIGILL on anything pre-Haswell.
  #
  # Override to benchmark or to build for one specific box, e.g.
  #   -DQWC_X86_ARCH=native      fastest on the build CPU, NOT portable
  #   -DQWC_X86_ARCH=x86-64-v4   AVX-512 (Skylake-X / Zen 4+)
  #   -DQWC_X86_ARCH=x86-64-v2   AVX2-less fallback (SSE4.2 era) -- but note the
  #                              AVX2 intrinsic kernels then need -mavx2 anyway
  # Needs GCC 11+ / Clang 12+ for the x86-64-vN names; use -mavx2 on older tools.
  set( QWC_X86_ARCH "x86-64-v3" CACHE STRING
       "x86-64 -march level for the SIMD qwc build (e.g. x86-64-v3, native)" )
  set( QWC_SIMD_FLAGS -march=${QWC_X86_ARCH} )
else()
  set( QWC_COUNTLINES src/count_scalar.cpp)
  set( QWC_WORDS src/words_scalar.cpp)
  set( QWC_MAXLINE src/maxlinelen_scalar.cpp)
  set( QWC_CHARS src/chars_scalar.cpp)
  set( QWC_SIMD_FLAGS )
endif()
message( STATUS "qwc: building ${QWC_COUNTLINES}, ${QWC_WORDS}, ${QWC_MAXLINE} and ${QWC_CHARS} for ${CMAKE_SYSTEM_PROCESSOR}" )

# The validate-csv per-chunk kernel (`qwc --validate-csv`): NEON on arm64,
# scalar elsewhere. AVX2 is the flagged release-parity port (not yet started).
if( CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64" AND NOT QWC_SCALAR )
  set( QWC_VALIDATECSV src/validatecsv_neon.cpp )
else()
  set( QWC_VALIDATECSV src/validatecsv_scalar.cpp )
endif()

# Every counting algorithm (countlines, words, max-line-length, chars) is
# arch-dispatched above (SIMD where available, scalar otherwise) and added to
# each target explicitly, so the common set is just the frontend and dispatcher.
# validatecsv.cpp (the always-linked sequential validator + driver) joins them.
set( QWC_COMMON_SOURCES src/main.cpp src/cli.cpp src/processfile.cpp src/validatecsv.cpp )

# GCC 13's -fanalyzer mis-models the libstdc++ vector constructor and reports
# bogus warnings on two of our TUs. Mute exactly those checks, only for
# those TUs, only under GCC. (The options are inert without -fanalyzer;
# clang would warn about the unknown -Wno- flag, hence the compiler gate.)
# GCC 16's analyzer no longer trips these (verified locally), but CI is
# pinned to GCC 13.
#
# - main.cpp: LOCATION-LESS -Wanalyzer-use-of-uninitialized-value against
#   the value-initialized `results` vector in `mapFiles`. A diagnostic with
#   no source location cannot be silenced by any #pragma region, only by
#   the command-line option.
# - cli.cpp: two flavours of the same `printResults` vector-modelling bug:
#   * -Wanalyzer-{null,possible-null}-argument against the `order.data()`
#     argument to `std::qsort` (new on the binary-size branch; the analyzer
#     thinks `order.data()` may be null even after
#     `std::vector<usize> order(numFiles)`);
#   * -Wanalyzer-null-dereference against the `sizes[i] = stat(...)` write
#     after `sizes.resize(numFiles)` -- the analyzer doesn't model that
#     resize initialises the underlying storage.
if( CMAKE_CXX_COMPILER_ID STREQUAL "GNU" )
  set_source_files_properties( src/main.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-analyzer-use-of-uninitialized-value" )
  set_source_files_properties( src/cli.cpp PROPERTIES
    COMPILE_OPTIONS "-Wno-analyzer-null-argument;-Wno-analyzer-possible-null-argument;-Wno-analyzer-null-dereference" )
endif()

# ---------------------------------------------------------------------------
# Shared config applied to a target
# ---------------------------------------------------------------------------
function( qwc_configure target )
  target_include_directories( ${target} PRIVATE include )

  # Every qwc binary embeds the build-time version header (see the
  # qwc_version_header target); the dependency makes the header refresh before
  # any TU that includes it compiles.
  target_include_directories( ${target} PRIVATE ${QWC_VERSION_HEADER_DIR} )
  add_dependencies( ${target} qwc_version_header )

  if( QWC_DEFINES )
    target_compile_definitions( ${target} PRIVATE ${QWC_DEFINES} )
  endif()

  if( QWC_NEON_NIBBLE )
    target_compile_definitions( ${target} PRIVATE QWC_NEON_NIBBLE=1 )
  endif()

  if( QWC_CSV_NEON_PHASE2 )
    target_compile_definitions( ${target} PRIVATE QWC_CSV_NEON_PHASE2=1 )
  endif()

  if( QWC_ASAN AND QWC_TSAN )
    message( FATAL_ERROR "QWC_ASAN and QWC_TSAN cannot be enabled together." )
  elseif( QWC_ASAN )
    target_compile_options( ${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g )
    target_link_options(    ${target} PRIVATE -fsanitize=address,undefined )
  elseif( QWC_TSAN )
    target_compile_options( ${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g )
    target_link_options(    ${target} PRIVATE -fsanitize=thread )
  endif()

  # Link libstdc++/libgcc statically. Startup of a short-lived process is
  # dominated by the dynamic loader binding and faulting in libstdc++.so for
  # the filesystem/thread/string machinery (measured ~0.9 ms of the ~1.3 ms
  # qwc-vs-GNU-wc gap on a bare `-c`, which never reads the file -- and the
  # cost stays even with <iostream> banished from the sources); linked
  # statically those relocations are resolved at link time and the cost
  # disappears (2.6 -> 1.7 ms, vs wc's 1.3 ms, on WSL2). Costs ~1.6 MiB of
  # binary size. Not on Apple: AppleClang has no -static-libgcc, and libc++
  # ships in the dyld shared cache anyway.
  if( NOT APPLE )
    target_link_options( ${target} PRIVATE -static-libstdc++ -static-libgcc )

    # Dead-strip unused functions and data at link time. With static libstdc++
    # the image is ~354 KB, big enough that the kernel pays ~276 minor faults
    # to map the code on startup vs wc's 96 (~50-90 us). Putting each function
    # and data object in its own section and letting the linker garbage-collect
    # the unreferenced ones trims the image without changing any source.
    # GNU ld / lld syntax; Apple's ld64 uses -Wl,-dead_strip on its own.
    target_compile_options( ${target} PRIVATE -ffunction-sections -fdata-sections )
    target_link_options( ${target} PRIVATE -Wl,--gc-sections )
  endif()

  # Run the clang-tidy gate on this target's own sources (set per-target, not
  # globally, so the vendored googletest is never linted). --warnings-as-errors
  # makes any finding fail the build.
  if( QWC_CLANG_TIDY AND QWC_CLANG_TIDY_EXE )
    set_target_properties( ${target} PROPERTIES
      CXX_CLANG_TIDY "${QWC_CLANG_TIDY_EXE};--warnings-as-errors=*" )
  endif()

endfunction()

# ---------------------------------------------------------------------------
# Main target
# ---------------------------------------------------------------------------
if( QWC_SUFFIX STREQUAL "" )
  set( QWC_OUTPUT_NAME "qwc" )
else()
  set( QWC_OUTPUT_NAME "qwc-${QWC_SUFFIX}" )
endif()

add_executable( qwc ${QWC_COMMON_SOURCES} ${QWC_COUNTLINES} ${QWC_WORDS} ${QWC_MAXLINE} ${QWC_CHARS} ${QWC_VALIDATECSV} )
set_target_properties( qwc PROPERTIES OUTPUT_NAME ${QWC_OUTPUT_NAME} )
target_compile_options( qwc PRIVATE ${QWC_SIMD_FLAGS} )
# Binary-size flags only apply to the Linux + glibc + static-libstdc++ link
# path, and only in Release. The big wins come from src/main.cpp's overrides
# of the libstdc++ / libgcc terminate-handler / personality-routine /
# unwind-API symbols (~110 KB of text), which work only because static
# archives contribute objects only when they satisfy unresolved references --
# a model that does not apply to Apple's dyld-shared-cache libc++/libunwind.
# The Release gate matches the C++-side `#if !defined(__APPLE__) &&
# defined(NDEBUG)` in main.cpp: Debug keeps full exceptions / unwinding /
# verbose terminate so debuggers and sanitizers work normally.
if( NOT APPLE
    AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo" )
  # Drop exceptions and RTTI for the qwc binary only -- qwc never throws and
  # uses no dynamic_cast / typeid; the implicit std::bad_alloc edges become
  # std::terminate() -> abort() under -fno-exceptions, which matches the
  # _Exit(1) contract qwc already gives on fatal errors. Per-target so
  # qwc-scalar and the googletest TUs are unaffected (googletest itself
  # requires exceptions to compile its EXPECT_THROW machinery).
  target_compile_options( qwc PRIVATE -fno-exceptions -fno-rtti )
  # Drop .eh_frame emission from qwc's own TUs. Combined with -fno-exceptions
  # above, our code has no use for unwind tables; the only consumer would be a
  # crash-time debugger walking the stack across qwc frames, which is a luxury
  # the side-project posture can do without. Does not affect libstdc++.a's
  # own .eh_frame entries (those came pre-built).
  target_compile_options( qwc PRIVATE -fno-asynchronous-unwind-tables -fno-unwind-tables )
endif()
qwc_configure( qwc )

# Apply IPO/LTO to the qwc target only (qwc-scalar and qwc_tests stay
# unprofiled+unLTO'd -- the scalar build is the SIMD-vs-autovec baseline and
# must stay clean; LTO-ing tests slows the iteration cycle for no semantic
# gain). check_ipo_supported() makes this warn-and-fall-through on toolchains
# that don't support LTO, matching the project's -march=x86-64-v3 fallback
# discipline.
if( QWC_LTO
    AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo"
    AND NOT QWC_ASAN AND NOT QWC_TSAN )
  include( CheckIPOSupported )
  check_ipo_supported( RESULT _qwc_ipo_ok OUTPUT _qwc_ipo_msg )
  if( _qwc_ipo_ok )
    set_property( TARGET qwc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE )
    message( STATUS "qwc: IPO/LTO enabled" )
  else()
    message( WARNING "qwc: IPO/LTO requested but unsupported: ${_qwc_ipo_msg}" )
  endif()
endif()

message( STATUS "qwc: output binary -> ${CMAKE_SOURCE_DIR}/${QWC_OUTPUT_NAME}" )

# Portable scalar build, on demand (old: `make qwc-scalar`). Uses the scalar
# implementation of every algorithm -- countlines, words, max-line-length and
# chars included -- so it serves as the baseline the SIMD build is benchmarked
# against. Built with `cmake --build <dir> --target qwc-scalar`; emits
# ./qwc-scalar.
add_executable( qwc-scalar EXCLUDE_FROM_ALL
  ${QWC_COMMON_SOURCES} src/count_scalar.cpp src/words_scalar.cpp
  src/maxlinelen_scalar.cpp src/chars_scalar.cpp src/validatecsv_scalar.cpp )
qwc_configure( qwc-scalar )
# Build the scalar baseline at the SAME -march as the SIMD qwc (QWC_SIMD_FLAGS).
# This isolates the variable under test -- hand-written SIMD vs the compiler's
# autovectorization -- by giving both binaries the same ISA ceiling (e.g. AVX2
# under x86-64-v3) instead of leaving the baseline at the portable SSE2 default.
target_compile_options( qwc-scalar PRIVATE ${QWC_SIMD_FLAGS} )
target_compile_options( qwc PRIVATE -Wall -Wextra -Wpedantic )

# ---------------------------------------------------------------------------
# Convenience targets wrapping the Python helpers, run from the project root.
# ---------------------------------------------------------------------------
add_custom_target( gen-test-data
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/gen-test-data.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        USES_TERMINAL )

add_custom_target( bench
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/bench.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS qwc
        USES_TERMINAL )

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if( QWC_BUILD_TESTS )
  enable_testing()
  add_subdirectory( tests )
endif()