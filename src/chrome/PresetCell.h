// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ui/ChromeButtons.h"   // FlatItem — the preset name button

#include <functional>

namespace tabby::chrome
{

//==============================================================================
// PresetModel — the product-owned preset state pushed into the cell. Contract SHAPE now; richness
// LEAN: the EQ shows only the current name here. Sections / stable IDs / save-as / delete / caps
// arrive in арка 2 — the product still builds the whole menu today (PresetActions::showList).
struct PresetModel
{
    juce::String currentName;   // the loaded preset's display name
};

// PresetActions — the cell's single gesture: the product opens the preset menu. Load AND
// Save/Import/Export all live INSIDE that menu now.
struct PresetActions
{
    std::function<void ()> showList;   // preset-name click → the product's Default + *.tabbyeq + Save/Import/Export menu
};

//==============================================================================
// PresetCell — the preset name as one chrome cell, driven by a product model + a single action
// callback. Save/Import/Export used to be an icon trio to the right of the name; they now live
// inside the preset menu (opened by clicking the name). Removing that trio also removed the 6px air
// before it — the source of the old +3px legacy centre bias (now retired; see ChromeBar).
class PresetCell final : public juce::Component
{
public:
    PresetCell()
    {
        preset.setButtonText ("Default");
        preset.onClick = [this] { if (actions.showList) actions.showList(); };
        addAndMakeVisible (preset);
    }

    void setActions (PresetActions a) { actions = std::move (a); }

    void setModel (const PresetModel& m) { model = m; preset.setButtonText (m.currentName); }
    void setCurrentName (const juce::String& n) { model.currentName = n; preset.setButtonText (n); }
    juce::String currentName() const { return model.currentName; }

    // The component a product popup (the preset menu) should anchor to — the name item.
    juce::Component* nameAnchor() { return &preset; }

    int fixedWidth() const { return kPresetW; }

    void resized() override
    {
        preset.setBounds (0, 4, getWidth(), getHeight() - 8);   // vInset 4, full cell width
    }

private:
    static constexpr int kPresetW = 104;

    tabby::ui::FlatItem preset;
    PresetActions actions;
    PresetModel   model;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetCell)
};

} // namespace tabby::chrome
