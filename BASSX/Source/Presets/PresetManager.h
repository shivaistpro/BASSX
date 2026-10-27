/*
  ==============================================================================

    BASSX — PresetManager.h

    Gestione dei preset di fabbrica e utente.

    Un preset contiene TUTTO lo stato sonoro: i ~230 parametri APVTS (compresi
    i quattro inviluppi completi, con delay/hold/curve), i 16 slot della
    matrice di modulazione e gli 8 pattern del sequencer. Questo è il motivo
    per cui il preset viene serializzato come ValueTree dell'intero stato e non
    come lista di coppie chiave-valore: aggiungere un parametro in futuro non
    rompe i preset già salvati, perché i parametri assenti nel file conservano
    il loro valore di default.

    I preset di fabbrica sono definiti come *scostamenti* dai default. Salvare
    230 valori per preset renderebbe il file sorgente illeggibile e, cosa
    peggiore, ogni cambio di default richiederebbe di rivedere ogni preset a
    mano.

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../Parameters.h"
#include "../DSP/ModMatrix.h"
#include "../DSP/Sequencer.h"

namespace bassx
{

//==============================================================================
/** Uno scostamento rispetto allo stato di default. */
struct PresetValue
{
    juce::String paramID;
    float value = 0.0f;
};

/** Un collegamento di modulazione dentro un preset di fabbrica. */
struct PresetModLink
{
    int       slot = 0;
    ModSource source = ModSource::None;
    ModDest   dest = ModDest::None;
    float     depth = 0.0f;
    ModSource via = ModSource::None;
};

struct FactoryPreset
{
    juce::String name;
    juce::String category;
    std::vector<PresetValue> values;
    std::vector<PresetModLink> modLinks;
};

//==============================================================================
class PresetManager
{
public:
    PresetManager (juce::AudioProcessorValueTreeState& state,
                   ModMatrix& matrix,
                   Sequencer& seq);

    //==========================================================================
    /** Categorie disponibili, nell'ordine in cui vanno mostrate. */
    juce::StringArray getCategories() const;

    /** Nomi dei preset di una categoria, o di tutti se la categoria è vuota. */
    juce::StringArray getPresetNames (const juce::String& category = {}) const;

    int getNumPresets() const noexcept { return (int) factoryPresets.size() + userPresets.size(); }

    /** Indice del preset attualmente caricato, -1 se lo stato è stato
        modificato dopo il caricamento. */
    int getCurrentPresetIndex() const noexcept { return currentIndex; }
    juce::String getCurrentPresetName() const;

    /** Nome del preset all'indice dato, stringa vuota se fuori range. */
    juce::String getPresetName (int index) const;

    /** Indici globali (quelli di loadPreset) dei preset di una categoria.
        Il menu deve usare questi e non cercare per nome: un preset utente
        salvato con lo stesso nome di uno di fabbrica caricava quello di
        fabbrica. */
    juce::Array<int> getPresetIndices (const juce::String& category) const;

    /** Chiamato da setStateInformation: rimette il nome del preset della
        sessione senza ricaricarlo (lo stato e' gia' quello salvato). */
    void restoreSessionPreset (int index, const juce::String& name);

    /** Parametri di impostazione (non di suono) che un preset non tocca:
        oversampling, accordatura, pitch bend, tetto del limiter. */
    static bool isSessionSetting (const juce::String& paramID);

    //==========================================================================
    void loadPreset (int index);
    void loadPresetByName (const juce::String& name);
    void loadNext();
    void loadPrevious();

    /** Salva lo stato corrente come preset utente. Restituisce false se il
        file non è scrivibile. */
    bool saveUserPreset (const juce::String& name);
    bool deleteUserPreset (const juce::String& name);

    void rescanUserPresets();

    juce::File getUserPresetDirectory() const;

    //==========================================================================
    /** Segna lo stato come modificato: chiamato dal processore quando un
        parametro cambia, così la GUI può mostrare "Nome *". */
    void markAsModified() noexcept { currentIndex = -1; }

    std::function<void()> onPresetChanged;

private:
    //==========================================================================
    void buildFactoryPresets();
    void applyFactoryPreset (const FactoryPreset& preset);
    void resetToDefaults();
    void applyParameterTree (const juce::ValueTree& paramState);

    juce::AudioProcessorValueTreeState& apvts;
    ModMatrix& modMatrix;
    Sequencer& sequencer;

    std::vector<FactoryPreset> factoryPresets;
    juce::Array<juce::File> userPresets;

    int currentIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace bassx
