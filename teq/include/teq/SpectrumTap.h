// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <algorithm>   // std::copy
#include <atomic>

//==============================================================================
// teq::SpectrumTap — one mono capture window with an atomic ready handshake (SPSC).
// The audio thread pushes samples; once a window fills, it snapshots into `data`
// (unless the reader hasn't consumed the previous one yet) and flags ready. The GUI
// analyser pulls the latest frame and runs the display FFT itself. Used for the
// pre/post spectrum AND the search-mode zoom view.
//
// RT-safe: only fixed-size copies, no alloc/lock. JUCE-free so the module stays portable.
//==============================================================================
namespace teq
{

constexpr int kSpectrumFftOrder = 11;                  // 2048-point
constexpr int kSpectrumFftSize  = 1 << kSpectrumFftOrder;

struct SpectrumTap
{
    float fifo[kSpectrumFftSize] {};
    float data[kSpectrumFftSize] {};
    int   idx = 0;
    std::atomic<bool> ready { false };

    void reset() noexcept { idx = 0; ready.store (false, std::memory_order_release); }

    // GUI thread: if a frame is ready, copy it into dst[kSpectrumFftSize] and re-arm; else false.
    // Preferred read path — avoids handing out the internal buffer.
    bool tryPull (float* dst) noexcept
    {
        if (! ready.load (std::memory_order_acquire)) return false;
        std::copy (data, data + kSpectrumFftSize, dst);
        ready.store (false, std::memory_order_release);
        return true;
    }

    void push (float s) noexcept
    {
        if (idx == kSpectrumFftSize)
        {
            if (! ready.load (std::memory_order_acquire))
            {
                std::copy (fifo, fifo + kSpectrumFftSize, data);
                ready.store (true, std::memory_order_release);
            }
            idx = 0;
        }
        fifo[idx++] = s;
    }
};

} // namespace teq
