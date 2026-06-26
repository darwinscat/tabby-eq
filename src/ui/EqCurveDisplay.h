// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <teq/EqBand.h>
#include <teq/SpectrumTap.h>

#include "PluginProcessor.h"

#include <array>

//==============================================================================
// The classic EQ canvas — log-frequency / dB grid, a live post spectrum (FFT of the engine's
// output tap), the composite response curve (computed race-free from the param snapshot), and a
// draggable node per active band. This is the foundation the semantic layer (slice 3+) sits on.
class EqCurveDisplay : public juce::Component, private juce::Timer
{
public:
    explicit EqCurveDisplay (TabbyEqAudioProcessor& p);
    ~EqCurveDisplay() override;

    void paint (juce::Graphics&) override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseExit        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Set by the editor: fires when the selected band changes (-1 = none). Drives the edit strip.
    std::function<void(int)> onBandSelected;
    int  selectedBand() const noexcept { return selBand; }
    void setAnalyzerPre (bool pre) noexcept { analyzerPre = pre; }   // analyzer reads pre- or post-EQ

private:
    void timerCallback() override;

    // coordinate maps
    float  freqToX (double f) const noexcept;
    double xToFreq (float x)  const noexcept;
    float  dbToY   (double db) const noexcept;     // curve gain scale (± kGainRange around centre)
    double yToDb   (float y)   const noexcept;
    float  specDbToY (double db) const noexcept;   // spectrum dBFS scale

    void   refreshDesigns();                       // pull the 12 bands into the cache + design them
    double compositeDb (double f) const noexcept;  // total response (dB) from the cache
    juce::Point<float> nodePos (int band) const noexcept;
    std::pair<juce::Point<float>, juce::Point<float>> whiskerEnds (int b) const noexcept;   // Q-handle positions
    juce::Point<float> addButtonAt() const noexcept;   // the "+" on the curve under the cursor, or {-1,-1}
    int    nodeAt (juce::Point<float> p) const noexcept;   // band index under p, or -1
    void   setParam (const juce::String& id, double value);
    void   setParamGestured (const juce::String& id, double value);   // begin+set+end (one-shot UI edits)
    void   endDragGesture();   // balance any open begin/endChangeGesture — from mouseUp AND the dtor
    void   addBandOfType (int typeIndex, juce::Point<float> at);   // enable the first free band
    void   pushSpectrum();
    void   selectBand (int newSel);                  // update selection + fire onBandSelected
    juce::String readoutText (int b) const;          // "1.24 kHz  +3.5 dB  Q 2.0" for the node bubble
    void   buildSpectrumPaths (juce::Path& fillOut, juce::Path& peakOut, float w, float h) const;  // liquid + peak-hold

    TabbyEqAudioProcessor& proc;

    // analyzer
    juce::dsp::FFT fft { teq::kSpectrumFftOrder };
    std::array<float, teq::kSpectrumFftSize>     window {};
    std::array<float, teq::kSpectrumFftSize * 2> fftBuf {};
    std::array<float, teq::kSpectrumFftSize / 2 + 1> specDb {};
    std::array<float, teq::kSpectrumFftSize / 2 + 1> specPeak {};   // slow-decay peak-hold

    // per-paint cache of the band designs (shared by curve / nodes / hit-test)
    teq::BandParams paramCache[tabby::kNumBands];
    teq::BandDesign designCache[tabby::kNumBands];
    double fsCache = 44100.0;

    int  draggingBand = -1;
    bool draggingGain = false;
    bool draggingQ    = false;   // dragging a Q-whisker handle (sets bandwidth, not freq/gain)
    int  qDragSide    = 0;       // which handle: +1 right / -1 left (clamps to its side, no crossing)
    int  selBand      = -1;      // currently selected band (highlighted; shown in the edit strip)
    int  starveTicks  = 0;       // consecutive analyzer ticks with no new frame
    juce::Point<float> hoverPos { -1.0f, -1.0f };   // last mouse-move position (hover halo + "+")
    bool analyzerPre = false;                       // analyzer taps pre-EQ (true) or post-EQ (false)

    static constexpr double kFreqMin   = 20.0, kFreqMax = 20000.0;
    static constexpr double kGainRange = 24.0;          // ± dB (curve y-axis)
    static constexpr double kSpecTop   = 6.0,  kSpecBottom = -90.0;
    static constexpr float  kNodeR     = 6.0f;
    static constexpr float  kAddThreshold = 36.0f;       // px from the curve to surface the "+" add button
    static constexpr float  kPeakFallDb = 0.8f;          // peak-hold fall (~24 dB/s at 30 Hz)
    static constexpr double kTiltDbPerOct = 4.5;         // analyzer tilt — pink-noise comp, like Pro-Q / Neutron
    static constexpr double kTiltPivotHz  = 1000.0;      // tilt pivot (highs lift, lows drop around 1 kHz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveDisplay)
};
