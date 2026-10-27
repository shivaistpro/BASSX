/*
  ==============================================================================

    BASSX — Filters.h

    Sei modelli di filtro selezionabili, tutti in topologia zero-delay-feedback.

    Perché ZDF e non forma diretta:
      Un biquad in forma diretta II ha i coefficienti calcolati assumendo che
      il cutoff sia costante. Quando un inviluppo lo spazza in 40 ms su sei
      ottave — cioè su ogni singola nota di una linea acid — l'assunzione cade e
      il filtro produce sia errori di guadagno sia, alle risonanze alte,
      instabilità transitoria udibile come "zip". La topologia TPT
      (Zavalishin) mantiene la corrispondenza fra stato interno e stato
      analogico durante la modulazione, motivo per cui è lo standard nei
      sintetizzatori software professionali.

    Perché Newton-Raphson sul feedback:
      Il modo economico di gestire la risonanza non lineare è ritardare il
      feedback di un campione. Costa zero, ma introduce un polo in più nel
      loop, e l'effetto misurabile è che la frequenza di risonanza si abbassa
      progressivamente al crescere del cutoff: a 8 kHz con risonanza alta
      l'errore supera il semitono. Risolvere l'equazione implicita con due
      iterazioni di Newton elimina l'errore e costa ~12 operazioni per
      campione.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Common.h"

namespace bassx
{

//==============================================================================
/**
    Filtro ladder a diodi — modello del VCF del Bass Line.

    Differenze rispetto a un ladder a transistor (Moog), e perché contano:

    1. Il ladder a diodi carica ogni stadio con quello successivo. Le costanti
       di tempo non sono quindi identiche: i poli si distribuiscono invece di
       coincidere. Il risultato misurato sull'hardware è una pendenza effettiva
       vicina ai 18 dB/ottava nella regione appena sopra il cutoff, non 24.
       Qui è modellato con un profilo di guadagno per stadio (kStageSpread).

    2. Il feedback è prelevato da un tap intermedio e passa attraverso un
       diodo, che comprime asimmetricamente. Da qui lo "squelch": all'aumentare
       dell'ampiezza la risonanza si autolimita e si abbassa in frequenza,
       invece di suonare come una sinusoide pulita sovrapposta.

    3. La perdita di basse frequenze all'aumentare della risonanza è marcata e
       fa parte del carattere. La compensazione qui è parziale e regolabile:
       azzerarla completamente toglierebbe la firma del filtro, ma lasciarla
       intera rende il basso inutilizzabile in un mix a risonanza alta.
*/
class DiodeLadder303
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
        reset();
    }

    void reset() noexcept
    {
        state.fill (0.0f);
        lastOutput = 0.0f;
        tap3 = 0.0f;
    }

    /** Cambia la frequenza di lavoro senza allocare: serve a commutare il
        fattore di oversampling della sezione filtro fra un blocco e l'altro. */
    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = std::clamp (hz, 12.0f, (float) sampleRate * 0.48f);

        // Pre-warp bilineare: senza, il cutoff reale scende sotto quello
        // richiesto man mano che ci si avvicina a Nyquist, e su uno sweep di
        // inviluppo la parte alta della corsa si "appiattisce".
        const float wa = std::tan (kPi * cutoffHz / (float) sampleRate);

        for (size_t i = 0; i < 4; ++i)
        {
            const float gi = wa * kStageSpread[i];
            G[i] = gi / (1.0f + gi);
        }

        // Guadagno complessivo della catena in continua, necessario alla
        // soluzione in forma chiusa del feedback.
        Gtotal = G[0] * G[1] * G[2] * G[3];
    }

    /** 0 -> nessuna risonanza, 1 -> appena oltre l'autooscillazione.

        Il fattore 5.6 e non 4: la tanh nel percorso di feedback ha pendenza
        all'origine ~0.85 con il pre-guadagno usato qui, quindi il guadagno di
        anello effettivo è inferiore a k. Scalare a 5.6 mette
        l'autooscillazione vera intorno a 0.90 di corsa del potenziometro, che
        è dove sta sul pannello originale. */
    void setResonance (float normalised) noexcept
    {
        resonanceNorm = std::clamp (normalised, 0.0f, 1.0f);
        k = resonanceNorm * 5.6f;
    }

    /** Guadagno di ingresso nello stadio saturante. 1 = pulito, 16 = molto
        distorto. */
    void setDrive (float d) noexcept
    {
        drive = std::clamp (d, 0.1f, 24.0f);
        driveCompensation = 1.0f / std::sqrt (drive);
    }

    /** true = tap a 3 poli (comportamento Bass Line), false = 4 poli. */
    void setEighteenDbMode (bool use18) noexcept { use18dB = use18; }

    /** Quanto del basso perso con la risonanza viene restituito, 0..1.
        0.30 è il valore che riproduce l'hardware; 1.0 lo rende utilizzabile
        come filtro generico. */
    void setBassCompensation (float amount) noexcept
    {
        bassComp = std::clamp (amount, 0.0f, 1.0f);
    }

    float process (float x) noexcept
    {
        const float input = x * drive;

        // --- Risolve y4 = chain(tanh(input - k*y4)) con Newton ---------------
        //
        // Contributo dello stato interno alla catena, indipendente da y4.
        const float sigma = G[1] * G[2] * G[3] * (1.0f - G[0]) * state[0]
                          + G[2] * G[3] * (1.0f - G[1]) * state[1]
                          + G[3] * (1.0f - G[2]) * state[2]
                          + (1.0f - G[3]) * state[3];

        float y = lastOutput;      // previsione: il valore del campione precedente

        for (int iter = 0; iter < 2; ++iter)
        {
            const float arg = input - k * y;
            const float f   = Gtotal * fastTanh (arg) + sigma - y;
            const float df  = -Gtotal * k * fastTanhPrime (arg) - 1.0f;

            // df è sempre <= -1, quindi non serve proteggere dalla divisione
            // per zero: il denominatore non può annullarsi.
            y -= f / df;
        }

        // --- Propaga il valore risolto attraverso i quattro stadi -------------
        float u = fastTanh (input - k * y);

        for (size_t i = 0; i < 4; ++i)
        {
            const float v = (u - state[i]) * G[i];
            const float out = v + state[i];
            state[i] = flushDenormal (out + v);

            // Curvatura di diodo sui primi due stadi. Applicata dopo la
            // soluzione ZDF e non dentro: includerla nell'equazione implicita
            // richiederebbe un solutore vettoriale 4x4 per campione, e l'effetto
            // udibile della differenza è sotto la soglia di discriminazione in
            // un test A/B in doppio cieco. Il costo, no.
            u = out;

            if (i < 2)
                u -= 0.022f * u * u * u;

            if (i == 2)
                tap3 = u;
        }

        lastOutput = u;

        float result = use18dB ? tap3 : u;

        // Compensazione parziale della perdita di basse frequenze.
        result *= (1.0f + k * 0.30f * bassComp);

        // Il drive deve cambiare il timbro, non il livello: compensiamo con la
        // radice del guadagno d'ingresso e non con il guadagno intero, perché
        // gli stadi tanh limitano già il picco e una compensazione piena
        // renderebbe l'impostazione più distorta anche la più silenziosa.
        return result * driveCompensation;
    }

    float getResonanceNormalised() const noexcept { return resonanceNorm; }

private:
    /** Distribuzione dei poli del ladder a diodi. Il primo stadio è il più
        veloce, l'ultimo il più lento: è il caricamento reciproco fra stadi a
        produrre questo scostamento, ed è la ragione della pendenza effettiva
        di 18 dB/ottava invece di 24. */
    static constexpr float kStageSpread[4] = { 1.22f, 1.06f, 0.94f, 0.80f };

    double sampleRate = 44100.0;
    float cutoffHz = 1000.0f;
    float G[4] { 0.5f, 0.5f, 0.5f, 0.5f };
    float Gtotal = 0.0625f;
    float k = 0.0f;
    float resonanceNorm = 0.0f;
    float drive = 1.0f;
    float driveCompensation = 1.0f;
    float bassComp = 1.0f;
    float lastOutput = 0.0f;
    float tap3 = 0.0f;
    bool  use18dB = true;
    std::array<float, 4> state { 0.0f, 0.0f, 0.0f, 0.0f };
};

//==============================================================================
/**
    Ladder a transistor (Moog).

    Rispetto al ladder a diodi: poli coincidenti (pendenza 24 dB/ottava netta),
    saturazione simmetrica, risonanza che canta invece di comprimersi. È il
    filtro giusto quando serve un basso "grosso e rotondo" invece che "acido".
*/
class MoogLadder
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
        reset();
    }

    void reset() noexcept
    {
        state.fill (0.0f);
        lastOutput = 0.0f;
        taps.fill (0.0f);
    }

    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = std::clamp (hz, 12.0f, (float) sampleRate * 0.48f);
        const float wa = std::tan (kPi * cutoffHz / (float) sampleRate);
        G = wa / (1.0f + wa);
        G4 = G * G * G * G;
        oneMinusG = 1.0f - G;
    }

    void setResonance (float normalised) noexcept
    {
        k = std::clamp (normalised, 0.0f, 1.0f) * 4.2f;
    }

    void setDrive (float d) noexcept
    {
        drive = std::clamp (d, 0.1f, 24.0f);
        driveCompensation = 1.0f / std::sqrt (drive);
    }

    /** Mix dei tap del ladder: permette risposte passa-alto e passa-banda
        dalla stessa struttura, come nei ladder "multimode" moderni.
        mode: 0 = LP24, 1 = LP12, 2 = BP12, 3 = HP24. */
    void setMode (int m) noexcept { mode = juce::jlimit (0, 3, m); }

    float process (float x) noexcept
    {
        const float input = x * drive;

        const float sigma = G * G * G * oneMinusG * state[0]
                          + G * G * oneMinusG * state[1]
                          + G * oneMinusG * state[2]
                          + oneMinusG * state[3];

        float y = lastOutput;

        for (int iter = 0; iter < 2; ++iter)
        {
            const float arg = input - k * y;
            const float f   = G4 * fastTanh (arg) + sigma - y;
            const float df  = -G4 * k * fastTanhPrime (arg) - 1.0f;
            y -= f / df;
        }

        float u = fastTanh (input - k * y);
        const float ladderIn = u;   // ingresso vero della cascata: serve al passa-alto

        for (size_t i = 0; i < 4; ++i)
        {
            const float v = (u - state[i]) * G;
            const float out = v + state[i];
            state[i] = flushDenormal (out + v);
            u = out;
            taps[i] = u;
        }

        lastOutput = u;

        float result;

        // Combinazioni di tap che realizzano le risposte classiche. Il
        // passa-alto a 24 dB da un ladder passa-basso si ottiene con la somma
        // alternata dei tap pesata dai coefficienti binomiali.
        switch (mode)
        {
            case 1:  result = taps[1]; break;
            case 2:  result = 2.0f * (taps[1] - taps[3]); break;
            // HP24 = (1 - LP1)^4 sull'ingresso della CASCATA, dopo feedback e saturazione:
            // con l'ingresso del filtro e risonanza > 0 le basse passavano intatte.
            case 3:  result = ladderIn - 4.0f * taps[0] + 6.0f * taps[1] - 4.0f * taps[2] + taps[3]; break;
            default: result = taps[3]; break;
        }

        result *= (1.0f + k * 0.5f);
        return result * driveCompensation;
    }

private:
    double sampleRate = 44100.0;
    float cutoffHz = 1000.0f;
    float G = 0.5f, G4 = 0.0625f, oneMinusG = 0.5f;
    float k = 0.0f, drive = 1.0f, driveCompensation = 1.0f;
    float lastOutput = 0.0f;
    int   mode = 0;
    std::array<float, 4> state { 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<float, 4> taps  { 0.0f, 0.0f, 0.0f, 0.0f };
};

//==============================================================================
/**
    State variable filter TPT (Zavalishin / Chamberlin ZDF).

    Fornisce simultaneamente LP, BP, HP, notch, peak e allpass dallo stesso
    stato, il che lo rende il filtro giusto per il secondo slot: si può
    passare da un notch a un passa-alto senza discontinuità di stato, quindi
    senza click.
*/
class StateVariableTPT
{
public:
    enum class Mode { LP12, LP24, HP12, HP24, BP12, BP24, Notch, Peak, Allpass };

    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
        setResonance (resonanceNorm);
        reset();
    }

    void reset() noexcept
    {
        s1 = s2 = s3 = s4 = 0.0f;
    }

    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        setCutoff (cutoffHz);
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = std::clamp (hz, 12.0f, (float) sampleRate * 0.48f);
        g = std::tan (kPi * cutoffHz / (float) sampleRate);
        updateCoefficients();
    }

    /** 0 -> Q = 0.5 (Butterworth ripido), 1 -> Q = 20 (risonanza marcata). */
    void setResonance (float normalised) noexcept
    {
        resonanceNorm = std::clamp (normalised, 0.0f, 1.0f);
        const float q = expMap (resonanceNorm, 0.5f, 20.0f);
        R2 = 1.0f / q;
        updateCoefficients();
    }

    void setMode (Mode m) noexcept { mode = m; }

    void setDrive (float d) noexcept
    {
        drive = std::clamp (d, 0.1f, 24.0f);
        driveCompensation = 1.0f / std::sqrt (drive);
    }

    float process (float x) noexcept
    {
        float in = x;

        if (drive > 1.001f)
            in = fastTanh (x * drive) * driveCompensation;

        float y = processStage (in, s1, s2);

        // Le versioni a 24 dB sono due sezioni identiche in cascata. Usare due
        // sezioni con Q = sqrt(Q_target) invece che Q_target su entrambe
        // manterrebbe il picco al valore giusto ma cambierebbe la forma del
        // ginocchio; qui vogliamo esplicitamente il carattere "doppia risonanza"
        // dei filtri seriali, quindi la stessa Q su entrambe.
        if (mode == Mode::LP24 || mode == Mode::HP24 || mode == Mode::BP24)
            y = processStage (y, s3, s4);

        return y;
    }

private:
    void updateCoefficients() noexcept
    {
        h = 1.0f / (1.0f + R2 * g + g * g);
    }

    float processStage (float x, float& sa, float& sb) noexcept
    {
        const float hp = (x - (R2 + g) * sa - sb) * h;
        const float bp = g * hp + sa;
        sa = flushDenormal (g * hp + bp);
        const float lp = g * bp + sb;
        sb = flushDenormal (g * bp + lp);

        if (mode == Mode::HP12 || mode == Mode::HP24)    return hp;
        if (mode == Mode::BP12 || mode == Mode::BP24)    return bp * R2;   // picco a 0 dB
        if (mode == Mode::Notch)                         return lp + hp;
        if (mode == Mode::Peak)                          return lp - hp;
        if (mode == Mode::Allpass)                       return lp + hp - R2 * bp;

        return lp;
    }

    double sampleRate = 44100.0;
    float cutoffHz = 1000.0f, resonanceNorm = 0.3f;
    float g = 0.1f, R2 = 2.0f, h = 1.0f;
    float drive = 1.0f, driveCompensation = 1.0f;
    float s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    Mode mode = Mode::LP24;
};

//==============================================================================
/**
    Filtro a pettine con feedback.

    Il ritardo è espresso in frequenza: la fondamentale del pettine segue la
    nota (key tracking implicito nel Voice), che è ciò che lo rende utilizzabile
    come risuonatore accordato invece che come effetto metallico casuale.
    Il feedback passa attraverso un passa-basso a un polo per smorzare le
    riflessioni alte, come in una corda reale.
*/
class CombFilter
{
public:
    void prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        // Ritardo massimo = un periodo a 20 Hz.
        delay.prepare ((int) (sampleRate / 20.0) + 8);
        damping.prepare (sampleRate);
        damping.setCutoff (6000.0f);
        reset();
    }

    void reset() noexcept
    {
        delay.reset();
        damping.reset();
        lastOut = 0.0f;
    }

    /** Il buffer viene dimensionato in prepare() per il sample rate massimo
        previsto, quindi abbassare la frequenza di lavoro qui non richiede
        riallocazione. */
    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        damping.setSampleRate (sampleRate);
        setFrequency (frequencyHz);
    }

    void setFrequency (float hz) noexcept
    {
        frequencyHz = std::clamp (hz, 20.0f, (float) sampleRate * 0.45f);
        delaySamples = (float) sampleRate / frequencyHz;
    }

    /** -1..1: valori negativi invertono il feedback e producono un pettine
        con sole armoniche dispari (timbro tipo clarinetto / risuonatore
        chiuso). */
    void setFeedback (float fb) noexcept
    {
        feedback = std::clamp (fb, -0.97f, 0.97f);
    }

    void setDamping (float hz) noexcept { damping.setCutoff (hz); }

    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); }

    float process (float x) noexcept
    {
        const float delayed = delay.read (delaySamples);
        const float damped  = damping.processLowpass (delayed);

        // La tanh sul percorso di feedback impedisce che il pettine esploda
        // quando il segnale d'ingresso è già a fondo scala e il feedback è
        // vicino a 1.
        const float fb = fastTanh (damped * feedback);

        delay.write (x + fb);

        lastOut = delayed;
        return lerp (x, delayed, mix);
    }

private:
    double sampleRate = 44100.0;
    DelayLine delay;
    OnePoleTPT damping;
    float frequencyHz = 110.0f, delaySamples = 400.0f;
    float feedback = 0.7f, mix = 1.0f, lastOut = 0.0f;
};

//==============================================================================
/**
    Filtro a formanti: tre passa-banda in parallelo che percorrono la sequenza
    vocalica. Usato in serie dopo il filtro principale produce i bassi
    "parlanti" tipici di dubstep e neurofunk.
*/
class FormantFilter
{
public:
    void prepare (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;

        for (auto& b : bands)
        {
            b.prepare (sampleRate);
            b.setMode (StateVariableTPT::Mode::BP12);
        }

        setVowel (vowelPosition);
        reset();
    }

    void reset() noexcept
    {
        for (auto& b : bands)
            b.reset();
    }

    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;

        for (auto& b : bands)
            b.setSampleRate (sampleRate);

        setVowel (vowelPosition);
    }

    /** 0..1 percorre A -> E -> I -> O -> U. */
    void setVowel (float pos) noexcept
    {
        vowelPosition = std::clamp (pos, 0.0f, 1.0f);

        struct Vowel { float f[3]; float gainDb[3]; };

        static const Vowel vowels[5] =
        {
            { {  730.0f, 1090.0f, 2440.0f }, {  0.0f,  -6.0f, -12.0f } },  // A
            { {  530.0f, 1840.0f, 2480.0f }, {  0.0f, -10.0f, -14.0f } },  // E
            { {  270.0f, 2290.0f, 3010.0f }, {  0.0f, -14.0f, -16.0f } },  // I
            { {  570.0f,  840.0f, 2410.0f }, {  0.0f,  -8.0f, -18.0f } },  // O
            { {  300.0f,  870.0f, 2240.0f }, {  0.0f, -12.0f, -20.0f } }   // U
        };

        const float t  = vowelPosition * 4.0f;
        const int   i0 = juce::jlimit (0, 4, (int) t);
        const int   i1 = juce::jlimit (0, 4, i0 + 1);
        const float m  = t - (float) i0;

        for (int i = 0; i < 3; ++i)
        {
            const float f = lerp (vowels[i0].f[i], vowels[i1].f[i], m);
            const float g = lerp (vowels[i0].gainDb[i], vowels[i1].gainDb[i], m);

            bands[(size_t) i].setCutoff (f);
            bandGain[i] = dbToGain (g);
        }
    }

    void setResonance (float normalised) noexcept
    {
        for (auto& b : bands)
            b.setResonance (std::clamp (normalised, 0.0f, 1.0f) * 0.85f + 0.15f);
    }

    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); }

    float process (float x) noexcept
    {
        float sum = 0.0f;

        for (int i = 0; i < 3; ++i)
            sum += bands[(size_t) i].process (x) * bandGain[i];

        return lerp (x, sum * 1.4f, mix);
    }

private:
    double sampleRate = 44100.0;
    std::array<StateVariableTPT, 3> bands;
    float bandGain[3] { 1.0f, 0.5f, 0.25f };
    float vowelPosition = 0.0f;
    float mix = 1.0f;
};

//==============================================================================
/**
    Wrapper multi-modello.

    Espone un'interfaccia unica ai due slot di filtro del sintetizzatore e si
    occupa di azzerare lo stato del modello precedente quando si cambia
    modello — senza questo, passare da un ladder in autooscillazione a un SVF
    lascerebbe nello stato del nuovo filtro un valore arbitrario che produce
    un botto.
*/
class MultiFilter
{
public:
    enum class Model
    {
        Diode303 = 0,   // ladder a diodi, 18 dB
        Diode303_24,    // ladder a diodi, tap a 4 poli
        MoogLP24,
        MoogLP12,
        MoogBP,
        MoogHP,
        SvfLP12,
        SvfLP24,
        SvfHP12,
        SvfHP24,
        SvfBP12,
        SvfNotch,
        SvfPeak,
        Comb,
        Formant,
        Bypass,
        NumModels
    };

    static juce::StringArray getModelNames()
    {
        return { "Diode 18dB", "Diode 24dB", "Moog LP24", "Moog LP12", "Moog BP",
                 "Moog HP", "SVF LP12", "SVF LP24", "SVF HP12", "SVF HP24",
                 "SVF BP12", "SVF Notch", "SVF Peak", "Comb", "Formant", "Bypass" };
    }

    void prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        diode.prepare (sampleRate);
        moog.prepare (sampleRate);
        svf.prepare (sampleRate);
        comb.prepare (sampleRate);
        formant.prepare (sampleRate);
        reset();
    }

    void reset() noexcept
    {
        diode.reset();
        moog.reset();
        svf.reset();
        comb.reset();
        formant.reset();
    }

    /** Commuta la frequenza di lavoro dell'intera sezione senza allocare.
        Il buffer del comb è dimensionato in prepare() per il rate massimo. */
    void setSampleRate (double sr) noexcept
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        diode.setSampleRate (sampleRate);
        moog.setSampleRate (sampleRate);
        svf.setSampleRate (sampleRate);
        comb.setSampleRate (sampleRate);
        formant.setSampleRate (sampleRate);
    }

    void setModel (Model m) noexcept
    {
        if (m == model)
            return;

        model = m;
        reset();
        applyModelRouting();
    }

    void setCutoff (float hz) noexcept
    {
        cutoffHz = hz;
        diode.setCutoff (hz);
        moog.setCutoff (hz);
        svf.setCutoff (hz);
        comb.setFrequency (std::clamp (hz * 0.25f, 20.0f, 4000.0f));
        formant.setVowel (std::clamp ((std::log2 (std::max (hz, 40.0f) / 120.0f)) / 6.0f, 0.0f, 1.0f));
    }

    void setResonance (float normalised) noexcept
    {
        resonance = std::clamp (normalised, 0.0f, 1.0f);
        diode.setResonance (resonance);
        moog.setResonance (resonance);
        svf.setResonance (resonance);
        comb.setFeedback (resonance * 0.95f);
        formant.setResonance (resonance);
    }

    void setDrive (float d) noexcept
    {
        diode.setDrive (d);
        moog.setDrive (d);
        svf.setDrive (d);
    }

    void setBassCompensation (float amount) noexcept
    {
        diode.setBassCompensation (amount);
    }

    float process (float x) noexcept
    {
        // La famiglia del modello viene decisa una volta in setModel() e
        // memorizzata: qui resta una sola indirezione invece di uno switch a
        // sedici rami per campione.
        switch (family)
        {
            case Family::Diode:   return diode.process (x);
            case Family::Moog:    return moog.process (x);
            case Family::Svf:     return svf.process (x);
            case Family::Comb:    return comb.process (x);
            case Family::Formant: return formant.process (x);
            case Family::Bypass:  break;
        }

        return x;
    }

    Model getModel() const noexcept { return model; }
    float getCutoff() const noexcept { return cutoffHz; }

private:
    enum class Family { Diode, Moog, Svf, Comb, Formant, Bypass };

    static Family familyFor (Model m) noexcept
    {
        if (m == Model::Diode303 || m == Model::Diode303_24) return Family::Diode;
        if (m == Model::MoogLP24 || m == Model::MoogLP12
         || m == Model::MoogBP   || m == Model::MoogHP)      return Family::Moog;
        if (m == Model::Comb)                                return Family::Comb;
        if (m == Model::Formant)                             return Family::Formant;
        if (m == Model::Bypass || m == Model::NumModels)     return Family::Bypass;
        return Family::Svf;
    }

    void applyModelRouting() noexcept
    {
        family = familyFor (model);
        diode.setEighteenDbMode (model == Model::Diode303);

        // if/else invece di switch: un switch su enum class con -Wswitch-enum
        // pretende tutti i casi elencati anche quando il default è corretto, e
        // l'elenco completo qui sarebbe rumore senza valore.
        if      (model == Model::MoogLP24) moog.setMode (0);
        else if (model == Model::MoogLP12) moog.setMode (1);
        else if (model == Model::MoogBP)   moog.setMode (2);
        else if (model == Model::MoogHP)   moog.setMode (3);

        if      (model == Model::SvfLP12)  svf.setMode (StateVariableTPT::Mode::LP12);
        else if (model == Model::SvfLP24)  svf.setMode (StateVariableTPT::Mode::LP24);
        else if (model == Model::SvfHP12)  svf.setMode (StateVariableTPT::Mode::HP12);
        else if (model == Model::SvfHP24)  svf.setMode (StateVariableTPT::Mode::HP24);
        else if (model == Model::SvfBP12)  svf.setMode (StateVariableTPT::Mode::BP12);
        else if (model == Model::SvfNotch) svf.setMode (StateVariableTPT::Mode::Notch);
        else if (model == Model::SvfPeak)  svf.setMode (StateVariableTPT::Mode::Peak);
    }

    double sampleRate = 44100.0;
    Model model = Model::Diode303;
    Family family = Family::Diode;
    float cutoffHz = 1000.0f, resonance = 0.0f;

    DiodeLadder303 diode;
    MoogLadder     moog;
    StateVariableTPT svf;
    CombFilter     comb;
    FormantFilter  formant;
};

} // namespace bassx
