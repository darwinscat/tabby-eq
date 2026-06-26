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

    output.setTextValueSuffix (" dB");
    output.setColour (juce::Slider::trackColourId, tabby::palette::violet());
    addAndMakeVisible (output);
    outputLabel.setText ("Output", juce::dontSendNotification);
    outputLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    outputLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (outputLabel);
    outputAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "output", output);

    prePost.setButtonText ("POST");
    prePost.setClickingTogglesState (true);
    prePost.onClick = [this]
    {
        const bool pre = prePost.getToggleState();
        prePost.setButtonText (pre ? "PRE" : "POST");
        display.setAnalyzerPre (pre);
    };
    addAndMakeVisible (prePost);

    setResizable (true, true);
    setResizeLimits (640, 360, 1600, 1000);
    setSize (860, 500);
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

    auto bottom = r.removeFromBottom (40);
    outputLabel.setBounds (bottom.removeFromLeft (64).reduced (4, 6));
    output.setBounds (bottom.reduced (8, 6));

    strip.setBounds (r.removeFromBottom (52).reduced (8, 4));

    display.setBounds (r.reduced (8, 4));
}
