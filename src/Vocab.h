// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <teq/EqTypes.h>

#include <span>
#include <string_view>

//==============================================================================
// TabbyEQ semantic vocabulary — THE source of truth, hand-authored in C++ (no codegen, no
// build deps; the compiler validates every filter type). docs/VOCABULARY.md is the human
// reference. Drafted via a DeepSeek+Codex panel; see docs/VOCABULARY-PILOT.md.
//
// A Trait is a named character/problem zone for a source. `searchQ > 0` marks a "search" trait:
// the search->treat workflow zooms to [rangeLo,rangeHi], hunts the resonance with a narrow
// boost (searchQ), then treats it (treatQ). Fixed traits use Q. Freqs are absolute Hz.
//==============================================================================
namespace tabby
{

using teq::FilterType;

enum class Action { Cut, Boost, Either };

struct Trait
{
    std::string_view key;                      // stable id, e.g. "nose"
    std::string_view label;                    // UI label (RU)
    FilterType       type    = FilterType::Bell;
    double           freq    = 1000.0;         // nominal centre Hz (sweep start for search traits)
    double           rangeLo = 0.0, rangeHi = 0.0;   // useful / sweep window [min,max] Hz
    double           Q       = 1.0;            // treat Q (bells); ignored for shelves & 24 dB HP/LP
    double           searchQ = 0.0;            // hunt Q  (> 0  =>  this is a search->treat trait)
    double           treatQ  = 0.0;            // Q after "found"
    int              slope   = 12;             // HP/LP only: 12 or 24 dB/oct
    Action           action  = Action::Cut;
    bool             core    = true;           // false => behind a "more" reveal
    std::string_view blurb   = {};             // one-line hint

    constexpr bool isSearch() const noexcept { return searchQ > 0.0; }
};

// ---- Voice — Male -----------------------------------------------------------
inline constexpr Trait kVoiceMale[] = {
    { .key="rumble",    .label="Гул/субниз",  .type=FilterType::HighPass, .freq=80,    .rangeLo=50,   .rangeHi=120,   .action=Action::Cut,   .blurb="рокот, стойка, проксимити-низ" },
    { .key="body",      .label="Тело",        .type=FilterType::LowShelf, .freq=200,   .rangeLo=150,  .rangeHi=300,   .Q=0.7, .action=Action::Either, .core=false, .blurb="вес/тепло" },
    { .key="mud",       .label="Муть",        .freq=280,   .rangeLo=200,  .rangeHi=400,   .Q=1.0, .action=Action::Cut,   .blurb="картон/бубнёж низ-середины" },
    { .key="boxy",      .label="Коробка",     .freq=500,   .rangeLo=350,  .rangeHi=700,   .Q=1.4, .action=Action::Cut,   .core=false, .blurb="резонанс комнаты/мика" },
    { .key="nose",      .label="Нос",         .freq=1000,  .rangeLo=800,  .rangeHi=1800,  .searchQ=6, .treatQ=2.5, .action=Action::Cut, .blurb="гнусавость — ищи свипом" },
    { .key="presence",  .label="Presence",    .freq=3000,  .rangeLo=2500, .rangeHi=5000,  .Q=0.8, .action=Action::Boost, .blurb="разборчивость/выезд вперёд" },
    { .key="harsh",     .label="Жёсткость",   .freq=4000,  .rangeLo=3000, .rangeHi=6000,  .searchQ=5, .treatQ=2.0, .action=Action::Cut, .blurb="режущий верх-середины" },
    { .key="sibilance", .label="Сибилянты",   .freq=7000,  .rangeLo=5500, .rangeHi=10000, .searchQ=8, .treatQ=2.5, .action=Action::Cut, .blurb="эс/ш" },
    { .key="air",       .label="Воздух",      .type=FilterType::HighShelf, .freq=12000, .rangeLo=8000, .rangeHi=16000, .Q=0.7, .action=Action::Boost, .blurb="открытость/дыхание" },
};

// ---- Voice — Female ---------------------------------------------------------
inline constexpr Trait kVoiceFemale[] = {
    { .key="rumble",    .label="Гул/субниз",  .type=FilterType::HighPass, .freq=90,    .rangeLo=60,   .rangeHi=120,   .action=Action::Cut },
    { .key="body",      .label="Тело",        .type=FilterType::LowShelf, .freq=190,   .rangeLo=120,  .rangeHi=300,   .Q=0.7, .action=Action::Either, .core=false },
    { .key="mud",       .label="Муть",        .freq=300,   .rangeLo=200,  .rangeHi=450,   .Q=1.0, .action=Action::Cut },
    { .key="boxy",      .label="Коробка",     .freq=520,   .rangeLo=400,  .rangeHi=700,   .searchQ=6, .treatQ=2.5, .action=Action::Cut, .core=false },
    { .key="nose",      .label="Нос",         .freq=1000,  .rangeLo=800,  .rangeHi=1500,  .searchQ=6, .treatQ=2.5, .action=Action::Cut },
    { .key="bite",      .label="Разборчивость", .freq=2500, .rangeLo=1800, .rangeHi=3500, .Q=0.9, .action=Action::Either, .core=false },
    { .key="presence",  .label="Presence",    .freq=4500,  .rangeLo=3500, .rangeHi=6000,  .Q=0.8, .action=Action::Boost },
    { .key="harsh",     .label="Жёсткость",   .freq=3500,  .rangeLo=2500, .rangeHi=5000,  .searchQ=5, .treatQ=2.0, .action=Action::Cut },
    { .key="sibilance", .label="Сибилянты",   .freq=7500,  .rangeLo=6000, .rangeHi=10000, .searchQ=8, .treatQ=2.5, .action=Action::Cut },
    { .key="air",       .label="Воздух",      .type=FilterType::HighShelf, .freq=12000, .rangeLo=9000, .rangeHi=16000, .Q=0.7, .action=Action::Boost },
};

// ---- Electric Guitar — Clean ------------------------------------------------
inline constexpr Trait kEgtrClean[] = {
    { .key="rumble",    .label="Гул",         .type=FilterType::HighPass, .freq=80,    .rangeLo=60,   .rangeHi=140,   .action=Action::Cut },
    { .key="body",      .label="Тело",        .type=FilterType::LowShelf, .freq=180,   .rangeLo=120,  .rangeHi=260,   .Q=0.7, .action=Action::Either, .core=false },
    { .key="mud",       .label="Муть",        .freq=300,   .rangeLo=200,  .rangeHi=450,   .Q=1.1, .action=Action::Cut },
    { .key="boxy",      .label="Коробка",     .freq=500,   .rangeLo=350,  .rangeHi=750,   .searchQ=6, .treatQ=2.0, .action=Action::Cut, .core=false },
    { .key="growl",     .label="Вуди/рык",    .freq=700,   .rangeLo=550,  .rangeHi=1000,  .Q=1.1, .action=Action::Either, .core=false },
    { .key="nose",      .label="Нос/quack",   .freq=1100,  .rangeLo=800,  .rangeHi=1600,  .searchQ=5, .treatQ=2.0, .action=Action::Cut },
    { .key="bite",      .label="Атака",       .freq=2400,  .rangeLo=1700, .rangeHi=3300,  .Q=1.0, .action=Action::Boost },
    { .key="presence",  .label="Presence",    .freq=4000,  .rangeLo=3000, .rangeHi=5500,  .Q=0.9, .action=Action::Boost },
    { .key="harsh",     .label="Глэр",        .freq=3300,  .rangeLo=2500, .rangeHi=4800,  .searchQ=5, .treatQ=2.0, .action=Action::Cut },
    { .key="icepick",   .label="Ice-pick",    .freq=6000,  .rangeLo=4500, .rangeHi=8000,  .searchQ=7, .treatQ=3.0, .action=Action::Cut },
    { .key="cabtop",    .label="Верх каба",   .type=FilterType::LowPass,  .freq=9000,  .rangeLo=7000, .rangeHi=13000, .action=Action::Cut, .core=false },
    { .key="air",       .label="Блеск/air",   .type=FilterType::HighShelf, .freq=11000, .rangeLo=8000, .rangeHi=16000, .Q=0.7, .action=Action::Boost, .core=false },
};

// ---- Electric Guitar — Dist (rhythm) ----------------------------------------
inline constexpr Trait kEgtrDist[] = {
    { .key="rumble",    .label="Гул",         .type=FilterType::HighPass, .freq=90,    .rangeLo=60,   .rangeHi=150,   .slope=24, .action=Action::Cut },
    { .key="chunk",     .label="Чанк/палм",   .freq=120,   .rangeLo=90,   .rangeHi=180,   .searchQ=3, .treatQ=1.0, .action=Action::Either, .blurb="чанк палм-мьюта" },
    { .key="mud",       .label="Муть",        .freq=300,   .rangeLo=200,  .rangeHi=450,   .Q=0.9, .action=Action::Cut },
    { .key="boxy",      .label="Коробка/каб", .freq=500,   .rangeLo=350,  .rangeHi=700,   .searchQ=6, .treatQ=1.5, .action=Action::Cut, .core=false },
    { .key="growl",     .label="Вуди",        .freq=750,   .rangeLo=600,  .rangeHi=950,   .Q=1.0, .action=Action::Either, .core=false },
    { .key="nose",      .label="Нос/honk",    .freq=1000,  .rangeLo=800,  .rangeHi=1500,  .searchQ=6, .treatQ=2.5, .action=Action::Cut },
    { .key="bite",      .label="Разборчивость", .freq=1800, .rangeLo=1400, .rangeHi=2400, .Q=1.0, .action=Action::Either, .core=false },
    { .key="presence",  .label="Атака",       .freq=3000,  .rangeLo=2000, .rangeHi=4000,  .Q=0.8, .action=Action::Boost },
    { .key="icepick",   .label="Ice-pick",    .freq=3800,  .rangeLo=3000, .rangeHi=5000,  .searchQ=8, .treatQ=2.5, .action=Action::Cut },
    { .key="fizz",      .label="Fizz",        .freq=6500,  .rangeLo=4500, .rangeHi=9000,  .searchQ=8, .treatQ=2.5, .action=Action::Cut, .blurb="жужжание дисторшна" },
    { .key="cabtop",    .label="Верх каба",   .type=FilterType::LowPass,  .freq=10000, .rangeLo=7000, .rangeHi=12000, .slope=24, .action=Action::Cut },
    { .key="air",       .label="Air",         .type=FilterType::HighShelf, .freq=9000, .rangeLo=7000, .rangeHi=11000, .Q=0.7, .action=Action::Either, .core=false },
};

// ---- Acoustic ---------------------------------------------------------------
inline constexpr Trait kAcoustic[] = {
    { .key="rumble",    .label="Гул",         .type=FilterType::HighPass, .freq=75,    .rangeLo=50,   .rangeHi=110,   .action=Action::Cut },
    { .key="body",      .label="Тело",        .type=FilterType::LowShelf, .freq=130,   .rangeLo=80,   .rangeHi=200,   .Q=0.7, .action=Action::Either, .core=false },
    { .key="boom",      .label="Бубнёж деки", .freq=130,   .rangeLo=90,   .rangeHi=200,   .searchQ=8, .treatQ=2.0, .action=Action::Cut, .blurb="резонанс розетки — ищи свипом" },
    { .key="mud",       .label="Муть",        .freq=270,   .rangeLo=200,  .rangeHi=400,   .Q=1.0, .action=Action::Cut },
    { .key="boxy",      .label="Коробка",     .freq=500,   .rangeLo=350,  .rangeHi=750,   .searchQ=6, .treatQ=2.0, .action=Action::Cut, .core=false },
    { .key="nose",      .label="Нос",         .freq=1000,  .rangeLo=800,  .rangeHi=1400,  .Q=1.2, .action=Action::Cut, .core=false },
    { .key="pick",      .label="Медиатор",    .freq=2000,  .rangeLo=1200, .rangeHi=3000,  .searchQ=6, .treatQ=2.5, .action=Action::Either, .blurb="щелчок атаки" },
    { .key="bite",      .label="Артикуляция", .freq=2800,  .rangeLo=2000, .rangeHi=4000,  .Q=0.9, .action=Action::Boost },
    { .key="harsh",     .label="Жёсткость",   .freq=4000,  .rangeLo=2800, .rangeHi=6000,  .searchQ=5, .treatQ=2.0, .action=Action::Cut },
    { .key="squeak",    .label="Скрип струн", .freq=7000,  .rangeLo=5000, .rangeHi=10000, .searchQ=8, .treatQ=3.0, .action=Action::Cut, .core=false },
    { .key="sparkle",   .label="Блеск",       .freq=9000,  .rangeLo=7000, .rangeHi=12000, .Q=0.7, .action=Action::Boost },
    { .key="air",       .label="Воздух",      .type=FilterType::HighShelf, .freq=13000, .rangeLo=9000, .rangeHi=18000, .Q=0.7, .action=Action::Boost },
};

// ---- Bass -------------------------------------------------------------------
inline constexpr Trait kBass[] = {
    { .key="rumble",    .label="Суброкот",    .type=FilterType::HighPass, .freq=30,    .rangeLo=20,   .rangeHi=45,    .action=Action::Cut },
    { .key="sub",       .label="Саб",         .type=FilterType::LowShelf, .freq=50,    .rangeLo=35,   .rangeHi=75,    .Q=0.7, .action=Action::Either },
    { .key="weight",    .label="Вес ноты",    .freq=90,    .rangeLo=60,   .rangeHi=130,   .Q=1.0, .action=Action::Boost },
    { .key="body",      .label="Тело",        .type=FilterType::LowShelf, .freq=140,   .rangeLo=90,   .rangeHi=200,   .Q=0.7, .action=Action::Either, .core=false },
    { .key="mud",       .label="Муть",        .freq=250,   .rangeLo=180,  .rangeHi=350,   .Q=1.1, .action=Action::Cut },
    { .key="boxy",      .label="Коробка/DI",  .freq=450,   .rangeLo=320,  .rangeHi=650,   .searchQ=5, .treatQ=2.0, .action=Action::Cut, .core=false },
    { .key="growl",     .label="Грайнд/рык",  .freq=900,   .rangeLo=600,  .rangeHi=1500,  .searchQ=5, .treatQ=1.5, .action=Action::Boost, .blurb="мидовая злость — ищи свипом" },
    { .key="bite",      .label="Разборчивость", .freq=2000, .rangeLo=1400, .rangeHi=3000, .Q=1.0, .action=Action::Boost, .core=false },
    { .key="clank",     .label="Клац струны", .freq=2800,  .rangeLo=2000, .rangeHi=4200,  .searchQ=5, .treatQ=2.0, .action=Action::Either },
    { .key="squeak",    .label="Шум пальцев", .freq=5000,  .rangeLo=3500, .rangeHi=7500,  .searchQ=5, .treatQ=2.5, .action=Action::Cut },
    { .key="presence",  .label="Presence",    .freq=4000,  .rangeLo=3000, .rangeHi=6000,  .Q=0.8, .action=Action::Boost, .core=false },
    { .key="air",       .label="Air/слэп",    .type=FilterType::HighShelf, .freq=10000, .rangeLo=7000, .rangeHi=15000, .Q=0.7, .action=Action::Boost, .core=false },
    { .key="cabtop",    .label="Верх каба",   .type=FilterType::LowPass,  .freq=9000,  .rangeLo=6000, .rangeHi=12000, .action=Action::Cut, .core=false },
};

struct Source
{
    std::string_view       key, label;
    std::span<const Trait>  traits;
    // roles (lead/mixed emphasis) deferred — added when the role UI lands.
};

inline constexpr Source kSources[] = {
    { "voice_male",   "Голос — М",            kVoiceMale   },
    { "voice_female", "Голос — Ж",            kVoiceFemale },
    { "egtr_clean",   "Гитара — клин",        kEgtrClean   },
    { "egtr_dist",    "Гитара — дист (ритм)", kEgtrDist    },
    { "acoustic",     "Акустика",             kAcoustic    },
    { "bass",         "Бас",                  kBass        },
};

} // namespace tabby
