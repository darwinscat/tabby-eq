// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <teq/EqEngine.h>
#include <felitronics/lineareq/LinearPhaseEq.h>

#include <array>
#include <atomic>

//==============================================================================
// LinearPhaseEq — TabbyEQ's host-side wrapper around the JUCE-free felitronics::lineareq::LinearPhaseEq.
// The CORE owns the DSP (composite-magnitude → zero-phase FIR → click-free partitioned M/S convolution).
// This wrapper owns the THREADING the core deliberately delegates to the host: a background thread rebuilds
// the FIR off the audio thread whenever the params change, and — the fix for the old node-drag stutter —
// COALESCES while a swap is still crossfading (core.isBusy()), so it never thrashes the convolver. The
// audio thread only calls core.process() (RT-safe). prepare()/updateSnapshot()/setQuality() are msg-thread.
class LinearPhaseEq : private juce::Thread
{
public:
    LinearPhaseEq() : juce::Thread ("tabby-lp-fir") {}
    ~LinearPhaseEq() override { stopThread (2000); }

    static constexpr int kNumQuality = felitronics::lineareq::LinearPhaseEq::kNumQuality;
    static int firSizeForQuality (int q) noexcept { return felitronics::lineareq::LinearPhaseEq::firSizeForQuality (q); }

    void prepare (double sampleRate, int maxBlock, int numChannels, int quality,
                  const teq::BandParams* bands, int numBands)
    {
        stopThread (2000);
        sr_ = sampleRate; maxBlock_ = maxBlock; nc_ = numChannels; quality_ = quality;
        core_.prepare (sampleRate, maxBlock, numChannels, quality);
        copySnapshot (bands, numBands);
        core_.setBands (snap_.data(), snapN_);                       // synchronous initial build → ready immediately
        startThread (juce::Thread::Priority::background);
    }

    void releaseResources() { stopThread (2000); core_.reset(); }
    void reset() noexcept { core_.reset(); }

    int latencySamples() const noexcept { return core_.latencySamples(); }

    // Message thread: push the latest params; the builder thread rebuilds only if they actually changed.
    void updateSnapshot (const teq::BandParams* bands, int numBands)
    {
        const juce::ScopedLock sl (snapLock_);
        bool changed = (numBands != snapN_);
        for (int i = 0; i < numBands && ! changed; ++i) changed = ! (bands[(size_t) i] == snap_[(size_t) i]);
        if (! changed) return;
        copySnapshotLocked (bands, numBands);
        dirty_.store (true, std::memory_order_release);
        notify();                                                    // wake the builder
    }

    // Message thread: change the FIR length. The core has no setQuality(), so re-prepare it at the new size.
    void setQuality (int quality)
    {
        if (quality == quality_) return;
        stopThread (2000);
        quality_ = quality;
        core_.prepare (sr_, maxBlock_, nc_, quality);
        { const juce::ScopedLock sl (snapLock_); core_.setBands (snap_.data(), snapN_); }
        startThread (juce::Thread::Priority::background);
    }

    // Audio thread, in place. Stereo → M/S; mono → the Mid IR (both handled inside the core).
    void process (juce::AudioBuffer<float>& buffer, int channels) noexcept
    {
        core_.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            if (threadShouldExit()) break;
            while (dirty_.exchange (false, std::memory_order_acquire))   // coalesce: always build the LATEST
            {
                while (core_.isBusy()) { if (threadShouldExit()) return; wait (10); }   // wait out the crossfade first
                std::array<teq::BandParams, teq::EqEngine::kMaxBands> local;
                int ln;
                { const juce::ScopedLock sl (snapLock_); ln = snapN_; for (int i = 0; i < ln; ++i) local[(size_t) i] = snap_[(size_t) i]; }
                core_.setBands (local.data(), ln);                   // not busy → build + click-free swap
            }
        }
    }

    void copySnapshot (const teq::BandParams* bands, int numBands)
    {
        const juce::ScopedLock sl (snapLock_);
        copySnapshotLocked (bands, numBands);
    }
    void copySnapshotLocked (const teq::BandParams* bands, int numBands)
    {
        snapN_ = juce::jmin (numBands, (int) snap_.size());
        for (int i = 0; i < snapN_; ++i) snap_[(size_t) i] = bands[(size_t) i];
    }

    felitronics::lineareq::LinearPhaseEq core_;

    double sr_ = 44100.0;
    int    maxBlock_ = 512, nc_ = 2, quality_ = 0;

    juce::CriticalSection snapLock_;
    std::array<teq::BandParams, teq::EqEngine::kMaxBands> snap_ {};
    int snapN_ = 0;
    std::atomic<bool> dirty_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinearPhaseEq)
};
