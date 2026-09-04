// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <felitronics/appkit/UpdateChecker.h>   // the running version + the "a newer release is known" flag
#include <felitronics/appkit/UpdateCompare.h>   // update::isCleanRelease — is this build an exact release?

#include <felitronics/appkit/chrome/ChromeMetrics.h>   // chrome::drawTracked — one tracked line, the family's way

#include "Palette.h"

//==============================================================================
// The build stamp at the far end of the bottom bar — "v0.6.0 · Standalone" — and the SECOND door to
// the About window the top bar's (i) opens. OrbitAmp's footer carries the same line for the same
// reason: what version is running, and in what wrapper, is a fact about the RUN, and the bottom bar
// is where this product keeps those (mode, latency, analyzer, correlation).
//
// It paints only text: no tile, no frame — the line lifts out of the background under the cursor,
// which is the only thing that says it can be pressed. The window itself belongs to appkit's
// VersionBadge (the editor owns it, invisible, for the (i)), so this is a door and nothing more —
// hence `onClick` rather than a badge reference.
namespace tabby::ui
{

class VersionStamp final : public juce::Component,
                           public juce::SettableTooltipClient,
                           private juce::Timer
{
public:
    // `checker` carries the running build's git-describe stamp (and the update flag) and must outlive
    // this component — the processor owns it. `format` = pluginFormatName(), spelled as the About
    // window spells it.
    VersionStamp (felitronics::appkit::UpdateChecker& c, juce::String format)
        : checker (c)
    {
        // The tag alone is what the line shows: "v0.6.0-1-g3478bea-dirty" is a stamp for the About
        // table, not for a 22 px strip. A build that is NOT an exact release keeps a "+" — the line
        // must not claim to be the release it merely descends from, and the About window (one click
        // away) carries the commit, the dirty flag and the build number in full.
        const auto describe = checker.currentVersion().trim();
        auto tag = describe.startsWithIgnoreCase ("v") ? describe.substring (1) : describe;
        tag = tag.upToFirstOccurrenceOf ("-", false, false);
        const bool clean = felitronics::appkit::update::isCleanRelease (describe.toStdString());

        versionText = "v" + (tag.isNotEmpty() ? tag : juce::String ("0.0.0")) + (clean ? "" : "+");
        fullText    = versionText + juce::String::fromUTF8 (" \xc2\xb7 ") + format;

        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        refreshTooltip();
        updateDot = checker.updateAvailable();
        startTimerHz (2);   // the flag only ever moves after a deliberate check — see timerCallback()
    }

    ~VersionStamp() override { stopTimer(); }

    std::function<void()> onClick;

    // The strip's own type. Given a face, the stamp is set in it — the bottom bar is chrome, and the
    // family's chrome wears the family's letters; given none, it stays on the system font.
    void setDisplayFont (juce::Typeface::Ptr face, float trackingEm = 0.0f)
    {
        typeface = std::move (face);
        tracking = trackingEm;
        if (auto* p = getParentComponent())
            p->resized();                     // the line's width just changed under the strip
        repaint();
    }

    // The width the full line wants, and the width it can still be read at (the version alone) —
    // the strip hands over what it has between the analyzer item and the correlation meter. The
    // alert dot rides in front of the text, so a lit dot asks for its own room.
    int preferredWidth() const { return juce::roundToInt (textWidth (fullText))    + dotRoom() + 2; }
    int minimumWidth()   const { return juce::roundToInt (textWidth (versionText)) + dotRoom() + 2; }

    void paint (juce::Graphics& g) override
    {
        // Right-aligned against its own right edge, so the line ends where the strip ends however
        // much room it was given — and so the stamp's last pixel sits on the spectrum's right edge.
        auto r = getLocalBounds();

        const bool wide = (float) (r.getWidth() - dotRoom()) >= textWidth (fullText);
        const auto text = wide ? fullText : versionText;

        if (updateDot)
        {
            // The family's "needs a look": a release seen by an earlier, deliberate check is newer
            // than what is running. It sits BEFORE the words rather than after them — the text edge
            // is flush with the plot's, and the dot must not shift it when it lights.
            auto slot = r.removeFromLeft (juce::jmax (0, r.getWidth() - juce::roundToInt (textWidth (text))));
            g.setColour (tabby::palette::orange());
            g.fillEllipse (slot.getRight() - 9.0f, (float) slot.getCentreY() - 3.0f, 6.0f, 6.0f);
        }

        g.setColour (isMouseOver() ? tabby::palette::text()
                                   : tabby::palette::text().withAlpha (0.42f));
        felitronics::appkit::chrome::drawTracked (g, text, r.toFloat(), font(), tracking,
                                                  juce::Justification::right);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (onClick != nullptr && getLocalBounds().contains (e.getPosition()))
            onClick();
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { repaint(); }

private:
    // The dot never moves on its own: it follows a check the user asked for, in this instance or in
    // an earlier session (the checker persists the tag it saw). Twice a second is a settings lookup
    // on the message thread — the same poll OrbitAmp's footer runs — and it repaints nothing until
    // the answer changes.
    void timerCallback() override
    {
        if (const bool upd = checker.updateAvailable(); upd != updateDot)
        {
            updateDot = upd;
            refreshTooltip();
            if (auto* p = getParentComponent())   // the lit dot asks for more room than the bare text
                p->resized();
            repaint();
        }
    }

    void refreshTooltip()
    {
        setTooltip (updateDot ? "A newer release is available \xe2\x80\x94 click for version and updates"
                              : "Version, build stamp and updates");
    }

    int dotRoom() const { return updateDot ? 13 : 0; }   // 6 px dot + the air around it

    juce::Font font() const
    {
        return typeface != nullptr
                 ? juce::Font (juce::FontOptions().withHeight (kTextH).withTypeface (typeface))
                 : juce::Font (juce::FontOptions (kTextH));
    }

    float textWidth (const juce::String& s) const
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (font(), s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth()
                 + kTextH * tracking * (float) juce::jmax (0, ga.getNumGlyphs() - 1);
    }

    static constexpr float kTextH = 11.0f;
    juce::Typeface::Ptr typeface;
    float tracking = 0.0f;

    felitronics::appkit::UpdateChecker& checker;
    juce::String versionText, fullText;
    bool updateDot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VersionStamp)
};

} // namespace tabby::ui
