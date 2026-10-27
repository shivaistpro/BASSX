/*
  ==============================================================================

    BASSX — Voice.h

    Una voce completa: 3 oscillatori wavetable + sub + rumore, cross-modulazione,
    due filtri con routing selezionabile, quattro inviluppi, quattro LFO e la
    matrice di modulazione.

    Nota sull'ordine delle operazioni per campione — non è arbitrario:

      1. Inviluppi e LFO avanzano PRIMA di tutto. Sono sorgenti di modulazione:
         se avanzassero dopo, ogni destinazione userebbe il valore del campione
         precedente, introducendo un ritardo di un campione che su una
         modulazione audio-rate diventa uno sfasamento dipendente dalla
         frequenza.

      2. La matrice viene valutata una volta, non per destinazione. Valutarla
         per destinazione significherebbe rileggere le sorgenti N volte.

      3. Il pitch viene calcolato prima degli oscillatori, il cutoff prima dei
         filtri. Ovvio, ma la conseguenza meno ovvia è che la modulazione del
         cutoff da parte di un LFO audio-rate è campione-esatta, quindi
         utilizzabile per FM sul filtro senza artefatti di zipper.

      4. La compensazione di livello del mix degli oscillatori è calcolata
         sulla somma dei livelli, non applicata a valle. Applicarla a valle
         significherebbe che aggiungere un oscillatore cambia quanto forte il
         filtro viene pilotato, cioè cambia il timbro oltre che il volume.

  ==============================================================================
*/

#pragma once

#include "Common.h"
#include "Oscillator.h"
#include "Filters.h"
#include "Envelope.h"
#include "LFO.h"
#include "ModMatrix.h"
#include "WavetableBank.h"
#include "SampleWavetable.h"
#include "NeuralDrive.h"
#include "../Parameters.h"

namespace bassx
{

//==============================================================================
class Voice
{
public:
    Voice() = default;

    //==========================================================================
    void prepare (double sr, const WavetableBank& bank)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        wavetableBank = &bank;

        for (auto& o : oscillators)
            o.prepare (sampleRate);

        subOsc.prepare (sampleRate);
        noise.reset();

        // I filtri vengono preparati al sample rate MASSIMO previsto
        // (base x 8). Così il buffer del comb è già dimensionato per il caso
        // peggiore e commutare il fattore di oversampling a runtime non
        // richiede alcuna allocazione sul thread audio.
        filter1.prepare (sampleRate * (double) kMaxFilterOversample);
        filter2.prepare (sampleRate * (double) kMaxFilterOversample);

        for (auto& d : decimator)
            d.prepare (sampleRate * (double) kMaxFilterOversample);

        setFilterOversampling (filterOversample);

        for (auto& e : envelopes)
            e.prepare (sampleRate);

        for (auto& l : lfos)
            l.prepare (sampleRate);

        accent.prepare (sampleRate);

        glide.prepare (sampleRate, 60.0f);
        glide.snap (std::log2 (110.0f));

        cutoff1Smoother.prepare (sampleRate, 6.0f);
        cutoff2Smoother.prepare (sampleRate, 6.0f);
        level1Smoother.prepare (sampleRate, 12.0f);
        level2Smoother.prepare (sampleRate, 12.0f);
        level3Smoother.prepare (sampleRate, 12.0f);
        // Risonanza e drive dei filtri: prima saltavano a ogni blocco (zipper
        // a manopola veloce o in automazione, molto udibile a risonanza alta).
        for (auto* sm : { &reso1Smoother, &reso2Smoother, &drive1Smoother, &drive2Smoother })
            sm->prepare (sampleRate, 10.0f);
        filterSmoothersPrimed = false;

        dcBlockL.prepare (sampleRate);
        dcBlockR.prepare (sampleRate);

        reset();
    }

    void reset() noexcept
    {
        for (auto& o : oscillators)
            o.reset (false);    // fasi programmate: layer allineati

        subOsc.reset();
        noise.reset();
        filter1.reset();
        filter2.reset();

        for (auto& d : decimator)
            d.reset();

        for (auto& e : envelopes)
            e.reset();

        for (auto& l : lfos)
            l.reset();

        accent.reset();
        dcBlockL.reset();
        dcBlockR.reset();
        neuralDrive.reset();
        lastVcaOut = 0.0f;
        vibratoPhase = 0.0f;

        active = false;
        noteNumber = -1;
        noteOnCounter = 0;
    }

    //==========================================================================
    bool isActive() const noexcept { return active; }
    int  getNoteNumber() const noexcept { return noteNumber; }
    uint32_t getNoteOnOrder() const noexcept { return noteOnCounter; }
    bool isReleasing() const noexcept { return envelopes[0].isInRelease(); }
    float getAmpEnvLevel() const noexcept { return envelopes[0].getCurrentLevel(); }
    float getAccentCharge() const noexcept { return accent.getCurrent(); }
    float getFilterEnvLevel() const noexcept { return envelopes[1].getCurrentLevel(); }

    //==========================================================================
    /**
        Fattore di oversampling della sola sezione filtro.

        L'oversampling è applicato qui e non all'intero sintetizzatore perché
        qui e solo qui nasce l'aliasing: gli oscillatori sono wavetable con
        mipmapping, quindi già band-limited per costruzione, e inviluppi e LFO
        sono segnali di controllo. Oversamplare tutto costerebbe da due a otto
        volte la CPU per ripulire uno stadio che era già pulito.

        Valori ammessi: 1, 2, 4, 8. Commutabile fra un blocco e l'altro senza
        allocazioni.
    */
    void setFilterOversampling (int factor) noexcept
    {
        factor = (factor >= 8) ? 8 : (factor >= 4 ? 4 : (factor >= 2 ? 2 : 1));

        filterOversample = factor;

        const double workingRate = sampleRate * (double) factor;

        filter1.setSampleRate (workingRate);
        filter2.setSampleRate (workingRate);

        // Filtro di decimazione: Butterworth del quarto ordine a 0.45 volte la
        // Nyquist di base. Le due Q sono quelle canoniche di un Butterworth di
        // ordine 4 fattorizzato in due sezioni biquadratiche.
        const float decimationHz = (float) (sampleRate * 0.45);

        for (int i = 0; i < 2; ++i)
            decimator[(size_t) i].setSampleRate (workingRate);

        decimator[0].setCutoffAndQ (decimationHz, 0.54120f);
        decimator[1].setCutoffAndQ (decimationHz, 1.30656f);
    }

    /** Applica i parametri di controllo. Chiamato una volta per blocco. */
    void setParameters (const SynthParams& p, const ModMatrix& matrix) noexcept
    {
        params = &p;
        modMatrix = &matrix;

        // --- Oscillatori -----------------------------------------------------
        for (int o = 0; o < kNumOscillators; ++o)
        {
            const auto& op = p.osc[(size_t) o];
            auto& osc = oscillators[(size_t) o];

            // "Sample": la wavetable dell'istanza (campione trascinato); senza campione, Analog Saw
            osc.setTable (op.waveIndex == kUserWaveIndex
                            ? (userTable != nullptr ? userTable : &wavetableBank->getTable (0))
                            : &wavetableBank->getTable (op.waveIndex));
            osc.setFramePosition (op.position);
            osc.setPan (op.pan);
            osc.setFold (op.fold);
            osc.setUnison (op.unisonVoices, op.unisonDetune, op.unisonBlend,
                           op.unisonSpread, op.phaseRandom);
        }

        subOsc.setShape ((SubOscillator::Shape) juce::jlimit (0, 2, p.subShape));
        subOsc.setOctave (p.subOctave);

        noise.setColour (p.noiseColour == 0 ? NoiseGenerator::Colour::White
                                            : NoiseGenerator::Colour::Pink);

        // --- Filtri -----------------------------------------------------------
        filter1.setModel ((MultiFilter::Model) juce::jlimit (0, (int) MultiFilter::Model::NumModels - 1, p.f1Model));
        filter1.setBassCompensation (p.f1BassComp);

        filter2.setModel ((MultiFilter::Model) juce::jlimit (0, (int) MultiFilter::Model::NumModels - 1, p.f2Model));

        // --- Inviluppi ---------------------------------------------------------
        for (int e = 0; e < kNumEnvelopes; ++e)
        {
            const auto& ep = p.env[(size_t) e];
            auto& env = envelopes[(size_t) e];

            env.setDelay (ep.delay);

            // Env Attack (Devil Fish "soft attack"): solo l'inviluppo di
            // ampiezza delle note NON accentate; le accentate restano secche.
            env.setAttack ((e == 0 && ! noteAccented) ? std::max (ep.attack, p.dfEnvAttack) : ep.attack);
            env.setHold (ep.hold);
            env.setDecay (ep.decay);
            env.setSustain (ep.sustain);
            env.setRelease (ep.release);
            env.setCurves (ep.curveA, ep.curveD, ep.curveR);
            env.setVelocityScaling (ep.velocityAmount);
        }

        // --- LFO ---------------------------------------------------------------
        for (int l = 0; l < kNumLFOs; ++l)
        {
            const auto& lp = p.lfo[(size_t) l];
            auto& lfo = lfos[(size_t) l];

            lfo.setShape ((LFO::Shape) juce::jlimit (0, (int) LFO::Shape::NumShapes - 1, lp.shape));
            lfo.setRateHz (lp.rateHz);
            lfo.setSyncEnabled (lp.sync);
            lfo.setSyncDivision (lp.division);
            lfo.setPhaseOffset (lp.phase);
            lfo.setTriggerMode ((LFO::TriggerMode) juce::jlimit (0, (int) LFO::TriggerMode::NumModes - 1, lp.trigger));
            lfo.setUnipolar (lp.unipolar);
            lfo.setFadeIn (lp.fadeIn);
            lfo.setSmoothing (lp.smoothMs);
        }

        accent.setDecayTime (p.accentDecay);
        accent.setAttackTime (p.dfSweepSpeed * 0.12f);

        // Il tempo di glide viene applicato al momento del note-on: cambiarlo
        // mentre uno slide è in corso produrrebbe un salto di intonazione.
        glide.setTime (glideTauMs (p.glideTime));
    }

    /** Trasporto dell'host, per gli LFO sincronizzati. */
    void setTransport (double bpm, double ppq, bool playing) noexcept
    {
        for (auto& l : lfos)
        {
            l.setTransport (bpm, ppq, playing);
            l.alignToTransport();
        }
    }

    //==========================================================================
    void startNote (int midiNote, float velocity, bool isAccentNote, bool isSlide, uint32_t order) noexcept
    {
        jassert (params != nullptr);

        const bool wasActive = active;
        const bool legato = isSlide && wasActive;

        noteNumber = midiNote;
        currentVelocity = std::clamp (velocity, 0.0f, 1.0f);
        active = true;
        noteOnCounter = order;

        if (! legato)
        {
            noteAccented = isAccentNote;
            envelopes[0].setAttack (isAccentNote ? params->env[0].attack
                                                 : std::max (params->env[0].attack, params->dfEnvAttack));
        }

        const float targetHz = noteToHz (midiNote);

        // --- Glide -------------------------------------------------------------
        const bool shouldGlide = (params->glideMode == 1)                    // sempre
                              || (params->glideMode == 0 && legato);          // solo legato

        if (shouldGlide && wasActive)
        {
            glide.setTime (glideTauMs (params->glideTime));
            glide.setTarget (std::log2 (targetHz));
        }
        else
        {
            glide.snap (std::log2 (targetHz));
        }

        // --- Reset di fase -----------------------------------------------------
        // Solo per note non legate: una nota slidata deve essere un singolo
        // evento continuo, e resettare la fase in mezzo produrrebbe un click.
        if (! legato)
        {
            for (int o = 0; o < kNumOscillators; ++o)
                if (params->osc[(size_t) o].retrigger)
                    oscillators[(size_t) o].resetPhaseHard();

            // Il sub segue l'oscillatore 1: se quello riparte a fase 0 riparte
            // anche lui, altrimenti girano liberi insieme e restano in fase.
            if (params->osc[0].retrigger)
                subOsc.reset();

            // Random per nota: sorgente di modulazione che cambia a ogni nota
            // e resta costante durante la nota. Serve a "umanizzare" senza
            // instabilità udibile.
            randomPerNote = rng.nextBipolar();
            randomUnipolar = rng.nextUnipolar();
        }

        // --- Inviluppi ---------------------------------------------------------
        for (auto& e : envelopes)
            e.noteOn (! legato, currentVelocity);

        for (auto& l : lfos)
            l.noteOn (legato);

        // --- Accento -----------------------------------------------------------
        if (isAccentNote)
            accent.trigger (0.9f);
    }

    void stopNote (bool allowTailOff) noexcept
    {
        if (! allowTailOff)
        {
            for (auto& e : envelopes)
                e.quickRelease();
        }
        else
        {
            for (auto& e : envelopes)
                e.noteOff();
        }
    }

    /** Riporta la voce a intonazione nuova senza ritriggerare: usato dallo
        slide del sequencer e dal fallback sulle note ancora premute. */
    void slideTo (int midiNote) noexcept
    {
        noteNumber = midiNote;

        if (params != nullptr)
            glide.setTime (glideTauMs (params->glideTime));

        glide.setTarget (std::log2 (noteToHz (midiNote)));
    }

    void setModWheel (float v) noexcept { modWheel = std::clamp (v, 0.0f, 1.0f); }
    void setAftertouch (float v) noexcept { aftertouch = std::clamp (v, 0.0f, 1.0f); }
    void setPitchBend (float semitones) noexcept { pitchBendSemitones = semitones; }
    void setSequencerValue (float v) noexcept { sequencerValue = std::clamp (v, -1.0f, 1.0f); }

    /** Ingresso esterno per la Ext Mode (stereo, già col guadagno applicato).
        nullptr = nessun ingresso: la voce usa gli oscillatori. */
    void setExternalInput (const float* left, const float* right) noexcept { extL = left; extR = right; }

    /** Ultimi valori delle destinazioni di modulazione: il processore li
        legge per modulare gli effetti globali dalla voce più recente. */
    const ModMatrix::DestValues& getLastDests() const noexcept { return dests; }

    //==========================================================================
    /** Somma la voce nel buffer stereo. */
    void renderNextBlock (float* outL, float* outR, int startSample, int numSamples) noexcept
    {
        if (! active || params == nullptr || modMatrix == nullptr || wavetableBank == nullptr)
            return;

        const auto& p = *params;

        // Costanti di blocco: tutto ciò che non cambia per campione viene
        // calcolato qui una volta sola.
        const float ratio[kNumOscillators] =
        {
            semitonesToRatio ((float) p.osc[0].octave * 12.0f + (float) p.osc[0].semitone + p.osc[0].fineCents * 0.01f),
            semitonesToRatio ((float) p.osc[1].octave * 12.0f + (float) p.osc[1].semitone + p.osc[1].fineCents * 0.01f),
            semitonesToRatio ((float) p.osc[2].octave * 12.0f + (float) p.osc[2].semitone + p.osc[2].fineCents * 0.01f)
        };

        const float keyTrackOctaves = ((float) noteNumber - 60.0f) / 12.0f;

        oscillators[1].setSyncEnabled (p.syncEnabled);

        const bool extMode = p.dfExtMode && extL != nullptr;
        const float vibInc = p.dfVibSpeed / (float) sampleRate;

        for (int n = 0; n < numSamples; ++n)
        {
            // --- 0. Destinazioni a frequenza di controllo ---------------------
            // Ricalcolare unison, tempi di inviluppo e velocità degli LFO a ogni
            // campione costerebbe più dell'intera voce; ogni 32 campioni
            // (<1 ms) la modulazione resta liscia all'orecchio.
            if ((n & 31) == 0)
                applyControlRateModulation (p);

            // --- 1. Sorgenti di modulazione -----------------------------------
            sources.reset();

            for (int e = 0; e < kNumEnvelopes; ++e)
                sources.value[(size_t) ModSource::Env1 + (size_t) e] = envelopes[(size_t) e].getNextSample();

            const float lfoDepthMod[kNumLFOs] = { dests[ModDest::LFO1Depth], dests[ModDest::LFO2Depth], 0.0f, 0.0f };

            for (int l = 0; l < kNumLFOs; ++l)
                sources.value[(size_t) ModSource::LFO1 + (size_t) l] =
                    lfos[(size_t) l].getNextSample() * p.lfo[(size_t) l].depth
                    * std::clamp (1.0f + lfoDepthMod[l], 0.0f, 2.0f);

            const float accentLevel = accent.getNextSample();

            sources[ModSource::Velocity]       = currentVelocity;
            sources[ModSource::KeyTrack]       = keyTrackOctaves / 4.0f;   // ±4 ottave -> ±1
            sources[ModSource::ModWheel]       = modWheel;
            sources[ModSource::PitchBend]      = pitchBendSemitones / 24.0f;
            sources[ModSource::Aftertouch]     = aftertouch;
            sources[ModSource::Accent]         = accentLevel;
            sources[ModSource::RandomPerNote]  = randomPerNote;
            sources[ModSource::RandomUnipolar] = randomUnipolar;
            sources[ModSource::StepSequencer]  = sequencerValue;

            // --- 2. Matrice ----------------------------------------------------
            modMatrix->apply (sources, dests);

            // --- 3. Intonazione -------------------------------------------------
            const float baseHz = std::exp2 (glide.next());

            // Vibrato dedicato (Vib Speed / Vib Depth): seno libero per voce,
            // profondità fino a ±1 semitono.
            vibratoPhase += vibInc;
            vibratoPhase -= std::floor (vibratoPhase);
            const float vibDepth = std::clamp (p.dfVibDepth + dests[ModDest::VibratoDepth], 0.0f, 1.0f);
            const float vibrato = vibDepth > 0.0f ? vibDepth * std::sin (kTwoPi * vibratoPhase) / 12.0f : 0.0f;

            const float globalPitchOctaves = dests[ModDest::AllOscPitch] + vibrato
                                           + (pitchBendSemitones + p.masterTune * 0.01f) / 12.0f;

            // --- 4. Oscillatori --------------------------------------------------
            float mixL = 0.0f, mixR = 0.0f;
            float levelSum = 0.0f;

            // Modulatore per FM e ring: generato per primo perché serve come
            // ingresso agli altri.
            const float fmAmount = std::clamp (p.fmAmount + dests[ModDest::FMAmount], 0.0f, 1.0f);
            const float ringAmount = std::clamp (p.ringAmount + dests[ModDest::RingModAmount], 0.0f, 1.0f);

            float modulatorOut = 0.0f;

            const int fmSourceIndex = juce::jlimit (0, 3, p.fmSource);
            const int fmTargetIndex = juce::jlimit (0, 2, p.fmTarget);

            // --- Sub e rumore ---------------------------------------------------
            float subOut = 0.0f, subRaw = 0.0f;

            // Il sub serve anche come modulatore FM quando non e' acceso:
            // prima, con il sub spento, "FM Source = Sub" non faceva nulla.
            if (p.subEnabled || (fmSourceIndex == 3 && fmAmount > 0.0001f))
            {
                subOsc.setFrequency (baseHz * std::exp2 (globalPitchOctaves));
                subOsc.setLevel (1.0f);
                subRaw = subOsc.process();
                if (p.subEnabled)
                    subOut = subRaw * std::clamp (p.subLevel + dests[ModDest::SubLevel], 0.0f, 2.0f);
            }

            float noiseOut = 0.0f;

            if (p.noiseEnabled)
            {
                noise.setLevel (1.0f);
                noiseOut = noise.process()
                         * std::clamp (p.noiseLevel + dests[ModDest::NoiseLevel], 0.0f, 2.0f);
            }

            // Il modulatore FM deve essere calcolato prima dei portanti. Se la
            // sorgente è uno degli oscillatori principali, quello viene
            // generato in questo passaggio e il suo output riusato: generarlo
            // due volte lo farebbe avanzare di due campioni per campione, cioè
            // suonare un'ottava sopra.
            std::array<bool, kNumOscillators> alreadyRendered { false, false, false };
            std::array<float, kNumOscillators> oscOutL { 0.0f, 0.0f, 0.0f };
            std::array<float, kNumOscillators> oscOutR { 0.0f, 0.0f, 0.0f };

            auto renderOsc = [&] (int index, float phaseMod) noexcept
            {
                const auto& op = p.osc[(size_t) index];
                auto& osc = oscillators[(size_t) index];

                const float pitchMod = dests[(ModDest) ((int) ModDest::Osc1Pitch + index * 5)]
                                     + globalPitchOctaves;

                osc.setFrequency (baseHz * ratio[index] * std::exp2 (pitchMod));

                // Hard sync: OSC 2 riparte a ogni ciclo di OSC 1 (il master). Prima
                // la frequenza del master non veniva mai impostata e restava 110 Hz
                // fissi: con SYNC acceso OSC 2 suonava sempre in La, su ogni nota.
                if (index == 1 && p.syncEnabled)
                    osc.setSyncFrequency (baseHz * ratio[0] * std::exp2 (dests[ModDest::Osc1Pitch] + globalPitchOctaves));

                const float posMod = dests[(ModDest) ((int) ModDest::Osc1Position + index * 5)];
                osc.setFramePosition (std::clamp (op.position + posMod, 0.0f, 1.0f));

                const float foldMod = dests[(ModDest) ((int) ModDest::Osc1Fold + index * 5)];
                osc.setFold (std::clamp (op.fold + foldMod, 0.0f, 1.0f));

                const float panMod = dests[(ModDest) ((int) ModDest::Osc1Pan + index * 5)];
                osc.setPan (std::clamp (op.pan + panMod, -1.0f, 1.0f));

                osc.setLevel (1.0f);

                float l = 0.0f, r = 0.0f;
                osc.processStereo (phaseMod, l, r);

                oscOutL[(size_t) index] = l;
                oscOutR[(size_t) index] = r;
                lastOscOut[(size_t) index] = 0.5f * (l + r);
                alreadyRendered[(size_t) index] = true;
            };

            if (fmAmount > 0.0001f)
            {
                if (fmSourceIndex == fmTargetIndex)
                {
                    // Auto-FM (sorgente = destinazione): feedback FM con il
                    // campione precedente dello stesso oscillatore. Prima la
                    // modulazione veniva calcolata e poi scartata: niente FM.
                    modulatorOut = lastOscOut[(size_t) fmSourceIndex];
                }
                else if (fmSourceIndex < kNumOscillators)
                {
                    renderOsc (fmSourceIndex, 0.0f);
                    modulatorOut = (oscOutL[(size_t) fmSourceIndex] + oscOutR[(size_t) fmSourceIndex]) * 0.5f;
                }
                else
                {
                    modulatorOut = subRaw;
                }
            }

            // L'indice di modulazione è scalato a 0.5 cicli al massimo: oltre,
            // lo spettro FM si estende talmente da rendere impossibile
            // controllare il contenuto in banda, e su un basso il risultato è
            // rumore invece che timbro.
            const float phaseModAmount = fmAmount * modulatorOut * 0.5f;

            for (int o = 0; o < kNumOscillators; ++o)
            {
                const auto& op = p.osc[(size_t) o];

                if (! op.enabled)
                    continue;

                const float pm = (o == fmTargetIndex) ? phaseModAmount : 0.0f;

                // Se l'oscillatore è già stato generato come modulatore ma
                // adesso serve anche modulato, va rigenerato: il caso è raro
                // (auto-FM) e la doppia generazione è comunque corretta perché
                // la seconda chiamata avanza la fase dello stesso oscillatore.
                if (! alreadyRendered[(size_t) o]
                    || (o == fmTargetIndex && std::abs (pm) > 1.0e-9f && o != fmSourceIndex))
                    renderOsc (o, pm);

                float levelMod = dests[(ModDest) ((int) ModDest::Osc1Level + o * 5)];
                const float lvl = std::clamp (op.level + levelMod, 0.0f, 2.0f);

                mixL += oscOutL[(size_t) o] * lvl;
                mixR += oscOutR[(size_t) o] * lvl;
                levelSum += lvl;
            }

            // --- Ring modulation --------------------------------------------------
            if (ringAmount > 0.0001f)
            {
                // Osc1 x Osc2. Il ring mod è la moltiplicazione dei due
                // segnali; il crossfade con il segnale asciutto permette
                // dosaggi intermedi senza perdere la fondamentale.
                const float ringL = oscOutL[0] * oscOutL[1] * 2.0f;
                const float ringR = oscOutR[0] * oscOutR[1] * 2.0f;

                mixL = lerp (mixL, ringL, ringAmount);
                mixR = lerp (mixR, ringR, ringAmount);
            }

            mixL += subOut + noiseOut;
            mixR += subOut + noiseOut;

            levelSum += (p.subEnabled ? p.subLevel : 0.0f) + (p.noiseEnabled ? p.noiseLevel : 0.0f);

            // Normalizzazione: stackare sorgenti deve cambiare il timbro, non
            // quanto forte il filtro viene pilotato.
            if (levelSum > 1.0f)
            {
                const float norm = 1.0f / levelSum;
                mixL *= norm;
                mixR *= norm;
            }

            // --- 5. Filtri ---------------------------------------------------------
            const float envF1 = sources.value[(size_t) ModSource::Env1 + (size_t) juce::jlimit (0, 3, p.f1EnvSource)];
            const float envF2 = sources.value[(size_t) ModSource::Env1 + (size_t) juce::jlimit (0, 3, p.f2EnvSource)];

            // Modulazione esponenziale, in ottave. Lineare in Hz sarebbe
            // sbagliato: la stessa profondità produrrebbe uno sweep enorme in
            // alto e nullo in basso, che è dove sta un basso.
            // Filter FM (Devil Fish): l'uscita del VCA modula il cutoff a
            // frequenza audio. Si usa il campione precedente: il ritardo di
            // un campione è quello del circuito reale, e senza il loop
            // sarebbe algebrico.
            const float filterFm = std::clamp (p.dfFilterFm + dests[ModDest::FishFilterFM], 0.0f, 1.0f);

            const float f1Octaves = p.f1EnvAmount * kFilterEnvOctaves * envF1
                                  + p.f1KeyTrack * keyTrackOctaves
                                  + p.accentFilter * p.accentAmount * kAccentOctaves * accentLevel
                                  + dests[ModDest::Filter1Cutoff] * kModCutoffOctaves
                                  + filterFm * kFilterFmOctaves * std::clamp (lastVcaOut, -1.5f, 1.5f);

            const float f2Octaves = p.f2EnvAmount * kFilterEnvOctaves * envF2
                                  + p.f2KeyTrack * keyTrackOctaves
                                  + dests[ModDest::Filter2Cutoff] * kModCutoffOctaves;

            cutoff1Smoother.setTarget (p.f1Cutoff);
            cutoff2Smoother.setTarget (p.f2Cutoff);
            reso1Smoother.setTarget  (std::clamp (p.f1Reso + dests[ModDest::Filter1Reso], 0.0f, 1.0f));
            reso2Smoother.setTarget  (std::clamp (p.f2Reso + dests[ModDest::Filter2Reso], 0.0f, 1.0f));
            drive1Smoother.setTarget (p.f1Drive * std::exp2 (dests[ModDest::Filter1Drive] * 3.0f));
            drive2Smoother.setTarget (p.f2Drive * std::exp2 (dests[ModDest::Filter2Drive] * 3.0f));

            if (! filterSmoothersPrimed)
            {
                // prima nota: si parte dal valore giusto, niente rampa da zero
                for (auto* sm : { &cutoff1Smoother, &cutoff2Smoother, &reso1Smoother,
                                  &reso2Smoother, &drive1Smoother, &drive2Smoother })
                    sm->snap (sm->getTarget());
                filterSmoothersPrimed = true;
            }

            const float f1Hz = std::clamp (cutoff1Smoother.next() * std::exp2 (f1Octaves),
                                           12.0f, (float) sampleRate * 0.48f);
            const float f2Hz = std::clamp (cutoff2Smoother.next() * std::exp2 (f2Octaves),
                                           12.0f, (float) sampleRate * 0.48f);

            filter1.setCutoff (f1Hz);
            filter2.setCutoff (f2Hz);

            filter1.setResonance (reso1Smoother.next());
            filter1.setDrive (drive1Smoother.next());
            filter2.setDrive (drive2Smoother.next());
            filter2.setResonance (reso2Smoother.next());

            // Il filtro lavora in mono sulla somma: due istanze stereo
            // raddoppierebbero il costo del blocco più caro della voce, e su un
            // basso la separazione stereo del filtro non è percepibile. Lo
            // stereo viene ricostruito dopo, dal pan degli oscillatori.
            float monoIn = (mixL + mixR) * 0.5f;
            float sideIn = (mixL - mixR) * 0.5f;

            // Ext Mode: l'audio esterno sostituisce gli oscillatori e passa
            // dal filtro e dal VCA, comandato dalle note come sul 303 modificato.
            // Il filtro è mono: si tiene solo il centro, altrimenti il lato
            // passerebbe non filtrato.
            if (extMode)
            {
                monoIn = (extL[startSample + n] + extR[startSample + n]) * 0.5f;
                sideIn = 0.0f;
            }

            // Neural Drive: preamp a diodi modellato da una GRU, prima del
            // filtro come l'overdrive di una bass line modificata.
            const float neural = std::clamp (p.dfNeuralDrive + dests[ModDest::FishNeuralDrive], 0.0f, 1.0f);

            if (neural > 0.0005f)
            {
                // Guadagno compensato: a drive 0 la rete è a guadagno ~1 (trim),
                // a drive pieno un segnale tipico sale di ~+7 dB; la compensazione
                // tiene il livello costante e lascia solo la compressione.
                const float wet = neuralDrive.process (monoIn, neural) * kNeuralOutputTrim / (1.0f + 1.2f * neural);
                monoIn = lerp (monoIn, wet, std::min (1.0f, neural * 4.0f));
            }

            const float blend = std::clamp (p.filterBlend + dests[ModDest::FilterBlend], 0.0f, 1.0f);

            // --- Sezione filtro con oversampling ---------------------------
            //
            // Upsampling a mantenimento (ZOH) e decimazione con Butterworth
            // del quarto ordine. Lo ZOH introduce una leggera attenuazione in
            // alta banda (sinc), trascurabile perché la banda che ci interessa
            // sta ben sotto la Nyquist di base; in cambio non costa nulla,
            // mentre un interpolatore polifase costerebbe più del filtro che
            // stiamo proteggendo.
            float filtered = 0.0f;

            for (int os = 0; os < filterOversample; ++os)
            {
                float y;

                switch (p.filterRouting)
                {
                    case 1:   // parallelo
                    {
                        const float a = filter1.process (monoIn);
                        const float b = filter2.process (monoIn);
                        y = lerp (a, b, blend);
                        break;
                    }

                    case 2:   // split: F2 riceve ciò che F1 scarta
                    {
                        const float a = filter1.process (monoIn);
                        const float b = filter2.process (monoIn - a);
                        y = a + b * blend;
                        break;
                    }

                    default:  // seriale
                    {
                        y = filter2.process (filter1.process (monoIn));
                        break;
                    }
                }

                if (filterOversample > 1)
                {
                    y = decimator[0].process (y);
                    y = decimator[1].process (y);
                }

                // Si conserva solo l'ultimo campione del gruppo: è la
                // decimazione vera e propria, ed è corretta perché il filtro
                // sopra ha già rimosso tutto ciò che ripiegherebbe.
                filtered = y;
            }

            // --- 6. VCA -------------------------------------------------------------
            const float ampEnv = sources[ModSource::Env1];

            const float accentGain = 1.0f + p.accentVca * p.accentAmount * accentLevel * 0.9f;
            const float velGain = lerp (1.0f, currentVelocity, p.velocityDepth);
            const float ampMod = 1.0f + dests[ModDest::AmpLevel];

            const float gain = ampEnv * accentGain * velGain * std::max (0.0f, ampMod);

            float yL = (filtered + sideIn) * gain;
            float yR = (filtered - sideIn) * gain;

            lastVcaOut = filtered * gain;

            // Muffler (Devil Fish): saturazione morbida sull'uscita del VCA.
            // Abbassa la soglia con la quantità: i picchi dell'accento si
            // arrotondano e il basso resta pieno.
            const float muffler = p.dfMuffler;

            if (muffler > 0.0005f)
            {
                const float t = 1.0f - 0.85f * muffler;
                const float inv = 1.0f / t;
                yL = lerp (yL, t * fastTanh (yL * inv), muffler);
                yR = lerp (yR, t * fastTanh (yR * inv), muffler);
            }

            // Pan della voce.
            const float panMod = dests[ModDest::AmpPan];

            if (std::abs (panMod) > 1.0e-5f)
            {
                float pl, pr;
                constantPowerPan (std::clamp (panMod, -1.0f, 1.0f), pl, pr);
                yL *= pl * kSqrt2;
                yR *= pr * kSqrt2;
            }

            outL[startSample + n] += dcBlockL.process (yL);
            outR[startSample + n] += dcBlockR.process (yR);
        }

        // La voce si libera quando l'inviluppo di ampiezza ha finito. Liberarla
        // prima taglierebbe la coda; liberarla mai esaurirebbe la polifonia.
        if (! envelopes[0].isActive())
        {
            active = false;
            noteNumber = -1;
        }
    }

private:
    //==========================================================================
    /** Ampiezza massima dello sweep dell'inviluppo di filtro, in ottave. 6.5
        copre l'intera escursione utile: da un cutoff chiuso a 80 Hz fino a
        7 kHz, che è il punto oltre il quale un basso non ha più contenuto. */
    static constexpr float kFilterEnvOctaves = 6.5f;
    static constexpr float kAccentOctaves    = 4.0f;

    /** Glide come lo slide del 303: RC sulla tensione di controllo, cioè in
        dominio logaritmico (ottave), non in Hz. Il parametro è il tempo per
        coprire il 90% dell'intervallo (= 2.3 costanti di tempo). Misurato sulle
        registrazioni di un 303: 10-90% in ~25-30 ms, uguale per ogni intervallo. */
    static float glideTauMs (float glideSeconds) noexcept { return glideSeconds * 1000.0f / 2.3f; }
    static constexpr float kModCutoffOctaves = 8.0f;
    static constexpr float kFilterFmOctaves  = 3.0f;

    /** Allineamento di livello dell'uscita della rete al segnale pulito,
        misurato sul set di test (vedi tools/neural). */
    static constexpr float kNeuralOutputTrim = 0.6f;

    /** Destinazioni che richiedono ricalcoli costosi: applicate ogni 32
        campioni invece che per campione. */
    void applyControlRateModulation (const SynthParams& p) noexcept
    {
        const float detuneMod = dests[ModDest::UnisonDetune];

        for (int o = 0; o < kNumOscillators; ++o)
        {
            const auto& op = p.osc[(size_t) o];
            oscillators[(size_t) o].setUnison (op.unisonVoices,
                                               std::clamp (op.unisonDetune + detuneMod, 0.0f, 1.0f),
                                               op.unisonBlend, op.unisonSpread, op.phaseRandom);
        }

        const ModDest rateDest[kNumLFOs] = { ModDest::LFO1Rate, ModDest::LFO2Rate, ModDest::LFO3Rate, ModDest::LFO4Rate };

        for (int l = 0; l < kNumLFOs; ++l)
        {
            const float m = dests[rateDest[l]];

            if (std::abs (m) > 1.0e-5f || lfoRateModulated[(size_t) l])
            {
                lfos[(size_t) l].setRateHz (p.lfo[(size_t) l].rateHz * std::exp2 (m * 4.0f));
                lfoRateModulated[(size_t) l] = std::abs (m) > 1.0e-5f;
            }
        }

        if (std::abs (dests[ModDest::Env2Decay]) > 1.0e-5f || envDecayModulated[0])
        {
            envelopes[1].setDecay (p.env[1].decay * std::exp2 (dests[ModDest::Env2Decay] * 3.0f));
            envDecayModulated[0] = std::abs (dests[ModDest::Env2Decay]) > 1.0e-5f;
        }

        if (std::abs (dests[ModDest::Env3Decay]) > 1.0e-5f || envDecayModulated[1])
        {
            envelopes[2].setDecay (p.env[2].decay * std::exp2 (dests[ModDest::Env3Decay] * 3.0f));
            envDecayModulated[1] = std::abs (dests[ModDest::Env3Decay]) > 1.0e-5f;
        }
    }

    /** Fattore massimo di oversampling della sezione filtro. Determina la
        dimensione dei buffer allocati in prepare(). */
    static constexpr int kMaxFilterOversample = 8;

    float noteToHz (int midiNote) const noexcept
    {
        const float tune = (params != nullptr) ? params->masterTune : 0.0f;
        return midiToHz ((float) midiNote + tune * 0.01f);
    }

    //==========================================================================
    double sampleRate = 44100.0;
    const WavetableBank* wavetableBank = nullptr;
    const Wavetable* userTable = nullptr;

public:
    void setUserTable (const Wavetable* t) noexcept { userTable = t; }
private:
    const SynthParams* params = nullptr;
    const ModMatrix* modMatrix = nullptr;

    std::array<WavetableOscillator, kNumOscillators> oscillators;
    SubOscillator subOsc;
    std::array<float, kNumOscillators> lastOscOut {};   // per l'auto-FM
    NoiseGenerator noise;

    MultiFilter filter1, filter2;
    std::array<TPTLowpass2, 2> decimator;
    int filterOversample = 2;

    std::array<Envelope, kNumEnvelopes> envelopes;
    std::array<LFO, kNumLFOs> lfos;
    AccentCircuit accent;

    ParamSmoother glide, cutoff1Smoother, cutoff2Smoother;
    ParamSmoother level1Smoother, level2Smoother, level3Smoother;
    ParamSmoother reso1Smoother, reso2Smoother, drive1Smoother, drive2Smoother;
    bool filterSmoothersPrimed = false;
    DCBlocker dcBlockL, dcBlockR;

    ModMatrix::SourceValues sources;
    ModMatrix::DestValues   dests;

    Xorshift rng { 0x3C6EF35Fu };

    bool  active = false;
    int   noteNumber = -1;
    uint32_t noteOnCounter = 0;
    float currentVelocity = 1.0f;
    float modWheel = 0.0f, aftertouch = 0.0f, pitchBendSemitones = 0.0f;
    float randomPerNote = 0.0f, randomUnipolar = 0.5f;
    float sequencerValue = 0.0f;

    NeuralDrive neuralDrive;
    float lastVcaOut = 0.0f;
    float vibratoPhase = 0.0f;
    bool  noteAccented = false;
    const float* extL = nullptr;
    const float* extR = nullptr;
    std::array<bool, kNumLFOs> lfoRateModulated {};
    std::array<bool, 2> envDecayModulated {};
};

} // namespace bassx
