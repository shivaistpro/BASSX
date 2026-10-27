/*
  ==============================================================================

    BASSX — Oscillator.h

    Oscillatore wavetable con unison, hard sync corretto con BLEP, modulazione
    di fase (FM) e wavefolding per oscillatore.

    Scelte di progetto e loro motivo:

    * Lettura con interpolazione di Hermite invece che lineare.
      L'interpolazione lineare su una tabella di 2048 punti attenua l'armonica
      più alta utilizzabile di circa 3.9 dB e ne sporca la fase. Su una singola
      voce è quasi impercettibile; su 16 voci di unison leggermente disaccordate
      gli errori non si cancellano ma si sommano in modo incoerente, e il
      risultato è una patina di rumore rosa sopra il segnale. Hermite porta
      quell'errore sotto i -90 dB al costo di due moltiplicazioni in più.

    * Hard sync con correzione BLEP.
      Il sync è una discontinuità di gradino nel segnale. Senza correzione
      genera aliasing a piena banda che l'oversampling non può rimuovere
      (l'alias nasce già dentro la banda). Il residuo polyBLEP scalato per
      l'altezza effettiva del gradino riduce l'aliasing di ~30 dB, che è ciò che
      separa un sync utilizzabile da uno che va usato solo in fortissimo.

    * Distribuzione del detune dell'unison non lineare.
      Distribuire N voci a intervalli uguali produce battimenti a frequenze
      multiple l'una dell'altra, che si fondono in un unico tremolo periodico
      invece che in un allargamento. La curva usata qui (potenza 1.35 sulla
      posizione normalizzata) rompe la commensurabilità.

  ==============================================================================
*/

#pragma once

#include "Common.h"
#include "WavetableBank.h"

namespace bassx
{

//==============================================================================

inline constexpr int kMaxUnisonVoices = 16;

//==============================================================================
/**
    Oscillatore wavetable stereo con unison.
*/
class WavetableOscillator
{
public:
    WavetableOscillator() = default;

    //==========================================================================
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        updateIncrements();
        reset (false);      // fasi programmate: layer allineati anche senza retrigger
    }

    /** @param randomisePhase  true all'inizializzazione, false sul note-on
                               quando si vuole un transiente ripetibile. */
    void reset (bool randomisePhase) noexcept
    {
        for (size_t v = 0; v < (size_t) kMaxUnisonVoices; ++v)
        {
            phase[v] = randomisePhase ? rng.nextUnipolar()
                                      : phaseOffsets[v];
            lastOutput[v] = 0.0f;
        }

        syncPhase = 0.0f;
        blepBuffer[0] = blepBuffer[1] = 0.0f;
    }

    /** Riporta tutte le voci alla fase iniziale programmata. Chiamato sul
        note-on non legato: è ciò che rende il transiente d'attacco identico a
        ogni nota invece che casuale, requisito per un basso che deve stare
        agganciato alla cassa. */
    void resetPhaseHard() noexcept
    {
        for (size_t v = 0; v < (size_t) kMaxUnisonVoices; ++v)
            phase[v] = phaseOffsets[v];

        syncPhase = 0.0f;
    }

    //==========================================================================
    void setTable (const Wavetable* t) noexcept
    {
        table = t;
        numFrames = (t != nullptr) ? t->getNumFrames() : 0;
    }

    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        updateIncrements();
    }

    /** Frequenza fondamentale in Hz, prima del detune di unison. */
    void setFrequency (float hz) noexcept
    {
        frequency = std::clamp (hz, 0.01f, (float) sampleRate * 0.49f);
        updateIncrements();
    }

    /** Posizione nella wavetable, 0..1. */
    void setFramePosition (float pos) noexcept
    {
        framePosition = std::clamp (pos, 0.0f, 1.0f);
    }

    /** @param voices    1..16
        @param detune    0..1, mappato su 0..50 cent di scostamento massimo
        @param blend     0 = solo voci esterne, 0.5 = bilanciato, 1 = solo centro
        @param spread    0..1, apertura stereo delle voci esterne
        @param phaseRnd  0..1, casualità della fase iniziale delle voci */
    void setUnison (int voices, float detune, float blend, float spread, float phaseRnd) noexcept
    {
        const int newCount = juce::jlimit (1, kMaxUnisonVoices, voices);

        // Confronto con tolleranza: i parametri arrivano da conversioni in
        // virgola mobile dell'host e un confronto esatto rieseguirebbe il
        // calcolo del layout a ogni blocco per differenze inudibili.
        constexpr float epsilon = 1.0e-6f;

        if (newCount != unisonCount
            || std::abs (detune - unisonDetune) > epsilon
            || std::abs (phaseRnd - phaseRandom) > epsilon)
        {
            unisonCount  = newCount;
            unisonDetune = std::clamp (detune, 0.0f, 1.0f);
            phaseRandom  = std::clamp (phaseRnd, 0.0f, 1.0f);
            recomputeUnisonLayout();
        }

        unisonBlend  = std::clamp (blend, 0.0f, 1.0f);
        unisonSpread = std::clamp (spread, 0.0f, 1.0f);
        updateUnisonGains();
    }

    /** Quantità di wavefolding applicata all'uscita dell'oscillatore, prima
        del mix. Applicato per voce di unison, non sulla somma: piegare la
        somma produrrebbe intermodulazione fra le voci disaccordate, cioè
        rumore anziché armoniche. */
    void setFold (float amount) noexcept { foldAmount = std::clamp (amount, 0.0f, 1.0f); }

    /** Guadagno lineare applicato all'uscita. */
    void setLevel (float g) noexcept { level = g; }

    /** Pan della somma dell'oscillatore, -1..1. */
    void setPan (float p) noexcept { pan = std::clamp (p, -1.0f, 1.0f); }

    /** Abilita il reset di fase forzato da un oscillatore master. */
    void setSyncEnabled (bool shouldSync) noexcept { syncEnabled = shouldSync; }

    /** Frequenza del "master" virtuale per l'hard sync, in Hz. */
    void setSyncFrequency (float hz) noexcept
    {
        syncFrequency = std::clamp (hz, 0.01f, (float) sampleRate * 0.49f);
        syncIncrement = syncFrequency / (float) sampleRate;
    }

    //==========================================================================
    /** Genera un campione stereo.

        @param phaseMod  modulazione di fase in cicli (1.0 = un periodo intero).
                         Applicata a tutte le voci di unison con la stessa
                         quantità: applicarla scalata per la frequenza di
                         ciascuna voce trasformerebbe la PM in FM vera e
                         sposterebbe l'intonazione percepita al crescere
                         dell'indice, cosa che su un basso è inaccettabile. */
    void processStereo (float phaseMod, float& outL, float& outR) noexcept
    {
        outL = outR = 0.0f;

        if (table == nullptr || numFrames == 0)
            return;

        // --- Hard sync: avanza il master e rileva il wrap ---------------------
        bool syncOccurred = false;
        float syncFrac = 0.0f;

        if (syncEnabled)
        {
            syncPhase += syncIncrement;

            if (syncPhase >= 1.0f)
            {
                syncPhase -= 1.0f;
                syncOccurred = true;

                // Posizione sub-campione del reset: serve al BLEP per sapere
                // dove cade la discontinuità dentro il campione corrente.
                syncFrac = (syncIncrement > 0.0f) ? (syncPhase / syncIncrement) : 0.0f;
                syncFrac = std::clamp (syncFrac, 0.0f, 1.0f);
            }
        }

        // --- Selezione dei frame ---------------------------------------------
        const float framePos = framePosition * (float) (numFrames - 1);
        const int   f0 = juce::jlimit (0, numFrames - 1, (int) framePos);
        const int   f1 = juce::jlimit (0, numFrames - 1, f0 + 1);
        const float fMix = framePos - (float) f0;

        const auto& frameA = table->frames[(size_t) f0];
        const auto& frameB = table->frames[(size_t) f1];

        float sumL = 0.0f, sumR = 0.0f;
        float stepJump = 0.0f;

        for (size_t v = 0; v < (size_t) unisonCount; ++v)
        {
            const float inc = increments[v];
            const int   mip = WavetableBank::selectMipLevel (inc);

            // Fase effettiva includendo la modulazione di fase.
            float p = wrap01 (phase[v] + phaseMod);

            float sample = readFrames (frameA, frameB, mip, fMix, p);

            if (foldAmount > 0.0f)
                sample = waveFold (sample, foldAmount);

            // --- Avanzamento e sync ------------------------------------------
            if (syncOccurred)
            {
                // Ampiezza del gradino introdotto dal reset: differenza fra il
                // valore che l'onda avrebbe avuto e quello che avrà dopo il
                // reset. È questa altezza a determinare quanta correzione BLEP
                // serve.
                const float afterSync = readFrames (frameA, frameB, mip, fMix, wrap01 (phaseOffsets[v] + phaseMod));
                stepJump += (afterSync - sample) * voiceGain[v];

                phase[v] = phaseOffsets[v] + inc * syncFrac;
            }
            else
            {
                phase[v] = wrap01 (phase[v] + inc);
            }

            const float g = voiceGain[v];
            sumL += sample * g * voicePanL[v];
            sumR += sample * g * voicePanR[v];

            lastOutput[v] = sample;
        }

        // --- Correzione BLEP del gradino di sync ------------------------------
        if (syncOccurred && std::abs (stepJump) > 1.0e-9f)
        {
            // Residuo polyBLEP a due campioni: uno immediato, uno ritardato.
            const float t = syncFrac;
            const float b0 = -0.5f * t * t + t - 0.5f;
            const float b1 =  0.5f * t * t;

            blepBuffer[0] += stepJump * b0;
            blepBuffer[1] += stepJump * b1;
        }

        const float blep = blepBuffer[0];
        blepBuffer[0] = blepBuffer[1];
        blepBuffer[1] = 0.0f;

        sumL += blep;
        sumR += blep;

        // --- Pan e livello ----------------------------------------------------
        float pl, pr;
        constantPowerPan (pan, pl, pr);

        outL = sumL * level * pl * kSqrt2;
        outR = sumR * level * pr * kSqrt2;
    }

    /** Uscita mono, per i percorsi in cui lo stereo non serve (modulatore FM,
        sorgente di ring modulation). */
    float processMono (float phaseMod) noexcept
    {
        float l, r;
        processStereo (phaseMod, l, r);
        return (l + r) * 0.5f;
    }

    int getUnisonCount() const noexcept { return unisonCount; }

private:
    //==========================================================================
    /** Legge i due frame al livello di mip richiesto e li interpola. */
    static float readFrames (const WavetableFrame& a, const WavetableFrame& b,
                             int mip, float frameMix, float p) noexcept
    {
        const float pos  = p * (float) kTableSize;
        const int   i0   = juce::jlimit (0, kTableSize - 1, (int) pos);
        const float frac = pos - (float) i0;

        const auto& ta = a.mip[(size_t) mip];
        const auto& tb = b.mip[(size_t) mip];

        // i-1 richiede un wrap esplicito; i+1 e i+2 sono coperti dai campioni
        // di guardia in coda alla tabella.
        const int im1 = (i0 == 0) ? (kTableSize - 1) : (i0 - 1);

        const float va = hermite (ta[(size_t) im1], ta[(size_t) i0],
                                  ta[(size_t) (i0 + 1)], ta[(size_t) (i0 + 2)], frac);

        if (frameMix <= 0.0f)
            return va;

        const float vb = hermite (tb[(size_t) im1], tb[(size_t) i0],
                                  tb[(size_t) (i0 + 1)], tb[(size_t) (i0 + 2)], frac);

        return lerp (va, vb, frameMix);
    }

    //==========================================================================
    void updateIncrements() noexcept
    {
        const float baseInc = frequency / (float) sampleRate;

        for (size_t v = 0; v < (size_t) kMaxUnisonVoices; ++v)
            increments[v] = baseInc * detuneRatio[v];

        syncIncrement = syncFrequency / (float) sampleRate;
    }

    /** Ricalcola i rapporti di detune, gli offset di fase e i pan delle voci.
        Chiamata solo quando cambia il numero di voci o la quantità di detune,
        mai per campione. */
    void recomputeUnisonLayout() noexcept
    {
        rng.setSeed (0x2545F491u);

        // Scostamento massimo in cent: 50 cent è mezzo semitono, oltre il quale
        // l'unison smette di essere allargamento e diventa un accordo.
        const float maxCents = unisonDetune * 50.0f;

        for (size_t v = 0; v < (size_t) kMaxUnisonVoices; ++v)
        {
            if (v >= (size_t) unisonCount)
            {
                detuneRatio[v]  = 1.0f;
                phaseOffsets[v] = 0.0f;
                continue;
            }

            float normalised;   // -1 .. +1

            if (unisonCount == 1)
                normalised = 0.0f;
            else
                normalised = (2.0f * (float) v / (float) (unisonCount - 1)) - 1.0f;

            // Curva di potenza 1.35: rompe la commensurabilità dei battimenti
            // che una distribuzione lineare produrrebbe.
            const float shaped = (normalised < 0.0f ? -1.0f : 1.0f)
                               * std::pow (std::abs (normalised), 1.35f);

            detuneRatio[v] = centsToRatio (shaped * maxCents);

            // Fase iniziale: la randomizzazione evita che tutte le voci
            // sommino in fase al primo campione, cosa che produrrebbe un picco
            // di ampiezza pari a unisonCount volte la singola voce.
            // Voce singola: PHASE e' la fase di partenza (0 = allineata agli
            // altri layer). Unison: casualita' della fase delle voci.
            phaseOffsets[v] = unisonCount == 1 ? phaseRandom : phaseRandom * rng.nextUnipolar();
        }

        updateIncrements();
        updateUnisonGains();
    }

    void updateUnisonGains() noexcept
    {
        float energy = 0.0f;

        for (size_t v = 0; v < (size_t) kMaxUnisonVoices; ++v)
        {
            if (v >= (size_t) unisonCount)
            {
                voiceGain[v] = 0.0f;
                voicePanL[v] = voicePanR[v] = 0.0f;
                continue;
            }

            float normalised = (unisonCount == 1)
                                 ? 0.0f
                                 : (2.0f * (float) v / (float) (unisonCount - 1)) - 1.0f;

            const float distance = std::abs (normalised);

            // Blend: 1 privilegia la voce centrale (suono più definito, meno
            // largo), 0 privilegia le esterne (suono cavo e ampio).
            const float centreWeight = 1.0f - distance;
            const float outerWeight  = distance;
            float g = lerp (outerWeight, centreWeight, unisonBlend) + 0.15f;

            voiceGain[v] = g;
            energy += g * g;

            // Le voci si aprono nello stereo proporzionalmente alla loro
            // distanza dal centro: le più disaccordate stanno ai lati, che è la
            // ragione per cui l'unison "suona largo" invece che "suona coro".
            float pl, pr;
            constantPowerPan (normalised * unisonSpread, pl, pr);
            voicePanL[v] = pl * kSqrt2;
            voicePanR[v] = pr * kSqrt2;
        }

        // Normalizzazione in potenza, non in ampiezza: le voci sono
        // decorrelate (detune diverso), quindi sommano in potenza. Dividere per
        // il numero di voci renderebbe l'unison progressivamente più silenzioso.
        const float norm = (energy > 1.0e-9f) ? (1.0f / std::sqrt (energy)) : 1.0f;

        for (size_t v = 0; v < (size_t) unisonCount; ++v)
            voiceGain[v] *= norm;
    }

    //==========================================================================
    const Wavetable* table = nullptr;
    int   numFrames = 0;

    double sampleRate = 44100.0;
    float  frequency = 110.0f;
    float  framePosition = 0.0f;
    float  level = 1.0f;
    float  pan = 0.0f;
    float  foldAmount = 0.0f;

    int   unisonCount = 1;
    float unisonDetune = 0.2f;
    float unisonBlend = 0.5f;
    float unisonSpread = 0.6f;
    float phaseRandom = 1.0f;

    std::array<float, kMaxUnisonVoices> phase {};
    std::array<float, kMaxUnisonVoices> phaseOffsets {};
    std::array<float, kMaxUnisonVoices> increments {};
    std::array<float, kMaxUnisonVoices> detuneRatio {};
    std::array<float, kMaxUnisonVoices> voiceGain {};
    std::array<float, kMaxUnisonVoices> voicePanL {};
    std::array<float, kMaxUnisonVoices> voicePanR {};
    std::array<float, kMaxUnisonVoices> lastOutput {};

    bool  syncEnabled = false;
    float syncFrequency = 110.0f;
    float syncIncrement = 0.0f;
    float syncPhase = 0.0f;

    float blepBuffer[2] { 0.0f, 0.0f };

    Xorshift rng { 0x2545F491u };
};

//==============================================================================
/**
    Sub oscillatore.

    Separato dagli oscillatori principali e volutamente semplice: il suo
    compito è mettere energia sotto i 120 Hz senza aggiungere armoniche che
    competano con l'oscillatore principale. Ha tre forme (sinusoide, triangolo,
    quadra) e due ottave di scelta.

    La sinusoide è generata per ricorsione (oscillatore digitale a due stati)
    invece che con std::sin: costa due moltiplicazioni per campione ed è
    esattamente priva di armoniche, quindi non alias mai a nessuna frequenza.
*/
class SubOscillator
{
public:
    enum class Shape { Sine, Triangle, Square };

    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setFrequency (frequency);
        reset();
    }

    void reset() noexcept
    {
        phase = 0.0f;
        // Oscillatore a ricorsione inizializzato a fase zero.
        sinState = 0.0f;
        cosState = 1.0f;
    }

    void setShape (Shape s) noexcept { shape = s; }
    void setOctave (int oct) noexcept { octave = juce::jlimit (-2, 0, oct); }
    void setLevel (float g) noexcept { level = g; }

    void setFrequency (float hz) noexcept
    {
        frequency = std::clamp (hz * std::exp2 ((float) octave), 0.01f, (float) sampleRate * 0.49f);
        increment = frequency / (float) sampleRate;

        const float w = kTwoPi * increment;
        recCos = std::cos (w);
        recSin = std::sin (w);
    }

    float process() noexcept
    {
        float out = 0.0f;

        switch (shape)
        {
            case Shape::Sine:
            {
                // Rotazione di un vettore unitario: esatta a meno della
                // precisione in virgola mobile, ma il modulo deriva lentamente.
                // La rinormalizzazione periodica lo riporta a 1.
                const float newSin = sinState * recCos + cosState * recSin;
                const float newCos = cosState * recCos - sinState * recSin;
                sinState = newSin;
                cosState = newCos;

                if (++renormCounter >= 4096)
                {
                    renormCounter = 0;
                    const float mag = std::sqrt (sinState * sinState + cosState * cosState);
                    if (mag > 1.0e-6f)
                    {
                        sinState /= mag;
                        cosState /= mag;
                    }
                    else
                    {
                        sinState = 0.0f;
                        cosState = 1.0f;
                    }
                }

                out = sinState;
                break;
            }

            case Shape::Triangle:
            {
                // Triangolo diretto dalla fase: le sue armoniche cadono a
                // 1/n^2, quindi anche a 40 Hz l'aliasing resta sotto -80 dB
                // fino a Nyquist senza band-limiting esplicito.
                // in fase col seno (zero in salita a fase 0): sta sotto i layer
                out = 1.0f - 4.0f * std::abs (wrap01 (phase + 0.25f) - 0.5f);
                break;
            }

            case Shape::Square:
            {
                // La quadra usa polyBLEP: le sue armoniche cadono solo a 1/n e
                // senza correzione l'aliasing sarebbe udibile come ronzio.
                out = phase < 0.5f ? 1.0f : -1.0f;
                out += polyBlep (phase);
                out -= polyBlep (wrap01 (phase + 0.5f));
                break;
            }
        }

        phase = wrap01 (phase + increment);
        return out * level;
    }

private:
    float polyBlep (float t) const noexcept
    {
        if (increment <= 0.0f)
            return 0.0f;

        if (t < increment)
        {
            t /= increment;
            return t + t - t * t - 1.0f;
        }

        if (t > 1.0f - increment)
        {
            t = (t - 1.0f) / increment;
            return t * t + t + t + 1.0f;
        }

        return 0.0f;
    }

    double sampleRate = 44100.0;
    float frequency = 55.0f, increment = 0.0f, phase = 0.0f, level = 0.0f;
    float sinState = 0.0f, cosState = 1.0f, recSin = 0.0f, recCos = 1.0f;
    int   renormCounter = 0;
    int   octave = -1;
    Shape shape = Shape::Sine;
};

//==============================================================================
/**
    Generatore di rumore bianco / rosa.

    Il rosa usa il filtro di Voss-McCartney a tre poli di Paul Kellett: errore
    massimo 0.3 dB rispetto alla pendenza teorica di -3 dB/ottava fra 20 Hz e
    20 kHz, a costo di sei moltiplicazioni. La versione "economica" a un polo
    che si vede spesso sbaglia di 6 dB agli estremi e sul rumore usato come
    sorgente di modulazione questo si sente come uno sbilanciamento.
*/
class NoiseGenerator
{
public:
    enum class Colour { White, Pink };

    void reset() noexcept
    {
        b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0f;
    }

    void setSeed (uint32_t s) noexcept { rng.setSeed (s); }
    void setColour (Colour c) noexcept { colour = c; }
    void setLevel (float g) noexcept { level = g; }

    float process() noexcept
    {
        const float white = rng.nextBipolar();

        if (colour == Colour::White)
            return white * level;

        b0 = 0.99886f * b0 + white * 0.0555179f;
        b1 = 0.99332f * b1 + white * 0.0750759f;
        b2 = 0.96900f * b2 + white * 0.1538520f;
        b3 = 0.86650f * b3 + white * 0.3104856f;
        b4 = 0.55000f * b4 + white * 0.5329522f;
        b5 = -0.7616f * b5 - white * 0.0168980f;

        const float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
        b6 = white * 0.115926f;

        // 0.11 riporta il rosa allo stesso RMS del bianco.
        return pink * 0.11f * level;
    }

private:
    Xorshift rng { 0x1F123BB5u };
    Colour colour = Colour::White;
    float level = 0.0f;
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
};

} // namespace bassx
