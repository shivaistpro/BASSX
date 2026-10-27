/*
  ==============================================================================

    BASSX — Parameters.h

    Identificatori, layout APVTS e struttura POD di lavoro.

    Perché una struttura POD invece di leggere l'APVTS dal percorso audio:
      getRawParameterValue restituisce un std::atomic<float>*. Leggerlo per
      campione significa una load atomica per parametro per campione: con ~200
      parametri e 8 voci diventa il costo dominante del plugin, e per giunta
      impedisce al compilatore di tenere i valori in registro. Leggiamo tutto
      una volta per blocco in una struct contigua di float semplici; da lì le
      voci lavorano su valori che stanno in cache e che il compilatore può
      vettorizzare.

    La conseguenza è che l'automazione ha risoluzione di un blocco. Per i
    parametri dove questo si sentirebbe (cutoff, livelli, volume) c'è uno
    smoother dedicato dentro la voce, quindi il risultato udibile è continuo.

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/Common.h"
#include "DSP/Filters.h"
#include "DSP/Effects.h"
#include "DSP/LFO.h"
#include "DSP/Oscillator.h"
#include "DSP/Sidechain.h"
#include "DSP/Glitch.h"

namespace bassx
{

//==============================================================================
inline constexpr int kNumOscillators = 3;
inline constexpr int kNumEnvelopes   = 4;
inline constexpr int kNumLFOs        = 4;
inline constexpr int kNumSeqSteps    = 16;
inline constexpr int kNumSeqPatterns = 8;
inline constexpr int kMaxPolyVoices  = 16;
inline constexpr int kNumFxSlots     = 10;

/** Effetti assegnabili agli slot della catena, nell'ordine di default. */
// nuovi effetti sempre in coda: gli indici salvati nei preset restano validi
enum class FxType { Distortion = 0, EQ, Chorus, Phaser, Delay, Reverb, Compressor, Space, Glitch, BeatRepeat, NumTypes };

inline juce::StringArray getFxTypeNames()
{
    return { "Distortion", "EQ", "Chorus", "Phaser", "Delay", "Reverb", "Compressor", "Space", "Glitch", "Beat Repeat" };
}

//==============================================================================
namespace ParamID
{
    // --- Oscillatori (prefisso osc1_ / osc2_ / osc3_) ------------------------
    inline juce::String osc (int index, const char* suffix)
    {
        return "osc" + juce::String (index + 1) + "_" + suffix;
    }

    inline juce::String env (int index, const char* suffix)
    {
        return "env" + juce::String (index + 1) + "_" + suffix;
    }

    inline juce::String lfo (int index, const char* suffix)
    {
        return "lfo" + juce::String (index + 1) + "_" + suffix;
    }

    // --- Globali --------------------------------------------------------------
    inline constexpr const char* subEnable      = "sub_enable";
    inline constexpr const char* subShape       = "sub_shape";
    inline constexpr const char* subOctave      = "sub_octave";
    inline constexpr const char* subLevel       = "sub_level";

    inline constexpr const char* noiseEnable    = "noise_enable";
    inline constexpr const char* noiseColour    = "noise_colour";
    inline constexpr const char* noiseLevel     = "noise_level";

    inline constexpr const char* fmAmount       = "fm_amount";
    inline constexpr const char* fmSource       = "fm_source";
    inline constexpr const char* fmTarget       = "fm_target";
    inline constexpr const char* ringAmount     = "ring_amount";
    inline constexpr const char* syncEnable     = "sync_enable";

    inline constexpr const char* f1Model        = "f1_model";
    inline constexpr const char* f1Cutoff       = "f1_cutoff";
    inline constexpr const char* f1Reso         = "f1_reso";
    inline constexpr const char* f1Drive        = "f1_drive";
    inline constexpr const char* f1EnvAmount    = "f1_env_amount";
    inline constexpr const char* f1EnvSource    = "f1_env_source";
    inline constexpr const char* f1KeyTrack     = "f1_keytrack";
    inline constexpr const char* f1BassComp     = "f1_bass_comp";

    inline constexpr const char* f2Model        = "f2_model";
    inline constexpr const char* f2Cutoff       = "f2_cutoff";
    inline constexpr const char* f2Reso         = "f2_reso";
    inline constexpr const char* f2Drive        = "f2_drive";
    inline constexpr const char* f2EnvAmount    = "f2_env_amount";
    inline constexpr const char* f2EnvSource    = "f2_env_source";
    inline constexpr const char* f2KeyTrack     = "f2_keytrack";

    inline constexpr const char* filterRouting  = "filter_routing";
    inline constexpr const char* filterBlend    = "filter_blend";

    inline constexpr const char* accentAmount   = "accent_amount";
    inline constexpr const char* accentDecay    = "accent_decay";
    inline constexpr const char* accentThresh   = "accent_threshold";
    inline constexpr const char* accentFilter   = "accent_filter";
    inline constexpr const char* accentVca      = "accent_vca";

    inline constexpr const char* glideTime      = "glide_time";
    inline constexpr const char* glideMode      = "glide_mode";

    inline constexpr const char* voiceMode      = "voice_mode";
    inline constexpr const char* polyVoices     = "poly_voices";
    inline constexpr const char* masterTune     = "master_tune";
    inline constexpr const char* pitchBendRange = "bend_range";
    inline constexpr const char* velocityDepth  = "velocity_depth";

    inline constexpr const char* seqEnable      = "seq_enable";
    inline constexpr const char* seqPattern     = "seq_pattern";
    inline constexpr const char* seqDivision    = "seq_division";
    inline constexpr const char* seqLength      = "seq_length";
    inline constexpr const char* seqSwing       = "seq_swing";
    inline constexpr const char* seqOctave      = "seq_octave";
    inline constexpr const char* seqGateLength  = "seq_gate_length";
    inline constexpr const char* seqDirection   = "seq_direction";
    inline constexpr const char* seqKeyFollow   = "seq_key_follow";

    // --- Arpeggiatore e combinazione con il sequencer -------------------------
    inline constexpr const char* arpEnable      = "arp_enable";
    inline constexpr const char* arpMode        = "arp_mode";
    inline constexpr const char* arpOctaves     = "arp_octaves";
    inline constexpr const char* arpDivision    = "arp_division";
    inline constexpr const char* arpGate        = "arp_gate";
    inline constexpr const char* arpSwing       = "arp_swing";
    inline constexpr const char* arpLatch       = "arp_latch";
    inline constexpr const char* arpAccent      = "arp_accent";
    inline constexpr const char* arpSlide       = "arp_slide";
    inline constexpr const char* playCombine    = "play_combine";

    // --- Sezione "Fish": controlli stile Devil Fish e stadio neurale ----------
    inline constexpr const char* dfExtMode      = "df_ext_mode";
    inline constexpr const char* dfExtGain      = "df_ext_gain";
    inline constexpr const char* dfEnvAttack    = "df_env_attack";
    inline constexpr const char* dfSweepSpeed   = "df_sweep_speed";
    inline constexpr const char* dfVibSpeed     = "df_vib_speed";
    inline constexpr const char* dfVibDepth     = "df_vib_depth";
    inline constexpr const char* dfFilterFm     = "df_filter_fm";
    inline constexpr const char* dfMuffler      = "df_muffler";
    inline constexpr const char* dfNeuralDrive  = "df_neural_drive";

    // --- Effetti ---------------------------------------------------------------
    inline constexpr const char* distOn         = "dist_on";
    inline constexpr const char* distAlgo       = "dist_algo";
    inline constexpr const char* distDrive      = "dist_drive";
    inline constexpr const char* distTone       = "dist_tone";
    inline constexpr const char* distMix        = "dist_mix";
    inline constexpr const char* distBits       = "dist_bits";
    inline constexpr const char* distDownsample = "dist_downsample";

    inline constexpr const char* tubeOn        = "tube_on";
    inline constexpr const char* tubeModel     = "tube_model";
    inline constexpr const char* tubeDrive     = "tube_drive";
    inline constexpr const char* tubeBias      = "tube_bias";
    inline constexpr const char* tubeSag       = "tube_sag";
    inline constexpr const char* tubeWarmth    = "tube_warmth";
    inline constexpr const char* tubePresence  = "tube_presence";
    inline constexpr const char* tubeTone      = "tube_tone";
    inline constexpr const char* tubeCharacter = "tube_character";
    inline constexpr const char* tubeAge       = "tube_age";
    inline constexpr const char* tubeMix       = "tube_mix";
    inline constexpr const char* tubeOutput    = "tube_output";

    inline constexpr const char* eqLowFreq      = "eq_low_freq";
    inline constexpr const char* eqLowGain      = "eq_low_gain";
    inline constexpr const char* eqMidFreq      = "eq_mid_freq";
    inline constexpr const char* eqMidGain      = "eq_mid_gain";
    inline constexpr const char* eqMidQ         = "eq_mid_q";
    inline constexpr const char* eqHighFreq     = "eq_high_freq";
    inline constexpr const char* eqHighGain     = "eq_high_gain";

    inline constexpr const char* chorusOn       = "chorus_on";
    inline constexpr const char* chorusRate     = "chorus_rate";
    inline constexpr const char* chorusDepth    = "chorus_depth";
    inline constexpr const char* chorusMix      = "chorus_mix";
    inline constexpr const char* chorusSpread   = "chorus_spread";
    inline constexpr const char* chorusVoices   = "chorus_voices";

    inline constexpr const char* phaserOn       = "phaser_on";
    inline constexpr const char* phaserRate     = "phaser_rate";
    inline constexpr const char* phaserDepth    = "phaser_depth";
    inline constexpr const char* phaserFreq     = "phaser_freq";
    inline constexpr const char* phaserFeedback = "phaser_feedback";
    inline constexpr const char* phaserMix      = "phaser_mix";
    inline constexpr const char* phaserStages   = "phaser_stages";

    inline constexpr const char* delayOn        = "delay_on";
    inline constexpr const char* delaySync      = "delay_sync";
    inline constexpr const char* delayTimeL     = "delay_time_l";
    inline constexpr const char* delayTimeR     = "delay_time_r";
    inline constexpr const char* delayDivL      = "delay_div_l";
    inline constexpr const char* delayDivR      = "delay_div_r";
    inline constexpr const char* delayFeedback  = "delay_feedback";
    inline constexpr const char* delayMix       = "delay_mix";
    inline constexpr const char* delayPingPong  = "delay_pingpong";
    inline constexpr const char* delayLowpass   = "delay_lowpass";
    inline constexpr const char* delayHighpass  = "delay_highpass";

    inline constexpr const char* reverbOn       = "reverb_on";
    inline constexpr const char* reverbSize     = "reverb_size";
    inline constexpr const char* reverbDecay    = "reverb_decay";
    inline constexpr const char* reverbDamping  = "reverb_damping";
    inline constexpr const char* reverbLowCut   = "reverb_lowcut";
    inline constexpr const char* reverbPreDelay = "reverb_predelay";
    inline constexpr const char* reverbWidth    = "reverb_width";
    inline constexpr const char* reverbMix      = "reverb_mix";
    inline constexpr const char* reverbMod      = "reverb_mod";

    inline constexpr const char* compOn         = "comp_on";
    inline constexpr const char* compThreshold  = "comp_threshold";
    inline constexpr const char* compRatio      = "comp_ratio";
    inline constexpr const char* compAttack     = "comp_attack";
    inline constexpr const char* compRelease    = "comp_release";
    inline constexpr const char* compKnee       = "comp_knee";
    inline constexpr const char* compMakeup     = "comp_makeup";
    inline constexpr const char* compMix        = "comp_mix";
    inline constexpr const char* compRms        = "comp_rms";

    inline constexpr const char* limiterOn      = "limiter_on";
    inline constexpr const char* limiterCeiling = "limiter_ceiling";

    // --- Spazio / panorama -------------------------------------------------------
    inline constexpr const char* spaceOn        = "space_on";
    inline constexpr const char* spacePan       = "space_pan";
    inline constexpr const char* spaceWidth     = "space_width";
    inline constexpr const char* spaceMonoFreq  = "space_mono_freq";
    inline constexpr const char* spaceHaas      = "space_haas";
    inline constexpr const char* spaceAutopan   = "space_autopan";
    inline constexpr const char* spaceAutopanDiv= "space_autopan_div";
    inline constexpr const char* spaceRotate    = "space_rotate";

    // --- Routing della catena effetti: effetto assegnato a ogni slot -----------
    inline juce::String fxSlot (int index)
    {
        return "fx_slot" + juce::String (index + 1);
    }

    // --- Glitch / Beat Repeat ---------------------------------------------------
    inline constexpr const char* glitchOn       = "glitch_on";
    inline constexpr const char* glitchRate     = "glitch_rate";
    inline constexpr const char* glitchAmount   = "glitch_amount";
    inline constexpr const char* glitchMode     = "glitch_mode";
    inline constexpr const char* glitchSeed     = "glitch_seed";
    inline constexpr const char* glitchMix      = "glitch_mix";
    inline constexpr const char* brepOn         = "brep_on";
    inline constexpr const char* brepInterval   = "brep_interval";
    inline constexpr const char* brepGrid       = "brep_grid";
    inline constexpr const char* brepChance     = "brep_chance";
    inline constexpr const char* brepGate       = "brep_gate";
    inline constexpr const char* brepDecay      = "brep_decay";
    inline constexpr const char* brepMix        = "brep_mix";

    // --- Sidechain (ducking a curva, stile Kickstart) ---------------------------
    inline constexpr const char* scOn           = "sc_on";
    inline constexpr const char* scTrigger      = "sc_trigger";
    inline constexpr const char* scRate         = "sc_rate";
    inline constexpr const char* scShape        = "sc_shape";
    inline constexpr const char* scDepth        = "sc_depth";
    inline constexpr const char* scPhase        = "sc_phase";
    inline constexpr const char* scSmooth       = "sc_smooth";
    inline constexpr const char* scBandLow      = "sc_band_low";
    inline constexpr const char* scBandHigh     = "sc_band_high";
    inline constexpr const char* scThreshold    = "sc_threshold";

    inline constexpr const char* outputGain     = "output_gain";
    inline constexpr const char* oversampling   = "oversampling";
}

//==============================================================================
/** Divisioni ritmiche condivise da sequencer e delay. */
inline juce::StringArray getTempoDivisionNames()
{
    return LFO::getSyncDivisionNames();
}

//==============================================================================
/**
    Copia di lavoro dei parametri, aggiornata una volta per blocco.

    Tutti i campi sono float/int/bool semplici: la struttura è banalmente
    copiabile e sta in poche linee di cache.
*/
struct SynthParams
{
    struct OscParams
    {
        bool  enabled = false;
        int   waveIndex = 0;
        float position = 0.0f;
        float level = 0.0f;
        float pan = 0.0f;
        int   octave = 0;
        int   semitone = 0;
        float fineCents = 0.0f;
        float fold = 0.0f;
        int   unisonVoices = 1;
        float unisonDetune = 0.2f;
        float unisonBlend = 0.5f;
        float unisonSpread = 0.6f;
        float phaseRandom = 0.0f;
        bool  retrigger = true;
    };

    struct EnvParams
    {
        float delay = 0.0f, attack = 0.003f, hold = 0.0f;
        float decay = 0.25f, sustain = 0.0f, release = 0.08f;
        float curveA = 0.0f, curveD = 0.0f, curveR = 0.0f;
        float velocityAmount = 0.0f;
    };

    struct LfoParams
    {
        int   shape = 0;
        float rateHz = 2.0f;
        bool  sync = false;
        int   division = 10;
        float phase = 0.0f;
        int   trigger = 0;
        bool  unipolar = false;
        float fadeIn = 0.0f;
        float smoothMs = 1.0f;
        float depth = 1.0f;
    };

    std::array<OscParams, kNumOscillators> osc;
    std::array<EnvParams, kNumEnvelopes>   env;
    std::array<LfoParams, kNumLFOs>        lfo;

    bool  subEnabled = false;
    int   subShape = 0;
    int   subOctave = -1;
    float subLevel = 0.0f;

    bool  noiseEnabled = false;
    int   noiseColour = 0;
    float noiseLevel = 0.0f;

    float fmAmount = 0.0f;
    int   fmSource = 1;
    int   fmTarget = 0;
    float ringAmount = 0.0f;
    bool  syncEnabled = false;

    int   f1Model = 0;
    float f1Cutoff = 700.0f, f1Reso = 0.5f, f1Drive = 1.5f;
    float f1EnvAmount = 0.6f; int f1EnvSource = 1;
    float f1KeyTrack = 0.0f, f1BassComp = 0.3f;

    int   f2Model = (int) MultiFilter::Model::Bypass;
    float f2Cutoff = 3000.0f, f2Reso = 0.2f, f2Drive = 1.0f;
    float f2EnvAmount = 0.0f; int f2EnvSource = 2;
    float f2KeyTrack = 0.0f;

    int   filterRouting = 0;
    float filterBlend = 0.0f;

    float accentAmount = 0.5f, accentDecay = 0.22f;
    int   accentThreshold = 100;
    float accentFilter = 1.0f, accentVca = 0.9f;

    float glideTime = 0.06f;
    int   glideMode = 0;

    int   voiceMode = 0;
    int   polyVoices = 8;
    float masterTune = 0.0f;
    float bendRange = 2.0f;
    float velocityDepth = 0.45f;

    bool  seqEnabled = false;
    int   seqPattern = 0;
    int   seqDivision = 13;      // 1/16
    int   seqLength = 16;
    float seqSwing = 0.0f;
    int   seqOctave = 0;
    float seqGateLength = 0.6f;
    int   seqDirection = 0;
    bool  seqKeyFollow = true;

    bool  arpEnabled = false;
    int   arpMode = 0;
    int   arpOctaves = 1;
    int   arpDivision = 13;      // 1/16
    float arpGate = 0.5f;
    float arpSwing = 0.0f;
    bool  arpLatch = false;
    int   arpAccent = 0;
    bool  arpSlide = false;
    int   playCombine = 0;

    // --- Fish ---------------------------------------------------------------------
    bool  dfExtMode = false;
    float dfExtGain = 0.0f;
    float dfEnvAttack = 0.0003f;
    float dfSweepSpeed = 0.0f;
    float dfVibSpeed = 5.0f, dfVibDepth = 0.0f;
    float dfFilterFm = 0.0f, dfMuffler = 0.0f, dfNeuralDrive = 0.0f;

    // --- Effetti ---------------------------------------------------------------
    bool  distOn = false;
    int   distAlgo = 1;
    float distDrive = 0.3f, distTone = 12000.0f, distMix = 1.0f;
    float distBits = 12.0f, distDownsample = 1.0f;

    bool  tubeOn = false;
    int   tubeModel = 6;
    float tubeDrive = 0.5f, tubeBias = 0.0f, tubeSag = 0.25f;
    float tubeWarmth = 0.0f, tubePresence = 0.0f, tubeTone = 12000.0f;
    float tubeCharacter = 0.35f, tubeAge = 0.0f, tubeMix = 1.0f, tubeOutput = 0.0f;

    float eqLowFreq = 100.0f, eqLowGain = 0.0f;
    float eqMidFreq = 800.0f, eqMidGain = 0.0f, eqMidQ = 1.0f;
    float eqHighFreq = 6000.0f, eqHighGain = 0.0f;

    bool  chorusOn = false;
    float chorusRate = 0.6f, chorusDepth = 0.5f, chorusMix = 0.35f, chorusSpread = 0.7f;
    int   chorusVoices = 2;

    bool  phaserOn = false;
    float phaserRate = 0.35f, phaserDepth = 0.7f, phaserFreq = 600.0f;
    float phaserFeedback = 0.5f, phaserMix = 0.4f;
    int   phaserStages = 6;

    bool  delayOn = false, delaySync = true, delayPingPong = false;
    float delayTimeL = 0.25f, delayTimeR = 0.375f;
    int   delayDivL = 10, delayDivR = 9;
    float delayFeedback = 0.35f, delayMix = 0.0f;
    float delayLowpass = 6000.0f, delayHighpass = 220.0f;

    bool  reverbOn = false;
    float reverbSize = 0.5f, reverbDecay = 2.0f, reverbDamping = 6000.0f;
    float reverbLowCut = 180.0f, reverbPreDelay = 0.02f;
    float reverbWidth = 1.0f, reverbMix = 0.0f, reverbMod = 0.4f;

    bool  compOn = false, compRms = false;
    float compThreshold = -12.0f, compRatio = 4.0f;
    float compAttack = 8.0f, compRelease = 120.0f, compKnee = 6.0f;
    float compMakeup = 0.0f, compMix = 1.0f;

    bool  spaceOn = true;
    float spacePan = 0.0f, spaceWidth = 1.0f, spaceMonoFreq = 20.0f, spaceHaas = 0.0f;
    float spaceAutopan = 0.0f, spaceRotate = 0.0f;
    int   spaceAutopanDiv = 7;   // 1/4

    std::array<int, kNumFxSlots> fxSlots { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

    bool  glitchOn = false;
    int   glitchRate = 13, glitchMode = 0, glitchSeed = 0;
    float glitchAmount = 0.4f, glitchMix = 1.0f;

    bool  brepOn = false;
    int   brepInterval = 3, brepGrid = 13, brepGate = 7;
    float brepChance = 1.0f, brepDecay = 0.15f, brepMix = 1.0f;

    bool  limiterOn = true;
    float limiterCeiling = -0.3f;

    bool  scOn = false;
    int   scTrigger = 0, scRate = 7, scShape = 0;       // 1/4, Classic
    float scDepth = 1.0f, scPhase = 0.0f, scSmooth = 1.0f;
    float scBandLow = 20.0f, scBandHigh = 20000.0f, scThreshold = -24.0f;

    float outputGain = -3.0f;
    int   oversampling = 1;

    //==========================================================================
    /** Legge tutti i valori dall'APVTS. Chiamato una volta per blocco dal
        thread audio; nessuna allocazione. */
    void updateFrom (juce::AudioProcessorValueTreeState& apvts) noexcept;

private:
    /** Puntatori ai valori APVTS, nell'ordine esatto in cui updateFrom() li
        legge. Riempito alla prima chiamata (costruttore del processore, thread
        messaggi); dopo, updateFrom() non costruisce più alcuna juce::String,
        cioè non alloca sul thread audio. */
    std::vector<std::atomic<float>*> paramCache;
};

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

} // namespace bassx
