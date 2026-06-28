// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <teq/EqEngine.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

//==============================================================================
// LinearPhaseEq — the linear-phase processing path. From the engine's composite MAGNITUDE response
// (per Mid/Side axis, via teq::EqEngine::magnitudeGridFor) it builds a symmetric, zero-phase FIR
// (IFFT of the zero-phase magnitude → fftshift → Blackman-Harris window) and convolves with it. The
// FIR is rebuilt on a background thread whenever the params change; the convolution itself is a
// juce::dsp::Convolution, which partitions + crossfades the impulse response on ITS OWN thread, so
// the audio thread never allocates. Group delay = N/2 samples (reported to the host for PDC).
//
// Threading: prepare()/setQuality()/updateSnapshot() are message-thread; process() is audio-thread.
// conv.prepare() runs only in prepare() (host not streaming); changing the FIR length at runtime is
// just another loadImpulseResponse() (thread-safe) — no re-prepare, so no race with process().
class LinearPhaseEq : private juce::Thread
{
public:
    LinearPhaseEq() : juce::Thread ("tabby-lp-fir") {}
    ~LinearPhaseEq() override { stopThread (2000); }

    static constexpr int kNumQuality = 4;                                  // Low / Medium / High / Max
    static int firSizeForQuality (int q) noexcept
    {
        static constexpr int sizes[kNumQuality] = { 4096, 16384, 65536, 131072 };
        return sizes[juce::jlimit (0, kNumQuality - 1, q)];
    }

    void prepare (double sampleRate, int maxBlock, int numChannels, int quality,
                  const teq::BandParams* bands, int numBands)
    {
        stopThread (2000);
        sr = sampleRate; maxB = juce::jmax (1, maxBlock); nc = juce::jmax (1, numChannels);
        copySnapshot (bands, numBands);
        setSizeN (firSizeForQuality (quality));
        conv.reset();
        conv.prepare ({ sr, (juce::uint32) maxB, (juce::uint32) nc });     // the ONLY conv.prepare (safe: not streaming)
        buildAndLoad();                                                    // synchronous initial build -> ready right away
        startThread (juce::Thread::Priority::background);
    }

    void releaseResources() { stopThread (2000); conv.reset(); }

    int latencySamples() const noexcept { return fftN / 2; }

    // Message-thread: push the latest params; rebuild only if they actually changed.
    void updateSnapshot (const teq::BandParams* bands, int numBands)
    {
        const juce::ScopedLock sl (snapLock);
        bool changed = (numBands != snapN);
        for (int i = 0; i < numBands && ! changed; ++i) changed = ! (bands[(size_t) i] == snap[(size_t) i]);
        if (! changed) return;
        copySnapshotLocked (bands, numBands);
        dirty.store (true, std::memory_order_release);
        notify();                                                          // wake the builder
    }

    // Message-thread: change the FIR length (quality). Just rebuilds + reloads — no conv.prepare.
    void setQuality (int quality)
    {
        const int newN = firSizeForQuality (quality);
        if (newN == fftN) return;
        stopThread (2000);
        setSizeN (newN);
        buildAndLoad();
        startThread (juce::Thread::Priority::background);
    }

    // Audio thread. In-place. channels==2 -> M/S convolution (mid IR on M, side IR on S); else the
    // Mid IR on every channel. RT-safe (only conv.process + arithmetic).
    void process (juce::AudioBuffer<float>& buffer, int channels) noexcept
    {
        const int n = buffer.getNumSamples();
        if (n <= 0) return;

        if (channels == 2)                                                 // encode L/R -> M/S in place
        {
            float* L = buffer.getWritePointer (0);
            float* R = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i) { const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]); L[i] = m; R[i] = s; }
        }

        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) channels, (size_t) n);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        conv.process (ctx);

        if (channels == 2)                                                 // decode M/S -> L/R
        {
            float* M = buffer.getWritePointer (0);
            float* S = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i) { const float l = M[i] + S[i], r = M[i] - S[i]; M[i] = l; S[i] = r; }
        }
    }

    void reset() noexcept { conv.reset(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (-1);
            if (threadShouldExit()) break;
            while (dirty.exchange (false, std::memory_order_acquire))      // coalesce: always build the latest
            {
                buildAndLoad();
                if (threadShouldExit()) return;
            }
        }
    }

    void copySnapshot (const teq::BandParams* bands, int numBands)
    {
        const juce::ScopedLock sl (snapLock);
        copySnapshotLocked (bands, numBands);
    }
    void copySnapshotLocked (const teq::BandParams* bands, int numBands)
    {
        snapN = juce::jmin (numBands, (int) snap.size());
        for (int i = 0; i < snapN; ++i) snap[(size_t) i] = bands[(size_t) i];
    }

    // (Re)allocate everything that depends on the FIR length N and pin the FFT-convention scale.
    void setSizeN (int N)
    {
        fftN = N;
        fft    = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 ((double) N)));
        window = std::make_unique<juce::dsp::WindowingFunction<float>> (
                     (size_t) N, juce::dsp::WindowingFunction<float>::blackmanHarris, false);   // peak 1.0 at centre
        magBuf.assign ((size_t) (N / 2 + 1), 0.0f);
        fftBuf.assign ((size_t) (2 * N), 0.0f);
        firBuf.assign ((size_t) N, 0.0f);
        computeFirScale();
    }

    // The inverse-FFT of a flat (all-ones) spectrum is a single delta; its height is exactly the
    // transform's normalisation constant. Dividing every FIR by it makes a flat EQ a unit passthrough
    // regardless of JUCE's inverse-FFT scaling convention (and correctly scales every other response).
    void computeFirScale()
    {
        const int N = fftN, half = N / 2 + 1;
        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int k = 0; k < half; ++k) fftBuf[(size_t) (2 * k)] = 1.0f;     // flat magnitude, zero phase
        fft->performRealOnlyInverseTransform (fftBuf.data());
        const float c = fftBuf[0];                                          // delta height (peak at n=0)
        firScale = (std::abs (c) > 1e-20f) ? 1.0f / c : 1.0f;
    }

    void buildAndLoad()
    {
        std::array<teq::BandParams, teq::EqEngine::kMaxBands> local;
        int ln;
        { const juce::ScopedLock sl (snapLock); ln = snapN; for (int i = 0; i < ln; ++i) local[(size_t) i] = snap[(size_t) i]; }

        const bool stereoIR = (nc == 2);
        juce::AudioBuffer<float> ir (stereoIR ? 2 : 1, fftN);
        buildFir (local.data(), ln, false, ir.getWritePointer (0));        // Mid axis
        if (stereoIR) buildFir (local.data(), ln, true, ir.getWritePointer (1));   // Side axis

        conv.loadImpulseResponse (std::move (ir), sr,
                                  stereoIR ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
                                  juce::dsp::Convolution::Trim::no,
                                  juce::dsp::Convolution::Normalise::no);
    }

    // Zero-phase magnitude -> symmetric, windowed FIR (centre tap = group delay N/2).
    void buildFir (const teq::BandParams* bands, int numBands, bool side, float* out)
    {
        const int N = fftN, half = N / 2 + 1;
        teq::EqEngine::magnitudeGridFor (bands, numBands, sr, magBuf.data(), half, side);

        std::fill (fftBuf.begin(), fftBuf.end(), 0.0f);
        for (int k = 0; k < half; ++k) fftBuf[(size_t) (2 * k)] = magBuf[(size_t) k];   // real spectrum, zero phase
        fft->performRealOnlyInverseTransform (fftBuf.data());                            // fftBuf[0..N-1] = h[n]

        for (int i = 0; i < N; ++i) firBuf[(size_t) i] = fftBuf[(size_t) ((i + N / 2) % N)] * firScale;   // fftshift + scale
        window->multiplyWithWindowingTable (firBuf.data(), (size_t) N);                  // taper -> tame pre-ring
        for (int i = 0; i < N; ++i) out[i] = firBuf[(size_t) i];
    }

    juce::dsp::Convolution conv;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;

    double sr = 44100.0;
    int    maxB = 512, nc = 2, fftN = 16384;
    float  firScale = 1.0f;

    juce::CriticalSection snapLock;
    std::array<teq::BandParams, teq::EqEngine::kMaxBands> snap {};
    int snapN = 0;
    std::atomic<bool> dirty { false };

    std::vector<float> magBuf, fftBuf, firBuf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinearPhaseEq)
};
