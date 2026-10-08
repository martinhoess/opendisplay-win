#pragma once

// Per-transport encode presets for the Windows sender.
//
// Dynamic mode adapts peak/mean under backpressure. Ceiling always comes from
// the Speed row of that transport (full pipe). QualityVsSpeed and GOP come from
// the chosen preset and stay fixed for the session (never drift mid-stream):
//   Dynamic+Speed    -> QVs=0,   GOP ~1s  (latency path)
//   Dynamic+Balanced -> QVs=40,  GOP ~2s
//   Dynamic+Quality  -> QVs=70,  GOP ~2s
//
// Fixed mode locks the chosen preset row for peak + QVs + GOP.
// Quality sits on Balanced-class peak/mean (not Speed-class / QVs=100).
//
// MS CODECAPI_AVEncCommonQualityVsSpeed: 0 = faster/lower quality, 100 = slower/higher.

#include "app/Config.h"

#include <algorithm>
#include <cstdint>

namespace od {

struct EncodeKnobs {
    uint32_t peakBitrateBps = 25'000'000;
    uint32_t meanBitrateBps = 15'000'000; // fraction of peak (see meanPct)
    uint32_t meanPct = 60;                // constrained VBR mean as % of peak
    uint32_t qualityVsSpeed = 0;          // 0..100
    uint32_t gopSeconds = 1;             // IDR interval in seconds
    uint32_t ceilingPeakBps = 25'000'000; // Dynamic: max peak to climb back toward
    uint32_t floorPeakBps = 8'000'000;    // Dynamic: do not step below this
    // Optional MF Quality RC path (unused by current presets; Speed/Balanced/Quality
    // all use PeakConstrainedVBR). Kept so a future A/B can flip without rewiring.
    bool useQualityRc = false;
    uint32_t rcQuality = 0; // 0..100 when useQualityRc
};

// USB Fixed:
//   Speed:    peak 50 Mbps, mean ~60%, QVs=0,   GOP ~1s
//   Balanced: peak 35 Mbps, mean ~60%, QVs=40,  GOP ~2s
//   Quality:  peak 35 Mbps, mean ~60%, QVs=70,  GOP ~2s
// Wi-Fi Fixed:
//   Speed:    peak 25 Mbps, mean ~60%, QVs=0,   GOP ~1s
//   Balanced: peak 18 Mbps, mean ~60%, QVs=40,  GOP ~2s
//   Quality:  peak 18 Mbps, mean ~60%, QVs=70,  GOP ~2s
// Dynamic: peak starts/climbs at the Speed ceiling; QVs/GOP/meanPct from preset.
inline EncodeKnobs ResolveEncodeKnobs(bool usb, EncodeMode mode, EncodePreset preset)
{
    EncodeKnobs k;

    // Transport ceilings / floors (Dynamic always uses these for peak).
    if (usb) {
        k.ceilingPeakBps = 50'000'000;
        k.floorPeakBps = 15'000'000;
    } else {
        k.ceilingPeakBps = 25'000'000;
        k.floorPeakBps = 6'000'000;
    }

    // Preset row: QVs + GOP + meanPct (+ Fixed peak). Dynamic peak overridden below.
    const EncodePreset row = preset;
    if (usb) {
        switch (row) {
            case EncodePreset::Balanced:
                k.peakBitrateBps = 35'000'000;
                k.qualityVsSpeed = 40;
                k.gopSeconds = 2;
                k.meanPct = 60;
                break;
            case EncodePreset::Quality:
                k.peakBitrateBps = 35'000'000;
                k.qualityVsSpeed = 70;
                k.gopSeconds = 2;
                k.meanPct = 60;
                break;
            case EncodePreset::Speed:
            default:
                k.peakBitrateBps = 50'000'000;
                k.qualityVsSpeed = 0;
                k.gopSeconds = 1;
                k.meanPct = 60;
                break;
        }
    } else {
        switch (row) {
            case EncodePreset::Balanced:
                k.peakBitrateBps = 18'000'000;
                k.qualityVsSpeed = 40;
                k.gopSeconds = 2;
                k.meanPct = 60;
                break;
            case EncodePreset::Quality:
                k.peakBitrateBps = 18'000'000;
                k.qualityVsSpeed = 70;
                k.gopSeconds = 2;
                k.meanPct = 60;
                break;
            case EncodePreset::Speed:
            default:
                k.peakBitrateBps = 25'000'000;
                k.qualityVsSpeed = 0;
                k.gopSeconds = 1;
                k.meanPct = 60;
                break;
        }
    }

    // Dynamic: ride the full Speed ceiling for peak; keep preset QVs/GOP/meanPct.
    if (mode == EncodeMode::Dynamic)
        k.peakBitrateBps = k.ceilingPeakBps;

    k.meanBitrateBps = k.peakBitrateBps * k.meanPct / 100;
    return k;
}

// Step peak down under backpressure (or up when healthy). Clamps to [floor, ceiling].
// Returns true if peak changed. Never touches qualityVsSpeed / gopSeconds / meanPct.
inline bool StepDynamicPeak(EncodeKnobs& k, bool congested)
{
    const uint32_t before = k.peakBitrateBps;
    // ~15% steps; keep mean as meanPct of peak.
    if (congested) {
        uint32_t next = static_cast<uint32_t>(static_cast<uint64_t>(k.peakBitrateBps) * 85 / 100);
        // At least ~1 Mbps per step so tiny peaks still move.
        if (before - next < 1'000'000 && before > k.floorPeakBps)
            next = before - 1'000'000;
        k.peakBitrateBps = std::max(next, k.floorPeakBps);
    } else {
        uint32_t next = static_cast<uint32_t>(static_cast<uint64_t>(k.peakBitrateBps) * 110 / 100);
        if (next - before < 1'000'000 && before < k.ceilingPeakBps)
            next = before + 1'000'000;
        k.peakBitrateBps = std::min(next, k.ceilingPeakBps);
    }
    k.meanBitrateBps = k.peakBitrateBps * k.meanPct / 100;
    return k.peakBitrateBps != before;
}

} // namespace od
