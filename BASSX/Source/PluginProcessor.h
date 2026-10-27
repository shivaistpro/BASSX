/*
  ==============================================================================

    BASSX — PluginProcessor.h

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "DSP/Voice.h"
#include "DSP/Sequencer.h"
#include "DSP/Effects.h"
#include "DSP/TubeStage.h"
#include "DSP/ModMatrix.h"
#include "DSP/WavetableBank.h"
#include "Presets/PresetManager.h"

//==============================================================================
class BassXAudioProcessor  : public juce::AudioProcessor
{
public:
    BassXAudioProcessor();
    ~BassXAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    //==========================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    //==========================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    juce::AudioProcessorValueTreeState apvts;
    bassx::ModMatrix modMatrix;
    bassx::Sequencer sequencer;
    bassx::PlayEngine playEngine;
    bassx::PresetManager presetManager;

    const bassx::WavetableBank& getWavetableBank() const noexcept { return *wavetableBank; }
    const bassx::SynthParams& getCurrentParams() const noexcept { return params; }

    //==========================================================================
    /** Valori letti dalla GUI. Atomici perché scritti dal thread audio e letti
        dal timer del thread messaggi. Nessun lock: una lettura leggermente
        stantia su un LED è invisibile, un priority inversion sul thread audio
        no. */
    struct MeterState
    {
        std::atomic<float> outputLevelL { 0.0f };
        std::atomic<float> outputLevelR { 0.0f };
        std::atomic<float> gainReduction { 0.0f };
        std::atomic<float> accentCharge { 0.0f };
        std::atomic<float> filterEnvLevel { 0.0f };
        std::atomic<float> ampEnvLevel { 0.0f };
        std::atomic<float> filterCutoffHz { 1000.0f };
        std::atomic<int>   currentStep { -1 };
        std::atomic<int>   activeVoices { 0 };
        std::atomic<int>   arpNote { -1 };
        std::atomic<float> correlation { 1.0f };
        std::atomic<bool>  extInputConnected { false };
    };

    /** Campioni L/R per il goniometro della pagina SPACE. Scritti dal thread
        audio (decimati), letti dal timer della GUI: anello senza lock, una
        lettura a cavallo di una scrittura mostra al più un punto stantio. */
    static constexpr int kScopeSize = 512;
    std::array<std::atomic<float>, kScopeSize> scopeL {}, scopeR {};
    std::atomic<int> scopeWrite { 0 };

    MeterState meters;

    //==========================================================================
    /** Sidechain: curva CUSTOM (thread messaggi) e stato per la GUI. */
    void setSidechainCustomCurve (const bassx::SidechainDucker::Curve& c);
    bassx::SidechainDucker::Curve getSidechainCustomCurve() const;
    /** Curva mostrata dall'editor: quella di fabbrica scelta o la CUSTOM. */
    bassx::SidechainDucker::Curve getSidechainDisplayCurve() const;
    float getSidechainPhase() const noexcept { return sidechain.getPhase(); }
    float getSidechainGain() const noexcept  { return sidechain.getGain(); }

    /** Analizzatore di spettro della pagina SIDECHAIN: mono a piena velocita',
        prima e dopo il ducking. Anelli senza lock come lo scope. */
    static constexpr int kAnalyserSize = 4096;
    std::array<std::atomic<float>, kAnalyserSize> analyserPre {}, analyserPost {};
    std::atomic<int> analyserWrite { 0 };

    //==========================================================================
    /** Wavetable da campione (thread messaggi): il file diventa la forma
        "Sample" degli oscillatori di QUESTA istanza. */
    bool loadSampleWavetable (const juce::File& file, juce::String& error);
    /** Da cicli gia' estratti (stato salvato, test). */
    void setSampleWavetableCycles (const std::vector<std::vector<float>>& cycles, const juce::String& name);
    const bassx::Wavetable* getSampleWavetable() const noexcept
    {
        const int live = userTableLive.load (std::memory_order_acquire);
        return live >= 0 ? &userTables[(size_t) live] : nullptr;
    }
    juce::String getSampleWavetableName() const { return userTableName; }
    int getSampleWavetableVersion() const noexcept { return userTableVersion.load(); }

    //==========================================================================
    /** Esporta in WAV il loop che seq/arp stanno suonando: BARS battute al BPM
        dell'host, a loop chiuso (code di delay/riverbero comprese, latenza
        compensata). Lavora su una copia offline dello strumento: si puo'
        chiamare da un thread qualsiasi mentre il plugin suona. */
    struct LoopExportJob
    {
        juce::MemoryBlock state;
        std::vector<int> keys;
        double sampleRate = 48000.0, bpm = 120.0;
        bool engineOn = false;
    };
    /** Thread messaggi: fotografa stato, tasti e tempo. */
    LoopExportJob makeLoopExportJob();
    /** Qualsiasi thread: non tocca questo processore (il plugin puo' anche
        essere chiuso nel frattempo). */
    static bool exportLoop (const LoopExportJob& job, const juce::File& file, int bars, juce::String& error);
    double getLastBpm() const noexcept { return lastBpm.load(); }

    /** Note suonata dalla GUI (tastiera su schermo). */
    void handleGuiNoteOn (int midiNote, float velocity);
    void handleGuiNoteOff (int midiNote);

private:
    //==========================================================================
    void updateEffectParameters (double bpm);
    void renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void handleMidiEvent (const juce::MidiMessage& message);
    void applyPolyPressure (float value);

    void noteOn (int midiNote, float velocity, bool isAccent, bool isSlide);
    void noteOff (int midiNote, bool fallbackToHeldNotes = true);
    void processFxChain (juce::dsp::AudioBlock<float>& block);
    void updateGlobalModulation();
    void syncParametersToState();
    bassx::PlayEngine::Settings makeEngineSettings() const noexcept;
    void allNotesOff (bool allowTailOff);

    bassx::Voice* findVoiceToSteal();
    bassx::Voice* findVoicePlayingNote (int midiNote);

    //==========================================================================
    bassx::SharedWavetableBank wavetableBank;

    bassx::SynthParams params;

    std::array<bassx::Voice, bassx::kMaxPolyVoices> voices;

    // Stack di note premute: il mono legato deve poter tornare alla nota
    // precedente quando quella in cima viene rilasciata, altrimenti trilli e
    // frasi legate veloci lasciano buchi.
    juce::Array<int> heldNotes;

    uint32_t noteOnCounter = 0;
    int monoCurrentNote = -1;
    bool monoNoteIsOn = false;

    float pitchBendSemitones = 0.0f;
    float modWheel = 0.0f;
    float aftertouch = 0.0f;

    //==========================================================================
    bassx::TubeStage   tube;
    bassx::Distortion  distortion;
    bassx::EQ3Band     eq;
    bassx::Chorus      chorus;
    bassx::Phaser      phaser;
    bassx::Delay       delay;
    bassx::Reverb      reverb;
    bassx::Compressor  compressor;
    bassx::Space       space;
    bassx::Limiter     limiter;
    // Due buffer per la wavetable da campione: si costruisce in quello spento e
    // poi si scambia l'indice. shortcut: un secondo caricamento entro un blocco
    // audio dal primo riscriverebbe la tabella in uso; serve un terzo buffer o
    // un ack del thread audio se mai diventa possibile (oggi: decodifica+analisi >> 1 blocco).
    std::array<bassx::Wavetable, 2> userTables;
    std::atomic<int> userTableLive { -1 }, userTableVersion { 0 };
    std::vector<std::vector<float>> userCycles;      // livello 0, per lo stato
    juce::String userTableName;

    std::atomic<double> lastBpm { 120.0 };
    static constexpr int kMaxExportKeys = 8;
    std::array<std::atomic<int>, kMaxExportKeys> exportKeys {};
    std::atomic<int> numExportKeys { 0 };

    bassx::SidechainDucker sidechain;
    bassx::GlitchFx    glitch;
    bassx::BeatRepeat  beatRepeat;
    double fxBpm = 120.0, fxPpq = 0.0;
    bool   fxPlaying = false;

    bassx::SidechainDucker::Curve customCurve;      // protetta da curveLock
    mutable juce::SpinLock curveLock;
    int  lastScShape = -1;
    std::atomic<bool> customCurveDirty { true };
    void updateSidechain (double bpm);

    /** Modulazione degli effetti globali, presa dalla voce suonata più di
        recente (in mono è l'unica). Valori per blocco. */
    bassx::ModMatrix::DestValues globalMod;

    juce::AudioBuffer<float> extBuffer;
    juce::MidiBuffer combinedMidi;
    bool engineWasActive = false;
    int  scopeDecimation = 0;

    bassx::ParamSmoother outputGainSmoother;

    juce::Array<bassx::SeqEvent> sequencerEvents;
    juce::MidiBuffer guiMidiBuffer;
    juce::CriticalSection guiMidiLock;

    double currentSampleRate = 44100.0;
    int    currentBlockSize = 512;
    int    reportedLatency = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassXAudioProcessor)
};
