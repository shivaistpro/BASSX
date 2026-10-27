/*
  ==============================================================================

    BASSX — Parameters.cpp

  ==============================================================================
*/

#include "Parameters.h"
#include "DSP/TubeStage.h"

namespace bassx
{

using namespace juce;

//==============================================================================
namespace
{
    /** Range con skew calcolato per mettere un valore preciso a metà corsa.
        Serve per i controlli dove il punto musicalmente utile non sta al
        centro aritmetico: il cutoff, per esempio, ha il suo range utile fra
        100 Hz e 2 kHz, che su una scala lineare 30-16000 occuperebbe il 12%
        della corsa. */
    NormalisableRange<float> skewedRange (float min, float max, float centre, float step = 0.0f)
    {
        NormalisableRange<float> range (min, max, step);
        range.setSkewForCentre (centre);
        return range;
    }

    /** Range di tempo per inviluppi e delay: skew forte perché i tempi brevi
        sono quelli che vanno regolati con precisione. */
    NormalisableRange<float> timeRange (float minSec, float maxSec, float centreSec)
    {
        return skewedRange (minSec, maxSec, centreSec, 0.0f);
    }

    String secondsToString (float seconds, int)
    {
        if (seconds < 1.0f)
            return String (seconds * 1000.0f, seconds < 0.01f ? 2 : 1) + " ms";

        return String (seconds, 2) + " s";
    }

    String hzToString (float hz, int)
    {
        if (hz >= 1000.0f)
            return String (hz / 1000.0f, 2) + " kHz";

        return String (hz, hz < 100.0f ? 1 : 0) + " Hz";
    }

    String percentToString (float v, int)
    {
        return String (juce::roundToInt (v * 100.0f)) + " %";
    }

    String dbToString (float v, int)
    {
        return String (v, 1) + " dB";
    }

    String centsToString (float v, int)
    {
        return String (v, 1) + " ct";
    }

    //--------------------------------------------------------------------------
    std::unique_ptr<AudioParameterFloat> makeFloat (const String& id, const String& name,
                                                    NormalisableRange<float> range, float def,
                                                    std::function<String (float, int)> toText = {},
                                                    const String& label = {})
    {
        auto attributes = AudioParameterFloatAttributes().withLabel (label);

        if (toText)
            attributes = attributes.withStringFromValueFunction (std::move (toText));

        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, range, def, attributes);
    }

    std::unique_ptr<AudioParameterChoice> makeChoice (const String& id, const String& name,
                                                      const StringArray& choices, int def)
    {
        return std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, choices, def);
    }

    std::unique_ptr<AudioParameterBool> makeBool (const String& id, const String& name, bool def)
    {
        return std::make_unique<AudioParameterBool> (ParameterID { id, 1 }, name, def);
    }

    std::unique_ptr<AudioParameterInt> makeInt (const String& id, const String& name,
                                                int min, int max, int def)
    {
        return std::make_unique<AudioParameterInt> (ParameterID { id, 1 }, name, min, max, def);
    }
}

//==============================================================================
AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    AudioProcessorValueTreeState::ParameterLayout layout;

    // I nomi delle wavetable devono coincidere con l'ordine di costruzione del
    // banco. Sono elencati qui staticamente e non letti dal banco perché il
    // layout dei parametri viene costruito nel costruttore del processore,
    // prima che il banco condiviso possa essere garantito inizializzato.
    const StringArray waveNames
    {
        "Analog Saw", "Analog Square", "Analog Tri", "Acid Saw", "Acid Square",
        "Sine > Saw", "Pulse Morph", "Formant Bass", "Reso Sweep", "FM Bass",
        "Harmonic Stack", "Folded Sine", "Digital", "Growl", "Sub Harmonic", "Metallic",
        "Sample"    // kUserWaveIndex: la wavetable ricavata dal campione trascinato sull'oscillatore
    };

    //==========================================================================
    // Oscillatori
    //==========================================================================
    for (int o = 0; o < kNumOscillators; ++o)
    {
        const String label = "OSC " + String (o + 1) + " ";

        // Osc 1 attivo con la rampa acid: la patch di default deve essere una
        // voce Bass Line utilizzabile senza toccare nulla.
        const bool  defaultOn    = (o == 0);
        const int   defaultWave  = (o == 0) ? 3 : (o == 1 ? 4 : 0);
        const float defaultLevel = (o == 0) ? 1.0f : 0.0f;
        const int   defaultOct   = (o == 2) ? -1 : 0;

        layout.add (makeBool   (ParamID::osc (o, "enable"),  label + "Enable", defaultOn));
        layout.add (makeChoice (ParamID::osc (o, "wave"),    label + "Wave", waveNames, defaultWave));
        layout.add (makeFloat  (ParamID::osc (o, "position"),label + "Position",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
        layout.add (makeFloat  (ParamID::osc (o, "level"),   label + "Level",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), defaultLevel, percentToString));
        layout.add (makeFloat  (ParamID::osc (o, "pan"),     label + "Pan",
                                NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f));
        layout.add (makeInt    (ParamID::osc (o, "octave"),  label + "Octave", -3, 3, defaultOct));
        layout.add (makeInt    (ParamID::osc (o, "semi"),    label + "Semitone", -12, 12, 0));
        layout.add (makeFloat  (ParamID::osc (o, "fine"),    label + "Fine",
                                NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, centsToString));
        layout.add (makeFloat  (ParamID::osc (o, "fold"),    label + "Fold",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
        layout.add (makeInt    (ParamID::osc (o, "uni_voices"), label + "Unison", 1, kMaxUnisonVoices, 1));
        layout.add (makeFloat  (ParamID::osc (o, "uni_detune"), label + "Detune",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.18f, percentToString));
        layout.add (makeFloat  (ParamID::osc (o, "uni_blend"),  label + "Blend",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.5f, percentToString));
        layout.add (makeFloat  (ParamID::osc (o, "uni_spread"), label + "Spread",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.7f, percentToString));
        layout.add (makeFloat  (ParamID::osc (o, "phase_rand"), label + "Phase",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
        layout.add (makeBool   (ParamID::osc (o, "retrig"),     label + "Retrigger", true));
    }

    //==========================================================================
    // Sub e rumore
    //==========================================================================
    layout.add (makeBool   (ParamID::subEnable, "Sub Enable", false));
    layout.add (makeChoice (ParamID::subShape,  "Sub Shape", { "Sine", "Triangle", "Square" }, 0));
    layout.add (makeInt    (ParamID::subOctave, "Sub Octave", -2, 0, -1));
    layout.add (makeFloat  (ParamID::subLevel,  "Sub Level",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));

    layout.add (makeBool   (ParamID::noiseEnable, "Noise Enable", false));
    layout.add (makeChoice (ParamID::noiseColour, "Noise Colour", { "White", "Pink" }, 1));
    layout.add (makeFloat  (ParamID::noiseLevel,  "Noise Level",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));

    //==========================================================================
    // Modulazione fra oscillatori
    //==========================================================================
    layout.add (makeFloat  (ParamID::fmAmount, "FM Amount",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeChoice (ParamID::fmSource, "FM Source", { "Osc 1", "Osc 2", "Osc 3", "Sub" }, 1));
    layout.add (makeChoice (ParamID::fmTarget, "FM Target", { "Osc 1", "Osc 2", "Osc 3" }, 0));
    layout.add (makeFloat  (ParamID::ringAmount, "Ring Mod",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeBool   (ParamID::syncEnable, "Osc 2 Sync", false));

    //==========================================================================
    // Filtri
    //==========================================================================
    const auto filterModels = MultiFilter::getModelNames();
    const StringArray envSources { "Env 1", "Env 2", "Env 3", "Env 4" };

    layout.add (makeChoice (ParamID::f1Model, "F1 Model", filterModels, 0));
    layout.add (makeFloat  (ParamID::f1Cutoff, "F1 Cutoff",
                            skewedRange (20.0f, 18000.0f, 700.0f, 0.01f), 700.0f, hzToString));
    layout.add (makeFloat  (ParamID::f1Reso, "F1 Resonance",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.55f, percentToString));
    layout.add (makeFloat  (ParamID::f1Drive, "F1 Drive",
                            skewedRange (1.0f, 24.0f, 4.0f, 0.01f), 1.8f));
    layout.add (makeFloat  (ParamID::f1EnvAmount, "F1 Env Amount",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.6f, percentToString));
    layout.add (makeChoice (ParamID::f1EnvSource, "F1 Env Source", envSources, 1));
    layout.add (makeFloat  (ParamID::f1KeyTrack, "F1 Key Track",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat  (ParamID::f1BassComp, "F1 Bass Comp",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.3f, percentToString));

    layout.add (makeChoice (ParamID::f2Model, "F2 Model", filterModels, (int) MultiFilter::Model::Bypass));
    layout.add (makeFloat  (ParamID::f2Cutoff, "F2 Cutoff",
                            skewedRange (20.0f, 18000.0f, 2000.0f, 0.01f), 3000.0f, hzToString));
    layout.add (makeFloat  (ParamID::f2Reso, "F2 Resonance",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.2f, percentToString));
    layout.add (makeFloat  (ParamID::f2Drive, "F2 Drive",
                            skewedRange (1.0f, 24.0f, 4.0f, 0.01f), 1.0f));
    layout.add (makeFloat  (ParamID::f2EnvAmount, "F2 Env Amount",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeChoice (ParamID::f2EnvSource, "F2 Env Source", envSources, 2));
    layout.add (makeFloat  (ParamID::f2KeyTrack, "F2 Key Track",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f, percentToString));

    layout.add (makeChoice (ParamID::filterRouting, "Filter Routing",
                            { "Serial 1>2", "Parallel", "Split (F1 low / F2 high)" }, 0));
    layout.add (makeFloat  (ParamID::filterBlend, "Filter Blend",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));

    //==========================================================================
    // Inviluppi
    //==========================================================================
    for (int e = 0; e < kNumEnvelopes; ++e)
    {
        const String label = "ENV " + String (e + 1) + " ";

        // Env 1 = ampiezza: gate quasi istantaneo, sustain pieno, release corta.
        // Env 2 = filtro: decay-only, che è il gesto acid fondamentale.
        const bool isAmp    = (e == 0);
        const bool isFilter = (e == 1);

        layout.add (makeFloat (ParamID::env (e, "delay"), label + "Delay",
                               timeRange (0.0f, 4.0f, 0.2f), 0.0f, secondsToString));
        layout.add (makeFloat (ParamID::env (e, "attack"), label + "Attack",
                               timeRange (0.0002f, 8.0f, 0.05f), isAmp ? 0.002f : 0.003f, secondsToString));
        layout.add (makeFloat (ParamID::env (e, "hold"), label + "Hold",
                               timeRange (0.0f, 4.0f, 0.2f), 0.0f, secondsToString));
        layout.add (makeFloat (ParamID::env (e, "decay"), label + "Decay",
                               timeRange (0.0005f, 16.0f, 0.4f), isFilter ? 0.22f : 0.5f, secondsToString));
        layout.add (makeFloat (ParamID::env (e, "sustain"), label + "Sustain",
                               NormalisableRange<float> (0.0f, 1.0f, 0.0f),
                               isAmp ? 1.0f : (isFilter ? 0.0f : 0.5f), percentToString));
        layout.add (makeFloat (ParamID::env (e, "release"), label + "Release",
                               timeRange (0.0005f, 16.0f, 0.2f), isAmp ? 0.02f : 0.12f, secondsToString));
        layout.add (makeFloat (ParamID::env (e, "curve_a"), label + "Attack Curve",
                               NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f));
        layout.add (makeFloat (ParamID::env (e, "curve_d"), label + "Decay Curve",
                               NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f));
        layout.add (makeFloat (ParamID::env (e, "curve_r"), label + "Release Curve",
                               NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f));
        layout.add (makeFloat (ParamID::env (e, "vel"), label + "Velocity",
                               NormalisableRange<float> (0.0f, 1.0f, 0.0f),
                               isAmp ? 0.35f : 0.0f, percentToString));
    }

    //==========================================================================
    // LFO
    //==========================================================================
    const auto lfoShapes = LFO::getShapeNames();
    const auto lfoModes  = LFO::getTriggerModeNames();
    const auto divisions = getTempoDivisionNames();

    for (int l = 0; l < kNumLFOs; ++l)
    {
        const String label = "LFO " + String (l + 1) + " ";

        layout.add (makeChoice (ParamID::lfo (l, "shape"), label + "Shape", lfoShapes, l == 0 ? 0 : 1));
        layout.add (makeFloat  (ParamID::lfo (l, "rate"), label + "Rate",
                                skewedRange (0.01f, 60.0f, 2.0f, 0.0f), 2.0f, hzToString));
        layout.add (makeBool   (ParamID::lfo (l, "sync"), label + "Sync", false));
        layout.add (makeChoice (ParamID::lfo (l, "division"), label + "Division", divisions, 10));
        layout.add (makeFloat  (ParamID::lfo (l, "phase"), label + "Phase",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
        layout.add (makeChoice (ParamID::lfo (l, "trigger"), label + "Trigger", lfoModes, 0));
        layout.add (makeBool   (ParamID::lfo (l, "unipolar"), label + "Unipolar", false));
        layout.add (makeFloat  (ParamID::lfo (l, "fade"), label + "Fade In",
                                timeRange (0.0f, 8.0f, 0.5f), 0.0f, secondsToString));
        layout.add (makeFloat  (ParamID::lfo (l, "smooth"), label + "Smoothing",
                                skewedRange (0.05f, 200.0f, 5.0f, 0.01f), 1.0f));
        layout.add (makeFloat  (ParamID::lfo (l, "depth"), label + "Depth",
                                NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    }

    //==========================================================================
    // Accento, glide, voce
    //==========================================================================
    layout.add (makeFloat (ParamID::accentAmount, "Accent",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat (ParamID::accentDecay, "Accent Decay",
                           timeRange (0.01f, 2.0f, 0.2f), 0.22f, secondsToString));
    layout.add (makeInt   (ParamID::accentThresh, "Accent Threshold", 1, 127, 100));
    layout.add (makeFloat (ParamID::accentFilter, "Accent > Filter",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat (ParamID::accentVca, "Accent > VCA",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.9f, percentToString));

    layout.add (makeFloat  (ParamID::glideTime, "Glide",
                            timeRange (0.001f, 2.0f, 0.08f), 0.06f, secondsToString));
    layout.add (makeChoice (ParamID::glideMode, "Glide Mode", { "Legato only", "Always", "Off" }, 0));

    layout.add (makeChoice (ParamID::voiceMode, "Voice Mode",
                            { "Mono 303", "Mono Legato", "Poly" }, 0));
    layout.add (makeInt    (ParamID::polyVoices, "Poly Voices", 1, kMaxPolyVoices, 8));
    layout.add (makeFloat  (ParamID::masterTune, "Master Tune",
                            NormalisableRange<float> (-100.0f, 100.0f, 0.1f), 0.0f, centsToString));
    layout.add (makeFloat  (ParamID::pitchBendRange, "Bend Range",
                            NormalisableRange<float> (0.0f, 24.0f, 1.0f), 2.0f));
    layout.add (makeFloat  (ParamID::velocityDepth, "Velocity Depth",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.45f, percentToString));

    //==========================================================================
    // Sequencer
    //==========================================================================
    layout.add (makeBool   (ParamID::seqEnable, "Sequencer", false));
    layout.add (makeInt    (ParamID::seqPattern, "Pattern", 1, kNumSeqPatterns, 1));
    layout.add (makeChoice (ParamID::seqDivision, "Seq Division", divisions, 13));
    layout.add (makeInt    (ParamID::seqLength, "Seq Length", 1, kNumSeqSteps, 16));
    layout.add (makeFloat  (ParamID::seqSwing, "Swing",
                            NormalisableRange<float> (0.0f, 0.75f, 0.0f), 0.0f, percentToString));
    layout.add (makeInt    (ParamID::seqOctave, "Seq Octave", -2, 2, 0));
    layout.add (makeFloat  (ParamID::seqGateLength, "Gate Length",   // 303 misurato: livello pieno ~60% del passo
                            NormalisableRange<float> (0.05f, 1.0f, 0.0f), 0.6f, percentToString));
    layout.add (makeChoice (ParamID::seqDirection, "Seq Direction",
                            { "Forward", "Backward", "Ping-Pong", "Random" }, 0));
    layout.add (makeBool   (ParamID::seqKeyFollow, "Seq Key Follow", true));

    //==========================================================================
    // Arpeggiatore
    //==========================================================================
    layout.add (makeBool   (ParamID::arpEnable, "Arpeggiator", false));
    layout.add (makeChoice (ParamID::arpMode, "Arp Mode",
                            { "Up", "Down", "Up/Down", "Down/Up", "As Played", "Random", "Chord" }, 0));
    layout.add (makeInt    (ParamID::arpOctaves, "Arp Octaves", 1, 4, 1));
    layout.add (makeChoice (ParamID::arpDivision, "Arp Division", divisions, 13));
    layout.add (makeFloat  (ParamID::arpGate, "Arp Gate",
                            NormalisableRange<float> (0.05f, 1.0f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat  (ParamID::arpSwing, "Arp Swing",
                            NormalisableRange<float> (0.0f, 0.75f, 0.0f), 0.0f, percentToString));
    layout.add (makeBool   (ParamID::arpLatch, "Arp Latch", false));
    layout.add (makeChoice (ParamID::arpAccent, "Arp Accent",
                            { "Off", "Every 2", "Every 3", "Every 4", "Every 5", "Every 6", "Every 7", "Every 8" }, 0));
    layout.add (makeBool   (ParamID::arpSlide, "Arp Slide", false));
    layout.add (makeChoice (ParamID::playCombine, "Seq+Arp Mode",
                            { "Layer", "Seq Rhythm > Arp Notes", "Seq Transposes Arp" }, 0));

    //==========================================================================
    // Fish: controlli stile Devil Fish e stadio neurale
    //==========================================================================
    layout.add (makeBool  (ParamID::dfExtMode, "Ext Mode", false));
    layout.add (makeFloat (ParamID::dfExtGain, "Ext Gain",
                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dbToString));
    layout.add (makeFloat (ParamID::dfEnvAttack, "Env Attack",
                           timeRange (0.0003f, 0.03f, 0.003f), 0.0003f, secondsToString));
    layout.add (makeFloat (ParamID::dfSweepSpeed, "Sweep Speed",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat (ParamID::dfVibSpeed, "Vib Speed",
                           skewedRange (0.1f, 12.0f, 4.0f, 0.01f), 5.0f,
                           [] (float v, int) { return String (v, 2) + " Hz"; }));
    layout.add (makeFloat (ParamID::dfVibDepth, "Vib Depth",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f,
                           [] (float v, int) { return String (roundToInt (v * 100.0f)) + " ct"; }));
    layout.add (makeFloat (ParamID::dfFilterFm, "Filter FM",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat (ParamID::dfMuffler, "Muffler",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat (ParamID::dfNeuralDrive, "Neural Drive",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));

    //==========================================================================
    // Effetti
    //==========================================================================
    layout.add (makeBool   (ParamID::distOn, "Distortion On", false));
    layout.add (makeChoice (ParamID::distAlgo, "Dist Type", Distortion::getAlgorithmNames(), 1));
    layout.add (makeFloat  (ParamID::distDrive, "Dist Drive",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.3f, percentToString));
    layout.add (makeFloat  (ParamID::distTone, "Dist Tone",
                            skewedRange (200.0f, 20000.0f, 3000.0f, 1.0f), 12000.0f, hzToString));
    layout.add (makeFloat  (ParamID::distMix, "Dist Mix",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat  (ParamID::distBits, "Dist Bits",
                            NormalisableRange<float> (1.0f, 16.0f, 0.1f), 12.0f));
    layout.add (makeFloat  (ParamID::distDownsample, "Dist Downsample",
                            NormalisableRange<float> (1.0f, 64.0f, 1.0f), 1.0f));

    // Stadio valvolare (motore ETA-1)
    layout.add (makeBool   (ParamID::tubeOn, "Tube On", false));
    layout.add (makeChoice (ParamID::tubeModel, "Tube Model", TubeStage::getModelNames(), 6));
    layout.add (makeFloat  (ParamID::tubeDrive, "Tube Drive",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat  (ParamID::tubeBias, "Tube Bias",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat  (ParamID::tubeSag, "Tube Sag",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.25f, percentToString));
    layout.add (makeFloat  (ParamID::tubeWarmth, "Tube Warmth",
                            NormalisableRange<float> (-6.0f, 6.0f, 0.01f), 0.0f, dbToString));
    layout.add (makeFloat  (ParamID::tubePresence, "Tube Presence",
                            NormalisableRange<float> (-6.0f, 6.0f, 0.01f), 0.0f, dbToString));
    layout.add (makeFloat  (ParamID::tubeTone, "Tube Tone",
                            skewedRange (1000.0f, 20000.0f, 6000.0f, 1.0f), 12000.0f, hzToString));
    layout.add (makeFloat  (ParamID::tubeCharacter, "Tube Character",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.35f, percentToString));
    layout.add (makeFloat  (ParamID::tubeAge, "Tube Age",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat  (ParamID::tubeMix, "Tube Mix",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat  (ParamID::tubeOutput, "Tube Output",
                            NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, dbToString));

    layout.add (makeFloat (ParamID::eqLowFreq, "EQ Low Freq",
                           skewedRange (20.0f, 1000.0f, 120.0f, 1.0f), 100.0f, hzToString));
    layout.add (makeFloat (ParamID::eqLowGain, "EQ Low Gain",
                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dbToString));
    layout.add (makeFloat (ParamID::eqMidFreq, "EQ Mid Freq",
                           skewedRange (40.0f, 12000.0f, 800.0f, 1.0f), 800.0f, hzToString));
    layout.add (makeFloat (ParamID::eqMidGain, "EQ Mid Gain",
                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dbToString));
    layout.add (makeFloat (ParamID::eqMidQ, "EQ Mid Q",
                           skewedRange (0.2f, 12.0f, 1.0f, 0.01f), 1.0f));
    layout.add (makeFloat (ParamID::eqHighFreq, "EQ High Freq",
                           skewedRange (1000.0f, 20000.0f, 6000.0f, 1.0f), 6000.0f, hzToString));
    layout.add (makeFloat (ParamID::eqHighGain, "EQ High Gain",
                           NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dbToString));

    layout.add (makeBool  (ParamID::chorusOn, "Chorus On", false));
    layout.add (makeFloat (ParamID::chorusRate, "Chorus Rate",
                           skewedRange (0.01f, 10.0f, 0.6f, 0.0f), 0.6f, hzToString));
    layout.add (makeFloat (ParamID::chorusDepth, "Chorus Depth",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat (ParamID::chorusMix, "Chorus Mix",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.35f, percentToString));
    layout.add (makeFloat (ParamID::chorusSpread, "Chorus Spread",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.7f, percentToString));
    layout.add (makeInt   (ParamID::chorusVoices, "Chorus Voices", 1, 3, 2));

    layout.add (makeBool  (ParamID::phaserOn, "Phaser On", false));
    layout.add (makeFloat (ParamID::phaserRate, "Phaser Rate",
                           skewedRange (0.01f, 10.0f, 0.35f, 0.0f), 0.35f, hzToString));
    layout.add (makeFloat (ParamID::phaserDepth, "Phaser Depth",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.7f, percentToString));
    layout.add (makeFloat (ParamID::phaserFreq, "Phaser Centre",
                           skewedRange (60.0f, 8000.0f, 600.0f, 1.0f), 600.0f, hzToString));
    layout.add (makeFloat (ParamID::phaserFeedback, "Phaser Feedback",
                           NormalisableRange<float> (-0.95f, 0.95f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat (ParamID::phaserMix, "Phaser Mix",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.4f, percentToString));
    layout.add (makeInt   (ParamID::phaserStages, "Phaser Stages", 2, 12, 6));

    layout.add (makeBool   (ParamID::delayOn, "Delay On", false));
    layout.add (makeBool   (ParamID::delaySync, "Delay Sync", true));
    layout.add (makeFloat  (ParamID::delayTimeL, "Delay Time L",
                            timeRange (0.001f, 3.0f, 0.25f), 0.25f, secondsToString));
    layout.add (makeFloat  (ParamID::delayTimeR, "Delay Time R",
                            timeRange (0.001f, 3.0f, 0.25f), 0.375f, secondsToString));
    layout.add (makeChoice (ParamID::delayDivL, "Delay Div L", divisions, 10));
    layout.add (makeChoice (ParamID::delayDivR, "Delay Div R", divisions, 9));
    layout.add (makeFloat  (ParamID::delayFeedback, "Delay Feedback",
                            NormalisableRange<float> (0.0f, 1.05f, 0.0f), 0.35f, percentToString));
    layout.add (makeFloat  (ParamID::delayMix, "Delay Mix",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeBool   (ParamID::delayPingPong, "Ping Pong", false));
    layout.add (makeFloat  (ParamID::delayLowpass, "Delay LPF",
                            skewedRange (200.0f, 20000.0f, 4000.0f, 1.0f), 6000.0f, hzToString));
    layout.add (makeFloat  (ParamID::delayHighpass, "Delay HPF",
                            skewedRange (20.0f, 2000.0f, 200.0f, 1.0f), 220.0f, hzToString));

    layout.add (makeBool  (ParamID::reverbOn, "Reverb On", false));
    layout.add (makeFloat (ParamID::reverbSize, "Reverb Size",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.5f, percentToString));
    layout.add (makeFloat (ParamID::reverbDecay, "Reverb Decay",
                           skewedRange (0.1f, 20.0f, 2.0f, 0.01f), 2.0f, secondsToString));
    layout.add (makeFloat (ParamID::reverbDamping, "Reverb Damping",
                           skewedRange (500.0f, 20000.0f, 5000.0f, 1.0f), 6000.0f, hzToString));
    layout.add (makeFloat (ParamID::reverbLowCut, "Reverb Low Cut",
                           skewedRange (20.0f, 1000.0f, 180.0f, 1.0f), 180.0f, hzToString));
    layout.add (makeFloat (ParamID::reverbPreDelay, "Reverb Pre-Delay",
                           timeRange (0.0f, 0.24f, 0.03f), 0.02f, secondsToString));
    layout.add (makeFloat (ParamID::reverbWidth, "Reverb Width",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat (ParamID::reverbMix, "Reverb Mix",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat (ParamID::reverbMod, "Reverb Mod",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.4f, percentToString));

    layout.add (makeBool  (ParamID::compOn, "Comp On", false));
    layout.add (makeFloat (ParamID::compThreshold, "Comp Threshold",
                           NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -12.0f, dbToString));
    layout.add (makeFloat (ParamID::compRatio, "Comp Ratio",
                           skewedRange (1.0f, 60.0f, 4.0f, 0.1f), 4.0f));
    layout.add (makeFloat (ParamID::compAttack, "Comp Attack",
                           skewedRange (0.05f, 200.0f, 10.0f, 0.01f), 8.0f));
    layout.add (makeFloat (ParamID::compRelease, "Comp Release",
                           skewedRange (5.0f, 2000.0f, 150.0f, 0.1f), 120.0f));
    layout.add (makeFloat (ParamID::compKnee, "Comp Knee",
                           NormalisableRange<float> (0.0f, 24.0f, 0.1f), 6.0f, dbToString));
    layout.add (makeFloat (ParamID::compMakeup, "Comp Makeup",
                           NormalisableRange<float> (-12.0f, 36.0f, 0.1f), 0.0f, dbToString));
    layout.add (makeFloat (ParamID::compMix, "Comp Mix",
                           NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeBool  (ParamID::compRms, "Comp RMS", false));

    //==========================================================================
    // Spazio / panorama
    //==========================================================================
    layout.add (makeBool   (ParamID::spaceOn, "Space On", true));
    layout.add (makeFloat  (ParamID::spacePan, "Space Pan",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f));
    layout.add (makeFloat  (ParamID::spaceWidth, "Space Width",
                            NormalisableRange<float> (0.0f, 2.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat  (ParamID::spaceMonoFreq, "Bass Mono",
                            skewedRange (20.0f, 400.0f, 120.0f, 1.0f), 20.0f,
                            [] (float v, int) { return v <= 20.5f ? String ("Off") : String (roundToInt (v)) + " Hz"; }));
    layout.add (makeFloat  (ParamID::spaceHaas, "Haas",
                            NormalisableRange<float> (0.0f, 0.03f, 0.0f), 0.0f, secondsToString));
    layout.add (makeFloat  (ParamID::spaceAutopan, "Autopan",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.0f, percentToString));
    layout.add (makeChoice (ParamID::spaceAutopanDiv, "Autopan Rate", divisions, 7));
    layout.add (makeFloat  (ParamID::spaceRotate, "Rotation",
                            NormalisableRange<float> (-1.0f, 1.0f, 0.0f), 0.0f, percentToString));

    //==========================================================================
    // Routing della catena effetti
    //==========================================================================
    for (int slot = 0; slot < kNumFxSlots; ++slot)
        layout.add (makeChoice (ParamID::fxSlot (slot), "FX Slot " + String (slot + 1),
                                getFxTypeNames(), slot));

    layout.add (makeBool  (ParamID::limiterOn, "Limiter", true));
    layout.add (makeFloat (ParamID::limiterCeiling, "Ceiling",
                           NormalisableRange<float> (-24.0f, 0.0f, 0.1f), -0.3f, dbToString));

    //==========================================================================
    // Glitch e Beat Repeat (slot 9 e 10 della catena)
    //==========================================================================
    layout.add (makeBool   (ParamID::glitchOn, "Glitch On", false));
    layout.add (makeChoice (ParamID::glitchRate, "Glitch Rate", divisions, 13));
    layout.add (makeFloat  (ParamID::glitchAmount, "Glitch Amount",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.4f, percentToString));
    layout.add (makeChoice (ParamID::glitchMode, "Glitch Mode", GlitchFx::getModeNames(), 0));
    layout.add (makeInt    (ParamID::glitchSeed, "Glitch Seed", 0, 64, 0));
    layout.add (makeFloat  (ParamID::glitchMix, "Glitch Mix",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeBool   (ParamID::brepOn, "Repeat On", false));
    layout.add (makeChoice (ParamID::brepInterval, "Repeat Interval", divisions, 3));
    layout.add (makeChoice (ParamID::brepGrid, "Repeat Grid", divisions, 13));
    layout.add (makeFloat  (ParamID::brepChance, "Repeat Chance",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeChoice (ParamID::brepGate, "Repeat Gate", divisions, 7));
    layout.add (makeFloat  (ParamID::brepDecay, "Repeat Decay",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 0.15f, percentToString));
    layout.add (makeFloat  (ParamID::brepMix, "Repeat Mix",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));

    //==========================================================================
    // Sidechain: ducking a curva (impostazione di sessione: i preset non lo toccano)
    //==========================================================================
    layout.add (makeBool   (ParamID::scOn, "Sidechain On", false));
    layout.add (makeChoice (ParamID::scTrigger, "SC Trigger", { "Sync", "MIDI ch16", "Audio (Ext In)" }, 0));
    layout.add (makeChoice (ParamID::scRate, "SC Rate", divisions, 7));
    layout.add (makeChoice (ParamID::scShape, "SC Shape", SidechainDucker::getShapeNames(), 0));
    layout.add (makeFloat  (ParamID::scDepth, "SC Depth",
                            NormalisableRange<float> (0.0f, 1.0f, 0.0f), 1.0f, percentToString));
    layout.add (makeFloat  (ParamID::scPhase, "SC Phase",
                            NormalisableRange<float> (-0.5f, 0.5f, 0.0f), 0.0f, percentToString));
    layout.add (makeFloat  (ParamID::scSmooth, "SC Smooth",
                            skewedRange (0.1f, 20.0f, 2.0f), 1.0f,
                            [] (float v, int) { return String (v, 1) + " ms"; }));
    layout.add (makeFloat  (ParamID::scBandLow, "SC Band Low",
                            skewedRange (20.0f, 18000.0f, 400.0f), 20.0f, hzToString));
    layout.add (makeFloat  (ParamID::scBandHigh, "SC Band High",
                            skewedRange (25.0f, 20000.0f, 1000.0f), 20000.0f, hzToString));
    layout.add (makeFloat  (ParamID::scThreshold, "SC Threshold",
                            NormalisableRange<float> (-60.0f, 0.0f, 0.0f), -24.0f, dbToString));

    layout.add (makeFloat  (ParamID::outputGain, "Output",
                            NormalisableRange<float> (-48.0f, 12.0f, 0.1f), -3.0f, dbToString));
    layout.add (makeChoice (ParamID::oversampling, "Oversampling",
                            { "Off", "2x", "4x", "8x" }, 1));

    return layout;
}

//==============================================================================
void SynthParams::updateFrom (AudioProcessorValueTreeState& apvts) noexcept
{
    // getRawParameterValue fa una ricerca in tabella hash, e soprattutto ogni
    // ID passato come const char* o costruito con ParamID::osc() diventa una
    // juce::String, cioè un'allocazione sul thread audio. Alla prima chiamata
    // (costruttore del processore, thread messaggi) si memorizzano i puntatori
    // nell'ordine di lettura; dalle successive si leggono per indice. Le macro
    // servono a non valutare affatto l'espressione dell'ID quando la cache è
    // pronta: con una funzione l'argomento verrebbe costruito comunque.
    //
    // Vincolo: la sequenza di letture qui sotto deve essere identica a ogni
    // chiamata — niente letture condizionali.
    const bool useCache = ! paramCache.empty();
    size_t cursor = 0;

    auto lookup = [this, &apvts, &cursor] (const juce::String& id) -> float
    {
        static std::atomic<float> missing { 0.0f };
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);   // ID inesistente: errore di programmazione
        paramCache.push_back (p != nullptr ? p : &missing);
        ++cursor;
        return paramCache.back()->load (std::memory_order_relaxed);
    };

   #define BX_F(id) (useCache ? paramCache[cursor++]->load (std::memory_order_relaxed) : lookup (id))
   #define BX_B(id) (BX_F (id) > 0.5f)
   #define BX_I(id) ((int) std::lround (BX_F (id)))

    //--------------------------------------------------------------------------
    for (int o = 0; o < kNumOscillators; ++o)
    {
        auto& p = osc[(size_t) o];
        p.enabled      = BX_B (ParamID::osc (o, "enable"));
        p.waveIndex    = BX_I (ParamID::osc (o, "wave"));
        p.position     = BX_F (ParamID::osc (o, "position"));
        p.level        = BX_F (ParamID::osc (o, "level"));
        p.pan          = BX_F (ParamID::osc (o, "pan"));
        p.octave       = BX_I (ParamID::osc (o, "octave"));
        p.semitone     = BX_I (ParamID::osc (o, "semi"));
        p.fineCents    = BX_F (ParamID::osc (o, "fine"));
        p.fold         = BX_F (ParamID::osc (o, "fold"));
        p.unisonVoices = BX_I (ParamID::osc (o, "uni_voices"));
        p.unisonDetune = BX_F (ParamID::osc (o, "uni_detune"));
        p.unisonBlend  = BX_F (ParamID::osc (o, "uni_blend"));
        p.unisonSpread = BX_F (ParamID::osc (o, "uni_spread"));
        p.phaseRandom  = BX_F (ParamID::osc (o, "phase_rand"));
        p.retrigger    = BX_B (ParamID::osc (o, "retrig"));
    }

    for (int e = 0; e < kNumEnvelopes; ++e)
    {
        auto& p = env[(size_t) e];
        p.delay          = BX_F (ParamID::env (e, "delay"));
        p.attack         = BX_F (ParamID::env (e, "attack"));
        p.hold           = BX_F (ParamID::env (e, "hold"));
        p.decay          = BX_F (ParamID::env (e, "decay"));
        p.sustain        = BX_F (ParamID::env (e, "sustain"));
        p.release        = BX_F (ParamID::env (e, "release"));
        p.curveA         = BX_F (ParamID::env (e, "curve_a"));
        p.curveD         = BX_F (ParamID::env (e, "curve_d"));
        p.curveR         = BX_F (ParamID::env (e, "curve_r"));
        p.velocityAmount = BX_F (ParamID::env (e, "vel"));
    }

    for (int l = 0; l < kNumLFOs; ++l)
    {
        auto& p = lfo[(size_t) l];
        p.shape    = BX_I (ParamID::lfo (l, "shape"));
        p.rateHz   = BX_F (ParamID::lfo (l, "rate"));
        p.sync     = BX_B (ParamID::lfo (l, "sync"));
        p.division = BX_I (ParamID::lfo (l, "division"));
        p.phase    = BX_F (ParamID::lfo (l, "phase"));
        p.trigger  = BX_I (ParamID::lfo (l, "trigger"));
        p.unipolar = BX_B (ParamID::lfo (l, "unipolar"));
        p.fadeIn   = BX_F (ParamID::lfo (l, "fade"));
        p.smoothMs = BX_F (ParamID::lfo (l, "smooth"));
        p.depth    = BX_F (ParamID::lfo (l, "depth"));
    }

    subEnabled = BX_B (ParamID::subEnable);
    subShape   = BX_I (ParamID::subShape);
    subOctave  = BX_I (ParamID::subOctave);
    subLevel   = BX_F (ParamID::subLevel);

    noiseEnabled = BX_B (ParamID::noiseEnable);
    noiseColour  = BX_I (ParamID::noiseColour);
    noiseLevel   = BX_F (ParamID::noiseLevel);

    fmAmount    = BX_F (ParamID::fmAmount);
    fmSource    = BX_I (ParamID::fmSource);
    fmTarget    = BX_I (ParamID::fmTarget);
    ringAmount  = BX_F (ParamID::ringAmount);
    syncEnabled = BX_B (ParamID::syncEnable);

    f1Model     = BX_I (ParamID::f1Model);
    f1Cutoff    = BX_F (ParamID::f1Cutoff);
    f1Reso      = BX_F (ParamID::f1Reso);
    f1Drive     = BX_F (ParamID::f1Drive);
    f1EnvAmount = BX_F (ParamID::f1EnvAmount);
    f1EnvSource = BX_I (ParamID::f1EnvSource);
    f1KeyTrack  = BX_F (ParamID::f1KeyTrack);
    f1BassComp  = BX_F (ParamID::f1BassComp);

    f2Model     = BX_I (ParamID::f2Model);
    f2Cutoff    = BX_F (ParamID::f2Cutoff);
    f2Reso      = BX_F (ParamID::f2Reso);
    f2Drive     = BX_F (ParamID::f2Drive);
    f2EnvAmount = BX_F (ParamID::f2EnvAmount);
    f2EnvSource = BX_I (ParamID::f2EnvSource);
    f2KeyTrack  = BX_F (ParamID::f2KeyTrack);

    filterRouting = BX_I (ParamID::filterRouting);
    filterBlend   = BX_F (ParamID::filterBlend);

    accentAmount    = BX_F (ParamID::accentAmount);
    accentDecay     = BX_F (ParamID::accentDecay);
    accentThreshold = BX_I (ParamID::accentThresh);
    accentFilter    = BX_F (ParamID::accentFilter);
    accentVca       = BX_F (ParamID::accentVca);

    glideTime = BX_F (ParamID::glideTime);
    glideMode = BX_I (ParamID::glideMode);

    voiceMode     = BX_I (ParamID::voiceMode);
    polyVoices    = BX_I (ParamID::polyVoices);
    masterTune    = BX_F (ParamID::masterTune);
    bendRange     = BX_F (ParamID::pitchBendRange);
    velocityDepth = BX_F (ParamID::velocityDepth);

    seqEnabled    = BX_B (ParamID::seqEnable);
    seqPattern    = BX_I (ParamID::seqPattern) - 1;
    seqDivision   = BX_I (ParamID::seqDivision);
    seqLength     = BX_I (ParamID::seqLength);
    seqSwing      = BX_F (ParamID::seqSwing);
    seqOctave     = BX_I (ParamID::seqOctave);
    seqGateLength = BX_F (ParamID::seqGateLength);

    distOn         = BX_B (ParamID::distOn);
    distAlgo       = BX_I (ParamID::distAlgo);
    distDrive      = BX_F (ParamID::distDrive);
    distTone       = BX_F (ParamID::distTone);
    distMix        = BX_F (ParamID::distMix);
    distBits       = BX_F (ParamID::distBits);
    distDownsample = BX_F (ParamID::distDownsample);

    tubeOn        = BX_B (ParamID::tubeOn);
    tubeModel     = BX_I (ParamID::tubeModel);
    tubeDrive     = BX_F (ParamID::tubeDrive);
    tubeBias      = BX_F (ParamID::tubeBias);
    tubeSag       = BX_F (ParamID::tubeSag);
    tubeWarmth    = BX_F (ParamID::tubeWarmth);
    tubePresence  = BX_F (ParamID::tubePresence);
    tubeTone      = BX_F (ParamID::tubeTone);
    tubeCharacter = BX_F (ParamID::tubeCharacter);
    tubeAge       = BX_F (ParamID::tubeAge);
    tubeMix       = BX_F (ParamID::tubeMix);
    tubeOutput    = BX_F (ParamID::tubeOutput);

    eqLowFreq  = BX_F (ParamID::eqLowFreq);
    eqLowGain  = BX_F (ParamID::eqLowGain);
    eqMidFreq  = BX_F (ParamID::eqMidFreq);
    eqMidGain  = BX_F (ParamID::eqMidGain);
    eqMidQ     = BX_F (ParamID::eqMidQ);
    eqHighFreq = BX_F (ParamID::eqHighFreq);
    eqHighGain = BX_F (ParamID::eqHighGain);

    chorusOn     = BX_B (ParamID::chorusOn);
    chorusRate   = BX_F (ParamID::chorusRate);
    chorusDepth  = BX_F (ParamID::chorusDepth);
    chorusMix    = BX_F (ParamID::chorusMix);
    chorusSpread = BX_F (ParamID::chorusSpread);
    chorusVoices = BX_I (ParamID::chorusVoices);

    phaserOn       = BX_B (ParamID::phaserOn);
    phaserRate     = BX_F (ParamID::phaserRate);
    phaserDepth    = BX_F (ParamID::phaserDepth);
    phaserFreq     = BX_F (ParamID::phaserFreq);
    phaserFeedback = BX_F (ParamID::phaserFeedback);
    phaserMix      = BX_F (ParamID::phaserMix);
    phaserStages   = BX_I (ParamID::phaserStages);

    delayOn       = BX_B (ParamID::delayOn);
    delaySync     = BX_B (ParamID::delaySync);
    delayTimeL    = BX_F (ParamID::delayTimeL);
    delayTimeR    = BX_F (ParamID::delayTimeR);
    delayDivL     = BX_I (ParamID::delayDivL);
    delayDivR     = BX_I (ParamID::delayDivR);
    delayFeedback = BX_F (ParamID::delayFeedback);
    delayMix      = BX_F (ParamID::delayMix);
    delayPingPong = BX_B (ParamID::delayPingPong);
    delayLowpass  = BX_F (ParamID::delayLowpass);
    delayHighpass = BX_F (ParamID::delayHighpass);

    reverbOn       = BX_B (ParamID::reverbOn);
    reverbSize     = BX_F (ParamID::reverbSize);
    reverbDecay    = BX_F (ParamID::reverbDecay);
    reverbDamping  = BX_F (ParamID::reverbDamping);
    reverbLowCut   = BX_F (ParamID::reverbLowCut);
    reverbPreDelay = BX_F (ParamID::reverbPreDelay);
    reverbWidth    = BX_F (ParamID::reverbWidth);
    reverbMix      = BX_F (ParamID::reverbMix);
    reverbMod      = BX_F (ParamID::reverbMod);

    compOn        = BX_B (ParamID::compOn);
    compThreshold = BX_F (ParamID::compThreshold);
    compRatio     = BX_F (ParamID::compRatio);
    compAttack    = BX_F (ParamID::compAttack);
    compRelease   = BX_F (ParamID::compRelease);
    compKnee      = BX_F (ParamID::compKnee);
    compMakeup    = BX_F (ParamID::compMakeup);
    compMix       = BX_F (ParamID::compMix);
    compRms       = BX_B (ParamID::compRms);

    limiterOn      = BX_B (ParamID::limiterOn);
    limiterCeiling = BX_F (ParamID::limiterCeiling);

    outputGain   = BX_F (ParamID::outputGain);
    oversampling = BX_I (ParamID::oversampling);

    // --- v1.1 --------------------------------------------------------------------
    seqDirection = BX_I (ParamID::seqDirection);
    seqKeyFollow = BX_B (ParamID::seqKeyFollow);

    arpEnabled  = BX_B (ParamID::arpEnable);
    arpMode     = BX_I (ParamID::arpMode);
    arpOctaves  = BX_I (ParamID::arpOctaves);
    arpDivision = BX_I (ParamID::arpDivision);
    arpGate     = BX_F (ParamID::arpGate);
    arpSwing    = BX_F (ParamID::arpSwing);
    arpLatch    = BX_B (ParamID::arpLatch);
    arpAccent   = BX_I (ParamID::arpAccent);
    arpSlide    = BX_B (ParamID::arpSlide);
    playCombine = BX_I (ParamID::playCombine);

    dfExtMode     = BX_B (ParamID::dfExtMode);
    dfExtGain     = BX_F (ParamID::dfExtGain);
    dfEnvAttack   = BX_F (ParamID::dfEnvAttack);
    dfSweepSpeed  = BX_F (ParamID::dfSweepSpeed);
    dfVibSpeed    = BX_F (ParamID::dfVibSpeed);
    dfVibDepth    = BX_F (ParamID::dfVibDepth);
    dfFilterFm    = BX_F (ParamID::dfFilterFm);
    dfMuffler     = BX_F (ParamID::dfMuffler);
    dfNeuralDrive = BX_F (ParamID::dfNeuralDrive);

    spaceOn         = BX_B (ParamID::spaceOn);
    spacePan        = BX_F (ParamID::spacePan);
    spaceWidth      = BX_F (ParamID::spaceWidth);
    spaceMonoFreq   = BX_F (ParamID::spaceMonoFreq);
    spaceHaas       = BX_F (ParamID::spaceHaas);
    spaceAutopan    = BX_F (ParamID::spaceAutopan);
    spaceAutopanDiv = BX_I (ParamID::spaceAutopanDiv);
    spaceRotate     = BX_F (ParamID::spaceRotate);

    for (int slot = 0; slot < kNumFxSlots; ++slot)
        fxSlots[(size_t) slot] = BX_I (ParamID::fxSlot (slot));

    glitchOn     = BX_B (ParamID::glitchOn);
    glitchRate   = BX_I (ParamID::glitchRate);
    glitchAmount = BX_F (ParamID::glitchAmount);
    glitchMode   = BX_I (ParamID::glitchMode);
    glitchSeed   = BX_I (ParamID::glitchSeed);
    glitchMix    = BX_F (ParamID::glitchMix);
    brepOn       = BX_B (ParamID::brepOn);
    brepInterval = BX_I (ParamID::brepInterval);
    brepGrid     = BX_I (ParamID::brepGrid);
    brepChance   = BX_F (ParamID::brepChance);
    brepGate     = BX_I (ParamID::brepGate);
    brepDecay    = BX_F (ParamID::brepDecay);
    brepMix      = BX_F (ParamID::brepMix);

    scOn        = BX_B (ParamID::scOn);
    scTrigger   = BX_I (ParamID::scTrigger);
    scRate      = BX_I (ParamID::scRate);
    scShape     = BX_I (ParamID::scShape);
    scDepth     = BX_F (ParamID::scDepth);
    scPhase     = BX_F (ParamID::scPhase);
    scSmooth    = BX_F (ParamID::scSmooth);
    scBandLow   = BX_F (ParamID::scBandLow);
    scBandHigh  = BX_F (ParamID::scBandHigh);
    scThreshold = BX_F (ParamID::scThreshold);

   #undef BX_I
   #undef BX_B
   #undef BX_F
}

} // namespace bassx
