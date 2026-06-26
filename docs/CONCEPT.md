# TabbyEQ — Concept & Design (working doc)

> Status: working draft. Started 2026-06-26. This is the local technical brain for the
> TabbyEQ plugin (Felitronics line, by Darwin's Cat). Decisions get logged here as we make
> them; raw research/panels live next to it.

## One-liner

An **educational semantic parametric EQ**. You don't dial freq/Q directly — you pick a
**SOURCE** (voice / guitar / bass) and a named **TRAIT** (mud, nose, presence, air, fizz…).
TabbyEQ maps the trait to a real filter (type / freq / Q / gain) and **shows you the actual
values** as you turn the one "amount" knob — so you learn the frequency cheat-sheet by using
it, instead of memorising tables.

The killer feature is the **search → treat** workflow for resonances you can't write down as a
constant (e.g. a vocalist's nasal "nose"): click the trait → the spectrum zooms to its range →
a narrow bell at +12 lets you sweep and *find* the offending resonance by ear → once found, it
flips to a musical cut at the found frequency.

## Naming / brand

- **Product name: TabbyEQ.** Canon EQ name from `darwinscat.com/doc/guitar-rig-naming.md`
  (Table 2, verdict). "Tabby" = полосатый кот = frequency *bands* (stripes); bonus for a
  teaching tool — stripes you can *see*.
- **Alt held in pocket: Pinna** (the outer-ear that shapes incoming sound — fits the
  "learn to *hear*" angle and the cat-anatomy axis). Decide after domain/TM check.
- **Line:** Felitronics (🔒 prod umbrella). **Repo:** `tabby-eq` (lowercase-kebab, like
  `loki-plugin`). **Independent repo**, history from scratch; the build scaffold is **copied**
  from `orbitcab` (CMake/FetchContent/CLAP/CI/SPDX/tests) — *not* a git fork.

## Decisions locked (2026-06-26)

| # | Decision | Choice |
|---|---|---|
| 1 | Architecture | Normal **N-band EQ engine** (real freq/Q/gain/type = the automatable state) + **semantic macro UI** on top. Band count TBD (6–8). |
| 2 | Search listening mode | **Both** boost-sweep AND solo-bandpass; **solo = default** for beginners; gain-compensated monitor for safety. |
| 3 | Filter coefficients | **Matched biquads (Vicanek 2016)** from day one (honest curves near Nyquist). **TPT SVF** for the swept/search band + HPF/LPF. |
| 4 | Sources v1 | Male voice, Female voice, E-gtr **clean**, E-gtr **dist**, Acoustic, Bass. **Drums → v1.1** (per-piece: kick/snare/OH). |
| 5 | Axes | **Source (timbre) × optional Role (rhythm↔lead)**. Role re-weights/re-orders traits + flips default actions, reusing the same freq map. **No genre/style axis in v1.** |
| 6 | Vocab format | **Hand-authored C++ table** (`src/Vocab.h`) = the source of truth. Zero build deps, compiler-validated, reads like the data. (Dropped the YAML→codegen/Python idea as overkill for ~6 sources.) `docs/VOCABULARY.md` stays the human reference; export to JSON/YAML later only if a web tool needs it. |
| 7 | Band model | **Trait = its own labelled band.** Pool of ~12 biquads under the hood; the selected source's `core` traits appear as dedicated labelled bands, `opt` traits light up spare bands. DAW sees a normal multiband EQ; automation stays clean. |
| 8 | Freq/Q access | **Advanced-unlock.** Default UI = the named **amount** knob + the real freq/Q/gain shown read-only as *info*. An `advanced` toggle reveals editable freq/Q for power users. |

## DSP / Nyquist plan

- **matched biquad** coefficients (Vicanek) default → magnitude matches the analog prototype
  near Nyquist; shelves/bells in the air band stop cramping. Same runtime cost as RBJ.
- **Optional 2× oversampling** as a "HQ" toggle — *low priority* for a purely linear EQ
  (matched coeffs already fix the shape; OS matters when there's saturation).
- **Clamp** `fc ≤ ~0.49·fs`; a high-shelf whose corner exceeds Nyquist degrades gracefully.
- **Swept band on `StateVariableTPTFilter`** (survives fast fc modulation during the hunt);
  smooth gain/freq to kill zipper noise.
- **Teaching toggle:** draw the realized `H(z)` vs the analog target so cramping is *visible*.
- Both panel models (DeepSeek, Codex) independently flagged air-band cramping at 44.1/48k →
  confirms the matched-shelf choice.

## Search → treat workflow

1. Click a trait with `search: true` → spectrum **zooms** to its range; a **narrow bell at
   +12** (search) appears, locked to the range.
2. **Two aids:** boost-sweep / **solo-bandpass (default)**. Monitor is **gain-compensated**
   (minus the boost) so beginners don't get blasted.
3. Optional **"reveal peak"** hint (max FFT bin in the band) — opt-in, not auto (the point is
   to train the ear).
4. On **"found"**: carry the **found frequency**, flip polarity to **cut**, gain → −3 default,
   Q changes **search→treat** (wider/musical). Only the frequency carries over, not the Q.

## Data model

Factor into a **global trait dictionary** (the *concept* + general behaviour) + **per-source
overrides** (freq center, useful range, relevance, default Q/action). Teaches "mud is always
mud — it just lives at a different freq per instrument." See `vocab/tabby-vocab.draft.yaml`
for the schema and the two pilot sources encoded.

```yaml
traits:                       # global concepts
  nose: { concept: "нос/гнусавость", type: bell, find: true }
sources:
  voice_male:                 # per-source = only the map overrides
    traits:
      nose: { search: [800,1800], searchQ: 6, treatQ: 2.5, action: cut }
```

## Vocabulary status

**All 6 v1 sources drafted** (Voice M/F, E-gtr clean/dist, Acoustic, Bass) via a
DeepSeek+Codex panel + human verdict → `docs/VOCABULARY.md` (verdict reference),
`docs/VOCABULARY-PILOT.md` (pilot methodology + detailed comparison), encoded in
`vocab/tabby-vocab.draft.yaml` (source of truth). **Gemini pending** — its idea-generation
gets folded in once the `agy` backend is wired.

## Open / next

- **Step 3 (UX):** band count/model + whether freq/Q are hidden / read-only / advanced-unlock.
- **Step 2:** scaffold the `tabby-eq` repo (copy orbitcab CMake/CLAP/CI/SPDX/tests; new git history).
- Audition open divergence: acoustic pick-noise center (split into `pick` ~2k + `squeak` ~7k).
- Wire Gemini: add `~/.local/bin` to PATH (done in `~/.zshrc`), `agy` sign-in, restart Claude;
  then re-run the panel for its `[IDEA]` traits.
