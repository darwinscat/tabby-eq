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
            { "felitronics-eqview", version::kEqviewVersion, "darwinscat/felitronics-eqview", version::kEqviewHash, {} },
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

}
