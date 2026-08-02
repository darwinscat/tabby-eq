// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ui/LevelMeter.h"
#include "ui/Palette.h"

namespace tabby::ui
{

//==============================================================================
// The OUT rail: the output METER and the output TRIM are ONE control, so the right rail is as
// narrow as the left (30 px) instead of a meter parked next to a fader.
//
// It IS a juce::Slider (so the APVTS attachment, the wheel, keyboard, double-click-to-0 dB, typed
// entry and the host gesture brackets all come for free) — only the PAINT is ours: the meter is
// the fader's track, and the trim rides it as a full-width grip line. Two scales share the column
// on purpose, and they stay legible because they never mean the same thing: the meter FILLS from
// the bottom (level, -60…+6 dBFS), the grip is a single LINE (trim, ±24 dB) with unity nubs at the
// centre. The value sits in the readout strip under the meter — it is the slider's own text box,
// so double-clicking it types a value.
//
// The clip cap keeps its click-to-reset, but only while it is actually latched (and only in its
// top sliver) — so a click there never silently costs you the top of the fader's travel.
class OutputRail final : public juce::Slider,
                         private juce::Timer
{
public:
    static constexpr int kReadoutH  = 13;   // the value strip under the meter
    static constexpr int kMeterW    = 16;   // the meter column — same width as the IN rail's meter
    static constexpr int kClipCapH  = 5;    // click target for resetting a latched clip cap
    static constexpr float kGripH   = 11.0f;   // the sliding frame's height
    static constexpr float kSightW  = 4.0f;    // its side ticks — the sight that marks the exact value

    explicit OutputRail (TabbyEqAudioProcessor& p)
        : juce::Slider (juce::Slider::LinearVertical, juce::Slider::TextBoxBelow), proc (p)
    {
        setLookAndFeel (&railLnf);
        setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, kReadoutH);   // width is clamped to the rail by getSliderLayout
        setColour (juce::Slider::textBoxTextColourId,    tabby::palette::text());
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setDoubleClickReturnValue (true, 0.0);
        // NB juce::String from a plain const char* is decoded as LATIN-1, so a UTF-8 dash in a
        // literal would reach the screen as mojibake — every non-ASCII glyph goes through fromUTF8.
        setTooltip (juce::String::fromUTF8 ("Output trim — drag to set, double-click for 0 dB. "
                                            "The fill is the post-EQ level (click a red clip cap to reset it)."));
        startTimerHz (30);
    }

    ~OutputRail() override
    {
        stopTimer();
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        const auto track = trackArea();

        // --- the meter IS the track ---------------------------------------------------------
        tabby::ui::meter::paint (g, meterArea (track), ballistics, proc.outClipped());

        // --- unity (0 dB trim) nubs: a scale mark, deliberately NOT a full line so it never
        //     reads as one of the meter's own dBFS ticks ----------------------------------------
        const float yUnity = std::floor (getPositionOfValue (0.0)) + 0.5f;
        g.setColour (juce::Colours::white.withAlpha (0.22f));
        g.fillRect (track.getX(), yUnity - 0.5f, 3.0f, 1.0f);
        g.fillRect (track.getRight() - 3.0f, yUnity - 0.5f, 3.0f, 1.0f);

        // --- the trim grip: a sliding FRAME with a sight, full rail width (so it overhangs the
        //     meter and reads as a fader riding it). The frame stays HOLLOW on purpose — the meter
        //     keeps running through it; the two ticks biting in from the sides are the sight, and
        //     they mark the exact value, which a 10 px-tall frame alone could not. ---------------
        const float yGrip = juce::jlimit (track.getY() + kGripH * 0.5f, track.getBottom() - kGripH * 0.5f,
                                          getPositionOfValue (getValue()));
        const juce::Rectangle<float> frame (track.getX() + 0.75f, yGrip - kGripH * 0.5f,
                                            track.getWidth() - 1.5f, kGripH);
        g.setColour (juce::Colours::black.withAlpha (0.45f));                  // seat it over the gradient
        g.drawRoundedRectangle (frame, 2.5f, 3.0f);
        g.setColour (isMouseOverOrDragging() ? tabby::palette::orange().brighter (0.25f)
                                             : tabby::palette::orange());
        g.drawRoundedRectangle (frame, 2.5f, 1.4f);
        g.fillRect (frame.getX(),              yGrip - 0.75f, kSightW, 1.5f);
        g.fillRect (frame.getRight() - kSightW, yGrip - 0.75f, kSightW, 1.5f);
    }

    // A latched clip cap keeps its click-to-reset; everything else is the fader.
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (hitsClipCap (e)) { proc.clearOutClip(); repaint(); return; }
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (hitsClipCap (e)) return;   // the reset click must not turn into a fader drag
        juce::Slider::mouseDrag (e);
    }

private:
    // The slider's own track rect (local bounds minus the readout strip) — the SAME rect the
    // slider maps the mouse and getPositionOfValue onto, so grip and drag can never disagree.
    juce::Rectangle<float> trackArea() const noexcept
    {
        return getLocalBounds().withTrimmedBottom (kReadoutH).toFloat();
    }
    static juce::Rectangle<float> meterArea (juce::Rectangle<float> track) noexcept
    {
        return track.withSizeKeepingCentre (juce::jmin (track.getWidth(), (float) kMeterW), track.getHeight());
    }
    bool hitsClipCap (const juce::MouseEvent& e) const noexcept
    {
        return proc.outClipped() && e.position.y <= trackArea().getY() + (float) kClipCapH;
    }

    void timerCallback() override
    {
        ballistics.tick (tabby::ui::meter::peakDb (proc.readOutPeak()));
        repaint();
    }

    // The rail's own look: a small readout font (40 px of text in a 30 px column would otherwise be
    // auto-squashed by drawFittedText) and an exact track/readout split with NO thumb indent — the
    // grip is clamped when drawn instead, so the full travel stays reachable.
    struct RailLnF final : juce::LookAndFeel_V4
    {
        juce::Label* createSliderTextBox (juce::Slider& s) override
        {
            auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
            l->setFont (juce::Font (juce::FontOptions (10.5f)));
            l->setMinimumHorizontalScale (1.0f);
            l->setBorderSize ({ 0, 0, 0, 0 });
            return l;
        }
        juce::Slider::SliderLayout getSliderLayout (juce::Slider& s) override
        {
            auto b = s.getLocalBounds();
            juce::Slider::SliderLayout layout;
            layout.textBoxBounds = b.removeFromBottom (kReadoutH);
            layout.sliderBounds  = b;
            return layout;
        }
    };

    TabbyEqAudioProcessor& proc;
    RailLnF railLnf;
    tabby::ui::meter::Ballistics ballistics;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputRail)
};

} // namespace tabby::ui
