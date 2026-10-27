/*
  ==============================================================================

    BASSX — PluginEditor.cpp

  ==============================================================================
*/

#include "PluginEditor.h"

using namespace juce;
using namespace bassx;

namespace
{
    constexpr int kDefaultWidth  = 1220;
    constexpr int kDefaultHeight = 800;

    constexpr int kHeaderH  = 62;
    constexpr int kTopRowH  = 106;
    constexpr int kBandH    = 22;
    constexpr int kTabH     = 30;
    constexpr int kKeyboardH = 70;

    const StringArray kPageNames { "OSC", "FILTER", "FISH", "ENV", "LFO", "MOD", "SEQ", "ARP", "FX", "TUBE", "SPACE", "DUCK" };
}

//==============================================================================
BassXAudioProcessorEditor::BassXAudioProcessorEditor (BassXAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (headerPanel);
    addAndMakeVisible (topRowPanel);
    addAndMakeVisible (bandPanel);
    addAndMakeVisible (bodyPanel);


    buildHeader();
    buildTopRow();

    // --- Pagine ----------------------------------------------------------------
    pages.add (new OscPage       (processorRef.apvts, processorRef));
    pages.add (new FilterPage    (processorRef.apvts));
    pages.add (new FishPage      (processorRef.apvts, processorRef));
    pages.add (new EnvPage       (processorRef.apvts, processorRef));
    pages.add (new LfoPage       (processorRef.apvts));
    pages.add (new ModMatrixPage (processorRef.apvts, processorRef.modMatrix));
    pages.add (new SeqPage       (processorRef.apvts, processorRef));
    pages.add (new ArpPage       (processorRef.apvts, processorRef));
    pages.add (new FxPage        (processorRef.apvts));
    pages.add (new TubePage      (processorRef.apvts));
    pages.add (new SpacePage     (processorRef.apvts, processorRef));
    pages.add (new SidechainPage (processorRef.apvts, processorRef));

    jassert (pages.size() == kPageNames.size());

    for (auto* page : pages)
    {
        addChildComponent (page);
        page->setVisible (false);
    }

    for (int i = 0; i < kPageNames.size(); ++i)
    {
        auto* b = pageButtons.add (new TextButton (kPageNames[i]));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (0x8A551);
        b->setConnectedEdges (
            (i > 0 ? Button::ConnectedOnLeft : 0)
          | (i < kPageNames.size() - 1 ? Button::ConnectedOnRight : 0));
        b->onClick = [this, i] { showPage (i); };
        addAndMakeVisible (b);
    }

    showPage (0);

    // --- Tastiera ---------------------------------------------------------------
    keyboardState.addListener (&keyboardListener);
    keyboard.setAvailableRange (24, 96);
    keyboard.setLowestVisibleKey (28);
    keyboard.setKeyWidth (22.0f);
    addAndMakeVisible (keyboard);

    // --- Finestra ----------------------------------------------------------------
    constrainer.setMinimumSize (1020, 680);
    constrainer.setMaximumSize (2400, 1560);

    // Proporzioni fissate: il layout è denso e permettere proporzioni
    // arbitrarie significherebbe che a un certo punto le etichette dei knob si
    // sovrappongono ai knob stessi.
    constrainer.setFixedAspectRatio ((double) kDefaultWidth / (double) kDefaultHeight);
    setConstrainer (&constrainer);

    setResizable (true, true);
    setSize (kDefaultWidth, kDefaultHeight);

    refreshPresetDisplay();
    Theme::custom = Colour ((uint32) (int) processorRef.apvts.state.getProperty ("uiCustomColour",
                                                                                 (int) Theme::custom.getARGB()));
    applyTheme ((int) processorRef.apvts.state.getProperty ("uiTheme", 0));

    processorRef.presetManager.onPresetChanged = [this]
    {
        // Il callback può arrivare dal thread audio (setCurrentProgram
        // dell'host): la GUI va aggiornata sul thread messaggi.
        // SafePointer creato fuori dalla init-capture: MSVC risolve male `this`
        // dentro l'init-capture di una lambda annidata (errore C2440).
        Component::SafePointer<BassXAudioProcessorEditor> safe (this);
        MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->refreshPresetDisplay();
        });
    };

    startTimerHz (30);
}

BassXAudioProcessorEditor::~BassXAudioProcessorEditor()
{
    processorRef.presetManager.onPresetChanged = nullptr;
    keyboardState.removeListener (&keyboardListener);
    setLookAndFeel (nullptr);
}

//==============================================================================
void BassXAudioProcessorEditor::buildHeader()
{
    titleLabel.setText ("BASSX", dontSendNotification);
    titleLabel.setJustificationType (Justification::centredLeft);
    titleLabel.getProperties().set (BassXLookAndFeel::onMetalProperty, true);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("BASS LINE SYNTHESIZER", dontSendNotification);
    subtitleLabel.setJustificationType (Justification::centredLeft);
    subtitleLabel.getProperties().set (BassXLookAndFeel::onMetalProperty, true);
    addAndMakeVisible (subtitleLabel);

    addAndMakeVisible (presetDisplay);

    for (auto* b : { &presetPrev, &presetNext, &presetBrowse, &presetSave })
        addAndMakeVisible (*b);

    presetPrev.onClick  = [this] { processorRef.presetManager.loadPrevious(); refreshPresetDisplay(); };
    presetNext.onClick  = [this] { processorRef.presetManager.loadNext();     refreshPresetDisplay(); };
    presetBrowse.onClick = [this] { showPresetMenu(); };
    presetSave.onClick   = [this] { showSaveDialog(); };

    addAndMakeVisible (meter);

    // Tema colore: solo grafica, salvato nello stato del plugin (non automatizzabile).
    themeBox.addItemList (Theme::names(), 1);
    themeBox.setTooltip ("Colour theme");
    themeBox.onChange = [this]
    {
        applyTheme (themeBox.getSelectedItemIndex());
        if (themeBox.getSelectedItemIndex() == Theme::kCustom)
            showColourPicker();
    };
    addAndMakeVisible (themeBox);

    if (auto* choice = dynamic_cast<AudioParameterChoice*> (processorRef.apvts.getParameter (ParamID::voiceMode)))
        voiceModeBox.addItemList (choice->choices, 1);

    addAndMakeVisible (voiceModeBox);
    voiceModeAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef.apvts, ParamID::voiceMode, voiceModeBox);

    polyVoicesSlider.setSliderStyle (Slider::IncDecButtons);
    polyVoicesSlider.setTextBoxStyle (Slider::TextBoxLeft, false, 34, 20);
    polyVoicesSlider.setColour (Slider::textBoxTextColourId, Palette::darkText);
    polyVoicesSlider.setColour (Slider::textBoxBackgroundColourId, Palette::bg0);
    polyVoicesSlider.setColour (Slider::textBoxOutlineColourId, Palette::line);
    addAndMakeVisible (polyVoicesSlider);
    polyVoicesAttachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (
        processorRef.apvts, ParamID::polyVoices, polyVoicesSlider);
}

//==============================================================================
void BassXAudioProcessorEditor::applyTheme (int index)
{
    // shortcut: la palette è globale al processo, quindi due istanze aperte
    // insieme condividono l'ultimo tema scelto; per temi per-istanza servirebbe
    // passare la palette ai componenti invece di leggerla da Palette::.
    Theme::apply (index);
    lookAndFeel.refreshColours();
    processorRef.apvts.state.setProperty ("uiTheme", Theme::current, nullptr);
    processorRef.apvts.state.setProperty ("uiCustomColour", (int) Theme::custom.getARGB(), nullptr);

    subtitleLabel.setColour (Label::textColourId, Palette::orange);
    themeBox.setSelectedItemIndex (Theme::current, dontSendNotification);

    sendLookAndFeelChange();
    repaint();
}

//==============================================================================
void BassXAudioProcessorEditor::showColourPicker()
{
    // Selettore colore in un fumetto sotto il menu THEME: ogni modifica si
    // applica subito a tutta l'interfaccia.
    struct Picker  : public ColourSelector, private ChangeListener
    {
        explicit Picker (BassXAudioProcessorEditor& e)
            : ColourSelector (ColourSelector::showColourAtTop | ColourSelector::showColourspace
                              | ColourSelector::showSliders), editor (e)
        {
            setCurrentColour (Theme::custom, dontSendNotification);
            setColour (ColourSelector::backgroundColourId, Palette::bg1);
            setColour (ColourSelector::labelTextColourId, Palette::darkText);
            setSize (300, 320);
            addChangeListener (this);
        }

        void changeListenerCallback (ChangeBroadcaster*) override
        {
            Theme::custom = getCurrentColour();
            editor.applyTheme (Theme::kCustom);
        }

        BassXAudioProcessorEditor& editor;
    };

    CallOutBox::launchAsynchronously (std::make_unique<Picker> (*this), themeBox.getScreenBounds(), nullptr);
}

//==============================================================================
void BassXAudioProcessorEditor::buildTopRow()
{
    // Selettore di forma d'onda dell'oscillatore 1: sul pannello originale è un
    // interruttore a due posizioni; qui è la lista completa delle wavetable,
    // perché è il controllo che cambia di più il suono e va tenuto in primo
    // piano.
    if (auto* choice = dynamic_cast<AudioParameterChoice*> (
            processorRef.apvts.getParameter (ParamID::osc (0, "wave"))))
        mainWaveBox.addItemList (choice->choices, 1);

    addAndMakeVisible (mainWaveBox);
    mainWaveAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (
        processorRef.apvts, ParamID::osc (0, "wave"), mainWaveBox);

    mainWaveLabel.setText ("WAVEFORM", dontSendNotification);
    mainWaveLabel.setJustificationType (Justification::centred);
    mainWaveLabel.setFont (BassXLookAndFeel::getCondensed (11.0f, true));
    mainWaveLabel.getProperties().set (BassXLookAndFeel::onMetalProperty, true);
    addAndMakeVisible (mainWaveLabel);

    mainWaveOn.setClickingTogglesState (true);
    addAndMakeVisible (mainWaveOn);
    mainWaveOnAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (
        processorRef.apvts, ParamID::osc (0, "enable"), mainWaveOn);

    struct TopControl { const char* id; const char* caption; BassXLookAndFeel::KnobStyle style; };

    const TopControl controls[] =
    {
        { ParamID::masterTune,             "Tuning",    BassXLookAndFeel::KnobStyle::Chrome },
        { ParamID::f1Cutoff,               "Cut Off",   BassXLookAndFeel::KnobStyle::Chrome },
        { ParamID::f1Reso,                 "Resonance", BassXLookAndFeel::KnobStyle::Chrome },
        { ParamID::f1EnvAmount,            "Env Mod",   BassXLookAndFeel::KnobStyle::Chrome },
        { nullptr,                         "Decay",     BassXLookAndFeel::KnobStyle::Chrome },  // env2 decay
        { ParamID::accentAmount,           "Accent",    BassXLookAndFeel::KnobStyle::Orange },
        { ParamID::glideTime,              "Slide",     BassXLookAndFeel::KnobStyle::Chrome },
        { ParamID::f1Drive,                "Drive",     BassXLookAndFeel::KnobStyle::Orange },
        { ParamID::outputGain,             "Volume",    BassXLookAndFeel::KnobStyle::Chrome }
    };

    for (const auto& c : controls)
    {
        auto* k = topKnobs.add (new LabelledKnob (c.caption, c.style, true));
        k->setCaptionHeight (16);
        k->setMaxKnob (54);
        addAndMakeVisible (k);

        const String paramID = (c.id != nullptr) ? String (c.id) : ParamID::env (1, "decay");

        topAttachments.add (new AudioProcessorValueTreeState::SliderAttachment (
            processorRef.apvts, paramID, k->getSlider()));
    }

    addAndMakeVisible (gateLed);
    addAndMakeVisible (accentLed);

    gateLabel.setText ("GATE", dontSendNotification);
    accentLabel.setText ("ACCENT", dontSendNotification);

    for (auto* l : { &gateLabel, &accentLabel })
    {
        l->setFont (BassXLookAndFeel::getCondensed (10.0f, true));
        l->setJustificationType (Justification::centredLeft);
        l->getProperties().set (BassXLookAndFeel::onMetalProperty, true);
        addAndMakeVisible (*l);
    }
}

//==============================================================================
void BassXAudioProcessorEditor::showPage (int index)
{
    currentPage = jlimit (0, pages.size() - 1, index);

    for (int i = 0; i < pages.size(); ++i)
        pages[i]->setVisible (i == currentPage);

    for (int i = 0; i < pageButtons.size(); ++i)
        pageButtons[i]->setToggleState (i == currentPage, dontSendNotification);

    resized();
}

//==============================================================================
void BassXAudioProcessorEditor::refreshPresetDisplay()
{
    const auto name = processorRef.presetManager.getCurrentPresetName();
    const int index = processorRef.presetManager.getCurrentPresetIndex();
    const int total = processorRef.presetManager.getNumPresets();

    presetDisplay.setLines (name.toUpperCase(),
                            (index >= 0 ? String (index + 1) : String ("--"))
                              + " / " + String (total) + "    BASSX");
}

void BassXAudioProcessorEditor::showPresetMenu()
{
    PopupMenu menu;

    auto& pm = processorRef.presetManager;
    const auto categories = pm.getCategories();

    for (const auto& category : categories)
    {
        PopupMenu sub;

        // Indici diretti: cercare per nome confondeva un preset utente con
        // quello di fabbrica che ha lo stesso nome.
        for (const int index : pm.getPresetIndices (category))
            sub.addItem (index + 1, pm.getPresetName (index), true, index == pm.getCurrentPresetIndex());

        menu.addSubMenu (category, sub);
    }

    menu.addSeparator();
    menu.addItem (100000, "Apri la cartella dei preset utente...");

    menu.showMenuAsync (PopupMenu::Options().withTargetComponent (presetBrowse),
                        [this] (int result)
    {
        if (result <= 0)
            return;

        if (result == 100000)
        {
            processorRef.presetManager.getUserPresetDirectory().revealToUser();
            return;
        }

        processorRef.presetManager.loadPreset (result - 1);
        refreshPresetDisplay();
    });
}

void BassXAudioProcessorEditor::showSaveDialog()
{
    auto* window = new AlertWindow ("Salva preset",
                                    "Nome del preset utente:",
                                    MessageBoxIconType::NoIcon);

    window->addTextEditor ("name", processorRef.presetManager.getCurrentPresetName(), "Nome:");
    window->addButton ("Salva", 1, KeyPress (KeyPress::returnKey));
    window->addButton ("Annulla", 0, KeyPress (KeyPress::escapeKey));

    window->enterModalState (true, ModalCallbackFunction::create (
        [this, window] (int result)
        {
            if (result == 1)
            {
                const auto name = window->getTextEditorContents ("name").trim();

                if (name.isNotEmpty())
                {
                    if (! processorRef.presetManager.saveUserPreset (name))
                        NativeMessageBox::showMessageBoxAsync (
                            MessageBoxIconType::WarningIcon, "BASSX",
                            "Impossibile scrivere il preset. Verifica i permessi della cartella.");

                    refreshPresetDisplay();
                }
            }

            delete window;
        }), false);
}

//==============================================================================
void BassXAudioProcessorEditor::timerCallback()
{
    meter.setLevels (processorRef.meters.outputLevelL.load(),
                     processorRef.meters.outputLevelR.load());

    gateLed.setLevel (processorRef.meters.ampEnvLevel.load());
    accentLed.setLevel (jlimit (0.0f, 1.0f, processorRef.meters.accentCharge.load() / 1.2f));

    // Numero di voci: significativo solo in modalità polifonica.
    const bool poly = processorRef.apvts.getRawParameterValue (ParamID::voiceMode)->load() > 1.5f;

    if (polyVoicesSlider.isEnabled() != poly)
        polyVoicesSlider.setEnabled (poly);
}

//==============================================================================
void BassXAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (Palette::bg0);
}

//==============================================================================
void BassXAudioProcessorEditor::resized()
{
    // Il layout è calcolato su una scala derivata dall'altezza: le proporzioni
    // sono vincolate, quindi una sola scala basta a mantenere tutto coerente.
    const float scale = (float) getHeight() / (float) kDefaultHeight;

    auto scaled = [scale] (int v) { return roundToInt ((float) v * scale); };

    auto area = getLocalBounds();

    // --- Testata -----------------------------------------------------------------
    {
        auto header = area.removeFromTop (scaled (kHeaderH));
        headerPanel.setBounds (header);

        auto inner = header.reduced (scaled (26), scaled (6));

        // Marchio inciso a sinistra.
        {
            auto titleArea = inner.removeFromLeft (scaled (186));
            titleLabel.setBounds (titleArea.removeFromTop (titleArea.getHeight() * 3 / 5));
            titleLabel.setFont (BassXLookAndFeel::getCondensed (
                jlimit (18.0f, 34.0f, (float) titleLabel.getHeight() * 0.95f), true));
            subtitleLabel.setBounds (titleArea);
            subtitleLabel.setFont (BassXLookAndFeel::getCondensed (
                jlimit (8.0f, 12.0f, (float) titleArea.getHeight() * 0.72f), false));
        }

        // Gruppo di destra: modalità di voce, numero di voci, indicatore.
        {
            auto right = inner.removeFromRight (scaled (268));

            meter.setBounds (right.removeFromRight (scaled (104)).reduced (0, scaled (12)));
            right.removeFromRight (scaled (8));

            voiceModeBox.setBounds (right.removeFromTop (right.getHeight() / 2).reduced (0, 2));
            polyVoicesSlider.setBounds (right.reduced (0, 2));
        }

        inner.removeFromRight (scaled (8));
        themeBox.setBounds (inner.removeFromRight (scaled (84)).reduced (0, scaled (16)));
        inner.removeFromRight (scaled (10));
        inner.removeFromLeft (scaled (6));

        // Browser dei preset: occupa tutto lo spazio rimasto, così la finestra
        // può allargarsi senza lasciare un vuoto in mezzo alla testata.
        presetPrev.setBounds (inner.removeFromLeft (scaled (26)).reduced (0, scaled (7)));
        inner.removeFromLeft (2);
        presetSave.setBounds (inner.removeFromRight (scaled (58)).reduced (0, scaled (7)));
        inner.removeFromRight (4);
        presetBrowse.setBounds (inner.removeFromRight (scaled (78)).reduced (0, scaled (7)));
        inner.removeFromRight (scaled (8));
        presetNext.setBounds (inner.removeFromRight (scaled (26)).reduced (0, scaled (7)));
        inner.removeFromRight (2);
        presetDisplay.setBounds (inner.reduced (0, scaled (3)));
    }

    // --- Fila principale -------------------------------------------------------------
    {
        auto top = area.removeFromTop (scaled (kTopRowH));
        topRowPanel.setBounds (top);

        auto inner = top.reduced (scaled (26), scaled (6));

        // Selettore di forma d'onda
        auto waveArea = inner.removeFromLeft (scaled (128));
        {
            auto row = waveArea.removeFromTop (scaled (16));
            mainWaveOn.setBounds (row.removeFromRight (scaled (34)));
            mainWaveLabel.setBounds (row);
        }
        waveArea.removeFromTop (scaled (4));
        mainWaveBox.setBounds (waveArea.removeFromTop (scaled (24)));

        // LED sotto il selettore
        auto ledArea = waveArea.removeFromTop (scaled (24));

        // Il LED di accento e la sua etichetta stanno a destra, quello di gate a
        // sinistra: la ripartizione a metà tagliava "ACCENT" perché la parola è
        // più lunga di "GATE".
        auto gateArea = ledArea.removeFromLeft (scaled (54));
        gateLed.setBounds (gateArea.removeFromLeft (scaled (16)).withSizeKeepingCentre (scaled (13), scaled (13)));
        gateLabel.setBounds (gateArea);

        accentLed.setBounds (ledArea.removeFromLeft (scaled (16)).withSizeKeepingCentre (scaled (13), scaled (13)));
        accentLabel.setBounds (ledArea);

        inner.removeFromLeft (scaled (10));

        Array<Component*> items;
        for (auto* k : topKnobs)
            items.add (k);

        PageBase::layoutRow (items, inner, scaled (6), false);
    }

    // --- Banda arancione -------------------------------------------------------------
    bandPanel.setBounds (area.removeFromTop (scaled (kBandH)));

    // --- Tastiera --------------------------------------------------------------------
    {
        auto keyArea = area.removeFromBottom (scaled (kKeyboardH));
        keyboard.setBounds (keyArea);

        // La larghezza dei tasti si ricava dallo spazio disponibile, così la
        // tastiera riempie la finestra invece di lasciare una fascia vuota a
        // destra a qualunque ridimensionamento.
        const int lowest = 28;                       // E1
        const int highest = 96;                      // C7
        keyboard.setAvailableRange (lowest, highest);
        keyboard.setLowestVisibleKey (lowest);

        int whiteKeys = 0;
        for (int n = lowest; n <= highest; ++n)
            if (! MidiMessage::isMidiNoteBlack (n))
                ++whiteKeys;

        keyboard.setKeyWidth (jmax (10.0f, (float) keyArea.getWidth() / (float) jmax (1, whiteKeys)));
    }

    // --- Corpo ------------------------------------------------------------------------
    bodyPanel.setBounds (area);

    auto body = area.reduced (scaled (10), scaled (6));

    // Barra delle pagine
    {
        auto tabs = body.removeFromTop (scaled (kTabH));

        const int tabW = tabs.getWidth() / jmax (1, pageButtons.size());

        for (auto* b : pageButtons)
            b->setBounds (tabs.removeFromLeft (tabW).reduced (0, 2));

        body.removeFromTop (scaled (6));
    }

    for (auto* page : pages)
        page->setBounds (body);
}
