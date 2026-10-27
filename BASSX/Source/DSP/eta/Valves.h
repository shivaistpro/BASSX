// ETA-1 — valve library: Koren triode/pentode models solved on a resistive
// load line and baked into 2-D tables (grid voltage x supply rail).
//
// Koren, "Improved VT models for SPICE simulations" (Glass Audio, 1996):
//   triode : E1 = Vp/kp * ln(1 + exp(kp (1/mu + Vg / sqrt(kvb + Vp^2))))
//   pentode: E1 = Vg2/kp * ln(1 + exp(kp (1/mu + Vg / Vg2)))   (* atan(Vp/kvb))
//   Ip = E1^ex / kg1 * (1 + sgn E1)
// Constants below are published Koren fits where they exist and behavioural
// approximations otherwise; they are the starting point for the Phase-4
// calibration against hardware captures, not measured data.
#pragma once
#include <array>
#include <cmath>
#include <vector>
#include "Filters.h"

namespace eta
{
struct ValveSpec
{
    const char* name;
    bool pentode;
    double mu, ex, kg1, kp, kvb;
    double bPlus;      // nominal supply (V)
    double rLoad;      // plate load (ohm)
    double screenFrac; // pentode screen = screenFrac * rail
    float headroomDb;  // input level (dBFS) where the stage starts to clip at DRIVE 0 dB
};

// Order = spec section 4.2.
inline const std::array<ValveSpec, 20>& valveSpecs()
{
    static const std::array<ValveSpec, 20> v {{
        { "12AX7 / ECC83", false, 100.0, 1.40, 1060.0, 600.0, 300.0, 250.0, 100e3, 0.0, -12.0f },
        { "12AT7 / ECC81", false,  60.0, 1.35,  460.0, 300.0, 300.0, 250.0,  47e3, 0.0,  -6.0f },
        { "12AU7 / ECC82", false,  21.5, 1.30, 1180.0,  84.0, 300.0, 250.0,  22e3, 0.0,   0.0f },
        { "5751",          false,  70.0, 1.40, 1180.0, 450.0, 300.0, 250.0, 100e3, 0.0,  -9.0f },
        { "6DJ8 / ECC88",  false,  28.0, 1.30,  330.0, 320.0, 300.0, 150.0,  10e3, 0.0,  -3.0f },
        { "6922",          false,  33.0, 1.30,  330.0, 320.0, 300.0, 150.0,  10e3, 0.0,  -4.0f },
        { "EF86",          true,   38.0, 1.35, 2000.0, 300.0,  30.0, 250.0, 100e3, 0.40, -15.0f },
        { "6SN7",          false,  20.0, 1.30, 1300.0, 200.0, 300.0, 250.0,  47e3, 0.0,  -2.0f },
        { "6SL7",          false,  70.0, 1.40, 1600.0, 600.0, 300.0, 250.0, 100e3, 0.0, -10.0f },
        { "12BH7A",        false,  16.5, 1.30,  800.0,  70.0, 300.0, 300.0,  22e3, 0.0,   2.0f },
        { "EL84",          true,   19.0, 1.35,  600.0, 200.0,  30.0, 300.0,   5e3, 0.95, -14.0f },
        { "EL34",          true,   11.0, 1.35,  650.0,  60.0,  24.0, 450.0, 3.4e3, 0.90, -16.0f },
        { "6L6GC",         true,    8.7, 1.35, 1460.0,  48.0,  12.0, 450.0,   4e3, 0.90, -10.0f },
        { "6V6GT",         true,  12.67, 1.35, 1672.0,  41.0,  12.0, 350.0,   5e3, 0.90,  -8.0f },
        { "KT66",          true,    7.0, 1.35, 1100.0,  50.0,  20.0, 450.0,   4e3, 0.90, -11.0f },
        { "KT77",          true,   11.0, 1.35,  700.0,  60.0,  24.0, 450.0, 3.4e3, 0.90,  -7.0f },
        { "KT88",          true,    8.8, 1.35,  730.0,  32.0,  16.0, 500.0,   3e3, 0.90,   4.0f },
        { "6550",          true,    7.9, 1.35,  890.0,  60.0,  24.0, 500.0,   3e3, 0.90,   3.0f },
        { "300B",          false,  3.95, 1.40, 1550.0,  65.0, 300.0, 400.0,   3e3, 0.0,  -1.0f },
        { "2A3",           false,   4.2, 1.40, 1500.0,  60.0, 300.0, 300.0, 2.5e3, 0.0,  -2.0f },
    }};
    return v;
}

inline double softplus (double x) noexcept { return x > 30.0 ? x : std::log1p (std::exp (x)); }

// Plate current (A) for grid voltage vg, plate voltage vp, supply rail (for the screen).
inline double korenIp (const ValveSpec& t, double vg, double vp, double rail) noexcept
{
    double e1;
    if (! t.pentode)
    {
        if (vp <= 0.0) return 0.0;
        e1 = vp / t.kp * softplus (t.kp * (1.0 / t.mu + vg / std::sqrt (t.kvb + vp * vp)));
    }
    else
    {
        const double vg2 = t.screenFrac * rail;
        e1 = vg2 / t.kp * softplus (t.kp * (1.0 / t.mu + vg / vg2));
    }
    if (e1 <= 0.0) return 0.0;
    double ip = 2.0 * std::pow (e1, t.ex) / t.kg1;
    if (t.pentode) ip *= std::atan (std::max (vp, 0.0) / t.kvb) * (2.0 / kPi);
    return ip;
}

// Solve Vp = rail - R * Ip(vg, Vp) (monotonic in Vp -> bisection always converges).
inline double solvePlate (const ValveSpec& t, double vg, double rail) noexcept
{
    double lo = 0.0, hi = rail;
    for (int i = 0; i < 44; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (mid + t.rLoad * korenIp (t, vg, mid, rail) > rail) hi = mid; else lo = mid;
    }
    return 0.5 * (lo + hi);
}

// Plate voltage table over grid voltage x rail scale. Built once, read-only after.
struct ValveTable
{
    static constexpr int N = 1024;     // grid-voltage points
    static constexpr int R = 7;        // rail points: 0.4, 0.5 ... 1.0
    static constexpr float railMin = 0.4f, railStep = 0.1f;

    float vCut = -2.0f;                // grid cutoff at nominal rail (V, negative)
    float vgMin = -4.0f, vgMax = 1.0f, invDv = 1.0f;
    std::vector<float> vp;             // R * N

    void build (const ValveSpec& t)
    {
        // cutoff: Ip(vg, Vp = B+) falls to 0.1 % of Ip(0, B+)
        const double ref = korenIp (t, 0.0, t.bPlus, t.bPlus);
        double lo = -t.bPlus, hi = 0.0;
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (korenIp (t, mid, t.bPlus, t.bPlus) < 1.0e-3 * ref) lo = mid; else hi = mid;
        }
        vCut  = (float) hi;
        vgMin = 1.6f * vCut;
        vgMax = -0.6f * vCut;
        invDv = (float) (N - 1) / (vgMax - vgMin);
        vp.resize ((size_t) (R * N));
        for (int r = 0; r < R; ++r)
        {
            const double rail = t.bPlus * (railMin + railStep * (float) r);
            for (int i = 0; i < N; ++i)
            {
                const double vg = vgMin + (vgMax - vgMin) * (double) i / (N - 1);
                vp[(size_t) (r * N + i)] = (float) solvePlate (t, vg, rail);
            }
        }
    }

    // Bilinear lookup; vg clamped to the table domain (flat beyond cutoff anyway).
    float lookup (float vg, float rail) const noexcept
    {
        // clamp so the top node is reached exactly (ax/ar = 1), keeping lookup()
        // bit-identical to the idle values sampled at the rail nodes
        const float fx = std::min (std::max ((vg - vgMin) * invDv, 0.0f), (float) (N - 1));
        const float fr = std::min (std::max ((rail - railMin) / railStep, 0.0f), (float) (R - 1));
        const int ix = std::min ((int) fx, N - 2), ir = std::min ((int) fr, R - 2);
        const float ax = fx - (float) ix, ar = fr - (float) ir;
        const float* r0 = vp.data() + ir * N + ix;
        const float* r1 = r0 + N;
        const float a = r0[0] + ax * (r0[1] - r0[0]);
        const float b = r1[0] + ax * (r1[1] - r1[0]);
        return a + ar * (b - a);
    }
};

// All 20 tables, built once on first use (thread-safe static init, ~100 ms).
// Call from a non-audio thread first (processor constructor does).
inline const std::array<ValveTable, 20>& valveTables()
{
    static const std::array<ValveTable, 20> tables = []
    {
        std::array<ValveTable, 20> t;
        for (size_t i = 0; i < t.size(); ++i) t[i].build (valveSpecs()[i]);
        return t;
    }();
    return tables;
}
} // namespace eta
