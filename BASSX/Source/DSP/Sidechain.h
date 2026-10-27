/*
  ==============================================================================

    BASSX — Sidechain.h

    Ducking a curva disegnata, sulla falsariga di Kickstart 2, con quello che
    a Kickstart manca:

      CURVA     8 curve di fabbrica + CUSTOM: punti liberi con tensione per
                segmento (la GUI la disegna, qui arriva gia' campionata in una
                tabella di 256 valori, scambiata senza lock).
      TRIGGER   SYNC  : la curva gira sulla griglia della DAW (1/4 = cassa
                        in quattro). A trasporto fermo segue il BPM.
                MIDI  : ogni nota sul canale 16 fa ripartire la curva (one
                        shot): la cassa del piano roll comanda il ducking.
                AUDIO : la cassa vera sull'ingresso esterno (sidechain):
                        rivelatore d'inviluppo con soglia, ri-armo a 60 ms.
      PHASE     sposta la curva rispetto al battito (-50..+50% del ciclo):
                il punto della battuta dove cade il taglio.
      BAND      il ducking agisce solo fra LOW e HIGH (crossover
                Linkwitz-Riley 4° ordine, somma piatta): si abbassano solo
                le basse sotto la cassa, il resto del basso resta davanti.
      SMOOTH    rampa anti-click sul guadagno (0.1..20 ms).

    Nessuna latenza, nessuna allocazione nel percorso audio.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cmath>

namespace bassx
{

class SidechainDucker
{
public:
    static constexpr int kTableSize = 256;
    static constexpr int kMaxPoints = 32;

    enum class Trigger { Sync = 0, Midi, Audio };

    struct Point { float x = 0.0f, y = 0.0f, tension = 0.0f; };   // tensione del segmento che parte da qui

    struct Curve
    {
        std::array<Point, kMaxPoints> points {};
        int numPoints = 0;
    };

    static juce::StringArray getShapeNames()
    {
        return { "Classic", "Hard Pump", "Smooth", "Short Tick", "Long Swell",
                 "Gate", "Double", "Slow Attack", "Custom" };
    }

    static constexpr int kCustomShape = 8;

    /** Curve di fabbrica: y = guadagno (0 = taglio pieno, 1 = passa). */
    static Curve getFactoryShape (int index)
    {
        Curve c;
        auto add = [&c] (float x, float y, float t = 0.0f) { c.points[(size_t) c.numPoints++] = { x, y, t }; };

        switch (index)
        {
            case 1:  add (0.0f, 0.0f, 0.0f);  add (0.30f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;      // Hard Pump
            case 2:  add (0.0f, 0.0f, -0.6f); add (0.75f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;      // Smooth
            case 3:  add (0.0f, 0.0f, 0.5f);  add (0.12f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;      // Short Tick
            case 4:  add (0.0f, 0.0f, -0.3f); add (0.95f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;      // Long Swell
            case 5:  add (0.0f, 0.0f, 0.0f);  add (0.25f, 0.0f, 0.0f);  add (0.27f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;  // Gate
            case 6:  add (0.0f, 0.0f, 0.5f);  add (0.40f, 1.0f, 0.0f);  add (0.50f, 0.1f, 0.5f);  add (0.90f, 1.0f, 0.0f); add (1.0f, 1.0f); break; // Double
            case 7:  add (0.0f, 1.0f, -0.4f); add (0.10f, 0.0f, 0.4f);  add (0.60f, 1.0f, 0.0f);  add (1.0f, 1.0f); break; // Slow Attack
            case 0:
            default: add (0.0f, 0.0f, 0.55f); add (0.45f, 1.0f, 0.0f);  add (1.0f, 1.0f); break;      // Classic
        }
        return c;
    }

    /** Valore della curva in x (0..1). Tensione > 0: sale presto (convessa),
        < 0: sale tardi (concava). */
    static float evaluate (const Curve& c, float x) noexcept
    {
        if (c.numPoints <= 0) return 1.0f;
        if (x <= c.points[0].x) return c.points[0].y;

        for (int i = 0; i < c.numPoints - 1; ++i)
        {
            const auto& a = c.points[(size_t) i];
            const auto& b = c.points[(size_t) i + 1];

            if (x <= b.x)
            {
                const float span = std::max (1.0e-6f, b.x - a.x);
                float t = std::clamp ((x - a.x) / span, 0.0f, 1.0f);
                const float k = std::clamp (a.tension, -0.95f, 0.95f);
                // curva di potenza: esponente 1 = retta
                const float e = (k >= 0.0f) ? (1.0f - k) : 1.0f / (1.0f + k);
                t = std::pow (t, e);
                return a.y + (b.y - a.y) * t;
            }
        }

        return c.points[(size_t) c.numPoints - 1].y;
    }

    static void render (const Curve& c, std::array<float, kTableSize>& table) noexcept
    {
        for (int i = 0; i < kTableSize; ++i)
            table[(size_t) i] = std::clamp (evaluate (c, (float) i / (float) (kTableSize - 1)), 0.0f, 1.0f);
    }

    //==========================================================================
    /** Thread messaggi: nuova curva. Doppio buffer, l'audio legge sempre una
        tabella intera e mai una a meta' scrittura. */
    void setCurve (const Curve& c) noexcept
    {
        const int next = 1 - liveTable.load (std::memory_order_acquire);
        render (c, tables[(size_t) next]);
        liveTable.store (next, std::memory_order_release);
    }

    //==========================================================================
    void prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;

        juce::dsp::ProcessSpec spec { sampleRate, 4096, 2 };
        for (auto* f : { &lowSplit, &highSplit })
        {
            f->prepare (spec);
            f->setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        }

        gainSmoothed = 1.0f;
        reset();
    }

    void reset() noexcept
    {
        lowSplit.reset(); highSplit.reset();
        phase = 1.0f;
        oneShotRunning = false;
        detector = 0.0f;
        rearm = 0;
        freePpq = 0.0;
    }

    struct Settings
    {
        bool  on = false;
        int   trigger = 0;
        float beatsPerCycle = 1.0f;     // 1/4 = 1 battito
        float depth = 1.0f;
        float phaseOffset = 0.0f;       // -0.5..0.5 del ciclo
        float smoothMs = 2.0f;
        float bandLowHz = 20.0f, bandHighHz = 20000.0f;
        float thresholdDb = -24.0f;
    };

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        smoothCoef = std::exp (-1.0f / (std::max (0.1f, s.smoothMs) * 0.001f * (float) sampleRate));
        lowSplit.setCutoffFrequency (std::clamp (s.bandLowHz, 20.0f, 18000.0f));
        highSplit.setCutoffFrequency (std::clamp (s.bandHighHz, 25.0f, 20000.0f));
    }

    /** Nota sul canale di trigger (MIDI) o colpo rilevato: riparte la curva. */
    void trigger() noexcept { phase = 0.0f; oneShotRunning = true; }

    /**
        @param ext   ingresso esterno (canale sinistro/destro) per il trigger AUDIO,
                     nullptr se non collegato
    */
    void process (juce::dsp::AudioBlock<float>& block, double bpm, double ppq, bool playing,
                  const float* extL, const float* extR) noexcept
    {
        const int n = (int) block.getNumSamples();
        const int numCh = std::min (2, (int) block.getNumChannels());
        auto* L = block.getChannelPointer (0);
        auto* R = numCh > 1 ? block.getChannelPointer (1) : nullptr;

        const float* table = tables[(size_t) liveTable.load (std::memory_order_acquire)].data();
        const double samplesPerBeat = 60.0 / std::max (20.0, bpm) * sampleRate;
        const double cycleSamples = std::max (16.0, samplesPerBeat * (double) settings.beatsPerCycle);

        const bool fullBand = settings.bandLowHz <= 20.5f && settings.bandHighHz >= 19999.0f;
        const bool hasLow = settings.bandLowHz > 20.5f;
        const bool hasHigh = settings.bandHighHz < 19999.0f;
        const auto trig = (Trigger) std::clamp (settings.trigger, 0, 2);
        const float thresh = std::pow (10.0f, settings.thresholdDb / 20.0f);
        const float detRelease = std::exp (-1.0f / (0.030f * (float) sampleRate));

        for (int i = 0; i < n; ++i)
        {
            // ---- fase della curva -------------------------------------------
            float p;
            if (trig == Trigger::Sync)
            {
                const double beats = playing ? ppq + (double) i / samplesPerBeat
                                             : freePpq + (double) i / samplesPerBeat;
                const double cyc = beats / (double) settings.beatsPerCycle;
                p = (float) (cyc - std::floor (cyc));
            }
            else
            {
                if (trig == Trigger::Audio && extL != nullptr)
                {
                    const float x = std::max (std::abs (extL[i]), extR != nullptr ? std::abs (extR[i]) : 0.0f);
                    detector = std::max (x, detector * detRelease);
                    if (rearm > 0) --rearm;
                    if (detector > thresh && ! detectorHigh && rearm == 0)
                    {
                        trigger();
                        rearm = (int) (0.060 * sampleRate);
                    }
                    detectorHigh = detector > thresh * 0.7f;   // isteresi
                }

                if (oneShotRunning)
                {
                    phase += (float) (1.0 / cycleSamples);
                    if (phase >= 1.0f) { phase = 1.0f; oneShotRunning = false; }
                }
                p = phase;
            }

            // PHASE sposta il taglio dentro il ciclo
            float q = p - settings.phaseOffset;
            if (trig == Trigger::Sync) q -= std::floor (q);
            else q = std::clamp (q, 0.0f, 1.0f);

            const float pos = q * (float) (kTableSize - 1);
            const int k = std::min (kTableSize - 2, (int) pos);
            const float frac = pos - (float) k;
            const float curve = table[k] + (table[k + 1] - table[k]) * frac;

            const float target = settings.on ? 1.0f - settings.depth * (1.0f - curve) : 1.0f;
            gainSmoothed = target + (gainSmoothed - target) * smoothCoef;
            const float g = gainSmoothed;

            if (i == n - 1) { lastPhase.store (q, std::memory_order_relaxed); lastGain.store (g, std::memory_order_relaxed); }

            // ---- applicazione: tutta la banda o solo LOW..HIGH --------------
            for (int ch = 0; ch < numCh; ++ch)
            {
                float* d = (ch == 0) ? L : R;
                if (fullBand) { d[i] *= g; continue; }

                float x = d[i], below = 0.0f, above = 0.0f;
                if (hasLow)  { float lo, hi; lowSplit.processSample (ch, x, lo, hi);  below = lo; x = hi; }
                if (hasHigh) { float lo, hi; highSplit.processSample (ch, x, lo, hi); above = hi; x = lo; }
                d[i] = below + x * g + above;
            }
        }

        if (! playing)
            freePpq += (double) n / samplesPerBeat;
        else
            freePpq = ppq + (double) n / samplesPerBeat;
    }

    float getPhase() const noexcept { return lastPhase.load (std::memory_order_relaxed); }
    float getGain() const noexcept  { return lastGain.load (std::memory_order_relaxed); }

private:
    double sampleRate = 44100.0;
    Settings settings;

    std::array<std::array<float, kTableSize>, 2> tables { [] {
        std::array<std::array<float, kTableSize>, 2> t {};
        for (auto& tb : t) tb.fill (1.0f);
        return t; }() };
    std::atomic<int> liveTable { 0 };

    juce::dsp::LinkwitzRileyFilter<float> lowSplit, highSplit;

    float phase = 1.0f, gainSmoothed = 1.0f, smoothCoef = 0.9f;
    bool  oneShotRunning = false, detectorHigh = false;
    float detector = 0.0f;
    int   rearm = 0;
    double freePpq = 0.0;

    std::atomic<float> lastPhase { 0.0f }, lastGain { 1.0f };
};

} // namespace bassx
