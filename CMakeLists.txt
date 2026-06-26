cmake_minimum_required(VERSION 3.22)

# ----------------------------------------------------------------------------
# TabbyEQ — educational semantic EQ (C++/JUCE). Felitronics line, by Darwin's Cat.
#
# The DSP lives in `teq/` — a reusable, framework-agnostic, header-only matched-EQ core
# (JUCE-free) with its own self-tests. The plugin (slice 2) is a thin JUCE adapter on top.
# Building this repo today builds + runs the teq core tests (no JUCE fetch needed).
# See docs/CONCEPT.md.
# ----------------------------------------------------------------------------
project(TabbyEQ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build archs" FORCE)
endif()

# --- The reusable EQ DSP core + its JUCE-free self-tests.
set(TEQ_BUILD_TESTS ON)
add_subdirectory(teq)

# --- SLICE 2: the TabbyEQ plugin target (VST3 + AU + Standalone + CLAP) — fetches JUCE 8.0.14
#     (FetchContent, pinned, same tag as orbitcab) + clap-juce-extensions, plus the YAML->C++
#     vocab codegen. Deferred so the core builds and tests without fetching JUCE.
