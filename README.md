# TabbyEQ

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](LICENSE)
[![CI](https://github.com/darwinscat/tabby-eq/actions/workflows/ci.yml/badge.svg)](https://github.com/darwinscat/tabby-eq/actions/workflows/ci.yml)

**A production EQ you drive by intent.** It's a full, honest parametric EQ — real
frequency / Q / gain, **always visible and editable** — with a **semantic helper** on top: pick
a **source** (male/female voice, electric guitar clean/dist, acoustic, bass) and name the
**problem** — *mud, boxy, nose, harsh, presence, air, fizz, ice-pick…* — and TabbyEQ lands the
right band, Q and filter type instantly. You stop hunting for "which frequency, how wide" and
work at the speed you hear.

Its signature move is **search → treat**: for resonances you can't write down as a constant
(a vocalist's nasal "nose", a guitar's fizz), click the trait — the spectrum zooms to that
range and a narrow boost lets you *sweep and find it by ear*, then it flips to a musical cut at
the frequency you found.

Under the hood it's an honest multiband EQ: **Vicanek matched biquads** (so the air band
doesn't cramp near Nyquist), a TPT state-variable filter for the live sweep, a spectrum
analyser, and mono → surround support (up to 16 channels / 9.1.6 Atmos). The semantic layer is a
**helper on top of a standard quality EQ — never a wall in front of the controls** — and your
DAW still sees a normal automatable EQ. *(You'll soak up the frequency cheat-sheet as a side
effect — a bonus, not the point.)*

Felitronics line, by **Darwin's Cat**. Ships **VST3 + AU + CLAP + Standalone**.

## Status

Working plugin. The matched-Nyquist DSP core (JUCE-free, 204 tests) and a playable 12-band
plugin with a classic editor — log grid, live spectrum, draggable nodes, mono → 16-channel
surround — build and pass `auval`. The **semantic helper layer** (source/trait pickers,
search → treat) is what's next.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release   # core is JUCE-free
cmake --build build --target teq_tests -j   # run the engine tests
```

Design and the trait data live in [`docs/CONCEPT.md`](docs/CONCEPT.md) and
[`docs/VOCABULARY.md`](docs/VOCABULARY.md).

## License

**AGPL-3.0-or-later** (see `LICENSE`). JUCE is used under its AGPLv3 option. The TabbyEQ /
Darwin's Cat names and logos are reserved, not under the code license.
