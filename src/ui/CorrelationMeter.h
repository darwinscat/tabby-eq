// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Palette.h"

//==============================================================================
// A compact horizontal phase-correlation meter: centre = 0, right = +1 (in phase / mono-safe),
// left = -1 (out of phase). Polls the processor's correlation atomic on a timer. Violet to the
// right, orange to the left (out-of-phase = a heads-up). No caption — a tooltip explains it
// (the bottom toolbar keeps it slim).
class CorrelationMeter : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    explicit CorrelationMeter (TabbyEqAudioProcessor& p) : proc (p)
    {
        setTooltip ("L/R phase correlation: right = in phase (mono-safe), left = out of phase");
        startTimerHz (30);
    }
    ~CorrelationMeter() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        auto track = b.reduced (1.0f, 1.0f);
        g.setColour (tabby::palette::panel().brighter (0.10f));
        g.fillRoundedRectangle (track, 2.0f);

        const float cx = track.getCentreX();
        const float v  = juce::jlimit (-1.0f, 1.0f, corr);
        const float x  = cx + v * (track.getWidth() * 0.5f);
        const auto  fill = (v >= 0.0f) ? juce::Rectangle<float> (cx, track.getY(), x - cx, track.getHeight())
                                       : juce::Rectangle<float> (x,  track.getY(), cx - x, track.getHeight());
        g.setColour ((v >= 0.0f ? tabby::palette::violet() : tabby::palette::orange()).withAlpha (0.85f));
        g.fillRect (fill);

        g.setColour (tabby::palette::text().withAlpha (0.6f));   // centre tick (0)
        g.fillRect (cx - 0.5f, track.getY(), 1.0f, track.getHeight());
    }

private:
    void timerCallback() override
    {
        const float c = proc.getCorrelation();
        if (std::abs (c - corr) > 0.002f) { corr = c; repaint(); }
    }

    TabbyEqAudioProcessor& proc;
    float corr = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationMeter)
};
