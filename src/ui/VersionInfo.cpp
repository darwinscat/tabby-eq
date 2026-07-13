// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/VersionInfo.h"
#include "ui/Palette.h"

#include "TabbyVersion.h"   // GENERATED at build time (build dir) — the only include of it (see VersionInfo.h)

namespace tabby
{
    // The running build's git-describe stamp, behind a function so the per-build generated header
    // stays included by this TU only (callers link against this symbol — no per-build recompile).
    const char* currentDescribe() { return version::kDescribe; }

    //==========================================================================
    // The build-info block, assembled once from the baked constants:
    //   TabbyEQ v0.1.0-3-g1234abc
    //   build 20260702191423
    //   g1234abc (dirty) · oleh
    //   core v0.2.0
    static juce::String buildInfoText()
    {
        juce::String hashLine = juce::String ("g") + version::kGitHash;
        if (version::kGitDirty) hashLine << " (dirty)";
        hashLine << " " << juce::String::fromUTF8 ("\xc2\xb7") << " " << version::kBuilder;   // middot separator

        juce::StringArray lines;
        lines.add (juce::String ("TabbyEQ ") + version::kDescribe);
        lines.add (juce::String ("build ")   + juce::String (version::kBuildNumber));
        lines.add (hashLine);
        lines.add (juce::String ("core ")    + version::kCoreVersion);
        return lines.joinIntoString ("\n");
    }

    //==========================================================================
    // The popover body: the panel-styled, monospaced version block (click it to copy) + an opt-in
    // "Check for updates" row below. Only the button click hits the network; the result lands on the
    // message thread. If an update is already known (a prior check, persisted), it shows up front.
    class InfoContent final : public juce::Component,
                              private juce::Timer
    {
    public:
        InfoContent (UpdateChecker& uc, juce::Component::SafePointer<InfoButton> ownerButton)
            : checker (uc), owner (ownerButton), text (buildInfoText())
        {
            check.setButtonText ("Check for updates");
            check.onClick = [this] { runCheck(); };
            addAndMakeVisible (check);

            result.setFont (juce::FontOptions (11.5f));
            result.setJustificationType (juce::Justification::centredLeft);
            result.setColour (juce::Label::textColourId, tabby::palette::textDim());
            addAndMakeVisible (result);

            releaseLink.setColour (juce::HyperlinkButton::textColourId, tabby::palette::orange());
            releaseLink.setJustificationType (juce::Justification::centredLeft);
            addChildComponent (releaseLink);   // hidden until an update is actually available

            // If an update is already known from a previous check, surface it immediately.
            if (checker.updateAvailable())
                showUpdate (checker.storedLatest());

            setSize (kWidth, kHeight);
            setWantsKeyboardFocus (false);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (tabby::palette::panel());
            g.fillRoundedRectangle (r, 6.0f);
            g.setColour (tabby::palette::violet().withAlpha (0.35f));
            g.drawRoundedRectangle (r, 6.0f, 1.0f);

            const juce::Font mono (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
            g.setFont (mono);

            auto block = blockBounds();
            juce::StringArray lines;
            lines.addLines (text);
            auto row = block;
            for (int i = 0; i < lines.size(); ++i)
            {
                // First line (name + describe) reads as the title colour; the rest are dimmed meta.
                g.setColour (i == 0 ? tabby::palette::text() : tabby::palette::textDim());
                g.drawText (lines[i], row.removeFromTop (kLineH), juce::Justification::centredLeft, true);
            }

            // Faint divider between the (copyable) stamp block and the update row.
            const int dy = block.getBottom() + 6;
            g.setColour (tabby::palette::textDim().withAlpha (0.18f));
            g.drawHorizontalLine (dy, (float) kPad, (float) (getWidth() - kPad));

            // Footer copy hint (right-aligned under the update row).
            g.setFont (mono.withHeight (10.5f));
            g.setColour (copied ? tabby::palette::orange() : tabby::palette::textDim().withAlpha (0.7f));
            g.drawText (copied ? "copied \xe2\x9c\x93" : "click the block to copy",
                        getLocalBounds().reduced (kPad, kPad).removeFromBottom (14),
                        juce::Justification::centredRight, false);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (kPad, kPad);
            r.removeFromTop (kNumLines * kLineH + 6 + 1 + 6);   // stamp block + gap + divider + gap
            check.setBounds (r.removeFromTop (24));
            r.removeFromTop (4);
            result.setBounds     (r.removeFromTop (18));
            releaseLink.setBounds (result.getBounds());          // the link replaces the status line when outdated
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Copy only when the click lands on the stamp block (the buttons/link handle their own).
            if (! blockBounds().contains (e.getPosition())) return;
            juce::SystemClipboard::copyTextToClipboard (text);
            copied = true;
            repaint();
            startTimer (1100);   // brief "copied" flash, then revert
        }

        static constexpr int kWidth = 300, kPad = 12, kLineH = 16, kNumLines = 4;
        static constexpr int kHeight = kPad * 2 + kNumLines * kLineH + 6 + 1 + 6 + 24 + 4 + 18 + 14;

    private:
        // The copyable stamp region (top block of monospaced lines), in local coords.
        juce::Rectangle<int> blockBounds() const
        {
            return getLocalBounds().reduced (kPad, kPad).removeFromTop (kNumLines * kLineH);
        }

        void runCheck()
        {
            check.setEnabled (false);
            releaseLink.setVisible (false);
            result.setColour (juce::Label::textColourId, tabby::palette::textDim());
            result.setText (juce::String::fromUTF8 ("Checking\xe2\x80\xa6"), juce::dontSendNotification);

            juce::Component::SafePointer<InfoContent> safe (this);
            checker.checkNow ([safe] (UpdateChecker::Result res)
            {
                if (auto* self = safe.getComponent())
                    self->onResult (res);
            });
        }

        void onResult (const UpdateChecker::Result& res)
        {
            check.setEnabled (true);
            if (auto* b = owner.getComponent()) b->repaint();   // the badge dot may have appeared/cleared

            if (! res.ok)
            {
                result.setColour (juce::Label::textColourId, tabby::palette::textDim());
                result.setText (juce::String::fromUTF8 ("Couldn\xe2\x80\x99t check (offline?)"), juce::dontSendNotification);
                return;
            }
            if (res.outdated)
                showUpdate (res.latest, res.url);
            else
            {
                releaseLink.setVisible (false);
                result.setColour (juce::Label::textColourId, juce::Colour (0xff7be29a));   // green
                result.setText (juce::String::fromUTF8 ("\xe2\x9c\x93 Up to date"), juce::dontSendNotification);
            }
        }

        // Show the actionable "vX.Y.Z available →" link (opens the release page). `url` may be empty
        // (a badge restored from a previous session) → fall back to the canonical releases page.
        void showUpdate (const juce::String& latest, const juce::String& url = {})
        {
            result.setText ({}, juce::dontSendNotification);
            releaseLink.setButtonText (juce::String::fromUTF8 ("v") + latest
                                       + juce::String::fromUTF8 (" available \xe2\x86\x92"));   // "→"
            releaseLink.setURL (juce::URL (url.isNotEmpty() ? url : UpdateChecker::releasesPageUrl()));
            releaseLink.setVisible (true);
        }

        void timerCallback() override { copied = false; repaint(); stopTimer(); }

        UpdateChecker& checker;
        juce::Component::SafePointer<InfoButton> owner;   // to repaint the badge dot after a check
        juce::String text;
        juce::TextButton      check;
        juce::Label           result;
        juce::HyperlinkButton releaseLink;
        bool copied = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoContent)
    };

    //==========================================================================
    InfoButton::InfoButton (UpdateChecker& checker) : juce::Button ("info"), updateChecker (checker)
    {
        setTooltip ("Build / version info \xe2\x80\x94 click to check for updates");
        onClick = [this] { showInfoCallout(); };
    }

    void InfoButton::paintButton (juce::Graphics& g, bool over, bool down)
    {
        // FLAT, like the rest of the top-bar chrome (gear / fullscreen / A-D): no tile, no frame —
        // just the drawn lower-case "i" (dot + rounded stem), dim at rest, lifting on hover.
        auto b = getLocalBounds().toFloat().reduced (0.5f);

        const auto ink = (over || down) ? tabby::palette::violetLo() : tabby::palette::textDim();
        g.setColour (ink);

        const float cx  = b.getCentreX();
        const float top = b.getY() + b.getHeight() * 0.28f;
        const float dot = 2.0f;
        g.fillEllipse (cx - dot * 0.5f, top - dot, dot, dot);                       // the "i" dot
        const float stemT = top + dot * 1.4f;
        const float stemB = b.getBottom() - b.getHeight() * 0.26f;
        g.fillRoundedRectangle (cx - 1.0f, stemT, 2.0f, stemB - stemT, 1.0f);        // the "i" stem

        // Update-available badge: a warm orange dot at the top-right corner (palette-consistent),
        // persisted across sessions by the UpdateChecker until the running build catches up.
        if (updateChecker.updateAvailable())
        {
            const float rr = 3.0f;
            const float ox = b.getRight() - rr - 0.5f;
            const float oy = b.getY()     + rr + 0.5f;
            g.setColour (tabby::palette::orange());
            g.fillEllipse (ox - rr, oy - rr, rr * 2.0f, rr * 2.0f);
        }
    }

    void InfoButton::showInfoCallout()
    {
        auto content = std::make_unique<InfoContent> (updateChecker, this);

        // Parent the call-out to the editor, NOT the desktop: a desktop call-out outlives the editor,
        // so closing the plugin window with it open (or an async check landing after) would orphan it.
        // As a child of the top-level editor it dies with the window; `areaToPointTo` is in parent coords.
        if (auto* top = getTopLevelComponent())
            juce::CallOutBox::launchAsynchronously (std::move (content),
                                                    top->getLocalArea (this, getLocalBounds()), top);
        else
            juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
    }
}
