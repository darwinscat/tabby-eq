// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/VersionInfo.h"
#include "ui/Palette.h"

#include <felitronics/appkit/Brand.h>   // brand::feedTheCatLink / drawPaw — the family tip jar

#include "ui/BrandMark.h"                // tabby::brand::drawMark — the badge popover mirrors the window header
#include "BinaryData.h"                  // logodarwinscat_svg — the family mark beside the byline

#include "TabbyVersion.h"   // GENERATED at build time (build dir) — the only include of it (see VersionInfo.h)

namespace tabby
{
    // The running build's git-describe stamp, behind a function so the per-build generated header
    // stays included by this TU only (callers link against this symbol — no per-build recompile).
    const char* currentDescribe() { return version::kDescribe; }

    //==========================================================================
    juce::String pluginFormatName (juce::AudioProcessor::WrapperType w)
    {
        using AP = juce::AudioProcessor;
        if (w == AP::wrapperType_VST3)        return "VST3";
        if (w == AP::wrapperType_AudioUnit)   return "AU";
        if (w == AP::wrapperType_AudioUnitv3) return "AUv3";
        if (w == AP::wrapperType_Standalone)  return "Standalone";
        if (w == AP::wrapperType_Undefined)   return "CLAP";   // the only unwrapped format we ship
        return AP::getWrapperTypeDescription (w);
    }

    //==========================================================================
    // The product half of the shared badge. The checker already carries the slug and the running
    // version (the badge derives its GitHub links from them), so this fills in only what it cannot
    // know: identity, the baked build stamp, the dependency line, the mark and the palette.
    felitronics::appkit::VersionBadge::Config makeVersionBadgeConfig (const juce::String& format)
    {
        felitronics::appkit::VersionBadge::Config cfg;
        cfg.productName = "TabbyEQ";
        cfg.byline      = "by Darwin's Cat";
        cfg.productUrl  = "https://darwinscat.com/tabbyeq";

        cfg.gitHash     = version::kGitHash;
        cfg.buildNumber = version::kBuildNumber;
        cfg.gitDirty    = false;   // kDescribe already ends in "-dirty" when it is — no second badge
        cfg.builder     = version::kBuilder;
        // JUCE still says "Mac OSX 26.5.1" — a name Apple retired in 2012; the row shows today's.
        cfg.os          = juce::SystemStats::getOperatingSystemName().replace ("Mac OSX", "macOS");
        cfg.arch        =
           #if JUCE_ARM
            "arm64";
           #else
            "x86_64";
           #endif

        // AGPL is not fine print here: the plugin IS the licence's subject, and a reader looking for
        // it looks at the version row.
        cfg.licence = "AGPL-3.0+";   // the shorthand for -or-later: the same grant, four chars less

        // What this build actually rides, as a list with its versions in one column. Each version
        // links to that repo's release tag; JUCE's tags are bare numbers, hence no leading v.
        cfg.dependencies = {
            { "felitronics-core",   version::kCoreVersion,   "darwinscat/felitronics-core",   version::kCoreHash,   {} },
            { "felitronics-appkit", version::kAppkitVersion, "darwinscat/felitronics-appkit", version::kAppkitHash, {} },
            { "JUCE",               version::kJuceVersion,   "juce-framework/JUCE",           version::kJuceHash,   {} } };

        // The popover's title mark = the window header's mark, so the two read as one product.
        cfg.drawMark = [] (juce::Graphics& g, float cx, float cy, float d)
        {
            tabby::brand::drawMark (g, juce::Rectangle<float> (cx - d * 0.5f, cy - d * 0.5f, d, d));
        };

        // ...and the byline carries the CAT from the window header, so the popover repeats the
        // header's pair: the product's mark over the family's. The drawable is module-lifetime on
        // purpose — the popup is parented to the top-level window and can outlive the editor that
        // owns the header's own copy.
        cfg.drawByline = [] (juce::Graphics& g, float cx, float cy, float d)
        {
            static const std::unique_ptr<juce::Drawable> cat =
                juce::Drawable::createFromImageData (BinaryData::logodarwinscat_svg,
                                                     (size_t) BinaryData::logodarwinscat_svgSize);
            if (cat != nullptr)
                cat->drawWithin (g, juce::Rectangle<float> (cx - d * 0.5f, cy - d * 0.5f, d, d),
                                 juce::RectanglePlacement::centred, 0.92f);
        };

        // TabbyEQ's palette, not the badge's inherited OrbitCab pixels.
        cfg.accent      = tabby::palette::violet();
        cfg.accentHover = tabby::palette::violetLo();
        cfg.accentB     = tabby::palette::orange();
        cfg.text        = tabby::palette::text();
        cfg.ground      = tabby::palette::panel();   // the About window's own ground

        // The tip jar. The badge signs the URL itself (?from=tabbyeq); the platform rides in the base
        // so both affordances — this popover and the toolbar paw — report the same thing.
        // The URL itself is appkit's default hop, which brand::feedTheCatLink signs on its own with
        // from=<product>, platform=<os> and format=<wrapper> — we only choose the words.
        cfg.feedPrompt = format == "Standalone" ? "Like the app?" : "Like the plugin?";
        cfg.feedLabel  = "Feed the Cat";
        return cfg;
    }

    //==========================================================================
    InfoButton::InfoButton (UpdateChecker& checker, felitronics::appkit::VersionBadge& versionBadge)
        : juce::Button ("info"), updateChecker (checker), badge (versionBadge)
    {
        setTooltip ("Build / version info \xe2\x80\x94 click to check for updates");
        // The window is the FAMILY one (appkit VersionBadge's), and it is presented as an ABOUT dialog:
        // centred on the editor, on a dimmed ground, with a close cross — it long outgrew what a
        // call-out under a toolbar button can carry. This button is just its door, so the badge stays
        // invisible and only holds the config + the checker wiring.
        onClick = [this] { badge.showAbout(); };
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

}
