/*
  ==============================================================================

    BASSX — Effects.h

    Catena di effetti stereo post-voce.

    Ordine della catena e perché è questo:

      Distorsione -> EQ -> Chorus -> Phaser -> Delay -> Reverb -> Compressore -> Limiter

    * La distorsione sta per prima perché deve lavorare sul segnale asciutto:
      distorcere un segnale già riverberato impasta la coda e produce
      intermodulazione fra riflessioni scorrelate, che percettivamente è
      rumore.
    * L'EQ sta subito dopo per domare le frequenze che la distorsione ha
      generato, prima che gli effetti a tempo le moltiplichino.
    * Il compressore sta in fondo, dopo gli effetti a tempo, perché su un basso
      il suo scopo è controllare il livello di ciò che esce, non di ciò che
      entra nel riverbero.
    * Il limiter è l'ultimo stadio e ha lookahead: senza, dovrebbe reagire dopo
      che il picco è già passato, e l'unico modo di rispettare il tetto sarebbe
      distorcere.

  ==============================================================================
*/

#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Common.h"

namespace bassx
{

//==============================================================================
/**
    Distorsione multi-algoritmo con oversampling interno 4x.

    L'oversampling non è opzionale: ogni non linearità genera armoniche fino a
    infinito, e tutto ciò che supera Nyquist torna indietro come componente
    inarmonica. Su un basso a 55 Hz con distorsione spinta, l'aliasing senza
    oversampling è di 20-30 dB sopra la soglia di udibilità e suona come un
    ronzio metallico che si muove in senso contrario alla melodia.
*/
class Distortion
{
public:
    enum class Algorithm
    {
        SoftClip = 0,
        Tube,
        HardClip,
        Foldback,
        WaveShaper,
        BitCrush,
        Diode,
        Fuzz,
        NumAlgorithms
    };

    static juce::StringArray getAlgorithmNames()
    {
        return { "Soft Clip", "Tube", "Hard Clip", "Foldback",
                 "Wave Shaper", "Bit Crush", "Diode", "Fuzz" };
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        // FIR a fase lineare con latenza intera: il segnale asciutto può
        // essere ritardato esattamente della stessa quantità. Con l'IIR
        // polifase la latenza è frazionaria e la fase non lineare, quindi il
        // mix parallelo dry/wet produceva un filtro a pettine.
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);

        oversampler->initProcessing ((size_t) spec.maximumBlockSize);
        oversampler->reset();

        // Il filtro anti-alias dell'oversampler introduce latenza. Va
        // dichiarata all'host, altrimenti la traccia risulta in anticipo
        // rispetto alle altre. Il valore viene letto dal processore.
        latencySamples = (int) std::lround (oversampler->getLatencyInSamples());

        for (auto& d : dryDelay)
            d.prepare (latencySamples + 4);

        dryBuffer.setSize (2, (int) spec.maximumBlockSize, false, true, false);

        for (auto& d : dcBlock)
            d.prepare (sampleRate * 4.0);

        for (auto& t : toneFilter)
        {
            t.prepare (sampleRate * 4.0);
            t.setCutoff (toneHz);
        }

        driveSmoother.prepare (sampleRate * 4.0, 20.0f);   // gira dentro l'oversampling 4x
        mixSmoother.prepare (sampleRate, 20.0f);

        reset();
    }

    void reset() noexcept
    {
        if (oversampler != nullptr)
            oversampler->reset();

        for (auto& d : dcBlock) d.reset();
        for (auto& t : toneFilter) t.reset();
        for (auto& d : dryDelay) d.reset();

        holdCounter = 0;
        heldSample[0] = heldSample[1] = 0.0f;
        driveSmoother.snap (driveNorm);
        mixSmoother.snap (mix);
    }

    void setAlgorithm (Algorithm a) noexcept { algorithm = a; }

    /** 0..1 mappato su 1..40 di guadagno d'ingresso. */
    void setDrive (float normalised) noexcept
    {
        driveNorm = std::clamp (normalised, 0.0f, 1.0f);
        driveSmoother.setTarget (driveNorm);
        driveGain = expMap (driveNorm, 1.0f, 40.0f);
        // Compensazione: la distorsione deve cambiare il timbro a livello
        // costante, altrimenti l'utente confonde "più forte" con "meglio".
        outputCompensation = 1.0f / std::pow (driveGain, 0.62f);
    }

    /** Filtro passa-basso dopo la non linearità, 200 Hz .. 20 kHz. */
    void setTone (float hz) noexcept
    {
        toneHz = std::clamp (hz, 200.0f, 20000.0f);
        for (auto& t : toneFilter)
            t.setCutoff (std::min (toneHz, (float) sampleRate * 1.9f));
    }

    void setBits (float b) noexcept { bits = std::clamp (b, 1.0f, 16.0f); }

    /** Riduzione di frequenza di campionamento, 1 = nessuna. */
    void setDownsample (float factor) noexcept { downsample = std::clamp (factor, 1.0f, 64.0f); }

    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }

    int getLatencySamples() const noexcept { return latencySamples; }

    //==========================================================================
    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (oversampler == nullptr)
            return;

        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        // Copia del segnale asciutto per il mix parallelo, ritardata della
        // latenza dell'oversampler. Su un basso il mix parallelo è quasi sempre
        // preferibile al seriale: mantiene il peso in basso del segnale pulito
        // e ci sovrappone il contenuto armonico.
        dryBuffer.setSize (2, numSamples, false, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* src = block.getChannelPointer ((size_t) ch);
            auto* dst = dryBuffer.getWritePointer (ch);
            auto& line = dryDelay[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                line.write (src[i]);
                dst[i] = line.read ((float) latencySamples + 1.0f);
            }
        }

        // Spenta: passa il segnale asciutto ritardato. La latenza dichiarata
        // all'host resta la stessa, quindi la traccia non si sposta quando la
        // distorsione viene accesa o spenta.
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            mixSmoother.snap (0.0f);
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::copy (block.getChannelPointer ((size_t) ch),
                                                   dryBuffer.getReadPointer (ch), numSamples);
            return;
        }

        auto up = oversampler->processSamplesUp (block);

        const int upSamples = (int) up.getNumSamples();

        for (int ch = 0; ch < (int) up.getNumChannels(); ++ch)
        {
            auto* data = up.getChannelPointer ((size_t) ch);
            auto& dc = dcBlock[(size_t) juce::jmin (ch, 1)];
            auto& tone = toneFilter[(size_t) juce::jmin (ch, 1)];

            // DRIVE in rampa (anti-zipper): gain 40^d e compensazione
            // 40^(-0.62 d) seguono il valore smussato campione per campione.
            // Lo smoother avanza solo sul canale 0, il canale 1 rilegge la
            // stessa rampa: i due lati restano identici.
            for (int i = 0; i < upSamples; ++i)
            {
                float d;
                if (ch == 0) { d = driveSmoother.next(); if (i < kMaxRamp) driveRampBuf[(size_t) i] = d; }
                else         d = (i < kMaxRamp) ? driveRampBuf[(size_t) i] : driveSmoother.getCurrent();

                const float gain = std::exp2 (d * kLog2Drive);
                const float comp = std::exp2 (-0.62f * d * kLog2Drive);

                float x = data[i] * gain;

                x = shape (x, ch);
                x = dc.process (x);
                x = tone.processLowpass (x);

                data[i] = x * comp;
            }
        }

        oversampler->processSamplesDown (block);

        for (int i = 0; i < numSamples; ++i)
        {
            const float m = mixSmoother.next();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* wet = block.getChannelPointer ((size_t) ch);
                wet[i] = lerp (dryBuffer.getReadPointer (ch)[i], wet[i], m);
            }
        }
    }

private:
    //==========================================================================
    float shape (float x, int channel) noexcept
    {
        switch (algorithm)
        {
            case Algorithm::SoftClip:
                return softClip (x) * 1.5f;

            case Algorithm::Tube:
                return tubeSaturate (x, 0.22f) * 1.2f;

            case Algorithm::HardClip:
                return std::clamp (x, -1.0f, 1.0f);

            case Algorithm::Foldback:
                return foldback (x, 0.7f);

            case Algorithm::WaveShaper:
                return waveFold (std::clamp (x * 0.5f, -1.0f, 1.0f), 0.75f);

            case Algorithm::BitCrush:
            {
                // Sample & hold + quantizzazione. Fatto qui, dentro
                // l'oversampling, l'aliasing del sample & hold viene filtrato
                // dal decimatore invece di finire nel segnale.
                const int idx = juce::jmin (channel, 1);

                if (holdCounter <= 0)
                {
                    heldSample[(size_t) idx] = bitCrush (std::clamp (x, -1.0f, 1.0f), bits);

                    if (idx == 1 || downsample <= 1.0f)
                        holdCounter = (int) downsample;
                }

                if (idx == 1)
                    --holdCounter;

                return heldSample[(size_t) idx];
            }

            case Algorithm::Diode:
            {
                // Coppia di diodi antiparallelo: soglia di conduzione, sopra la
                // quale la curva è esponenziale. Sotto soglia il segnale passa
                // quasi inalterato, e questo è ciò che rende il diode clipping
                // percettivamente "trasparente sui passaggi piano e feroce sui
                // picchi".
                constexpr float vt = 0.35f;
                const float s = (x < 0.0f) ? -1.0f : 1.0f;
                const float a = std::abs (x);
                return s * (a < vt ? a : vt + (1.0f - std::exp (-(a - vt) * 2.4f)) * 0.65f);
            }

            case Algorithm::Fuzz:
            {
                // Asimmetria forte + compressione: la curva di un fuzz a
                // transistor in saturazione profonda.
                const float y = fastTanh (x * 1.6f + 0.35f) - fastTanh (0.35f);
                return y * 1.35f - 0.08f * y * y;
            }

            case Algorithm::NumAlgorithms:
            default:
                return x;
        }
    }

    //==========================================================================
    double sampleRate = 44100.0;
    Algorithm algorithm = Algorithm::Tube;
    float driveGain = 1.0f, outputCompensation = 1.0f, driveNorm = 0.0f;
    ParamSmoother driveSmoother, mixSmoother;            // anti-zipper, tau 20 ms
    static constexpr int kMaxRamp = 8192;
    std::array<float, kMaxRamp> driveRampBuf {};
    static inline const float kLog2Drive = std::log2 (40.0f);
    float toneHz = 12000.0f, mix = 1.0f;
    float bits = 12.0f, downsample = 1.0f;
    int   holdCounter = 0;
    int   latencySamples = 0;
    float heldSample[2] { 0.0f, 0.0f };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    std::array<DCBlocker, 2> dcBlock;
    std::array<OnePoleTPT, 2> toneFilter;
    std::array<DelayLine, 2> dryDelay;
    juce::AudioBuffer<float> dryBuffer;
};

//==============================================================================
/**
    Chorus a tre voci per canale.

    Le voci hanno LFO sfasati di 120° e frequenze leggermente diverse (rapporti
    1 : 1.13 : 0.87). Usare la stessa frequenza per tutte produce un movimento
    coerente che l'orecchio riconosce come modulazione periodica invece che
    come allargamento. È l'errore che rende "economico" un chorus.

    Il ritardo base è di 12 ms: sotto i 5 ms l'effetto diventa flanging (le
    cancellazioni cadono in banda audio e colorano il timbro), sopra i 30 ms si
    sente come eco distinto.
*/
class Chorus
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        mixSmoother.prepare (sampleRate, 20.0f);

        const int maxDelay = (int) (sampleRate * 0.06) + 8;

        for (auto& ch : lines)
            for (auto& v : ch)
                v.prepare (maxDelay);

        for (auto& f : highpass)
        {
            f.prepare (sampleRate);
            // Sotto i 120 Hz il chorus va tenuto fuori: modulare la fase delle
            // basse frequenze di un basso lo fa "annaspare" e in mono produce
            // cancellazioni sulla fondamentale.
            f.setCutoff (120.0f);
        }

        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : lines)
            for (auto& v : ch)
                v.reset();

        for (auto& f : highpass)
            f.reset();

        for (int i = 0; i < 3; ++i)
            lfoPhase[i] = (float) i / 3.0f;

        mixSmoother.snap (mix);
        idle = mix <= 0.0001f;
    }

    /** Fuori catena o spento: le linee non scorrono, quindi alla prossima
        accensione vanno svuotate (niente audio vecchio che riaffiora). */
    void markIdle() noexcept { idle = true; mixSmoother.snap (0.0f); }

    void setRate (float hz) noexcept { rateHz = std::clamp (hz, 0.01f, 10.0f); }
    void setDepth (float d) noexcept { depth = std::clamp (d, 0.0f, 1.0f); }
    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setSpread (float s) noexcept { spread = std::clamp (s, 0.0f, 1.0f); }
    void setVoices (int v) noexcept { numVoices = juce::jlimit (1, 3, v); }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        // Spento o fuori catena: niente lavoro. Alla riaccensione le linee
        // vanno svuotate, altrimenti riaffiora l'audio di prima (il "loop").
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            mixSmoother.snap (0.0f);
            idle = true;
            return;
        }

        if (idle)
        {
            clearTails();
            idle = false;
        }

        const int numSamples  = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        const float baseDelay = 0.012f * (float) sampleRate;
        const float modDepth  = depth * 0.006f * (float) sampleRate;
        const float inc       = rateHz / (float) sampleRate;

        static constexpr float rateRatio[3] = { 1.0f, 1.13f, 0.87f };

        for (int i = 0; i < numSamples; ++i)
        {
            float lfoValue[3];

            for (int v = 0; v < 3; ++v)
            {
                lfoValue[v] = std::sin (kTwoPi * lfoPhase[v]);
                lfoPhase[v] = wrap01 (lfoPhase[v] + inc * rateRatio[v]);
            }

            const float m = mixSmoother.next();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = block.getChannelPointer ((size_t) ch);
                const float dry = data[i];

                // Il passa-alto è nel percorso wet soltanto: il segnale
                // asciutto deve conservare tutto il basso.
                const float wetIn = highpass[(size_t) ch].processHighpass (dry);

                float wet = 0.0f;

                for (int v = 0; v < numVoices; ++v)
                {
                    // I due canali leggono l'LFO in controfase secondo lo
                    // spread: è ciò che produce l'allargamento stereo senza
                    // rendere il segnale incompatibile in mono.
                    const float polarity = (ch == 0) ? 1.0f : lerp (1.0f, -1.0f, spread);
                    const float d = baseDelay + modDepth * lfoValue[v] * polarity;

                    lines[(size_t) ch][(size_t) v].write (wetIn);
                    wet += lines[(size_t) ch][(size_t) v].read (d);
                }

                wet /= (float) numVoices;

                data[i] = lerp (dry, dry * 0.7f + wet, m);
            }
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<std::array<DelayLine, 3>, 2> lines;
    std::array<OnePoleTPT, 2> highpass;
    float lfoPhase[3] { 0.0f, 0.333f, 0.667f };
    float rateHz = 0.6f, depth = 0.5f, mix = 0.0f, spread = 0.7f;
    ParamSmoother mixSmoother;
    bool idle = true;

    void clearTails() noexcept
    {
        for (auto& ch : lines)
            for (auto& v : ch)
                v.reset();
        for (auto& f : highpass)
            f.reset();
    }
    int numVoices = 2;
};

//==============================================================================
/**
    Phaser a 4/6/8/12 stadi allpass.

    Il feedback è prelevato dopo l'ultimo stadio e reimmesso all'ingresso: è ciò
    che trasforma le notch da larghe e morbide a strette e risonanti. Senza
    feedback un phaser suona come un filtro che si muove; con feedback suona
    come un phaser.
*/
class Phaser
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        mixSmoother.prepare (sampleRate, 20.0f);

        for (auto& ch : stages)
            for (auto& s : ch)
                s.prepare (sampleRate);

        reset();
    }

    void reset() noexcept
    {
        for (auto& ch : stages)
            for (auto& s : ch)
                s.reset();

        feedbackState[0] = feedbackState[1] = 0.0f;
        lfoPhase = 0.0f;
        mixSmoother.snap (mix);
        idle = mix <= 0.0001f;
    }

    /** Fuori catena: le code non scorrono, alla riaccensione si svuotano. */
    void markIdle() noexcept { idle = true; mixSmoother.snap (0.0f); }

    void setRate (float hz) noexcept { rateHz = std::clamp (hz, 0.01f, 10.0f); }
    void setDepth (float d) noexcept { depth = std::clamp (d, 0.0f, 1.0f); }
    void setCentreFrequency (float hz) noexcept { centreHz = std::clamp (hz, 60.0f, 8000.0f); }
    void setFeedback (float fb) noexcept { feedback = std::clamp (fb, -0.95f, 0.95f); }
    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setStereoPhase (float p) noexcept { stereoPhase = std::clamp (p, 0.0f, 0.5f); }

    void setNumStages (int n) noexcept
    {
        numStages = juce::jlimit (2, kMaxStages, (n / 2) * 2);
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        // Spento o fuori catena: niente lavoro. Alla riaccensione le code
        // vanno svuotate, altrimenti riaffiora l'audio di prima (il "loop").
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            mixSmoother.snap (0.0f);
            idle = true;
            return;
        }

        if (idle)
        {
            clearTails();
            idle = false;
        }

        const int numSamples  = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());
        const float inc = rateHz / (float) sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const float m = mixSmoother.next();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float phaseForChannel = wrap01 (lfoPhase + (ch == 1 ? stereoPhase : 0.0f));
                const float lfo = std::sin (kTwoPi * phaseForChannel);

                // Lo sweep è esponenziale: le notch devono muoversi di un
                // numero costante di ottave, non di Hz, altrimenti in basso
                // non si muovono e in alto sfrecciano.
                const float f = centreHz * std::exp2 (lfo * depth * 2.2f);

                auto* data = block.getChannelPointer ((size_t) ch);
                const float dry = data[i];

                float x = dry + feedbackState[(size_t) ch] * feedback;

                for (int s = 0; s < numStages; ++s)
                {
                    // Gli stadi sono scalati in frequenza fra loro: stadi tutti
                    // alla stessa frequenza darebbero notch coincidenti, cioè
                    // una singola notch profonda invece del pettine
                    // caratteristico.
                    auto& stage = stages[(size_t) ch][(size_t) s];
                    stage.setFrequency (std::clamp (f * (1.0f + 0.22f * (float) s),
                                                    20.0f, (float) sampleRate * 0.48f));
                    x = stage.process (x);
                }

                feedbackState[(size_t) ch] = flushDenormal (x);

                data[i] = lerp (dry, (dry + x) * 0.7f, m);
            }

            lfoPhase = wrap01 (lfoPhase + inc);
        }
    }

private:
    static constexpr int kMaxStages = 12;

    double sampleRate = 44100.0;
    std::array<std::array<AllpassTPT, kMaxStages>, 2> stages;
    float feedbackState[2] { 0.0f, 0.0f };
    float lfoPhase = 0.0f;
    float rateHz = 0.35f, depth = 0.7f, centreHz = 600.0f;
    float feedback = 0.5f, mix = 0.0f, stereoPhase = 0.25f;
    int numStages = 6;
    ParamSmoother mixSmoother;
    bool idle = true;

    void clearTails() noexcept
    {
        for (auto& ch : stages)
            for (auto& st : ch)
                st.reset();
        feedbackState[0] = feedbackState[1] = 0.0f;
    }
};

//==============================================================================
/**
    Delay stereo con sincronizzazione al tempo, ping-pong e filtro nel loop.

    Il filtro passa-alto nel loop di feedback è la ragione per cui questo delay
    è utilizzabile su un basso: senza, ogni ripetizione riporta la fondamentale
    a pieno livello e dopo tre echi il basso è impastato. Con il passa-alto le
    ripetizioni si assottigliano e lasciano spazio alla nota nuova.
*/
class Delay
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        const int maxSamples = (int) (sampleRate * 4.0) + 8;   // 4 secondi

        for (auto& l : lines)
            l.prepare (maxSamples);

        for (auto& f : loopLowpass)  { f.prepare (sampleRate); f.setCutoff (6000.0f); }
        for (auto& f : loopHighpass) { f.prepare (sampleRate); f.setCutoff (220.0f); }

        for (auto& s : timeSmoother)
            s.prepare (sampleRate, 60.0f);
        mixSmoother.prepare (sampleRate, 20.0f);
        fbSmoother.prepare (sampleRate, 20.0f);

        reset();
    }

    void reset() noexcept
    {
        for (auto& l : lines) l.reset();
        for (auto& f : loopLowpass) f.reset();
        for (auto& f : loopHighpass) f.reset();
        feedbackState[0] = feedbackState[1] = 0.0f;

        for (int i = 0; i < 2; ++i)
            timeSmoother[(size_t) i].snap (delaySamples[i]);

        mixSmoother.snap (mix);
        fbSmoother.snap (feedback);
        idle = mix <= 0.0001f;
    }

    /** Fuori catena: le code non scorrono, alla riaccensione si svuotano. */
    void markIdle() noexcept { idle = true; mixSmoother.snap (0.0f); }

    void setTimeSeconds (float left, float right) noexcept
    {
        delaySamples[0] = std::clamp (left,  0.001f, 3.99f) * (float) sampleRate;
        delaySamples[1] = std::clamp (right, 0.001f, 3.99f) * (float) sampleRate;

        for (int i = 0; i < 2; ++i)
            timeSmoother[(size_t) i].setTarget (delaySamples[i]);
    }

    void setFeedback (float fb) noexcept { feedback = std::clamp (fb, 0.0f, 1.05f); fbSmoother.setTarget (feedback); }
    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setPingPong (bool shouldPingPong) noexcept { pingPong = shouldPingPong; }
    void setLowpass (float hz) noexcept { for (auto& f : loopLowpass) f.setCutoff (std::clamp (hz, 200.0f, 20000.0f)); }
    void setHighpass (float hz) noexcept { for (auto& f : loopHighpass) f.setCutoff (std::clamp (hz, 20.0f, 2000.0f)); }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        // Prima: con il delay spento il loop continuava a girare nascosto
        // (feedback fino a 1.05 = autooscillazione) e alla riaccensione, o al
        // cambio preset, ripartiva una frase vecchia di minuti.
        // Spento o fuori catena: niente lavoro. Alla riaccensione le code
        // vanno svuotate, altrimenti riaffiora l'audio di prima (il "loop").
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            mixSmoother.snap (0.0f);
            idle = true;
            return;
        }

        if (idle)
        {
            clearTails();
            idle = false;
        }

        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        auto* left  = block.getChannelPointer (0);
        auto* right = (numChannels > 1) ? block.getChannelPointer (1) : left;

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i];
            const float dryR = right[i];

            const float m  = mixSmoother.next();
            const float fb = fbSmoother.next();
            const float tL = timeSmoother[0].next();
            const float tR = timeSmoother[1].next();

            const float outL = lines[0].read (tL);
            const float outR = lines[1].read (tR);

            float fbL = loopHighpass[0].processHighpass (loopLowpass[0].processLowpass (outL));
            float fbR = loopHighpass[1].processHighpass (loopLowpass[1].processLowpass (outR));

            // La saturazione morbida sul feedback permette di superare
            // feedback = 1 senza divergenza: il delay entra in
            // autooscillazione controllata invece di esplodere, che è il
            // comportamento dei delay a nastro e uno strumento espressivo.
            fbL = fastTanh (fbL * fb);
            fbR = fastTanh (fbR * fb);

            if (pingPong)
            {
                // L'ingresso entra solo a sinistra, poi rimbalza. Sommare
                // l'ingresso su entrambi i lati produrrebbe un delay stereo
                // normale, non un ping-pong.
                lines[0].write (dryL + dryR + fbR);
                lines[1].write (fbL);
            }
            else
            {
                lines[0].write (dryL + fbL);
                lines[1].write (dryR + fbR);
            }

            left[i]  = lerp (dryL, outL, m);
            right[i] = lerp (dryR, outR, m);
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<DelayLine, 2> lines;
    std::array<OnePoleTPT, 2> loopLowpass, loopHighpass;
    std::array<ParamSmoother, 2> timeSmoother;
    float delaySamples[2] { 11025.0f, 11025.0f };
    float feedbackState[2] { 0.0f, 0.0f };
    float feedback = 0.35f, mix = 0.0f;
    bool pingPong = false;
    ParamSmoother mixSmoother, fbSmoother;
    bool idle = true;

    void clearTails() noexcept
    {
        for (auto& l : lines) l.reset();
        for (auto& f : loopLowpass) f.reset();
        for (auto& f : loopHighpass) f.reset();
    }
};

//==============================================================================
/**
    Riverbero a rete di ritardi retroazionata (FDN) a 8 linee.

    Perché una FDN e non un Schroeder (comb + allpass):
      La struttura di Schroeder produce una densità di eco che cresce
      linearmente nel tempo; su una coda lunga si sentono i singoli echi come
      "granuli". Una FDN con matrice di feedback ortogonale (qui Householder)
      diffonde ogni eco su tutte le linee a ogni giro, quindi la densità cresce
      esponenzialmente e la coda diventa liscia entro ~40 ms.

    Le lunghezze delle linee sono numeri primi in rapporto irrazionale fra
    loro: lunghezze commensurabili farebbero coincidere periodicamente gli echi
    e produrrebbero risonanze metalliche a frequenze fisse.

    La modulazione lenta delle lunghezze (±0.3%) rompe le risonanze modali
    residue senza introdurre pitch shift udibile.
*/
class Reverb
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        mixSmoother.prepare (sampleRate, 20.0f);

        const float scale = (float) sampleRate / 44100.0f;

        for (int i = 0; i < kNumLines; ++i)
        {
            baseLength[i] = kPrimeLengths[i] * scale;
            lines[(size_t) i].prepare ((int) (baseLength[i] * 1.2f) + 64);

            damping[(size_t) i].prepare (sampleRate);
            damping[(size_t) i].setCutoff (dampingHz);

            lowCut[(size_t) i].prepare (sampleRate);
            lowCut[(size_t) i].setCutoff (lowCutHz);

            modPhase[i] = (float) i / (float) kNumLines;
        }

        preDelay.prepare ((int) (sampleRate * 0.25) + 8);

        for (auto& d : inputDiffusion)
            d.prepare (sampleRate);

        setSize (sizeAmount);
        reset();
    }

    void reset() noexcept
    {
        for (auto& l : lines) l.reset();
        for (auto& d : damping) d.reset();
        for (auto& c : lowCut) c.reset();
        for (auto& d : inputDiffusion) d.reset();
        preDelay.reset();
        feedbackVector.fill (0.0f);
        mixSmoother.snap (mix);
        idle = mix <= 0.0001f;
    }

    /** Fuori catena: le code non scorrono, alla riaccensione si svuotano. */
    void markIdle() noexcept { idle = true; mixSmoother.snap (0.0f); }

    /** 0..1: da stanza piccola a hall grande. */
    void setSize (float s) noexcept
    {
        sizeAmount = std::clamp (s, 0.0f, 1.0f);
        sizeScale = lerp (0.35f, 1.0f, sizeAmount);
    }

    /** Tempo di decadimento RT60 in secondi. */
    void setDecay (float seconds) noexcept
    {
        decaySeconds = std::clamp (seconds, 0.1f, 30.0f);
        updateFeedbackGains();
    }

    void setDamping (float hz) noexcept
    {
        dampingHz = std::clamp (hz, 500.0f, 20000.0f);
        for (auto& d : damping)
            d.setCutoff (dampingHz);
    }

    void setLowCut (float hz) noexcept
    {
        lowCutHz = std::clamp (hz, 20.0f, 1000.0f);
        for (auto& c : lowCut)
            c.setCutoff (lowCutHz);
    }

    void setPreDelay (float seconds) noexcept
    {
        preDelaySamples = std::clamp (seconds, 0.0f, 0.24f) * (float) sampleRate;
    }

    void setWidth (float w) noexcept { width = std::clamp (w, 0.0f, 1.0f); }
    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setModulation (float m) noexcept { modDepth = std::clamp (m, 0.0f, 1.0f); }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        // Spento o fuori catena: niente lavoro. Alla riaccensione le code
        // vanno svuotate, altrimenti riaffiora l'audio di prima (il "loop").
        if (mix <= 0.0001f && mixSmoother.getCurrent() <= 0.0001f)
        {
            mixSmoother.snap (0.0f);
            idle = true;
            return;
        }

        if (idle)
        {
            clearTails();
            idle = false;
        }

        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        auto* left  = block.getChannelPointer (0);
        auto* right = (numChannels > 1) ? block.getChannelPointer (1) : left;

        const float modInc = 0.13f / (float) sampleRate;

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i];
            const float dryR = right[i];
            const float m = mixSmoother.next();

            // Pre-delay: separa il suono diretto dall'inizio della coda. Su un
            // basso è ciò che permette di usare il riverbero senza perdere la
            // definizione dell'attacco.
            preDelay.write ((dryL + dryR) * 0.5f);
            float x = preDelay.read (std::max (1.0f, preDelaySamples));

            // Diffusione d'ingresso: quattro allpass in serie che spalmano
            // l'impulso prima che entri nella rete. Senza, i primi echi
            // arrivano come click distinti.
            for (auto& d : inputDiffusion)
                x = d.process (x);

            // --- Lettura delle linee ---------------------------------------
            std::array<float, kNumLines> readValues;

            for (int n = 0; n < kNumLines; ++n)
            {
                const float mod = std::sin (kTwoPi * modPhase[n]) * modDepth * 0.003f;
                modPhase[n] = wrap01 (modPhase[n] + modInc * (1.0f + 0.17f * (float) n));

                const float len = baseLength[n] * sizeScale * (1.0f + mod);
                float v = lines[(size_t) n].read (std::max (2.0f, len));

                v = damping[(size_t) n].processLowpass (v);
                v = lowCut[(size_t) n].processHighpass (v);

                readValues[(size_t) n] = v * feedbackGain[n];
            }

            // --- Matrice di Householder -------------------------------------
            // y = x - (2/N) * sum(x). Ortogonale, quindi conserva l'energia
            // esattamente, e costa N addizioni invece delle N^2 moltiplicazioni
            // di una matrice densa.
            float sum = 0.0f;
            for (float v : readValues)
                sum += v;

            const float factor = 2.0f / (float) kNumLines * sum;

            for (int n = 0; n < kNumLines; ++n)
            {
                const float injected = x * kInputGain[n];
                lines[(size_t) n].write (flushDenormal (readValues[(size_t) n] - factor + injected));
            }

            // --- Uscita stereo ------------------------------------------------
            // Le linee pari vanno a sinistra, le dispari a destra: sono
            // decorrelate per costruzione, quindi l'immagine è larga senza
            // bisogno di ritardi aggiuntivi.
            float wetL = 0.0f, wetR = 0.0f;

            for (int n = 0; n < kNumLines; ++n)
            {
                if ((n & 1) == 0) wetL += readValues[(size_t) n];
                else              wetR += readValues[(size_t) n];
            }

            wetL *= 0.35f;
            wetR *= 0.35f;

            // Controllo di larghezza per somma/differenza: a width = 0 il
            // riverbero collassa in mono, utile per bassi che devono restare
            // centrati.
            const float mid  = (wetL + wetR) * 0.5f;
            const float side = (wetL - wetR) * 0.5f * width;

            left[i]  = lerp (dryL, mid + side, m);
            right[i] = lerp (dryR, mid - side, m);
        }
    }

private:
    static constexpr int kNumLines = 8;

    /** Lunghezze in campioni a 44.1 kHz. Primi, e scelti in modo che nessun
        rapporto sia vicino a un numero razionale semplice. */
    static constexpr float kPrimeLengths[kNumLines] =
    {
        1123.0f, 1291.0f, 1531.0f, 1721.0f,
        1949.0f, 2141.0f, 2371.0f, 2609.0f
    };

    /** Guadagni d'iniezione alternati in segno: distribuisce l'ingresso senza
        creare un fronte coerente su tutte le linee. */
    static constexpr float kInputGain[kNumLines] =
    {
        0.35f, -0.35f, 0.35f, -0.35f, 0.35f, -0.35f, 0.35f, -0.35f
    };

    void updateFeedbackGains() noexcept
    {
        for (int n = 0; n < kNumLines; ++n)
        {
            const float lenSeconds = (baseLength[n] * sizeScale) / (float) sampleRate;

            // g = 10^(-3 * T / RT60): il guadagno che, applicato a ogni giro,
            // produce una caduta di 60 dB in RT60 secondi.
            feedbackGain[n] = std::pow (10.0f, -3.0f * lenSeconds / decaySeconds);
            feedbackGain[n] = std::clamp (feedbackGain[n], 0.0f, 0.9995f);
        }
    }

    double sampleRate = 44100.0;
    std::array<DelayLine, kNumLines> lines;
    std::array<OnePoleTPT, kNumLines> damping, lowCut;
    std::array<AllpassTPT, 4> inputDiffusion;
    DelayLine preDelay;

    float baseLength[kNumLines] {};
    float feedbackGain[kNumLines] {};
    float modPhase[kNumLines] {};
    std::array<float, kNumLines> feedbackVector {};

    float sizeAmount = 0.5f, sizeScale = 0.7f;
    float decaySeconds = 2.0f, dampingHz = 6000.0f, lowCutHz = 180.0f;
    float preDelaySamples = 0.0f, width = 1.0f, mix = 0.0f, modDepth = 0.4f;
    ParamSmoother mixSmoother;
    bool idle = true;

    void clearTails() noexcept
    {
        for (auto& l : lines) l.reset();
        for (auto& d : damping) d.reset();
        for (auto& c : lowCut) c.reset();
        for (auto& d : inputDiffusion) d.reset();
        preDelay.reset();
        feedbackVector.fill (0.0f);
    }
};

//==============================================================================
/**
    Compressore feed-forward con ginocchio morbido e rilevazione RMS/peak.

    Feed-forward e non feedback: il rilevatore legge l'ingresso, non l'uscita.
    Il feedback (topologia dei compressori a valvole) ha un rapporto effettivo
    che dipende dal livello e non può superare ~10:1; il feed-forward realizza
    esattamente il rapporto richiesto, compreso il limiting.

    Il rilevatore RMS usa una finestra scorrevole esponenziale invece di un
    buffer: matematicamente equivalente per il nostro scopo, e costa memoria
    costante.
*/
class Compressor
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        mixSmoother.prepare (sampleRate, 20.0f);
        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        envelope = 0.0f;
        rmsState = 0.0f;
        gainReduction = 0.0f;
        mixSmoother.snap (mix);
    }

    void setThreshold (float db) noexcept { thresholdDb = std::clamp (db, -60.0f, 0.0f); }
    void setRatio (float r) noexcept { ratio = std::clamp (r, 1.0f, 60.0f); }
    void setAttack (float ms) noexcept { attackMs = std::clamp (ms, 0.05f, 200.0f); updateCoefficients(); }
    void setRelease (float ms) noexcept { releaseMs = std::clamp (ms, 5.0f, 2000.0f); updateCoefficients(); }
    void setKnee (float db) noexcept { kneeDb = std::clamp (db, 0.0f, 24.0f); }
    void setMakeup (float db) noexcept { makeupGain = dbToGain (std::clamp (db, -12.0f, 36.0f)); }
    void setMix (float m) noexcept { mix = std::clamp (m, 0.0f, 1.0f); mixSmoother.setTarget (mix); }
    void setRmsMode (bool shouldUseRms) noexcept { useRms = shouldUseRms; }

    /** Riduzione di guadagno corrente in dB, per il meter della GUI. */
    float getGainReductionDb() const noexcept { return gainReduction; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        auto* left  = block.getChannelPointer (0);
        auto* right = (numChannels > 1) ? block.getChannelPointer (1) : left;

        float maxReduction = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float dryL = left[i];
            const float dryR = right[i];
            const float m = mixSmoother.next();

            // Rilevazione accoppiata sui due canali: rilevare separatamente
            // farebbe spostare l'immagine stereo ogni volta che un canale è
            // più forte dell'altro.
            const float key = juce::jmax (std::abs (dryL), std::abs (dryR));

            float detected;

            if (useRms)
            {
                rmsState = rmsCoef * rmsState + (1.0f - rmsCoef) * key * key;
                detected = std::sqrt (std::max (rmsState, 0.0f));
            }
            else
            {
                detected = key;
            }

            const float coef = (detected > envelope) ? attackCoef : releaseCoef;
            envelope = detected + (envelope - detected) * coef;
            envelope = flushDenormal (envelope);

            const float inputDb = gainToDb (envelope);

            // Ginocchio morbido: transizione quadratica fra il tratto lineare e
            // quello compresso su una larghezza di kneeDb. Senza, un segnale
            // che oscilla attorno alla soglia entra e esce dalla compressione
            // producendo una modulazione di ampiezza udibile.
            float overDb = inputDb - thresholdDb;
            float reductionDb = 0.0f;

            if (overDb > kneeDb * 0.5f)
            {
                reductionDb = overDb * (1.0f - 1.0f / ratio);
            }
            else if (overDb > -kneeDb * 0.5f && kneeDb > 0.0f)
            {
                const float t = overDb + kneeDb * 0.5f;
                reductionDb = (1.0f - 1.0f / ratio) * t * t / (2.0f * kneeDb);
            }

            maxReduction = juce::jmax (maxReduction, reductionDb);

            const float g = dbToGain (-reductionDb) * makeupGain;

            left[i]  = lerp (dryL, dryL * g, m);
            right[i] = lerp (dryR, dryR * g, m);
        }

        gainReduction = maxReduction;
    }

private:
    void updateCoefficients() noexcept
    {
        attackCoef  = std::exp (-1.0f / (attackMs  * 0.001f * (float) sampleRate));
        releaseCoef = std::exp (-1.0f / (releaseMs * 0.001f * (float) sampleRate));
        rmsCoef     = std::exp (-1.0f / (0.010f * (float) sampleRate));   // finestra 10 ms
    }

    double sampleRate = 44100.0;
    float thresholdDb = -12.0f, ratio = 4.0f, kneeDb = 6.0f;
    float attackMs = 8.0f, releaseMs = 120.0f;
    float attackCoef = 0.99f, releaseCoef = 0.999f, rmsCoef = 0.999f;
    float makeupGain = 1.0f, mix = 1.0f;
    ParamSmoother mixSmoother;
    float envelope = 0.0f, rmsState = 0.0f, gainReduction = 0.0f;
    bool useRms = false;
};

//==============================================================================
/**
    Equalizzatore a tre bande: low shelf, peak parametrico, high shelf.

    Realizzato con SVF TPT invece che con biquad RBJ: i coefficienti si possono
    modulare a velocità audio senza artefatti, il che permette di mettere la
    frequenza del peak nella matrice di modulazione.
*/
class EQ3Band
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        update();
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    }

    void setLowShelf (float hz, float gainDb) noexcept
    {
        lowHz = std::clamp (hz, 20.0f, 1000.0f);
        lowGainDb = std::clamp (gainDb, -24.0f, 24.0f);
        update();
    }

    void setPeak (float hz, float gainDb, float q) noexcept
    {
        midHz = std::clamp (hz, 40.0f, 12000.0f);
        midGainDb = std::clamp (gainDb, -24.0f, 24.0f);
        midQ = std::clamp (q, 0.2f, 12.0f);
        update();
    }

    void setHighShelf (float hz, float gainDb) noexcept
    {
        highHz = std::clamp (hz, 1000.0f, 20000.0f);
        highGainDb = std::clamp (gainDb, -24.0f, 24.0f);
        update();
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (isFlat)
            return;

        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = block.getChannelPointer ((size_t) ch);
            auto& st = state[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                float x = data[i];
                x = shelfLow  (x, st.l1, st.l2);
                x = peakBand  (x, st.m1, st.m2);
                x = shelfHigh (x, st.h1, st.h2);
                data[i] = x;
            }
        }
    }

private:
    struct BandState { float l1, l2, m1, m2, h1, h2; };

    void update() noexcept
    {
        gLow  = std::tan (kPi * lowHz  / (float) sampleRate);
        gMid  = std::tan (kPi * midHz  / (float) sampleRate);
        gHigh = std::tan (kPi * highHz / (float) sampleRate);

        aLow  = dbToGain (lowGainDb);
        aMid  = dbToGain (midGainDb);
        aHigh = dbToGain (highGainDb);

        r2Mid = 1.0f / midQ;
        hMid  = 1.0f / (1.0f + r2Mid * gMid + gMid * gMid);

        // Gli shelf usano Q fissa 0.707: un shelf risonante produce un
        // sovraelongamento che su un basso si traduce in un colpo di
        // fondamentale fuori controllo.
        r2Shelf = kSqrt2;
        hLow  = 1.0f / (1.0f + r2Shelf * gLow  + gLow  * gLow);
        hHigh = 1.0f / (1.0f + r2Shelf * gHigh + gHigh * gHigh);

        isFlat = std::abs (lowGainDb) < 0.01f
              && std::abs (midGainDb) < 0.01f
              && std::abs (highGainDb) < 0.01f;
    }

    float shelfLow (float x, float& s1, float& s2) noexcept
    {
        const float hp = (x - (r2Shelf + gLow) * s1 - s2) * hLow;
        const float bp = gLow * hp + s1;
        s1 = flushDenormal (gLow * hp + bp);
        const float lp = gLow * bp + s2;
        s2 = flushDenormal (gLow * bp + lp);

        return lp * aLow + bp + hp;
    }

    float peakBand (float x, float& s1, float& s2) noexcept
    {
        const float hp = (x - (r2Mid + gMid) * s1 - s2) * hMid;
        const float bp = gMid * hp + s1;
        s1 = flushDenormal (gMid * hp + bp);
        const float lp = gMid * bp + s2;
        s2 = flushDenormal (gMid * bp + lp);

        return lp + hp + bp * r2Mid * aMid;
    }

    float shelfHigh (float x, float& s1, float& s2) noexcept
    {
        const float hp = (x - (r2Shelf + gHigh) * s1 - s2) * hHigh;
        const float bp = gHigh * hp + s1;
        s1 = flushDenormal (gHigh * hp + bp);
        const float lp = gHigh * bp + s2;
        s2 = flushDenormal (gHigh * bp + lp);

        return hp * aHigh + bp + lp;
    }

    double sampleRate = 44100.0;
    float lowHz = 100.0f, lowGainDb = 0.0f;
    float midHz = 800.0f, midGainDb = 0.0f, midQ = 1.0f;
    float highHz = 6000.0f, highGainDb = 0.0f;
    float gLow = 0.1f, gMid = 0.1f, gHigh = 0.1f;
    float aLow = 1.0f, aMid = 1.0f, aHigh = 1.0f;
    float r2Mid = 1.0f, r2Shelf = kSqrt2;
    float hLow = 1.0f, hMid = 1.0f, hHigh = 1.0f;
    bool isFlat = true;
    std::array<BandState, 2> state { BandState {}, BandState {} };
};

//==============================================================================
/**
    Sezione spaziale: panorama e immagine stereo.

    Catena, nell'ordine:
      1. Bass Mono — il canale laterale viene passato in un passa-alto
         Linkwitz-Riley del 4° ordine: sotto la frequenza scelta il basso è
         mono. È la regola di base per un basso da club (sub in mono, niente
         cancellazioni sull'impianto e sul vinile), applicata senza toccare il
         canale centrale, quindi senza alcuna perdita di peso.
      2. Width — guadagno del canale laterale (M/S): 0 mono, 1 neutro, 2 largo.
      3. Haas — ritardo di 0-30 ms sul solo canale destro: allarga senza
         modulazione. Sopra ~35 ms diventerebbe un'eco.
      4. Rotation — rotazione dell'immagine stereo (matrice di rotazione L/R).
      5. Pan + Autopan — bilanciamento a potenza costante; l'autopan è un seno
         agganciato alla posizione del brano, quindi resta in fase con la
         griglia anche dopo un salto della testina.
*/
class Space
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate > 0.0 ? spec.sampleRate : 44100.0;

        haasLine.prepare ((int) std::ceil (sampleRate * 0.035));

        for (auto& f : sideHighpass)
            f.prepare (sampleRate);

        panSmoother.prepare (sampleRate, 20.0f);
        widthSmoother.prepare (sampleRate, 20.0f);
        haasSmoother.prepare (sampleRate, 60.0f);
        reset();
    }

    void reset() noexcept
    {
        haasLine.reset();
        for (auto& f : sideHighpass) f.reset();
        panSmoother.snap (pan);
        widthSmoother.snap (width);
        haasSmoother.snap (haasSeconds);
    }

    void setEnabled (bool e) noexcept       { enabled = e; }
    void setPan (float p) noexcept          { pan = std::clamp (p, -1.0f, 1.0f); }
    void setWidth (float w) noexcept        { width = std::clamp (w, 0.0f, 2.0f); }
    void setHaas (float seconds) noexcept   { haasSeconds = std::clamp (seconds, 0.0f, 0.03f); }
    void setRotation (float r) noexcept     { rotation = std::clamp (r, -1.0f, 1.0f); }
    void setAutopan (float depth, float beatsPerCycle) noexcept
    {
        autopanDepth = std::clamp (depth, 0.0f, 1.0f);
        autopanBeats = std::max (0.0625f, beatsPerCycle);
    }

    void setMonoFrequency (float hz) noexcept
    {
        monoHz = hz;

        // Linkwitz-Riley 4 = due Butterworth del 2° ordine in cascata (Q 0.7071).
        for (auto& f : sideHighpass)
            f.setCutoffAndQ (std::clamp (hz, 20.0f, 1000.0f), 0.70710678f);
    }

    /** A trasporto fermo l'autopan continua col proprio orologio: riallinearlo
        ogni blocco alla stessa posizione lo farebbe saltare a ogni blocco. */
    void setTransport (double bpm, double ppq, bool playing) noexcept
    {
        currentBpm = std::max (1.0, bpm);

        if (playing)
            currentPpq = ppq;
    }

    /** Correlazione L/R dell'ultimo blocco (-1..+1), per il meter. */
    float getCorrelation() const noexcept { return correlation; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (! enabled || block.getNumChannels() < 2)
            return;

        auto* left  = block.getChannelPointer (0);
        auto* right = block.getChannelPointer (1);
        const int numSamples = (int) block.getNumSamples();

        const bool bassMono = monoHz > 20.5f;
        const float rotAngle = rotation * kHalfPi * 0.5f;    // ±45°
        const float rc = std::cos (rotAngle), rs = std::sin (rotAngle);

        const double ppqPerSample = currentBpm / (60.0 * sampleRate);

        panSmoother.setTarget (pan);
        widthSmoother.setTarget (width);
        haasSmoother.setTarget (haasSeconds * (float) sampleRate);

        double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            float l = left[i], r = right[i];

            // --- M/S: bass mono e width --------------------------------------
            const float mid = 0.5f * (l + r);
            float side = 0.5f * (l - r);

            if (bassMono)
                side = sideHighpass[1].processHighpass (sideHighpass[0].processHighpass (side));

            side *= widthSmoother.next();

            l = mid + side;
            r = mid - side;

            // --- Haas ------------------------------------------------------------
            const float haas = haasSmoother.next();
            haasLine.write (r);

            if (haas > 0.5f)
                r = haasLine.read (haas + 1.0f);

            // --- Rotazione -------------------------------------------------------
            if (std::abs (rotAngle) > 1.0e-4f)
            {
                const float nl = rc * l - rs * r;
                const float nr = rs * l + rc * r;
                l = nl; r = nr;
            }

            // --- Pan + autopan ------------------------------------------------------
            float p = panSmoother.next();

            if (autopanDepth > 0.0f)
            {
                const double phase = (currentPpq + ppqPerSample * i) / (double) autopanBeats;
                p += autopanDepth * std::sin ((float) (2.0 * kPiD * (phase - std::floor (phase))));
            }

            if (std::abs (p) > 1.0e-4f)
            {
                float gl, gr;
                constantPowerPan (std::clamp (p, -1.0f, 1.0f), gl, gr);
                l *= gl * kSqrt2;
                r *= gr * kSqrt2;
            }

            left[i] = l;
            right[i] = r;

            sumLR += (double) l * r;
            sumLL += (double) l * l;
            sumRR += (double) r * r;
        }

        const double denom = std::sqrt (sumLL * sumRR);
        correlation = denom > 1.0e-12 ? (float) (sumLR / denom) : 1.0f;
        currentPpq += ppqPerSample * numSamples;
    }

private:
    /** Biquad passa-alto (forma TPT/SVF) per il crossover del lato. */
    struct HighpassSVF
    {
        void prepare (double sr) noexcept { sampleRate = sr; update(); }
        void reset() noexcept { s1 = s2 = 0.0f; }
        void setCutoffAndQ (float hz, float q) noexcept { cutoff = hz; Q = q; update(); }

        float processHighpass (float x) noexcept
        {
            const float hp = (x - (2.0f * R + g) * s1 - s2) * h;
            const float bp = g * hp + s1;
            s1 = flushDenormal (g * hp + bp);
            const float lp = g * bp + s2;
            s2 = flushDenormal (g * bp + lp);
            return hp;
        }

        void update() noexcept
        {
            g = std::tan (kPi * std::min (cutoff, (float) sampleRate * 0.45f) / (float) sampleRate);
            R = 1.0f / (2.0f * Q);
            h = 1.0f / (1.0f + 2.0f * R * g + g * g);
        }

        double sampleRate = 44100.0;
        float cutoff = 100.0f, Q = 0.7071f, g = 0.0f, R = 0.7071f, h = 1.0f;
        float s1 = 0.0f, s2 = 0.0f;
    };

    double sampleRate = 44100.0;
    bool  enabled = true;
    float pan = 0.0f, width = 1.0f, haasSeconds = 0.0f, rotation = 0.0f, monoHz = 20.0f;
    float autopanDepth = 0.0f, autopanBeats = 1.0f;
    double currentBpm = 120.0, currentPpq = 0.0;
    float correlation = 1.0f;

    DelayLine haasLine;
    std::array<HighpassSVF, 2> sideHighpass;
    ParamSmoother panSmoother, widthSmoother, haasSmoother;
};

//==============================================================================
/**
    Limiter con lookahead.

    Il lookahead è realizzato ritardando il segnale mentre il rilevatore vede
    il futuro. Senza, un limiter deve reagire dopo che il picco è già passato,
    e l'unico modo di rispettare il tetto sarebbe tagliare — cioè distorcere.
    Il costo è una latenza pari alla finestra di lookahead, dichiarata all'host.

    L'inviluppo di riduzione è lisciato con un filtro a media mobile su tutta la
    finestra: questo garantisce che la riduzione sia già a regime quando il
    picco arriva, senza il "pompaggio" di un rilascio esponenziale.
*/
class Limiter
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        lookaheadSamples = juce::jmax (8, (int) (0.002 * sampleRate));   // 2 ms

        for (auto& l : delayLines)
            l.prepare (lookaheadSamples + 8);

        peakWindow.assign ((size_t) lookaheadSamples, 0.0f);
        smoothWindow.assign ((size_t) lookaheadSamples, 1.0f);

        reset();
    }

    void reset() noexcept
    {
        for (auto& l : delayLines)
            l.reset();

        std::fill (peakWindow.begin(), peakWindow.end(), 0.0f);
        std::fill (smoothWindow.begin(), smoothWindow.end(), 1.0f);

        windowIndex = 0;
        smoothSum = (float) lookaheadSamples;
        currentReduction = 0.0f;
    }

    void setCeiling (float db) noexcept { ceilingGain = dbToGain (std::clamp (db, -24.0f, 0.0f)); }
    void setEnabled (bool shouldBeEnabled) noexcept { enabled = shouldBeEnabled; }

    int getLatencySamples() const noexcept { return lookaheadSamples; }
    float getReductionDb() const noexcept { return currentReduction; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (! enabled)
            return;

        const int numSamples = (int) block.getNumSamples();
        const int numChannels = juce::jmin (2, (int) block.getNumChannels());

        auto* left  = block.getChannelPointer (0);
        auto* right = (numChannels > 1) ? block.getChannelPointer (1) : left;

        float maxRed = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float peak = juce::jmax (std::abs (left[i]), std::abs (right[i]));

            // Guadagno istantaneo necessario per questo campione.
            const float required = (peak > ceilingGain) ? (ceilingGain / peak) : 1.0f;

            // Il minimo su tutta la finestra: se un picco arriva fra N
            // campioni, la riduzione deve già essere in atto adesso.
            peakWindow[(size_t) windowIndex] = required;

            float windowMin = 1.0f;
            for (float g : peakWindow)
                windowMin = juce::jmin (windowMin, g);

            // Media mobile sulla finestra: rende la curva di guadagno continua
            // e derivabile, quindi priva di distorsione di intermodulazione.
            smoothSum -= smoothWindow[(size_t) windowIndex];
            smoothWindow[(size_t) windowIndex] = windowMin;
            smoothSum += windowMin;

            const float gain = smoothSum / (float) lookaheadSamples;

            windowIndex = (windowIndex + 1) % lookaheadSamples;

            delayLines[0].write (left[i]);
            delayLines[1].write (right[i]);

            const float dl = delayLines[0].read ((float) lookaheadSamples);
            const float dr = delayLines[1].read ((float) lookaheadSamples);

            left[i]  = std::clamp (dl * gain, -ceilingGain, ceilingGain);
            right[i] = std::clamp (dr * gain, -ceilingGain, ceilingGain);

            maxRed = juce::jmax (maxRed, -gainToDb (gain));
        }

        currentReduction = maxRed;
    }

private:
    double sampleRate = 44100.0;
    std::array<DelayLine, 2> delayLines;
    std::vector<float> peakWindow, smoothWindow;
    int lookaheadSamples = 96, windowIndex = 0;
    float smoothSum = 96.0f, ceilingGain = 1.0f, currentReduction = 0.0f;
    bool enabled = true;
};

} // namespace bassx
