# TabbyEQ — Roadmap (living doc)

> Started 2026-06-26. Firm on **schema + DSP**, deliberately **iterative on analyzer + UI**
> (the creative parts are not waterfalled — we build, listen, adjust). Synthesised from a
> Codex + DeepSeek + Gemini design panel.

## Positioning

A **premium TRACK parametric EQ** that's desirable **without** its semantic layer. Not a
mastering tool (linear-phase / LUFS / saturation are deferred). The semantic **Helper** is an
optional accelerator on top, riding the same real, automatable parameters a human drives.

## Principles

1. **Honest engine** — every parameter is real and automatable; the DAW sees a normal EQ. The
   Helper is just a client of those params (no hidden DSP).
2. **Reusable core** — `teq::` stays header-only, RT-safe (no alloc/lock/IO in `process`),
   framework-agnostic. DSP is planned up front (dependencies + state live there).
3. **One state point** — single versioned APVTS (`stateVersion`); grow it **additively** per
   phase + migrate on load. **No A/B / undo / presets in v1.**
4. **Freeze schema WIDE, implement NARROW** — band count + param-ID naming settle now so indices
   never shift; feature params are added additively later.
5. **Render behind a thin seam** — software `juce::Graphics` now; only the heavy visuals
   (spectrum / curve / glow) sit behind a small renderer interface so a bespoke GL/**Metal**
   backend can slot in later. Attaching `OpenGLContext` is a free global fallback if needed.
6. **Analyzer + ergonomics are iterative** — not pre-specced. Mockups when useful.

## Phases

### Phase 1 — v1 "Standard": premium standard track EQ + WOW UI
**DSP / engine**
- **24 bands** (`kMaxBands` / `kNumBands`).
- Types: bell, low/high shelf, HP, LP, BP **+ notch, all-pass, tilt**.
- Variable HP/LP slopes **6 / 12 / 24 / 36 / 48** (72/96 later) — stack matched sections.
- Per-band **on, solo/listen**.
- **M/S · L/R per band** (standard; lowest priority — may slip past first cut).
- In/out metering. Output trim (done).

**Control / UX**
- **Selected-band edit strip** — type / freq / Q / gain / slope, numeric + keyboard entry; live
  readout on the node. (In progress.)
- Draggable nodes, wheel-Q, add/remove (done). Multi-select / modifier-drag — iterate.

**WOW UI (cheap, software — see the visual language below).**

### Phase 2 — Dynamics
- **Per-band dynamic EQ**: matched static biquad **+ cheap Cytomic SVF gain delta** driven by an
  envelope detector (threshold / ratio / attack / release).
- **Auto-relative threshold** — threshold = detector running RMS + N dB, so it works at any input
  gain (no absolute-dBFS guesswork).
- **De-esser = a dynamic-band preset.**
- **GR metering** per band (tension-vector / EKG trace, below). *(Needed by the Helper.)*

### Phase 3 — Helper (semantic)
- Source + named trait → pre-place & **label real bands** (static AND dynamic).
- **Macro-Node** primitive: one diamond node owns a multi-band fix; drag scales all constituents;
  double-click "shatters" into normal nodes; **delete = undo that helper action** (solves
  multi-band traits + single-action undo + non-destructive coexistence in one primitive).
- **search → treat**: zoom to the trait range; narrow boost to find the resonance by ear; flip to
  a musical cut at the found freq. With: **FFT-assisted peak finder** (snap to the loudest bin in
  range), **monitoring safety** (gain-comp + auto-dim/fade outside the range, no-peak fallback),
  and a **"spotlight"** on the active range.
- **Ambiguity as a feature** — a trait that could mean several things shows 2–3 candidate lanes
  (rule-based confidence, not ML) with a quick A/B toggle.
- **Role reweighting** (lead / rhythm / backing) — a weight matrix retunes/reorders, never moves
  the frequency map.
- **Auto-gain** after a move (measure pre/post RMS → one-click makeup; never silent).
- **Provenance / transparency** — helper bands carry `origin`, `traitKey`, label state (manual
  override → italic, bypass → hollow ring); a small action log with per-action **Revert**.
- **Blind search mode** (optional) — hide the graph during search→treat, find by ear only.
- **Wildcards** (optional, fun, all trivial to add) — "Make it Worse / Ruiner" calibration trait
  (boost mud / cut air to train the ear); "opposable" traits (a cut links a gentle complementary
  boost to hold perceived energy); collision sparks on aggressive GR.

### Later (post-v1.x)
Saturation / "warmers", linear-phase mode, external sidechain, surround channel masks,
A/B / undo / presets, LUFS / true-peak, spectrogram, EQ-match, cross-track collision.

## Freeze now (schema)

- **24 bands**; `bandN_*` param-ID convention (indices never shift).
- `teq::BandParams` reserves the design for future fields: `dynamic{ on, threshold(rel), ratio,
  attack, release, range, detector }`, `route{ stereo / L / R / M / S }`. Implement per phase;
  migrate state additively.
- Band model reserves **provenance** (`origin`, `traitKey`, `actionId`, `manualDirty`) and a
  **macro-group** id for Macro-Nodes (helper phase).
- `swept` is an **internal** search detail, not a pro-facing control.

## Cheap WOW visual language (v1, software)

All ≈free on CPU; concrete techniques chosen to dodge the traps.

- **Fake curve glow** — 3–4 stacked low-alpha over-strokes (NOT real blur).
- **Thermal spectrum fill + peak-hold dots** with a decaying trail.
- **"Liquid" FFT** — per-bin one-pole smoothing + straight `lineTo` + a **pre-rendered gradient
  image** (NOT `createPathWithRoundedCorners`).
- **Freeze / ghost spectrum** (Shift = capture) — visual A/B without an A/B system.
- **Node hover** — halo + value bubble with spring easing; per-band colour.
- **Fill-to-curve** shading (warm above 0 dB, cool below).
- **Note-name snap + overlay** when dragging.
- **Search "spotlight"** — pre-rendered black image + soft hole, `multiply` blend at cursor X
  (near-zero CPU); dim outside the active trait range.
- **Dynamics visuals** — tension vector (static→active node) + tiny scrolling **EKG** GR trace on
  hover.
- **Premium theme** — radial vignette + soft grid + glow on the 0 dB line; UI easing everywhere
  (audio stays exact).

**Traps to avoid:** real Gaussian blur, per-frame `DropShadow` on many nodes,
`createPathWithRoundedCorners` on the FFT, particle systems > ~10, spectrogram/waterfall,
pitch/fundamental detection, high-refresh M/S analyzer, true cross-track masking.

## Open / iterate (not pre-spec'd)

Analyzer details (pre/post/both, peak-hold, tilt slope, collision hints); exact ergonomics;
the precise WOW visual language (with mockups as we go).

## Phase 1 status (in progress)

**Done:** 24 bands · selected-band edit strip (#8: numeric + keyboard freq/Q/gain) · symmetric
peaking cut · brand palette + warm/cool fill + curve glow · spectrum (liquid 1/12-oct smoothing +
peak-hold + 4.5 dB/oct pink tilt + peak-detail; Neutron/FabFilter-ish) · Q-whiskers (Neutron-style,
log-linear-calibrated, side-clamped) · "+"-on-curve add + node hover-halo · notch / all-pass / tilt ·
variable HP/LP slopes 6–96 dB/oct (Butterworth cascade) · per-band solo (band-listen) · premium
vignette + 0 dB glow · analyzer pre/post toggle · HP/LP whisker steps the discrete slope ·
filter-type shape icons. (227 engine checks; all four Mac formats build + AU passes auval.)

**Deferred — need a decision / live feedback:**
- **Node on/off + ghost** — needs a band "used vs bypassed" flag (distinct from `on=false` = free
  slot), part of the reserved band-provenance schema, so we don't ghost all 24 empty slots.
- **Floating point-toolbar** (Neutron-style) — a mini panel by the node holding ALL point info +
  control (type-icon + on/off + solo + close + dropdown + freq/gain/Q/note). The bottom strip then
  goes away and the bottom is **reserved for the Helper**. Big UI move — do with live feedback.
  *(Current bottom strip + the type-shape icon are interim, to be folded into this toolbar.)*
