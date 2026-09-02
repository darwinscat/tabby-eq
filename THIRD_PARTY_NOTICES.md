# Third-party notices

TabbyEQ is AGPL-3.0-or-later. Bundled / fetched third-party components and their licenses:

| Component | Use | License | Notes |
|---|---|---|---|
| **JUCE** 8.0.14 | framework (fetched via CMake) | AGPLv3 (our option) | Free, no revenue cap. This repo being AGPL + source-public *is* the JUCE compliance — no key/flag. |
| **clap-juce-extensions** | CLAP wrapper (slice 2) | MIT | Pinned commit, like JUCE. To be added with the plugin target. |
| **Michroma** (font) | header wordmark (embedded via `TabbyEQData`) | SIL OFL 1.1 | © The Michroma Project Authors (https://github.com/googlefonts/Michroma-font). OFL text bundled at `resources/fonts/Michroma-OFL.txt`. OFL §1 permits embedding in software under a different license (AGPLv3 here); the font itself stays OFL. Same asset as OrbitCab. |
| **pffft** (via felitronics-core's `fftpffft` module) | the analyzer's SIMD FFT — **only in builds configured with `-DTABBYEQ_WITH_PFFFT=ON`** (default OFF: the core's own scalar FFT, no third-party code) | BSD-style (FFTPACK5 / UCAR) | © 2013 Julien Pommier, based on FFTPACKv4 by Dr Paul Swarztrauber (NCAR, 1985); © 2004 UCAR. The licence requires the notice below to accompany binary distributions — it does, here. Audio processing never uses it; only the spectrum display does. |

### pffft / FFTPACK notice (binaries built with `TABBYEQ_WITH_PFFFT`)

Copyright (c) 2013 Julien Pommier (pommier@modartt.com). Based on original fortran 77 code from
FFTPACKv4 from NETLIB (http://www.netlib.org/fftpack), authored by Dr Paul Swarztrauber of NCAR, in
1985. As confirmed by the NCAR fftpack software curators, the following FFTPACKv5 license applies to
FFTPACKv4 sources. Julien Pommier's changes are released under the same terms.

FFTPACK license: http://www.cisl.ucar.edu/css/software/fftpack5/ftpk.html

Copyright (c) 2004 the University Corporation for Atmospheric Research ("UCAR"). All rights reserved.
Developed by NCAR's Computational and Information Systems Laboratory, UCAR, www.cisl.ucar.edu.

Redistribution and use of the Software in source and binary forms, with or without modification, is
permitted provided that the following conditions are met:

- Neither the names of NCAR's Computational and Information Systems Laboratory, the University
  Corporation for Atmospheric Research, nor the names of its sponsors or contributors may be used to
  endorse or promote products derived from this Software without specific prior written permission.
- Redistributions of source code must retain the above copyright notices, this list of conditions,
  and the disclaimer below.
- Redistributions in binary form must reproduce the above copyright notice, this list of conditions,
  and the disclaimer below in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING, BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS WITH THE SOFTWARE.

## Algorithms (method, not code)

- **Matched second-order digital filters** — the coefficient design in `teq/include/teq/MatchedBiquad.h`
  implements the closed-form method from **Martin Vicanek, "Matched Second Order Digital
  Filters" (2016)** and its companion "Matched Two-Pole Digital Shelving Filters". The formulas
  are a published mathematical method (cited inline in the header); no third-party source code
  was copied.
- The **RBJ Audio EQ Cookbook** formulas (Robert Bristow-Johnson) appear in `MatchedBiquad.h`
  **only** as the baseline the unit test measures the matched design against — also a published
  method, not copied code.

## First-party

The **Darwin's Cat** logo (`resources/brand/logo-darwinscat.svg`, embedded via `TabbyEQData` and
shown in the header blister) and the **TabbyEQ** name/marks are © Darwin's Cat / Oleh Tsymaienko &
Alisa. The stripe-cat "spectrum" mark is drawn programmatically (`src/ui/BrandMark.h`); the same
mark also ships as the app/bundle icon (`resources/icon/`, fed to JUCE's `ICON_BIG`). These are
**not** covered by the AGPLv3 grant on the source code — trademarks and brand assets are reserved.
