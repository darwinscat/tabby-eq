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

    display.setToolbar (&strip);                                      // floating toolbar parented over the canvas
    display.onBandSelected = [this] (int b, bool side) { strip.setBand (b); strip.setActiveLane (side); };   // node+lane -> toolbar
    strip.onLaneChanged    = [this] (bool side) { display.setSelectedSide (side); };   // Mid/Side tab -> highlight node
    strip.onStep   = [this] (int d) { display.stepSelection (d); };   // < / > step to the prev / next band
    strip.onEdited = [this] { display.refreshToolbar(); };            // re-place toolbar after a slider edit

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

    phaseButton.onClick = [this]
    {
        if (auto* pp = proc.apvts.getParameter ("phaseMode"))
            pp->setValueNotifyingHost (pp->getValue() > 0.5f ? 0.0f : 1.0f);   // toggle Natural <-> Linear
    };
    addAndMakeVisible (phaseButton);
    if (auto* pm = proc.apvts.getParameter ("phaseMode"))
    {
        phaseAtt = std::make_unique<juce::ParameterAttachment> (*pm, [this] (float) { updatePhaseLabel(); });
        phaseAtt->sendInitialUpdate();
    }

    viewButton.setButtonText ("View");
    viewButton.onClick = [this] { showViewMenu(); };
    addAndMakeVisible (viewButton);

    resetButton.setButtonText ("Reset");
    resetButton.onClick = [this] { resetAll(); };
    addAndMakeVisible (resetButton);

    fullButton.setButtonText ("Full");
    fullButton.onClick = [this] { toggleFullscreen(); };
    fullButton.setVisible (juce::JUCEApplicationBase::isStandaloneApp());   // kiosk only makes sense standalone
    addAndMakeVisible (fullButton);
    display.onToggleFullscreen = [this] { toggleFullscreen(); };            // 'f' key

    display.setViewBandColors ((bool) proc.apvts.state.getProperty ("viewBandColors", true));
    display.setViewBandCurves ((bool) proc.apvts.state.getProperty ("viewBandCurves", true));
    display.setViewBandFill   ((bool) proc.apvts.state.getProperty ("viewBandFill",   false));
    display.setViewLongSolo   ((bool) proc.apvts.state.getProperty ("viewLongSolo",   true));
    display.setAddLineMode    ((int)  proc.apvts.state.getProperty ("addLineMode",    3));   // default: both lines
    display.setAuditionVisual ((int)  proc.apvts.state.getProperty ("auditionVisual", 1));   // default: bell
    display.setAuditionQ      ((float) (double) proc.apvts.state.getProperty ("auditionQ", 6.0));
    display.setAuditionLockGain ((bool) proc.apvts.state.getProperty ("audLockGain", true));

    msFreqLink = (bool) proc.apvts.state.getProperty ("msFreqLink", false);   // M/S Mid<->Side freq lock
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        proc.apvts.addParameterListener (tabby::bandId (b, "freq"),  this);
        proc.apvts.addParameterListener (tabby::bandId (b, "sFreq"), this);
    }

    setResizable (true, true);
    setResizeLimits (640, 360, 7680, 4320);   // drag-resize freely; maximise / fullscreen to any display
    setSize (860, 500);
}

TabbyEqEditor::~TabbyEqEditor()
{
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        proc.apvts.removeParameterListener (tabby::bandId (b, "freq"),  this);
        proc.apvts.removeParameterListener (tabby::bandId (b, "sFreq"), this);
    }
}

void TabbyEqEditor::parameterChanged (const juce::String& id, float value)   // M/S freq-link mirror
{
    if (! msFreqLink || mirroring) return;
    const bool isSide = id.endsWith ("_sFreq");
    const bool isMid  = ! isSide && id.endsWith ("_freq");
    if (! isSide && ! isMid) return;
    const int b = id.substring (4).upToFirstOccurrenceOf ("_", false, false).getIntValue();
    if (b < 0 || b >= tabby::kNumBands) return;
    if (proc.apvts.getRawParameterValue (tabby::bandId (b, "ms"))->load() < 0.5f) return;   // only while split
    if (auto* sib = proc.apvts.getParameter (tabby::bandId (b, isSide ? "freq" : "sFreq")))
    {
        const juce::ScopedValueSetter<bool> guard (mirroring, true);
        sib->setValueNotifyingHost (sib->convertTo0to1 (value));
    }
}

void TabbyEqEditor::alignLinkedFreqs()   // copy each split band's Mid freq onto its Side
{
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (proc.apvts.getRawParameterValue (tabby::bandId (b, "ms"))->load() >= 0.5f)
            if (auto* mid = proc.apvts.getParameter (tabby::bandId (b, "freq")))
                if (auto* side = proc.apvts.getParameter (tabby::bandId (b, "sFreq")))
                    side->setValueNotifyingHost (mid->getValue());
}

void TabbyEqEditor::updatePhaseLabel()
{
    const bool lin = proc.apvts.getRawParameterValue ("phaseMode")->load() > 0.5f;
    if (! lin) { phaseButton.setButtonText ("Natural"); return; }
    static constexpr int sizes[] = { 4096, 16384, 65536, 131072 };
    const int q = juce::jlimit (0, 3, (int) proc.apvts.getRawParameterValue ("lpQuality")->load());
    const double sr = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
    const double ms = (double) sizes[q] / 2.0 / sr * 1000.0;
    phaseButton.setButtonText ("Lin " + juce::String (ms, ms < 100.0 ? 1 : 0) + " ms");
}

void TabbyEqEditor::showViewMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Per-band colours", true, display.viewBandColors());
    m.addItem (2, "Per-band curves",  true, display.viewBandCurves());
    m.addItem (3, "Per-band fill",    true, display.viewBandFill());
    m.addItem (4, "Long-press solo",  true, display.viewLongSolo());

    juce::PopupMenu addLineMenu;
    const int cur = display.addLineMode();
    addLineMenu.addItem (10, "Off",          true, cur == 0);
    addLineMenu.addItem (11, "0 dB line",    true, cur == 1);
    addLineMenu.addItem (12, "Curve",        true, cur == 2);
    addLineMenu.addItem (13, "0 dB + curve", true, cur == 3);
    m.addSubMenu ("Show \"+\" on", addLineMenu);

    juce::PopupMenu audMenu;
    audMenu.addSectionHeader ("Visual");
    audMenu.addItem (20, "Spotlight (band)", true, display.auditionVisual() == 0);
    audMenu.addItem (21, "Bell curve",       true, display.auditionVisual() == 1);
    audMenu.addItem (22, "Lock gain (sweep freq only)", true, display.auditionLockGain());
    audMenu.addSectionHeader ("Listen Q");
    const int audQv[] = { 3, 6, 9, 12 };
    for (int i = 0; i < 4; ++i)
        audMenu.addItem (30 + i, "Q " + juce::String (audQv[i]), true, juce::roundToInt (display.auditionQ()) == audQv[i]);
    m.addSubMenu ("Audition (Alt-drag)", audMenu);
    m.addItem (40, "M/S: link Mid/Side freq", true, msFreqLink);

    juce::PopupMenu lpMenu;
    const int lq = juce::jlimit (0, 3, (int) proc.apvts.getRawParameterValue ("lpQuality")->load());
    const char* lpNames[] = { "Low", "Medium", "High", "Max" };
    for (int i = 0; i < 4; ++i) lpMenu.addItem (50 + i, lpNames[i], true, lq == i);
    m.addSubMenu ("Linear-phase quality", lpMenu);

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
        if (r >= 10 && r <= 13) { const int mode = r - 10; d.setAddLineMode (mode); st.setProperty ("addLineMode", mode, nullptr); }
        if (r == 20 || r == 21) { const int v = r - 20; d.setAuditionVisual (v); st.setProperty ("auditionVisual", v, nullptr); }
        if (r == 22) { const bool v = ! d.auditionLockGain(); d.setAuditionLockGain (v); st.setProperty ("audLockGain", v, nullptr); }
        if (r >= 30 && r <= 33) { const int qv[] = { 3, 6, 9, 12 }; const float q = (float) qv[r - 30]; d.setAuditionQ (q); st.setProperty ("auditionQ", q, nullptr); }
        if (r == 40) { safe->msFreqLink = ! safe->msFreqLink; st.setProperty ("msFreqLink", safe->msFreqLink, nullptr);
                       if (safe->msFreqLink) safe->alignLinkedFreqs(); }   // snap Side->Mid immediately
        if (r >= 50 && r <= 53) { if (auto* p = safe->proc.apvts.getParameter ("lpQuality"))
                                      p->setValueNotifyingHost (p->convertTo0to1 ((float) (r - 50)));
                                  safe->updatePhaseLabel(); }
    });
}

void TabbyEqEditor::toggleFullscreen()
{
    if (! juce::JUCEApplicationBase::isStandaloneApp()) return;   // don't kiosk a DAW's window
    auto& d = juce::Desktop::getInstance();
    if (d.getKioskModeComponent() != nullptr) d.setKioskModeComponent (nullptr);          // exit
    else if (auto* top = getTopLevelComponent()) d.setKioskModeComponent (top, false);    // enter (borderless)
}

void TabbyEqEditor::resetAll()
{
    for (auto* p : proc.getParameters())          // every band param + output back to its default
        p->setValueNotifyingHost (p->getDefaultValue());
    proc.setSoloBand (-1);                         // clear any solo
    display.clearSelection();                       // deselect + hide the floating toolbar
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
    phaseButton.setBounds (top.removeFromRight (82).reduced (4, 3));
    viewButton.setBounds (top.removeFromRight (60).reduced (4, 3));
    resetButton.setBounds (top.removeFromRight (58).reduced (4, 3));
    fullButton.setBounds (top.removeFromRight (50).reduced (4, 3));

    // (The per-band editor is now a floating toolbar parented in the display; the bottom area
    //  it used to occupy is reserved for the upcoming Helper.)

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
