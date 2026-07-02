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
    addAndMakeVisible (corrMeter);
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

    // Phase + quality combos. NB: AudioProcessorValueTreeState::ComboBoxAttachment does NOT reliably
    // auto-populate from the choice param here (getAllValueStrings() comes back empty -> "(no choices)"),
    // so we fill each combo from the parameter's own `choices` (single source of truth for the strings)
    // and let the attachment only sync the selection. Item ids are 1..N (the attachment maps id-1 -> index).
    for (auto* c : { &phaseCombo, &qualityCombo })
    {
        c->setColour (juce::ComboBox::textColourId,       tabby::palette::text());
        c->setColour (juce::ComboBox::backgroundColourId, tabby::palette::panel());
        c->setColour (juce::ComboBox::outlineColourId,    tabby::palette::panel().brighter (0.20f));
        c->setColour (juce::ComboBox::arrowColourId,      tabby::palette::textDim());
    }

    // Phase mode (Zero Latency / Natural Phase / Linear Phase) — all three live now. The attachment is the
    // single source of truth (param <-> combo), which also kills the old button/label one-event lag.
    addAndMakeVisible (phaseCombo);
    if (auto* pm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("phaseMode")))
    {
        phaseCombo.addItemList (pm->choices, 1);
        phaseAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "phaseMode", phaseCombo);
    }
    // Refresh the latency readout from the combo's OWN selection (always current) on its onChange — NOT
    // from getRawParameterValue(), whose atom can lag the attachment by one event (that lag is what
    // showed the wrong mode: "Linear Phase" with 0 ms, "Zero Latency" with 1365 ms). Set after the
    // attachment so its sendInitialUpdate doesn't fire this before the quality combo exists.
    phaseCombo.onChange = [this] { updatePhaseUi(); };

    // Linear-phase FIR quality — promoted out of the View menu into its own combo beside the mode.
    addAndMakeVisible (qualityCombo);
    if (auto* lq = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("lpQuality")))
    {
        qualityCombo.addItemList (lq->choices, 1);
        qualityAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "lpQuality", qualityCombo);
    }
    qualityCombo.onChange = [this] { updatePhaseUi(); };

    // Natural-phase blend knob (0 linear … 1 minimum phase) — shown only in Natural mode, in the SAME
    // top-bar slot as the quality combo (which is Linear-only).
    phaseAmountSlider.setSliderStyle (juce::Slider::LinearBar);
    phaseAmountSlider.setColour (juce::Slider::trackColourId,          tabby::palette::violet().withAlpha (0.55f));
    phaseAmountSlider.setColour (juce::Slider::textBoxTextColourId,    tabby::palette::text());
    phaseAmountSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    phaseAmountSlider.setTooltip ("Phase blend: linear (0) … minimum-phase (1)");
    addAndMakeVisible (phaseAmountSlider);
    phaseAmountAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "phaseAmount", phaseAmountSlider);
    phaseAmountSlider.onValueChange = [this] { updatePhaseUi(); };

    latencyLabel.setJustificationType (juce::Justification::centredLeft);
    latencyLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (latencyLabel);
    updatePhaseUi();   // initial latency text + quality-combo enable state

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
    display.setAddLineMode    ((int)  proc.apvts.state.getProperty ("addLineMode",    1));   // default: the 0 dB line only
    display.setAuditionVisual ((int)  proc.apvts.state.getProperty ("auditionVisual", 1));   // default: bell
    display.setAuditionQ      ((float) (double) proc.apvts.state.getProperty ("auditionQ", 6.0));
    display.setAuditionLockGain ((bool) proc.apvts.state.getProperty ("audLockGain", true));
    display.setToolbarPlacement ((int)  proc.apvts.state.getProperty ("toolbarPlace", 0));   // floating edit-strip behaviour

    proc.setSpectrumDomain ((int) proc.apvts.state.getProperty ("specDomain", 0));   // analyzer Stereo/Mid/Side
    // Per-point Link FQ / Link Q are now mirrored PROCESSOR-side (host-safe, works with the editor closed);
    // the old editor-only msFreqLink machinery is gone. The View menu edits only the GLOBAL new-split defaults.

    // One-time migration notice: the v2->v3 migration coerced a legacy M/S band's Side type (all 24 slots
    // were in use, so the Side lane couldn't fission into its own point). Async = non-blocking; removing
    // the property makes it one-time (the removal is saved with the session).
    if (proc.apvts.state.hasProperty ("migrationNote"))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "TabbyEQ — session migrated",
            "An older session's M/S band used a different filter type on its Side lane.\n"
            "All 24 bands were in use, so that Side lane now uses the band's shared type.");
        proc.apvts.state.removeProperty ("migrationNote", nullptr);
    }

    setResizable (true, true);
    setResizeLimits (640, 360, 7680, 4320);   // drag-resize freely; maximise / fullscreen to any display
    setSize (860, 500);
}

TabbyEqEditor::~TabbyEqEditor() = default;

void TabbyEqEditor::updatePhaseUi()
{
    const int mode = phaseCombo.getSelectedItemIndex();   // 0 Zero Latency / 1 Natural Phase / 2 Linear Phase
    qualityCombo.setVisible     (mode == 2);              // quality combo: Linear only
    phaseAmountSlider.setVisible (mode == 1);             // blend knob: Natural only (same top-bar slot)

    if (mode <= 0)   // Zero Latency — no added delay
    {
        latencyLabel.setText ("0 ms", juce::dontSendNotification);
        latencyLabel.setColour (juce::Label::textColourId, tabby::palette::textDim());
        return;
    }
    // FIR mode — compute the reported latency directly from the UI state (avoids the param-atomic lag):
    const double sr = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
    double samples; juce::Colour col;
    if (mode == 1)   // Natural: FIXED bulk delay = L/4 (L = 4096) — does NOT move with the blend knob. YELLOW.
    {
        samples = 4096.0 / 4.0;
        col = juce::Colour (0xffe8c020);
    }
    else             // Linear: N/2. RED (heavy).
    {
        static constexpr int sizes[] = { 4096, 8192, 16384, 32768, 131072 };
        const int q = juce::jlimit (0, 4, qualityCombo.getSelectedItemIndex());
        samples = (double) sizes[q] / 2.0;
        col = juce::Colour (0xffff5a5a);
    }
    const double ms = samples / sr * 1000.0;
    latencyLabel.setText (juce::String (ms, ms < 100.0 ? 1 : 0) + " ms", juce::dontSendNotification);
    latencyLabel.setColour (juce::Label::textColourId, col);
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
    // Link FQ / Link Q are per-point (mirrored processor-side); here the View menu edits the GLOBAL defaults
    // for newly split points. Interim (PR D adds the per-point lane menu): toggling ON also links the
    // currently-selected split band right away.
    m.addItem (40, "Link FQ (new splits)", true, (bool) proc.apvts.state.getProperty ("defaultLinkFq", false));
    m.addItem (41, "Link Q (new splits)",  true, (bool) proc.apvts.state.getProperty ("defaultLinkQ", false));

    juce::PopupMenu domMenu;
    const int dom = proc.getSpectrumDomain();
    const char* domNames[] = { "Stereo", "Mid", "Side" };
    for (int i = 0; i < 3; ++i) domMenu.addItem (60 + i, domNames[i], true, dom == i);
    m.addSubMenu ("Analyzer domain", domMenu);

    juce::PopupMenu placeMenu;   // floating edit-strip placement (try each live; persisted)
    const int tp = display.toolbarPlacement();
    const char* placeNames[] = { "Classic (centered)", "Anchor to open side", "Collision-aware",
                                 "Hybrid (dock when crowded)", "Fixed lane (FabFilter)" };
    for (int i = 0; i < 5; ++i) placeMenu.addItem (70 + i, placeNames[i], true, tp == i);
    m.addSubMenu ("Edit-strip placement", placeMenu);

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
        if (r == 40 || r == 41)
        {
            const char* gKey = (r == 40) ? "defaultLinkFq" : "defaultLinkQ";
            const char* bKey = (r == 40) ? "linkFq"        : "linkQ";
            const bool v = ! (bool) st.getProperty (gKey, false);
            st.setProperty (gKey, v, nullptr);
            const int sb = safe->display.selectedBand();
            if (v && sb >= 0)   // interim: apply immediately to the selected split band
            {
                const bool split = safe->proc.apvts.getRawParameterValue (tabby::laneParamId (sb, 3, "on"))->load() > 0.5f
                                || safe->proc.apvts.getRawParameterValue (tabby::laneParamId (sb, 4, "on"))->load() > 0.5f;
                if (split) st.setProperty (tabby::bandId (sb, bKey), true, nullptr);
            }
        }
        if (r >= 60 && r <= 62) { const int dn = r - 60; safe->proc.setSpectrumDomain (dn); st.setProperty ("specDomain", dn, nullptr); }
        if (r >= 70 && r <= 74) { const int mp = r - 70; d.setToolbarPlacement (mp); st.setProperty ("toolbarPlace", mp, nullptr); }
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
    title.setBounds (top.removeFromLeft (150).reduced (8, 4));
    prePost.setBounds (top.removeFromRight (70).reduced (6, 3));
    latencyLabel.setBounds (top.removeFromRight (56).reduced (2, 4));   // red latency readout (separate from the combo)
    const auto firSlot = top.removeFromRight (84).reduced (4, 4);      // shared slot: quality (Linear) / blend knob (Natural)
    qualityCombo.setBounds (firSlot);
    phaseAmountSlider.setBounds (firSlot);
    phaseCombo.setBounds (top.removeFromRight (110).reduced (4, 4));    // phase mode
    viewButton.setBounds (top.removeFromRight (52).reduced (4, 3));
    resetButton.setBounds (top.removeFromRight (52).reduced (4, 3));
    fullButton.setBounds (top.removeFromRight (46).reduced (4, 3));
    corrMeter.setBounds (top.removeFromLeft (108).reduced (8, 2));   // remaining middle-left of the top bar

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
