# TabbyEQ — Vocabulary v1 (verdict reference)

> Human-readable map for all **6 v1 sources**. Source of truth is
> `vocab/tabby-vocab.draft.yaml` (this doc summarises it). Methodology + the detailed
> DeepSeek-vs-Codex comparison for the two pilots is in `VOCABULARY-PILOT.md`.
>
> **Provenance:** all maps drafted 2026-06-26 via a **DeepSeek (v4-pro, reasoning) + Codex
> (gpt-5.2)** panel + human verdict. **Gemini pending** (agy backend needs sign-in + Claude
> restart — see CONCEPT.md). Both models independently flagged air-band cramping → confirms
> the matched-shelf plan.

Legend: **★** = search trait (zoom + sweep to find), **·opt** = behind a "more" reveal.
Freq = nominal center (Hz); search traits also carry a sweep `range` (see YAML).

## Voice — Male
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 80 | cut | |
| mud | bell | 280 | cut | |
| nose | bell | 1000 | cut | ★ |
| presence | bell | 3000 | boost | |
| harsh | bell | 4000 | cut | ★ |
| sibilance | bell | 7000 | cut | ★ |
| air | h-shelf | 12000 | boost | |
| body·opt 200 (low-shelf, either) · boxy·opt 500 (cut) | | | | |

## Voice — Female
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 90 | cut | |
| mud | bell | 300 | cut | |
| nose | bell | 1000 | cut | ★ |
| presence | bell | 4500 | boost | |
| harsh | bell | 3500 | cut | ★ |
| sibilance | bell | 7500 | cut | ★ |
| air | h-shelf | 12000 | boost | |
| body·opt 190 · boxy·opt 520 ★ · bite·opt 2500 | | | | |

*vs Male: presence + sibilance shifted up (~4.5k / ~7.5k); nose ~same.*

## Electric Guitar — Clean
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 80 | cut | |
| mud | bell | 300 | cut | |
| nose | bell | 1100 | cut | ★ |
| bite | bell | 2400 | boost | |
| harsh | bell | 3300 | cut | ★ |
| presence | bell | 4000 | boost | |
| icepick | bell | 6000 | cut | ★ |
| body·opt 180 · boxy·opt 500 ★ · growl·opt 700 · cabtop·opt LPF 9000 · air·opt 11000 | | | | |

*Clean = bright/musical highs: ice-pick (search cut) for brittle spikes, air/sparkle as boost — no fizz problem.*

## Electric Guitar — Dist (rhythm)
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 90 | cut | |
| chunk | bell | 120 | either | ★ |
| mud | bell | 300 | cut | |
| nose | bell | 1000 | cut | ★ |
| presence | bell | 3000 | boost | |
| icepick | bell | 3800 | cut | ★ |
| fizz | bell | 6500 | cut | ★ |
| cabtop | LPF | 10000 | cut | |
| boxy·opt 500 ★ · growl·opt 750 · bite·opt 1800 · air·opt 9000 | | | | |

## Acoustic
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 75 | cut | |
| boom | bell | 130 | cut | ★ (soundhole — flagship) |
| mud | bell | 270 | cut | |
| pick | bell | 2000 | either | ★ |
| bite | bell | 2800 | boost | |
| harsh | bell | 4000 | cut | ★ |
| sparkle | bell | 9000 | boost | |
| air | h-shelf | 13000 | boost | |
| body·opt 130 · boxy·opt 500 ★ · nose·opt 1000 · squeak·opt 7000 ★ | | | | |

*Air-heavy as expected: sparkle + air both boost targets.*

## Bass
| Trait | Type | Freq | Action | |
|---|---|---|---|---|
| rumble | HPF | 30 | cut | |
| sub | low-shelf | 50 | either | |
| weight | bell | 90 | boost | |
| mud | bell | 250 | cut | |
| growl | bell | 900 | boost | ★ (grind — flagship) |
| clank | bell | 2800 | either | ★ (string/fret) |
| squeak | bell | 5000 | cut | ★ |
| body·opt 140 · boxy·opt 450 ★ · bite·opt 2000 · presence·opt 4000 · air·opt 10000 · cabtop·opt LPF 9000 | | | | |

---

## Panel notes (4 new sources)

- **Strong agreement** on the skeleton again; divergences ±20–30% on a few centers → musical
  middles taken.
- **Codex** contributed: explicit **cab-top LPF** for clean/dist gtr + bass; finer search
  flags (boxy, proximity); a vocal **hiss/top LPF** (18k, dropped from v1 — niche).
- **DeepSeek** cleaner trait economy + descriptions.
- **Flagship search traits per source confirmed:** vocal **nose** · dist **ice-pick/fizz** ·
  acoustic **soundhole boom** · bass **growl + clank**. Exactly what search→treat is built for.
- **Open divergence to audition:** acoustic pick-noise center (DeepSeek ~1.2k "click" vs Codex
  ~4.2k "attack") — I split it: `pick` ~2k (attack) + `squeak` ~7k (mechanical). Verify by ear.

Next vocab pass: fold in Gemini's `[IDEA]` traits once its backend is wired.
