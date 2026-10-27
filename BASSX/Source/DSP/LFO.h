/*
  ==============================================================================

    BASSX — LFO.h

    Oscillatore a bassa frequenza multiforma, sincronizzabile al tempo dell'host.

    Due dettagli che distinguono un LFO professionale da uno amatoriale:

    1. Le forme con discontinuità (quadra, rampa) sono lisciate da un filtro a
       un polo con costante di tempo di ~1 ms. Un LFO che salta da -1 a +1 in un
       campione, applicato al cutoff o al pitch, produce un click. Non è
       aliasing — l'LFO è sotto la banda audio — è proprio un gradino nel
       segnale di controllo. Tutti i sintetizzatori seri lo lisciano; è la
       ragione per cui la quadra di un LFO analogico ha sempre un piccolo
       fronte di salita.

    2. La sincronizzazione al tempo è ancorata alla posizione assoluta nel
       brano (ppqPosition), non a un contatore interno. Con un contatore
       interno, spostare la testina di riproduzione lascia l'LFO sfasato
       rispetto alla griglia, e ogni render successivo del brano suona
       leggermente diverso. Ancorandolo alla posizione, il risultato è
       riproducibile campione per campione.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include "Common.h"

namespace bassx
{

//==============================================================================
class LFO
{
public:
    enum class Shape
    {
        Sine = 0,
        Triangle,
        SawUp,
        SawDown,
        Square,
        Pulse25,
        SampleHold,
        SmoothRandom,
        ExpRamp,
        Chaos,
        NumShapes
    };

    enum class TriggerMode
    {
        Free = 0,     // corre indipendente, mai resettato
        Note,         // riparte da zero a ogni note-on
        Legato,       // riparte solo sulla prima nota di una frase legata
        Envelope,     // percorre una sola volta e si ferma (one-shot)
        NumModes
    };

    static juce::StringArray getShapeNames()
    {
        return { "Sine", "Triangle", "Saw Up", "Saw Down", "Square",
                 "Pulse 25%", "Sample & Hold", "Smooth Random", "Exp Ramp", "Chaos" };
    }

    static juce::StringArray getTriggerModeNames()
    {
        return { "Free", "Note", "Legato", "One Shot" };
    }

    /** Divisioni di tempo disponibili in modalità sync, espresse in battute.
        L'ordine definisce l'indice del parametro: non riordinare. */
    static juce::StringArray getSyncDivisionNames()
    {
        return { "8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/2T", "1/4.", "1/4",
                 "1/4T", "1/8.", "1/8", "1/8T", "1/16.", "1/16", "1/16T", "1/32" };
    }

    static float getSyncDivisionInBeats (int index)
    {
        // In quarti (beat). 1 bar = 4 beat in 4/4.
        static const float beats[] =
        {
            32.0f, 16.0f, 8.0f, 4.0f,
            2.0f, 4.0f / 3.0f, 1.5f, 1.0f,
            2.0f / 3.0f, 0.75f, 0.5f, 1.0f / 3.0f,
            0.375f, 0.25f, 1.0f / 6.0f, 0.125f
        };

        constexpr int count = (int) (sizeof (beats) / sizeof (beats[0]));
        return beats[juce::jlimit (0, count - 1, index)];
    }

    //==========================================================================
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        smoother.prepare (sampleRate, 1.0f);
        updateIncrement();
        reset();
    }

    void reset() noexcept
    {
        phase = (triggerMode == TriggerMode::Free) ? 0.0f : phaseOffset;
        smoother.snap (0.0f);
        sampleHoldValue = rng.nextBipolar();
        randomTarget = rng.nextBipolar();
        randomPrevious = randomTarget;
        lastPhase = phase;
        chaosState = 0.31f;
        fadeGain = (fadeInSeconds > 0.0f) ? 0.0f : 1.0f;
        oneShotFinished = false;
    }

    //==========================================================================
    void setShape (Shape s) noexcept { shape = s; }
    void setTriggerMode (TriggerMode m) noexcept { triggerMode = m; }

    void setRateHz (float hz) noexcept
    {
        rateHz = std::clamp (hz, 0.01f, 200.0f);
        updateIncrement();
    }

    void setSyncEnabled (bool shouldSync) noexcept
    {
        tempoSync = shouldSync;
        updateIncrement();
    }

    void setSyncDivision (int index) noexcept
    {
        syncDivisionIndex = index;
        updateIncrement();
    }

    /** Informazioni di trasporto dall'host, aggiornate una volta per blocco. */
    void setTransport (double bpm, double ppqPos, bool playing) noexcept
    {
        hostBpm = (bpm > 1.0) ? bpm : 120.0;
        hostPpq = ppqPos;
        hostPlaying = playing;
        updateIncrement();
    }

    /** Riallinea la fase alla griglia assoluta del brano. Chiamato una volta
        per blocco quando il sync è attivo e il trasporto sta suonando. */
    void alignToTransport() noexcept
    {
        if (! tempoSync || ! hostPlaying)
            return;

        const float beatsPerCycle = getSyncDivisionInBeats (syncDivisionIndex);

        if (beatsPerCycle <= 0.0f)
            return;

        phase = wrap01 ((float) (hostPpq / (double) beatsPerCycle) + phaseOffset);
    }

    /** 0..1, sfasamento iniziale. */
    void setPhaseOffset (float p) noexcept { phaseOffset = wrap01 (p); }

    /** 0 = bipolare (-1..1), 1 = unipolare (0..1). L'unipolare serve a
        modulare grandezze che non hanno senso negative, tipicamente
        l'ampiezza. */
    void setUnipolar (bool shouldBeUnipolar) noexcept { unipolar = shouldBeUnipolar; }

    /** Tempo di dissolvenza in ingresso: l'LFO parte a zero e cresce. Su un
        vibrato è la differenza fra "espressivo" e "seasick". */
    void setFadeIn (float seconds) noexcept
    {
        fadeInSeconds = std::max (0.0f, seconds);
        fadeIncrement = (fadeInSeconds > 0.0f)
                          ? (1.0f / (fadeInSeconds * (float) sampleRate))
                          : 1.0f;
    }

    /** Lisciatura delle forme discontinue, in millisecondi. */
    void setSmoothing (float ms) noexcept { smoother.setTime (std::max (0.05f, ms)); }

    //==========================================================================
    void noteOn (bool isLegatoContinuation) noexcept
    {
        switch (triggerMode)
        {
            case TriggerMode::Free:
                break;

            case TriggerMode::Note:
                phase = phaseOffset;
                oneShotFinished = false;
                break;

            case TriggerMode::Legato:
                if (! isLegatoContinuation)
                {
                    phase = phaseOffset;
                    oneShotFinished = false;
                }
                break;

            case TriggerMode::Envelope:
                if (! isLegatoContinuation)
                {
                    phase = phaseOffset;
                    oneShotFinished = false;
                }
                break;

            case TriggerMode::NumModes:
                break;
        }

        if (fadeInSeconds > 0.0f && ! isLegatoContinuation)
            fadeGain = 0.0f;
    }

    //==========================================================================
    float getNextSample() noexcept
    {
        if (triggerMode == TriggerMode::Envelope && oneShotFinished)
            return unipolar ? 0.0f : 0.0f;

        // In modalita' Free la fase non viene mai riportata a phaseOffset (nessun
        // trigger): l'offset si applica in lettura, altrimenti la manopola PHASE
        // non faceva nulla. Col sync a trasporto in play l'offset e' gia' nella fase.
        const bool offsetOnRead = triggerMode == TriggerMode::Free && ! (tempoSync && hostPlaying);
        const float raw = evaluate (offsetOnRead ? wrap01 (phase + phaseOffset) : phase);

        // Avanzamento e rilevazione del wrap (serve a sample & hold e chaos).
        lastPhase = phase;
        phase += increment;

        if (phase >= 1.0f)
        {
            phase -= std::floor (phase);

            if (triggerMode == TriggerMode::Envelope)
                oneShotFinished = true;

            onCycleWrap();
        }

        // Lisciatura: applicata a tutte le forme per uniformità di latenza fra
        // di esse. Su seno e triangolo l'effetto è sotto i -80 dB.
        smoother.setTarget (raw);
        float value = smoother.next();

        // Dissolvenza in ingresso.
        if (fadeGain < 1.0f)
        {
            fadeGain = std::min (1.0f, fadeGain + fadeIncrement);
            value *= fadeGain;
        }

        return unipolar ? (value * 0.5f + 0.5f) : value;
    }

    float getCurrentValue() const noexcept
    {
        const float v = smoother.getCurrent();
        return unipolar ? (v * 0.5f + 0.5f) : v;
    }

    float getPhase() const noexcept { return phase; }

private:
    //==========================================================================
    void updateIncrement() noexcept
    {
        if (tempoSync)
        {
            const float beatsPerCycle = getSyncDivisionInBeats (syncDivisionIndex);
            const double beatsPerSecond = hostBpm / 60.0;
            const double cyclesPerSecond = beatsPerSecond / (double) beatsPerCycle;
            increment = (float) (cyclesPerSecond / sampleRate);
        }
        else
        {
            increment = (float) (rateHz / sampleRate);
        }
    }

    void onCycleWrap() noexcept
    {
        sampleHoldValue = rng.nextBipolar();
        randomPrevious  = randomTarget;
        randomTarget    = rng.nextBipolar();
    }

    float evaluate (float p) noexcept
    {
        switch (shape)
        {
            case Shape::Sine:
                return std::sin (kTwoPi * p);

            case Shape::Triangle:
                return 4.0f * std::abs (p - 0.5f) - 1.0f;

            case Shape::SawUp:
                return 2.0f * p - 1.0f;

            case Shape::SawDown:
                return 1.0f - 2.0f * p;

            case Shape::Square:
                return p < 0.5f ? 1.0f : -1.0f;

            case Shape::Pulse25:
                return p < 0.25f ? 1.0f : -1.0f;

            case Shape::SampleHold:
                return sampleHoldValue;

            case Shape::SmoothRandom:
            {
                // Interpolazione coseno fra il valore precedente e il nuovo:
                // continua anche nella derivata prima, quindi non produce
                // spigoli sul parametro modulato.
                const float t = 0.5f - 0.5f * std::cos (kPi * p);
                return lerp (randomPrevious, randomTarget, t);
            }

            case Shape::ExpRamp:
                // Rampa esponenziale discendente: la forma di un inviluppo di
                // percussione, utile per accentare ritmicamente il cutoff.
                return 2.0f * std::exp (-4.0f * p) - 1.0f;

            case Shape::Chaos:
            {
                // Mappa logistica al bordo del caos (r = 3.95). Deterministica
                // e riproducibile, ma percettivamente non periodica: dà
                // movimento continuo senza il pattern riconoscibile di un
                // sample & hold.
                if (p < lastPhase)
                    chaosState = 3.95f * chaosState * (1.0f - chaosState);

                return chaosState * 2.0f - 1.0f;
            }

            case Shape::NumShapes:
            default:
                return 0.0f;
        }
    }

    //==========================================================================
    double sampleRate = 44100.0;
    double hostBpm = 120.0, hostPpq = 0.0;
    bool   hostPlaying = false;

    Shape shape = Shape::Sine;
    TriggerMode triggerMode = TriggerMode::Free;

    float rateHz = 2.0f;
    bool  tempoSync = false;
    int   syncDivisionIndex = 10;      // 1/8

    float phase = 0.0f, lastPhase = 0.0f, increment = 0.0f, phaseOffset = 0.0f;
    bool  unipolar = false;

    float fadeInSeconds = 0.0f, fadeIncrement = 1.0f, fadeGain = 1.0f;
    bool  oneShotFinished = false;

    float sampleHoldValue = 0.0f;
    float randomTarget = 0.0f, randomPrevious = 0.0f;
    float chaosState = 0.31f;

    ParamSmoother smoother;
    Xorshift rng { 0x7A5F1C3Du };
};

} // namespace bassx
