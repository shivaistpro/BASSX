// ETA-1 — tube engine. Runs at the OVERSAMPLED rate; framework-free, RT-safe
// (no allocation after construction, no locks, no I/O).
//
// Per channel:  pre-emphasis -> [bass split] -> grid (bias, drift, grid-leak
// blocking, grid-current clamp) -> Koren load-line table (rail from PSU sag)
// -> character -> [2nd stage] -> de-emphasis -> output transformer -> tone LP
// -> soft ceiling.  The PSU (sag envelope + rail) is shared by both channels,
// as in a real amp.
#pragma once
#include "Filters.h"
#include "Profiles.h"
#include "Valves.h"

namespace eta
{
struct EngineParams
{
    int   profile    = 0;
    float drive      = 0.5f;     // 0..1
    float bias       = 0.0f;     // -1 (cold) .. 0 (balanced) .. +1 (hot)
    float sag        = 0.25f;    // 0..1
    float warmthDb   = 0.0f;     // -6..6
    float presenceDb = 0.0f;     // -6..6
    float toneHz     = 12000.0f; // 1k..20k
    float character  = 0.35f;    // 0..1
    float age        = 0.0f;     // 0..1
};

class TubeEngine
{
public:
    static constexpr int kMaxCh = 2;
    static constexpr int kBlock = 32;          // control-rate sub-block
    static constexpr float kPresenceHz = 5000.0f;
    static constexpr float kBaseHighDb = -6.0f, kBaseHighHz = 10000.0f; // spec: tame >10 kHz before the tube

    TubeEngine() { (void) valveTables(); }

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        Rng r;
        for (int c = 0; c < kMaxCh; ++c)
        {
            ageBiasUnit[c] = r.next();
            ageGainUnit[c] = r.next();
            noise[c].s = 0x1234567u + 7919u * (uint32_t) c;
        }
        cur = target;
        reset();
    }

    void setParams (const EngineParams& p) noexcept { target = p; }

    void reset() noexcept
    {
        for (auto& ch : st)
        {
            for (auto& f : ch.pre) f.reset();
            for (auto& f : ch.de)  f.reset();
            ch.split.reset(); ch.trafo.reset(); ch.tone.reset();
            ch.grid = 0.0f;
        }
        env.reset();
        rail = 1.0f;
        driftPhase = 0.0f;
        cur = target;
    }

    // Current supply rail (1 = nominal): drives the sag/GR meter.
    float getRail() const noexcept { return rail; }

    // ------------------------------------------------------------------
    // Derived control values shared by process() and transfer()
    struct Derived
    {
        const Profile* pr = nullptr;
        const ValveTable* tab = nullptr;
        float g = 1, comp = 1, bias = 0, swing = 1, invNorm = 1, vClamp = 0.2f, vCut = -2;
        float character = 0;
        float vpIdle[ValveTable::R] {};

        float idleAt (float r) const noexcept
        {
            float fr = (r - ValveTable::railMin) / ValveTable::railStep;
            fr = std::min (std::max (fr, 0.0f), (float) (ValveTable::R - 1));
            const int i = std::min ((int) fr, ValveTable::R - 2);
            return vpIdle[i] + (fr - (float) i) * (vpIdle[i + 1] - vpIdle[i]);
        }
    };

    static Derived derive (const EngineParams& p) noexcept
    {
        Derived d;
        const int idx = std::min (std::max (p.profile, 0), kNumProfiles - 1);
        d.pr  = &profiles()[(size_t) idx];
        const auto& spec = valveSpecs()[(size_t) d.pr->valve];
        d.tab = &valveTables()[(size_t) d.pr->valve];
        const auto& t = *d.tab;

        d.vCut  = t.vCut;
        d.swing = -0.5f * t.vCut;                     // u = 1 reaches cutoff / 0 V from centre
        const float b = std::min (std::max (p.bias + d.pr->biasOffset, -1.0f), 1.0f);
        d.bias   = t.vCut * (0.5f - 0.42f * b);       // hot -> towards 0 V (grid conduction)
        d.vClamp = 0.1f * d.swing + 0.05f;

        const float driveDb = -18.0f + 42.0f * p.drive - spec.headroomDb + d.pr->driveOffsetDb;
        d.g    = dbToGain (driveDb);
        d.comp = d.g < 1.0f ? 1.0f / d.g : 1.0f / std::sqrt (d.g); // unity when clean, louder when driven
        d.g   *= 1.0f - 0.3f * p.age;                 // worn cathode: lower emission, weaker stage

        const float h = 0.01f * d.swing;
        auto slope = [&] (float vb) { return (t.lookup (vb - h, 1.0f) - t.lookup (vb + h, 1.0f)) / (2.0f * h); };
        const float g0 = std::max (slope (d.bias), 0.25f * slope (0.5f * t.vCut));
        d.invNorm = 1.0f / (d.swing * std::max (g0, 1.0e-6f));
        for (int r = 0; r < ValveTable::R; ++r)
            d.vpIdle[r] = t.lookup (d.bias, ValveTable::railMin + ValveTable::railStep * (float) r);

        d.character = std::min (std::max (p.character + d.pr->charOffset, 0.0f), 1.0f);
        return d;
    }

    // Grid voltage -> normalised output (unity small-signal slope at the bias point).
    static float plate (const Derived& d, float vg, float r) noexcept
    {
        const float v = vg > 0.0f ? vg / (1.0f + vg / d.vClamp) : vg; // grid current clamps positive swing
        return (d.idleAt (r) - d.tab->lookup (v, r)) * d.invNorm;
    }

    static float characterShape (float y, float c) noexcept
    {
        // odd-order profile, unity small-signal slope, that bites well before hard clip
        // (the old sin() fold only acted at the very top and was barely audible)
        constexpr float k = 1.6f;
        const float h = std::tanh (k * y) / k;
        return y + c * (h - y);
    }

    static float stage2 (const Derived& d, float y, float r) noexcept
    {
        constexpr float k = 1.5f;
        return plate (d, d.bias + k * y * d.swing, r) / k;
    }

    // Memoryless transfer curve (no filters / sag / blocking): GUI plot + tests.
    static float transfer (const Derived& d, float x) noexcept
    {
        float y = plate (d, d.bias + d.g * x * d.swing, 1.0f);
        y = characterShape (y, d.character);
        if (d.pr->stages > 1) y = stage2 (d, y, 1.0f);
        return softCeiling (y * d.comp);
    }

    static float softCeiling (float y) noexcept { return 2.0f * std::tanh (0.5f * y); }

    // ------------------------------------------------------------------
    void process (float* const* ch, int numCh, int n) noexcept
    {
        numCh = std::min (numCh, kMaxCh);
        for (int start = 0; start < n; start += kBlock)
        {
            const int len = std::min (kBlock, n - start);
            updateControl (len);

            for (int i = start; i < start + len; ++i)
            {
                rail = std::max (ValveTable::railMin, 1.0f - 0.45f * sagDepth * std::min (env.env, 1.5f));
                float peak = 0.0f;

                for (int c = 0; c < numCh; ++c)
                {
                    auto& s = st[c];
                    float x = ch[c][i];
                    x = s.pre[2].process (s.pre[1].process (s.pre[0].process (x)));

                    float low = 0.0f;
                    if (d.pr->bassSplitHz > 0.0f) { low = s.split.process (x); x -= low; }

                    const float vg = d.bias + biasAge[c] + drift[c] + s.grid + x * d.g * gainAge[c] * d.swing;
                    if (vg > 0.0f) s.grid -= chargeK * vg;            // grid-leak cap charges -> blocking
                    s.grid = std::max (s.grid * recoverK, 0.8f * d.vCut);

                    float y = plate (d, vg, rail);
                    y = characterShape (y, d.character);
                    if (d.pr->stages > 1) y = stage2 (d, y, rail);
                    peak = std::max (peak, std::fabs (y));

                    y = y * d.comp + low + noiseAmp * noise[c].next();
                    y = s.de[2].process (s.de[1].process (s.de[0].process (y)));

                    const float lb = s.trafo.process (y);            // core flux ~ low band
                    y += kTrafoSat * (std::tanh (1.5f * lb) / 1.5f - lb);

                    y = s.tone.process (y);
                    ch[c][i] = softCeiling (y);
                }
                env.process (peak);
            }
        }
    }

private:
    static constexpr float kTrafoSat = 0.6f;

    struct Channel
    {
        FirstOrder pre[3], de[3];
        OnePoleLP split, trafo;
        SvfLP tone;
        float grid = 0.0f;
    };

    void updateControl (int len) noexcept
    {
        const float a = (float) std::exp (-(double) len / (0.02 * fs)); // ~20 ms parameter smoothing
        auto sm = [a] (float& v, float t) { v = t + a * (v - t); };
        cur.profile = target.profile;
        sm (cur.drive, target.drive);       sm (cur.bias, target.bias);
        sm (cur.sag, target.sag);           sm (cur.warmthDb, target.warmthDb);
        sm (cur.presenceDb, target.presenceDb);
        sm (cur.toneHz, target.toneHz);     sm (cur.character, target.character);
        sm (cur.age, target.age);

        d = derive (cur);
        const auto& pr = *d.pr;

        const float lowDb = pr.preLowDb + cur.warmthDb;
        float toneHz = cur.toneHz * (1.0f - 0.6f * pr.dynRollOff * std::min (env.env, 1.0f))
                                  * (1.0f - 0.35f * cur.age);                 // aged coupling caps / Miller: duller top
        toneHz = std::max (toneHz, 800.0f);

        const float ageSpread = cur.age * 0.12f;
        driftPhase += (float) (2.0 * kPi * 0.13 * len / fs);
        if (driftPhase > 6.2831853f) driftPhase -= 6.2831853f;

        for (int c = 0; c < kMaxCh; ++c)
        {
            auto& s = st[c];
            s.pre[0].lowShelf  (lowDb, pr.preLowHz, fs, false);
            s.pre[1].highShelf (cur.presenceDb, kPresenceHz, fs, false);
            s.pre[2].highShelf (kBaseHighDb, kBaseHighHz, fs, false);
            s.de[0].lowShelf   (lowDb, pr.preLowHz, fs, true);
            s.de[1].highShelf  (0.5f * cur.presenceDb, kPresenceHz, fs, true); // half undone: presence = HF drive + ~3 dB tilt
            s.de[2].highShelf  (kBaseHighDb, kBaseHighHz, fs, true);
            if (pr.bassSplitHz > 0.0f) s.split.set (pr.bassSplitHz, fs);
            s.trafo.set (90.0, fs);
            s.tone.set (toneHz, 0.7071, fs);

            biasAge[c] = ageSpread * d.swing * ageBiasUnit[c];
            gainAge[c] = 1.0f + ageSpread * ageGainUnit[c];
            drift[c]   = cur.age * 0.03f * d.swing * std::sin (driftPhase + 1.7f * (float) c);
        }
        noiseAmp = cur.age * cur.age * dbToGain (-66.0f);                // worn-valve hiss

        // PSU: SAG sets depth and RC time constants (attack 15->5 ms, release 100->450 ms)
        sagDepth = cur.sag * pr.sagDepth;
        env.set (15.0 - 10.0 * cur.sag, (100.0 + 350.0 * cur.sag) * pr.sagRelease, fs);
        chargeK  = (float) (1.0 - std::exp (-1.0 / (0.003 * fs)));
        recoverK = (float) std::exp (-1.0 / (0.08 * fs));
    }

    double fs = 48000.0;
    EngineParams target, cur;
    Derived d;
    Channel st[kMaxCh];
    EnvFollower env;
    Rng noise[kMaxCh];
    float rail = 1.0f, sagDepth = 0.0f, noiseAmp = 0.0f, chargeK = 0.0f, recoverK = 1.0f;
    float driftPhase = 0.0f;
    float ageBiasUnit[kMaxCh] {}, ageGainUnit[kMaxCh] {};
    float biasAge[kMaxCh] {}, gainAge[kMaxCh] { 1.0f, 1.0f }, drift[kMaxCh] {};
};
} // namespace eta
