<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# TabbyEQ — Split points: per-band placement lanes (ST / L / R / M / S)

> Started 2026-07-02. **Supersedes `MS-DUAL-MODE.md`** (the shipped ST↔M/S dual-mode is the
> 2-lane special case of this design). Spec agreed with Oleh in-session; crew design review
> pending. Inspiration: Pro-Q 3 per-band stereo placement — but **multi-select**: one point may
> live in several placement lanes at once, each lane its own node.

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
3. **Full param set per lane**: `on / freq / Q / gain / slope / bypass`. Honest ~800 host
   parameters, grouped per band (`AudioProcessorParameterGroup`, Pro-Q-style tree) with
   self-sufficient names (`B4 Mid Freq`) for hosts that flatten the tree.
4. **Link FQ and Link Q are per-point** (two checkboxes in the lane menu), replacing the global
   View → "M/S: link Mid/Side freq". View keeps only the *defaults for new points*. Linking is an
   edit-time mirror implemented **processor-side** (message thread), not editor-side — the old
   `msFreqLink` only worked while the editor was open; that flaw dies here. Link flags are
   **not parameters** (not automatable) — they're per-band `ValueTree` properties.
5. **Lane colours + badges.** An unsplit (single-`ST`) point looks exactly as today (per-band
   colour, no badge). At ≥ 2 lanes, each node takes its **fixed lane colour** and a small
   always-visible **badge** glued to its side: `l / r / m / s / st` (the `st` badge appears only
   when the Stereo lane coexists with others). Lane colour roles are locked (L white, R red,
   ST orange, M green, S blue); exact values are tuned to the brand palette in the UI PR.
6. **No lane tabs in the strip.** The dropdown menu row carries three functions: the checkbox
   toggles the lane, clicking the *name* selects it as the active lane (enabling it first if
   needed) and closes the menu, and the row highlight indicates the active lane. Primary lane
   selection is **clicking nodes on the canvas**; `<` `>` step across *all visible nodes*
   (lanes within a point, then the next point) with a `3m`-style readout; the mouse wheel over
   the dropdown button cycles the active lane; Alt-click on a checkbox solos that lane (checks
   it, unchecks the rest). The same submenu hangs off the node right-click menu.
7. **Display convention (industry standard, not exact math):** each axis composite = ∏(that
   axis's own-domain lanes) · ∏(Stereo lanes). Cross-domain coupling (an L/R lane does couple
   M and S, and vice versa) is ignored *on the display only*. The main composite stays as today;
   extra axes draw dimmed, and only where some band has a differing lane on that axis.
8. **FIR modes go exact:** Natural/Linear build the true **2×2 transfer matrix** (4 IRs) when
   L/R and M/S domains are mixed, degenerating to the current 2-IR path when the whole set is
   diagonal in one basis (all-M/S or all-L/R — the common case, zero added cost).
9. **Clean parameter-ID break.** Pre-release is the last cheap moment: new lane-keyed IDs, no
   legacy aliases (the old flat set meant *Stereo* or *Mid* depending on `ms` — an alias cannot
   be honest). Saved v2 states migrate by value; `stateVersion` → **3**.

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
    bool       bypass = false;    // whole-point bypass (the strip power button)
    LaneParams lanes[kNumLanes];  // fresh band: lanes[Stereo].on = true, rest off

    bool operator== (const BandParams&) const noexcept;   // bitwise doubles, as today
};
```

- A band is *active* iff `on && !bypass && any(lane.on && !lane.bypass)`.
- Defaults reproduce today's fresh band exactly (one Stereo lane at 1 kHz / Q 1 / 0 dB).
- The old flat+`s*` fields disappear; `teq::` shim re-export unchanged in name only.

## Engine — `felitronics::eq::EqBand` v2

State per band (fixed-size, zero-alloc, RT-safe as today):

- Biquad columns: **ST lane keeps per-channel columns** (`bq[kMaxSections][kMaxChannels]`,
  mono→surround→ambisonics as now). **L / R / M / S each get one column** (they are single-signal
  filters by construction). Memory ≈ current M/S layout + 3 columns — trivial.
- Smoothers ×5 lanes (freq/Q/gain each), per-lane `designN`, per-lane coeff caches.
  Recompute-skip: settled-check spans all lanes; `operator==` covers everything bitwise.
- **Topology resets** (state cleared, snap-not-ramp), extending today's rules: lane `on` toggle
  resets that lane's column; `type`/`swept` change or slope-driven section-count change resets
  the affected lanes; channel-count change resets all.

Processing, `nc == 2`, per block (order fixed for determinism; series LTI ⇒ order-invariant):

```
1. ST lane:   per-channel sections on L and R              (skip if lane inactive)
2. L lane:    sections on ch0 only                          (skip if inactive)
   R lane:    sections on ch1 only                          (skip if inactive)
3. M/S lanes: per sample  m=(L+R)/2, s=(L−R)/2;
              dM = filt(m)−m (if Mid active), dS = filt(s)−s (if Side active);
              L += dM+dS, R += dM−dS          — the shipped delta-fold, verbatim:
              an idle lane leaves its axis bit-exact.
```

`nc != 2` (mono / surround / ambisonics): **only the ST lane runs** — L/R/M/S are silently
inactive, exactly like `ms` today. The UI dims non-ST rows in the menu with a "stereo only" hint.

`nc == 1` special case stays: a mono signal gets the ST lane; nothing else.

### Analytic response (race-free GUI + FIR source of truth)

- `laneView (const BandParams&, Lane)` — a `BandParams`-shaped view of one lane (generalizes
  `sideView`), so `designBand()` / `bandResponse()` serve every lane unchanged.
- Per-axis composite (decision #7):
  `compositeResponse (bands, n, fs, w, Axis a)`, `Axis ∈ {L, R, M, S}`:
  axis L = ∏ H_ST·H_L, axis R = ∏ H_ST·H_R, axis M = ∏ H_ST·H_M, axis S = ∏ H_ST·H_S.
- **Exact 2×2 matrix response** (new, for FIR + tests). Per band, in the L/R basis:

  ```
  H_MS   = [[ (H_M+H_S)/2, (H_M−H_S)/2 ],
            [ (H_M−H_S)/2, (H_M+H_S)/2 ]]      (identity when both lanes idle)
  H_band = H_MS · diag(H_L, H_R) · H_ST·I      (matrix product; scalar lanes = identity when idle)
  H_eq(w) = ∏ over bands H_band(w)             (2×2 complex per frequency)
  ```

  An idle lane contributes exact identity, so the matrix degenerates cleanly.

## FIR modes — `felitronics::lineareq` matrix engine (core v0.2.0)

- **Basis detection** per snapshot: if no band has an active L or R lane → the matrix is
  `[[a,b],[b,a]]` → diagonal in M/S → **2 IRs (today's path, bit-compatible)**. If no band has
  active M or S lanes → diagonal in L/R → 2 IRs. Otherwise → **4 IRs** `h_LL h_LR h_RL h_RR`;
  `yL = h_LL∗xL + h_LR∗xR`, `yR = h_RL∗xL + h_RR∗xR` (≈ 2× stereo convolution cost).
- **Linear phase stays truly linear-phase:** replace every *scalar lane* response by its
  zero-phase magnitude `|H_lane|`, then compose the matrix. All entries are then **real** on the
  FFT grid ⇒ every IR is symmetric ⇒ exact linear phase, no entrywise-|·| fallacy (taking the
  magnitude of matrix *entries* would break M/S reconstruction — entries are signed).
- **Natural phase:** apply the φ = k·φ_min cepstral blend to each scalar lane response, then
  compose (complex entries, 4 general IRs). Same `k` knob, same latency story as today.
- **Click-free edits:** the convolution engine's IR swap generalizes to an **IR-set swap** — all
  2 or 4 IRs crossfade as one atomic set (never mix old h_LL with new h_LR).
- Mono in FIR modes: Mid IR only (unchanged).

## Adapter — `src/Parameters` v3 + processor

Per band `b`, lane keys `k ∈ {st, l, r, m, s}`:

```
band{b}_on        Bool    "B{n} On"
band{b}_type      Choice  "B{n} Type"      (9 items, unchanged)
band{b}_swept     Bool    "B{n} Swept"
band{b}_bypass    Bool    "B{n} Bypass"          — whole point
band{b}_{k}_on    Bool    "B{n} {Lane} On"        st default true, rest false
band{b}_{k}_freq  Float   "B{n} {Lane} Freq"      20..20k skew 1k, default 1000
band{b}_{k}_q     Float   "B{n} {Lane} Q"         0.05..40 skew 1, default 1
band{b}_{k}_gain  Float   "B{n} {Lane} Gain"      −24..24, default 0
band{b}_{k}_slope Choice  "B{n} {Lane} Slope"     7 items, default 12
band{b}_{k}_byp   Bool    "B{n} {Lane} Bypass"
```

- Lane display names: `Stereo/Left/Right/Mid/Side`; for the `st` lane the lane word is dropped
  from names (`B4 Freq`) so unsplit automation reads like a normal EQ.
- **Groups:** one `AudioProcessorParameterGroup` per band ("Band 4"), all 34 of its params
  inside — the Pro-Q-style DAW tree.
- **Non-parameter state** (per-band `ValueTree` properties, saved with the session, not
  automatable): `linkFq`, `linkQ`, `activeLane` (pure UI). Global properties: `defaultLinkFq`,
  `defaultLinkQ` (View menu) — seeds for newly split points. `msFreqLink` dies.
- **Link mirroring lives in the processor** (APVTS listener, message thread, re-entrancy
  guard): `linkFq` mirrors freq across the point's enabled lanes; `linkQ` mirrors Q — or slope
  when the shared type is HP/LP/Notch-like. Mirrors follow the *edited* lane (the last writer
  wins; no master lane). Works headless — automation and generic-editor edits mirror too.
- `readBand()` packs the 5 lanes straight into `BandParams` v2. Solo (`S`) band-passes at the
  **active lane's** freq. Analyzer domains (ST ch0 / Mid / Side) unchanged.

### State migration — `stateVersion` 2 → 3

In `setStateInformation`, when the incoming tree carries version ≤ 2, translate values:

| v2 state                  | v3 result                                                        |
|---------------------------|------------------------------------------------------------------|
| `ms = false`              | `st` lane ← flat fields; other lanes off                         |
| `ms = true`               | `m` lane ← flat fields, `s` lane ← `s*` fields, `st` off         |
| `bandN_bypass`            | point `bypass` (and NOT the lane's — semantics match: it muted the whole band) |
| editor `msFreqLink` prop  | `linkFq = old value` on every migrated split band; also becomes `defaultLinkFq` |
| `sOn/sBypass`             | `s` lane `on/bypass`                                             |

Old automation lanes in existing DAW projects break (accepted — pre-release, decision #9).
A v3 state loaded by an old binary simply resets (also accepted, pre-release).

## UI — `src/ui/*`

- **Strip:** the `ST/M·S` mode button + `[M][S]` tabs are replaced by one dropdown button
  (shows a venn glyph of the enabled set + a dot in the active lane's colour). Everything else
  in the strip stays; the strip gets *simpler* and keeps its fixed width.
- **Menu** (custom PopupMenu rows so it stays open on checkbox clicks):
  `☑ [venn] ● Stereo · Left · Right · Mid · Side` — checkbox toggles (last one refuses with a
  subtle shake/flash), name-click = enable-if-needed + set active + close, active row
  highlighted, Alt-click = lane solo. Separator. `☑ Link FQ`, `☑ Link Q`. On non-stereo buses
  the four domain rows are dimmed with a "stereo only" hint.
- **Venn icons** (FilterShapes-style vector paths): two overlapping circles; L = left disc,
  R = right disc, ST = both bold, M = the lens, S = the two crescents.
- **Canvas:** node per enabled lane. ≥ 2 lanes → lane colours + permanent lane badge
  (`st/l/r/m/s`, small chip, lane-coloured, glued to the node's side); node number stays the
  band number. Per-lane curves (under the existing View → band curves toggle) draw in dimmed
  lane colour. Selection: click node → selects point + lane; `<` `>` walk all visible nodes in
  frequency order; readout style `3m  1.24 kHz  +3.5 dB  Q 2.0`.
- **Axes:** main composite as today; additional dimmed axis composites only where some band has
  a differing lane on that axis (decision #7). Beyond-Nyquist fog, floor dive, bottom axis strip
  — all shipped behaviour unchanged.

## Tests

**Core (`felitronics_eq_tests`), measured == analytic as always:**
1. **Legacy equivalence (regression guarantee):** a `{st}`-only band ≡ old Stereo path; a
   `{m,s}` band ≡ old M/S path — same input, bit-exact output vs. reference vectors captured
   from the v0.1.x engine before the change.
2. **Lane axis purity:** L-only band leaves ch1 bit-exact; R-only leaves ch0; M-only preserves
   `L−R`; S-only preserves `L+R` (extends the shipped delta-fold identity tests).
3. **The key new one — matrix truth:** for random mixed lane sets (incl. all-five), the measured
   stereo response (both channels, both cross terms — probe L-only then R-only input) matches
   the analytic 2×2 `H_eq` at every probe frequency.
4. Topology/reset: lane toggles, type change, ST↔split switches — no clicks/NaN; mono and 6-ch
   inputs run ST-only; swept honored only unsplit.
5. **FIR:** matrix path measured == analytic |H_eq| per entry; linear-phase symmetry of all 4
   IRs; degeneracy detection picks the 2-IR path on all-M/S and all-L/R sets and matches the
   matrix prediction bit-for-bit... (within convolution epsilon); IR-set swap atomicity.
6. **Adapter (`tests/`):** v2→v3 migration fixtures (plain, ms-split, bypassed, linked);
   link mirroring on the message thread incl. HP/LP slope mirroring; group layout sanity;
   auval in CI as today.

**Live:** standalone smoke per UI PR (screenshots: split point with badges, menu states,
axes) — same drill as the toolbar branch.

## Phasing (each PR: build all formats + ctest + auval + crew adversarial review)

- **A — fcore `eq` lanes** (BandParams v2, EqBand, laneView/composite/matrix response, tests 1–4).
  Plugin keeps building against the *pinned* v0.1.5 until C lands (sibling dev builds move first).
- **B — fcore `lineareq` matrix FIR** (matrix builder, IR-set swap, tests 5) → tag **v0.2.0**.
- **C — tabby adapter** (schema v3 + groups + migration + link mirroring + `readBand`, pin bump
  to v0.2.0, mechanical UI port so everything compiles & auval passes; canvas still renders the
  lanes it knows). `main` stays green.
- **D — tabby lanes UI** (menu, venn icons, badges/colours, stepping, wheel-cycle, alt-solo,
  axis composites, mono dimming) + live verification.

## Risks

- **CPU:** worst case 5 lanes × 24 bands × sections. Mitigated: inactive lanes cost zero
  (skip + recompute-skip), matched design is cheap, and the 4-IR path only engages on genuinely
  mixed sets. Profile a 24-band all-five session in A and D.
- **Param count (~820):** grouped; names self-sufficient; verified in Live/Logic/Reaper during C.
- **Migration fidelity:** fixtures in tests 6 are written from *real* v2 session XML dumped
  before the schema change, not hand-built trees.
- **UI clutter:** badges only on split points; lane curves dimmed + behind the existing View
  toggles; axes only-when-differing.
