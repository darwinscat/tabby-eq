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
>
> **Then reconciled with what `felitronics-core` already ships (§ 0).** The core `dynamics`,
> `dynamiceq` and `deesser` modules exist, are tested, and implement Rev 1's model — its
> `GainComputer` *is* Rev 1, mode enum and all. So this revision is **not** a from-scratch build: it
> is a UX/schema layer plus an auto layer over primitives that already work, and Rev 2's proposed
> bespoke transfer law was **dropped** in favour of the shipped `GainComputer` with ratio and knee
> fixed internally (§ 2.3). The FabFilter *interface* survives intact; only the arithmetic behind it
> is now reused instead of reinvented.

The next big TabbyEQ feature: make **any point optionally dynamic** — its gain reacts to the level
in its own frequency region — plus a **de-esser** preset on the same engine, with **gain-reduction
(GR) metering** the semantic Helper will later read. Pro-Q / Neutron tier.

---

## 0. What already exists (inventory before design)

`felitronics-core` (v0.13.1; tabby pins v0.12.0) **already ships the whole dynamics stack**, tested
and RT-safe. Nothing below needs to be written:

| module | what's in it |
|---|---|
| `modules/dynamics` | `EnvelopeFollower` (Peak/Rms, `flushDenormals`), `GainComputer`, `GainReductionFollower`, `ChannelLinker`, plus `Compressor`, `NoiseGate`, `TransientShaper`, `AutoLeveler` |
| `modules/dynamiceq` | `DynamicEqBand` — sidechain BandPass SVF at the band's freq/Q → follower → gain computer → GR ballistics → audio SVF; control-rate coeff recompute (K = 16 ≈ 0.33 ms); zero latency |
| `modules/deesser` | `DeEsser` — two topologies (delegating `DynamicEq`, and `SplitBand` on an LR4 `Crossover2`), bandpass sidechain, `listen` solo |

**`felitronics::dynamics::GainComputer` is literally Rev 1 of this document** — `Mode
{ DownCompress, UpCompress, DownExpand }`, ratio, symmetric knee, range clamp, down to the comment
wording. Rev 1 was not a proposal that went unbuilt; it shipped upstream.

**So what is actually left to build:**

1. **The auto layer (new, genuinely ours).** Auto-relative threshold and auto time constants derived
   from the band's own `fc`/`Q` do not exist in core. (`AutoLeveler` is loudness matching from
   OrbitCab — a different problem — though its slew-limit discipline is a good model.) These are
   core-shaped primitives, not product glue: a broadband compressor wants the same auto threshold.
2. **Our composition (§ 2.4).** `DynamicEqBand` runs the *whole* gain (static + delta) through one
   Cytomic SVF. TabbyEQ cannot: decision #3 requires the static curve to stay a **matched Vicanek**
   biquad, or a dynamic band would sound different from a static one and the drawn curve would
   diverge from reality near Nyquist. We reuse the **primitives**, not `DynamicEqBand` itself.
3. **The TabbyEQ layer** — per-lane detectors inside `felitronics::eq::EqBand`, the point-level
   schema, the adapter, the UI. Product glue, stays in the product, per the architecture rule.

The de-esser preset (§ 4) is then nearly free — `deesser::DeEsser` already encodes the topology
choice and the bandpass-sidechain reasoning; we mostly need its parameter defaults.

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
| Ratio | explicit | **fixed at 4:1 inside** — not exposed (§ 2.3) |
| Knee | explicit | **fixed, scaled off range** — not exposed (§ 2.3) |
| Mode | 3-way choice | not exposed — direction is the **sign of range** (§ 2.3, down-expand deferred) |
| **Per band** | **7 × 5 lanes = 35** | **5** |
| **Total (24 bands)** | **840** | **120** |

> The **auto layer** in § 2.2–2.3 (time constants from `fc`/`Q`, the running-program-level
> threshold) is our own formulation in the spirit of that model — FabFilter's actual algorithm is
> closed, so these are written out concretely to be implementable and testable, not
> reverse-engineering claims. The **static curve** underneath is not ours and not FabFilter's: it is
> core's shipped `GainComputer`, driven with ratio and knee fixed.

---

## 2. DSP design

### 2.1 Detector (sidechain)

- Per **running lane** of a dynamic point — `lane.on && !lane.bypass`, which `LANES.md` calls an
  *active* lane; **not** the UI's `activeLane` property (the lane currently being edited). Each
  gets an internal **band-pass probe** (a Cytomic SVF in band-pass at that lane's `freq` / `Q`)
  tracking the energy in the lane's region, fed the lane's **input** — the signal entering this
  lane — so detection does not chase its own gain change.
- **Channel linking is mandatory, not optional** (`dynamics::ChannelLinker`, `LinkMode::Max` or
  `Rms`): the probe runs per channel, but the channels collapse to **one linked level → one gain
  applied to all of them**. Without it a stereo ST lane would compress L and R independently and
  the image would wander on every sibilant. Per-lane, so an L lane links nothing (it is one signal
  by construction) and the ST lane links across the whole bus.
- **Ballistics sit on the gain, not on the level** — the lesson already encoded in core: the
  `EnvelopeFollower` runs a short symmetric window (≈ 2 ms) purely to get a level, and the musical
  attack / release live in `dynamics::GainReductionFollower`, applied to the computed gain. Keeping
  them decoupled is what stops the knee from warping the attack — detector-level smoothing does
  warp it. Both followers **software-flush denormals** every block (Law 8).
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

**New — core has no equivalent.** Attack and release are derived from what the band actually is,
then modulated by the deviation knobs, and the result is fed to
`GainReductionFollower::setTimes()`. The frequency term is the important one: a band's region
cannot be tracked faster than its own period, and this is exactly what makes a de-esser fast and a
low-mid tamer slow **without the user setting anything**.

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

### 2.3 Gain computer — the shipped one, with ratio and knee fixed internally

**Reused as-is: `felitronics::dynamics::GainComputer`.** An earlier draft of this revision proposed
a bespoke asymptotic law (`delta = range·(1−exp(−over/tau))`) to make ratio and knee disappear. That
was solving the wrong problem: those knobs disappear from the *interface* simply by not exposing
them, and the shipped computer is already tested. **Dropped — we drive the existing one.**

```
gc.setMode      (Mode::DownCompress);          // always; direction handled by the sign of range
gc.setRatio     (4.0);                         // FIXED, not a parameter
gc.setKneeDb    (min (6.0, 1.5 * |range|));    // FIXED law, scaled so a small range keeps a
                                               // proportionate corner instead of a 6 dB smear
gc.setRangeDb   (|range|);                     // the user's one knob
gc.setThresholdDb (resolved);                  // auto or manual — below
```

The sign of `range` is applied outside the computer (the `sign_` multiplier, exactly as
`DynamicEqBand` does for its `BoostWhenLoud` mode), so one `Mode` covers both directions.

`levelDb = 20·log10(env)` — the shipped `EnvelopeFollower` returns an **amplitude** envelope (its
`Rms` detector square-roots internally), and core's own `DynamicEqBand` feeds `gainToDb` the same
way. *(An earlier draft of this section said `10·log10` on the assumption the follower exposed mean
square. It does not.)*

**The threshold — this part is new:**

- **Auto (default):** a slow running estimate of the lane's own band-limited program level (a
  long-time-constant follower on the same probe, ≈ 1–2 s) plus a fixed offset, pushed into
  `setThresholdDb()` at control rate. The threshold sits just above "normal" for this band and
  adapts to gain staging by construction. Core has nothing like it — this is ours to write, and it
  belongs in `dynamics/` rather than the product, since a broadband compressor wants it too.
- **Manual:** an absolute dBFS value. The slider's top position *is* auto (displayed `A`), the
  Pro-Q affordance — and the live trigger level is drawn inside the slider so a manual threshold is
  set by eye, not by guesswork.

**What the fixed constants buy, and what they cost:**

- `ratio = 4` gives a slope of `1 − 1/4 = 0.75`, so the full range is reached at `|range| / 0.75`
  dB of overshoot — 16 dB above threshold for a 12 dB range. Firm but not limiting.
- Unlike the dropped exponential law, this one **does** hit its limit exactly (the computer clamps
  at `±range`) rather than approaching it asymptotically. That is a real behavioural difference:
  above the clamp point the band stops responding to further level. Acceptable — it is what every
  ranged dynamic EQ does, `range` is the user's declared ceiling, and the knee already softens the
  corner. Noted so nobody rediscovers it as a bug.
- **Direction is the sign of `range`:** negative = the band pulls down as its region gets loud
  (**downward compression**, the common move); positive = the band pushes up as it gets loud
  (**upward expansion**). The classic "lift when quiet" is approximated by a **static boost with a
  negative range** — the band rests boosted and gives the boost back once the region gets loud. Not
  literally upward compression (that reacts to level *falling below* the threshold), but the
  audible move is the same and it costs no parameter.
- **Down-expand / gating is deferred** (§ 12) — the computer *has* `Mode::DownExpand`, so this is
  purely a UI/parameter decision now, not a DSP one. It costs a mode parameter; it waits for demand.

The computed delta then goes through `GainReductionFollower` (§ 2.1), which is where all the
smoothing happens — nothing further is needed to keep it zipper-free.

### 2.4 Application — "matched static + SVF gain-delta"

Unchanged from Rev 1, still the right split — and **the one place we deliberately diverge from
`dynamiceq::DynamicEqBand`**, which runs the whole gain (`static + delta`) through a single Cytomic
SVF. TabbyEQ cannot do that: decision #3 requires the static curve to stay a **matched Vicanek**
biquad, otherwise a dynamic band would not sound like the static band it replaces, and the drawn
curve would part company with reality near Nyquist. So we take core's primitives and compose them
ourselves.

The resting lane stays a **matched biquad** (Nyquist-accurate static curve); the **dynamic delta**
is a **Cytomic SVF bell** at the same `freq` / `Q` in series, whose gain is the smoothed `delta` dB:

```
y = matchedStatic (x);            // base curve (the user's set gain), matched / Nyquist-accurate
y = svfDelta (y, gain = delta);   // dynamic delta; delta = 0 → transparent
```

- At `delta = 0` the SVF is transparent → a non-dynamic lane is unchanged, zero cost beyond a
  detector that is itself skipped when `dyn_on` is false.
- SVF rather than a re-matched biquad because its gain is **cheap to modulate** sample-to-sample
  without a full redesign — that is the entire point of the split.
- Cadence: **borrow `DynamicEqBand`'s answer** — detector and gain computer per sample, but the SVF
  gain recompute at **control rate** (its `coeffUpdatePeriod`, K = 16 ≈ 0.33 ms at 48 k). The
  Cytomic gain enters the damping term (`k = 1/(Q·A)`), so a gain change costs a full coefficient
  recompute; K = 16 is core's measured compromise and there is no reason to re-derive it. This also
  retires most of Rev 2's "detector cadence" open question before it is asked.
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
| type / freq / Q | high bell, **7 kHz** — core's `DeEsserParams::fc`; sidechain Q ≈ 2 there, so a bell Q of 2–3.5 is the range to try by ear |
| `dyn_on` | true |
| `dyn_range` | **−8 dB** — core's tested `rangeDb`, chosen to cap the cut before it lisps (Rev 2 guessed −12 with no such reasoning) |
| `dyn_thr` | **auto** (the default) — core hard-codes −30 dBFS, which is exactly the guesswork auto exists to remove |
| `dyn_atk` / `dyn_rel` | **auto** (0.5) — resolves to 1 ms / 20 ms at 7 kHz (§ 2.2), against core's hand-set 2 ms / 90 ms |

Pure parameters; the engine does not know it is a "de-esser". Under Rev 1's model the same preset
also had to assert ratio, knee, mode and explicit attack / release / threshold in ms and dBFS —
magic numbers a user cannot sanity-check, and wrong the moment the input gain changes.

---

## 5. GR metering

Each **lane** of a dynamic point exposes its current gain reduction (dB, the live `delta`) via a
lock-free atomic, read and smoothed on the UI side — the accessor shape `DynamicEqBand` already has
as `dynamicDeltaDb()`, widened to per-lane and made cross-thread safe. Shown on the node (the node riding its range
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

`GainComputer`, `EnvelopeFollower`, `GainReductionFollower` and `ChannelLinker` are **already
covered upstream** — we do not re-test them. New coverage only:

**Core (`felitronics_dynamics_tests` for the auto layer, `felitronics_eq_tests` for the band):**

1. **Auto threshold** (the load-bearing one): after a level step the threshold converges to
   programLevel + offset within the specified window; **a 20 dB input-gain change leaves the
   `delta` trajectory unchanged** — this is what proves one setting can be shared across lanes at
   wildly different levels, and it gates the whole point-level design (§ 11).
2. **Auto time constants:** `baseAttackMs`/`baseReleaseMs` monotone in `fc` and clamped at the
   rails; the width term applied before the clamp; the deviation multiplier exactly ×1 at 0.5 and
   ×1/4 / ×4 at the ends; the resolved values reach `GainReductionFollower::setTimes`.
3. **Our composition vs core's:** a static band and a dynamic band resting at `delta = 0` are
   **bit-identical** (the delta SVF is transparent at unity), and a dynamic high shelf's *static*
   response still matches the matched-biquad analytic curve near Nyquist — the property
   `DynamicEqBand`'s single-SVF topology would have lost.
4. **Channel linking:** a stereo ST lane fed a sibilant in one channel only applies the **same**
   gain to both (image preserved); an L-only lane is unaffected by R content.
5. **Per-lane independence:** a dynamic `{m,s}` point driven by a Mid-only signal moves the Mid lane
   and leaves the Side axis bit-exact; a dynamic `{st}` point on a 6-channel bus follows the ST-only
   rule; control-rate updates leave no zipper (measured spectrum of a swept-level tone);
   no-alloc-in-`process`.
6. **Gating:** `dyn_on` is inert on the five gainless types (output bit-identical to the static
   band, including after a Bell → Notch switch with dynamics left on); dynamics bypassed in Natural
   and Linear (output == the static FIR response).

**Adapter (`tests/`):** v3→v4 migration (dynamics off ⇒ output bit-identical to the v3 reference);
display-name uniqueness across all ~940; group layout sanity; auval in CI as today.

---

## 10. Phasing (each PR: build all formats + ctest + auval + crew adversarial review)

**0. Pin bump first, on its own** — tabby v0.12.0 → v0.13.1, a separate small PR (ctest + auval, no
behaviour change) so "newer core" never shares a diff with "dynamics". Everything below assumes it.

1. **fcore auto layer** — `AutoThreshold` (slow program-level follower + offset) and the
   auto-time-constant helper, in `dynamics/`, JUCE-free, software-flush, tests 1–2 → core minor tag.
   **This phase is the gate:** test 1 either validates point-level shared settings or sends us to
   the § 11 fallback, and it must answer before the schema is frozen in phase 3.
2. **fcore `eq` integration** — `BandParams` v3 `Dyn` sub-struct; per-lane sidechain probe +
   linked detector + `GainComputer` + `GainReductionFollower` + delta SVF composed inside `EqBand`
   (reusing the primitives, *not* `DynamicEqBand`); GR atomics; tests 3–6.
3. **tabby adapter** — 120 params + state v4 + migration + `readBand`, mechanical UI so everything
   compiles and auval passes. `main` stays green. **Schema freezes here.**
4. **tabby UI** — dynamic handle + range band + live GR + the expandable dynamics row + resolved-ms
   readouts + the type/phase-mode gating.
5. **De-esser preset** — parameter defaults cribbed from `deesser::DeEsserParams`, which already
   encodes the bandpass-sidechain and range-cap reasoning.

Ship desktop first; everything new is JUCE-free so it rides to WASM / embedded later for free.

---

## 11. Risks

- **CPU:** worst case 24 points × 5 running lanes = 120 probes + followers + delta SVFs. Mitigated
  by construction (non-dynamic points and idle lanes are free) and by the control-rate coeff
  recompute we inherit, but still **unproven at this width** — core's band was profiled as *one*
  band, not 120. Profile a deliberately hostile session in phase 2, before the UI lands.
- **Auto behaviour is a taste problem, not a correctness problem.** The constants in § 2.2–2.3
  (`kA`, `kR`, the fixed ratio and knee law, the threshold offset and its time constant) are first
  estimates; they will be tuned by ear on real material, and the tests are written to pin the
  *laws*, not the constants.
- **Divergence from core's own dynamic EQ is deliberate and must stay visible.** We reuse the
  primitives but compose them differently from `DynamicEqBand` (§ 2.4). If core later hardens or
  changes that band, the temptation will be to "just use it" and quietly lose the matched static
  curve. Test 3 exists to make that regression loud.
- **Shared settings across lanes** rest entirely on auto-relative threshold working well. If test 4
  disappoints on real material, the fallback is to make **threshold alone** per-lane: 5 × 24 = 120
  lane thresholds replacing the 24 point-level ones, so **+96 parameters**, and range / attack /
  release stay shared. Not a redesign — but worth knowing early, which is why phase 1 proves it
  before anything is built on top.
- **Param count (~940):** the same host-verification drill as `LANES.md`'s ~820 — Live / Logic /
  Reaper, group tree, state size, scan time.

---

## 12. Open questions

- **Down-expand / gating** — deferred (§ 2.3). Now purely a UI/parameter question: the DSP for it
  already exists (`Mode::DownExpand`), so enabling it later costs one mode parameter and no engine
  work.
- ~~Detector cadence~~ — **settled**: control-rate gain recompute at K = 16, core's existing
  answer (§ 2.4). Only revisit if the phase-2 profile says otherwise.
- **External sidechain input** — out of scope v1 (internal detector only). It would be a point-level
  source choice, so it fits the schema without restructuring.
- **Spectral-dynamics territory** (Pro-Q 4's many-band automatic mode) — explicitly not v1; the
  semantic Helper is TabbyEQ's answer to that problem, and it rides these same honest parameters.
