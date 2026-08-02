<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE. -->

# TabbyEQ — Dynamic EQ + de-esser (design)

**Status:** design agreed · **Depends on:** `felitronics-core` DSP-ARCHITECTURE.md (this lands the
**first `dynamics/` module**, software-denormal-flush per Law 8). · **Started:** 2026-06-29.

> **Rev 2 (2026-08-02) — the FabFilter model, agreed with Oleh in-session.** Rev 1 was written
> before `LANES.md` shipped and was, in substance, the **Nova GE school**: seven explicit knobs per
> band (threshold / ratio / attack / release / range / mode / knee), full manual control, dynamics
> declared "per Mid/Side lane". Two things killed it: (a) against the shipped 5-lane model it reads
> as 7 × 5 × 24 = **840 new host parameters**, doubling the existing ~820; (b) it contradicts
> the product's own UX tier — Pro-Q 4's dynamics is **one knob in the default case**, because
> threshold is auto-relative and attack / release / knee are program-dependent. Rev 2 takes the
> FabFilter model on both axes: **auto-first parameters** and **dynamics belongs to the point, not
> the lane** (per-lane dynamics is reached by fission, exactly as Pro-Q's Split button reaches
> per-channel bands). Result: **5 parameters × 24 bands = 120**.

The next big TabbyEQ feature: make **any point optionally dynamic** — its gain reacts to the level
in its own frequency region — plus a **de-esser** preset on the same engine, with **gain-reduction
(GR) metering** the semantic Helper will later read. Pro-Q / Neutron tier.

---

## 0. Where the code lives (per the architecture)

- **Core primitives → `dynamics/` module** (JUCE-free, RT-safe, software-flush): `EnvelopeFollower`
  + `GainComputer`. Pure: signal-in → gain-out. No EQ knowledge, no params system, no GUI. It must
  obey every portability law — most pointedly **Law 8: the envelope / release followers flush
  denormal state in software** (the `|state| < 1e-15 → 0` per-block pattern), so it is WASM-safe
  without hardware FTZ.
- **Composition → TabbyEQ** (`felitronics::eq::EqBand` integration + `src/` adapter + UI). The
  *dynamic EQ* (a detector fed by the lane's region, applying the computed gain through that lane)
  is product glue, not a core primitive — the "cross-module glue stays in the product" rule. A
  standalone compressor would reuse the same `dynamics/` primitives broadband.
- Core is at **v0.13.1**; tabby pins **v0.12.0**. The `dynamics/` module lands as a core minor bump
  and the pin moves with phase 1.

---

## 1. The two decisions that shape everything

### 1.1 Dynamics belongs to the POINT (not the lane)

A point already shares **one filter type** across its lanes (`LANES.md` decision #2). Dynamics
joins it: **one dynamics setting per point**, shared by every running lane (enabled and not
bypassed — § 2.1 pins the term).

- **Each running lane still gets its own detector and its own delta.** The lanes have independent
  `freq`/`Q`, so each probes its own region, of its own signal (Mid lane detects Mid energy, Side
  lane detects Side energy) and moves its own gain. What they share is the *settings*: range,
  threshold, attack, release.
- **Different dynamics on Mid vs Side is reached by fission** — split the point, exactly the move
  `LANES.md` already implements for `sType ≠ type` migration, and exactly what Pro-Q's **Split**
  button does for L/R. Two points, two independent dynamics.
- This is what the industry does. Pro-Q 3/4 has one stereo placement per band; Neutron has one
  channel mode per node; Nova GE has no per-band M/S at all. Nobody carries dynamics across a
  lane set, because nobody has a lane set.

**Why shared settings are not a compromise here:** the auto-relative threshold (§ 2.3) is what
makes them work. An *absolute* dBFS threshold shared between a Mid lane and a Side lane would be
meaningless — those signals sit at completely different levels. An auto-relative threshold tracks
each lane's own program level, so one setting genuinely means the same thing in both domains. The
two halves of this revision are not independent: **auto-first parameters are what make point-level
dynamics correct**, not merely cheap. Rev 1's explicit dBFS threshold could not have been shared
across lanes at any parameter count.

### 1.2 Auto-first parameters

The default dynamic point exposes **one number the user actually sets: Dynamic Range.** Everything
else has a defined automatic behaviour and is only *deviated from*, never dialled from scratch.

| | Rev 1 (Nova school) | **Rev 2 (FabFilter model)** |
|---|---|---|
| Range | explicit | **explicit — the one primary knob**, dragged off the node |
| Threshold | explicit dBFS | auto-relative by default; slider top = `A` |
| Attack / Release | explicit ms | **deviation** from auto: centre 50 % = auto, lower/higher = faster/slower |
| Ratio | explicit | gone — implied by the transfer law (§ 2.3) |
| Knee | explicit | gone — automatic, derived from range |
| Mode | 3-way choice | gone — direction is the **sign of range** (§ 2.3, down-expand deferred) |
| **Per band** | **7 × 5 lanes = 35** | **5** |
| **Total (24 bands)** | **840** | **120** |

> The auto laws in § 2 are **our own** formulation in the spirit of the FabFilter model —
> FabFilter's actual algorithm is closed. They are written out concretely so they are implementable
> and testable; they are not reverse-engineering claims.

---

## 2. DSP design

### 2.1 Detector (sidechain)

- Per **running lane** of a dynamic point — `lane.on && !lane.bypass`, which `LANES.md` calls an
  *active* lane; **not** the UI's `activeLane` property (the lane currently being edited). Each
  gets an internal **band-pass probe** (a Cytomic SVF in band-pass at that lane's `freq` / `Q`)
  tracking the energy in the lane's region, fed the lane's **input** — the signal entering this
  lane — so detection does not chase its own gain change.
- `EnvelopeFollower` (`dynamics/`): one-pole attack / release on `probe²` (RMS; peak available for
  the tests). **Software denormal flush** of the envelope state every block (Law 8).
- Shelves: probe = band-pass at the corner.
- **Dynamics exists exactly where gain does** — `eqview::handles::hasGain(t)`, already the single
  source of truth in `src/eqview/HandleMath.h`: **Bell, Low/High Shelf, Tilt**. The other five
  types (HP, LP, BandPass, Notch, All-pass) have no gain for the delta to modulate, and their
  "region" is a whole stop/pass band anyway. The UI hides the dynamics affordance for them; the
  engine ignores `dyn_on` for them, so a type switch away from Bell cannot leave dynamics acting
  invisibly. (The parameters still exist on every point — a host's parameter list may not change
  shape at runtime; they are simply inert, exactly like `gain` is today for those types.)
- **Idle lanes cost zero**, by the same construction as `LANES.md`: a disabled lane has no probe
  and no follower running. A non-dynamic point pays nothing at all (`dyn_on` false → the whole path
  is skipped).

### 2.2 Auto time constants (program-dependent)

Attack and release are derived from what the band actually is, then modulated by the deviation
knobs. The frequency term is the important one: a band's region cannot be tracked faster than its
own period, and this is exactly what makes a de-esser fast and a low-mid tamer slow **without the
user setting anything**.

```
rawAttackMs   = kA * 1000 / fc                          // kA ≈ 3 periods of the centre frequency
widened       = rawAttackMs * (1 + log2 (Q) / 8)        // width term, below
baseAttackMs  = clamp (widened,  1.0, 50.0)
baseReleaseMs = clamp (kR * baseAttackMs, 20.0, 800.0)  // kR ≈ 12
```

- At 120 Hz (Q 1): attack = 25 ms, release = 300 ms — musical low-end control, for free.
- At 8 kHz (Q 1): the raw term is 0.375 ms, so the **1 ms rail** takes over → attack = 1 ms,
  release = 20 ms (its own rail) — de-esser behaviour, for free.
- **The rails are load-bearing, not cosmetic:** with `kA = 3` the frequency term only moves the
  attack between ≈ 60 Hz and ≈ 3 kHz; above 3 kHz every band gets the 1 ms floor, below 60 Hz the
  50 ms ceiling. That is intended — 1 ms is already ~8 periods at 8 kHz, and chasing individual
  cycles is neither useful nor stable — but it means the audible tuning above 3 kHz lives in the
  floor constant, and that is where ear-tuning effort will go.
- **Width term:** a narrow (high-Q) band gets a longer attack — its probe rings longer, so a faster
  follower would just track the ringing. Applied **before** the clamp, so it cannot push a band
  through the rails.
- **Note on the release ceiling:** with `kA = 3` / `kR = 12` the attack clamp caps `baseReleaseMs`
  at 12 × 50 = 600 ms, so the 800 ms rail never actually engages — it is a guard for future
  constant tuning, not live behaviour. Called out so nobody later "fixes" a rail that was never
  reached.
- **Program dependence (release):** two parallel release states, fast (`baseReleaseMs`) and slow
  (`4 × baseReleaseMs`); the applied release is the **slower-recovering of the two** (classic dual
  time-constant). Short transients recover quickly; sustained energy releases smoothly, without a
  ratio knob to blame.
- **Deviation knobs:** `atk` / `rel` ∈ [0, 1], 0.5 = auto. Multiplier = `2^((x − 0.5) * 4)` — 0.5 →
  ×1, 0 → ×1/4 (four times faster), 1 → ×4 (four times slower). Continuous, monotonic, and "centre
  = auto" is a visible detent in the UI.

### 2.3 Gain computer — one transfer law, no ratio, no knee

`levelDb = 10·log10(env)` — `env` is the *mean square* (the follower runs on `probe²`, § 2.1), hence
`10·`, not `20·`. The threshold:

- **Auto (default):** a slow running estimate of the lane's own band-limited program level (a
  long-time-constant follower on the same probe, ≈ 1–2 s), plus a fixed offset. The threshold then
  sits just above "normal" for this band and adapts to gain staging by construction.
- **Manual:** an absolute dBFS value. The slider's top position *is* auto (displayed `A`), the
  Pro-Q affordance — and the live trigger level is drawn inside the slider so a manual threshold is
  set by eye, not by guesswork.

The delta is then a single smooth law, asymptotic in `range`:

```
over  = max (0, levelDb − thresholdDb)
delta = range * (1 − exp (−over / tau)),     tau = |range| / s0,   s0 ≈ 0.75
```

- **Bounded by construction:** `|delta| < |range|` for all inputs — no clamp, no discontinuity.
- **Ratio is implied:** the slope at `over = 0` is `range / tau = s0 · sign(range)`, and a classic
  ratio `R` is exactly a slope of `1/R − 1`, so `s0 = 0.75` ⇔ **4:1** at onset. It then softens
  continuously toward the range limit as it compresses harder — an infinite ratio is never reached
  because `delta` is asymptotic, which is also why no limiter-style discontinuity exists.
- **Knee is implied:** the exponential *is* the knee, and it scales with range automatically — a
  small range gets a gentle corner, a large one a defined onset.
- **Direction is the sign of `range`:** negative = the band pulls down as its region gets loud
  (**downward compression**, the common move); positive = the band pushes up as its region gets
  loud (**upward expansion**). The classic "lift when quiet" is approximated by a **static boost
  with a negative range** — the band rests boosted and gives the boost back once the region gets
  loud. Not literally upward compression (that reacts to level *falling below* the threshold; this
  reacts to level *crossing above* it), but the audible move is the same and it costs no parameter.
- **Down-expand / gating is deferred** (§ 12): it reacts to the *absence* of level and is the one
  behaviour this law does not cover. Nova GE has it, Pro-Q does not; it costs a mode parameter, so
  it waits for demand.

`delta` is smoothed (the existing `Smoother`) before it reaches the filter, so nothing zippers.

### 2.4 Application — "matched static + SVF gain-delta"

Unchanged from Rev 1, and still the right split. The resting lane stays a **matched biquad**
(Nyquist-accurate static curve); the **dynamic delta** is a **Cytomic SVF bell** at the same
`freq` / `Q` in series, whose gain is the smoothed `delta` dB:

```
y = matchedStatic (x);            // base curve (the user's set gain), matched / Nyquist-accurate
y = svfDelta (y, gain = delta);   // dynamic delta; delta = 0 → transparent
```

- At `delta = 0` the SVF is transparent → a non-dynamic lane is unchanged, zero cost beyond a
  detector that is itself skipped when `dyn_on` is false.
- SVF rather than a re-matched biquad because its gain is **cheap to modulate** sample-to-sample
  without a full redesign — that is the entire point of the split.
- Cadence: envelope + gain computer per sample; the SVF delta-gain update is a couple of cheap
  coefficients. Profile the 24-band × 5-lane worst case and drop to per-sub-block gain updates only
  if it demands (§ 12).
- **Placement in the lane chain is normative** and follows `LANES.md`'s processing order: the delta
  SVF sits immediately after that lane's static sections, **inside** the lane. For the M/S lanes
  that means the delta is part of `filt()` — the fold stays `dM = filt(m) − m` with
  `filt = static ∘ svfDelta`, so a lane whose delta is 0 still leaves its axis bit-exact and the
  shipped delta-fold identity tests keep passing verbatim.

### 2.5 Combined curve (for the GUI)

The resting curve is the matched static composite, unchanged. A dynamic point draws a translucent
**range band** from the static gain to `static + range` (where it can move) and a **live curve** at
`static + delta`, per lane, computed race-free from the param snapshot + the GR atom — the same
discipline as the existing composite.

---

## 3. FIR-mode interaction (hard constraint)

Dynamics is **time-varying** → it **cannot** be baked into a static FIR (the FIR is a fixed
snapshot of the magnitude). Decision: **in Linear and Natural phase modes, dynamics is bypassed**
(lanes render their static curve only) and the UI shows a hint. No attempt to convolve a moving
target.

> Rev 1 said "Linear mode" only. Natural phase rides the same `MixedPhaseFir` IR-swap engine, so it
> is under the identical constraint — the omission was a Rev 1 bug, not a design choice.

A hybrid path (dynamic points in Zero-Latency, static points in FIR) is out of scope.

---

## 4. De-esser preset

A one-click action configuring the selected (or a new) point — and the parameter model is what makes
it trivial: past placing the bell itself, **the dynamics half is two values**, `dyn_on` and
`dyn_range`. Everything else is auto and stays auto.

| | |
|---|---|
| type / freq / Q | high bell, ≈ 7 kHz, Q ≈ 3.5 |
| `dyn_on` | true |
| `dyn_range` | **−12 dB** |
| `dyn_thr` | **auto** (the default) |
| `dyn_atk` / `dyn_rel` | **auto** (0.5) — and auto at 7 kHz already means 1 ms / 20 ms (§ 2.2) |

Pure parameters; the engine does not know it is a "de-esser". Under Rev 1's model the same preset
also had to assert ratio, knee, mode and explicit attack / release / threshold in ms and dBFS —
magic numbers a user cannot sanity-check, and wrong the moment the input gain changes.

---

## 5. GR metering

Each **lane** of a dynamic point exposes its current gain reduction (dB, the live `delta`) via a
lock-free atomic, read and smoothed on the UI side. Shown on the node (the node riding its range
band + a small GR arc) and reserved for the **Helper** ("this point is pumping 4 dB" → suggestions).
One atom per band per lane (24 × 5), sized as today's per-band atoms.

---

## 6. Parameters / schema / state

Per **point** `b` (not per lane), additive to schema v3, inside the band's existing
`AudioProcessorParameterGroup`:

```
band{b}_dyn_on     Bool    "B{n} Dyn"            default false
band{b}_dyn_range  Float   "B{n} Dyn Range"      −24..+24 dB, default 0     ← the primary control
band{b}_dyn_thr    Float   "B{n} Dyn Thr"        −60..0 dBFS, top value = Auto, default Auto
band{b}_dyn_atk    Float   "B{n} Dyn Attack"     0..1, default 0.5 (= auto)
band{b}_dyn_rel    Float   "B{n} Dyn Release"    0..1, default 0.5 (= auto)
```

**120 new parameters** (~940 total). Auto threshold is the parameter's **top value**, not a
companion bool — so it is automatable, and it costs only the 0 dBFS setting, which is not a useful
manual threshold anyway. The adapter decodes that one float into the struct's `thrDb` + `thrAuto`
pair; the core never sees a sentinel.

- **`stateVersion` → 4** (additive; v3 sessions load with dynamics off = bit-identical sound).
- `felitronics::eq::BandParams` v3 gains a **point-level** `Dyn` sub-struct (`on`, `range`, `thrDb`,
  `thrAuto`, `atk`, `rel`) — *not* per-lane, matching decision 1.1 — with `operator==` extended
  (bitwise doubles, as today) so the engine's recompute-skip stays exact.
- Non-parameter state: none. Dynamics adds no `ValueTree` properties.

---

## 7. RT-safety (the laws)

- Detector + gain computer + SVF-delta all run in `process()` — **no alloc / lock / IO** (Law 2).
- **Software-flush** the envelope, the slow program-level estimate and the SVF-delta state every
  block (Law 8) — these followers are exactly the "unguarded feedback kernels" the third core
  review warned about; they adopt the `< 1e-15 → 0` pattern.
- Float hot path; `double` only for the dB log / accumulation (Law 3 carve-out).
- A non-dynamic point pays **zero** dynamics cost; an idle lane of a dynamic point pays zero
  (no probe, no follower) — the `LANES.md` cost-zero construction, extended.

---

## 8. UI

- **Primary gesture:** drag the **dynamic handle** off the node to set `range` — the range band
  grows from the node as you drag, direction included. That is the whole default interaction.
- **Live GR:** the node rides between static and `static + delta`; per-lane, so a split point shows
  its lanes reacting independently.
- **Expand for manual control:** the floating point-toolbar grows a dynamics row — `Dyn` power,
  range, threshold (with the live trigger level drawn in the slider, top = `A`), attack, release —
  revealed by an expand affordance, closed by default. Attack / release show their **resolved
  ms value** next to the deviation knob, so "auto" is never opaque: the user sees that a 7 kHz band
  chose 1 ms.
- **De-esser** quick action in the add / right-click menu.
- Dynamics affordances are **hidden for the five gainless types** (§ 2.1 — the `hasGain` predicate)
  and **greyed with a hint in Natural / Linear phase modes** (§ 3).

---

## 9. Tests

**Core (`felitronics_dynamics_tests` + `felitronics_eq_tests`):**

1. `GainComputer` transfer law: input dB → delta dB matches `range·(1−exp(−over/tau))` analytically;
   `|delta| < |range|` for extreme inputs; `|slope|` at `over = 0` == `s0` for both signs of range;
   `delta == 0` exactly at and below threshold (no leak into the static case).
2. `EnvelopeFollower`: attack / release reach 1 − 1/e in the specified ms; the dual release
   recovers fast after a transient and slow after sustained level; **no subnormals after a long
   silence** (denormal-flush proof, as the existing suites do).
3. Auto time constants: `baseAttackMs`/`baseReleaseMs` monotone in `fc`, clamped at the rails;
   the deviation multiplier is exactly ×1 at 0.5 and ×1/4 / ×4 at the ends.
4. Auto threshold: after a level step, the threshold converges to programLevel + offset within the
   specified window; a 20 dB input-gain change leaves the *delta* trajectory unchanged (the whole
   point of auto-relative) — the test that proves shared settings work across lanes.
5. Integration: a tone above threshold in a dynamic point converges to `static + delta` (measured
   gain == analytic); below threshold == static exactly; **per-lane independence** (a dynamic
   `{m,s}` point driven by a Mid-only signal moves the Mid lane and leaves the Side axis bit-exact);
   dynamic `{st}` point on a 6-channel bus behaves per the ST-only rule; no-alloc-in-`process`.
6. Gating: `dyn_on` is inert on the five gainless types (output bit-identical to the static band,
   including after a Bell → Notch type switch with dynamics left on); dynamics bypassed in Natural
   and Linear (output == the static FIR response).
7. **Adapter:** v3→v4 migration (dynamics off, output bit-identical to the v3 reference); display-name
   uniqueness across all ~940; group layout sanity; auval in CI as today.

---

## 10. Phasing (each PR: build all formats + ctest + auval + crew adversarial review)

1. **fcore `dynamics/`** — `EnvelopeFollower` + `GainComputer` + the auto-time-constant and
   auto-threshold helpers, JUCE-free, software-flush, tests 1–4 → core minor tag.
2. **fcore `eq` integration** — `BandParams` v3 `Dyn` sub-struct, per-lane probe + delta SVF in
   `EqBand`, GR atomics, tests 5–6. Pin bump (v0.12.0 → the new tag; picks up v0.13.x on the way).
3. **tabby adapter** — 120 params + state v4 + migration + `readBand`, mechanical UI so everything
   compiles and auval passes. `main` stays green.
4. **tabby UI** — dynamic handle + range band + live GR + the expandable dynamics row + resolved-ms
   readouts + the type/phase-mode gating.
5. **De-esser preset.**

Ship desktop first; `dynamics/` is JUCE-free so it rides to WASM / embedded later for free.

---

## 11. Risks

- **CPU:** worst case 24 points × 5 active lanes = 120 probes + followers + delta SVFs. Mitigated by
  construction (non-dynamic points and idle lanes are free) but **unproven** — profile a
  deliberately hostile session in phase 2 before the UI lands. This is the one number that could
  force per-sub-block gain updates.
- **Auto behaviour is a taste problem, not a correctness problem.** The constants in § 2.2–2.3
  (`kA`, `kR`, `s0`, the threshold offset and its time constant) are first estimates; they will be
  tuned by ear on real material, and the tests are written to pin the *laws*, not the constants.
- **Shared settings across lanes** rest entirely on auto-relative threshold working well. If test 4
  disappoints on real material, the fallback is to make **threshold alone** per-lane: 5 × 24 = 120
  lane thresholds replacing the 24 point-level ones, so **+96 parameters**, and range / attack /
  release stay shared. Not a redesign — but worth knowing early, which is why phase 1 proves it
  before anything is built on top.
- **Param count (~940):** the same host-verification drill as `LANES.md`'s ~820 — Live / Logic /
  Reaper, group tree, state size, scan time.

---

## 12. Open questions

- **Down-expand / gating** — deferred (§ 2.3). Revisit if real use demands it; it costs a mode
  parameter and it is the one thing the single transfer law cannot express.
- **Detector cadence** — per-sample vs per-sub-block SVF gain updates; decided by the phase-2
  profile, not in advance.
- **External sidechain input** — out of scope v1 (internal detector only). It would be a point-level
  source choice, so it fits the schema without restructuring.
- **Spectral-dynamics territory** (Pro-Q 4's many-band automatic mode) — explicitly not v1; the
  semantic Helper is TabbyEQ's answer to that problem, and it rides these same honest parameters.
