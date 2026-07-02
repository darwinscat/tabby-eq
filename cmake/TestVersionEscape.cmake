# SPDX-License-Identifier: AGPL-3.0-or-later
# ----------------------------------------------------------------------------
# ctest unit for GenerateTabbyVersion.cmake's C++ string-literal escaping: runs the generator with
# a HOSTILE TABBYEQ_BUILDER (double quote + backslash + tab — the same class of input a weird git
# tag would inject into kDescribe, exercised through the one input we fully control) and asserts
# the emitted header carries a correctly escaped literal. Registered as `tabbyeq_version_escape`.
#
# Expected -D inputs: GENERATOR (the generator script), TMP_DIR (a scratch dir inside the build tree).
# ----------------------------------------------------------------------------

if(NOT GENERATOR OR NOT TMP_DIR)
    message(FATAL_ERROR "TestVersionEscape: pass -DGENERATOR=<GenerateTabbyVersion.cmake> -DTMP_DIR=<scratch>")
endif()

set(_out "${TMP_DIR}/TabbyVersion.h")
file(REMOVE "${_out}")

# Hostile builder: we"ird\bu<TAB>ilder  → must emit as  "we\"ird\\bu ilder"
set(ENV{TABBYEQ_BUILDER} "we\"ird\\bu\tilder")

# No git / no repo on purpose: kDescribe degrades to "0.0.0-dev" and the builder is the hostile input.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -DGIT_EXECUTABLE=
        -DSRC_DIR=
        "-DOUT_FILE=${_out}"
        -DFCORE_LOCAL=OFF
        -DFCORE_DIR=
        -DFCORE_TAG=v9.9.9
        -P "${GENERATOR}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "TestVersionEscape: the generator failed (rc=${_rc})")
endif()

if(NOT EXISTS "${_out}")
    message(FATAL_ERROR "TestVersionEscape: no header emitted at ${_out}")
endif()
file(READ "${_out}" _content)

# The escaped literal we demand: kBuilder = "we\"ird\\bu ilder"  (tab collapsed to a space).
set(_expected "kBuilder     = \"we\\\"ird\\\\bu ilder\"")
string(FIND "${_content}" "${_expected}" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR "TestVersionEscape: escaped kBuilder literal not found.\nExpected substring: ${_expected}\nHeader was:\n${_content}")
endif()

# And the RAW (unescaped) hostile text must NOT appear anywhere (it would end the literal early).
string(FIND "${_content}" "we\"ird" _raw_pos)
if(NOT _raw_pos EQUAL -1)
    message(FATAL_ERROR "TestVersionEscape: the raw unescaped quote leaked into the header:\n${_content}")
endif()

message(STATUS "TestVersionEscape: escaped literal verified — ok")
