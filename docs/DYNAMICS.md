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
>
> **Rev 2.1 (2026-08-02) — after the architecture consilium** (codex `gpt-5.6-sol` xhigh + DeepSeek
> v4-pro + Fable 5 xhigh, per felitronics-core's mandated per-item workflow). Two substantive
> corrections, both to claims this document previously made with confidence. The **auto-attack law
> was physically wrong**: a band's envelope rises with its inverse bandwidth `Q/fc`, not with its
> period, and a direct measurement of the real probe put the old law **5.6× too fast at Q = 40**
> (§ 2.2). And **"GR fades to zero on sustained material" was wrong**: the true asymptote is
> `over → −offset`, so steady state is set by the offset's *sign* — which also forces the offset to
> be mode-aware (§ 2.3). The auto layer additionally gained the constraints that let it survive real
> sessions: interval-correct control-rate coefficients, a silence freeze, seeding, and
> reseed-on-retune.

The next big TabbyEQ feature: make **any point optionally dynamic** — its gain reacts to the level
in its own frequency region — plus a **de-esser** preset on the same engine, with **gain-reduction
(GR) metering** the semantic Helper will later read. Pro-Q / Neutron tier.

---

## 0. What already exists (inventory before design)

`felitronics-core` (v0.13.1, and tabby now pins it — PR #38) **already ships the whole dynamics
stack**, tested
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

**The law is the band's own ringing time, not its period.** An earlier draft used
`3000/fc · (1 + log2(Q)/8)` — "three periods, slightly widened for narrow bands". Both review seats
rejected it as physics, and a direct measurement settled it: the 10–90 % envelope rise time of an
`eq::Svf` BandPass probe scales with the **inverse bandwidth**, `Q/fc`, not with the period.

| fc / Q | measured rise | old law | `2.2·Q/(π·fc)` |
|---|---|---|---|
| 1000 / 10 | 7.17 ms | 4.25 | **7.00** |
| 1000 / 40 | 28.08 ms | 5.00 | **28.01** |
| 100 / 40 | 280.8 ms | 49.96 | **280.1** |
| 7000 / 40 | 4.94 ms | 0.71 | **4.00** |

The old law under-shot by **5.6×** at Q = 40 — the follower would have modulated the gain faster
than the filter can ring, which is intermodulation distortion on every narrow band. So:

The closed form is derivable, not just fitted: a 2nd-order BP has poles at real part `−α`,
`α = ω₀/(2Q)`, the driven envelope is `1 − e^(−αt)`, so the 10–90 % rise is `ln9/α = ln9·Q/(π·fc)`
**≈ 2.197·Q/(π·fc)** — which is the measured column.

**But the naive form breaks near Nyquist.** The SVF is bilinear-prewarped in `fc` only
(`Svf.h:46`, `g = tan(π·f/fs)`), so the −3 dB edges compress toward Nyquist and effective Q *rises*.
That — not measurement noise — is why the 7 kHz row sits above prediction; I wrongly wrote it off as
follower contamination. The digital-bandwidth form is

```
ringMs        = 2 * 1000 * ln(9) * Q / (fs * sin (2*pi*fc/fs))   // -> 2.197*Q/(pi*fc) as fc/fs -> 0
periodFloor   = kP * 1000 / fc                                    // kP ~ 2-3; see below
baseAttackMs  = clamp (max (periodFloor, attackFraction * ringMs), 1.0, 300.0)
baseReleaseMs = clamp (3.0 * baseAttackMs, 40.0, 500.0)
```

- **Costs one `sin` and matters.** At fc = 16 kHz / Q = 40 the naive form is off by **more than 2×**
  (effective Q ≈ 93). At 7 kHz / Q = 40 it explains most of the 4.94 vs 4.00 ms gap.
- **The period floor is physics, not fudge.** Rise drops below one period whenever
  `Q < π/ln9 ≈ 1.43`, and a sub-period "attack" on an envelope is undefined — it modulates gain
  inside the cycle, which is the classic low-frequency compressor distortion.
- **`attackFraction < 1` is deliberate.** The sidechain BP *already* imposes this same rise on the
  probe; setting the GR attack equal to it puts two matched lags in series (≈ 1.4× slower than
  either). To react as fast as the band physically allows, the follower should take a **fraction**
  (≈ 0.25–0.5) of the probe's own ring time. Nobody had noticed the double count.
- **Release is 3×, not 12×.** The band's ring-*down* is the same `2.2·Q/(π·fc)` (symmetric poles),
  so tracking a resonance decay needs ~1–3×, and chatter is prevented by an absolute floor rather
  than a large multiplier. 12× would have put release at 3.6 s — **the same timescale as the auto
  threshold's own 2–5 s**, so the threshold servo and the release would chase each other and the GR
  would breathe. Timescale hierarchy is now explicit: `τ_estimator ≥ 8–10 × release_max`.

- **The ceiling had to move from 50 ms to 300 ms.** At fc = 100 / Q = 40 the band physically cannot
  be tracked faster than ~280 ms; clamping to 50 would reintroduce exactly the distortion the law
  exists to avoid.
- **"Three periods" was a myth twice over** — after clamping, the old law was really a 50 ms bass
  plateau, a narrow transition, and a 1 ms treble plateau, with genuine frequency dependence over
  only about five octaves. If a treble floor is kept it must be documented as a **time-domain
  floor** chosen for de-essing, not as a period-derived value.
- **The floor constant, the release multiplier and whether a period term is needed at all** are
  with the third review seat; the numbers land when that verdict does. What is already settled is
  the shape: linear in `Q/fc`.
- **Dual-rate release is dropped.** It would have to live inside `GainReductionFollower`, and that
  is a shipped primitive we are not changing. Program dependence comes from the auto threshold
  instead.
- **Deviation knobs:** `atk` / `rel` ∈ [0, 1], 0.5 = auto. Multiplier = `2^((x − 0.5) * 4)` — 0.5 →
  ×1, 0 → ×1/4, 1 → ×4. **Applied before the final clamp**, which means "faster" cannot push a
  treble band below the floor — a documented dead zone, not a bug.

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

- **Auto (default):** a slow running estimate of the lane's own band-limited program level `P`, plus
  a fixed offset `O`, pushed into `setThresholdDb()` at control rate. Core has nothing like it —
  ours to write, and it belongs in `dynamics/` rather than the product, since a broadband compressor
  wants the same thing.
- **Manual:** an absolute dBFS value. The slider's top position *is* auto (displayed `A`), the
  Pro-Q affordance — and the live trigger level is drawn inside the slider so a manual threshold is
  set by eye, not by guesswork.

**What auto actually means — the asymptote.** On stationary material the detector converges to the
program estimate, `D → P`, so `over = D − (P + O) → −O`. The steady-state behaviour is therefore
set entirely by the **sign of the offset**, and "gain reduction fades to zero on sustained material"
(an earlier claim in this doc, and one of the two review seats agreed with it) is **wrong**:

Exactly: `GR_steady = −slope · kneeOver(−O)`, and `kneeOver` (`GainComputer.h:59-67`) makes the
three regimes precise —

| offset | stationary behaviour |
|---|---|
| `O ≥ knee/2` | `kneeOver = 0` → band idles exactly. **Sustained content untouched.** |
| `O = 0` | `kneeOver = knee/8` → with knee 6 and ratio 4:1 a **permanent −0.5625 dB**, at any level |
| `O < −knee/2` | `GR → slope·O` — constant reduction; a static gain shift wearing a dynamics costume |

So the idle condition is not "positive offset" but **`O ≥ knee/2`** — up to 3 dB with knee 6, and
only 1.5 dB once a small range shrinks the knee to 3. The primitive therefore stores
`O_effective = O_user + knee/2`, making "idle on stationary, engage on peaks" exact by construction
rather than approximately true. Without that the drawn static curve and the audible one disagree by
half a dB forever.

So `O > 0` is the only honest auto default, and it defines the feature: **auto mode reacts to
departures from the lane's own recent norm** — sibilance, a resonance ringing on one note, a boom on
one chord. It cannot, in principle, keep taming a *permanently* excessive band: given enough time
that level simply becomes the norm. That is not a bug to engineer around; it is what a same-signal
estimator can observe.

**The product answer is already in the schema:** auto = event semantics, **manual threshold =
classic behaviour** (a fixed threshold never drifts, so a steady resonance keeps getting cut). Users
who need the second reach for the slider they already have. The primitive is therefore named and
documented as a **lane-relative program-level estimator**, not a resonance detector.

**Wiring: invert it.** Rather than writing a moving threshold into the gain computer, keep its
threshold **fixed at 0 and never rewritten**, and feed it `levelDb − estimateDb − O_effective`. The
transfer is identical and per-lane semantics are unchanged, but it structurally deletes a whole
class of bug: no control-rate threshold writes, so the interval-coefficient trap cannot exist on the
computer side, no unit confusion on reseed, and the stationary constant becomes a one-line query
(`gc.deltaDb(−O)`, the pattern `Compressor.h:43` already uses). The primitive is then a
**`RelativeLevel`** — same two headers, less to get wrong.

**Where the probe taps matters, and nobody had asked.** In a 24-point series chain, if each point's
sidechain tapped its own input, then every earlier point's *dynamic delta* would modulate later
detectors at overlapping frequencies — chained pumping the slow estimator cannot absorb, because the
feed-through is fast. **All probes tap the common EQ-section input** (parallel detection); for the
M/S lanes that means the derived mid/side signal taken *pre-EQ*, which constrains the processing
order in `EqBand`.

**The offset must be mode-aware.** A positive offset idles down-compression but *activates*
up-compression, since the two read opposite sides of the threshold (`GainComputer.h:46-48`). One
signed offset cannot give both directions the same idle behaviour, so the sign is applied per
direction, not stored raw.

**Implementation traps, all of them real:**

- **Control-rate update needs the interval coefficient.** Running a one-pole every 16 samples with a
  per-sample coefficient silently makes it **16× slower** and aliases the probe. Either use
  `exp(−K/(τ·fs))` or accumulate mean square per sample and update the estimate per block. The power
  recursion stays per sample; only `log10` and the threshold write are control-rate.
- **Asymmetric tracking:** rise faster than fall (≈ 1 s up, ≈ 3 s down). Equal rates would let the
  estimate sag between notes and over-compress every tail. Both seats agreed independently.
- **🔴 Gate the gain computer's input, not just the estimator.** The worst defect the panel found:
  in an up-compress band, silence makes `over = thr − L` enormous (estimator held at −40 dB, input
  decayed to −90 → 47 dB of "under"), so the delta **rails at +range and stays there** — the band
  boosts the noise floor by the full Dynamic Range on every pause and every fade. `BoostWhenQuiet`
  is a first-class mode (`DynamicEqBand.h:36,132`), so this is not hypothetical. Below the activity
  floor the delta is forced toward 0, not merely frozen.
- **Cap the slew in dB/s — in BOTH directions.** One seat said "upward"; that is wrong. The
  dangerous direction is mode-dependent: for down-compression the *falling* estimate is the hazard
  (quiet verse → threshold sinks → the chorus hit slams into the range clamp), for up-compression
  the estimate hanging high is. `AutoLeveler.h:159` already clamps both ways.
- **Freeze — hold, don't decay — below an absolute activity floor**, so a pause cannot drag the
  estimate into the noise and range-cap the downbeat. The floor is ≈ −90…−100 dBFS, **not**
  `AutoLeveler`'s −60 (`AutoLeveler.h:223`) — a Side lane or a Q = 40 slice legitimately sits at
  −50…−70. Pair it with an estimate clamp (≈ −80…0 dB) so the estimator cannot learn the noise floor
  itself.
- **Seed on the first valid observation** rather than starting from zero (`AutoLeveler.h:70`) — a
  bad seed out-masses the signal for ~5 τ.
- **Reseed on retune — but through a window, not instantly.** A drag spams `setParams` at control
  rate, so literal reseed-per-change makes the estimator *instantaneous* while dragging, which pins
  the dynamics at the stationary constant. Use `AutoLeveler.h:192-195`'s pattern: a bounded
  fast-adaptation window (~0.3–1 s, re-armed per retune), triggered only by material `fc`/`Q`
  changes and **never** by a static-gain change.
- **Feed the estimator the envelope, not the raw probe.** Sampling `|bp|` every 16 samples beats
  against the carrier; accumulate mean square per sample or reuse the followed envelope. And pin the
  estimator's averaging law to the detector's (RMS against RMS) — a Peak detector against a
  mean-square estimate silently shifts the offset by the crest factor.
- **Do not freeze while GR is active.** All three seats rejected it: a sustained +10 dB step would
  latch the estimate and leave GR stuck indefinitely.

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
- **The pre-static detector tap is correct here, and only because the threshold is auto-relative.**
  A static gain change shifts detector and estimator by the same amount at the same point, so
  `over = fast − slow` is invariant — dragging static gain does not perturb the dynamics at all. A
  post-static tap would step the probe and produce ~τ of false GR after every gain drag. (Caveat for
  later: if a manual-threshold expert path is ever taken further, pre-static detection stops being
  right for it.)
- **Two known consequences of the series split**, neither fatal, both to be documented rather than
  discovered: (1) the static section is matched/decramped while the delta bell is BLT and exactly
  unity at Nyquist (`Svf.h:21-25`), so a static boost with a full-depth dynamic cut leaves a
  shelf-like residual in the top octave; (2) two same-Q bells in series is **not** the same curve as
  one bell at the summed gain — which is what `DynamicEqBand.h:100` computes — so the skirts differ
  by a few dB, the UI must draw `matched(static) × SVF(delta)`, and any A/B against `DynamicEqBand`
  will "fail" by design.

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
covered upstream** — we do not re-test them. New coverage only.

**The single most valuable test — level-translation invariance.** One harness that trips nearly
every failure mode identified above: run the same band-limited program at −50 / −30 / −10 dBFS
through the finished band and assert (i) settled GR identical across levels within 0.1 dB **and
equal to the analytic constant `−slope·kneeOver(−O)`**, (ii) settle time after a +10 dB step within
[0.5, 2]× the design τ, (iii) all three modes, (iv) repeated at fc = 16 kHz / Q = 40. That one test
catches the 16× control-rate coefficient bug via (ii), a too-high silence floor via (i), a wrong
mode-offset sign (up-compress rails at +range instead of the constant) via (i)/(iii), bad seeding,
the stationary-GR formula, and the Nyquist attack-law error via (iv) — and it *is* the product
promise: one knob, any lane level.

**Core (`felitronics_dynamics_tests` for the auto layer, `felitronics_eq_tests` for the band):**

1. **Auto threshold** (the load-bearing one): after a level step the threshold converges to
   programLevel + offset within the specified window; **the same band-limited stream at gains
   spanning −60…+24 dB, through independent lane instances, must yield thresholds differing by
   exactly that gain and scale-normalised `delta` trajectories that null against each other.** This
   is what proves one setting can be shared across lanes at wildly different levels, and it gates
   the whole point-level design (§ 11).
   - **Asymptote:** hold a level step for ≥ 10 time constants and assert the gain computer's input
     converges to exactly `−offset` — the property the panel disagreed about, pinned as a test.
   - **`20·log10` correctness:** a settled sine of peak `A` must report `20·log10(A/√2)`, and
     doubling the amplitude must move the threshold by **6.0206 dB**. This single check catches the
     `10·log10`-on-amplitude error that was in an earlier draft.
   - **Pause safety:** settle, insert digital-zero gaps of 1 ms … 60 s, resume the identical
     waveform; the frozen estimate must survive the gap and the first 100 ms of GR must stay within
     tolerance of a no-gap reference. First-ever startup tested separately (seeding).
   - **Retune:** a large `fc`/`Q` jump must reseed, not drag seconds of stale level behind it.
2. **Auto time constants:** attack linear in `Q/fc` and matching the measured probe rise within
   tolerance; monotone non-increasing in `fc`, non-decreasing in `Q`; clamped at the rails with
   continuity at the corners; the deviation multiplier exactly ×1 at 0.5 and ×1/4 / ×4 at the ends;
   finite output for NaN / Inf / negative `fc`, `Q`, `x`; the resolved values reach
   `GainReductionFollower::setTimes`.
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

**0. Pin bump first, on its own** — tabby v0.12.0 → v0.13.1. **Done** (PR #38: ctest 72/72, all four
formats, auval PASS), kept out of the dynamics diff on purpose.

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
  (the attack floor and release multiplier, the fixed ratio and knee law, the threshold offset and
  its time constants) are first estimates; they will be tuned by ear on real material, and the tests
  are written to pin the *laws*, not the constants.
- **Auto mode does not tame a permanently excessive band** (§ 2.3) — given time, that level becomes
  the norm. This is a property of same-signal estimation, not a defect, and the manual threshold is
  the answer. The risk is one of *expectation*: a user who assumes "dynamic EQ" means the classic
  fixed-threshold behaviour will find auto mysterious. Mitigation is UI copy and the resolved-value
  readouts, not DSP.
- **Divergence from core's own dynamic EQ is deliberate and must stay visible.** We reuse the
  primitives but compose them differently from `DynamicEqBand` (§ 2.4). If core later hardens or
  changes that band, the temptation will be to "just use it" and quietly lose the matched static
  curve. Test 3 exists to make that regression loud.
- **The one knob is crest-factor-dependent across lanes.** Auto-relative fixes the 20 dB disparity
  in lane *means*, not in lane *peakiness*: Side is systematically spikier than Mid (transients and
  reverb dominate it), so at the same setting the S lane engages deeper than M. Since `kneeOver` is
  convex, actual engagement follows the probe's crest factor, which the single knob cannot see.
  Either document it as intended flavour or bias the offset per lane by a fast/slow estimate ratio —
  decide with ears, in phase 2, not on paper.
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
- **A local spectral reference** (compare the band against its neighbours or a smoothed spectral
  envelope rather than against its own history) is the one structure that *could* detect a
  persistent resonance. It is materially more machinery than two header-only primitives, so it is
  named here as the known upgrade path and deliberately not attempted in this phase.
- **Poison tolerance is chain-wide, not ours to fix alone.** The shipped followers accept NaN/Inf
  straight into their state and their block flush only clears tiny finite values
  (`EnvelopeFollower.h:57`, `GainReductionFollower.h:37`); `GainComputer::deltaDb(NaN)` returns NaN.
  Two new primitives cannot make the chain poison-tolerant — either the finite-audio precondition is
  documented, or those existing states get hardened in a separate pass.
