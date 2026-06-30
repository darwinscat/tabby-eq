// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <teq/EqEngine.h>
#include <felitronics/lineareq/NaturalPhaseEq.h>

#include <array>
#include <atomic>

//==============================================================================
// NaturalPhase — TabbyEQ's host-side wrapper around felitronics::lineareq::NaturalPhaseEq (the mixed-phase
// "Natural" rendering, phase = k·φ_min). Identical threading model to LinearPhase.h: a background thread
// rebuilds the FIR off the audio thread on param changes and COALESCES while a swap is still crossfading
// (core.isBusy()), so it never thrashes the convolver. The audio thread only calls core.process() (RT-safe).
// prepare()/updateSnapshot()/setQuality()/setBlend() are message-thread. The blend k (0 linear … 1 minimum)
// is a re-prepare-on-change parameter (it moves the reported bulk-delay latency).
class NaturalPhase : private juce::Thread
{
public:
    NaturalPhase() : juce::Thread ("tabby-np-fir") {}
    ~NaturalPhase() override { stopThread (2000); }

    static constexpr int kNumQuality = felitronics::lineareq::NaturalPhaseEq::kNumQuality;
    static int firSizeForQuality (int q) noexcept { return felitronics::lineareq::NaturalPhaseEq::firSizeForQuality (q); }

    void prepare (double sampleRate, int maxBlock, int numChannels, int quality, float k,
                  const teq::BandParams* bands, int numBands)
    {
        stopThread (2000);
        sr_ = sampleRate; maxBlock_ = maxBlock; nc_ = numChannels; quality_ = quality; k_ = k;
        core_.prepare (sampleRate, maxBlock, numChannels, quality, k);
        copySnapshot (bands, numBands);
        core_.setBands (snap_.data(), snapN_);                       // synchronous initial build → ready immediately
        startThread (juce::Thread::Priority::background);
    }

    void releaseResources() { stopThread (2000); core_.reset(); }
    void reset() noexcept { core_.reset(); }

    int latencySamples() const noexcept { return core_.latencySamples(); }   // = the bulk delay (1−k)·L/2

    // Message thread: push the latest params; the builder rebuilds only if they actually changed.
    void updateSnapshot (const teq::BandParams* bands, int numBands)
    {
        const juce::ScopedLock sl (snapLock_);
        bool changed = (numBands != snapN_);
        for (int i = 0; i < numBands && ! changed; ++i) changed = ! (bands[(size_t) i] == snap_[(size_t) i]);
        if (! changed) return;
        copySnapshotLocked (bands, numBands);
        dirty_.store (true, std::memory_order_release);
        notify();
    }

    // Message thread: change the FIR length (quality) — the core has no setQuality(), so re-prepare it.
    void setQuality (int quality)
    {
        if (quality == quality_) return;
        rebuildAt (quality, k_);
    }

    // Message thread: change the phase blend k (0 linear … 1 minimum-phase) LIVE — no re-prepare (Natural's
    // bulk-delay latency is fixed). The core's k is atomic; we just mark dirty so the builder rebuilds the
    // FIR with the new phase (a click-free convolver swap, exactly like dragging a band).
    void setBlend (float k)
    {
        if (std::abs (k - k_) < 1.0e-4f) return;
        k_ = k;
        core_.setBlend (k);
        dirty_.store (true, std::memory_order_release);
        notify();
    }

    // Audio thread, in place. Stereo → M/S; mono → the Mid IR (both handled inside the core).
    void process (juce::AudioBuffer<float>& buffer, int channels) noexcept
    {
        core_.process (buffer.getArrayOfWritePointers(), channels, buffer.getNumSamples());
    }

private:
    void rebuildAt (int quality, float k)
    {
        stopThread (2000);
        quality_ = quality; k_ = k;
        core_.prepare (sr_, maxBlock_, nc_, quality, k);
        { const juce::ScopedLock sl (snapLock_); core_.setBands (snap_.data(), snapN_); }
        startThread (juce::Thread::Priority::background);
    }

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

    felitronics::lineareq::NaturalPhaseEq core_;

    double sr_ = 44100.0;
    int    maxBlock_ = 512, nc_ = 2, quality_ = 0;
    float  k_ = 0.5f;

    juce::CriticalSection snapLock_;
    std::array<teq::BandParams, teq::EqEngine::kMaxBands> snap_ {};
    int snapN_ = 0;
    std::atomic<bool> dirty_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaturalPhase)
};
