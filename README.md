# TabbyEQ

**An EQ that teaches you EQ.** Instead of dialing raw frequency and Q, you pick a **source**
(male/female voice, electric guitar clean/dist, acoustic, bass) and a **named trait** — *mud,
nose, boxy, presence, air, fizz, ice-pick…* — and TabbyEQ maps it to the right filter while
**showing you the real numbers** as you turn. The frequency cheat-sheet every engineer keeps
googling, built into the tool.

Its signature move is **search → treat**: for resonances you can't write down as a constant
(a vocalist's nasal "nose", a guitar's fizz), click the trait — the spectrum zooms to that
range and a narrow boost lets you *sweep and find it by ear*, then it flips to a musical cut at
the frequency you found.

Under the hood it's an honest multiband EQ: **Vicanek matched biquads** (so the air band
doesn't cramp near Nyquist), a TPT state-variable filter for the live sweep, and a spectrum
analyser. The "named" layer is a UI macro on top — your DAW still sees a normal automatable EQ.

Felitronics line, by **Darwin's Cat**. Ships **VST3 + AU + CLAP** (planned).

## Status

Early. The DSP core (matched biquads + the cramping test) builds today:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target TabbyEQ_Tests -j
```

Design and the trait data live in [`docs/CONCEPT.md`](docs/CONCEPT.md) and
[`vocab/`](vocab/).

## License

**AGPL-3.0-or-later** (see `LICENSE`). JUCE is used under its AGPLv3 option. The TabbyEQ /
Darwin's Cat names and logos are reserved, not under the code license.
