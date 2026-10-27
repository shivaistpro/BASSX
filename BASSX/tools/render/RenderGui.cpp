// Strumento di verifica: istanzia processore ed editor fuori da un host e
// salva uno screenshot dell'interfaccia. Non fa parte del plugin.
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    BassXAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // argv[3] opzionale: indice del tema colore.
    if (argc > 3)
        proc.apvts.state.setProperty ("uiTheme", atoi (argv[3]), nullptr);
    // argv[4] opzionale: colore CUSTOM in esadecimale (es. ff5500).
    if (argc > 4)
        proc.apvts.state.setProperty ("uiCustomColour",
                                      (int) (0xff000000u | (juce::uint32) juce::String (argv[4]).getHexValue32()), nullptr);

    // argv[5] opzionale: nome del preset da caricare prima dello screenshot.
    if (argc > 5)
        proc.presetManager.loadPresetByName (argv[5]);

    // argv[6] opzionale: campione audio da trasformare in wavetable sull'OSC 1.
    if (argc > 6)
    {
        juce::String err;
        if (proc.loadSampleWavetable (juce::File (argv[6]), err))
            proc.apvts.getParameter (bassx::ParamID::osc (0, "wave"))->setValueNotifyingHost (proc.apvts.getParameterRange (bassx::ParamID::osc (0, "wave")).convertTo0to1 ((float) bassx::kUserWaveIndex));
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    editor->setSize (1220, 800);

    const int page = (argc > 1) ? atoi (argv[1]) : 0;

    // Seleziona la pagina simulando il click sul tab corrispondente.
    std::function<void (juce::Component&, int&)> clickTab =
        [&] (juce::Component& c, int& remaining)
    {
        for (auto* child : c.getChildren())
        {
            if (auto* b = dynamic_cast<juce::TextButton*> (child))
            {
                static const juce::StringArray tabs { "OSC", "FILTER", "FISH", "ENV", "LFO", "MOD", "SEQ", "ARP", "FX", "TUBE", "SPACE", "DUCK" };
                if (tabs.contains (b->getButtonText()))
                {
                    if (tabs.indexOf (b->getButtonText()) == remaining)
                    {
                        // triggerClick() è asincrono e qui non gira un ciclo di
                        // messaggi: si invoca direttamente il gestore.
                        if (b->onClick) b->onClick();
                        remaining = -1;
                        return;
                    }
                }
            }
            clickTab (*child, remaining);
            if (remaining < 0) return;
        }
    };

    int target = page;
    clickTab (*editor, target);

    // Nessun ciclo di messaggi: lo snapshot disegna direttamente i componenti.

    auto img = editor->createComponentSnapshot (editor->getLocalBounds(), true);

    juce::File out (juce::String (argc > 2 ? argv[2] : "/tmp/gui.png"));
    juce::FileOutputStream stream (out);
    stream.setPosition (0);
    stream.truncate();
    juce::PNGImageFormat png;
    png.writeImageToStream (img, stream);

    editor.reset();
    return 0;
}
