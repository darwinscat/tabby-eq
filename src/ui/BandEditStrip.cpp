// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/BandEditStrip.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"

namespace
{
    const char* kTypeNames[9] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };

    // Lane indices (teq::Lane order): ST=0, L=1, R=2, M=3, S=4. The two-lane UX uses ST / M / S only.
    enum { LaneSt = 0, LaneM = 3, LaneS = 4 };
}

BandEditStrip::BandEditStrip (TabbyEqAudioProcessor& p) : proc (p)
{
    title.setText ("—", juce::dontSendNotification);
    title.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
    title.setColour (juce::Label::textColourId, tabby::palette::text());
    title.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (title);

    onButton.onClick = [this]
    {
        if (bypassAtt != nullptr)
            bypassAtt->setValueAsCompleteGesture (onButton.getToggleState() ? 1.0f : 0.0f);   // lit -> bypass the point
    };
    addAndMakeVisible (onButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    soloButton.onClick = [this] { proc.setSoloBand (soloButton.getToggleState() ? curBand : -1); };
    addAndMakeVisible (soloButton);

    // Type: a button (showing the current type) that opens a menu whose items carry shape icons. The type is
    // SHARED by all of the point's lanes (decision #2), so this edits band{b}_type regardless of the edit lane.
    typeButton.onClick = [this] { showTypeMenu(); };
    addAndMakeVisible (typeButton);

    modeButton.onClick = [this] { toggleMs(); };                // ST <-> M/S
    modeButton.setColour (juce::TextButton::buttonColourId, tabby::palette::panel().brighter (0.18f));
    addAndMakeVisible (modeButton);

    midTab.setClickingTogglesState (true);  sideTab.setClickingTogglesState (true);
    midTab.setColour  (juce::TextButton::buttonOnColourId, tabby::palette::violet());
    sideTab.setColour (juce::TextButton::buttonOnColourId, tabby::palette::orange());
    midTab.onClick  = [this] { setLane (false); };
    sideTab.onClick = [this] { setLane (true); };
    addChildComponent (midTab);                                 // shown only when split
    addChildComponent (sideTab);

    prevButton.onClick = [this] { if (onStep) onStep (-1); };   // editor maps these to display.stepSelection
    nextButton.onClick = [this] { if (onStep) onStep (+1); };
    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);

    const char* slopes[] = { "6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
    for (int i = 0; i < 7; ++i) slopeBox.addItem (slopes[i], i + 1);
    addAndMakeVisible (slopeBox);

    auto setupField = [this] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearBar);          // value + unit live inside the bar; editing types the number
        s.setColour (juce::Slider::trackColourId,       tabby::palette::violet().withAlpha (0.45f));
        s.setColour (juce::Slider::textBoxTextColourId, tabby::palette::text());
        addAndMakeVisible (s);
    };
    setupField (freq);
    setupField (q);
    setupField (gain);

    freq.onDragEnd = [this] { if (onEdited) onEdited(); };   // re-place the toolbar AFTER the bar drag ends
    gain.onDragEnd = [this] { if (onEdited) onEdited(); };

    setAlpha (0.55f);                 // translucent by default; opaque while the mouse is over / editing
    addMouseListener (this, true);    // also receive child mouse events for the hover-opacity logic

    setBand (-1);
}

void BandEditStrip::updateOpacity()
{
    setAlpha (isMouseOverOrDragging (true) ? 1.0f : 0.55f);
}

void BandEditStrip::mouseEnter (const juce::MouseEvent&) { updateOpacity(); }
void BandEditStrip::mouseExit  (const juce::MouseEvent&) { updateOpacity(); }
void BandEditStrip::mouseUp    (const juce::MouseEvent&) { updateOpacity(); }

BandEditStrip::~BandEditStrip() { proc.setSoloBand (-1); }   // never leave audio stuck in solo

int BandEditStrip::activeLaneIndex() const
{
    return curMs ? (editingSide ? LaneS : LaneM) : LaneSt;
}

void BandEditStrip::setBand (int band)
{
    curBand = band;
    const bool has = curBand >= 0;
    // "split" (the two-lane UX's M/S mode) = the Mid or Side lane is enabled. Migration fission can make
    // single-domain ({m}-only / {s}-only) points — clamp the edit lane to a lane that actually EXISTS.
    const bool mOn = has && proc.apvts.getRawParameterValue (tabby::laneParamId (curBand, LaneM, "on"))->load() > 0.5f;
    const bool sOn = has && proc.apvts.getRawParameterValue (tabby::laneParamId (curBand, LaneS, "on"))->load() > 0.5f;
    curMs = mOn || sOn;
    if (! curMs)                      editingSide = false;
    else if (editingSide && ! sOn)    editingSide = false;   // {m}-only: no Side lane to edit
    else if (! editingSide && ! mOn)  editingSide = true;    // {s}-only: no Mid lane to edit

    title.setText (has ? juce::String (curBand + 1) : juce::String ("—"), juce::dontSendNotification);
    juce::Component* controls[] = { &onButton, &soloButton, &typeButton, &prevButton, &nextButton,
                                    &modeButton, &midTab, &sideTab, &slopeBox, &freq, &q, &gain };
    for (auto* c : controls) c->setEnabled (has);
    midTab.setEnabled (mOn);   sideTab.setEnabled (sOn);     // single-domain points: only the live lane's tab

    modeButton.setButtonText (curMs ? "M/S" : "ST");
    modeButton.setColour (juce::TextButton::textColourOffId, curMs ? tabby::palette::orange() : tabby::palette::text());
    midTab.setVisible (curMs);   sideTab.setVisible (curMs);                    // M/S lane tabs — RIGHT of ST
    prevButton.setVisible (true); nextButton.setVisible (true); title.setVisible (true);   // < index > nav — always
    midTab.setToggleState (! editingSide, juce::dontSendNotification);
    sideTab.setToggleState (editingSide, juce::dontSendNotification);

    typeButton.setType (tabby::filterTypeFromChoice (bandTypeIndex()));

    rebind();   // (re)creates the lane attachments + point-bypass attachment (drives the power button)
    updateForType();
    soloButton.setToggleState (has && proc.getSoloBand() == curBand, juce::dontSendNotification);
    if (has) proc.setBandActiveLane (curBand, activeLaneIndex());
    resized();   // top-row layout depends on split
    repaint();
}

juce::String BandEditStrip::laneId (const juce::String& base) const   // lane-scoped id (ST / Mid / Side)
{
    const int lane = activeLaneIndex();
    const juce::String field = (base == "bypass") ? "byp" : base;
    return tabby::laneParamId (curBand, lane, field);
}

int BandEditStrip::bandTypeIndex() const   // the point's SHARED filter type
{
    if (curBand < 0) return 0;
    if (auto* prm = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (tabby::bandId (curBand, "type"))))
        return juce::jlimit (0, 8, prm->getIndex());
    return 0;
}

void BandEditStrip::setActiveLane (bool side)
{
    // Never point the strip at a disabled lane (single-domain points from migration fission).
    if (curBand >= 0 && curMs)
    {
        auto laneOn = [this] (int lane)
        { return proc.apvts.getRawParameterValue (tabby::laneParamId (curBand, lane, "on"))->load() > 0.5f; };
        if      (side   && ! laneOn (LaneS)) side = false;
        else if (! side && ! laneOn (LaneM)) side = true;
    }
    editingSide = side;
    midTab.setToggleState (! side, juce::dontSendNotification);
    sideTab.setToggleState (side, juce::dontSendNotification);
    typeButton.setType (tabby::filterTypeFromChoice (bandTypeIndex()));
    rebind();
    updateForType();
    if (curBand >= 0) proc.setBandActiveLane (curBand, activeLaneIndex());
    repaint();
}

void BandEditStrip::setLane (bool side)   // tab click: switch lane AND tell the canvas to follow
{
    setActiveLane (side);
    if (onLaneChanged) onLaneChanged (side);
}

void BandEditStrip::copyStToMs()   // seed Mid + Side from the ST lane so a first split makes two equals
{
    if (curBand < 0) return;
    auto copyField = [this] (const char* field)
    {
        auto* st = proc.apvts.getParameter (tabby::laneParamId (curBand, LaneSt, field));
        auto* m  = proc.apvts.getParameter (tabby::laneParamId (curBand, LaneM,  field));
        auto* s  = proc.apvts.getParameter (tabby::laneParamId (curBand, LaneS,  field));
        if (st == nullptr) return;
        const float v = st->getValue();                       // normalised (all lanes share the same range)
        if (m != nullptr) m->setValueNotifyingHost (v);
        if (s != nullptr) s->setValueNotifyingHost (v);
    };
    copyField ("freq"); copyField ("q"); copyField ("gain"); copyField ("slope");   // type is shared, no copy
}

void BandEditStrip::toggleMs()
{
    if (curBand < 0) return;
    auto setLaneOn = [this] (int lane, bool on)
    {
        if (auto* prm = proc.apvts.getParameter (tabby::laneParamId (curBand, lane, "on")))
            prm->setValueNotifyingHost (on ? 1.0f : 0.0f);
    };

    if (! curMs)   // ST -> split: enable m+s (seed from st on the FIRST split), disable st
    {
        if (msLanesFresh()) copyStToMs();                     // afterwards m/s persist across ST<->M/S
        setLaneOn (LaneM, true); setLaneOn (LaneS, true); setLaneOn (LaneSt, false);
        proc.apvts.state.setProperty (tabby::bandId (curBand, "linkFq"),                     // seed per-band link from the View defaults
                                      (bool) proc.apvts.state.getProperty ("defaultLinkFq", false), nullptr);
        proc.apvts.state.setProperty (tabby::bandId (curBand, "linkQ"),
                                      (bool) proc.apvts.state.getProperty ("defaultLinkQ", false), nullptr);
    }
    else           // split -> ST: enable st, disable m+s (their params persist for the next split)
    {
        setLaneOn (LaneSt, true); setLaneOn (LaneM, false); setLaneOn (LaneS, false);
        editingSide = false;
    }
    setBand (curBand);                                        // refresh mode / tabs / lane / layout
}

bool BandEditStrip::msLanesFresh() const   // Mid + Side lanes untouched (factory defaults) -> safe to seed from ST
{
    if (curBand < 0) return false;
    auto raw = [this] (int lane, const char* f) { return proc.apvts.getRawParameterValue (tabby::laneParamId (curBand, lane, f))->load(); };
    auto fresh = [&] (int lane)
    {
        return std::abs (raw (lane, "freq") - 1000.0f) < 0.5f && std::abs (raw (lane, "gain")) < 0.01f
            && std::abs (raw (lane, "q") - 1.0f) < 0.001f && (int) raw (lane, "slope") == 1 && raw (lane, "byp") < 0.5f;
    };
    return fresh (LaneM) && fresh (LaneS);   // ignore `on` (that's what we're toggling)
}

void BandEditStrip::rebind()
{
    // Drop the old bindings first (an attachment must outlive nothing it points at).
    slopeAtt.reset(); freqAtt.reset(); qAtt.reset(); gainAtt.reset(); bypassAtt.reset();
    if (curBand < 0) { onButton.setToggleState (false, juce::dontSendNotification); return; }

    slopeAtt = std::make_unique<ComboAtt>  (proc.apvts, laneId ("slope"), slopeBox);   // ST / Mid / Side lane
    freqAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("freq"),  freq);
    qAtt     = std::make_unique<SliderAtt> (proc.apvts, laneId ("q"),     q);
    gainAtt  = std::make_unique<SliderAtt> (proc.apvts, laneId ("gain"),  gain);

    // Power button mirrors the POINT bypass param — single source of truth (the whole point, not one lane).
    if (auto* bp = proc.apvts.getParameter (tabby::bandId (curBand, "bypass")))
    {
        bypassAtt = std::make_unique<juce::ParameterAttachment> (*bp,
            [this] (float v) { onButton.setToggleState (v < 0.5f, juce::dontSendNotification); onButton.repaint(); });
        bypassAtt->sendInitialUpdate();
    }
}

void BandEditStrip::showTypeMenu()
{
    if (curBand < 0) return;
    const int cur = bandTypeIndex();

    juce::PopupMenu m;
    for (int i = 0; i < 9; ++i)
    {
        juce::PopupMenu::Item it;
        it.itemID = i + 1;
        it.text   = kTypeNames[i];
        it.setImage (tabby::shapes::icon (tabby::filterTypeFromChoice (i), tabby::palette::violetLo()));
        it.setTicked (i == cur);
        m.addItem (it);
    }

    juce::Component::SafePointer<BandEditStrip> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (typeButton),
                     [safe] (int r)
                     {
                         if (safe == nullptr || r <= 0 || safe->curBand < 0) return;
                         if (auto* prm = safe->proc.apvts.getParameter (tabby::bandId (safe->curBand, "type")))   // SHARED point type
                             prm->setValueNotifyingHost (prm->convertTo0to1 ((float) (r - 1)));
                         safe->typeButton.setType (tabby::filterTypeFromChoice (r - 1));
                         safe->updateForType();
                     });
}

void BandEditStrip::updateForType()
{
    if (curBand < 0) { slopeBox.setVisible (false); return; }

    const auto t       = tabby::filterTypeFromChoice (bandTypeIndex());
    const bool isCut   = (t == teq::FilterType::HighPass || t == teq::FilterType::LowPass);
    const bool isShelf = (t == teq::FilterType::LowShelf || t == teq::FilterType::HighShelf);
    const bool isTilt  = (t == teq::FilterType::Tilt);
    const bool hasGain = (t == teq::FilterType::Bell || isShelf || isTilt);
    // The Notch rides a variable ORDER (slope->order like HP/LP), so it takes the slope combo in octaves too.
    const bool usesSlope = isCut || (t == teq::FilterType::Notch);

    slopeBox.setVisible (usesSlope);                   // HP/LP + Notch -> the slope combo (octaves) replaces Q
    gain.setVisible (hasGain);   gain.setEnabled (hasGain);
    q.setVisible (! usesSlope && ! isTilt);   q.setEnabled (! usesSlope && ! isTilt);   // no Q for HP/LP/Notch or tilt
    resized();                                         // visibility changed -> re-lay the bottom row
}

void BandEditStrip::paint (juce::Graphics& g)
{
    auto rr = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (tabby::palette::panel());
    g.fillRoundedRectangle (rr, 8.0f);
    g.setColour (tabby::palette::violetLo().withAlpha (0.35f));        // floating-panel rim
    g.drawRoundedRectangle (rr, 8.0f, 1.0f);
}

void BandEditStrip::resized()
{
    auto r = getLocalBounds().reduced (8, 6);

    // top row: power · < index > · type-icon · solo · ST · [M][S].  The < index > nav is ALWAYS present; the
    // M/S lane tabs (when split) sit to the RIGHT of ST. The strip has a fixed width that always reserves the
    // [M][S] slot, so ST never resizes it.
    auto top = r.removeFromTop (22);
    r.removeFromTop (8);
    onButton.setBounds (top.removeFromLeft (22).withSizeKeepingCentre (22, 22));      // power (point enable/bypass)
    top.removeFromLeft (6);
    prevButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));    // < index > nav — always
    title.setBounds (top.removeFromLeft (18));
    nextButton.setBounds (top.removeFromLeft (12).withSizeKeepingCentre (10, 14));
    top.removeFromLeft (8);
    typeButton.setBounds (top.removeFromLeft (30).withSizeKeepingCentre (30, 22));    // icon only (shared type)
    top.removeFromLeft (8);
    soloButton.setBounds (top.removeFromLeft (24).withSizeKeepingCentre (24, 22));
    top.removeFromLeft (6);
    modeButton.setBounds (top.removeFromLeft (34).withSizeKeepingCentre (34, 22));    // ST <-> M/S
    if (curMs)   // Mid | Side lane tabs — to the RIGHT of ST
    {
        top.removeFromLeft (6);
        midTab.setBounds  (top.removeFromLeft (21).withSizeKeepingCentre (21, 20));
        top.removeFromLeft (2);
        sideTab.setBounds (top.removeFromLeft (21).withSizeKeepingCentre (21, 20));
    }

    // bottom row (one line), adapts to the type:
    //   HP/LP/Notch -> FREQ + SLOPE combo (no Q) · bell/shelf -> FREQ + Q + GAIN
    //   band-pass/all-pass -> FREQ + Q · tilt -> FREQ + GAIN
    auto row = r.withSizeKeepingCentre (r.getWidth(), 22);
    if (slopeBox.isVisible())
    {
        slopeBox.setBounds (row.removeFromRight (104).withSizeKeepingCentre (104, 22));
        row.removeFromRight (8);
        freq.setBounds (row);                                          // FREQ fills the rest
    }
    else
    {
        if (gain.isVisible())
        {
            gain.setBounds (row.removeFromRight (68).withSizeKeepingCentre (68, 22));
            row.removeFromRight (8);
        }
        if (q.isVisible())
        {
            freq.setBounds (row.removeFromLeft (84).withSizeKeepingCentre (84, 22));
            row.removeFromLeft (8);
            q.setBounds (row);                                         // Q fills the middle
        }
        else
            freq.setBounds (row);
    }
}
