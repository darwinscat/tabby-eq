<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE. -->

# TabbyEQ — Dynamic EQ + de-esser (design)

**Status:** draft / planning · **Depends on:** `felitronics-core` DSP-ARCHITECTURE.md (this lands the
**first new module** `dynamics/`, software-denormal-flush per Law 8). · **Started:** 2026-06-29.

The next big TabbyEQ feature: make **any band optionally dynamic** — its gain reacts to the level in
its own frequency region — plus a **de-esser** preset on the same engine, **per Mid/Side lane**, with
**gain-reduction (GR) metering** the semantic Helper will later read. Pro-Q3 / Neutron tier.

---

## 0. Where the code lives (per the architecture)

- **Core primitives → `dynamics/` module** (JUCE-free, RT-safe, software-flush): `EnvelopeFollower` +
  `GainComputer`. Pure: signal-in → gain-out. No EQ knowledge, no params system, no GUI. This is the
  first module of the new `felitronics-core` structure; **developed inside `teq/` for now** (where the
  EQ core lives) and migrated with the `eq` extraction later. It must obey every portability law —
  most pointedly **Law 8: the envelope/release followers flush denormal state in software** (teq's
  `|state| < 1e-15 → 0` per-block pattern), so it's WASM-safe without hardware FTZ.
- **Composition → TabbyEQ** (`teq::EqBand` integration + `src/` adapter + UI). The *dynamic EQ* (detector
  fed by the band's region, applying the computed gain through the band) is product glue, not a core
  primitive — exactly the "cross-module glue stays in the product" rule. A standalone compressor would
  reuse the same `dynamics/` broadband.

---

## 1. Goal & UX

- Each band gets an optional **dynamic** behaviour: a target gain it moves toward as its band level
  crosses a **threshold**, bounded by a **range**. Modes: **down-compress** (tame when loud — the
  common dynamic-EQ move), **up-compress** (lift when quiet), **down-expand** (gate-ish — duck when
  quiet). Resting state (no dynamics, or signal below/above threshold) = exactly today's static band.
- **De-esser** = a one-click preset: a dynamic band in the sibilance region (~5–9 kHz), down-compress,
  fast attack/release, detector on the band region. Nothing new in the engine — just preset params.
- **Per Mid/Side lane:** in M/S mode each lane has independent dynamics (the Side de-essing a wide
  vocal, the Mid taming a boomy centre, etc.). Sits on the existing M/S dual-design.
- **GR metering:** every dynamic band shows live gain reduction on its node/curve; exposed as atomics
  so the future Helper can read "how hard is this band working".

---

## 2. DSP design

### 2.1 Detector (sidechain)
- Per dynamic band+lane: an **internal band-pass probe** (a Cytomic SVF in band-pass at the band's
  `freq`/`Q`) tracks the energy in the band's region. Fed the band's **input** (the signal entering
  this band) so detection doesn't chase its own gain change.
- **EnvelopeFollower** (`dynamics/`): one-pole attack/release on `|probe|` (peak) or `probe²` (RMS,
  default for musical EQ dynamics). Time constants from `attack`/`release` ms. **Software denormal
  flush** of the envelope state every block (Law 8).
- Shelves: probe = band-pass at the corner. HP/LP dynamic deferred (v2) — their "region" is the whole
  stop/pass band; less useful and ambiguous.

### 2.2 Gain computer (`dynamics/`, dB domain)
`levelDb = 20·log10(env)`. Output: a signed **delta in dB**, clamped to `[−range, +range]`:
- **down-compress:** `over = max(0, levelDb − thr)`; `delta = −over·(1 − 1/ratio)`. (cut as it gets loud)
- **up-compress:** `under = max(0, thr − levelDb)`; `delta = +under·(1 − 1/ratio)`. (lift as it gets quiet)
- **down-expand:** `under = max(0, thr − levelDb)`; `delta = −under·(ratio − 1)`, clamped. (duck quiet)
- **Soft knee** (`knee` dB) smooths the threshold corner (quadratic blend).
- **Threshold modes:** absolute (dBFS) and **auto-relative** (threshold tracks a slow program-level
  estimate of the band, so it adapts to gain staging) — auto can be v2; ship absolute first.

### 2.3 Application — "matched static + SVF gain-delta"
The resting band stays a **matched biquad** (Nyquist-accurate static curve). The **dynamic delta** is a
**Cytomic SVF bell** at the same `freq`/`Q` in series, whose gain = the smoothed `delta` dB:
```
y = matchedStatic(x);            // base curve (user's set gain), matched/Nyquist-accurate
y = svfDelta(y, gain = delta);   // dynamic delta; delta = 0 → transparent
```
- At `delta = 0` the SVF is bit-transparent → a non-dynamic band is unchanged (zero cost beyond the
  detector, which is skipped when `dynOn` is false).
- SVF (not a re-matched biquad) because its gain is **cheap to modulate** sample-to-sample without a
  full redesign — that's the whole point of the split.
- Cadence: envelope + gain computer per sample; SVF delta-gain applied per sample (Cytomic bell gain
  update is a couple of cheap coeffs) — or per small sub-block if profiling demands. Smooth the delta
  to avoid zipper on fast transients.

### 2.4 Combined curve (for the GUI)
Resting curve = matched static (unchanged). The dynamic band draws a **shaded range** from the static
gain to `static ± range` (the envelope of where it can move), and the **live curve** shows the current
`static + delta`. Race-free, computed from the param snapshot + the GR atom (like the existing curve).

---

## 3. Per Mid/Side lane

`teq::EqBand` already runs a Mid lane (state col 0) + Side lane (col 1) in `ms` mode. Dynamics extends
each lane with its own detector + gain computer + SVF-delta, driven by that lane's signal (Mid = `(L+R)/2`,
Side = `(L−R)/2`). Non-M/S band = one (Mid) dynamics path on the per-channel signal. Mono/surround = Mid
lane only (existing rule).

---

## 4. De-esser preset
A button / menu action that configures the selected (or a new) band: high bell or high-shelf, `freq`
≈ 6–8 kHz, `Q` ≈ 3–4, `dynOn`, mode = down-compress, `ratio` ≈ 4:1, fast `attack` (~1 ms) / `release`
(~80 ms), `range` ≈ −12 dB, threshold auto-relative (or a sane absolute default). Pure params — the
engine doesn't know it's a "de-esser".

---

## 5. GR metering
Each band+lane exposes **current gain reduction** (dB, the live `delta`) via a lock-free atomic
(read-and-smooth on the UI side). Shown on the node (a small GR arc / the node riding its range band)
and reserved for the **Helper** ("this band is pumping 4 dB" → suggestions). One atom per band per lane.

---

## 6. Linear-phase interaction (hard constraint)
Dynamics is **time-varying** → it **cannot** be baked into the static linear-phase FIR (the FIR is a
fixed snapshot of the magnitude). Decision: **in Linear mode, dynamics is bypassed** (bands render their
static curve only) and the UI shows a hint ("Dynamics is inactive in Linear-phase mode"). No attempt to
convolve a moving target. (Future: a hybrid path could run dynamic bands in Natural and static bands in
Linear, but that's out of scope.)

---

## 7. Parameters / schema / state

Per band, per lane, add (APVTS, additive): `dynOn`, `dynThr` (dBFS), `dynRatio`, `dynAtk` (ms),
`dynRel` (ms), `dynRange` (dB, signed cap), `dynMode` (choice: down-comp / up-comp / down-expand),
optional `dynKnee`. Side lane mirrors with `sDyn*`. That's ~7 × 2 per band — APVTS handles it; group
them so the host parameter list stays scannable.

- **`kStateVersion → 3`** (additive; v2 sessions load with dynamics off = identical sound).
- `teq::BandParams` gains a `Dyn` sub-struct ×2 (Mid + Side), with `operator==` extended (bit-compared
  doubles, like the M/S fields) so the engine's recompute-skip stays exact.

---

## 8. RT-safety (the laws)
- Detector + gain computer + SVF-delta all run in `process()` — **no alloc / lock / IO** (Law 2).
- **Software-flush** the envelope + SVF-delta state every block (Law 8) — the new followers are exactly
  the "unguarded feedback kernels" the 3rd review warned about; they adopt teq's `<1e-15 → 0` pattern.
- Float hot path; `double` only for the dB log/accumulation if needed (Law 3 carve-out).
- A non-dynamic band pays **zero** dynamics cost (detector skipped when `dynOn` false).

---

## 9. UI
- **Node second handle:** drag from the static node up/down to set the dynamic **target** → defines
  `range` + direction (down/up). The band shows a translucent **range band** between static and target.
- **Live GR:** the node/curve animates to `static + delta`; a small GR read-out in the toolbar.
- **Toolbar dynamics row** (for the selected band/lane): power (dynOn), threshold, ratio, attack,
  release, mode — appears under the existing freq/Q/gain row.
- **De-esser** quick action (toolbar button or add-menu).
- Per-lane: the Mid|Side tabs already switch the edited lane → the dynamics row binds to that lane.

---

## 10. Tests (teq + integration)
- `GainComputer` static curve: input dB → delta dB matches the analytic compressor/expander law
  (threshold, ratio, knee) for all three modes; range clamp respected.
- `EnvelopeFollower`: attack/release reach 1−1/e in the specified ms; peak vs RMS; **no subnormals after
  a long silence** (denormal-flush proof, like teq's existing tests).
- Integration: a tone above threshold in a dynamic band converges to `static + delta` (measured gain ==
  analytic); below threshold == static; M/S per-lane independence (Mid dynamics doesn't move the Side
  axis); no-alloc-in-`process`.
- Linear mode: dynamics bypassed (output == static linear curve).

---

## 11. Phasing
1. **`dynamics/` core primitives** — `EnvelopeFollower` + `GainComputer` (JUCE-free, software-flush) + tests.
2. **Schema** — `BandParams` `Dyn` sub-struct ×2 + APVTS params + state v3.
3. **Engine integration** — `EqBand` dynamic path (detector probe + matched-static × SVF-delta), per lane.
4. **GR atomics + processor wiring** — expose per-band/lane GR; read params on the audio thread.
5. **UI** — node second handle + range band + live GR + toolbar dynamics row.
6. **De-esser preset.**
7. **Polish** — auto-relative threshold, soft knee tuning, Linear-mode bypass hint.

Ship desktop first (priority 1); the `dynamics/` module is JUCE-free so it rides to WASM/embedded later
for free.

---

## 12. Open questions
- Detector cadence: per-sample SVF-delta gain vs per-sub-block (profile on the 24-band worst case).
- Auto-relative threshold: v1 or v2? (lean v2 — ship absolute first.)
- Dynamic HP/LP: skip in v1 (region ambiguous).
- One detector per band vs a shared analysis bank (24 band-pass probes = cost; measure).
- External/sidechain input: out of scope v1 (internal detector only).
