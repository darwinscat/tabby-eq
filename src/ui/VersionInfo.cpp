// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/VersionInfo.h"
#include "ui/Palette.h"

#include "TabbyVersion.h"   // GENERATED at build time (build dir) — the only include of it (see VersionInfo.h)

namespace tabby
{
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
    // The popover body: paints the panel-styled, monospaced block + a subtle "click to copy" footer
    // that flashes "copied" on click. Click anywhere copies the whole block to the system clipboard.
    class InfoContent final : public juce::Component,
                              private juce::Timer
    {
    public:
        InfoContent() : text (buildInfoText())
        {
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

            auto body = getLocalBounds().reduced (kPad, kPad);
            auto footer = body.removeFromBottom (16);

            juce::StringArray lines;
            lines.addLines (text);
            for (int i = 0; i < lines.size(); ++i)
            {
                // First line (name + describe) reads as the title colour; the rest are dimmed meta.
                g.setColour (i == 0 ? tabby::palette::text() : tabby::palette::textDim());
                g.drawText (lines[i], body.removeFromTop (kLineH), juce::Justification::centredLeft, true);
            }

            g.setFont (mono.withHeight (10.5f));
            g.setColour (copied ? tabby::palette::orange() : tabby::palette::textDim().withAlpha (0.7f));
            g.drawText (copied ? "copied \xe2\x9c\x93" : "click to copy",
                        footer, juce::Justification::centredRight, false);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            juce::SystemClipboard::copyTextToClipboard (text);
            copied = true;
            repaint();
            startTimer (1100);   // brief "copied" flash, then revert
        }

        static constexpr int kWidth = 290, kHeight = 108, kPad = 12, kLineH = 16;

    private:
        void timerCallback() override { copied = false; repaint(); stopTimer(); }

        juce::String text;
        bool copied = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoContent)
    };

    //==========================================================================
    InfoButton::InfoButton() : juce::Button ("info")
    {
        setTooltip ("Build / version info");
        onClick = [this] { showInfoCallout(); };
    }

    void InfoButton::paintButton (juce::Graphics& g, bool over, bool down)
    {
        // A rounded tile with a drawn lower-case "i" (dot + rounded stem). Fill + subtle outline so it
        // reads as a button next to the top-bar's View/POST, brightening on hover like the icon tiles.
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (tabby::palette::panel().brighter (over ? 0.34f : 0.22f));
        g.fillRoundedRectangle (b, 4.0f);
        g.setColour (tabby::palette::textDim().withAlpha (over ? 0.9f : 0.5f));
        g.drawRoundedRectangle (b, 4.0f, 1.0f);

        const auto ink = (over || down) ? tabby::palette::violetLo() : tabby::palette::text();
        g.setColour (ink);

        const float cx  = b.getCentreX();
        const float top = b.getY() + b.getHeight() * 0.28f;
        const float dot = 2.0f;
        g.fillEllipse (cx - dot * 0.5f, top - dot, dot, dot);                       // the "i" dot
        const float stemT = top + dot * 1.4f;
        const float stemB = b.getBottom() - b.getHeight() * 0.26f;
        g.fillRoundedRectangle (cx - 1.0f, stemT, 2.0f, stemB - stemT, 1.0f);        // the "i" stem
    }

    void InfoButton::showInfoCallout()
    {
        auto content = std::make_unique<InfoContent>();
        // areaToPointTo is a GLOBAL screen coord when no parent is supplied — anchor the arrow to the button.
        juce::CallOutBox::launchAsynchronously (std::move (content), getScreenBounds(), nullptr);
    }
}
