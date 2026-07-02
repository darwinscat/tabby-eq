<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# TabbyEQ — Split points: per-band placement lanes (ST / L / R / M / S)

> Started 2026-07-02. **Supersedes `MS-DUAL-MODE.md`** (the shipped ST↔M/S dual-mode is the
> 2-lane special case of this design). Spec agreed with Oleh in-session. **Rev 2** after crew
> round 1 (Codex REJECT + DeepSeek APPROVE-WITH-FIXES; Gemini unavailable): fixed the bypass
> migration, the sType migration, the FIR degeneracy/convolver story, the order-invariance
> claim, link-mirroring thread/race spec, idle-lane cost, name collisions, mono wording,
> coincident-node selection, stepping order. **Rev 3** after round 2: RT-safe drain (no
> AsyncUpdater from the audio thread — FIFO + dirty flag + the 30 Hz timer), shared-FDL warm
> history for topology switches, lane toggles = documented hard steps (no clickless
> overclaim), delivery-order mirroring determinism, fission terminology + no-reorder +
> one-time notice, activeLane load fallback, point-bypass preserves lane state.
> Inspiration: Pro-Q 3 per-band stereo placement — but **multi-select**: one point may live in
> several placement lanes at once.

## Goal

A band ("point") is no longer *either* Stereo *or* Mid/Side. It carries a **set of placement
lanes** — any subset of **Stereo, Left, Right, Mid, Side** — chosen from a checkbox dropdown in
the edit strip. Each enabled lane is an independently editable node on the canvas (own freq / Q /
gain / slope / bypass); the point's **filter type is shared** by all its lanes. Per-point
**Link FQ** / **Link Q** keep lane frequencies / widths locked while gains differ — the classic
"same bell, different amount per domain" move.

## Locked decisions

1. **Lane set `{ST, L, R, M, S}`, any subset, ≥ 1 always.** No domain-mixing restrictions —
   prohibitions confuse more than odd combos do. The UI refuses to uncheck the last enabled lane.
2. **One filter type per point** (deliberately *not* FabFilter's per-placement type). A point is
   one filter split across domains; heterogeneous stacks are simply several points. This also
   makes Link Q unambiguous (Q links Q, slope links slope — the type is common). `swept` (the
   search band) also stays per-point and, as today, only bites in the plain single-`ST`
   configuration — the search→treat workflow operates on unsplit points.
3. **Full param set per lane**: `on / freq / Q / gain / slope / bypass`. Honest ~820 host
   parameters, grouped per band (`AudioProcessorParameterGroup`, Pro-Q-style tree) with
   collision-free names (§ Adapter) for hosts that flatten the tree.
4. **Link FQ and Link Q are per-point** (two checkboxes in the lane menu), replacing the global
   View → "M/S: link Mid/Side freq". View keeps only the *defaults for new points*. Linking is an
   edit-time mirror implemented **processor-side** (§ Link mirroring) — the old `msFreqLink`
   only worked while the editor was open; that flaw dies here. Link flags are **not parameters**
   (not automatable) — they're per-band `ValueTree` properties.
5. **Lane colours + badges.** An unsplit (single-`ST`) point looks exactly as today (per-band
   colour, no badge). At ≥ 2 lanes, each node takes its **fixed lane colour** and a small
   always-visible **badge** glued to its side: `l / r / m / s / st` (the `st` badge appears only
   when the Stereo lane coexists with others). Lane colour roles are locked (L white, R red,
   ST orange, M green, S blue); exact values are tuned to the brand palette in the UI PR.
6. **No lane tabs in the strip.** The dropdown menu row carries three functions: the checkbox
   toggles the lane, clicking the *name* selects it as the active lane (enabling it first if
   needed) and closes the menu, and the row highlight indicates the active lane. Primary lane
   selection is **clicking nodes on the canvas** (§ UI: coincident nodes); `<` `>` step across
   *all visible nodes* in **global frequency order** (tiebreak: band index, then lane order) with
   a `3m`-style readout; the mouse wheel over the dropdown button cycles the active lane;
   Alt-click on a checkbox makes that lane the *only* enabled one (an edit action on the lane
   set — latching, not an audio solo). The same submenu hangs off the node right-click menu.
7. **Display convention (industry standard, not exact math):** each axis composite = ∏(that
   axis's own-domain lanes) · ∏(Stereo lanes). Cross-domain coupling (an L/R lane does couple
   M and S, and vice versa) is ignored *on the display only* — accepted, as every EQ does; the
   FIR path and the tests use the exact matrix. The main composite stays as today; extra axes
   draw dimmed, and only where some band has a differing lane on that axis.
8. **FIR modes go exact:** Natural/Linear build the true **2×2 transfer matrix** and pick one of
   three convolver topologies (§ FIR): M/S-diagonal (today's 2-IR path), L/R-diagonal (new,
   2 direct IRs), or full matrix (new, 4 IRs + cross sums).
9. **Clean parameter-ID break.** Pre-release is the last cheap moment: new lane-keyed IDs, no
   legacy aliases (the old flat set meant *Stereo* or *Mid* depending on `ms` — an alias cannot
   be honest). Saved v2 states migrate by value **with two documented lossy corners**
   (§ Migration: `sType` fission when the pool is full; nothing else loses data);
   `stateVersion` → **3**.

## Data model — `felitronics::eq::BandParams` v2 (breaking, core v0.2.0)

```c++
enum class Lane : uint8_t { Stereo, Left, Right, Mid, Side };   // fixed order = processing order
inline constexpr int kNumLanes = 5;

struct LaneParams
{
    bool       on     = false;    // lane enabled (the menu checkbox)
    double     freq   = 1000.0;
    double     Q      = 1.0;
    double     gainDb = 0.0;
    int        slope  = 12;       // HP/LP/Notch width analog — per lane, linkable via Link Q
    bool       bypass = false;    // lane kept but muted (ghost node) — distinct from on=false
};

struct BandParams
{
    bool       on     = false;    // the point exists
    FilterType type   = FilterType::Bell;   // SHARED by all lanes (decision #2)
    bool       swept  = false;    // search band; honored only in the single-ST configuration
    bool       bypass = false;    // whole-point bypass (strip power button) — NEW in v3, no v2 ancestor
    LaneParams lanes[kNumLanes];  // fresh band: lanes[Stereo].on = true, rest off

    bool operator== (const BandParams&) const noexcept;   // bitwise doubles, as today
};
```

- A band is *active* iff `on && !bypass && any(lane.on && !lane.bypass)`.
- Defaults reproduce today's fresh band exactly (one Stereo lane at 1 kHz / Q 1 / 0 dB).
- The old flat+`s*` fields disappear; `teq::` shim re-export unchanged in name only.
- **Note:** v2's flat `bypass` was a *Mid-lane* bypass (Side kept running). The v3 point
  `bypass` is a genuinely new control; migration maps the old flag to a *lane* bypass (§ Migration).

## Engine — `felitronics::eq::EqBand` v2

State per band (fixed-size, zero-alloc, RT-safe as today):

- Biquad columns: **ST lane keeps per-channel columns** (`bq[kMaxSections][kMaxChannels]`,
  mono→surround→ambisonics as now). **L / R / M / S each get one column** (they are single-signal
  filters by construction). Memory ≈ current M/S layout + 3 columns — trivial.
- Smoothers ×5 lanes (freq/Q/gain each), per-lane `designN`, per-lane coeff caches.
- **Idle lanes cost zero.** A disabled (`!on`) lane is never designed and its smoothers *snap*
  (no ramp) on param writes, so parking automation on a disabled lane burns nothing. The
  per-band *moving/settled* check spans **active lanes only**; `operator==` still compares all
  fields bitwise (recompute-skip stays exact). Enabling a lane = topology reset for that lane's
  column + snap its smoothers to current targets (the shipped snap-on-load pattern — fresh
  state, no stale-history artifacts). **Lane on/off (and lane bypass) is a hard operator step —
  exactly today's band-enable/bypass semantics**: a deliberate, rare user action; the engine
  guarantees no stale state and no NaN, not an amplitude crossfade. (Same as shipped ST↔M/S.)
- **Topology resets**, extending today's rules: lane `on` toggle resets that lane's column;
  `type`/`swept` change or slope-driven section-count change resets the affected lanes;
  channel-count change resets all.

Processing, `nc == 2`, per block — **the order below is normative** (matrix operators do *not*
commute: `diag(H_L,H_R)` and the M/S matrix only commute when `H_L == H_R`; the analytic matrix,
the FIR builder and the tests all compose in this same order):

```
1. ST lane:   per-channel sections on L and R              (skip if lane inactive)
2. L lane:    sections on ch0 only                          (skip if inactive)
   R lane:    sections on ch1 only                          (skip if inactive)
3. M/S lanes: per sample  m=(L+R)/2, s=(L−R)/2;
              dM = filt(m)−m (if Mid active), dS = filt(s)−s (if Side active);
              L += dM+dS, R += dM−dS          — the shipped delta-fold, verbatim:
              an idle lane leaves its axis bit-exact.
```

`nc != 2` (true mono, surround, ambisonics): **only the ST lane runs** — L/R/M/S are silently
inactive, exactly like `ms` today. The UI dims non-ST rows in the menu with a "stereo only"
hint. **Host mono→stereo layouts are not this case:** the plugin upmixes mono to identical L/R
*before* the engine, the bus is genuinely stereo (`nc==2`), and all lanes operate — an L lane
legitimately creates stereo from a mono source there, and the S lane sees silence. Correct, not
a bug.

### Analytic response (race-free GUI + FIR source of truth)

- `laneView (const BandParams&, Lane)` — a `BandParams`-shaped view of one lane (generalizes
  `sideView`), so `designBand()` / `bandResponse()` serve every lane unchanged.
- Per-axis composite (decision #7):
  `compositeResponse (bands, n, fs, w, Axis a)`, `Axis ∈ {L, R, M, S}`:
  axis L = ∏ H_ST·H_L, axis R = ∏ H_ST·H_R, axis M = ∏ H_ST·H_M, axis S = ∏ H_ST·H_S.
- **Exact 2×2 matrix response** (new, for FIR + tests). Per band, in the L/R basis, composed in
  the normative processing order (ST first ⇒ rightmost factor):

  ```
  H_MS   = [[ (H_M+H_S)/2, (H_M−H_S)/2 ],
            [ (H_M−H_S)/2, (H_M+H_S)/2 ]]      (identity when both lanes idle)
  H_band = H_MS · diag(H_L, H_R) · H_ST·I      (H_ST is scalar — its position is free;
                                                the H_MS↔diag order is NOT)
  H_eq(w) = ∏ over bands H_band(w)             (2×2 complex per frequency; band order matters
                                                only through the same rule and matches process order)
  ```

  An idle lane contributes exact identity, so the matrix degenerates cleanly.

## FIR modes — `felitronics::lineareq` matrix engine (core v0.2.0)

Today's core builds exactly 2 IRs (Mid+Side) and always encodes stereo→M/S→convolve→decode;
the convolution engine swaps a single IR per channel and has **no cross-input routing**. The
matrix engine therefore adds real new machinery, not just "more IRs":

- **Three topologies, picked per snapshot (basis detection):**
  1. *No active L/R lane anywhere* → every `H_band` is of the form `[[a,b],[b,a]]`, product
     stays that form → diagonal in M/S → **today's 2-IR encode/decode path, unchanged**.
  2. *No active M/S lane anywhere* → `H_eq = diag(H_LL, H_RR)` → **2 IRs convolved directly on
     L and R** — a *new* (trivial) direct path; today's code has no L/R mode.
  3. *Mixed* → **full matrix: 4 IRs** `h_LL h_LR h_RL h_RR`;
     `yL = h_LL∗xL + h_LR∗xR`, `yR = h_RL∗xL + h_RR∗xR` — a *new* `MatrixConvolver` with
     cross-input sums (2 extra convolutions + 2 adds; ≈ 2× stereo cost).
- **Linear phase stays truly linear-phase:** replace every *scalar lane* response by its
  zero-phase magnitude `|H_lane|`, then compose the matrix. All entries are then **real** on the
  FFT grid ⇒ every IR is symmetric ⇒ exact linear phase. (Taking magnitudes of matrix *entries*
  instead would be a fallacy — entries are signed; `(|H_M|−|H_S|)/2` must keep its sign or M/S
  reconstruction breaks.)
- **Natural phase:** apply the φ = k·φ_min cepstral blend to each scalar lane response, then
  compose (complex entries, 4 general IRs). Same `k` knob, same latency story as today.
- **Click-free edits AND topology switches — shared warm history by construction:** all
  topologies ride **one shared per-channel input FDL** (frequency-domain delay line; the input
  spectra history depends only on the input and the partition scheme, which are common). An
  "operator" is then just a set of spectral IR banks + routing *over that shared FDL* — so a
  freshly engaged topology reads the same warm history as the outgoing one; there is **no
  cold-start tail**. The atomic **operator swap** crossfades the *outputs* of the old and new
  operators computed from the same FDL. Never mixes old h_LL with new h_LR; enabling the first
  L lane mid-playback crossfades cleanly between topologies.
- Mono (`nc==1`) in FIR modes: one IR = the **ST-axis composite** (∏ H_ST — the only lanes that
  run on mono; the old code called this the "Mid" IR, same thing under v2 semantics).

## Adapter — `src/Parameters` v3 + processor

Per band `b`, lane keys `k ∈ {st, l, r, m, s}`:

```
band{b}_on        Bool    "B{n} On"
band{b}_type      Choice  "B{n} Type"      (9 items, unchanged)
band{b}_swept     Bool    "B{n} Swept"
band{b}_bypass    Bool    "B{n} Bypass"          — whole point (new in v3)
band{b}_{k}_on    Bool    "B{n} {Lane} On"        st default true, rest false
band{b}_{k}_freq  Float   "B{n} {Lane} Freq"      20..20k skew 1k, default 1000
band{b}_{k}_q     Float   "B{n} {Lane} Q"         0.05..40 skew 1, default 1
band{b}_{k}_gain  Float   "B{n} {Lane} Gain"      −24..24, default 0
band{b}_{k}_slope Choice  "B{n} {Lane} Slope"     7 items, default 12
band{b}_{k}_byp   Bool    "B{n} {Lane} Bypass"
```

- **Display names are collision-free by rule:** the `st` lane drops the lane word *only where no
  point-level param shares the name* — so `B4 Freq / B4 Gain / B4 Q / B4 Slope` (unsplit
  automation reads like a normal EQ) but **`B4 Stereo On`** and **`B4 Stereo Bypass`** (the
  point already owns `B4 On` / `B4 Bypass`). Other lanes always carry the lane word.
- **Groups:** one `AudioProcessorParameterGroup` per band ("Band 4"), all 34 of its params
  inside — the Pro-Q-style DAW tree.
- **Non-parameter state** (per-band `ValueTree` properties, saved with the session, not
  automatable): `linkFq`, `linkQ`, `activeLane` (pure UI; persisted — on load, a missing or
  no-longer-enabled `activeLane` falls back to the lowest enabled lane index). Global
  properties: `defaultLinkFq`, `defaultLinkQ` (View menu) — seeds for newly split points.
  `msFreqLink` dies. Point `bypass` never touches lane state — lanes keep their `on/bypass`
  flags and params and come back exactly as left when the point is un-bypassed.

### Link mirroring (processor-side, host-safe)

- `AudioProcessorValueTreeState::Listener::parameterChanged` may fire on **any thread**
  (automation arrives on the audio thread). The listener does **only RT-safe work**: push a
  `{band, lane, param, value}` event into a **lock-free FIFO** and set an atomic dirty flag —
  **no `AsyncUpdater`, no posting from the audio thread** (posting a message is not an RT
  primitive). The FIFO is drained by the processor's **existing 30 Hz message-thread timer**
  (the `lpTick` pattern) — worst-case mirror latency one timer period, fine for an edit mirror.
- Mirrored writes go through `setValueNotifyingHost` under a re-entrancy guard (mirror-writes
  are tagged and never re-mirrored). No gesture forwarding — mirrors are plain value changes
  (JUCE `ParameterAttachment` behaves the same way).
- `linkFq` mirrors freq across the point's *enabled* lanes; `linkQ` mirrors Q — or slope when
  the shared type is HP/LP/Notch-like. Disabled lanes are not written; they inherit on enable
  (the enable seeds from the active lane, § UI).
- **Determinism = delivery order:** the drain processes events **in FIFO order** (the host's own
  delivery order within the block), each event mirroring in turn (consecutive events for the
  same param coalesce). The final state is the *last delivered* edit — temporally correct, not
  an arbitrary priority. Simultaneously automating two linked lanes of one point is still silly
  and documented as such, but it resolves the way the host ordered it.
- `readBand()` packs the 5 lanes straight into `BandParams` v2. Solo (`S`) band-passes at the
  **active lane's** freq. Analyzer domains (ST ch0 / Mid / Side) unchanged.

### State migration — `stateVersion` 2 → 3

In `setStateInformation`, when the incoming tree carries version ≤ 2, translate values:

| v2 state                          | v3 result                                                     |
|-----------------------------------|---------------------------------------------------------------|
| `ms = false`                      | `st` lane ← flat fields (incl. `bypass` → **`st` lane bypass**); other lanes off |
| `ms = true`                       | `m` lane ← flat fields (incl. `bypass` → **`m` lane bypass**), `s` lane ← `s*` fields, `st` off |
| point `bypass` (new)              | **always `false`** — v2's flat `bypass` muted only the Mid lane (Side kept running: `sideActive` ignores it); mapping it to the point would silence previously-running Side lanes |
| `ms = true && sType ≠ type`       | **fission into two points**: the `s` lane moves to the **first free band slot** (existing bands keep their indices — no reorder) as an `{s}`-only point with `type = sType` (shared-type invariant holds per point); both fissioned points get `linkFq = false` (a single-lane point has nothing to link). If no slot is free: coerce `sType → type` and record a `migrationNote` state property (accepted, logged loss; the editor shows a one-time notice when it's present) |
| `sOn / sBypass`                   | `s` lane `on / bypass`                                        |
| editor `msFreqLink` prop          | `defaultLinkFq ← old value`. Per-band `linkFq = true` **only for bands migrated as `{m,s}` (not fissioned ones) whose Mid/Side freqs actually match** (ε = 0.01 Hz) — v2 sessions saved with the link on but freqs later diverged (headless edits — the old editor-only flaw) must NOT be re-linked, or the first edit would collapse the divergence |

Old automation lanes in existing DAW projects break (accepted — pre-release, decision #9).
A v3 state loaded by an old binary simply resets (also accepted, pre-release).
Migration fixtures come from **real v2 session XML** dumped before the schema change.

## UI — `src/ui/*`

- **Strip:** the `ST/M·S` mode button + `[M][S]` tabs are replaced by one dropdown button
  (venn glyph of the enabled set + a dot in the active lane's colour). The **type icon becomes
  point-level** (today it edits the active lane's `laneId("type")` — that binding, the
  right-click type menu and the readout all repoint to the shared `band{b}_type`). The strip
  keeps its fixed width.
- **Menu** (custom PopupMenu rows so it stays open on checkbox clicks):
  `☑ [venn] ● Stereo · Left · Right · Mid · Side` — checkbox toggles (last one refuses with a
  subtle shake/flash), name-click = enable-if-needed + set active + close, active row
  highlighted, Alt-click = make-only-lane. Newly enabled lanes **seed from the active lane**
  (freq/Q/gain/slope; the shipped copyMidToSide pattern). Separator. `☑ Link FQ`, `☑ Link Q`.
  On non-stereo buses the four domain rows are dimmed with a "stereo only" hint.
- **Venn icons** (FilterShapes-style vector paths): two overlapping circles; L = left disc,
  R = right disc, ST = both bold, M = the lens, S = the two crescents.
- **Canvas:** node per enabled lane. ≥ 2 lanes → lane colours + permanent lane badge
  (`st/l/r/m/s`, small chip, lane-coloured, glued to the node's side); node number stays the
  band number. Per-lane curves (under the existing View → band curves toggle) draw in dimmed
  lane colour.
- **Coincident nodes** (Link FQ + equal gains stacks nodes exactly): hit-testing returns the
  *set* of nodes within the grab radius; a **repeated click in place cycles** through them
  (badge + strip readout show which lane is live). Wheel-over-button and the menu are the
  fallbacks. Selection: click node → selects point + lane; `<` `>` walk all visible nodes in
  **global frequency order** (tiebreak: band index, then lane order); readout style
  `3m  1.24 kHz  +3.5 dB  Q 2.0`.
- **Axes:** main composite as today; additional dimmed axis composites only where some band has
  a differing lane on that axis (decision #7). Beyond-Nyquist fog, floor dive, bottom axis strip
  — all shipped behaviour unchanged.

## Tests

**Core (`felitronics_eq_tests`), measured == analytic as always:**
1. **Legacy equivalence (regression guarantee):** a `{st}`-only band ≡ old Stereo path; a
   `{m,s}` band ≡ old M/S path — same input, bit-exact output vs. reference vectors captured
   from the v0.1.x engine before the change. Includes **Mid-bypassed-Side-active** (v2's
   `bypass` semantics) mapped per the migration table.
2. **Lane axis purity:** L-only band leaves ch1 bit-exact; R-only leaves ch0; M-only preserves
   `L−R`; S-only preserves `L+R` (extends the shipped delta-fold identity tests).
3. **The key new one — matrix truth:** for random mixed lane sets (incl. all-five), the measured
   stereo response (probe L-only then R-only input → all four complex cross terms) matches the
   analytic 2×2 `H_eq` at every probe frequency — **complex compare, not magnitudes**, so
   off-diagonal sign/polarity is proven. Also proves the normative order (a reordered cascade
   would fail the cross terms).
4. Topology/reset: lane toggles (enable mid-stream = hard step with fresh state — assert no
   stale-history artifacts, no NaN, output settles within one block; amplitude steps are the
   documented semantic, as with today's band enable), type change, ST↔split switches; true-mono
   and 6-ch inputs run ST-only; **mono→stereo upmix runs all lanes**; swept honored only
   unsplit; **disabled-lane automation** neither designs nor marks the band moving (cost-zero
   proof).
5. **FIR:** measured complex response == analytic `H_eq` per entry (linear: real signed entries
   after removing the N/2 delay; natural: complex within tolerance); linear-phase symmetry of
   all 4 IRs; **basis detection**: all-M/S picks the encode/decode path, all-L/R picks the
   direct path, both match the matrix prediction; **operator swap** atomicity incl. a
   mid-playback topology switch (2-IR → 4-IR) — no click, no old/new IR mixing, and the
   incoming topology reads the **shared warm FDL** (its first crossfaded block already carries
   the input's past — assert against a cold-started reference, which must differ).
6. **Adapter (`tests/`):** v2→v3 migration fixtures from real session XML — plain, ms-split,
   **Mid-bypassed + Side-active**, **`sType ≠ type` (split-to-new-point + pool-full coercion)**,
   **`msFreqLink=true` with diverged freqs (must NOT re-link)**, linked; link mirroring on the
   message thread incl. HP/LP slope mirroring, the FIFO drain, re-entrancy guard and the
   lowest-lane-wins determinism rule; display-name uniqueness across all 820 (automated check);
   group layout sanity; auval in CI as today.

**Live:** standalone smoke per UI PR (screenshots: split point with badges, menu states,
coincident-node cycling, axes) — same drill as the toolbar branch.

## Phasing (each PR: build all formats + ctest + auval + crew adversarial review)

- **A — fcore `eq` lanes** (BandParams v2, EqBand, laneView/composite/matrix response, tests 1–4).
  Plugin keeps building against the *pinned* v0.1.5 until C lands (sibling dev builds move first).
- **B — fcore `lineareq` matrix engine** (basis detection, direct-L/R path, MatrixConvolver,
  operator swap, tests 5) → tag **v0.2.0**.
- **C — tabby adapter** (schema v3 + groups + migration + link mirroring + `readBand`, pin bump
  to v0.2.0, mechanical UI port so everything compiles & auval passes; canvas still renders the
  lanes it knows). `main` stays green.
- **D — tabby lanes UI** (menu, venn icons, badges/colours, coincident cycling, stepping,
  wheel-cycle, alt-make-only, axis composites, mono dimming) + live verification.

## Risks

- **CPU:** worst case 5 lanes × 24 bands × sections. Mitigated: disabled lanes are cost-zero by
  construction (no design, snap smoothers, excluded from moving-checks), matched design is
  cheap, and the 4-IR path only engages on genuinely mixed sets. Profile a 24-band all-five
  session in A and D.
- **Param count (~820):** VST3/AU handle it (Pro-Q ships ~2k), but verify early in C on
  Live / Logic / Reaper — group tree rendering, state size, scan time. Fallback if a host
  chokes: nothing structural — the count is inherent to decision #3; we'd document host minima.
- **Migration fidelity:** fixtures are real v2 XML, and the two lossy corners (`sType` split
  with a full pool; point-bypass semantics) are explicit table rows with tests, not surprises.
- **UI clutter:** badges only on split points; lane curves dimmed + behind the existing View
  toggles; axes only-when-differing.
