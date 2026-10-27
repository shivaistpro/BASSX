/*
  ==============================================================================

    BASSX — Glitch.h

    Due effetti a tempo, slot della catena FX:

      GLITCH       a ogni passo della griglia (RATE), con probabilita' AMOUNT,
                   il passo appena suonato viene rimpiazzato da uno di:
                   STUTTER (il primo quarto ripetuto 4 volte), REVERSE,
                   TAPE STOP (velocita' 1 -> 0), GATE (tagli a 1/4 del passo).
                   Il dado e' deterministico (passo + SEED): un loop di 4
                   battute glitcha uguale a ogni giro, come una clip editata.
      BEAT REPEAT  ogni INTERVAL, con probabilita' CHANCE, cattura GRID di
                   audio e lo ripete per GATE, con DECAY per ripetizione:
                   il classico rullo di fine battuta.

    Tutto suona da memoria (nessuna latenza): il glitch rilegge il passo
    precedente, il beat repeat registra mentre il primo grid passa.
    Bordi con dissolvenza di 3 ms: niente click ai tagli.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <cmath>
#include <vector>

namespace bassx
{

/** Orologio musicale comune: posizione in battiti campione per campione,
    dalla DAW in play o dal BPM a trasporto fermo. */
struct FxClock
{
    double sampleRate = 44100.0, freePpq = 0.0;

    void advance (double bpm, double ppq, bool playing, int n) noexcept
    {
        const double spb = 60.0 / std::max (20.0, bpm) * sampleRate;
        freePpq = (playing ? ppq : freePpq) + (double) n / spb;
    }

    double beatAt (double bpm, double ppq, bool playing, int i) const noexcept
    {
        const double spb = 60.0 / std::max (20.0, bpm) * sampleRate;
        return (playing ? ppq : freePpq) + (double) i / spb;
    }
};

//==============================================================================
class GlitchFx
{
public:
    enum class Mode { Random = 0, Stutter, Reverse, TapeStop, Gate };

    static juce::StringArray getModeNames() { return { "Random", "Stutter", "Reverse", "Tape Stop", "Gate" }; }

    void prepare (double sr)
    {
        clock.sampleRate = sampleRate = sr > 0.0 ? sr : 44100.0;
        const int len = (int) (sampleRate * 8.0) + 8;           // un passo fino a 2 battute a 30 BPM
        for (auto& b : history) b.assign ((size_t) len, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : history) std::fill (b.begin(), b.end(), 0.0f);
        write = 0; lastStep = -1; action = Mode::Random; acting = false; mixSm = 0.0f;
    }

    void set (bool on, float stepBeats, float amount, float mix, int mode, int seed) noexcept
    {
        enabled = on; beatsPerStep = std::max (1.0f / 64.0f, stepBeats);
        prob = std::clamp (amount, 0.0f, 1.0f); wet = std::clamp (mix, 0.0f, 1.0f);
        forcedMode = std::clamp (mode, 0, 4); seedValue = seed;
    }

    void process (juce::dsp::AudioBlock<float>& block, double bpm, double ppq, bool playing) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int numCh = std::min (2, (int) block.getNumChannels());
        const int len = (int) history[0].size();
        if (len == 0) return;

        const double spb = 60.0 / std::max (20.0, bpm) * sampleRate;
        const int stepLen = std::clamp ((int) (beatsPerStep * spb), 32, len / 2);
        const int fade = std::max (8, (int) (0.003 * sampleRate));
        const float mixCoef = std::exp (-1.0f / (0.01f * (float) sampleRate));

        for (int i = 0; i < n; ++i)
        {
            const long long step = (long long) std::floor (clock.beatAt (bpm, ppq, playing, i) / beatsPerStep);
            if (step != lastStep)
            {
                lastStep = step;
                stepStart = write;
                posInStep = 0;
                // dado deterministico: stesso passo, stessa scelta
                const uint32_t h = hash ((uint32_t) step * 2654435761u ^ (uint32_t) seedValue * 40503u);
                acting = enabled && (float) (h & 0xffff) / 65535.0f < prob;
                action = forcedMode != 0 ? (Mode) forcedMode : (Mode) (1 + (int) ((h >> 16) % 4));
            }

            // posizione di lettura dentro il passo precedente (gia' registrato)
            const int prevStart = (stepStart - stepLen + len) % len;
            float src = (float) posInStep;
            float gain = 1.0f;

            switch (action)
            {
                case Mode::Stutter:  src = (float) (posInStep % std::max (1, stepLen / 4)); break;
                case Mode::Reverse:  src = (float) (stepLen - 1 - posInStep); break;
                case Mode::TapeStop:
                {
                    const float t = std::min (1.0f, (float) posInStep / (float) stepLen);
                    src = (float) stepLen * (t - 0.5f * t * t);    // velocita' 1 -> 0, integrata
                    gain = 1.0f - t * t;
                    break;
                }
                case Mode::Gate:     gain = ((posInStep * 8 / std::max (1, stepLen)) % 2 == 0) ? 1.0f : 0.0f; break;
                case Mode::Random:
                default: break;
            }

            // dissolvenze ai bordi del passo e dei frammenti ripetuti
            const int seg = (action == Mode::Stutter) ? std::max (1, stepLen / 4) : stepLen;
            const int inSeg = posInStep % seg;
            const float edge = std::min (1.0f, std::min ((float) inSeg, (float) (seg - 1 - inSeg)) / (float) fade);
            mixSm = (acting ? wet : 0.0f) + (mixSm - (acting ? wet : 0.0f)) * mixCoef;

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = block.getChannelPointer ((size_t) ch);
                auto& h = history[(size_t) ch];
                const float x = d[i];
                h[(size_t) write] = x;

                const float pos = (float) prevStart + std::clamp (src, 0.0f, (float) (stepLen - 1));
                const int i0 = (int) pos % len, i1 = (i0 + 1) % len;
                const float fr = pos - std::floor (pos);
                const float g = h[(size_t) i0] + (h[(size_t) i1] - h[(size_t) i0]) * fr;

                d[i] = x + (g * gain * edge - x) * mixSm;
            }

            write = (write + 1) % len;
            ++posInStep;
        }

        clock.advance (bpm, ppq, playing, n);
    }

private:
    static uint32_t hash (uint32_t x) noexcept
    {
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x;
    }

    double sampleRate = 44100.0;
    FxClock clock;
    std::array<std::vector<float>, 2> history;
    int write = 0, stepStart = 0, posInStep = 0;
    long long lastStep = -1;
    bool enabled = false, acting = false;
    Mode action = Mode::Random;
    float beatsPerStep = 0.25f, prob = 0.5f, wet = 1.0f, mixSm = 0.0f;
    int forcedMode = 0, seedValue = 0;
};

//==============================================================================
class BeatRepeat
{
public:
    void prepare (double sr)
    {
        clock.sampleRate = sampleRate = sr > 0.0 ? sr : 44100.0;
        for (auto& b : grab) b.assign ((size_t) (sampleRate * 4.0) + 8, 0.0f);
        reset();
    }

    void reset() noexcept
    {
        lastInterval = -1; repeating = false; recorded = 0; pos = 0; repeatsDone = 0; mixSm = 0.0f;
    }

    void set (bool on, float intervalBeats, float gridBeats, float chance, float gateBeats, float decay, float mix) noexcept
    {
        enabled = on; interval = std::max (0.25f, intervalBeats); grid = std::max (1.0f / 32.0f, gridBeats);
        prob = std::clamp (chance, 0.0f, 1.0f); gate = std::max (grid, gateBeats);
        decayPerRepeat = std::clamp (decay, 0.0f, 1.0f); wet = std::clamp (mix, 0.0f, 1.0f);
    }

    bool isRepeating() const noexcept { return repeating; }

    void process (juce::dsp::AudioBlock<float>& block, double bpm, double ppq, bool playing) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int numCh = std::min (2, (int) block.getNumChannels());
        const int cap = (int) grab[0].size();
        if (cap == 0) return;

        const double spb = 60.0 / std::max (20.0, bpm) * sampleRate;
        const int gridLen = std::clamp ((int) (grid * spb), 32, cap);
        const int fade = std::max (8, (int) (0.003 * sampleRate));
        const float mixCoef = std::exp (-1.0f / (0.005f * (float) sampleRate));

        for (int i = 0; i < n; ++i)
        {
            const double beat = clock.beatAt (bpm, ppq, playing, i);
            const long long iv = (long long) std::floor (beat / interval);

            if (iv != lastInterval)
            {
                lastInterval = iv;
                const uint32_t h = (uint32_t) iv * 2654435761u;
                repeating = enabled && (float) ((h >> 8) & 0xffff) / 65535.0f < prob;
                startBeat = (double) iv * interval;
                recorded = 0; pos = 0; repeatsDone = 0;
            }

            // il ripetere dura GATE battiti dall'inizio dell'intervallo
            const bool inGate = repeating && (beat - startBeat) < (double) gate;
            const float target = inGate ? wet : 0.0f;
            mixSm = target + (mixSm - target) * mixCoef;

            float level = std::pow (1.0f - decayPerRepeat, (float) repeatsDone);
            const int inGrid = pos;
            const float edge = std::min (1.0f, std::min ((float) inGrid, (float) (gridLen - 1 - inGrid)) / (float) fade);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = block.getChannelPointer ((size_t) ch);
                const float x = d[i];
                if (recorded < gridLen) grab[(size_t) ch][(size_t) recorded] = x;   // il primo grid passa e si registra
                const float rep = (recorded < gridLen) ? x : grab[(size_t) ch][(size_t) pos] * level * edge;
                d[i] = x + (rep - x) * mixSm;
            }

            if (recorded < gridLen) ++recorded;
            if (++pos >= gridLen) { pos = 0; if (recorded >= gridLen) ++repeatsDone; }
        }

        clock.advance (bpm, ppq, playing, n);
    }

private:
    double sampleRate = 44100.0;
    FxClock clock;
    std::array<std::vector<float>, 2> grab;
    long long lastInterval = -1;
    double startBeat = 0.0;
    bool enabled = false, repeating = false;
    int recorded = 0, pos = 0, repeatsDone = 0;
    float interval = 4.0f, grid = 0.25f, prob = 1.0f, gate = 1.0f, decayPerRepeat = 0.0f, wet = 1.0f, mixSm = 0.0f;
};

} // namespace bassx
