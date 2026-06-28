# teq — matched parametric-EQ DSP core

A small, **header-only, JUCE-free** C++20 module: a reusable parametric-EQ engine you can drop
into any plugin without extraction. Built for TabbyEQ (Felitronics / Darwin's Cat) but framework-
agnostic — it processes raw float buffers and owns no parameter system.

## What's inside (`include/teq/`)

| Header | Role |
|---|---|
| `MatchedBiquad.h` | Vicanek **matched** coefficient design — bell, low/high-pass, band-pass, low/high **shelf** (2-pole Butterworth). Matches the analog magnitude near Nyquist (no cookbook cramping). + a TDF-II `Biquad`. |
| `Svf.h` | Cytomic/Zavalishin **TPT state-variable** filter — for swept "search" bands (clean under fast fc modulation). |
| `Smoother.h` | one-pole parameter smoother (closed-form `advance(n)`). |
| `EqBand.h` | one band: matched biquad(s) for static bands, SVF for swept; smoothing, 12/24 dB/oct, stereo. |
| `EqEngine.h` | up to 12 bands in series + `magnitudeDb()` GUI readout + pre/post `SpectrumTap`. |
| `SpectrumTap.h` | lock-free SPSC analyser tap (`push` on audio thread, `tryPull` on GUI). |
| `EqTypes.h` | `FilterType`, `BandParams`. |

## Use it

Link the namespaced target `teq::core` (a typo then fails at configure time, not at link):

```cmake
# Local — a vendored copy or sibling checkout:
add_subdirectory(path/to/teq)
target_link_libraries(app PRIVATE teq::core)
```
```cmake
# Public repo — pin by tag. SOURCE_SUBDIR configures only teq/, never the JUCE-pulling root:
include(FetchContent)
FetchContent_Declare(teq
    GIT_REPOSITORY <repo-url>
    GIT_TAG        v0.1.0
    SOURCE_SUBDIR  teq)
FetchContent_MakeAvailable(teq)
target_link_libraries(app PRIVATE teq::core)
```

Entry point: `#include <teq/EqEngine.h>` (pulls in `BandParams` / `EqBand` transitively).

```cpp
#include <teq/EqEngine.h>

teq::EqEngine eq;
eq.prepare (sampleRate, maxBlock, numChannels);   // numChannels ≤ 2

teq::BandParams b;                                // on the audio thread (see Threading):
b.on = true; b.type = teq::FilterType::Bell; b.freq = 1000.0; b.Q = 1.4; b.gainDb = -3.0;
eq.setBand (0, b);

eq.process (channelPointers, numChannels, numSamples);   // in place
double curveDb = eq.magnitudeDb (1000.0);                // for the GUI
```

## Contract

- **Threading.** `prepare()` / `setBand()` / `process()` run on the **same thread** (typically the
  audio thread, where the host adapter reads its atomic params and calls `setBand()` at the top of
  `process()`), **or** you synchronise externally. The engine takes **no internal lock**.
  `setSpectrumActive()` is safe from any thread (atomic). `SpectrumTap.push` is audio-thread,
  `SpectrumTap.tryPull` is GUI-thread (lock-free SPSC).
- **Realtime.** `process()` does **no allocation / lock / IO / throw**. Coefficients recompute only
  when a parameter is moving (settled bands skip the per-block transcendentals). The engine flushes
  denormals per block; still enable **FTZ/DAZ** on the audio thread (e.g. `juce::ScopedNoDenormals`).
- **Units & ranges** (`BandParams`). `freq` Hz (clamped to `[10, 0.49·fs]`); `Q` (clamped
  `[0.05, 40]`; ignored for shelves — they're Butterworth); `gainDb` (clamped `[-30, 30]`, bells &
  shelves); `slope` 12 dB/oct (uses Q) or 24 dB/oct (HP/LP; Butterworth, Q ignored); `swept` → SVF.
  Non-finite values are sanitised. The first `setParams()` per band **snaps** (no ramp from defaults
  on load); later changes smooth. Identical params are a no-op (cheap to call every block).
- **Preconditions.** Call `prepare()` before `process()`. Up to `teq::kMaxChannels` (16) channels —
  mono through 9.1.6 Atmos / 3rd-order ambisonics; the host clamps the live count to this.
- **GUI curve.** For a **race-free** curve use `EqEngine::magnitudeDbFor(params, n, freqHz, fs)` —
  it computes purely from a `BandParams` array the GUI owns (the member `magnitudeDb()` is a
  best-effort live readout that may briefly race a param change). Reports the matched (analog-honest)
  design; a swept band shows the *target* curve (live SVF differs only by a sub-dB skirt near Nyquist).

## Test it

JUCE-free self-tests (audio is pushed through and measured against the analytic curve):
```sh
cmake -S teq -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Roadmap (not yet)

Notch / All-pass filter types; exact SVF response in `magnitudeDb()` for swept bands; `> 2` channels;
optional float-only fast path.

## License

AGPL-3.0-or-later. The matched-filter method is from Martin Vicanek's papers (cited inline); no
third-party code is copied.
