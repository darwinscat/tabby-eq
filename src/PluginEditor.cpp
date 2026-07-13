// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginEditor.h"
#include "ui/Palette.h"
#include "BinaryData.h"   // TabbyEQData — the embedded Michroma wordmark font

TabbyEqEditor::TabbyEqEditor (TabbyEqAudioProcessor& p)
    : juce::AudioProcessorEditor (p), proc (p), display (p), strip (p)
{
    // The clickable brand blister — FabFilter-style badge (the Darwin's Cat mark + "TabbyEQ"
    // Michroma wordmark + byline). Links to the product page.
    catLogo = juce::Drawable::createFromImageData (BinaryData::logodarwinscat_svg,
                                                   (size_t) BinaryData::logodarwinscat_svgSize);
    brand.catLogo = catLogo.get();
    brand.wordmarkTypeface = juce::Typeface::createSystemTypefaceFor (BinaryData::MichromaRegular_ttf,
                                                                      (size_t) BinaryData::MichromaRegular_ttfSize);
    brand.onLaunch = [] { juce::URL ("https://darwinscat.com/tabbyeq").launchInDefaultBrowser(); };
    brand.setTooltip ("darwinscat.com/tabbyeq");
    brand.toolbarBottom = 30.0f;   // the toolbar's bottom line = the bell's "0" (kBarH in resized())
    addAndMakeVisible (brand);

    addAndMakeVisible (display);
    brand.toFront (false);   // the blister protrudes below the toolbar, ON TOP of the display's top edge
    addAndMakeVisible (underline);   // the continuous toolbar-bottom line, ON TOP of the blister + graph
    underline.toFront (false);

    display.setToolbar (&strip);                                      // floating toolbar parented over the canvas
    display.onBandSelected = [this] (int b, int lane) { strip.setBand (b); strip.setActiveLane (lane); };   // node+lane -> toolbar
    strip.onLaneChanged    = [this] (int lane) { display.setSelectedLane (lane); };   // lane dropdown/wheel -> highlight node
    strip.onLanesEdited    = [this] { display.refreshAfterLaneEdit(); };   // lane set / links changed -> re-cache + repaint
    strip.onStep   = [this] (int d) { display.stepSelection (d); };   // < / > step across all visible nodes
    strip.onEdited = [this] { display.refreshToolbar(); };            // re-place toolbar after a slider edit

    // OUT rail trim — a minimalist vertical fader; double-click returns to 0 dB.
    output.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 16);   // the 0 dB editor, full rail width
    output.setNumDecimalPlacesToDisplay (1);
    output.setDoubleClickReturnValue (true, 0.0);
    output.setColour (juce::Slider::trackColourId,         tabby::palette::violet());
    output.setColour (juce::Slider::thumbColourId,         tabby::palette::orange());
    output.setColour (juce::Slider::textBoxTextColourId,   tabby::palette::text());
    output.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (output);
    outputAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, "output", output);
    // AFTER the attachment (it installs the param's text conversion): the param formats the default
    // as "-0.00" — a floating-point negative zero. Snap the dust to a plain 0.
    output.textFromValueFunction = [] (double v)
    {
        return juce::String (std::abs (v) < 0.005 ? 0.0 : v, 2);
    };
    output.updateText();
    // A MOUSE fader drag = ONE labelled undo step (and keyPressed's navigation gate covers it
    // while open). Mouse-only, like the strip bars: JUCE also fires the drag pair for wheel
    // notches / typed entry / double-click — those coalesce through the settle timer instead.
    {
        auto open = std::make_shared<bool> (false);
        output.onDragStart = [this, open] { if (output.isMouseButtonDown()) { *open = true; proc.beginHistoryGesture ("Output Trim"); } };
        output.onDragEnd   = [this, open] { if (*open) { *open = false; proc.endHistoryGesture(); } };
    }

    addAndMakeVisible (inMeter);    // no IN/OUT captions — the meters carry tooltips
    addAndMakeVisible (outMeter);
    addAndMakeVisible (corrMeter);

    // A/B/C/D compare registers — recall on click, copy via right-click menu / drag / clipboard.
    // Toggle look: the ACTIVE register is violet (the family accent), siblings sit on the panel.
    for (int i = 0; i < TabbyEqAudioProcessor::kNumSnapshots; ++i)
    {
        auto& b = snapBtn[i];
        b.setRegisterIndex (i);
        b.setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + i)));
        b.setColour (juce::TextButton::buttonColourId,   tabby::palette::panel());
        b.setColour (juce::TextButton::buttonOnColourId, tabby::palette::violet());
        b.setColour (juce::TextButton::textColourOffId,  tabby::palette::textDim());
        b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        b.onClick    = [this, i] { switchSnapshot (i); };
        b.onPopup    = [this, i] { showSnapshotMenu (i); };
        b.onCopyDrop = [this] (int from, int to) { applySnapshotCopy (from, to); };
        addAndMakeVisible (b);
    }
    updateSnapshotButtons();

    // Undo / redo — enablement + peek-label tooltips follow historyRevision() (see timerCallback).
    // The arrow icons are self-painted (HistoryArrowButton) — no button text.
    for (auto* b : { &undoBtn, &redoBtn })
    {
        b->setColour (juce::TextButton::buttonColourId,  tabby::palette::panel());
        b->setColour (juce::TextButton::textColourOffId, tabby::palette::text());
        addAndMakeVisible (*b);
    }
    undoBtn.onClick = [this] { if (! historyNavBlocked()) { proc.undo(); afterHistoryNav(); } };
    redoBtn.onClick = [this] { if (! historyNavBlocked()) { proc.redo(); afterHistoryNav(); } };
    refreshHistoryUi();

    addAndMakeVisible (infoButton);   // (i) build/version popover — top-bar right

    // ---- bottom toolbar (FabFilter-style): flat items, every popup opens UPWARD ----
    // Phase mode + Natural blend + Linear quality all live in ONE item now ("Zero Latency" /
    // "Natural Phase – 70" / "Linear Phase – High"); the old combos and the blend slider are gone.
    // The menu writes the params (a real edit — one labelled undo step); the label follows the
    // params through the timer poll, so host automation moves it too.
    modeItem.onClick   = [this] { showModeMenu(); };
    presetItem.onClick = [this] { showPresetMenu(); };
    saveItem.onClick   = [this] { doSavePreset(); };    saveItem.setTooltip ("Save preset");
    importItem.onClick = [this] { doImportPreset(); };  importItem.setTooltip ("Import preset…");
    exportItem.onClick = [this] { doExportPreset(); };  exportItem.setTooltip ("Export preset…");
    anaItem.onClick    = [this] { showAnalyzerPanel(); };
    for (juce::Component* it : std::initializer_list<juce::Component*> { &modeItem, &presetItem, &saveItem, &importItem, &exportItem, &anaItem })
        addAndMakeVisible (*it);

    latencyLabel.setJustificationType (juce::Justification::centredLeft);
    latencyLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    addAndMakeVisible (latencyLabel);
    updatePhaseUi();        // initial mode label + latency text
    refreshAnalyzerItem();  // initial "Analyzer: …" label

    gearBtn.onClick = [this] { showViewMenu(); };   // Reset lives in the preset menu ("Default")
    gearBtn.setTooltip ("View options");
    addAndMakeVisible (gearBtn);

    fullBtn.onClick = [this] { toggleFullscreen(); };
    fullBtn.setTooltip ("Fullscreen (f)");
    // Kiosk fullscreen only works on a window WE own. JUCEApplicationBase::isStandaloneApp() is
    // unreliable inside plugin wrappers (it keyed off a static and showed the button in Cubase);
    // the processor's wrapperType is the authoritative answer.
    fullBtn.setVisible (proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
    addAndMakeVisible (fullBtn);
    display.onToggleFullscreen = [this] { toggleFullscreen(); };            // 'f' key

    syncViewFromState();   // the view properties ride the state tree — also re-read on every history apply
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
        // Suppressed: a programmatic state write at editor-open must not seed a junk undo step
        // ("Parameter Change" that just removes a note flag) into the freshly loaded history.
        const felitronics::appkit::CompareHistory::ScopedSuppress ss (proc.compareHistory());
        proc.apvts.state.removeProperty ("migrationNote", nullptr);
    }

    setWantsKeyboardFocus (true);             // ⌘Z/⇧⌘Z, 1-4, ⌘C/⌘V — see keyPressed()
    setResizable (true, true);
    setResizeLimits (720, 256, 7680, 4320);   // drag-resize freely; maximise / fullscreen to any display
    setSize (860, 500);

    startTimerHz (10);   // undo settle pump — see timerCallback() (10 Hz × settleTicks 4 ≈ 0.4 s window)
}

// Re-read every view property the state tree carries — at editor open AND after every history
// apply (undo/switch/load may swap them; the display objects don't watch the tree themselves).
void TabbyEqEditor::syncViewFromState()
{
    display.setViewBandColors ((bool) proc.apvts.state.getProperty ("viewBandColors", true));
    display.setViewBandCurves ((bool) proc.apvts.state.getProperty ("viewBandCurves", true));
    display.setViewBandFill   ((bool) proc.apvts.state.getProperty ("viewBandFill",   false));
    display.setViewLongSolo   ((bool) proc.apvts.state.getProperty ("viewLongSolo",   true));
    display.setAddLineMode    ((int)  proc.apvts.state.getProperty ("addLineMode",    1));   // default: the 0 dB line only
    display.setAuditionVisual ((int)  proc.apvts.state.getProperty ("auditionVisual", 1));   // default: bell
    display.setAuditionQ      ((float) (double) proc.apvts.state.getProperty ("auditionQ", 6.0));
    display.setAuditionLockGain ((bool) proc.apvts.state.getProperty ("audLockGain", true));
    display.setToolbarPlacement ((int)  proc.apvts.state.getProperty ("toolbarPlace", 0));   // floating edit-strip behaviour
    display.setLeaderMode       ((int)  proc.apvts.state.getProperty ("leaderMode",   1));   // node<->strip leader: default Flash

    // The vertical dB scale rides the snapshot too — restore the VISIBLE range (gainRangeLive
    // first: what the user saw at capture, auto-fit included), so a recalled register's +20 dB
    // node never sits off-plot. Same-value re-writes inside setGainRange are suppressed no-ops.
    display.setGainRange ((double) proc.apvts.state.getProperty ("gainRangeLive",
                              proc.apvts.state.getProperty ("gainRange", 12.0)), false);

    proc.setSpectrumDomain ((int) proc.apvts.state.getProperty ("specDomain", 0));   // analyzer Stereo/Mid/Side

    // The analyzer settings ride the snapshot too (view state, suppressed writes — AnalyzerPanel).
    display.setAnalyzerShow  ((bool) proc.apvts.state.getProperty ("anaPre", false),
                              (bool) proc.apvts.state.getProperty ("anaPost", true));
    display.setAnalyzerRange ((double) proc.apvts.state.getProperty ("anaRange", 90.0));
    display.setAnalyzerSpeed ((int) proc.apvts.state.getProperty ("anaSpeed", 1));
    display.setAnalyzerTilt  ((double) proc.apvts.state.getProperty ("anaTilt", 4.5));
    refreshAnalyzerItem();
}

TabbyEqEditor::~TabbyEqEditor()
{
    // A host may destroy the editor MID-DRAG. The display's own dtor closes a node drag, but the
    // slider brackets (strip bars, output fader, phase blend) would leak their open gesture on the
    // out-living PROCESSOR — navigation gate stuck, settle commits blocked, forever. Close the
    // node drag first (so nothing double-ends), then force-close whatever brackets remain.
    display.endDragGesture();
    while (proc.historyGestureOpen())
        proc.endHistoryGesture();
}

void TabbyEqEditor::updatePhaseUi()
{
    // Param-driven (the old combos are gone): the bottom-bar mode item shows mode + its detail
    // ("Natural Phase – 70" / "Linear Phase – High"), the latency label sits beside it, coloured
    // dim (Zero) / yellow (Natural) / red (Linear). The timer poll re-runs this when the params
    // move, so host automation is reflected too.
    const int   mode = juce::jlimit (0, 2, (int) (proc.apvts.getRawParameterValue ("phaseMode")->load() + 0.5f));
    const int   q    = juce::jlimit (0, 4, (int) (proc.apvts.getRawParameterValue ("lpQuality")->load() + 0.5f));
    const float k    = proc.apvts.getRawParameterValue ("phaseAmount")->load();

    const auto dash = " " + juce::String::charToString (0x2013) + " ";   // en-dash (a char* literal would mojibake)
    juce::String label = "Zero Latency";
    if (mode == 1) label = "Natural Phase" + dash + juce::String (juce::roundToInt (k * 100.0f));
    if (mode == 2)
    {
        auto* lq = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("lpQuality"));
        label = "Linear Phase" + dash + (lq != nullptr ? lq->choices[q] : juce::String (q));
    }
    modeItem.setButtonText (label);

    if (mode <= 0)   // Zero Latency — no added delay
    {
        latencyLabel.setText ("0 ms", juce::dontSendNotification);
        latencyLabel.setColour (juce::Label::textColourId, tabby::palette::textDim());
        return;
    }
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
        samples = (double) sizes[q] / 2.0;
        col = juce::Colour (0xffff5a5a);
    }
    const double ms = samples / sr * 1000.0;
    // NB juce::String(double, 0) ignores the "0 decimals" and prints everything — round explicitly.
    latencyLabel.setText ((ms < 100.0 ? juce::String (ms, 1) : juce::String (juce::roundToInt (ms))) + " ms",
                          juce::dontSendNotification);
    latencyLabel.setColour (juce::Label::textColourId, col);
}

void TabbyEqEditor::showModeMenu()
{
    const int   mode = juce::jlimit (0, 2, (int) (proc.apvts.getRawParameterValue ("phaseMode")->load() + 0.5f));
    const int   q    = juce::jlimit (0, 4, (int) (proc.apvts.getRawParameterValue ("lpQuality")->load() + 0.5f));
    const int   kPct = juce::roundToInt (proc.apvts.getRawParameterValue ("phaseAmount")->load() * 100.0f);

    // Natural blend as fixed steps (10..100; 0 is omitted — k=0 IS linear phase, the mode below).
    // The checkmark demands an EXACT match: an off-grid k (host automation, e.g. 37) shows no tick
    // rather than lying with the nearest decade — picking a "ticked" item must never change sound.
    juce::PopupMenu natural;
    for (int p = 10; p <= 100; p += 10)
        natural.addItem (100 + p / 10, juce::String (p), true, mode == 1 && kPct == p);

    juce::PopupMenu linear;
    if (auto* lq = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter ("lpQuality")))
        for (int i = 0; i < lq->choices.size(); ++i)
            linear.addItem (200 + i, lq->choices[i], true, mode == 2 && q == i);

    juce::PopupMenu m;
    m.addItem (1, "Zero Latency", true, mode == 0);
    m.addSubMenu ("Natural Phase", natural, true, nullptr, mode == 1);
    m.addSubMenu ("Linear Phase",  linear,  true, nullptr, mode == 2);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&modeItem),
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this)] (int r)
        {
            if (safe == nullptr || r == 0) return;
            auto& p = safe->proc;
            // One menu pick = ONE labelled undo step (mode + its detail param move together).
            const TabbyEqAudioProcessor::ScopedHistoryGesture sg (p, "Phase Mode");
            auto set = [&p] (const char* id, float real)
            {
                if (auto* prm = p.apvts.getParameter (id))
                    prm->setValueNotifyingHost (prm->convertTo0to1 (real));
            };
            if      (r == 1)   { set ("phaseMode", 0.0f); }
            else if (r < 200)  { set ("phaseMode", 1.0f); set ("phaseAmount", (float) (r - 100) / 10.0f); }
            else               { set ("phaseMode", 2.0f); set ("lpQuality", (float) (r - 200)); }
            safe->updatePhaseUi();
        });
}

namespace
{
    // The analyzer settings popover (the bottom bar's "Analyzer:" item) — FabFilter-style rows.
    // Every value is VIEW state on the state tree: suppressed writes (no undo steps; the props
    // still ride the snapshot), re-applied by syncViewFromState after each history apply. Freeze
    // is runtime-only and never persisted.
    class AnalyzerPanel final : public juce::Component
    {
    public:
        AnalyzerPanel (TabbyEqAudioProcessor& p, EqCurveDisplay& d, std::function<void()> onChangedFn)
            : proc (p), display (d), onChanged (std::move (onChangedFn))
        {
            auto initToggle = [this] (juce::TextButton& b, const char* text, bool on)
            {
                b.setButtonText (text);
                b.setClickingTogglesState (true);
                b.setToggleState (on, juce::dontSendNotification);
                b.setColour (juce::TextButton::buttonColourId,   tabby::palette::panel());
                b.setColour (juce::TextButton::buttonOnColourId, tabby::palette::violet().withAlpha (0.55f));
                b.setColour (juce::TextButton::textColourOffId,  tabby::palette::textDim());
                b.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
                addAndMakeVisible (b);
            };
            initToggle (preBtn,  "Pre",  (bool) proc.apvts.state.getProperty ("anaPre",  false));
            initToggle (postBtn, "Post", (bool) proc.apvts.state.getProperty ("anaPost", true));
            preBtn.onClick  = [this] { setProp ("anaPre",  preBtn.getToggleState()); };
            postBtn.onClick = [this] { setProp ("anaPost", postBtn.getToggleState()); };

            auto initRow = [this] (juce::Label& l, const char* text, FlatItem& v)
            {
                l.setText (text, juce::dontSendNotification);
                l.setFont (juce::Font (juce::FontOptions (12.0f)));
                l.setColour (juce::Label::textColourId, tabby::palette::textDim());
                addAndMakeVisible (l);
                addAndMakeVisible (v);
            };
            initRow (rangeLbl, "Range:", rangeVal);
            initRow (speedLbl, "Speed:", speedVal);
            initRow (tiltLbl,  "Tilt:",  tiltVal);
            rangeVal.onClick = [this] { rangeMenu(); };
            speedVal.onClick = [this] { speedMenu(); };
            tiltVal.onClick  = [this] { tiltMenu(); };

            initToggle (freezeBtn, "Freeze", display.analyzerFrozen());
            freezeBtn.onClick = [this] { display.setAnalyzerFrozen (freezeBtn.getToggleState()); };

            refreshValues();
            setSize (216, 150);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (tabby::palette::panel());   // the CallOutBox draws its own bubble; keep rows readable
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (10, 8);
            auto rowTop = r.removeFromTop (24);
            preBtn.setBounds (rowTop.removeFromLeft (62));
            rowTop.removeFromLeft (6);
            postBtn.setBounds (rowTop.removeFromLeft (62));
            r.removeFromTop (6);
            for (auto row : { std::pair<juce::Label*, FlatItem*> { &rangeLbl, &rangeVal },
                              { &speedLbl, &speedVal }, { &tiltLbl, &tiltVal } })
            {
                auto line = r.removeFromTop (22);
                row.first->setBounds (line.removeFromLeft (86));
                row.second->setBounds (line);
            }
            r.removeFromTop (6);
            freezeBtn.setBounds (r.removeFromTop (24).removeFromLeft (86));
        }

    private:
        void setProp (const juce::Identifier& id, const juce::var& v)
        {
            // Value-guard BEFORE the suppress scope: an empty ScopedSuppress still flushes a
            // pending edit burst on entry (the activeLane bug class) — re-picking the already-
            // selected value must be a TRUE no-op.
            if (proc.apvts.state.getProperty (id) == v)
                { refreshValues(); return; }
            // View state, not an edit: suppressed — records no undo step. The editor's refresh
            // callback re-applies the state to the display and relabels the bottom-bar item.
            {
                const felitronics::appkit::CompareHistory::ScopedSuppress ss (proc.compareHistory());
                proc.apvts.state.setProperty (id, v, nullptr);
            }
            if (onChanged) onChanged();
            refreshValues();
        }

        void refreshValues()
        {
            const int range = (int) proc.apvts.state.getProperty ("anaRange", 90);
            rangeVal.setButtonText (juce::String (range) + " dB");
            static const char* speeds[] = { "Slow", "Medium", "Fast" };
            speedVal.setButtonText (speeds[juce::jlimit (0, 2, (int) proc.apvts.state.getProperty ("anaSpeed", 1))]);
            tiltVal.setButtonText (juce::String ((double) proc.apvts.state.getProperty ("anaTilt", 4.5), 1) + " dB/oct");
        }

        void rangeMenu()
        {
            juce::PopupMenu m;
            const int cur = (int) proc.apvts.state.getProperty ("anaRange", 90);
            for (const int v : { 60, 90, 120 })
                m.addItem (v, juce::String (v) + " dB", true, cur == v);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rangeVal),
                [safe = juce::Component::SafePointer<AnalyzerPanel> (this)] (int r)
                { if (safe != nullptr && r > 0) safe->setProp ("anaRange", r); });
        }

        void speedMenu()
        {
            juce::PopupMenu m;
            const int cur = juce::jlimit (0, 2, (int) proc.apvts.state.getProperty ("anaSpeed", 1));
            static const char* speeds[] = { "Slow", "Medium", "Fast" };
            for (int i = 0; i < 3; ++i)
                m.addItem (i + 1, speeds[i], true, cur == i);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&speedVal),
                [safe = juce::Component::SafePointer<AnalyzerPanel> (this)] (int r)
                { if (safe != nullptr && r > 0) safe->setProp ("anaSpeed", r - 1); });
        }

        void tiltMenu()
        {
            juce::PopupMenu m;
            const double cur = (double) proc.apvts.state.getProperty ("anaTilt", 4.5);
            static constexpr double tilts[] = { 0.0, 1.5, 3.0, 4.5, 6.0 };
            for (int i = 0; i < 5; ++i)
                m.addItem (i + 1, juce::String (tilts[i], 1) + " dB/oct", true, std::abs (cur - tilts[i]) < 0.01);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&tiltVal),
                [safe = juce::Component::SafePointer<AnalyzerPanel> (this)] (int r)
                { if (safe != nullptr && r > 0) safe->setProp ("anaTilt", tilts[r - 1]); });
        }

        TabbyEqAudioProcessor& proc;
        EqCurveDisplay&        display;
        std::function<void()>  onChanged;
        juce::TextButton preBtn, postBtn, freezeBtn;
        juce::Label rangeLbl, speedLbl, tiltLbl;
        FlatItem    rangeVal, speedVal, tiltVal;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerPanel)
    };
}

void TabbyEqEditor::showAnalyzerPanel()
{
    auto panel = std::make_unique<AnalyzerPanel> (proc, display,
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this)]
        {
            if (safe != nullptr) { safe->syncViewFromState(); safe->refreshAnalyzerItem(); }
        });
    // Parent the call-out to the editor, NOT the desktop (the VersionInfo pattern): the panel holds
    // editor members by reference, so a desktop call-out surviving the editor would be a UAF the
    // moment Freeze is clicked. As a child of the top-level editor it dies with the window.
    if (auto* top = getTopLevelComponent())
        juce::CallOutBox::launchAsynchronously (std::move (panel),
                                                top->getLocalArea (&anaItem, anaItem.getLocalBounds()), top);
    else
        juce::CallOutBox::launchAsynchronously (std::move (panel), anaItem.getScreenBounds(), nullptr);
}

void TabbyEqEditor::refreshAnalyzerItem()
{
    const bool pre  = (bool) proc.apvts.state.getProperty ("anaPre",  false);
    const bool post = (bool) proc.apvts.state.getProperty ("anaPost", true);
    anaItem.setButtonText ("Analyzer:  " + juce::String (pre && post ? "Pre+Post" : pre ? "Pre" : post ? "Post" : "Off"));
}

void TabbyEqEditor::showPresetMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "Default", true, currentPresetName == "Default");
    auto files = TabbyEqAudioProcessor::presetDirectory().findChildFiles (juce::File::findFiles, false, "*.tabbyeq");
    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });
    if (! files.isEmpty()) m.addSeparator();
    for (int i = 0; i < files.size(); ++i)
        m.addItem (100 + i, files[i].getFileNameWithoutExtension(), true,
                   files[i].getFileNameWithoutExtension() == currentPresetName);

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetItem),
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this), files] (int r)
        {
            // Preset loads are history-consuming (fromTree/reset) — same gate as every nav path:
            // a pick landing mid-drag (second input device) must be inert, not an engine misuse.
            if (safe == nullptr || r == 0 || safe->historyNavBlocked()) return;
            if (r == 1) { safe->resetAll(); return; }             // "Default" = the reset, as a preset
            const auto f = files[r - 100];
            if (safe->proc.loadStateFile (f))                     // fresh clean history (setStateInformation path)
            {
                safe->currentPresetName = f.getFileNameWithoutExtension();
                safe->presetItem.setButtonText (safe->currentPresetName);
                safe->afterHistoryNav();
            }
        });
}

void TabbyEqEditor::doSavePreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Save preset",
        TabbyEqAudioProcessor::presetDirectory().getChildFile (
            (currentPresetName == "Default" ? juce::String ("My Preset") : currentPresetName) + ".tabbyeq"),
        "*.tabbyeq");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this)] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe == nullptr || f == juce::File()) return;
            if (safe->proc.saveStateFile (f.withFileExtension ("tabbyeq")))
            {
                safe->currentPresetName = f.getFileNameWithoutExtension();
                safe->presetItem.setButtonText (safe->currentPresetName);
            }
        });
}

void TabbyEqEditor::doImportPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Import preset", juce::File(), "*.tabbyeq;*.xml");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this)] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe == nullptr || f == juce::File() || safe->historyNavBlocked()) return;
            if (! safe->proc.loadStateFile (f))
                return;                                           // not a TabbyEQ state — silently ignore
            // Import = load AND adopt: copy into the preset directory so it shows up in the menu.
            // Never clobber an existing preset of the same name — uniquify instead.
            auto dest = TabbyEqAudioProcessor::presetDirectory()
                            .getChildFile (f.getFileNameWithoutExtension() + ".tabbyeq");
            if (f != dest)
            {
                if (dest.existsAsFile()) dest = dest.getNonexistentSibling();
                f.copyFileTo (dest);
            }
            safe->currentPresetName = dest.getFileNameWithoutExtension();
            safe->presetItem.setButtonText (safe->currentPresetName);
            safe->afterHistoryNav();
        });
}

void TabbyEqEditor::doExportPreset()
{
    chooser = std::make_unique<juce::FileChooser> ("Export preset",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ((currentPresetName == "Default" ? juce::String ("TabbyEQ Preset") : currentPresetName) + ".tabbyeq"),
        "*.tabbyeq");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<TabbyEqEditor> (this)] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (safe != nullptr && f != juce::File())
                safe->proc.saveStateFile (f.withFileExtension ("tabbyeq"));
        });
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
    // Link FQ / Link Q are per-point (edited in the lane dropdown, mirrored processor-side). Here the View
    // menu edits ONLY the GLOBAL defaults seeded into newly split points (both default ON).
    m.addItem (40, "New splits: Link FQ default", true, (bool) proc.apvts.state.getProperty ("defaultLinkFq", true));
    m.addItem (41, "New splits: Link Q default",  true, (bool) proc.apvts.state.getProperty ("defaultLinkQ", true));

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

    juce::PopupMenu leadMenu;    // node<->strip leader line — governs every placement mode uniformly
    const int lm = display.leaderMode();
    const char* leadNames[] = { "Off", "Flash", "Always" };
    for (int i = 0; i < 3; ++i) leadMenu.addItem (50 + i, leadNames[i], true, lm == i);
    m.addSubMenu ("Leader line", leadMenu);

    juce::Component::SafePointer<TabbyEqEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gearBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& d  = safe->display;
        auto& st = safe->proc.apvts.state;
        // Suppressed: View-menu toggles are view state, not edits (same policy as the lane tabs
        // and the vertical scale) — they must not settle into junk "Parameter Change" undo steps.
        const felitronics::appkit::CompareHistory::ScopedSuppress ss (safe->proc.compareHistory());
        if (r == 1) { const bool v = ! d.viewBandColors(); d.setViewBandColors (v); st.setProperty ("viewBandColors", v, nullptr); }
        if (r == 2) { const bool v = ! d.viewBandCurves(); d.setViewBandCurves (v); st.setProperty ("viewBandCurves", v, nullptr); }
        if (r == 3) { const bool v = ! d.viewBandFill();   d.setViewBandFill (v);   st.setProperty ("viewBandFill", v, nullptr); }
        if (r == 4) { const bool v = ! d.viewLongSolo();   d.setViewLongSolo (v);   st.setProperty ("viewLongSolo", v, nullptr); }
        if (r >= 10 && r <= 13) { const int mode = r - 10; d.setAddLineMode (mode); st.setProperty ("addLineMode", mode, nullptr); }
        if (r == 20 || r == 21) { const int v = r - 20; d.setAuditionVisual (v); st.setProperty ("auditionVisual", v, nullptr); }
        if (r == 22) { const bool v = ! d.auditionLockGain(); d.setAuditionLockGain (v); st.setProperty ("audLockGain", v, nullptr); }
        if (r >= 30 && r <= 33) { const int qv[] = { 3, 6, 9, 12 }; const float q = (float) qv[r - 30]; d.setAuditionQ (q); st.setProperty ("auditionQ", q, nullptr); }
        if (r == 40 || r == 41)   // GLOBAL new-split default only (per-point linking lives in the lane dropdown)
        {
            const char* gKey = (r == 40) ? "defaultLinkFq" : "defaultLinkQ";
            st.setProperty (gKey, ! (bool) st.getProperty (gKey, true), nullptr);
        }
        if (r >= 50 && r <= 52) { const int lv = r - 50; d.setLeaderMode (lv); st.setProperty ("leaderMode", lv, nullptr); }
        if (r >= 60 && r <= 62) { const int dn = r - 60; safe->proc.setSpectrumDomain (dn); st.setProperty ("specDomain", dn, nullptr); }
        if (r >= 70 && r <= 74) { const int mp = r - 70; d.setToolbarPlacement (mp); st.setProperty ("toolbarPlace", mp, nullptr); }
    });
}

void TabbyEqEditor::toggleFullscreen()
{
    // Don't kiosk a DAW's window — and gate on wrapperType, the AUTHORITATIVE check: this also
    // covers the canvas 'f' key path (isStandaloneApp() lied inside Cubase's wrapper).
    if (proc.wrapperType != juce::AudioProcessor::wrapperType_Standalone) return;
    auto& d = juce::Desktop::getInstance();
    if (d.getKioskModeComponent() != nullptr) d.setKioskModeComponent (nullptr);          // exit
    else if (auto* top = getTopLevelComponent()) d.setKioskModeComponent (top, false);    // enter (borderless)
}

void TabbyEqEditor::resetAll()
{
    if (historyNavBlocked()) return;   // reset() asserts on an open gesture — same gate as every nav path
    // Suppressed (Oleh's call): Reset is a programmatic bulk write and records NO undo step — the
    // writes apply, the settle baseline absorbs them, and earlier history still walks back past the
    // reset. (The alternative — a "Reset All" gesture bracket making the reset itself one undoable
    // step — was raised and can be flipped later without touching anything else.) The processor
    // method also drains the link-mirror FIFO inside the scope on both sides.
    proc.resetAllToDefaults();
    proc.setSoloBand (-1);                         // clear any solo
    display.clearSelection();                       // deselect + hide the floating toolbar
    currentPresetName = "Default";                  // Reset lives in the preset menu as "Default"
    presetItem.setButtonText ("Default");
    afterHistoryNav();                              // markers/enablement (redo is now stale by contract)
}

void TabbyEqEditor::paint (juce::Graphics& g)
{
    g.fillAll (tabby::palette::bg());
    // The toolbar-bottom line is one continuous stroke drawn by the ToolbarUnderline overlay (on
    // top), so it is a single uniform hairline that dips under the blister — no junction here.
}

void TabbyEqEditor::resized()
{
    static constexpr int kBarH = 30, kBlisterH = 46;   // the flat toolbar band + the taller brand badge

    auto r   = getLocalBounds();
    auto top = r.removeFromTop (kBarH);                 // the flat band for everything EXCEPT the brand blister

    // Right corner (pinned, never moves): gear · (i) · fullscreen. Removed right-to-left so they
    // read gear → (i) → full left-to-right.
    if (fullBtn.isVisible())
        fullBtn.setBounds (top.removeFromRight (26).reduced (2, 4));    // fullscreen — the very corner (standalone)
    infoButton.setBounds (top.removeFromRight (24).reduced (3, 5));     // (i)
    gearBtn.setBounds (top.removeFromRight (26).reduced (2, 4));        // view options gear
    top.removeFromRight (4);
    const int bandRight = top.getRight();              // the group centres in [0 .. bandRight]

    // ONE contiguous group — [cat + TabbyEQ blister] · undo/redo · A/B/C/D · preset · Save/Import/
    // Export — that stays TOGETHER and floats to the centre of the free band (FabFilter). The logo
    // and the buttons never separate; the flexible margins on both sides keep the group centred.
    const int brandW = brand.preferredWidth (kBlisterH);
    constexpr int undoW = 22, redoW = 22, tGap = 6, abcdW = 26, gGap = 14;
    constexpr int presetW = 104, saveW = 26, importW = 26, exportW = 26;   // save/import/export are icons now
    const int groupW = brandW + undoW + redoW + tGap + TabbyEqAudioProcessor::kNumSnapshots * abcdW
                     + gGap + presetW + saveW + importW + exportW;

    // The whole group floats to the centre of the free band; as the window narrows it slides left.
    // It may slide PAST x=0 (negative) — but only until the CAT's left edge reaches a small margin;
    // the blister then keeps sliding its left skirt OFF-SCREEN (the window clips it into a straight
    // rectangular cut) with NO content jump. The cat never crosses the window edge.
    constexpr int kCatMargin = 4;
    const int xMin = kCatMargin - (int) HeaderBrand::contentLeftOffset();   // blister left when the cat butts the edge
    int x = juce::jmax (xMin, (bandRight - groupW) / 2);
    const auto barItem = [&x] (juce::Component& c, int w, int vInset) { c.setBounds (x, vInset, w, kBarH - 2 * vInset); x += w; };

    brand.setBounds (x, 0, brandW, kBlisterH);          x += brandW;   // the blister protrudes; the rest sit in the bar
    barItem (undoBtn, undoW, 5);
    barItem (redoBtn, redoW, 5);
    x += tGap;
    for (auto& b : snapBtn) barItem (b, abcdW, 5);
    x += gGap;
    barItem (presetItem, presetW, 4);
    x += 6;                               // a touch of air between the preset name and the icon trio
    barItem (saveItem,   saveW,   2);
    barItem (importItem, importW, 2);
    barItem (exportItem, exportW, 2);

    // ---- three vertical blocks: |IN meter| spectrum |OUT meter + fader| ------------------------
    // The rails run the FULL height below the top bar; the bottom toolbar belongs to the MIDDLE
    // block only. No IN/OUT captions — the meters carry tooltips instead.
    auto leftRail  = r.removeFromLeft (30);
    auto rightRail = r.removeFromRight (64);

    inMeter.setBounds (leftRail.reduced (7, 6));
    {
        auto rr = rightRail.reduced (5, 6);
        // The output VALUE editor (the slider's own text box) sits at the rail's bottom, under BOTH
        // the meter and the fader — the slider spans the full rail width so its TextBoxBelow lands
        // there; the meter overlaps the slider's (empty) left margin, clear of the centred track.
        outMeter.setBounds (rr.getX(), rr.getY(), 14, rr.getHeight() - 20);
        output.setBounds (rr);
    }

    // ---- bottom toolbar (middle block only): mode+latency left · Analyzer centred · corr right --
    auto bottom = r.removeFromBottom (22);
    bottom.reduce (8, 0);                                            // align with the display's insets
    const auto middleStrip = bottom;                                 // full middle width (for centring)
    modeItem.setBounds (bottom.removeFromLeft (150).reduced (2, 1));
    latencyLabel.setBounds (bottom.removeFromLeft (58).reduced (0, 1));
    corrMeter.setBounds (bottom.removeFromRight (76).reduced (0, 5));   // right end == the spectrum's right edge
    anaItem.setBounds (juce::Rectangle<int> (middleStrip.getCentreX() - 70, middleStrip.getY() + 1, 140,
                                             middleStrip.getHeight() - 2));

    // The graph starts RIGHT under the toolbar line — no black inset strip below it (the toolbar
    // must end AT the line). Keep the side/bottom insets, drop the top one.
    display.setBounds (r.reduced (8, 0).withTrimmedBottom (4));

    underline.setBounds (getLocalBounds());   // full-width overlay; draws only the toolbar-bottom hairline
    underline.toFront (false);                // stay on top after any child re-add
}

//==============================================================================
// A/B/C/D compare registers — recall + copy (right-click menu / drag-n-drop / system clipboard).
// Every copy path lands in the engine (copyRegister / applyEdit): ONE discrete undoable edit in
// the TARGET register's own history; a byte-equal copy records nothing.
//==============================================================================
void TabbyEqEditor::updateSnapshotButtons()
{
    const int a = proc.getActiveSnapshot();
    for (int i = 0; i < TabbyEqAudioProcessor::kNumSnapshots; ++i)
    {
        snapBtn[i].setToggleState (i == a, juce::dontSendNotification);
        snapBtn[i].setEdited (proc.snapshotEdited (i));   // "modified since you dialed it in" dot
    }
}

// History NAVIGATION is blocked while any gesture is open — for the keyboard AND for the pointer
// paths (undo/redo buttons, A/B/C/D clicks, snapshot drag-drop, menu copy/paste): with a second
// input source a click can land mid-drag, force-close the engine gesture and bleed the drag's
// tail into the post-undo / recalled register.
bool TabbyEqEditor::historyNavBlocked() const
{
    return display.isDragActive() || proc.historyGestureOpen();
}

void TabbyEqEditor::switchSnapshot (int i)
{
    if (historyNavBlocked()) return;
    // Recall register i (the engine stashes the live state into the register we leave first),
    // then re-sync the editor — lane sets, links, colours and view properties may all have changed.
    proc.switchToSnapshot (i);
    afterHistoryNav();
}

// Full editor re-sync after any history navigation (undo / redo / switch / copy / paste). The
// timerCallback polls cover the same ground as a safety net (host-driven loads, other editors);
// calling it directly keeps editor-initiated actions snappy at the 10 Hz poll rate.
void TabbyEqEditor::afterHistoryNav()
{
    display.refreshAfterLaneEdit();
    syncViewFromState();
    revalidateSolo();
    updateSnapshotButtons();
    refreshHistoryUi();
    lastHistoryRev = proc.historyRevision();   // this re-sync already covered these revisions —
    lastApplyRev   = proc.applyRevision();     // don't repeat the full refresh on the next tick
}

// The band-listen solo is processor-transient (not part of the snapshot): a history apply that
// turns the soloed band off would otherwise keep the audio band-passed with zero UI indication
// (the SOLO overlay and the node both vanish with the band).
void TabbyEqEditor::revalidateSolo()
{
    const int s = proc.getSoloBand();
    if (s >= 0)
        if (auto* on = proc.apvts.getRawParameterValue (tabby::bandId (s, "on")); on == nullptr || on->load() <= 0.5f)
            proc.setSoloBand (-1);
}

void TabbyEqEditor::refreshHistoryUi()
{
    undoBtn.setEnabled (proc.canUndo());
    redoBtn.setEnabled (proc.canRedo());
    // Peek-label tooltips: an unlabelled settle burst reads as "Parameter Change".
    const auto pretty = [] (const juce::String& l) { return l.isEmpty() ? juce::String ("Parameter Change") : l; };
    const auto hint   = [] (juce::ModifierKeys::Flags mods)
    { return " (" + juce::KeyPress ('z', mods, 0).getTextDescriptionWithIcons() + ")"; };
    undoBtn.setTooltip ((proc.canUndo() ? "Undo " + pretty (proc.peekUndoLabel()) : juce::String ("Undo"))
                        + hint (juce::ModifierKeys::commandModifier));
    redoBtn.setTooltip ((proc.canRedo() ? "Redo " + pretty (proc.peekRedoLabel()) : juce::String ("Redo"))
                        + hint (juce::ModifierKeys::Flags (juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier)));
}

bool TabbyEqEditor::keyPressed (const juce::KeyPress& key)
{
    using MK = juce::ModifierKeys;

    // While ANY history gesture is open (node/whisker drag, a strip/fader slider drag, a "+"-place
    // drag), history-navigation keys are inert: a switch/undo/paste mid-gesture would hit the
    // engine's gesture×navigation misuse path (force-commit + debug assert), and a register switch
    // under a live slider drag would bleed the drag's tail into the recalled register. Swallow.
    // (The pointer nav paths — buttons, drag-drop, menus — check the same gate at their entries.)
    const bool midDrag = historyNavBlocked();

    // ⌘Z / ⇧⌘Z — undo/redo on the active register's own history. Swallowed even when there is
    // nothing to undo (the plugin owns the shortcut while focused; letting it fall through would
    // trigger the HOST's undo — far more destructive than a no-op).
    if (key == juce::KeyPress ('z', MK::commandModifier, 0))                     { if (! midDrag) { proc.undo(); afterHistoryNav(); } return true; }
    if (key == juce::KeyPress ('z', MK::commandModifier | MK::shiftModifier, 0)) { if (! midDrag) { proc.redo(); afterHistoryNav(); } return true; }

    // 1..4 → A/B/C/D register switch. Digit row, no modifiers — a focused text field consumes its
    // own digits before they bubble here, so numeric entry is unaffected.
    for (int i = 0; i < TabbyEqAudioProcessor::kNumSnapshots; ++i)
        if (key == juce::KeyPress ((juce::juce_wchar) ('1' + i)))
        {
            if (! midDrag)
                switchSnapshot (i);
            return true;
        }

    // ⌘C / ⌘V — copy/paste the ACTIVE register via the system clipboard (the right-click menu
    // reaches any register). ⌘C stays live mid-drag (read-only). Mid-drag ⌘V is swallowed like the
    // other navigation keys (same policy: inert, never the host's). Otherwise an unrecognised
    // clipboard falls through to the host (return false), so a stray ⌘V isn't swallowed.
    if (key == juce::KeyPress ('c', MK::commandModifier, 0)) { copySnapshotToClipboard (proc.getActiveSnapshot()); return true; }
    if (key == juce::KeyPress ('v', MK::commandModifier, 0)) return midDrag || pasteSnapshotFromClipboard (proc.getActiveSnapshot());

    return false;
}

void TabbyEqEditor::showSnapshotMenu (int i)
{
    // Sources for "copy here from": only content-bearing registers (the active one or a stored
    // snapshot) — copying from a never-visited slot is meaningless. Targets are unrestricted.
    juce::PopupMenu from, to;
    for (int j = 0; j < TabbyEqAudioProcessor::kNumSnapshots; ++j)
    {
        if (j == i) continue;
        const auto name = snapBtn[j].getButtonText();
        if (proc.snapshotHasContent (j))
            from.addItem (100 + j, name);
        to.addItem (200 + j, name);
    }

    const bool hasContent = proc.snapshotHasContent (i);
    juce::PopupMenu m;
    m.addSectionHeader ("Snapshot " + snapBtn[i].getButtonText());
    m.addSubMenu ("Copy here from", from, from.getNumItems() > 0);
    m.addSubMenu ("Copy this to",   to,   hasContent);
    m.addSeparator();
    m.addItem (1, "Copy state", hasContent);
    {
        // Enablement uses the SAME predicate the paste itself enforces — a hollow or foreign
        // tree must show as disabled, not as an enabled item that silently no-ops.
        const auto clip = juce::ValueTree::fromXml (juce::SystemClipboard::getTextFromClipboard());
        m.addItem (2, "Paste state", proc.canPasteState (clip));
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&snapBtn[i]),
        [this, safe = juce::Component::SafePointer<TabbyEqEditor> (this), i] (int r)
        {
            if (safe == nullptr || r == 0) return;
            if      (r == 1)        copySnapshotToClipboard (i);
            else if (r == 2)        pasteSnapshotFromClipboard (i);
            else if (r >= 200)      applySnapshotCopy (i, r - 200);   // "copy this to" → i is the source
            else if (r >= 100)      applySnapshotCopy (r - 100, i);   // "copy here from" → i is the target
        });
}

void TabbyEqEditor::applySnapshotCopy (int from, int to)
{
    if (from == to || historyNavBlocked()) return;
    proc.copySnapshot (from, to);
    afterHistoryNav();   // live may have changed (copy INTO the active register); markers always
}

void TabbyEqEditor::copySnapshotToClipboard (int i)
{
    // The register's whole state tree (params + view/session properties) as XML text — a paste in
    // another TabbyEQ instance re-imports it through the validated pasteState path.
    if (const auto t = proc.snapshotState (i); t.isValid())
        if (const auto xml = t.createXml())
            juce::SystemClipboard::copyTextToClipboard (xml->toString());
}

bool TabbyEqEditor::pasteSnapshotFromClipboard (int toReg)
{
    // The clipboard is untrusted input: accept only a tree of this plugin's state type (the same
    // predicate the menu enablement showed; applyLiveState re-validates at the apply seam).
    const auto t = juce::ValueTree::fromXml (juce::SystemClipboard::getTextFromClipboard());
    if (historyNavBlocked() || ! proc.canPasteState (t))
        return false;
    proc.pasteState (toReg, t);
    afterHistoryNav();
    return true;
}
