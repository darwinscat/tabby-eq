<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# TabbyEQ — Per-band Stereo ↔ M/S dual-mode (design + plan)

> Started 2026-06-28. Decided after a design panel (Codex + DeepSeek, both independently
> recommended **Architecture A**). Inspiration: **Steinberg Frequency 2** (per-band ST / M-S split).

## Goal

Each band can be **Stereo (ST)** or **Mid/Side (M/S)**. In M/S a band carries **two independent
lanes — Mid and Side** — each with its **own** full set: type / freq / Q / gain / slope / on /
bypass (and **later dynamics**). On the graph the band shows **two nodes `N M` and `N S`**; the
floating toolbar gets **Mid | Side tabs**. This is the Frequency-2 model, but with **fully
independent frequency** per lane (Frequency 2 shares the freq; we give more — the "expensive" model).

## Locked decisions

- **Architecture A — dual-design per band.** One band = one logical node (pool stays **24**). A band
  holds a mode + a **Mid/main lane** (the existing params) + a **Side lane** (new params). `EqBand`
  runs **two matched designs** in M/S (Mid filter on the Mid signal, Side filter on the Side signal),
  with a **local per-band M/S encode→filter→decode** (exact; composes in series; no global pass).
- **`route` is removed** (per-band L/R/M/S routing). Replaced by **`ms` mode** (Stereo | M/S).
  *(Why no L/R: no convincing track scenario; Side covers it. Re-add later if ever needed — engine
  could.)*
- **Frequency independent** per lane (type/freq/Q/gain all independent). A fresh M/S split starts
  with Side at the Mid's freq; an optional **freq-link** toggle is a later nicety.
- **Minimum-phase** M/S (no linear-phase; deferred). Acceptable for a TRACK EQ (Pro-Q3 default is
  min-phase M/S). Honest positioning: not mastering linear-phase. Add mono/side audition; a
  correlation meter + a global linear-phase mode are later.
- **Dynamics later attach per lane** (Mid dyn + Side dyn). A is the architecture that makes this
  natural — a key reason it won.

## Panel consensus (Codex + DeepSeek)

- Both: **A**, keep 24 logical bands; do NOT fake it with linked pairs (B leaks: 2 slots, lifecycle,
  automation, undo, copy/paste, helper confusion).
- Freq **independent** (+ optional link default). Min-phase **OK** for a track EQ; watch asymmetric
  Mid/Side moves (image/mono colour). UX: **two sub-nodes** + Mid/Side tabs; show Mid/Side overlay
  curves **only for the selected band** (global = composite, else 24 bands = clutter).
- Param growth (~+side lane) is fine for VST3; use **stable IDs**, clear names (`B3 Side Gain`),
  migrate old Stereo → Mid lane, init Side from Mid on first switch.
- Watch: **mono-sum** (Side=0 on mono → fall back to Mid-only/Stereo + indicate); composite/analyzer
  meaning under M/S; **Helper operates on logical bands**, not lanes.
- Top risks: (1) state/param/migration complexity — design IDs + migration carefully now;
  (2) UI clutter + CPU (up to 2 filters/band → 48 — profile).

## Data model — `teq::BandParams` (additive)

Keep the existing flat fields as the **Mid/main lane**. Add:

```
bool       ms      = false;          // M/S dual-mode (false = Stereo)
bool       sOn     = true;           // Side lane enabled
FilterType sType   = Bell;           // --- Side lane (used only when ms) ---
double     sFreq   = 1000.0;
double     sQ      = 1.0;
double     sGainDb = 0.0;
int        sSlope  = 12;
bool       sBypass = false;
```

- `route` is **removed** in Phase 1b (engine keeps it during 1a only so the plugin keeps building).
- `swept` stays a band-level (Mid/main) flag (internal search). Side lane is always matched (no swept).
- `operator==` compares `ms`, `sOn`, `sType`, `sSlope`, `sBypass` and the bit-patterns of
  `sFreq/sQ/sGainDb` (recompute-skip stays exact).

## APVTS schema — `src/Parameters.cpp` (Phase 1b)

Per band, **remove** `bandN_route`; **add**:

```
bandN_ms       AudioParameterBool   "B{n} M/S"        default false
bandN_sOn      AudioParameterBool   "B{n} Side On"    default true
bandN_sType    AudioParameterChoice "B{n} Side Type"  (same 9 items)   default 0
bandN_sFreq    AudioParameterFloat  "B{n} Side Freq"  (20..20k skew1k) default 1000   "Hz" string
bandN_sQ       AudioParameterFloat  "B{n} Side Q"     (0.05..40 skew1) default 1      0.1 string
bandN_sGain    AudioParameterFloat  "B{n} Side Gain"  (-24..24)        default 0      0.1 " dB"
bandN_sSlope   AudioParameterChoice "B{n} Side Slope" (7 items)        default 1
```

- **Stable IDs**, never reused. Migration: old states carry an orphan `bandN_route` (ignored); new
  `ms`/side params default → a loaded old session behaves exactly as before (Stereo). No migration
  code needed; bump `stateVersion` to 2 as a marker.
- All side params automatable (clear names). Dynamic `isAutomatable` per mode is unreliable across
  hosts → skip; revisit if the list feels bloated.

## Engine — `teq::EqBand` (Phase 1a)

- New state: side smoothers `sFreqS/sQS/sGainS`; `coeffsSide[kMaxSections]`, `designNside`,
  `lastMs`. `bq` columns: **col 0 = Mid/main, col 1 = Side** in M/S; per-channel in Stereo.
- `prepare/reset`: prepare + snap side smoothers.
- `setParams`: snap (first call) / set targets for side freq/Q/gain too; `type/slope/ms/sOn/...`
  apply next block.
- `updateCoeffs`: design Mid (existing). If `ms`: build a **side-view** BandParams (copy `s*` into the
  design fields), `designBand(...)` → `coeffsSide/designNside`, push to `bq[*][1]`. **Reset on any
  topology change**: `designN`, `designNside`, `swept`, **or `ms`** changed.
- `processBlock`:
  - `midActive = on && !bypass`;  `sideActive = on && ms && sOn && !sBypass`;
    `active = midActive || sideActive` (so a bypassed Mid still lets Side run).
  - **M/S path** (`ms && nc==2`), per sample (exact local M/S):
    `m=(L+R)/2; s=(L-R)/2;` filter `m` through Mid sections (col 0) if `midActive`; filter `s`
    through Side sections (col 1) if `sideActive`; `L=m+s; R=m-s;`
  - **else** (Stereo / mono / surround): the existing per-channel path (swept SVF or matched), Mid
    lane only. (Phase 1a leaves the old `route` delta-fold here; Phase 1b deletes it.)
  - `flushState()` covers both columns.
- **Mono / surround:** `ms && nc!=2` → never enters the M/S path → Mid lane on all channels (Side
  silently inactive). The UI indicates "Side does nothing on mono".

## GUI — `src/ui/*` (Phase 2)

- Mode control on the toolbar: **`ST ↔ M·S`** (replaces the route button). When `ms`:
  - **Two nodes** `N M` and `N S` on the graph (distinct look: e.g. M filled, S ringed + tint),
    both draggable. Selecting either shows its lane in the toolbar; a **Mid | Side tab** jumps to the
    twin. Hotkeys/whisker act on the focused lane.
  - **Composite curves (correct + cheap):** a **Mid composite** = ∏(Stereo-band H)·∏(M/S-band Mid H)
    and a **Side composite** = ∏(Stereo-band H)·∏(M/S-band Side H). Draw **both** only when any band
    is M/S (else they're identical → draw one). A Stereo band contributes to BOTH (it filters L&R ==
    M&S equally). Selected band: overlay its Mid/Side lane curves (thin/tinted).
  - Node Y: Stereo node on the (single) composite; Mid node on Mid composite, Side node on Side
    composite.
- `readBand` (processor) fills `ms` + side fields; `BandEditStrip`/`EqCurveDisplay` lose all `route`
  references (badge, menu, `routeFromChoice`).

## Tests (felitronics-core `eq` suites)

Replace the old per-band routing test with **M/S dual-mode**:
- Stereo mode unchanged (existing tests still green).
- M/S: a Mid-only band changes the **Mid** content only (Side `L−R` preserved); a Side-only band
  changes **Side** only (Mid `L+R` preserved); **independent** Mid vs Side freq/type/gain measurable.
- Mid bypass + Side active still processes Side (and vice-versa).
- Mode switch ST↔M/S resets state (no click/tail).
- Stability across freq/Q/gain incl. Nyquist for both lanes.
- Mono input + `ms` → Mid lane applies to the mono channel; no NaN.

## Phasing

1. **1a — engine + DSP tests (in felitronics-core, JUCE-free).** Add `ms`+Side to `BandParams`, dual-design in
   `EqBand`, new tests. Plugin still builds (ms defaults false; route path untouched). ← start here.
2. **1b — schema + processor.** Add `bandN_ms` + side params, remove `bandN_route`; `readBand` fills
   them; delete the route delta-fold from `EqBand`; strip route from the UI. Plugin builds + auval.
3. **2 — M/S UI.** Mode toggle, two nodes, Mid/Side tabs, Mid/Side composite curves, mono indicator.
4. **3 — later.** Per-lane dynamics, linear-phase global mode, analyzer domain (Stereo/Mid/Side),
   freq-link toggle, correlation meter.

## Risks + mitigations

- **State/param/migration** — stable IDs from day one; old `route` orphaned harmlessly; `stateVersion`
  → 2. Init Side from Mid on first ST→M/S switch (UI does this).
- **UI clutter + CPU** — global = composite only; Mid/Side overlays for the selected band only.
  Two filters/band only when `ms`; profile 24×M/S. The matched design is cheap; recompute-skip when
  settled keeps idle bands free.
- **Mono** — explicit fallback + indicator.
