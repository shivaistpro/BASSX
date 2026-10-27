/*
  ==============================================================================

    BASSX — PluginEditor.h

  ==============================================================================
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "GUI/BassXLookAndFeel.h"
#include "GUI/Widgets.h"
#include "GUI/Pages.h"

//==============================================================================
/**
    Interfaccia principale.

    Struttura verticale, dall'alto:

      1. Testata in alluminio: nome, browser dei preset su display LCD,
         indicatore di livello, modalità di voce.
      2. Fila di controlli "classica" — Waveform, Tune, Cutoff, Resonance,
         Env Mod, Decay, Accent, Slide, Volume — sempre visibile su qualunque
         pagina. È la fila che si trova sul pannello di una Bass Line, ed è la
         ragione per cui lo strumento resta suonabile senza aprire nulla.
      3. Banda arancione di separazione con i LED di gate e accento.
      4. Barra delle pagine e pagina attiva, su plastica scura.
      5. Tastiera su schermo.

    La finestra è ridimensionabile con proporzioni vincolate: i controlli sono
    posizionati proporzionalmente, quindi qualunque dimensione dentro i limiti
    resta usabile.
*/
class BassXAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit BassXAudioProcessorEditor (BassXAudioProcessor&);
    ~BassXAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    //==========================================================================
    void timerCallback() override;
    void buildTopRow();
    void buildHeader();
    void applyTheme (int index);
    void showColourPicker();
    void showPage (int index);
    void refreshPresetDisplay();
    void showPresetMenu();
    void showSaveDialog();

    //==========================================================================
    BassXAudioProcessor& processorRef;
    bassx::BassXLookAndFeel lookAndFeel;

    // --- Testata ---------------------------------------------------------------
    bassx::TexturedPanel headerPanel { bassx::TexturedPanel::Kind::Metal };
    bassx::TexturedPanel topRowPanel { bassx::TexturedPanel::Kind::Metal };
    bassx::TexturedPanel bandPanel   { bassx::TexturedPanel::Kind::Orange };
    bassx::TexturedPanel bodyPanel   { bassx::TexturedPanel::Kind::Dark };

    juce::Label titleLabel, subtitleLabel;
    bassx::LcdDisplay presetDisplay;
    juce::TextButton presetPrev { "<" }, presetNext { ">" };
    juce::TextButton presetBrowse { "PRESETS" }, presetSave { "SAVE" };

    bassx::LevelMeter meter;
    juce::ComboBox voiceModeBox;
    juce::ComboBox themeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> voiceModeAttachment;

    juce::Slider polyVoicesSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> polyVoicesAttachment;

    // --- Fila principale ---------------------------------------------------------
    juce::OwnedArray<bassx::LabelledKnob> topKnobs;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> topAttachments;
    juce::ComboBox mainWaveBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mainWaveAttachment;
    juce::Label mainWaveLabel;
    juce::TextButton mainWaveOn { "ON" };      // layer 303 (oscillatore 1) acceso/spento
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mainWaveOnAttachment;

    bassx::LedComponent gateLed { bassx::LedComponent::Colour::Red };
    bassx::LedComponent accentLed { bassx::LedComponent::Colour::Amber };
    juce::Label gateLabel, accentLabel;

    // --- Pagine --------------------------------------------------------------------
    juce::OwnedArray<juce::TextButton> pageButtons;
    juce::OwnedArray<bassx::PageBase> pages;
    int currentPage = 0;

    // --- Tastiera --------------------------------------------------------------------
    juce::MidiKeyboardState keyboardState;
    /** Tastiera nel tema cyber: tasti neri pieni, tasti premuti accesi col colore del tema. */
    struct CyberKeyboard  : public juce::MidiKeyboardComponent
    {
        using juce::MidiKeyboardComponent::MidiKeyboardComponent;

        void drawWhiteNote (int note, juce::Graphics& g, juce::Rectangle<float> area,
                            bool isDown, bool isOver, juce::Colour, juce::Colour) override
        {
            g.setColour (bassx::Palette::bg2);
            g.fillRect (area);
            if (isDown || isOver)
            {
                g.setColour (bassx::Palette::orange.withAlpha (isDown ? 0.55f : 0.12f));
                g.fillRect (area);
            }
            g.setColour (bassx::Palette::bg0);
            g.fillRect (area.withWidth (1.0f));

            if (note % 12 == 0)
            {
                g.setColour (bassx::Palette::dimText);
                g.setFont (bassx::BassXLookAndFeel::getCondensed (10.0f, false));
                g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 3),
                            area.withTrimmedBottom (3.0f), juce::Justification::centredBottom, false);
            }
        }

        void drawBlackNote (int, juce::Graphics& g, juce::Rectangle<float> area,
                            bool isDown, bool isOver, juce::Colour) override
        {
            g.setColour (isDown ? bassx::Palette::orange : bassx::Palette::bg0);
            g.fillRect (area);
            if (! isDown)
            {
                g.setColour (isOver ? bassx::Palette::orange.withAlpha (0.4f) : bassx::Palette::line);
                g.drawRect (area, 1.0f);
            }
        }
    };

    CyberKeyboard keyboard { keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };

    /** Ponte fra la tastiera su schermo e il processore. Necessario perché
        MidiKeyboardComponent lavora su un MidiKeyboardState, che notifica sul
        thread messaggi; il processore accoda gli eventi in un buffer protetto
        e li consuma nel blocco successivo. */
    struct KeyboardListener  : public juce::MidiKeyboardState::Listener
    {
        explicit KeyboardListener (BassXAudioProcessor& p) : proc (p) {}

        void handleNoteOn (juce::MidiKeyboardState*, int, int midiNote, float velocity) override
        {
            proc.handleGuiNoteOn (midiNote, velocity);
        }

        void handleNoteOff (juce::MidiKeyboardState*, int, int midiNote, float) override
        {
            proc.handleGuiNoteOff (midiNote);
        }

        BassXAudioProcessor& proc;
    };

    KeyboardListener keyboardListener { processorRef };

    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BassXAudioProcessorEditor)
};
