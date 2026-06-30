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

# --- The reusable EQ DSP core — consumed from felitronics-core (teq:: is the transitional compat shim, so
# src/ is unchanged). HYBRID consumption: if a local sibling checkout exists (TABBYEQ_FCORE_DIR, default
# ../felitronics-core) it's used as-is for fast co-development; otherwise the pinned public release is
# fetched from GitHub, so a fresh clone / CI is self-contained. Set TABBYEQ_FCORE_DIR="" to force the fetch.
include(FetchContent)
set(TABBYEQ_FCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../felitronics-core" CACHE PATH "Local felitronics-core checkout (used if present); else the pinned release is fetched")
set(TABBYEQ_FCORE_TAG "v0.1.1" CACHE STRING "felitronics-core git tag to fetch when no local checkout is present")
set(FELITRONICS_BUILD_TESTS ON)   # keep the JUCE-free core self-tests in this repo's `ctest`
enable_testing()                  # so this repo's ctest discovers felitronics-core's suites (top-level)
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

# The plugin pulls JUCE — gate it so CI (and anyone who only wants the core) builds the
# JUCE-free teq engine + tests without fetching JUCE. `-DTABBYEQ_BUILD_PLUGIN=OFF` for that.
option(TABBYEQ_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/Standalone/CLAP)" ON)
if(TABBYEQ_BUILD_PLUGIN)

# ----------------------------------------------------------------------------
# The TabbyEQ plugin — a thin JUCE adapter over teq_core. JUCE + clap-juce-extensions are
# fetched (FetchContent, pinned, same tags as orbitcab). Builds VST3 + AU + Standalone + CLAP.
# ----------------------------------------------------------------------------
include(FetchContent)

set(TABBYEQ_JUCE_TAG "8.0.14" CACHE STRING "JUCE git tag")
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${TABBYEQ_JUCE_TAG}
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(juce)

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
