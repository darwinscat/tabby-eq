// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/Palette.h"

//==============================================================================
// The meter's ballistics + paint, factored out of the component so BOTH rails share one pixel
// source: the IN rail is a plain LevelMeter, the OUT rail is a fader whose TRACK is this same
// meter (see ui/OutputRail.h). Instant-attack / smooth-release peak, a sticky peak-hold tick and a
// sticky clip cap. The fill is the brand gradient — mostly violet, turning orange only near the
// top — anchored to dB position so a given height always reads the same colour.
namespace tabby::ui::meter
{
    inline constexpr float kTopDb = 6.0f, kBotDb = -60.0f;
    inline constexpr float kFallPerTick     = 1.2f;   // ~36 dB/s release at 30 Hz
    inline constexpr float kPeakFallPerTick = 0.5f;   // slow peak-hold fall
    inline constexpr int   kHoldTicks       = 30;     // ~1 s hold

    // 30 Hz ballistics state — one per meter, advanced by tick() with the raw peak in dBFS.
    struct Ballistics
    {
        float level     = -120.0f;   // displayed level (dBFS)
        float peakHold  = -120.0f;   // sticky peak (dBFS)
        int   holdTicks = 0;

        void tick (float db) noexcept
        {
            level = (db > level) ? db : juce::jmax (db, level - kFallPerTick);   // instant up, smooth down

            if (db >= peakHold) { peakHold = db; holdTicks = kHoldTicks; }       // catch + hold
            else if (--holdTicks <= 0) peakHold = juce::jmax (db, peakHold - kPeakFallPerTick);
        }
    };

    inline float dbToY (float db, juce::Rectangle<float> r) noexcept
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - kBotDb) / (kTopDb - kBotDb));
        return r.getY() + (1.0f - t) * r.getHeight();
    }

    // dBFS of the processor's linear peak (the shared floor, so both rails read the same).
    inline float peakDb (float lin) noexcept
    {
        return lin > 1.0e-6f ? juce::Decibels::gainToDecibels (lin) : -120.0f;
    }

    inline void paint (juce::Graphics& g, juce::Rectangle<float> r, const Ballistics& b, bool clipped)
    {
        g.setColour (tabby::palette::bg().brighter (0.05f));
        g.fillRoundedRectangle (r, 3.0f);

        // Gradient anchored to dB position (violet low -> orange hot); clip the paint to the filled
        // portion so the colour at a height is stable regardless of the current level.
        juce::ColourGradient grad (tabby::palette::violet().withAlpha (0.50f), 0.0f, r.getBottom(),
                                   tabby::palette::orange(), 0.0f, r.getY(), false);
        grad.addColour (0.58, tabby::palette::violet().withAlpha (0.60f));   // thinner violet up to ~58%, then warms to orange
        const float yLevel = dbToY (b.level, r);
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion ((int) std::floor (r.getX()), (int) yLevel,
                                (int) std::ceil (r.getWidth()) + 1, (int) std::ceil (r.getBottom() - yLevel) + 1);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (r, 3.0f);
        }

        // 0 dBFS reference tick
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.fillRect (r.getX(), dbToY (0.0f, r), r.getWidth(), 1.0f);

        // sticky peak-hold tick
        if (b.peakHold > kBotDb)
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.fillRect (r.getX(), dbToY (b.peakHold, r) - 0.5f, r.getWidth(), 1.5f);
        }

        // clip cap (top sliver) — red while latched
        g.setColour (clipped ? juce::Colour (0xffff3b30) : juce::Colours::white.withAlpha (0.06f));
        g.fillRect (r.getX(), r.getY(), r.getWidth(), 3.0f);
    }
} // namespace tabby::ui::meter

//==============================================================================
// A minimalist vertical level meter (IN or OUT). Click anywhere on it to reset the sticky clip cap.
// Reads the processor's lock-free peak atomics on a 30 Hz timer. ~free on CPU (no blur, no shadows).
class LevelMeter : public juce::Component,
                   public juce::SettableTooltipClient,   // the IN/OUT captions are gone — a hint instead
                   private juce::Timer
{
public:
    enum class Which { In, Out };

    LevelMeter (TabbyEqAudioProcessor& p, Which w) : proc (p), which (w)
    {
        setTooltip (w == Which::In ? "Input level (click to reset the clip cap)"
                                   : "Output level, post-EQ (click to reset the clip cap)");
        startTimerHz (30);
    }
    ~LevelMeter() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        tabby::ui::meter::paint (g, getLocalBounds().toFloat(), ballistics,
                                 which == Which::In ? proc.inClipped() : proc.outClipped());
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (which == Which::In) proc.clearInClip(); else proc.clearOutClip();
        repaint();
    }

private:
    void timerCallback() override
    {
        ballistics.tick (tabby::ui::meter::peakDb (which == Which::In ? proc.readInPeak() : proc.readOutPeak()));
        repaint();
    }

    TabbyEqAudioProcessor& proc;
    Which which;
    tabby::ui::meter::Ballistics ballistics;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};
