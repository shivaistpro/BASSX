// ETA-1 — model profiles: 6 groovebox valve-stage voicings + the 20-valve library.
// Groovebox voicings are inspired by the 12AX7 output stages of classic
// sampler/synth grooveboxes; no third-party trademarks are used in the product.
#pragma once
#include <array>

namespace eta
{
struct Profile
{
    const char* name;
    int   valve;          // index into valveSpecs()
    float preLowDb;       // fixed pre-emphasis low shelf (dB), undone after the tube
    float preLowHz;       // shelf centre (also where WARMTH acts)
    float driveOffsetDb;  // extra grid drive
    float biasOffset;     // added to BIAS (-1..1 units)
    float sagDepth;       // multiplies SAG depth
    float sagRelease;     // multiplies SAG release time (<1 = faster recovery)
    float bassSplitHz;    // >0: lows below this bypass the tube (tight low end)
    int   stages;         // 2 = cascaded second triode stage
    float dynRollOff;     // 0..1 HF roll-off that follows clipping depth
    float charOffset;     // added to CHARACTER (0..1 units)
};

constexpr int kNumGrooveProfiles = 6;
constexpr int kNumProfiles = 26;

inline const std::array<Profile, kNumProfiles>& profiles()
{
    static const std::array<Profile, kNumProfiles> p {{
        //  name                valve  preLo  Hz    drv   bias   sagD  sagR  split  st  dynRO  char
        { "SX TUBE",              0,   3.0f, 120.f,  0.f, 0.30f, 1.0f, 0.5f,   0.f, 1, 0.0f,  0.00f },
        { "MX TUBE",              0,  -3.0f, 120.f,  0.f, 0.00f, 0.7f, 0.8f,   0.f, 1, 0.0f,  0.30f },
        { "SX DRIVE",             0,   6.0f, 250.f,  6.f, 0.15f, 1.0f, 0.7f,   0.f, 1, 0.0f,  0.05f },
        { "MX DRIVE",             0,   0.0f, 250.f,  6.f, 0.00f, 0.8f, 0.8f, 150.f, 1, 0.0f,  0.20f },
        { "SX EXTREME",           0,   3.0f, 200.f, 12.f, 0.60f, 2.0f, 1.0f,   0.f, 1, 0.0f,  0.10f },
        { "MX EXTREME",           0,   0.0f, 250.f, 10.f, 0.10f, 1.2f, 0.8f, 120.f, 2, 0.7f,  0.25f },
        { "12AX7 / ECC83",        0,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "12AT7 / ECC81",        1,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "12AU7 / ECC82",        2,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "5751",                 3,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6DJ8 / ECC88",         4,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6922",                 5,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "EF86",                 6,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.15f },
        { "6SN7",                 7,   1.f, 120.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6SL7",                 8,   1.f, 300.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "12BH7A",               9,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "EL84",                10,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "EL34",                11,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6L6GC",               12,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6V6GT",               13,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.3f, 0.f },
        { "KT66",                14,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "KT77",                15,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "KT88",                16,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "6550",                17,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "300B",                18,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
        { "2A3",                 19,   0.f, 250.f, 0.f, 0.f, 1.f, 1.f, 0.f, 1, 0.f, 0.f },
    }};
    return p;
}
} // namespace eta
