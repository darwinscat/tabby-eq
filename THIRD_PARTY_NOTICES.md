# Third-party notices

TabbyEQ is AGPL-3.0-or-later. Bundled / fetched third-party components and their licenses:

| Component | Use | License | Notes |
|---|---|---|---|
| **JUCE** 8.0.14 | framework (fetched via CMake) | AGPLv3 (our option) | Free, no revenue cap. This repo being AGPL + source-public *is* the JUCE compliance — no key/flag. |
| **clap-juce-extensions** | CLAP wrapper (slice 2) | MIT | Pinned commit, like JUCE. To be added with the plugin target. |
| **Michroma** (font) | header wordmark (embedded via `TabbyEQData`) | SIL OFL 1.1 | © The Michroma Project Authors (https://github.com/googlefonts/Michroma-font). OFL text bundled at `resources/fonts/Michroma-OFL.txt`. OFL §1 permits embedding in software under a different license (AGPLv3 here); the font itself stays OFL. Same asset as OrbitCab. |

## Algorithms (method, not code)

- **Matched second-order digital filters** — the coefficient design in `teq/include/teq/MatchedBiquad.h`
  implements the closed-form method from **Martin Vicanek, "Matched Second Order Digital
  Filters" (2016)** and its companion "Matched Two-Pole Digital Shelving Filters". The formulas
  are a published mathematical method (cited inline in the header); no third-party source code
  was copied.
- The **RBJ Audio EQ Cookbook** formulas (Robert Bristow-Johnson) appear in `MatchedBiquad.h`
  **only** as the baseline the unit test measures the matched design against — also a published
  method, not copied code.
