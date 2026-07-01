cmake_minimum_required(VERSION 3.22)

# ----------------------------------------------------------------------------
# TabbyEQ — educational semantic EQ (C++/JUCE). Felitronics line, by Darwin's Cat.
#
# The DSP core is consumed from felitronics-core (FetchContent — see the hybrid block below); the
# `teq::` namespace is a compatibility shim, so the plugin in src/ is a thin JUCE adapter on top,
# unchanged by the move. `-DTABBYEQ_BUILD_PLUGIN=OFF` configures core-only (no JUCE fetch) and runs
# its JUCE-free test suites via `ctest`. See docs/CONCEPT.md.
# ----------------------------------------------------------------------------
project(TabbyEQ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build archs" FORCE)
endif()

# ----------------------------------------------------------------------------
# Build knobs. The plugin (JUCE) is ON by default; the JUCE-free CI configures -DTABBYEQ_BUILD_PLUGIN=OFF.
# TABBYEQ_BUILD_TESTS builds the headless adapter lifecycle/misuse harness (needs JUCE, run under ASan/UBSan
# — see tests/ + the sanitizers CI job). TABBYEQ_BUILD_CORE_TESTS keeps felitronics-core's own JUCE-free
# suites in this repo's ctest (turn OFF in the sanitizer job — we do NOT re-test the already-hardened core
# here). TABBYEQ_ENABLE_SANITIZERS instruments ONLY TabbyEQ's own sources with ASan+UBSan.
# ----------------------------------------------------------------------------
option(TABBYEQ_BUILD_PLUGIN     "Build the JUCE plugin (VST3/AU/Standalone/CLAP)"                 ON)
option(TABBYEQ_BUILD_TESTS      "Build the headless adapter lifecycle/misuse test (ctest)"        OFF)
option(TABBYEQ_BUILD_CORE_TESTS "Build + ctest felitronics-core's JUCE-free self-tests"           ON)
option(TABBYEQ_ENABLE_SANITIZERS "Instrument TabbyEQ's own sources with ASan+UBSan (Clang/GCC)"   OFF)

if(TABBYEQ_ENABLE_SANITIZERS AND NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    message(FATAL_ERROR "TABBYEQ_ENABLE_SANITIZERS requires Clang or GCC (MSVC has no UBSan).")
endif()

# tabbyeq_sanitize(<target> <our .cpp files...>) — ASan+UBSan on OUR sources only, never on JUCE.
# We use set_source_files_properties, NOT target_compile_options, on purpose: juce_add_* injects each JUCE
# module's unity .cpp INTO the consuming target, so a target-wide flag would compile ALL of JUCE with the
# sanitizers too — and JUCE's own (benign) internal UB would then red-fail -fno-sanitize-recover on paths we
# don't even own (esp. the Linux X11 / message-queue native code the dev machine never compiles). Scoping to
# our listed files keeps UBSan on TabbyEQ's adapter + the header-only felitronics-core it inlines. ASan is
# still process-wide (the runtime is pulled in by the link flag), so heap-overflow / use-after-free ANYWHERE,
# including our misuse of a JUCE buffer, is still caught. Flag set mirrors felitronics-core's; LeakSanitizer
# is left to the CI env (ASAN_OPTIONS=detect_leaks=0 — JUCE keeps process-exit singletons by design).
function(tabbyeq_sanitize target)
    if(NOT TABBYEQ_ENABLE_SANITIZERS)
        return()
    endif()
    set(_opts -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _opts -ftrivial-auto-var-init=pattern)   # cheap uninitialised-stack tripwire (Clang only)
    endif()
    set_source_files_properties(${ARGN} TARGET_DIRECTORY ${target} PROPERTIES
        COMPILE_OPTIONS     "${_opts}"
        COMPILE_DEFINITIONS "_GLIBCXX_ASSERTIONS")           # libstdc++ container bounds checks (inert on libc++)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()

# --- The reusable EQ DSP core — consumed from felitronics-core (teq:: is the transitional compat shim, so
# src/ is unchanged). HYBRID consumption: if a local sibling checkout exists (TABBYEQ_FCORE_DIR, default
# ../felitronics-core) it's used as-is for fast co-development; otherwise the pinned public release is
# fetched from GitHub, so a fresh clone / CI is self-contained. Set TABBYEQ_FCORE_DIR="" to force the fetch.
include(FetchContent)
set(TABBYEQ_FCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../felitronics-core" CACHE PATH "Local felitronics-core checkout (used if present); else the pinned release is fetched")
set(TABBYEQ_FCORE_TAG "v0.1.4" CACHE STRING "felitronics-core git tag to fetch when no local checkout is present")
set(FELITRONICS_BUILD_TESTS ${TABBYEQ_BUILD_CORE_TESTS})   # keep the JUCE-free core self-tests in this repo's `ctest` (unless the sanitizer job opts out)
enable_testing()                  # so this repo's ctest discovers felitronics-core's suites + our adapter test (top-level)
if(TABBYEQ_FCORE_DIR AND EXISTS "${TABBYEQ_FCORE_DIR}/CMakeLists.txt")
    message(STATUS "felitronics-core: local checkout — ${TABBYEQ_FCORE_DIR}")
    FetchContent_Declare(felitronics_core SOURCE_DIR "${TABBYEQ_FCORE_DIR}")
else()
    message(STATUS "felitronics-core: fetching ${TABBYEQ_FCORE_TAG} from GitHub")
    FetchContent_Declare(felitronics_core
        GIT_REPOSITORY https://github.com/darwinscat/felitronics-core.git
        GIT_TAG        ${TABBYEQ_FCORE_TAG}
        GIT_SHALLOW    TRUE)
endif()
FetchContent_MakeAvailable(felitronics_core)

# JUCE is fetched when EITHER the plugin OR the headless adapter test is built (both link it); a pure
# core build (-DTABBYEQ_BUILD_PLUGIN=OFF with tests off) never pulls it. Pinned like orbitcab.
if(TABBYEQ_BUILD_PLUGIN OR TABBYEQ_BUILD_TESTS)
    include(FetchContent)
    set(TABBYEQ_JUCE_TAG "8.0.14" CACHE STRING "JUCE git tag")
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${TABBYEQ_JUCE_TAG}
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(juce)
endif()

if(TABBYEQ_BUILD_PLUGIN)

# ----------------------------------------------------------------------------
# The TabbyEQ plugin — a thin JUCE adapter over teq_core. clap-juce-extensions is fetched
# (FetchContent, pinned, same tags as orbitcab). Builds VST3 + AU + Standalone + CLAP.
# ----------------------------------------------------------------------------
include(FetchContent)

# CLAP via the MIT clap-juce-extensions wrapper (native CLAP slated for JUCE 9). Pinned commit
# like orbitcab (upstream stopped tagging in 2022; main is their de-facto release channel).
set(TABBYEQ_CLAP_EXT_TAG "51a9359315298de632cf44e9d7524940868441e6" CACHE STRING "clap-juce-extensions commit")
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        ${TABBYEQ_CLAP_EXT_TAG})
FetchContent_MakeAvailable(clap-juce-extensions)

# 4-char codes (LOCKED post-release): MANUFACTURER Dcat (Darwin's Cat), PLUGIN_CODE TbEq.
juce_add_plugin(TabbyEQ
    PRODUCT_NAME                "TabbyEQ"
    COMPANY_NAME                "Darwin's Cat"
    BUNDLE_ID                   "com.darwinscat.tabbyeq"
    PLUGIN_MANUFACTURER_CODE    Dcat
    PLUGIN_CODE                 TbEq
    FORMATS                     VST3 AU Standalone
    VST3_CATEGORIES             "Fx" "EQ"
    AU_MAIN_TYPE                "kAudioUnitType_Effect"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "TabbyEQ analyses live audio input to draw the spectrum."
    COPY_PLUGIN_AFTER_BUILD     TRUE)

clap_juce_extensions_plugin(TARGET TabbyEQ
    CLAP_ID       "com.darwinscat.tabbyeq"
    CLAP_FEATURES audio-effect equalizer)

target_sources(TabbyEQ PRIVATE
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/PluginEditor.cpp
    src/ui/EqCurveDisplay.cpp
    src/ui/BandEditStrip.cpp)

target_include_directories(TabbyEQ PRIVATE src)   # "Parameters.h", "Vocab.h"

target_compile_definitions(TabbyEQ PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(TabbyEQ PRIVATE
    teq::core                           # the framework-agnostic EQ engine
    felitronics::lineareq               # JUCE-free linear-phase EQ (pulls core + eq + convolution)
    juce::juce_audio_utils              # AudioProcessor, components, parameter attachments
    juce::juce_dsp                      # (legacy link; the FFT/convolution are now core — JUCE-dsp-free in src)
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Re-sign the Standalone ad-hoc with the correct bundle id so macOS TCC (mic access) persists
# across rebuilds — JUCE's linker-sign otherwise uses the exe name ("TabbyEQ") as the identifier.
if(APPLE)
    add_custom_command(TARGET TabbyEQ_Standalone POST_BUILD
        COMMAND codesign --force --sign - --identifier "com.darwinscat.tabbyeq" "$<TARGET_BUNDLE_DIR:TabbyEQ_Standalone>"
        VERBATIM)
endif()

endif()  # TABBYEQ_BUILD_PLUGIN

if(TABBYEQ_BUILD_TESTS)

# ----------------------------------------------------------------------------
# Headless adapter lifecycle/misuse harness — drives the REAL TabbyEqAudioProcessor through adversarial host
# orders (process before prepare / after release, double prepare, mode switches, mono→stereo, short/empty
# buffers, hostile restored state) under ASan+UBSan. It links TabbyEQ's OWN sources (minus the JUCE plugin
# wrappers), so the sanitizer instruments the ADAPTER glue — NOT the core (hardened + tested upstream) and
# NOT JUCE. No editor is ever created, so no X display is needed on the runner. See tests/lifecycle_misuse.cpp.
# ----------------------------------------------------------------------------
juce_add_console_app(TabbyEQ_LifecycleTest PRODUCT_NAME "TabbyEQ_LifecycleTest")

target_sources(TabbyEQ_LifecycleTest PRIVATE
    tests/lifecycle_misuse.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/PluginEditor.cpp                # createEditor() references TabbyEqEditor → must link it (never instantiated)
    src/ui/EqCurveDisplay.cpp
    src/ui/BandEditStrip.cpp)

target_include_directories(TabbyEQ_LifecycleTest PRIVATE src)

target_compile_definitions(TabbyEQ_LifecycleTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1        # runDispatchLoopUntil() — pump the msg loop so the ctor's 30 Hz timer fires
    JucePlugin_Name="TabbyEQ")          # getName(); juce_add_plugin normally supplies this macro

target_link_libraries(TabbyEQ_LifecycleTest PRIVATE
    teq::core
    felitronics::lineareq
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ASan+UBSan on TabbyEQ's OWN sources only (never JUCE) — no-op unless -DTABBYEQ_ENABLE_SANITIZERS=ON.
tabbyeq_sanitize(TabbyEQ_LifecycleTest
    tests/lifecycle_misuse.cpp
    src/PluginProcessor.cpp
    src/Parameters.cpp
    src/PluginEditor.cpp
    src/ui/EqCurveDisplay.cpp
    src/ui/BandEditStrip.cpp)

add_test(NAME tabbyeq_lifecycle_misuse COMMAND TabbyEQ_LifecycleTest)

endif()  # TABBYEQ_BUILD_TESTS
