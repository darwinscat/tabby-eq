cmake_minimum_required(VERSION 3.22)
project(teq VERSION 0.1.0 LANGUAGES CXX)

# ----------------------------------------------------------------------------
# teq — reusable, framework-agnostic matched-EQ DSP core.
#
# Header-only INTERFACE library. JUCE-free. Drop the `teq/` folder into any project:
#   add_subdirectory(path/to/teq)
#   target_link_libraries(myplugin PRIVATE teq::core)   # then #include <teq/EqEngine.h>
# ----------------------------------------------------------------------------
add_library(teq_core INTERFACE)
add_library(teq::core ALIAS teq_core)   # namespaced alias for consumers (typo -> configure error)
target_include_directories(teq_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(teq_core INTERFACE cxx_std_20)

# Self-tests (JUCE-free) build only when teq is the top-level project OR a parent sets
# TEQ_BUILD_TESTS — so a consumer that add_subdirectory()'s teq doesn't inherit them.
if(PROJECT_IS_TOP_LEVEL OR TEQ_BUILD_TESTS)
    add_executable(teq_tests
        tests/TestMain.cpp
        tests/MatchedBiquadTests.cpp
        tests/EqEngineTests.cpp)
    target_link_libraries(teq_tests PRIVATE teq_core)
    target_compile_features(teq_tests PRIVATE cxx_std_20)
    enable_testing()
    add_test(NAME teq_tests COMMAND teq_tests)
endif()
