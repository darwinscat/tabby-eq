# Agent task — repoint TabbyEQ onto felitronics-core (T1)

**Goal:** stop owning the DSP core locally in `teq/`; consume it from `felitronics-core` instead, with
**zero `src/` source change** and the plugin staying byte-identical. This is the ADR §6 step-4 move.

## Context (verified)
- TabbyEQ's `teq/` was migrated **verbatim** into `felitronics-core` as `felitronics::eq` (+ `felitronics::core` for `Smoother`/`Math`/`kMaxChannels`, `felitronics::analysis` for `SpectrumTap`). felitronics-core ships a transitional compat layer: a **real CMake target `teq_core`** (`teq::core` alias) and `<teq/*.h>` re-export shims, so `#include <teq/EqEngine.h>` / `teq::EqEngine` still compile.
- Proof of parity: the **full teq test-suite (596 checks) passes verbatim** through the shims in felitronics-core (`ctest`), so the move is behaviour-identical.
- TabbyEQ's adapter (`src/PluginProcessor.cpp` etc.) uses only `teq::*` → unchanged by this task.

## Steps
1. **Branch** off `main` (never work on `main`).
2. In `tabby-eq/CMakeLists.txt`, replace the local core build:
   - remove `set(TEQ_BUILD_TESTS ON)` and `add_subdirectory(teq)`;
   - add, near the other `FetchContent` blocks (JUCE-free → no JUCE pulled):
     ```cmake
     set(TABBYEQ_FCORE_TAG "<pinned-tag>" CACHE STRING "felitronics-core tag")
     FetchContent_Declare(felitronics_core
         GIT_REPOSITORY <felitronics-core repo URL>
         GIT_TAG        ${TABBYEQ_FCORE_TAG}
         GIT_SHALLOW    TRUE)
     FetchContent_MakeAvailable(felitronics_core)
     ```
   - keep `target_link_libraries(TabbyEQ PRIVATE teq::core ...)` **unchanged** (compat alias). Optionally also link `felitronics::dynamics` when Phase-2 dynamics lands (the `dynamics` module now lives in felitronics-core, NOT "inside `teq/`" as `docs/DYNAMICS.md` predates the split — note that in DYNAMICS.md).
3. **Decide the local `teq/` folder's fate** (flag to the owner if unsure): the source now lives upstream. Recommended once the build is green: delete `tabby-eq/teq/` (the 596-check upstream suite already guards parity). Until then it is dead code (no longer `add_subdirectory`'d).
4. If a core-only CI job built `teq` tests, repoint it to build felitronics-core with `-DFELITRONICS_BUILD_TESTS=ON` and `ctest`.

## Acceptance (must all pass)
- Configure + build **VST3 + AU + Standalone + CLAP** (the `-DTABBYEQ_BUILD_PLUGIN=ON` default).
- `auval -v aufx TbEq Dcat` passes.
- A spot A/B: load a session, confirm identical sound (same code upstream → expect bit-identical).
- `main` stays releasable; land via PR.

## Verify
```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
auval -v aufx TbEq Dcat
```

## Notes / pitfalls
- felitronics-core's root CMake is JUCE-free and configures all modules fast — fetch the **root** (no `SOURCE_SUBDIR` needed).
- Do not also keep the old local `teq/` on the include path simultaneously with the fetched one — two copies of the headers is the only real ODR/include trap here.
