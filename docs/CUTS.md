<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
# TabbyEQ — Cuts as boundaries: HP/LP zones + Mono Bass (PROPOSAL)

> ⚠️ **STATUS: PROPOSAL ONLY — NOTHING IN THIS FILE IS AGREED.** Started 2026-07-02 from a raw
> discussion during the lanes crew review. No design round has happened, Oleh has approved NO item
> below. A future session MUST NOT treat anything here as a decision or instruction. When (if)
> items get approved, they will be moved into an explicit "Agreed" section with Oleh's sign-off
> noted per item. Depends on the shipped lanes feature (LANES.md).

## Agreed (explicit sign-off only)

**A1 (Oleh, 2026-07-02): every restriction lives in the UI, never in the data model.** Bands stay
homogeneous inside — any band can be a cut; NO hardcoded rules like "only one HPF" in the DSP,
state or serialization; if a "single cut" (or drag) restriction is ever chosen, the UI simply
refuses to create/perform it. His words: «все ограничения типа "HPF только влево-вправо" /
запреты держать на уровне UI, а не в модели данных. Внутри — полосы однородные, любая может быть
срезом; UI просто не даёт создать второй срез».

**Everything else in this file remains UNDECIDED.** Oleh is deliberately still forming the vision
through discussion and will deliver a concrete short directive when it crystallizes. Do NOT
advance this doc to a crew round or implementation until that directive arrives.

## What Oleh actually said (the source material — his positions, still open for his own revision)

- Vertical-dragging an HPF/LPF node makes no sense to him; two HPFs also feel pointless.
  (Context he compared: FabFilter allows vertical drag + many cuts; Neutron forbids both; Cubase
  Frequency 2 allows the drag but it does nothing; Cubase stock EQ treats cuts as zones.)
- Splitting a Notch across lanes "feels silly"; splitting HPF/LPF too.
- He is seriously considering a DEDICATED mono-bass filter kind.
- He is seriously considering a dedicated HP/LP presentation: a filled/shaded zone, a different
  node type, and a vertical draggable dashed line through the cut points — "это реально другое",
  as is mono bass.

## Claude's proposals (UNCONFIRMED — for discussion, not instructions)

Numbered so Oleh can approve/reject/edit item by item.

**P1. No vertical drag on cuts; keep drag=freq, whiskers=slope, wheel=Q where it exists.**
Rationale offered: Pro-Q's vertical drag is resonance, and our Butterworth cascades only honour Q
at 6/12 — mapping the gesture to a sometimes-Q would be inconsistent. (Note: this aligns with
Oleh's stated instinct, but he has not signed off a final rule.)

**P2. No count limit on cuts.** Rationale offered: homogeneous band slots; lanes make multiple
cuts idiomatic (Side-LP + ST-LP). Oleh's instinct leaned the OTHER way ("2 HPF не имеют смысла")
— this is genuinely open.

**P3. Lane-splitting of cuts/notch stays available but unadvertised.** Oleh called it silly;
Claude argued the capability backs Mono Bass and mastering moves. Open.

**P4. Mono Bass as a macro ROLE over an `{s}`-only HighPass point** (state property
`band{b}_role="monoBass"`; honest EQ params underneath; special presentation on top) — rather
than a new core FilterType. Trade-off written up for discussion; Oleh only said he wants a
dedicated "вид фильтра", he did NOT choose macro-vs-core-type.

**P5. Zone rendering sketch** (all details unconfirmed): cool-tinted gradient shading beyond the
cutoff; full-height dashed cutoff line as the primary horizontal drag target; distinct node glyph
on the 0 dB line; nested cuts render outermost-strongest; split-cut zones only while selected;
Mono Bass gets a "narrowed, not gone" texture below the crossover instead of the discard tint.

**P6. Mono Bass strip mode**: readout "Mono Bass 120 Hz 24 dB/oct", freq+slope only, role glyph
on the lane button; creation via the add menus. Defaults (120 Hz? slope 12 vs 24?) undecided.

**P7. Core roadmap note**: resonant Q for HP/LP cascades (would make vertical drag meaningful) —
record as deferred; no core work in this feature otherwise.

## Open questions (nobody has answered these yet)

1. Zone visibility for unselected plain cuts — always, or hover/selected-only?
2. Dashed lines for all cuts simultaneously, or selected-only past N cuts?
3. Mono Bass default frequency and slope?
4. Macro-role vs real core FilterType for Mono Bass (P4 is only a proposal).
5. Does Mono Bass enter the Vocab/Helper tables now or in Phase 3?
6. Zone/texture behaviour in the Fixed-lane placement mode.

## Process

Next step is a DISCUSSION with Oleh item by item (P1–P7 + questions), then the usual crew design
round, and only then implementation (would be PR E; no core, no schema, no migration expected —
unless P4 resolves toward a core type).
