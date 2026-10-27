/*
  ==============================================================================

    BASSX — PluginProcessor.cpp

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

using namespace bassx;

//==============================================================================
BassXAudioProcessor::BassXAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                              // Ingresso per la Ext Mode: disattivato di default, l'host lo
                              // espone come ingresso sidechain/audio dello strumento.
                              .withInput  ("Ext In", juce::AudioChannelSet::stereo(), false)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BASSX", createParameterLayout()),
      presetManager (apvts, modMatrix, sequencer)
{
    // Il banco di wavetable viene costruito qui, al primo utilizzo del
    // processo. Sono ~100 ms di IFFT una volta sola: farlo in prepareToPlay
    // significherebbe rifarlo a ogni cambio di sample rate, e farlo pigramente
    // al primo processBlock significherebbe un buco audio all'inserimento del
    // plugin.
    juce::ignoreUnused (wavetableBank->getNumTables());

    params.updateFrom (apvts);
    customCurve = SidechainDucker::getFactoryShape (0);
}

BassXAudioProcessor::~BassXAudioProcessor() = default;

//==============================================================================
double BassXAudioProcessor::getTailLengthSeconds() const
{
    // Il riverbero è la coda più lunga della catena. Dichiararla troppo corta
    // fa tagliare la coda quando l'host esegue il freeze o il bounce offline.
    return params.reverbOn ? (double) params.reverbDecay + 1.0 : 2.0;
}

//==============================================================================
void BassXAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize  = samplesPerBlock;

    for (auto& v : voices)
        v.prepare (sampleRate, *wavetableBank);

    playEngine.prepare (sampleRate);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) juce::jmax (1, samplesPerBlock);
    spec.numChannels = 2;

    tube.prepare (spec);
    distortion.prepare (spec);
    eq.prepare (spec);
    chorus.prepare (spec);
    phaser.prepare (spec);
    delay.prepare (spec);
    reverb.prepare (spec);
    compressor.prepare (spec);
    space.prepare (spec);
    limiter.prepare (spec);
    sidechain.prepare (sampleRate);
    lastScShape = -1;
    glitch.prepare (sampleRate);
    beatRepeat.prepare (sampleRate);

    extBuffer.setSize (2, juce::jmax (1, samplesPerBlock), false, true, false);
    combinedMidi.ensureSize (4096);

    outputGainSmoother.prepare (sampleRate, 15.0f);
    outputGainSmoother.snap (dbToGain (params.outputGain));

    sequencerEvents.ensureStorageAllocated (1024);
    heldNotes.ensureStorageAllocated (128);

    // La latenza è la somma dei due stadi che ne introducono: l'oversampler
    // della distorsione e il lookahead del limiter. Dichiararla è obbligatorio,
    // altrimenti l'host non compensa e la traccia risulta in ritardo rispetto
    // alle altre.
    reportedLatency = tube.getLatencySamples() + distortion.getLatencySamples() + limiter.getLatencySamples();
    setLatencySamples (reportedLatency);

    allNotesOff (false);
}

void BassXAudioProcessor::releaseResources()
{
    for (auto& v : voices)
        v.reset();

    tube.reset();
    distortion.reset();
    eq.reset();
    chorus.reset();
    phaser.reset();
    delay.reset();
    reverb.reset();
    compressor.reset();
    space.reset();
    limiter.reset();
    sidechain.reset();
    glitch.reset();
    beatRepeat.reset();
    playEngine.reset();
}

bool BassXAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;

    const auto& in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

//==============================================================================
// Gestione delle voci
//==============================================================================

Voice* BassXAudioProcessor::findVoicePlayingNote (int midiNote)
{
    for (auto& v : voices)
        if (v.isActive() && v.getNoteNumber() == midiNote)
            return &v;

    return nullptr;
}

Voice* BassXAudioProcessor::findVoiceToSteal()
{
    // Priorità di furto:
    //   1. una voce libera;
    //   2. la voce in release più avanzata (meno udibile);
    //   3. la voce più vecchia.
    // Rubare la voce più vecchia per prima, come fanno molte implementazioni
    // semplici, taglia la nota di basso tenuta sotto un accordo — l'errore più
    // evidente possibile su uno strumento che suona bassi.
    Voice* oldestReleasing = nullptr;
    Voice* oldest = nullptr;
    float lowestLevel = 2.0f;
    uint32_t oldestOrder = std::numeric_limits<uint32_t>::max();

    const int limit = juce::jlimit (1, kMaxPolyVoices, params.polyVoices);

    for (int i = 0; i < limit; ++i)
    {
        auto& v = voices[(size_t) i];

        if (! v.isActive())
            return &v;

        if (v.isReleasing())
        {
            const float level = v.getAmpEnvLevel();

            if (level < lowestLevel)
            {
                lowestLevel = level;
                oldestReleasing = &v;
            }
        }

        if (v.getNoteOnOrder() < oldestOrder)
        {
            oldestOrder = v.getNoteOnOrder();
            oldest = &v;
        }
    }

    if (oldestReleasing != nullptr)
        return oldestReleasing;

    return oldest;
}

//==============================================================================
void BassXAudioProcessor::noteOn (int midiNote, float velocity, bool isAccent, bool isSlide)
{
    const bool isMono = (params.voiceMode != 2);

    if (isMono)
    {
        auto& v = voices[0];

        // In modalità 303 lo slide è implicito nel legato: una nota che arriva
        // mentre un'altra è ancora premuta scivola e non ritriggera. In modalità
        // "Mono Legato" vale lo stesso; la differenza fra le due è che la 303
        // forza il retrigger degli inviluppi a ogni nota non legata anche se il
        // parametro di retrigger dell'oscillatore è disattivato.
        const bool legato = isSlide || (monoNoteIsOn && params.voiceMode == 0)
                                    || (monoNoteIsOn && params.voiceMode == 1);

        v.setModWheel (modWheel);
        v.setAftertouch (aftertouch);
        v.setPitchBend (pitchBendSemitones);
        v.startNote (midiNote, velocity, isAccent, legato, ++noteOnCounter);

        monoCurrentNote = midiNote;
        monoNoteIsOn = true;
        return;
    }

    // --- Polifonico -----------------------------------------------------------
    // Una nota già suonante viene ritriggerata sulla stessa voce: allocarne una
    // seconda produrrebbe un raddoppio di fase e quindi un salto di 6 dB.
    if (auto* existing = findVoicePlayingNote (midiNote))
    {
        existing->setModWheel (modWheel);
        existing->setAftertouch (aftertouch);
        existing->setPitchBend (pitchBendSemitones);
        existing->startNote (midiNote, velocity, isAccent, false, ++noteOnCounter);
        return;
    }

    if (auto* v = findVoiceToSteal())
    {
        if (v->isActive() && ! v->isReleasing())
            v->stopNote (false);

        v->setModWheel (modWheel);
        v->setAftertouch (aftertouch);
        v->setPitchBend (pitchBendSemitones);
        v->startNote (midiNote, velocity, isAccent, false, ++noteOnCounter);
    }
}

void BassXAudioProcessor::noteOff (int midiNote, bool fallbackToHeldNotes)
{
    const bool isMono = (params.voiceMode != 2);

    if (isMono)
    {
        // Le note del motore seq/arp non tornano ai tasti tenuti: quelli, col
        // motore attivo, servono a trasporre e a dare gli accordi, non a suonare.
        if (fallbackToHeldNotes && ! heldNotes.isEmpty())
        {
            // Torna alla nota premuta più di recente fra quelle ancora tenute,
            // scivolandoci. Senza questo, un trillo legato lascia silenzio ogni
            // volta che si rilascia la nota superiore.
            const int fallback = heldNotes.getLast();

            if (fallback != monoCurrentNote)
            {
                voices[0].slideTo (fallback);
                monoCurrentNote = fallback;
            }

            return;
        }

        if (midiNote == monoCurrentNote || monoCurrentNote < 0)
        {
            voices[0].stopNote (true);
            monoCurrentNote = -1;
            monoNoteIsOn = false;
        }

        return;
    }

    for (auto& v : voices)
        if (v.isActive() && v.getNoteNumber() == midiNote && ! v.isReleasing())
            v.stopNote (true);
}

void BassXAudioProcessor::allNotesOff (bool allowTailOff)
{
    heldNotes.clearQuick();
    monoCurrentNote = -1;
    monoNoteIsOn = false;

    for (auto& v : voices)
        v.stopNote (allowTailOff);

    if (! allowTailOff)
        for (auto& v : voices)
            v.reset();
}

//==============================================================================
void BassXAudioProcessor::handleMidiEvent (const juce::MidiMessage& message)
{
    // Sidechain MIDI: le note sul canale 16 fanno ripartire la curva e non suonano.
    if (params.scOn && params.scTrigger == (int) SidechainDucker::Trigger::Midi && message.getChannel() == 16)
    {
        if (message.isNoteOn())
            sidechain.trigger();
        return;
    }

    if (message.isNoteOn())
    {
        const int note = message.getNoteNumber();
        const float velocity = message.getFloatVelocity();

        // L'accento è determinato dalla velocity: è così che si comanda un
        // 303 da un piano roll, ed è l'unico modo che non richiede una traccia
        // di automazione separata.
        const bool isAccent = message.getVelocity() >= params.accentThreshold;

        const bool wasHolding = ! heldNotes.isEmpty();

        heldNotes.removeAllInstancesOf (note);
        heldNotes.add (note);

        // Motore seq/arp attivo: il tasto alimenta il motore e non suona.
        if (params.seqEnabled || params.arpEnabled)
            playEngine.keyDown (note, message.getVelocity(), params.arpLatch);
        else
            noteOn (note, velocity, isAccent, wasHolding);
    }
    else if (message.isNoteOff())
    {
        const int note = message.getNoteNumber();
        heldNotes.removeAllInstancesOf (note);

        if (params.seqEnabled || params.arpEnabled)
            playEngine.keyUp (note, params.arpLatch);
        else
            noteOff (note);
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        playEngine.clearKeys();
        allNotesOff (message.isAllNotesOff());
    }
    else if (message.isPitchWheel())
    {
        const float normalised = ((float) message.getPitchWheelValue() - 8192.0f) / 8192.0f;
        pitchBendSemitones = normalised * params.bendRange;

        for (auto& v : voices)
            v.setPitchBend (pitchBendSemitones);
    }
    else if (message.isController())
    {
        if (message.getControllerNumber() == 1)          // mod wheel
        {
            modWheel = (float) message.getControllerValue() / 127.0f;

            for (auto& v : voices)
                v.setModWheel (modWheel);
        }
        else if (message.getControllerNumber() == 123)   // all notes off
        {
            allNotesOff (true);
        }
        else if (message.getControllerNumber() == 64)    // sustain
        {
            // Il sustain non è implementato come pedale che trattiene le note
            // perché su uno strumento monofonico legato produrrebbe un
            // comportamento ambiguo. Lo ignoriamo esplicitamente invece di
            // lasciarlo cadere in un ramo non gestito.
        }
    }
    else if (message.isChannelPressure())
    {
        applyPolyPressure ((float) message.getChannelPressureValue() / 127.0f);
    }
    else if (message.isAftertouch())
    {
        applyPolyPressure ((float) message.getAfterTouchValue() / 127.0f);
    }
}

void BassXAudioProcessor::applyPolyPressure (float value)
{
    aftertouch = std::clamp (value, 0.0f, 1.0f);

    for (auto& v : voices)
        v.setAftertouch (aftertouch);
}

//==============================================================================
void BassXAudioProcessor::handleGuiNoteOn (int midiNote, float velocity)
{
    const juce::ScopedLock sl (guiMidiLock);
    guiMidiBuffer.addEvent (juce::MidiMessage::noteOn (1, midiNote, velocity), 0);
}

void BassXAudioProcessor::handleGuiNoteOff (int midiNote)
{
    const juce::ScopedLock sl (guiMidiLock);
    guiMidiBuffer.addEvent (juce::MidiMessage::noteOff (1, midiNote), 0);
}

//==============================================================================
void BassXAudioProcessor::updateEffectParameters (double bpm)
{
    {
        eta::EngineParams tp;
        tp.profile    = params.tubeModel;
        tp.drive      = params.tubeDrive;
        tp.bias       = params.tubeBias;
        tp.sag        = params.tubeSag;
        tp.warmthDb   = params.tubeWarmth;
        tp.presenceDb = params.tubePresence;
        tp.toneHz     = params.tubeTone;
        tp.character  = params.tubeCharacter;
        tp.age        = params.tubeAge;
        tube.setParams (tp);
        tube.setMix (params.tubeOn ? params.tubeMix : 0.0f);
        tube.setOutputDb (params.tubeOutput);
    }

    distortion.setAlgorithm ((Distortion::Algorithm)
        juce::jlimit (0, (int) Distortion::Algorithm::NumAlgorithms - 1, params.distAlgo));
    distortion.setDrive (std::clamp (params.distDrive + globalMod[ModDest::FxDriveAmount], 0.0f, 1.0f));
    distortion.setTone (params.distTone);
    distortion.setMix (params.distOn ? params.distMix : 0.0f);
    distortion.setBits (params.distBits);
    distortion.setDownsample (params.distDownsample);

    eq.setLowShelf  (params.eqLowFreq,  params.eqLowGain);
    eq.setPeak      (params.eqMidFreq,  params.eqMidGain, params.eqMidQ);
    eq.setHighShelf (params.eqHighFreq, params.eqHighGain);

    chorus.setRate (params.chorusRate);
    chorus.setDepth (std::clamp (params.chorusDepth + globalMod[ModDest::FxChorusDepth], 0.0f, 1.0f));
    chorus.setSpread (params.chorusSpread);
    chorus.setVoices (params.chorusVoices);
    chorus.setMix (params.chorusOn ? params.chorusMix : 0.0f);

    phaser.setRate (params.phaserRate);
    phaser.setDepth (params.phaserDepth);
    phaser.setCentreFrequency (params.phaserFreq * std::exp2 (globalMod[ModDest::FxPhaserFreq] * 3.0f));
    phaser.setFeedback (params.phaserFeedback);
    phaser.setNumStages (params.phaserStages);
    phaser.setMix (params.phaserOn ? params.phaserMix : 0.0f);

    // Tempi del delay: in modalità sync derivano dal tempo dell'host, così un
    // cambio di BPM sposta le ripetizioni invece di scollarle dalla griglia.
    if (params.delaySync)
    {
        const double secondsPerBeat = 60.0 / juce::jmax (1.0, bpm);
        const float beatsL = LFO::getSyncDivisionInBeats (params.delayDivL);
        const float beatsR = LFO::getSyncDivisionInBeats (params.delayDivR);

        delay.setTimeSeconds ((float) (secondsPerBeat * beatsL),
                              (float) (secondsPerBeat * beatsR));
    }
    else
    {
        delay.setTimeSeconds (params.delayTimeL, params.delayTimeR);
    }

    delay.setFeedback (std::clamp (params.delayFeedback + globalMod[ModDest::FxDelayFeedback], 0.0f, 1.0f));
    delay.setPingPong (params.delayPingPong);
    delay.setLowpass (params.delayLowpass);
    delay.setHighpass (params.delayHighpass);
    delay.setMix (params.delayOn ? std::clamp (params.delayMix + globalMod[ModDest::FxDelayMix], 0.0f, 1.0f) : 0.0f);

    reverb.setSize (params.reverbSize);
    reverb.setDecay (params.reverbDecay);
    reverb.setDamping (params.reverbDamping);
    reverb.setLowCut (params.reverbLowCut);
    reverb.setPreDelay (params.reverbPreDelay);
    reverb.setWidth (params.reverbWidth);
    reverb.setModulation (params.reverbMod);
    reverb.setMix (params.reverbOn ? std::clamp (params.reverbMix + globalMod[ModDest::FxReverbMix], 0.0f, 1.0f) : 0.0f);

    compressor.setThreshold (params.compThreshold);
    compressor.setRatio (params.compRatio);
    compressor.setAttack (params.compAttack);
    compressor.setRelease (params.compRelease);
    compressor.setKnee (params.compKnee);
    compressor.setMakeup (params.compMakeup);
    compressor.setRmsMode (params.compRms);
    compressor.setMix (params.compOn ? params.compMix : 0.0f);

    space.setEnabled (params.spaceOn);
    space.setPan (params.spacePan + globalMod[ModDest::FxSpacePan]);
    space.setWidth (params.spaceWidth + globalMod[ModDest::FxSpaceWidth]);
    space.setMonoFrequency (params.spaceMonoFreq);
    space.setHaas (params.spaceHaas);
    space.setRotation (params.spaceRotate);
    space.setAutopan (std::clamp (params.spaceAutopan + globalMod[ModDest::FxAutopanDepth], 0.0f, 1.0f),
                      LFO::getSyncDivisionInBeats (params.spaceAutopanDiv));

    glitch.set (params.glitchOn, LFO::getSyncDivisionInBeats (params.glitchRate), params.glitchAmount,
                params.glitchMix, params.glitchMode, params.glitchSeed);
    beatRepeat.set (params.brepOn, LFO::getSyncDivisionInBeats (params.brepInterval),
                    LFO::getSyncDivisionInBeats (params.brepGrid), params.brepChance,
                    LFO::getSyncDivisionInBeats (params.brepGate), params.brepDecay, params.brepMix);

    limiter.setEnabled (params.limiterOn);
    limiter.setCeiling (params.limiterCeiling);

    outputGainSmoother.setTarget (dbToGain (params.outputGain));
}

//==============================================================================
void BassXAudioProcessor::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    auto* left  = buffer.getWritePointer (0);
    auto* right = (buffer.getNumChannels() > 1) ? buffer.getWritePointer (1) : left;

    const int limit = (params.voiceMode == 2)
                        ? juce::jlimit (1, kMaxPolyVoices, params.polyVoices)
                        : 1;

    for (int i = 0; i < limit; ++i)
        voices[(size_t) i].renderNextBlock (left, right, startSample, numSamples);
}

//==============================================================================
void BassXAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    // --- 0. Ingresso esterno (Ext Mode) ---------------------------------------
    // Va copiato PRIMA di azzerare il buffer: ingresso e uscita condividono gli
    // stessi canali.
    const int numInputs = getTotalNumInputChannels();
    meters.extInputConnected.store (numInputs > 0);

    if (numInputs > 0 && numSamples <= extBuffer.getNumSamples())
    {
        const float extGain = dbToGain (params.dfExtGain);

        for (int ch = 0; ch < 2; ++ch)
        {
            const int srcCh = juce::jmin (ch, numInputs - 1);
            extBuffer.copyFrom (ch, 0, buffer, srcCh, 0, numSamples);
            extBuffer.applyGain (ch, 0, numSamples, extGain);
        }

        for (auto& v : voices)
            v.setExternalInput (extBuffer.getReadPointer (0), extBuffer.getReadPointer (1));
    }
    else
    {
        for (auto& v : voices)
            v.setExternalInput (nullptr, nullptr);
    }

    buffer.clear();

    // --- 1. Parametri ---------------------------------------------------------
    params.updateFrom (apvts);

    double bpm = 120.0, ppq = 0.0;
    bool isPlaying = false;

    if (auto* transport = getPlayHead())
    {
        if (const auto position = transport->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);
            ppq = position->getPpqPosition().orFallback (0.0);
            isPlaying = position->getIsPlaying();
        }
    }

    lastBpm.store (bpm);
    const int oversampleFactor = 1 << juce::jlimit (0, 3, params.oversampling);

    const auto* userTable = getSampleWavetable();
    for (auto& v : voices)
    {
        v.setUserTable (userTable);
        v.setParameters (params, modMatrix);
        v.setFilterOversampling (oversampleFactor);
        v.setTransport (bpm, ppq, isPlaying);
    }

    updateEffectParameters (bpm);

    // --- 2. Sequencer / arpeggiatore -----------------------------------------
    sequencer.setPattern (params.seqPattern);
    sequencer.setLength (params.seqLength);
    sequencer.setDirection (params.seqDirection);

    const bool engineActive = params.seqEnabled || params.arpEnabled;

    if (engineActive != engineWasActive)
    {
        // Cambio di modalità: le note suonate direttamente (o dal motore)
        // non riceverebbero più il loro note-off dalla strada giusta.
        allNotesOff (true);
        playEngine.clearKeys();

        if (engineActive)
            for (int n : heldNotes)
                playEngine.keyDown (n, 100, params.arpLatch);

        engineWasActive = engineActive;
    }

    sequencerEvents.clearQuick();

    // tasti (anche in latch) per l'export del loop
    {
        const int nk = juce::jmin (kMaxExportKeys, playEngine.getNumKeys());
        for (int k = 0; k < nk; ++k) exportKeys[(size_t) k].store (playEngine.getKey (k), std::memory_order_relaxed);
        numExportKeys.store (nk, std::memory_order_relaxed);
    }

    {
        const bool hostPlaying = isPlaying && getPlayHead() != nullptr;
        playEngine.processBlock (sequencer, makeEngineSettings(), bpm, ppq, hostPlaying,
                                 numSamples, sequencerEvents);
    }

    // --- 3. MIDI dalla GUI -----------------------------------------------------
    combinedMidi.clear();
    combinedMidi.addEvents (midiMessages, 0, numSamples, 0);

    {
        const juce::ScopedTryLock sl (guiMidiLock);

        if (sl.isLocked() && ! guiMidiBuffer.isEmpty())
        {
            combinedMidi.addEvents (guiMidiBuffer, 0, numSamples, 0);
            guiMidiBuffer.clear();
        }
    }

    // --- 4. Render con eventi campione-esatti -----------------------------------
    //
    // Il blocco viene spezzato a ogni evento, MIDI o del sequencer. Senza,
    // il timing quantizzerebbe alla dimensione del buffer: a 512 campioni e
    // 48 kHz sono ~10 ms di jitter, chiaramente udibili su una linea in
    // sedicesimi.
    int position = 0;
    int seqIndex = 0;

    auto midiIterator = combinedMidi.begin();
    const auto midiEnd = combinedMidi.end();

    while (position < numSamples)
    {
        int nextEventPos = numSamples;

        if (midiIterator != midiEnd)
            nextEventPos = juce::jmin (nextEventPos,
                                       juce::jlimit (0, numSamples, (*midiIterator).samplePosition));

        if (seqIndex < sequencerEvents.size())
            nextEventPos = juce::jmin (nextEventPos,
                                       juce::jlimit (0, numSamples, sequencerEvents[seqIndex].samplePosition));

        if (nextEventPos > position)
        {
            renderVoices (buffer, position, nextEventPos - position);
            position = nextEventPos;
        }

        // Consuma tutti gli eventi che cadono a questa posizione o prima.
        // Il confronto è "<=" e non "==" per assorbire eventuali posizioni
        // fuori ordine o negative fornite da un host mal implementato: senza,
        // un evento con posizione arretrata non verrebbe mai consumato e il
        // ciclo non terminerebbe.
        while (midiIterator != midiEnd
               && juce::jlimit (0, numSamples, (*midiIterator).samplePosition) <= position)
        {
            handleMidiEvent ((*midiIterator).getMessage());
            ++midiIterator;
        }

        while (seqIndex < sequencerEvents.size()
               && juce::jlimit (0, numSamples, sequencerEvents[seqIndex].samplePosition) <= position)
        {
            const auto& e = sequencerEvents.getReference (seqIndex);

            if (e.type == SeqEvent::Type::NoteOn)
            {
                for (auto& v : voices)
                    v.setSequencerValue (e.modValue);

                noteOn (e.midiNote, e.velocity, e.accent, e.slide);
            }
            else
            {
                noteOff (e.midiNote, false);
            }

            ++seqIndex;
        }

        // Esauriti gli eventi: si renderizza il resto del blocco in un colpo
        // solo e si esce. Ogni iterazione o avanza `position` o consuma almeno
        // un evento, quindi la terminazione è garantita.
        if (midiIterator == midiEnd && seqIndex >= sequencerEvents.size())
        {
            if (position < numSamples)
                renderVoices (buffer, position, numSamples - position);

            break;
        }
    }

    // --- 5. Catena effetti -------------------------------------------------------
    juce::dsp::AudioBlock<float> block (buffer);

    updateGlobalModulation();
    space.setTransport (bpm, ppq, isPlaying);
    fxBpm = bpm; fxPpq = ppq; fxPlaying = isPlaying;
    tube.process (block);   // preamp valvolare: sempre in testa, latenza costante
    processFxChain (block);

    // Sidechain: ultimo stadio prima del volume, come Kickstart sul canale.
    {
        const int w0 = analyserWrite.load (std::memory_order_relaxed);
        int w = w0;
        for (int i = 0; i < numSamples; ++i)
        {
            analyserPre[(size_t) w].store (0.5f * (buffer.getSample (0, i) + buffer.getSample (buffer.getNumChannels() - 1, i)), std::memory_order_relaxed);
            w = (w + 1) % kAnalyserSize;
        }

        updateSidechain (bpm);
        const bool extOk = numInputs > 0 && numSamples <= extBuffer.getNumSamples();
        sidechain.process (block, bpm, ppq, isPlaying,
                           extOk ? extBuffer.getReadPointer (0) : nullptr,
                           extOk ? extBuffer.getReadPointer (1) : nullptr);

        w = w0;
        for (int i = 0; i < numSamples; ++i)
        {
            analyserPost[(size_t) w].store (0.5f * (buffer.getSample (0, i) + buffer.getSample (buffer.getNumChannels() - 1, i)), std::memory_order_relaxed);
            w = (w + 1) % kAnalyserSize;
        }
        analyserWrite.store (w, std::memory_order_relaxed);
    }

    // --- 6. Volume di uscita ------------------------------------------------------
    {
        auto* left  = buffer.getWritePointer (0);
        auto* right = (buffer.getNumChannels() > 1) ? buffer.getWritePointer (1) : left;

        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGainSmoother.next();
            left[i]  *= g;
            right[i] *= g;
        }
    }

    limiter.process (block);

    // Se l'host ci ha dato un bus mono, la voce è già sommata sul canale 0.
    if (buffer.getNumChannels() == 1)
    {
        // Niente da fare: renderVoices ha scritto entrambi i canali sullo
        // stesso puntatore, quindi il canale 0 contiene già L+R.
    }

    // --- 7. Meter ------------------------------------------------------------------
    {
        float peakL = 0.0f, peakR = 0.0f;

        if (numSamples > 0)
        {
            peakL = buffer.getMagnitude (0, 0, numSamples);
            peakR = (buffer.getNumChannels() > 1) ? buffer.getMagnitude (1, 0, numSamples) : peakL;
        }

        // Decadimento lento verso il basso: un meter che segue il picco
        // istantaneo lampeggia in modo illeggibile.
        const float decay = 0.72f;
        meters.outputLevelL.store (juce::jmax (peakL, meters.outputLevelL.load() * decay));
        meters.outputLevelR.store (juce::jmax (peakR, meters.outputLevelR.load() * decay));

        meters.gainReduction.store (juce::jmax (compressor.getGainReductionDb(), limiter.getReductionDb()));
        meters.currentStep.store (sequencer.getCurrentStep());

        int active = 0;
        float accentCharge = 0.0f, ampEnv = 0.0f, filtEnv = 0.0f;

        for (auto& v : voices)
        {
            if (v.isActive())
            {
                ++active;
                accentCharge = juce::jmax (accentCharge, v.getAccentCharge());
                ampEnv = juce::jmax (ampEnv, v.getAmpEnvLevel());
                filtEnv = juce::jmax (filtEnv, v.getFilterEnvLevel());
            }
        }

        meters.activeVoices.store (active);
        meters.arpNote.store (playEngine.getLastPlayedArpNote());
        meters.correlation.store (space.getCorrelation());

        // Goniometro: un punto ogni 4 campioni basta a disegnare la figura.
        if (buffer.getNumChannels() > 1)
        {
            const auto* l = buffer.getReadPointer (0);
            const auto* r = buffer.getReadPointer (1);
            int w = scopeWrite.load (std::memory_order_relaxed);

            for (int i = scopeDecimation; i < numSamples; i += 4)
            {
                scopeL[(size_t) w].store (l[i], std::memory_order_relaxed);
                scopeR[(size_t) w].store (r[i], std::memory_order_relaxed);
                w = (w + 1) % kScopeSize;
            }

            scopeDecimation = (scopeDecimation + (4 - numSamples % 4)) % 4;
            scopeWrite.store (w, std::memory_order_relaxed);
        }
        meters.accentCharge.store (accentCharge);
        meters.ampEnvLevel.store (ampEnv);
        meters.filterEnvLevel.store (filtEnv);
        meters.filterCutoffHz.store (params.f1Cutoff);
    }
}

//==============================================================================
void BassXAudioProcessor::setSampleWavetableCycles (const std::vector<std::vector<float>>& cycles, const juce::String& name)
{
    if (cycles.empty()) return;
    const int next = userTableLive.load() == 0 ? 1 : 0;
    SampleWavetable::buildFromCycles (cycles, userTables[(size_t) next]);
    userTables[(size_t) next].name = name;
    userCycles = cycles;
    userTableName = name;
    userTableLive.store (next, std::memory_order_release);
    ++userTableVersion;
}

bool BassXAudioProcessor::loadSampleWavetable (const juce::File& file, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "Formato non supportato: " + file.getFileName(); return false; }

    // fino a 20 s: una wavetable racconta l'evoluzione, non un brano intero
    const int n = (int) std::min<long long> ((long long) reader->lengthInSamples, (long long) (reader->sampleRate * 20.0));
    juce::AudioBuffer<float> audio ((int) std::min<unsigned int> (2u, reader->numChannels), std::max (1, n));
    reader->read (&audio, 0, n, 0, true, audio.getNumChannels() > 1);

    Wavetable scratch;
    std::vector<std::vector<float>> cycles;
    bool pitched = false;
    if (! SampleWavetable::build (audio, reader->sampleRate, file.getFileNameWithoutExtension(), scratch, cycles, pitched))
    {
        error = "Campione vuoto o troppo corto: " + file.getFileName();
        return false;
    }

    setSampleWavetableCycles (cycles, file.getFileNameWithoutExtension());
    return true;
}

//==============================================================================
namespace
{
    struct ExportPlayHead : public juce::AudioPlayHead
    {
        double bpm = 120.0, ppq = 0.0;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (bpm); info.setPpqPosition (ppq); info.setIsPlaying (true);
            info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
            return info;
        }
    };
}

BassXAudioProcessor::LoopExportJob BassXAudioProcessor::makeLoopExportJob()
{
    LoopExportJob job;
    getStateInformation (job.state);
    for (int k = 0; k < numExportKeys.load(); ++k) job.keys.push_back (exportKeys[(size_t) k].load());
    job.sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
    job.bpm = lastBpm.load();
    job.engineOn = apvts.getRawParameterValue (ParamID::seqEnable)->load() > 0.5f
                || apvts.getRawParameterValue (ParamID::arpEnable)->load() > 0.5f;
    return job;
}

bool BassXAudioProcessor::exportLoop (const LoopExportJob& job, const juce::File& file, int bars, juce::String& error)
{
    if (! job.engineOn)
    {
        error = "Accendi il sequencer (SEQ) o l'arpeggiatore (ARP): l'export registra il loop che suonano.";
        return false;
    }

    const double sr = job.sampleRate;
    const double bpm = job.bpm;
    const int blockSize = 512;

    BassXAudioProcessor offline;
    offline.setStateInformation (job.state.getData(), (int) job.state.getSize());
    offline.setPlayConfigDetails (0, 2, sr, blockSize);
    offline.prepareToPlay (sr, blockSize);
    ExportPlayHead head; head.bpm = bpm;
    offline.setPlayHead (&head);

    // Due giri del loop: si tiene il secondo, che contiene gia' le code del
    // primo (delay, riverbero, release). Cosi' il file si ripete senza buco.
    const double samplesPerBeat = 60.0 / juce::jmax (20.0, bpm) * sr;
    const int loopLen = (int) std::lround (bars * 4.0 * samplesPerBeat);
    const int latency = offline.getLatencySamples();
    const int total = loopLen * 2 + latency;

    juce::AudioBuffer<float> out (2, total), buf (2, blockSize);

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int n = juce::jmin (blockSize, total - pos);
        buf.setSize (2, n, false, false, true);
        buf.clear();
        juce::MidiBuffer midi;
        if (pos == 0)
            for (int k : job.keys)                       // l'accordo dell'arpeggio resta tenuto
                midi.addEvent (juce::MidiMessage::noteOn (1, k, (juce::uint8) 100), 0);
        offline.processBlock (buf, midi);
        head.ppq += (double) n / samplesPerBeat;
        for (int ch = 0; ch < 2; ++ch) out.copyFrom (ch, pos, buf, ch, 0, n);
    }
    offline.setPlayHead (nullptr);

    juce::AudioBuffer<float> loop (2, loopLen);
    for (int ch = 0; ch < 2; ++ch)
        loop.copyFrom (ch, 0, out, ch, loopLen + latency, loopLen);

    if (loop.getMagnitude (0, loopLen) < 1.0e-5f)
    {
        error = "Il loop e' muto: con ARP servono dei tasti tenuti (o il LATCH), con SEQ un pattern con dei passi.";
        return false;
    }

    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr) { error = "Impossibile scrivere " + file.getFullPathName(); return false; }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr) { error = "Formato WAV non disponibile"; return false; }
    stream.release();                                    // ora lo possiede il writer
    writer->writeFromAudioSampleBuffer (loop, 0, loopLen);
    return true;
}

//==============================================================================
void BassXAudioProcessor::updateSidechain (double bpm)
{
    juce::ignoreUnused (bpm);
    SidechainDucker::Settings s;
    s.on            = params.scOn;
    s.trigger       = params.scTrigger;
    s.beatsPerCycle = LFO::getSyncDivisionInBeats (params.scRate);
    s.depth         = params.scDepth;
    s.phaseOffset   = params.scPhase;
    s.smoothMs      = params.scSmooth;
    s.bandLowHz     = params.scBandLow;
    s.bandHighHz    = juce::jmax (params.scBandHigh, params.scBandLow * 1.05f);
    s.thresholdDb   = params.scThreshold;
    sidechain.setSettings (s);

    // La tabella della curva si ricalcola solo quando cambia la forma (o la
    // CUSTOM viene ridisegnata). Il lock e' solo provato: mai attese qui.
    const bool custom = params.scShape == SidechainDucker::kCustomShape;
    if (params.scShape != lastScShape || (custom && customCurveDirty.load()))
    {
        if (! custom)
        {
            sidechain.setCurve (SidechainDucker::getFactoryShape (params.scShape));
            lastScShape = params.scShape;
        }
        else if (const juce::SpinLock::ScopedTryLockType tl (curveLock); tl.isLocked())
        {
            sidechain.setCurve (customCurve);
            customCurveDirty.store (false);
            lastScShape = params.scShape;
        }
    }
}

void BassXAudioProcessor::setSidechainCustomCurve (const SidechainDucker::Curve& c)
{
    {
        const juce::SpinLock::ScopedLockType sl (curveLock);
        customCurve = c;
    }
    customCurveDirty.store (true);
}

SidechainDucker::Curve BassXAudioProcessor::getSidechainCustomCurve() const
{
    const juce::SpinLock::ScopedLockType sl (curveLock);
    return customCurve;
}

SidechainDucker::Curve BassXAudioProcessor::getSidechainDisplayCurve() const
{
    const int shape = (int) apvts.getRawParameterValue (ParamID::scShape)->load();
    return shape == SidechainDucker::kCustomShape ? getSidechainCustomCurve()
                                                  : SidechainDucker::getFactoryShape (shape);
}

//==============================================================================
PlayEngine::Settings BassXAudioProcessor::makeEngineSettings() const noexcept
{
    PlayEngine::Settings s;
    s.seqOn          = params.seqEnabled;
    s.arpOn          = params.arpEnabled;
    s.combine        = params.playCombine;
    s.seqDivision    = params.seqDivision;
    s.seqSwing       = params.seqSwing;
    s.seqGate        = params.seqGateLength;
    s.seqOctave      = params.seqOctave;
    s.seqKeyFollow   = params.seqKeyFollow;
    s.arpMode        = params.arpMode;
    s.arpOctaves     = params.arpOctaves;
    s.arpDivision    = params.arpDivision;
    s.arpGate        = params.arpGate;
    s.arpSwing       = params.arpSwing;
    s.arpLatch       = params.arpLatch;
    s.arpSlide       = params.arpSlide;
    s.arpAccentEvery = params.arpAccent;
    s.accentThreshold = params.accentThreshold;
    return s;
}

void BassXAudioProcessor::updateGlobalModulation()
{
    // Gli effetti sono globali, la matrice è per voce: si usa la voce attiva
    // più recente. In mono (il caso del basso) è esattamente la voce che suona.
    const Voice* newest = nullptr;

    for (auto& v : voices)
        if (v.isActive() && (newest == nullptr || v.getNoteOnOrder() > newest->getNoteOnOrder()))
            newest = &v;

    if (newest != nullptr)
        globalMod = newest->getLastDests();
    else
        globalMod.reset();
}

void BassXAudioProcessor::processFxChain (juce::dsp::AudioBlock<float>& block)
{
    // Ogni slot della catena sceglie un effetto. Un effetto assegnato a più
    // slot suona solo nel primo: ogni processore ha un solo stato interno, e
    // processarlo due volte nello stesso blocco ne corromperebbe le code.
    // Un effetto non assegnato a nessuno slot è fuori catena.
    std::array<bool, (size_t) FxType::NumTypes> used {};

    for (int slot = 0; slot < kNumFxSlots; ++slot)
    {
        const int type = juce::jlimit (0, (int) FxType::NumTypes - 1, params.fxSlots[(size_t) slot]);

        if (used[(size_t) type])
            continue;

        used[(size_t) type] = true;

        switch ((FxType) type)
        {
            case FxType::Distortion: distortion.process (block); break;
            case FxType::EQ:         eq.process (block);         break;
            case FxType::Chorus:     chorus.process (block);     break;
            case FxType::Phaser:     phaser.process (block);     break;
            case FxType::Delay:      delay.process (block);      break;
            case FxType::Reverb:     reverb.process (block);     break;
            case FxType::Compressor: compressor.process (block); break;
            case FxType::Space:      space.process (block);      break;
            case FxType::Glitch:     glitch.process (block, fxBpm, fxPpq, fxPlaying);     break;
            case FxType::BeatRepeat: beatRepeat.process (block, fxBpm, fxPpq, fxPlaying); break;
            case FxType::NumTypes:
            default:                 break;
        }
    }

    // Effetti con code (linee di ritardo) rimasti fuori catena: le linee non
    // scorrono, quindi al rientro vanno svuotate invece di risuonare audio vecchio.
    if (! used[(size_t) FxType::Chorus]) chorus.markIdle();
    if (! used[(size_t) FxType::Phaser]) phaser.markIdle();
    if (! used[(size_t) FxType::Delay])  delay.markIdle();
    if (! used[(size_t) FxType::Reverb]) reverb.markIdle();

    // La distorsione introduce sempre la stessa latenza (dichiarata all'host):
    // se è fuori catena, il ritardo va applicato comunque.
    if (! used[(size_t) FxType::Distortion])
    {
        distortion.setMix (0.0f);
        distortion.process (block);
    }
}

//==============================================================================
// Programmi (esposti all'host come i preset di fabbrica)
//==============================================================================

// I preset NON sono esposti come "programmi" dell'host. Molti host (Ableton,
// Reaper, FL) richiamano setCurrentProgram subito dopo setStateInformation
// quando riaprono un progetto: il preset veniva ricaricato sopra lo stato
// salvato e le modifiche dell'utente sparivano. I preset si caricano solo dal
// browser del plugin.
int BassXAudioProcessor::getNumPrograms()                      { return 1; }
int BassXAudioProcessor::getCurrentProgram()                   { return 0; }
void BassXAudioProcessor::setCurrentProgram (int)              {}
const juce::String BassXAudioProcessor::getProgramName (int)   { return presetManager.getCurrentPresetName(); }

//==============================================================================
void BassXAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("BASSXSTATE");
    root.setProperty ("version", 1, nullptr);
    // Il nome del preset fa parte della sessione: riaprendo il progetto il
    // display mostra il preset vero e non il primo della lista.
    root.setProperty ("presetIndex", presetManager.getCurrentPresetIndex(), nullptr);
    root.setProperty ("presetName",  presetManager.getCurrentPresetName(), nullptr);

    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (modMatrix.toValueTree(), nullptr);
    root.appendChild (sequencer.toValueTree(), nullptr);

    if (! userCycles.empty())
    {
        // la wavetable da campione viaggia con la sessione: il file originale puo' sparire
        juce::ValueTree wt ("USER_WAVETABLE");
        wt.setProperty ("name", userTableName, nullptr);
        juce::MemoryBlock mb;
        for (const auto& c : userCycles) mb.append (c.data(), c.size() * sizeof (float));
        wt.setProperty ("frames", (int) userCycles.size(), nullptr);
        wt.setProperty ("data", mb.toBase64Encoding(), nullptr);
        root.appendChild (wt, nullptr);
    }

    {
        juce::ValueTree sc ("SIDECHAIN_CURVE");
        const auto c = getSidechainCustomCurve();
        for (int i = 0; i < c.numPoints; ++i)
        {
            juce::ValueTree pt ("PT");
            pt.setProperty ("x", c.points[(size_t) i].x, nullptr);
            pt.setProperty ("y", c.points[(size_t) i].y, nullptr);
            pt.setProperty ("t", c.points[(size_t) i].tension, nullptr);
            sc.appendChild (pt, nullptr);
        }
        root.appendChild (sc, nullptr);
    }

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void BassXAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    const auto root = juce::ValueTree::fromXml (*xml);

    if (! root.isValid())
        return;

    // Compatibilità: le prime versioni salvavano direttamente lo stato APVTS
    // come radice. Riconoscerlo evita di far perdere il lavoro a chi ha già
    // sessioni salvate.
    if (root.hasType (apvts.state.getType()))
    {
        apvts.replaceState (root);
        syncParametersToState();
        return;
    }

    if (! root.hasType ("BASSXSTATE"))
        return;

    const auto paramState = root.getChildWithName (apvts.state.getType());

    if (paramState.isValid())
    {
        apvts.replaceState (paramState);
        syncParametersToState();
    }

    modMatrix.fromValueTree (root.getChildWithName ("MODMATRIX"));
    sequencer.fromValueTree (root.getChildWithName ("SEQUENCER"));

    if (const auto wt = root.getChildWithName ("USER_WAVETABLE"); wt.isValid())
    {
        juce::MemoryBlock mb;
        const int frames = (int) wt.getProperty ("frames", 0);
        if (frames > 0 && mb.fromBase64Encoding (wt.getProperty ("data").toString())
            && mb.getSize() == (size_t) frames * kTableSize * sizeof (float))
        {
            std::vector<std::vector<float>> cycles ((size_t) frames, std::vector<float> ((size_t) kTableSize));
            for (int f = 0; f < frames; ++f)
                std::memcpy (cycles[(size_t) f].data(), (const char*) mb.getData() + (size_t) f * kTableSize * sizeof (float),
                             (size_t) kTableSize * sizeof (float));
            setSampleWavetableCycles (cycles, wt.getProperty ("name").toString());
        }
    }

    if (const auto sc = root.getChildWithName ("SIDECHAIN_CURVE"); sc.isValid() && sc.getNumChildren() >= 2)
    {
        SidechainDucker::Curve c;
        for (int i = 0; i < juce::jmin (sc.getNumChildren(), SidechainDucker::kMaxPoints); ++i)
        {
            const auto pt = sc.getChild (i);
            c.points[(size_t) c.numPoints++] = { (float) pt.getProperty ("x"), (float) pt.getProperty ("y"),
                                                 (float) pt.getProperty ("t") };
        }
        setSidechainCustomCurve (c);
    }

    presetManager.restoreSessionPreset ((int) root.getProperty ("presetIndex", 0),
                                        root.getProperty ("presetName", {}).toString());
}

//==============================================================================
void BassXAudioProcessor::syncParametersToState()
{
    // replaceState() aggiorna solo i parametri il cui valore (denormalizzato)
    // differisce da quello salvato. Un booleano impostato dall'host a 0.49
    // vale già "false" e non viene toccato, quindi il suo valore grezzo resta
    // 0.49 invece di tornare a 0: l'host vede uno stato non ripristinato
    // (pluginval, test "Plugin state"). Si riallinea ogni parametro al valore
    // esatto dello stato.
    for (auto* p : getParameters())
    {
        if (auto* withID = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto node = apvts.state.getChildWithProperty ("id", withID->paramID);

            if (node.isValid() && node.hasProperty ("value"))
            {
                const float normalised = withID->convertTo0to1 ((float) node.getProperty ("value"));

                if (std::abs (withID->getValue() - normalised) > 1.0e-6f)
                    withID->setValueNotifyingHost (normalised);
            }
        }
    }
}

juce::AudioProcessorEditor* BassXAudioProcessor::createEditor()
{
    return new BassXAudioProcessorEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BassXAudioProcessor();
}
