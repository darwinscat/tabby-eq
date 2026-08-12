// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"
#include "ui/LaneGlyphs.h"

#include <functional>
#include <memory>

//==============================================================================
// A small power-symbol toggle (lit = band enabled) — the toolbar's enable button.
class PowerButton : public juce::Button
{
public:
    PowerButton() : juce::Button ({}) {}   // toggle state is driven by the bypass parameter, not self-toggled
    // Lit colour. The band's own power is violet; the dynamics switch takes the range hue, so two
    // identical glyphs one above the other can never be read as the same switch.
    juce::Colour litColour = tabby::palette::violet();
    void paintButton (juce::Graphics& g, bool, bool) override
    {
        auto b = getLocalBounds().toFloat().reduced (5.0f);
        const float cx = b.getCentreX(), cy = b.getCentreY();
        const float r  = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        g.setColour (getToggleState() ? litColour : tabby::palette::textDim());
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
// The placement-lane DROPDOWN button (replaces the old ST<->M/S toggle + [M][S] tabs). It shows the venn
// "set glyph" of the point's enabled lanes plus a dot in the ACTIVE lane's colour; clicking opens the lane
// menu, and the mouse-wheel cycles the active lane among the enabled ones.
class LaneSetButton : public juce::Button
{
public:
    LaneSetButton() : juce::Button ({}) {}
    void setState (unsigned mask, int active) { enabledMask = mask; activeLane = active; repaint(); }
    std::function<void(int)> onWheel;   // dir (+1 / -1) -> cycle the active lane

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (tabby::palette::panel().brighter (over ? 0.30f : 0.18f));
        g.fillRoundedRectangle (b, 4.0f);
        if (! isEnabled()) return;
        auto glyph = b.reduced (4.0f, 3.0f); glyph.removeFromRight (7.0f);       // leave room for the active dot
        tabby::venn::drawSet (g, glyph, enabledMask, activeLane);
        g.setColour (tabby::palette::lane (activeLane));                          // active-lane dot, bottom-right
        g.fillEllipse (b.getRight() - 8.0f, b.getBottom() - 8.0f, 5.0f, 5.0f);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        if (onWheel && w.deltaY != 0.0f) onWheel (w.deltaY > 0.0f ? +1 : -1);
    }

private:
    unsigned enabledMask = 1; int activeLane = 0;
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
// The DYN rail — the disclosure lives as a narrow vertical strip down the panel's RIGHT EDGE, and
// opening it grows the panel SIDEWAYS. Two reasons that beats a button plus a third row: the strip
// floats over the curve and is placed relative to the selected node, so growing downwards walks it
// into the frequency axis (and, near the floor, forces the whole panel to jump above the node
// mid-edit); and the edge rail is always in the same place whatever the type does to the rows.
// It reports as well as reveals: lit when the point's dynamics is armed, so a closed rail still
// says whether anything is moving.
class DynRailButton : public juce::Button
{
public:
    DynRailButton() : juce::Button ({}) {}
    void setState (bool armed, bool open) { isArmed = armed; isOpen = open; repaint(); }
    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto b = getLocalBounds().toFloat();
        const auto tint = ! isEnabled() ? tabby::palette::textDim()
                                        : (isArmed ? tabby::palette::orange() : tabby::palette::textDim());
        g.setColour (tabby::palette::panel().brighter (over ? 0.26f : (isOpen ? 0.16f : 0.10f)));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (tint.withAlpha (isEnabled() ? 0.55f : 0.25f));
        g.drawLine (b.getX() + 0.5f, b.getY() + 4.0f, b.getX() + 0.5f, b.getBottom() - 4.0f, 1.0f);   // the seam it opens along

        // "DYN" reading bottom-to-top, the way a spine label does — it has to survive a 14 px column.
        juce::Graphics::ScopedSaveState ss (g);
        g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi,
                                                         b.getCentreX(), b.getCentreY()));
        g.setColour (tint);
        g.setFont (juce::Font (juce::FontOptions (8.5f).withStyle ("Bold")));
        auto text = juce::Rectangle<float> (b.getCentreX() - b.getHeight() * 0.5f, b.getCentreY() - b.getWidth() * 0.5f,
                                            b.getHeight(), b.getWidth());
        g.drawText ("DYN", text.withTrimmedLeft (10.0f), juce::Justification::centred);

        juce::Path chev;   // › closed / ‹ open — drawn in the ROTATED frame, so it points sideways on screen
        const float cx = text.getX() + 8.0f, cy = text.getCentreY(), h = 3.0f, w = isOpen ? -2.5f : 2.5f;
        chev.startNewSubPath (cx - w * 0.5f, cy - h);
        chev.lineTo (cx + w * 0.5f, cy);
        chev.lineTo (cx - w * 0.5f, cy + h);
        g.strokePath (chev, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
private:
    bool isArmed = false, isOpen = false;
};

//==============================================================================
// The selected-band inspector strip: shows AND keyboard-edits the real freq / Q / gain / type / slope of
// whichever band+lane is selected on the curve (#8). One set of controls serves all 24 bands × 5 lanes:
// when the selection changes it rebinds its APVTS attachments to the ACTIVE lane's parameters, so the strip
// and the draggable node stay in lock-step. The lane dropdown + canvas + wheel are the lane selectors.
class BandEditStrip : public juce::Component
{
public:
    explicit BandEditStrip (TabbyEqAudioProcessor& p);
    ~BandEditStrip() override;

    void setBand (int band);                 // -1 = nothing selected (controls disabled)
    std::function<void(int)> onStep;         // < / > pressed: step the selection by +/-1 (set by the editor)
    std::function<void()>    onEdited;       // a value-bar drag ended — editor re-places the floating toolbar
    std::function<void(int)> onLaneChanged;  // active lane changed (menu/wheel) — editor highlights that node
    std::function<void()>    onLanesEdited;  // lane set / link edited — editor repaints the canvas
    void setActiveLane (int lane);           // set the editing lane WITHOUT firing onLaneChanged (external sync)
    std::function<void()> onSizeChanged;     // the dynamics panel opened/closed — the editor re-sizes + re-places
    void refreshDynamicsAvailability();      // the View-menu switch flipped — the rail appears/vanishes (and resizes us)
    // With dynamics off the rail is not merely hidden: the strip gives its 16 px back, so nothing on
    // the panel hints at a feature that is switched off.
    int  preferredWidth()  const noexcept { return kColMain - (dynAvailable() ? 0 : kRail) + (dynRowShown ? kColDyn : 0); }
    int  preferredHeight() const noexcept { return kRowTop; }   // the panel grows sideways, never down

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseEnter (const juce::MouseEvent&) override;   // background opaque on hover / edit, faint otherwise
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAtt  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    void rebind();                           // (re)create attachments for the current band + active lane
    void updateEngaged();                    // hover/edit state -> the BACKGROUND's opacity (text stays opaque)
    void updateForType();                    // slope shown only for HP/LP; gain/Q enabled by type
    void showTypeMenu();                     // popup with filter-shape icons -> sets the SHARED point type
    void showLaneMenu();                     // the placement-lane dropdown (checkbox set + Link FQ/Q)
    void selectLane (int lane);              // user picked a lane (menu name-click / wheel): set active + notify
    void cycleLane (int dir);                // wheel over the dropdown: step the active lane among enabled ones
    void refreshLaneButton();                // sync the dropdown glyph to the enabled set + active lane
    juce::String laneId (const juce::String& base) const;   // active-lane-scoped param id for freq/Q/gain/slope/byp
    int  bandTypeIndex() const;                             // the point's SHARED filter-type index
    bool laneOn (int lane) const;                           // is lane `lane` enabled on the current band?
    unsigned enabledMask() const;                           // bit set of enabled lanes
    bool stereoBus() const;                                 // the processor's bus is stereo (L/R/M/S usable)

    TabbyEqAudioProcessor& proc;
    int  curBand = -1;
    int  activeLane = 0;         // teq::Lane index (0..4) currently edited by the strip
    bool engaged = false;        // mouse over / editing -> the background paints fully opaque

    void toggleDynRow();                     // open/close the dynamics panel (and resize through onSizeChanged)
    void refreshDyn();                        // sync the DYN button + the row's enabled/auto state
    bool dynAvailable() const noexcept { return proc.dynamicsEnabled(); }   // the View-menu preview switch
    juce::String resolvedTimeText (bool attack) const;   // the deviation's ACTUAL ms, from the band's own fc/Q

    static constexpr float kCorner = 10.0f;   // panel corner radius (FabFilter-esque rounding)
    static constexpr int   kRowTop  = 64;     // height — CONSTANT: the panel never grows downwards
    static constexpr int   kColMain = 234;    // collapsed width = top row (202) + margins (2x8) + the rail (16)
    static constexpr int   kColDyn  = 168;    // what the dynamics panel adds on the right
    static constexpr int   kRail   = 16;      // the DYN spine down the right edge

    juce::Label        title;
    PowerButton        onButton;                 // enable / bypass — power glyph, top-left
    juce::TextButton   soloButton { "S" };
    TypeIconButton     typeButton;               // icon-only; opens the type menu
    LaneSetButton      laneButton;               // placement-lane dropdown (venn set glyph + active dot)
    ChevronButton      prevButton { -1 };   // lightweight outline <
    ChevronButton      nextButton { +1 };   // lightweight outline >
    juce::ComboBox     slopeBox;
    juce::Slider       freq, q, gain;            // value + unit shown inside each LinearBar
    DynRailButton      dynRail;                  // right-edge spine: reports armed + reveals the panel
    PowerButton        dynOnButton;              // the dynamics on/off switch itself
    juce::Slider       dynRange, dynAtk, dynRel; // range dB · attack/release DEVIATIONS (text = resolved ms)
    juce::Slider       dynThr;                   // absolute threshold — disabled (and reading "Auto") while auto is on
    juce::TextButton   dynAutoButton { "A" };    // the explicit auto/manual switch, styled like the S button
    bool               dynOpen = false;          // the row is closed by default (§ 8)
    bool               dynRowShown = false;      // ...and never shown at all on a type without gain

    std::unique_ptr<ComboAtt>  slopeAtt;
    std::unique_ptr<SliderAtt> freqAtt, qAtt, gainAtt;
    std::unique_ptr<SliderAtt> dynRangeAtt, dynThrAtt, dynAtkAtt, dynRelAtt;
    std::unique_ptr<juce::ParameterAttachment> dynOnAtt, dynAutoAtt;
    bool dynArmed = false, dynAuto = true;       // last seen, for the button + the threshold readout
    // The power button reads BOTH bypass levels (a node is dark if either is set) but WRITES the one
    // the strip's readout names — the active lane on a split point, the point otherwise. See onClick.
    std::unique_ptr<juce::ParameterAttachment> bypassAtt;     // POINT bypass
    std::unique_ptr<juce::ParameterAttachment> laneBypAtt;    // ACTIVE LANE bypass (split points only)
    bool pointByp = false, laneByp = false;                   // last seen values, for the lit state
    bool splitPoint() const;                                  // > 1 enabled lane -> the readout carries a lane letter
    void refreshPower();                                      // lit == the node is NOT dark

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BandEditStrip)
};
