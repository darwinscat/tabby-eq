// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"

#include <functional>
#include <memory>

//==============================================================================
// A small power-symbol toggle (lit = band enabled) — the toolbar's enable button.
class PowerButton : public juce::Button
{
public:
    PowerButton() : juce::Button ({}) {}   // toggle state is driven by the bypass parameter, not self-toggled
    void paintButton (juce::Graphics& g, bool, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (5.0f);
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float r  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        g.setColour (getToggleState() ? tabby::palette::violet() : tabby::palette::textDim());
        juce::Path ring;
        ring.addCentredArc (cx, cy, r, r, 0.0f,
                            juce::degreesToRadians (38.0f), juce::degreesToRadians (322.0f), true);
        g.strokePath (ring, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillRect (cx - 0.85f, cy - r * 1.15f, 1.7f, r * 1.05f);   // the vertical bar through the gap
    }
};

//==============================================================================
// A narrow icon-only type button — shows the current filter shape; the dropdown carries icon + name.
class TypeIconButton : public juce::Button
{
public:
    TypeIconButton() : juce::Button ({}) {}
    void setType (teq::FilterType t) { type = t; repaint(); }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (tabby::palette::panel().brighter (over ? 0.30f : 0.18f));
        g.fillRoundedRectangle (b, 4.0f);
        if (auto d = tabby::shapes::icon (type, isEnabled() ? tabby::palette::violetLo() : tabby::palette::textDim()))
            d->drawWithin (g, b.reduced (5.0f), juce::RectanglePlacement::centred, 1.0f);
    }
private:
    teq::FilterType type = teq::FilterType::Bell;
};

//==============================================================================
// A lightweight OUTLINE chevron (< or >) — no button chrome. dir < 0 points left, > 0 right.
class ChevronButton : public juce::Button
{
public:
    explicit ChevronButton (int dir) : juce::Button ({}), d (dir) {}
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (3.0f, 4.0f);
        const float my     = b.getCentreY();
        const float xArms  = d > 0 ? b.getX()     : b.getRight();
        const float xTip   = d > 0 ? b.getRight() : b.getX();
        juce::Path p;
        p.startNewSubPath (xArms, b.getY());
        p.lineTo (xTip, my);
        p.lineTo (xArms, b.getBottom());
        g.setColour ((isEnabled() ? tabby::palette::text() : tabby::palette::textDim()).withAlpha (over ? 1.0f : 0.85f));
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
private:
    int d;
};

//==============================================================================
// The selected-band inspector strip: shows AND keyboard-edits the real freq / Q / gain / type /
// slope of whichever band is selected on the curve (#8 — values are first-class, not hidden). One
// set of controls serves all 24 bands: when the selection changes it rebinds its APVTS attachments
// to the new band's parameters, so the strip and the draggable node stay in lock-step for free.
class BandEditStrip : public juce::Component
{
public:
    explicit BandEditStrip (TabbyEqAudioProcessor& p);
    ~BandEditStrip() override;

    void setBand (int band);                 // -1 = nothing selected (controls disabled)
    std::function<void(int)> onStep;         // < / > pressed: step the selection by +/-1 (set by the editor)
    std::function<void()>    onEdited;       // a value-bar drag ended — editor re-places the floating toolbar

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;   // opaque on hover / edit, translucent otherwise
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAtt = juce::AudioProcessorValueTreeState::ButtonAttachment;

    void rebind();                           // (re)create attachments for the current band
    void updateOpacity();                    // 1.0 when the mouse is over (or dragging) it, else translucent
    void updateForType();                    // slope shown only for HP/LP; gain/Q enabled by type
    void showTypeMenu();                     // popup with filter-shape icons -> sets the type param
    void showRouteMenu();                    // popup Stereo/L/R/M/S -> sets the route param

    TabbyEqAudioProcessor& proc;
    int curBand = -1;

    juce::Label        title;
    PowerButton        onButton;                 // enable / bypass — power glyph, top-left
    juce::TextButton   soloButton { "S" };
    TypeIconButton     typeButton;               // icon-only; opens the type menu
    juce::TextButton   routeButton;
    ChevronButton      prevButton { -1 };   // lightweight outline <
    ChevronButton      nextButton { +1 };   // lightweight outline >
    juce::ComboBox     slopeBox;
    juce::Slider       freq, q, gain;            // value + unit shown inside each LinearBar

    std::unique_ptr<ComboAtt>  slopeAtt;
    std::unique_ptr<SliderAtt> freqAtt, qAtt, gainAtt;
    std::unique_ptr<juce::ParameterAttachment> bypassAtt;   // power button <-> bypass param (single source)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandEditStrip)
};
