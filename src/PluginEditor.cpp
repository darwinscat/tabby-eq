// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginEditor.h"
#include "ui/Palette.h"

TabbyEqEditor::TabbyEqEditor (TabbyEqAudioProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p), display (p), strip (p)
{
    title.setText ("TabbyEQ", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (18.0f).withStyle ("Bold")));
    title.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (title);

    addAndMakeVisible (display);

    addAndMakeVisible (strip);
    display.onBandSelected = [this] (int b) { strip.setBand (b); };   // node selection drives the strip

    // OUT rail trim — a minimalist vertical fader; double-click returns to 0 dB.
    output.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 40, 14);
    output.setNumDecimalPlacesToDisplay (1);
    output.setDoubleClickReturnValue (true, 0.0);
    output.setColour (juce::Slider::trackColourId,         tabby::palette::violet());
    output.setColour (juce::Slider::thumbColourId,         tabby::palette::orange());
    output.setColour (juce::Slider::textBoxTextColourId,   tabby::palette::text());
    output.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (output);
    outputAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "output", output);

    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    auto setupCap = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::Font (juce::FontOptions (9.5f)));
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::textColourId, tabby::palette::textDim());
        addAndMakeVisible (l);
    };
    setupCap (inCap, "IN");
    setupCap (outCap, "OUT");

    prePost.setButtonText ("POST");
    prePost.setClickingTogglesState (true);
    prePost.onClick = [this]
    {
        const bool pre = prePost.getToggleState();
        prePost.setButtonText (pre ? "PRE" : "POST");
        display.setAnalyzerPre (pre);
    };
    addAndMakeVisible (prePost);

    viewButton.setButtonText ("View");
    viewButton.onClick = [this] { showViewMenu(); };
    addAndMakeVisible (viewButton);

    display.setViewBandColors ((bool) proc.apvts.state.getProperty ("viewBandColors", true));
    display.setViewBandCurves ((bool) proc.apvts.state.getProperty ("viewBandCurves", true));
    display.setViewBandFill   ((bool) proc.apvts.state.getProperty ("viewBandFill",   false));
    display.setViewLongSolo   ((bool) proc.apvts.state.getProperty ("viewLongSolo",   true));

    setResizable (true, true);
    setResizeLimits (640, 360, 1600, 1000);
    setSize (860, 500);
}

void TabbyEqEditor::showViewMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Per-band colours", true, display.viewBandColors());
    m.addItem (2, "Per-band curves",  true, display.viewBandCurves());
    m.addItem (3, "Per-band fill",    true, display.viewBandFill());
    m.addItem (4, "Long-press solo",  true, display.viewLongSolo());
    juce::Component::SafePointer<TabbyEqEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& d  = safe->display;
        auto& st = safe->proc.apvts.state;
        if (r == 1) { const bool v = ! d.viewBandColors(); d.setViewBandColors (v); st.setProperty ("viewBandColors", v, nullptr); }
        if (r == 2) { const bool v = ! d.viewBandCurves(); d.setViewBandCurves (v); st.setProperty ("viewBandCurves", v, nullptr); }
        if (r == 3) { const bool v = ! d.viewBandFill();   d.setViewBandFill (v);   st.setProperty ("viewBandFill", v, nullptr); }
        if (r == 4) { const bool v = ! d.viewLongSolo();   d.setViewLongSolo (v);   st.setProperty ("viewLongSolo", v, nullptr); }
    });
}

void TabbyEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (tabby::palette::bg());
}

void TabbyEqEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30);
    title.setBounds (top.removeFromLeft (160).reduced (8, 4));
    prePost.setBounds (top.removeFromRight (74).reduced (6, 3));
    viewButton.setBounds (top.removeFromRight (60).reduced (4, 3));

    strip.setBounds (r.removeFromBottom (52).reduced (8, 4));

    // IN / OUT rails flank the graph; the OUT rail also holds the output trim fader.
    auto leftRail  = r.removeFromLeft (30);
    auto rightRail = r.removeFromRight (64);
    {
        auto lr = leftRail.reduced (7, 6);
        inCap.setBounds (lr.removeFromBottom (14));
        inMeter.setBounds (lr);
    }
    {
        auto rr = rightRail.reduced (7, 6);
        outCap.setBounds (rr.removeFromBottom (14));
        outMeter.setBounds (rr.removeFromLeft (14));
        rr.removeFromLeft (6);
        output.setBounds (rr);
    }

    display.setBounds (r.reduced (8, 4));
}
