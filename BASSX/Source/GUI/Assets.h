/*
  ==============================================================================

    BASSX — Assets.h

    Accesso alle texture incorporate nel binario.

    Le immagini vengono decodificate una volta sola e tenute in una cache di
    processo. Decodificarle a ogni repaint costerebbe millisecondi per frame:
    lo sprite sheet cromato da solo è di 96x6240 pixel, cioè 2.4 milioni di
    pixel da decomprimere.

    La cache è condivisa fra tutte le istanze del plugin: aprire otto tracce
    con BASSX non deve moltiplicare per otto la memoria delle texture.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

namespace bassx
{

//==============================================================================
class AssetCache
{
public:
    AssetCache()
    {
        panelMetal   = load (BinaryData::panel_metal_png,   BinaryData::panel_metal_pngSize);
        panelDark    = load (BinaryData::panel_dark_png,    BinaryData::panel_dark_pngSize);
        panelModule  = load (BinaryData::panel_module_png,  BinaryData::panel_module_pngSize);
        bandOrange   = load (BinaryData::band_orange_png,   BinaryData::band_orange_pngSize);

        knobChrome   = load (BinaryData::knob_chrome_png,   BinaryData::knob_chrome_pngSize);
        // Tema Anthracite Neon: un solo knob per tutti gli stili (le immagini
        // JUCE sono a conteggio di riferimento, nessuna copia dei pixel).
        knobDark     = knobChrome;
        knobOrange   = knobChrome;

        screw        = load (BinaryData::screw_png,         BinaryData::screw_pngSize);

        ledRedOn     = load (BinaryData::led_red_on_png,    BinaryData::led_red_on_pngSize);
        ledRedOff    = load (BinaryData::led_red_off_png,   BinaryData::led_red_off_pngSize);
        ledAmberOn   = load (BinaryData::led_amber_on_png,  BinaryData::led_amber_on_pngSize);
        ledAmberOff  = load (BinaryData::led_amber_off_png, BinaryData::led_amber_off_pngSize);
        ledGreenOn   = load (BinaryData::led_green_on_png,  BinaryData::led_green_on_pngSize);
        ledGreenOff  = load (BinaryData::led_green_off_png, BinaryData::led_green_off_pngSize);

        buttonUp     = load (BinaryData::button_up_png,     BinaryData::button_up_pngSize);
        buttonDown   = load (BinaryData::button_down_png,   BinaryData::button_down_pngSize);

        sliderTrack  = load (BinaryData::slider_track_png,  BinaryData::slider_track_pngSize);
        sliderThumb  = load (BinaryData::slider_thumb_png,  BinaryData::slider_thumb_pngSize);

        lcd          = load (BinaryData::lcd_png,           BinaryData::lcd_pngSize);
    }

    juce::Image panelMetal, panelDark, panelModule, bandOrange;
    juce::Image knobChrome, knobDark, knobOrange;
    juce::Image screw;
    juce::Image ledRedOn, ledRedOff, ledAmberOn, ledAmberOff, ledGreenOn, ledGreenOff;
    juce::Image buttonUp, buttonDown;
    juce::Image sliderTrack, sliderThumb;
    juce::Image lcd;

    /** Numero di fotogrammi negli sprite sheet dei knob. Deve coincidere con
        il parametro `frames` di tools/generate_textures.py. */
    static constexpr int knobFrames = 65;

private:
    static juce::Image load (const char* data, int size)
    {
        auto img = juce::ImageCache::getFromMemory (data, size);
        jassert (img.isValid());
        return img;
    }
};

using SharedAssets = juce::SharedResourcePointer<AssetCache>;

} // namespace bassx
