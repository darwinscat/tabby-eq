# TabbyEQ — Vocabulary pilot (Voice-M + E-Gtr-Dist)

> Panel-driven draft of the trait→frequency maps for two pilot sources, used to validate the
> schema and the multi-LLM process before doing all of v1. Panel run 2026-06-26.
> Process mirrors `guitar-rig-naming.md` Table 2: ask each model the same schema, tabulate,
> then a human verdict.

## Panel

| Model | Backend | Result |
|---|---|---|
| **DeepSeek** | `deepseek-v4-pro`, reasoning=high | ✅ full tables, conservative, clean descriptions |
| **Codex** | `gpt-5.2` | ✅ full tables, added LPF/cab-top + finer search flags |
| **Gemini** | `agy` (Antigravity CLI) | ❌ backend not installed — skipped (least reliable anyway) |

Both models **independently flagged air-band cramping** near Nyquist at 44.1/48k → confirms
the matched-shelf / oversampling plan.

Columns: **type** · **freq** (nominal center) · **range** [min–max] · **Q** (searchQ→treatQ for
search traits) · **action** · **★=search trait** (hunt by sweeping).

---

## Source A — Male lead vocal (close-mic, pop/rock)

| Trait | DeepSeek (freq/Q) | Codex (freq/Q) | **Verdict** (type, freq, range, Q, action) | Notes |
|---|---|---|---|---|
| rumble (HPF) | 80 / 12dB | 80 / 0.7, 12–18dB | **HPF 80, [50–120], 12·or·24 dB/oct, cut** | agree. Plosive folds in here (skip own trait). |
| body / warmth *(opt)* | — | 220 bell either | **low-shelf 200, [150–300], either** | adds weight to thin voices. |
| mud | 250 / 1.0 cut | 320 / 1.1 cut | **bell 280, [200–400], Q1.0, cut** | split the difference. |
| boxy | 450 / 1.5 cut | 520 / 1.4 cut ★ | **bell 500, [350–700], Q1.4, cut ★(mild)** | room/mic resonance → mild search. |
| **nose / honk** | 1200 / 3.0 cut ★ | 950 / 2.0 cut ★ | **bell 1000, [800–1800], searchQ 6→treatQ 2.5, cut ★** | flagship search trait. |
| presence | 3000 / 0.8 boost | 3200 / 0.8 boost | **bell 3000, [2500–5000], Q0.8, boost** | agree ~3k. |
| harshness | 3500 / 2.0 cut ★ | 4300 / 2.2 cut ★ | **bell 4000, [3000–6000], Q2.0, cut ★** | overlaps presence (boost vs cut) — good teaching point. |
| sibilance | 7000 / 2.0 cut ★ | 7200 / 3.0 cut ★ | **bell 7000, [5500–10000], searchQ hi, cut ★** | static for v1; dynamic de-ess later. |
| air | h-shelf 12000 boost | h-shelf 12000 boost | **high-shelf 12000, [8000–16000], Q0.7, boost** | agree. Cramping → matched shelf. |

## Source B — Distorted electric guitar (rhythm, high-gain, mic'd 4×12)

| Trait | DeepSeek (freq/Q) | Codex (freq/Q) | **Verdict** (type, freq, range, Q, action) | Notes |
|---|---|---|---|---|
| rumble / tighten (HPF) | 100 / 12dB | 80 / 12–24dB | **HPF 90, [60–150], 12·or·24 dB/oct, cut** | high-gain rhythm. |
| chunk / palm-mute | 120 / 1.0 boost ★ | 140 / 0.9 either | **bell 120, [90–180], Q1.0, either(boost) ★(mild)** | depends on tuning/cab → mild search. |
| mud | 300 / 0.8 cut | 280 / 1.0 cut | **bell 300, [200–450], Q0.9, cut** | agree ~300. |
| boxy / cab | 500 / 1.5 cut | 500 / 1.5 cut ★ | **bell 500, [350–700], Q1.5, cut ★(mild)** | cab coloration varies. |
| growl / wood *(opt)* | — | 750 either | **bell 750, [600–950], either** | woody mid character. |
| **honk / nasal** | 1000 / 3.0 cut ★ | 1100 / 2.0 cut ★ | **bell 1000, [800–1500], searchQ 6→treatQ 2.5, cut ★** | wah-like cab/mic peak. |
| bite / definition *(opt)* | — | 1800 either | **bell 1800, [1400–2400], either** | cut-through in a dense mix. |
| presence | 2500 / 0.7 boost | 3200 / 0.9 boost | **bell 3000, [2000–4000], Q0.8, boost** | pick attack. |
| **ice-pick** | (in harsh) 3500 ★ | 3900 / 2.5 cut ★ | **bell 3800, [3000–5000], searchQ→Q2.5, cut ★** | flagship dist search trait. |
| **fizz** | 6000 / 2.5 cut ★ | 7200 / 2.5 cut ★ | **bell 6500, [4500–9000], searchQ→Q2.5, cut ★** | distortion sizzle byproduct. |
| cab-top (LPF) | (mentions LPF) | LPF 10000 cut | **LPF 10000, [7000–12000], 12·or·24 dB/oct, cut** | Codex's catch — kills fizz extension. |
| air *(opt, rare)* | h-shelf 10000 boost | h-shelf 8500 either | **high-shelf 9000, [7000–11000], either** | rare on high-gain; often LPF instead. |

---

## Reading the panel (my take)

- **Strong structural agreement.** Both models land the same trait skeleton; divergences are
  ±20–30% on a few centers (mud 250↔320, harsh 3.5↔4.3k, fizz 6↔7.2k) — I took musical
  middles. No contradictions on *type* or *action*.
- **Codex contributed two things worth keeping:** (1) an explicit **LPF/cab-top** for dist
  guitar (DeepSeek only hinted), and (2) more **search flags** on resonances that genuinely
  vary take-to-take (boxy, boom). Folded both in.
- **DeepSeek** was cleaner on descriptions and more conservative on trait count — good base.
- **Three flagship search traits validated:** vocal **nose** (~1k), guitar **ice-pick**
  (~3.8k) and **fizz** (~6.5k). These are exactly the "can't-write-it-down, hunt-by-ear"
  resonances the search→treat workflow is built for.
- **Optional traits** (body, growl, bite) are tagged `*(opt)*` — keep them behind a "more"
  reveal so the default trait set stays beginner-legible.

Next: same panel on Voice-F, E-Gtr-clean, Acoustic, Bass; add Gemini once `agy` is installed.
