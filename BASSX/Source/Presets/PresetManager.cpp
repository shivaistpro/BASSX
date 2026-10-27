/*
  ==============================================================================

    BASSX — PresetManager.cpp

  ==============================================================================
*/

#include "PresetManager.h"

namespace bassx
{

using namespace juce;

//==============================================================================
namespace
{
    constexpr const char* kUserPresetExtension = ".bassx";
    constexpr const char* kPresetFolderName    = "BASSX/Presets";

    /** Scorciatoie per costruire gli scostamenti dei preset di fabbrica. */
    PresetValue pv (const String& id, float v) { return { id, v }; }

    PresetModLink ml (int slot, ModSource s, ModDest d, float depth, ModSource via = ModSource::None)
    {
        return { slot, s, d, depth, via };
    }

    /** L'indice di una scelta va passato come valore normalizzato all'APVTS
        quando si usa setValueNotifyingHost, ma qui usiamo direttamente
        setValue sul parametro tramite il suo range, quindi passiamo l'indice
        intero come float. La conversione avviene in applyFactoryPreset. */
    float choice (int index) { return (float) index; }
    float onOff (bool b) { return b ? 1.0f : 0.0f; }
}

//==============================================================================
PresetManager::PresetManager (AudioProcessorValueTreeState& state, ModMatrix& matrix, Sequencer& seq)
    : apvts (state), modMatrix (matrix), sequencer (seq)
{
    buildFactoryPresets();
    rescanUserPresets();
}

//==============================================================================
StringArray PresetManager::getCategories() const
{
    StringArray categories;

    for (const auto& p : factoryPresets)
        if (! categories.contains (p.category))
            categories.add (p.category);

    if (! userPresets.isEmpty())
        categories.add ("User");

    return categories;
}

StringArray PresetManager::getPresetNames (const String& category) const
{
    StringArray names;

    for (const auto& p : factoryPresets)
        if (category.isEmpty() || p.category == category)
            names.add (p.name);

    if (category.isEmpty() || category == "User")
        for (const auto& f : userPresets)
            names.add (f.getFileNameWithoutExtension());

    return names;
}

String PresetManager::getPresetName (int index) const
{
    const auto names = getPresetNames();
    return isPositiveAndBelow (index, names.size()) ? names[index] : String();
}

Array<int> PresetManager::getPresetIndices (const String& category) const
{
    Array<int> indices;
    const int numFactory = (int) factoryPresets.size();

    for (int i = 0; i < numFactory; ++i)
        if (category.isEmpty() || factoryPresets[(size_t) i].category == category)
            indices.add (i);

    if (category.isEmpty() || category == "User")
        for (int j = 0; j < userPresets.size(); ++j)
            indices.add (numFactory + j);

    return indices;
}

void PresetManager::restoreSessionPreset (int index, const String& name)
{
    if (name.isNotEmpty() && getPresetName (index) == name)
        currentIndex = index;
    else if (name.isNotEmpty() && getPresetNames().contains (name))
        currentIndex = getPresetNames().indexOf (name);
    else
        currentIndex = -1;

    if (onPresetChanged)
        onPresetChanged();
}

bool PresetManager::isSessionSetting (const String& paramID)
{
    // il sidechain e' del mix, non del suono: cambiare preset non lo spegne
    return paramID.startsWith ("sc_")
        || paramID == ParamID::oversampling
        || paramID == ParamID::masterTune
        || paramID == ParamID::pitchBendRange
        || paramID == ParamID::limiterCeiling;
}

String PresetManager::getCurrentPresetName() const
{
    if (currentIndex < 0)
        return "Modified";

    if (currentIndex < (int) factoryPresets.size())
        return factoryPresets[(size_t) currentIndex].name;

    const int userIndex = currentIndex - (int) factoryPresets.size();

    if (userIndex < userPresets.size())
        return userPresets[userIndex].getFileNameWithoutExtension();

    return "Init";
}

//==============================================================================
File PresetManager::getUserPresetDirectory() const
{
    return File::getSpecialLocation (File::userApplicationDataDirectory)
             .getChildFile (kPresetFolderName);
}

void PresetManager::rescanUserPresets()
{
    userPresets.clear();

    const auto dir = getUserPresetDirectory();

    if (! dir.isDirectory())
    {
        dir.createDirectory();
        return;
    }

    for (const auto& entry : RangedDirectoryIterator (dir, false, String ("*") + kUserPresetExtension))
        userPresets.add (entry.getFile());

    // Ordine alfabetico: l'ordine di enumerazione del filesystem non è
    // garantito e cambierebbe fra un avvio e l'altro, spostando gli indici dei
    // preset sotto i piedi dell'utente.
    userPresets.sort();
}

//==============================================================================
void PresetManager::resetToDefaults()
{
    for (auto* param : apvts.processor.getParameters())
        if (auto* p = dynamic_cast<RangedAudioParameter*> (param))
            if (! isSessionSetting (p->getParameterID()))
                p->setValueNotifyingHost (p->getDefaultValue());

    modMatrix.clear();
}

void PresetManager::applyParameterTree (const ValueTree& paramState)
{
    // Prima: apvts.replaceState (preset). Sostituiva l'INTERO albero di stato,
    // comprese le proprieta' che non sono parametri (tema e colore della GUI)
    // e le impostazioni di sessione: cambiando preset cambiava anche il tema,
    // l'oversampling, l'accordatura. Qui si scrivono solo i parametri di suono.
    for (int i = 0; i < paramState.getNumChildren(); ++i)
    {
        const auto node = paramState.getChild (i);
        const auto id = node.getProperty ("id").toString();

        if (id.isEmpty() || isSessionSetting (id) || ! node.hasProperty ("value"))
            continue;

        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 ((float) node.getProperty ("value")));
    }
}

void PresetManager::applyFactoryPreset (const FactoryPreset& preset)
{
    resetToDefaults();

    for (const auto& v : preset.values)
    {
        if (isSessionSetting (v.paramID))
            continue;

        if (auto* param = apvts.getParameter (v.paramID))
        {
            // convertTo0to1 rispetta lo skew del range, quindi il valore
            // scritto nel preset è quello reale (Hz, secondi, dB) e non una
            // posizione normalizzata che cambierebbe significato se un giorno
            // il range venisse ritoccato.
            param->setValueNotifyingHost (param->convertTo0to1 (v.value));
        }
        else
        {
            // Un ID inesistente in un preset di fabbrica è un errore di
            // battitura nel codice, non una condizione di runtime.
            jassertfalse;
        }
    }

    modMatrix.clear();

    for (const auto& link : preset.modLinks)
        modMatrix.setSlot (link.slot, link.source, link.dest, link.depth, link.via);
}

//==============================================================================
void PresetManager::loadPreset (int index)
{
    const int numFactory = (int) factoryPresets.size();
    const int total = numFactory + userPresets.size();

    if (total == 0)
        return;

    index = jlimit (0, total - 1, index);

    if (index < numFactory)
    {
        applyFactoryPreset (factoryPresets[(size_t) index]);
    }
    else
    {
        const auto file = userPresets[index - numFactory];

        if (auto xml = parseXML (file))
        {
            const auto tree = ValueTree::fromXml (*xml);

            if (tree.isValid())
            {
                resetToDefaults();

                const auto paramState = tree.getChildWithName (apvts.state.getType());

                if (paramState.isValid())
                    applyParameterTree (paramState);

                modMatrix.fromValueTree (tree.getChildWithName ("MODMATRIX"));
                sequencer.fromValueTree (tree.getChildWithName ("SEQUENCER"));
            }
        }
    }

    currentIndex = index;

    if (onPresetChanged)
        onPresetChanged();
}

void PresetManager::loadPresetByName (const String& name)
{
    const auto all = getPresetNames();
    const int index = all.indexOf (name);

    if (index >= 0)
        loadPreset (index);
}

void PresetManager::loadNext()
{
    const int total = getNumPresets();

    if (total > 0)
        loadPreset ((jmax (0, currentIndex) + 1) % total);
}

void PresetManager::loadPrevious()
{
    const int total = getNumPresets();

    if (total > 0)
        loadPreset ((jmax (0, currentIndex) + total - 1) % total);
}

//==============================================================================
bool PresetManager::saveUserPreset (const String& name)
{
    if (name.trim().isEmpty())
        return false;

    const auto dir = getUserPresetDirectory();

    if (! dir.isDirectory() && ! dir.createDirectory())
        return false;

    const auto file = dir.getChildFile (File::createLegalFileName (name) + kUserPresetExtension);

    ValueTree root ("BASSXPRESET");
    root.setProperty ("name", name, nullptr);
    root.setProperty ("version", 1, nullptr);

    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (modMatrix.toValueTree(), nullptr);
    root.appendChild (sequencer.toValueTree(), nullptr);

    if (auto xml = root.createXml())
    {
        if (! xml->writeTo (file))
            return false;

        rescanUserPresets();

        // Indice per FILE, non per nome: "Acid Bass" utente e "Acid Bass" di
        // fabbrica sono due preset diversi.
        const int userIndex = userPresets.indexOf (file);
        currentIndex = userIndex >= 0 ? (int) factoryPresets.size() + userIndex : -1;

        if (onPresetChanged)
            onPresetChanged();

        return true;
    }

    return false;
}

bool PresetManager::deleteUserPreset (const String& name)
{
    const auto file = getUserPresetDirectory()
                        .getChildFile (File::createLegalFileName (name) + kUserPresetExtension);

    if (! file.existsAsFile())
        return false;

    const bool ok = file.deleteFile();

    if (ok)
    {
        rescanUserPresets();
        currentIndex = 0;

        if (onPresetChanged)
            onPresetChanged();
    }

    return ok;
}

//==============================================================================
void PresetManager::buildFactoryPresets()
{
    factoryPresets.clear();

    auto add = [this] (const String& name, const String& category,
                       std::vector<PresetValue> values,
                       std::vector<PresetModLink> links = {})
    {
        factoryPresets.push_back ({ name, category, std::move (values), std::move (links) });
    };

    // Nomi dei parametri usati spesso, per accorciare le liste.
    const String o1w = ParamID::osc (0, "wave"),   o1l = ParamID::osc (0, "level");
    const String o1p = ParamID::osc (0, "position"), o1o = ParamID::osc (0, "octave");
    const String o1u = ParamID::osc (0, "uni_voices"), o1d = ParamID::osc (0, "uni_detune");
    const String o1f = ParamID::osc (0, "fold");

    const String o2e = ParamID::osc (1, "enable"), o2w = ParamID::osc (1, "wave");
    const String o2l = ParamID::osc (1, "level"),  o2o = ParamID::osc (1, "octave");
    const String o2s = ParamID::osc (1, "semi"),   o2fi = ParamID::osc (1, "fine");
    const String o2p = ParamID::osc (1, "position"), o2u = ParamID::osc (1, "uni_voices");
    const String o2d = ParamID::osc (1, "uni_detune");

    const String o3e = ParamID::osc (2, "enable"), o3w = ParamID::osc (2, "wave");
    const String o3l = ParamID::osc (2, "level"),  o3o = ParamID::osc (2, "octave");

    const String e1a = ParamID::env (0, "attack"), e1d = ParamID::env (0, "decay");
    const String e1s = ParamID::env (0, "sustain"), e1r = ParamID::env (0, "release");
    const String e2a = ParamID::env (1, "attack"), e2d = ParamID::env (1, "decay");
    const String e2s = ParamID::env (1, "sustain"), e2r = ParamID::env (1, "release");
    const String e2cd = ParamID::env (1, "curve_d");
    const String e3a = ParamID::env (2, "attack"), e3d = ParamID::env (2, "decay");
    const String e3s = ParamID::env (2, "sustain"), e3r = ParamID::env (2, "release");

    const String l1s = ParamID::lfo (0, "shape"), l1r = ParamID::lfo (0, "rate");
    const String l1sy = ParamID::lfo (0, "sync"), l1dv = ParamID::lfo (0, "division");
    const String l2s = ParamID::lfo (1, "shape"), l2r = ParamID::lfo (1, "rate");

    //==========================================================================
    // ACID — il territorio del 303
    //==========================================================================

    add ("Init", "Basics",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 700.0f), pv (ParamID::f1Reso, 0.5f),
        pv (ParamID::f1EnvAmount, 0.55f), pv (ParamID::f1Drive, 1.6f)
    });

    add ("Acid Classic", "Acid",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)),
        pv (ParamID::f1Cutoff, 340.0f), pv (ParamID::f1Reso, 0.78f),
        pv (ParamID::f1EnvAmount, 0.72f), pv (ParamID::f1Drive, 3.4f),
        pv (ParamID::f1BassComp, 0.25f),
        pv (e2d, 0.19f), pv (e2s, 0.0f), pv (e2cd, 0.35f),
        pv (e1a, 0.0015f), pv (e1s, 1.0f), pv (e1r, 0.018f),
        pv (ParamID::accentAmount, 0.65f), pv (ParamID::accentDecay, 0.20f),
        pv (ParamID::glideTime, 0.03f), pv (ParamID::dfEnvAttack, 0.004f),   // slide e soft attack misurati su 303
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (1)),
        pv (ParamID::distDrive, 0.28f), pv (ParamID::distMix, 0.55f),
        pv (ParamID::outputGain, -4.0f)
    });

    add ("Acid Screamer", "Acid",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 260.0f), pv (ParamID::f1Reso, 0.92f),
        pv (ParamID::f1EnvAmount, 0.88f), pv (ParamID::f1Drive, 7.5f),
        pv (ParamID::f1BassComp, 0.2f),
        pv (e2d, 0.14f), pv (e2s, 0.0f), pv (e2cd, 0.6f),
        pv (e1r, 0.012f),
        pv (ParamID::accentAmount, 0.85f), pv (ParamID::accentDecay, 0.16f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (6)),
        pv (ParamID::distDrive, 0.48f), pv (ParamID::distTone, 5200.0f),
        pv (ParamID::eqLowFreq, 90.0f), pv (ParamID::eqLowGain, 3.5f),
        pv (ParamID::outputGain, -6.0f)
    });

    add ("Acid Deep", "Acid",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f), pv (o1o, -1.0f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.5f),
        pv (ParamID::f1Cutoff, 180.0f), pv (ParamID::f1Reso, 0.68f),
        pv (ParamID::f1EnvAmount, 0.55f), pv (ParamID::f1Drive, 2.2f),
        pv (ParamID::f1BassComp, 0.7f),
        pv (e2d, 0.42f), pv (e2s, 0.0f),
        pv (ParamID::glideTime, 0.28f),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.14f),
        pv (ParamID::reverbSize, 0.62f), pv (ParamID::reverbLowCut, 340.0f),
        pv (ParamID::outputGain, -4.5f)
    });

    add ("Acid Squelch Mod", "Acid",
    {
        pv (o1w, choice (8)), pv (o1l, 1.0f), pv (o1p, 0.25f),
        pv (ParamID::f1Cutoff, 300.0f), pv (ParamID::f1Reso, 0.85f),
        pv (ParamID::f1EnvAmount, 0.75f), pv (ParamID::f1Drive, 4.0f),
        pv (e2d, 0.17f), pv (e2s, 0.0f),
        pv (ParamID::accentAmount, 0.7f)
    },
    {
        // La position della wavetable segue l'inviluppo di filtro: il timbro si
        // apre insieme al filtro, raddoppiando l'effetto dello sweep senza
        // aumentare la risonanza.
        ml (0, ModSource::Env2, ModDest::Osc1Position, 0.55f),
        ml (1, ModSource::Accent, ModDest::Osc1Position, 0.3f)
    });

    //==========================================================================
    // REESE / NEURO — territorio Massive
    //==========================================================================

    add ("Reese Wide", "Reese",
    {
        pv (o1w, choice (0)), pv (o1l, 0.85f), pv (o1u, 7.0f), pv (o1d, 0.12f),
        pv (ParamID::osc (0, "uni_spread"), 0.9f),
        pv (o2e, onOff (true)), pv (o2w, choice (0)), pv (o2l, 0.85f),
        pv (o2fi, 9.0f), pv (o2u, 7.0f), pv (o2d, 0.14f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.62f),
        pv (ParamID::f1Model, choice (2)),
        pv (ParamID::f1Cutoff, 1400.0f), pv (ParamID::f1Reso, 0.25f),
        pv (ParamID::f1EnvAmount, 0.18f),
        pv (e1a, 0.006f), pv (e1s, 1.0f), pv (e1r, 0.09f),
        pv (e2d, 0.6f), pv (e2s, 0.35f),
        pv (ParamID::eqLowFreq, 80.0f), pv (ParamID::eqLowGain, 2.0f),
        pv (ParamID::compOn, onOff (true)), pv (ParamID::compThreshold, -16.0f),
        pv (ParamID::compRatio, 3.5f),
        pv (ParamID::outputGain, -6.0f)
    });

    add ("Neuro Growl", "Reese",
    {
        pv (o1w, choice (13)), pv (o1l, 0.9f), pv (o1u, 4.0f), pv (o1d, 0.1f),
        pv (o2e, onOff (true)), pv (o2w, choice (11)), pv (o2l, 0.6f), pv (o2fi, -7.0f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.55f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 620.0f),
        pv (ParamID::f1Reso, 0.6f), pv (ParamID::f1Drive, 3.0f),
        pv (ParamID::f2Model, choice (14)),      // formant
        pv (ParamID::f2Cutoff, 900.0f), pv (ParamID::f2Reso, 0.55f),
        pv (ParamID::filterRouting, choice (0)),
        pv (l1s, choice (7)), pv (l1r, 3.2f),
        pv (l2s, choice (0)), pv (l2r, 0.8f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (4)),
        pv (ParamID::distDrive, 0.35f), pv (ParamID::distMix, 0.7f),
        pv (ParamID::outputGain, -7.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Osc1Position, 0.75f),
        ml (1, ModSource::LFO2, ModDest::Filter2Cutoff, 0.35f),
        ml (2, ModSource::LFO2, ModDest::Osc2Position, 0.5f),
        ml (3, ModSource::Env2, ModDest::FxDriveAmount, 0.4f)
    });

    add ("Talking Bass", "Reese",
    {
        pv (o1w, choice (7)), pv (o1l, 0.95f), pv (o1u, 3.0f), pv (o1d, 0.08f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.5f),
        pv (ParamID::f1Model, choice (14)), pv (ParamID::f1Cutoff, 700.0f),
        pv (ParamID::f1Reso, 0.6f),
        pv (ParamID::f2Model, choice (7)), pv (ParamID::f2Cutoff, 4500.0f),
        pv (l1s, choice (1)), pv (l1r, 1.6f), pv (l1sy, onOff (true)), pv (l1dv, choice (10)),
        pv (ParamID::outputGain, -6.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Osc1Position, 0.9f),
        ml (1, ModSource::LFO1, ModDest::Filter1Cutoff, 0.25f)
    });

    //==========================================================================
    // SUB / CLEAN
    //==========================================================================

    add ("Pure Sub", "Sub",
    {
        pv (o1w, choice (14)), pv (o1l, 0.7f), pv (o1o, -1.0f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.85f),
        pv (ParamID::subShape, choice (0)), pv (ParamID::subOctave, -1.0f),
        pv (ParamID::f1Model, choice (7)), pv (ParamID::f1Cutoff, 220.0f),
        pv (ParamID::f1Reso, 0.05f), pv (ParamID::f1EnvAmount, 0.1f),
        pv (e1a, 0.008f), pv (e1s, 1.0f), pv (e1r, 0.12f),
        pv (ParamID::limiterOn, onOff (true)),
        pv (ParamID::outputGain, -5.0f)
    });

    add ("808 Long", "Sub",
    {
        pv (o1w, choice (14)), pv (o1l, 0.0f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 1.0f),
        pv (ParamID::subShape, choice (0)), pv (ParamID::subOctave, 0.0f),
        pv (ParamID::f1Model, choice (7)), pv (ParamID::f1Cutoff, 400.0f),
        pv (ParamID::f1Reso, 0.0f),
        pv (e1a, 0.001f), pv (e1d, 2.6f), pv (e1s, 0.0f), pv (e1r, 0.4f),
        pv (ParamID::env (0, "curve_d"), 0.55f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (1)),
        pv (ParamID::distDrive, 0.15f), pv (ParamID::distMix, 0.35f),
        pv (ParamID::outputGain, -4.0f)
    },
    {
        // Pitch drop d'attacco: l'inviluppo 3 scende veloce sul pitch. È ciò
        // che distingue un 808 da una sinusoide.
        ml (0, ModSource::Env3, ModDest::AllOscPitch, 0.35f)
    });

    add ("Wobble Sub", "Sub",
    {
        pv (o1w, choice (14)), pv (o1l, 0.6f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.8f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 320.0f),
        pv (ParamID::f1Reso, 0.55f), pv (ParamID::f1EnvAmount, 0.0f),
        pv (l1s, choice (0)), pv (l1sy, onOff (true)), pv (l1dv, choice (10)),
        pv (e1s, 1.0f), pv (e1r, 0.15f),
        pv (ParamID::outputGain, -5.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Filter1Cutoff, 0.42f)
    });

    //==========================================================================
    // PLUCK / STAB
    //==========================================================================

    add ("Acid Pluck", "Pluck",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 500.0f), pv (ParamID::f1Reso, 0.72f),
        pv (ParamID::f1EnvAmount, 0.8f), pv (ParamID::f1Drive, 2.6f),
        pv (e1a, 0.001f), pv (e1d, 0.16f), pv (e1s, 0.0f), pv (e1r, 0.06f),
        pv (e2a, 0.001f), pv (e2d, 0.10f), pv (e2s, 0.0f),
        pv (ParamID::delayOn, onOff (true)), pv (ParamID::delayMix, 0.22f),
        pv (ParamID::delayFeedback, 0.32f), pv (ParamID::delayPingPong, onOff (true)),
        pv (ParamID::outputGain, -5.0f)
    });

    add ("Metal Stab", "Pluck",
    {
        pv (o1w, choice (15)), pv (o1l, 0.8f), pv (o1p, 0.6f),
        pv (o2e, onOff (true)), pv (o2w, choice (9)), pv (o2l, 0.5f), pv (o2p, 0.4f),
        pv (ParamID::ringAmount, 0.25f),
        pv (ParamID::f1Model, choice (10)), pv (ParamID::f1Cutoff, 1200.0f),
        pv (ParamID::f1Reso, 0.5f), pv (ParamID::f1EnvAmount, 0.65f),
        pv (e1a, 0.001f), pv (e1d, 0.22f), pv (e1s, 0.0f), pv (e1r, 0.08f),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.2f),
        // La tabella Metallic ha ampiezze per armonica basse per costruzione
        // (spettro sparso): senza questo recupero il preset esce quindici dB
        // sotto gli altri e sembra rotto.
        pv (ParamID::compOn, onOff (true)), pv (ParamID::compThreshold, -30.0f),
        pv (ParamID::compRatio, 3.0f), pv (ParamID::compMakeup, 8.0f),
        pv (ParamID::outputGain, 2.0f)
    });

    //==========================================================================
    // FX / SPERIMENTALI
    //==========================================================================

    add ("Comb Resonator", "FX",
    {
        pv (o1w, choice (12)), pv (o1l, 0.7f),
        pv (ParamID::noiseEnable, onOff (true)), pv (ParamID::noiseLevel, 0.3f),
        pv (ParamID::f1Model, choice (13)), pv (ParamID::f1Cutoff, 900.0f),
        pv (ParamID::f1Reso, 0.8f),
        pv (ParamID::f2Model, choice (7)), pv (ParamID::f2Cutoff, 3000.0f),
        pv (e1a, 0.002f), pv (e1d, 0.9f), pv (e1s, 0.2f), pv (e1r, 0.5f),
        pv (ParamID::outputGain, -8.0f)
    });

    add ("FM Bite", "FX",
    {
        pv (o1w, choice (9)), pv (o1l, 0.9f), pv (o1p, 0.3f),
        pv (o2e, onOff (true)), pv (o2w, choice (0)), pv (o2l, 0.0f), pv (o2o, 1.0f),
        pv (ParamID::fmAmount, 0.35f), pv (ParamID::fmSource, choice (1)),
        pv (ParamID::f1Cutoff, 900.0f), pv (ParamID::f1Reso, 0.4f),
        pv (ParamID::f1EnvAmount, 0.5f),
        pv (e1d, 0.35f), pv (e1s, 0.4f),
        pv (ParamID::outputGain, -7.0f)
    },
    {
        ml (0, ModSource::Env2, ModDest::FMAmount, 0.5f)
    });

    add ("Chaos Bass", "FX",
    {
        pv (o1w, choice (11)), pv (o1l, 0.85f), pv (o1f, 0.4f),
        pv (o2e, onOff (true)), pv (o2w, choice (13)), pv (o2l, 0.5f), pv (o2fi, 4.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 700.0f),
        pv (ParamID::f1Reso, 0.65f), pv (ParamID::f1Drive, 4.5f),
        pv (l1s, choice (9)), pv (l1r, 6.0f),
        pv (l2s, choice (6)), pv (l2r, 8.0f),
        pv (ParamID::phaserOn, onOff (true)), pv (ParamID::phaserMix, 0.4f),
        pv (ParamID::outputGain, -8.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Osc1Fold, 0.5f),
        ml (1, ModSource::LFO2, ModDest::Osc2Position, 0.6f),
        ml (2, ModSource::LFO1, ModDest::Filter1Cutoff, 0.3f)
    });

    //==========================================================================
    // SEQUENCER — preset che accendono il sequencer interno
    //==========================================================================

    add ("Seq: Acid Line", "Sequencer",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 320.0f), pv (ParamID::f1Reso, 0.8f),
        pv (ParamID::f1EnvAmount, 0.75f), pv (ParamID::f1Drive, 3.6f),
        pv (e2d, 0.17f), pv (e2s, 0.0f),
        pv (ParamID::accentAmount, 0.7f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 1.0f),
        pv (ParamID::seqDivision, choice (13)), pv (ParamID::seqGateLength, 0.55f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distDrive, 0.3f),
        pv (ParamID::outputGain, -5.0f)
    });

    add ("Seq: Rolling 16ths", "Sequencer",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 420.0f), pv (ParamID::f1Reso, 0.7f),
        pv (ParamID::f1EnvAmount, 0.65f),
        pv (e2d, 0.11f), pv (e2s, 0.0f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 2.0f),
        pv (ParamID::seqSwing, 0.18f), pv (ParamID::seqGateLength, 0.42f),
        pv (ParamID::delayOn, onOff (true)), pv (ParamID::delayMix, 0.18f),
        pv (ParamID::outputGain, -5.0f)
    });

    //==========================================================================
    // POLY
    //==========================================================================

    add ("Poly Warm", "Poly",
    {
        pv (ParamID::voiceMode, choice (2)), pv (ParamID::polyVoices, 8.0f),
        pv (o1w, choice (5)), pv (o1l, 0.8f), pv (o1p, 0.6f), pv (o1u, 3.0f), pv (o1d, 0.1f),
        pv (o2e, onOff (true)), pv (o2w, choice (2)), pv (o2l, 0.5f), pv (o2o, -1.0f),
        pv (ParamID::f1Model, choice (2)), pv (ParamID::f1Cutoff, 1600.0f),
        pv (ParamID::f1Reso, 0.2f), pv (ParamID::f1EnvAmount, 0.35f),
        pv (ParamID::f1KeyTrack, 0.5f),
        pv (e1a, 0.02f), pv (e1d, 0.8f), pv (e1s, 0.7f), pv (e1r, 0.6f),
        pv (e2a, 0.04f), pv (e2d, 1.2f), pv (e2s, 0.3f),
        pv (ParamID::chorusOn, onOff (true)), pv (ParamID::chorusMix, 0.4f),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.25f),
        pv (ParamID::outputGain, -9.0f)
    });

    add ("Poly Formant Pad", "Poly",
    {
        pv (ParamID::voiceMode, choice (2)), pv (ParamID::polyVoices, 8.0f),
        pv (o1w, choice (7)), pv (o1l, 0.75f), pv (o1u, 5.0f), pv (o1d, 0.16f),
        pv (ParamID::f1Model, choice (7)), pv (ParamID::f1Cutoff, 2400.0f),
        pv (ParamID::f1Reso, 0.15f), pv (ParamID::f1KeyTrack, 0.6f),
        pv (e1a, 0.4f), pv (e1d, 1.5f), pv (e1s, 0.75f), pv (e1r, 1.6f),
        pv (l1s, choice (0)), pv (l1r, 0.25f),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.4f),
        pv (ParamID::reverbDecay, 4.5f), pv (ParamID::reverbSize, 0.8f),
        pv (ParamID::outputGain, -11.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Osc1Position, 0.4f),
        ml (1, ModSource::LFO1, ModDest::Filter1Cutoff, 0.15f)
    });

    //==========================================================================
    // HARDWARE — ricostruzioni di suoni classici
    //==========================================================================

    add ("TB Original Saw", "Hardware",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 480.0f),
        pv (ParamID::f1Reso, 0.62f), pv (ParamID::f1EnvAmount, 0.6f),
        pv (ParamID::f1Drive, 1.4f), pv (ParamID::f1BassComp, 0.3f),
        pv (e1a, 0.0012f), pv (e1s, 1.0f), pv (e1r, 0.016f),
        pv (e2a, 0.0012f), pv (e2d, 0.2f), pv (e2s, 0.0f),
        pv (ParamID::accentAmount, 0.55f), pv (ParamID::accentDecay, 0.2f),
        pv (ParamID::glideTime, 0.03f), pv (ParamID::dfEnvAttack, 0.004f),   // slide e soft attack misurati su 303
        pv (ParamID::outputGain, -4.0f)
    });

    add ("TB Original Square", "Hardware",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 420.0f),
        pv (ParamID::f1Reso, 0.6f), pv (ParamID::f1EnvAmount, 0.58f),
        pv (ParamID::f1Drive, 1.4f), pv (ParamID::f1BassComp, 0.3f),
        pv (e1a, 0.0012f), pv (e1s, 1.0f), pv (e1r, 0.016f),
        pv (e2a, 0.0012f), pv (e2d, 0.22f), pv (e2s, 0.0f),
        pv (ParamID::accentAmount, 0.55f),
        pv (ParamID::glideTime, 0.03f), pv (ParamID::dfEnvAttack, 0.004f),   // slide e soft attack misurati su 303
        pv (ParamID::outputGain, -4.0f)
    });

    // Calibrato su 44 loop registrati da un TB-303 (tools/calibration): filtro
    // aperto con risonanza al massimo, inviluppo corto, fondamentale attenuata
    // (-13 dB sotto 190 Hz: la "magrezza" del 303 registrato) e un filo di grana.
    add ("TB Recorded", "Hardware",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 1500.0f),
        pv (ParamID::f1Reso, 0.98f), pv (ParamID::f1EnvAmount, 0.077f),
        pv (ParamID::f1Drive, 3.4f), pv (ParamID::f1BassComp, 0.0f),
        pv (e2d, 0.29f), pv (e2s, 0.0f), pv (e2cd, 0.35f),
        pv (e1a, 0.0015f), pv (e1s, 1.0f), pv (e1r, 0.018f),
        pv (ParamID::accentAmount, 0.65f), pv (ParamID::accentDecay, 0.20f),
        pv (ParamID::glideTime, 0.03f), pv (ParamID::dfEnvAttack, 0.004f),
        pv (ParamID::eqLowFreq, 193.0f), pv (ParamID::eqLowGain, -12.9f), pv (ParamID::eqHighGain, 3.0f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (1)),
        pv (ParamID::distDrive, 0.15f), pv (ParamID::distMix, 0.6f),
        pv (ParamID::outputGain, -2.0f)
    });

    // Stadio valvolare ETA-1: 12AX7 spinta con sag, il 303 "respira" sugli accenti.
    add ("Acid Tube 12AX7", "Hardware",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 900.0f),
        pv (ParamID::f1Reso, 0.85f), pv (ParamID::f1EnvAmount, 0.35f),
        pv (ParamID::f1Drive, 2.4f),
        pv (e2d, 0.3f), pv (e2s, 0.0f),
        pv (e1a, 0.0015f), pv (e1s, 1.0f), pv (e1r, 0.018f),
        pv (ParamID::accentAmount, 0.7f), pv (ParamID::accentDecay, 0.20f),
        pv (ParamID::glideTime, 0.03f), pv (ParamID::dfEnvAttack, 0.004f),
        pv (ParamID::tubeOn, onOff (true)), pv (ParamID::tubeModel, choice (6)),
        pv (ParamID::tubeDrive, 0.62f), pv (ParamID::tubeSag, 0.55f),
        pv (ParamID::tubeBias, 0.15f), pv (ParamID::tubeCharacter, 0.4f),
        pv (ParamID::tubeWarmth, 2.0f), pv (ParamID::tubeTone, 9000.0f),
        pv (ParamID::outputGain, -2.0f)
    });

    // Hard tekno: pentodo EF86 spinto, bias caldo, presenza alta per tagliare il mix.
    add ("Hard Tube EF86", "Hardware",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Model, choice (0)), pv (ParamID::f1Cutoff, 1400.0f),
        pv (ParamID::f1Reso, 0.6f), pv (ParamID::f1EnvAmount, 0.3f),
        pv (ParamID::f1Drive, 3.0f),
        pv (e2d, 0.22f), pv (e2s, 0.0f),
        pv (e1a, 0.001f), pv (e1s, 1.0f), pv (e1r, 0.03f),
        pv (ParamID::accentAmount, 0.6f),
        pv (ParamID::tubeOn, onOff (true)), pv (ParamID::tubeModel, choice (12)),
        pv (ParamID::tubeDrive, 0.85f), pv (ParamID::tubeSag, 0.35f),
        pv (ParamID::tubeBias, 0.4f), pv (ParamID::tubeCharacter, 0.6f),
        pv (ParamID::tubePresence, 3.0f), pv (ParamID::tubeTone, 11000.0f),
        pv (ParamID::tubeOutput, 6.0f), pv (ParamID::outputGain, 0.0f)
    });

    add ("Moog Round", "Hardware",
    {
        pv (o1w, choice (0)), pv (o1l, 0.9f),
        pv (o2e, onOff (true)), pv (o2w, choice (1)), pv (o2l, 0.55f), pv (o2fi, -6.0f),
        pv (o3e, onOff (true)), pv (o3w, choice (0)), pv (o3l, 0.45f), pv (o3o, -1.0f),
        pv (ParamID::f1Model, choice (2)), pv (ParamID::f1Cutoff, 800.0f),
        pv (ParamID::f1Reso, 0.35f), pv (ParamID::f1EnvAmount, 0.45f),
        pv (ParamID::f1Drive, 2.2f), pv (ParamID::f1KeyTrack, 0.35f),
        pv (e1a, 0.004f), pv (e1d, 0.6f), pv (e1s, 0.85f), pv (e1r, 0.12f),
        pv (e2d, 0.5f), pv (e2s, 0.1f),
        pv (ParamID::outputGain, -8.0f)
    });

    add ("SH Bass", "Hardware",
    {
        pv (o1w, choice (6)), pv (o1l, 0.95f), pv (o1p, 0.35f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.45f),
        pv (ParamID::subShape, choice (2)),
        pv (ParamID::f1Model, choice (2)), pv (ParamID::f1Cutoff, 600.0f),
        pv (ParamID::f1Reso, 0.45f), pv (ParamID::f1EnvAmount, 0.55f),
        pv (e1a, 0.002f), pv (e1d, 0.4f), pv (e1s, 0.5f), pv (e1r, 0.1f),
        pv (e2d, 0.3f), pv (e2s, 0.0f),
        pv (l1s, choice (0)), pv (l1r, 5.0f),
        pv (ParamID::outputGain, -7.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::Osc1Position, 0.25f, ModSource::ModWheel)
    });

    //==========================================================================
    // FISH — controlli Devil Fish e motore neurale (v1.1)
    //==========================================================================

    add ("DF Acid Fish", "Fish",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 300.0f), pv (ParamID::f1Reso, 0.82f),
        pv (ParamID::f1EnvAmount, 0.74f), pv (ParamID::f1Drive, 2.8f),
        pv (e2d, 0.21f), pv (e2s, 0.0f), pv (e1s, 1.0f), pv (e1r, 0.018f),
        pv (ParamID::accentAmount, 0.75f), pv (ParamID::accentFilter, 1.0f),
        pv (ParamID::accentDecay, 0.24f), pv (ParamID::glideTime, 0.17f),
        pv (ParamID::dfFilterFm, 0.22f), pv (ParamID::dfMuffler, 0.35f),
        pv (ParamID::dfSweepSpeed, 0.4f), pv (ParamID::dfEnvAttack, 0.004f),
        pv (ParamID::dfNeuralDrive, 0.35f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 1.0f),
        pv (ParamID::seqGateLength, 0.55f),
        pv (ParamID::spaceMonoFreq, 120.0f),
        pv (ParamID::outputGain, -5.0f)
    });

    add ("DF Neural Screamer", "Fish",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 240.0f), pv (ParamID::f1Reso, 0.9f),
        pv (ParamID::f1EnvAmount, 0.85f), pv (ParamID::f1Drive, 4.0f),
        pv (e2d, 0.15f), pv (e2s, 0.0f), pv (e1r, 0.012f),
        pv (ParamID::accentAmount, 0.85f), pv (ParamID::accentDecay, 0.18f),
        pv (ParamID::dfNeuralDrive, 0.8f), pv (ParamID::dfFilterFm, 0.45f),
        pv (ParamID::dfMuffler, 0.2f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 2.0f),
        pv (ParamID::seqSwing, 0.12f),
        pv (ParamID::eqLowFreq, 90.0f), pv (ParamID::eqLowGain, 2.5f),
        pv (ParamID::spaceMonoFreq, 140.0f),
        pv (ParamID::outputGain, -7.0f)
    });

    add ("DF Vibrato Lead", "Fish",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f), pv (o1o, 1.0f),
        pv (ParamID::f1Cutoff, 900.0f), pv (ParamID::f1Reso, 0.6f),
        pv (ParamID::f1EnvAmount, 0.45f),
        pv (e1a, 0.002f), pv (e1s, 1.0f), pv (e1r, 0.15f),
        pv (e2d, 0.5f), pv (e2s, 0.2f),
        pv (ParamID::dfVibSpeed, 5.5f), pv (ParamID::dfVibDepth, 0.3f),
        pv (ParamID::dfEnvAttack, 0.02f), pv (ParamID::glideTime, 0.28f),
        pv (ParamID::delayOn, onOff (true)), pv (ParamID::delayMix, 0.22f),
        pv (ParamID::delayPingPong, onOff (true)),
        pv (ParamID::spaceWidth, 1.3f),
        pv (ParamID::outputGain, -7.0f)
    });

    //==========================================================================
    // ARP — arpeggiatore e combinazioni col sequencer
    //==========================================================================

    add ("Arp Tekno Tribe", "Arp",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 520.0f), pv (ParamID::f1Reso, 0.7f),
        pv (ParamID::f1EnvAmount, 0.6f), pv (e2d, 0.12f), pv (e2s, 0.0f),
        pv (ParamID::arpEnable, onOff (true)), pv (ParamID::arpMode, choice (2)),
        pv (ParamID::arpOctaves, 2.0f), pv (ParamID::arpGate, 0.35f),
        pv (ParamID::arpSwing, 0.12f), pv (ParamID::arpAccent, choice (3)),
        pv (ParamID::arpLatch, onOff (true)),
        pv (ParamID::delayOn, onOff (true)), pv (ParamID::delayMix, 0.2f),
        pv (ParamID::delayDivL, choice (9)), pv (ParamID::delayDivR, choice (10)),
        pv (ParamID::spaceAutopan, 0.25f), pv (ParamID::spaceAutopanDiv, choice (4)),
        pv (ParamID::spaceMonoFreq, 110.0f),
        pv (ParamID::outputGain, -6.0f)
    });

    add ("Seq Rhythm Arp Notes", "Arp",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 330.0f), pv (ParamID::f1Reso, 0.8f),
        pv (ParamID::f1EnvAmount, 0.72f), pv (e2d, 0.18f), pv (e2s, 0.0f),
        pv (ParamID::accentAmount, 0.7f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 5.0f),
        pv (ParamID::arpEnable, onOff (true)), pv (ParamID::arpMode, choice (0)),
        pv (ParamID::arpLatch, onOff (true)),
        pv (ParamID::playCombine, choice (1)),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distDrive, 0.3f), pv (ParamID::distMix, 0.5f),
        pv (ParamID::outputGain, -5.0f)
    });

    add ("Arp Transposed Acid", "Arp",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 420.0f), pv (ParamID::f1Reso, 0.75f),
        pv (ParamID::f1EnvAmount, 0.66f), pv (e2d, 0.14f), pv (e2s, 0.0f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 3.0f),
        pv (ParamID::seqDivision, choice (7)),
        pv (ParamID::arpEnable, onOff (true)), pv (ParamID::arpMode, choice (0)),
        pv (ParamID::arpOctaves, 2.0f), pv (ParamID::arpGate, 0.4f),
        pv (ParamID::arpLatch, onOff (true)),
        pv (ParamID::playCombine, choice (2)),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.12f),
        pv (ParamID::outputGain, -6.0f)
    });

    add ("Hardfloor Ratchet", "Arp",
    {
        pv (o1w, choice (4)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 380.0f), pv (ParamID::f1Reso, 0.78f),
        pv (ParamID::f1EnvAmount, 0.7f), pv (ParamID::f1Drive, 3.0f),
        pv (e2d, 0.13f), pv (e2s, 0.0f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 6.0f),
        pv (ParamID::seqSwing, 0.08f), pv (ParamID::seqGateLength, 0.45f),
        pv (ParamID::dfNeuralDrive, 0.25f),
        pv (ParamID::compOn, onOff (true)), pv (ParamID::compThreshold, -16.0f),
        pv (ParamID::compRatio, 4.0f),
        pv (ParamID::outputGain, -6.0f)
    });

    //==========================================================================
    // SPACE — immagine stereo e catena riordinata
    //==========================================================================

    add ("Space Wide Reese", "Space",
    {
        pv (o1w, choice (0)), pv (o1l, 0.9f), pv (o1u, 7.0f), pv (o1d, 0.14f),
        pv (o2e, onOff (true)), pv (o2w, choice (0)), pv (o2l, 0.8f), pv (o2fi, 9.0f),
        pv (o2u, 5.0f), pv (o2d, 0.1f),
        pv (ParamID::subEnable, onOff (true)), pv (ParamID::subLevel, 0.55f),
        pv (ParamID::f1Model, choice (2)), pv (ParamID::f1Cutoff, 1100.0f),
        pv (ParamID::f1Reso, 0.25f), pv (ParamID::f1EnvAmount, 0.2f),
        pv (e1s, 1.0f), pv (e1r, 0.2f),
        pv (ParamID::spaceWidth, 1.6f), pv (ParamID::spaceMonoFreq, 160.0f),
        pv (ParamID::spaceHaas, 0.006f),
        // Catena: distorsione dopo il chorus, riverbero in fondo dopo lo Space.
        pv (ParamID::fxSlot (0), choice (2)), pv (ParamID::fxSlot (1), choice (0)),
        pv (ParamID::fxSlot (2), choice (1)), pv (ParamID::fxSlot (3), choice (6)),
        pv (ParamID::fxSlot (4), choice (4)), pv (ParamID::fxSlot (5), choice (7)),
        pv (ParamID::fxSlot (6), choice (5)), pv (ParamID::fxSlot (7), choice (3)),
        pv (ParamID::chorusOn, onOff (true)), pv (ParamID::chorusMix, 0.3f),
        pv (ParamID::distOn, onOff (true)), pv (ParamID::distAlgo, choice (0)),
        pv (ParamID::distDrive, 0.25f), pv (ParamID::distMix, 0.4f),
        pv (ParamID::reverbOn, onOff (true)), pv (ParamID::reverbMix, 0.15f),
        pv (ParamID::outputGain, -8.0f)
    });

    add ("Space Autopan Groove", "Space",
    {
        pv (o1w, choice (3)), pv (o1l, 1.0f),
        pv (ParamID::f1Cutoff, 600.0f), pv (ParamID::f1Reso, 0.65f),
        pv (ParamID::f1EnvAmount, 0.55f), pv (e2d, 0.2f), pv (e2s, 0.0f),
        pv (ParamID::seqEnable, onOff (true)), pv (ParamID::seqPattern, 4.0f),
        pv (ParamID::seqDirection, choice (2)),
        pv (ParamID::spaceAutopan, 0.6f), pv (ParamID::spaceAutopanDiv, choice (3)),
        pv (ParamID::spaceRotate, 0.2f),
        pv (ParamID::delayOn, onOff (true)), pv (ParamID::delayMix, 0.18f),
        pv (ParamID::delayPingPong, onOff (true)),
        pv (ParamID::outputGain, -6.0f)
    },
    {
        ml (0, ModSource::LFO1, ModDest::FxSpaceWidth, 0.4f),
        ml (1, ModSource::Env2, ModDest::FxDelayMix, 0.2f)
    });

    currentIndex = 0;
}

} // namespace bassx
