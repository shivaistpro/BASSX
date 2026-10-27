// ETA-1 — building-block filters (framework-free, header-only, RT-safe).
// References: Zavalishin "The Art of VA Filter Design" (TPT one-pole / SVF),
// JOS "Introduction to Digital Filters" (bilinear transform + prewarping).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace eta
{
constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept  { return 20.0f * std::log10 (std::max (g, 1.0e-12f)); }

// First-order section from analog prototype H(s) = (B1 s + B0) / (A1 s + A0),
// bilinear-transformed with prewarping at wc. Being a pure substitution, the
// bilinear of 1/H(s) is exactly 1/H(z): a section cascaded with its inverse()
// is an identity -> pre/de-emphasis null cleanly on linear signals.
struct FirstOrder
{
    float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, z = 0.0f; // transposed DF-II

    void setAnalog (double B1, double B0, double A1, double A0, double wc, double fs) noexcept
    {
        const double K = wc / std::tan (wc / (2.0 * fs));
        const double nb0 = B1 * K + B0, nb1 = B0 - B1 * K;
        const double na0 = A1 * K + A0, na1 = A0 - A1 * K;
        b0 = (float) (nb0 / na0); b1 = (float) (nb1 / na0); a1 = (float) (na1 / na0);
    }

    // Low shelf, DC gain = gDb, HF gain = 0 dB, geometric centre at fc.
    void lowShelf (float gDb, double fc, double fs, bool inverse) noexcept
    {
        const double wc = 2.0 * kPi * fc, s = std::pow (10.0, gDb / 40.0); // sqrt(G)
        if (! inverse) setAnalog (1.0, wc * s, 1.0, wc / s, wc, fs);
        else           setAnalog (1.0, wc / s, 1.0, wc * s, wc, fs);
    }

    // High shelf, DC gain = 0 dB, HF gain = gDb.
    void highShelf (float gDb, double fc, double fs, bool inverse) noexcept
    {
        const double wc = 2.0 * kPi * fc, s = std::pow (10.0, gDb / 40.0), G = s * s;
        if (! inverse) setAnalog (G, wc * s, 1.0, wc * s, wc, fs);
        else           setAnalog (1.0, wc * s, G, wc * s, wc, fs);
    }

    float process (float x) noexcept { const float y = b0 * x + z; z = b1 * x - a1 * y; return y; }
    void reset() noexcept { z = 0.0f; }
};

// TPT one-pole low-pass (zero-delay feedback).
struct OnePoleLP
{
    float G = 0.0f, s = 0.0f;
    void set (double fc, double fs) noexcept
    {
        const double g = std::tan (kPi * std::min (fc, 0.49 * fs) / fs);
        G = (float) (g / (1.0 + g));
    }
    float process (float x) noexcept { const float v = (x - s) * G; const float y = v + s; s = y + v; return y; }
    void reset() noexcept { s = 0.0f; }
};

// TPT state-variable filter, low-pass output (stable under fast modulation).
struct SvfLP
{
    float a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
    void set (double fc, double q, double fs) noexcept
    {
        const double g = std::tan (kPi * std::min (fc, 0.45 * fs) / fs), k = 1.0 / q;
        const double A1 = 1.0 / (1.0 + g * (g + k));
        a1 = (float) A1; a2 = (float) (g * A1); a3 = (float) (g * g * A1);
    }
    float process (float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1; ic2 = 2.0f * v2 - ic2;
        return v2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

// Classic DC blocker y = x - x1 + R y1, -3 dB around fc.
struct DcBlocker
{
    float R = 0.999f, x1 = 0.0f, y1 = 0.0f;
    void set (double fc, double fs) noexcept { R = (float) std::exp (-2.0 * kPi * fc / fs); }
    float process (float x) noexcept { const float y = x - x1 + R * y1; x1 = x; y1 = y; return y; }
    void reset() noexcept { x1 = y1 = 0.0f; }
};

// Attack/release peak-ish envelope follower.
struct EnvFollower
{
    float att = 0.0f, rel = 0.0f, env = 0.0f;
    void set (double attackMs, double releaseMs, double fs) noexcept
    {
        att = (float) std::exp (-1.0 / (attackMs * 0.001 * fs));
        rel = (float) std::exp (-1.0 / (releaseMs * 0.001 * fs));
    }
    float process (float x) noexcept
    {
        const float c = x > env ? att : rel;
        env = x + c * (env - x);
        return env;
    }
    void reset() noexcept { env = 0.0f; }
};

// xorshift32 — allocation-free, deterministic noise for the AGE stage.
struct Rng
{
    uint32_t s = 0x9E3779B9u;
    float next() noexcept // uniform [-1, 1)
    {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float) ((double) s * (2.0 / 4294967296.0) - 1.0);
    }
};
} // namespace eta
