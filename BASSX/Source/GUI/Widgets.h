/*
  ==============================================================================

    BASSX — Widgets.h

    Componenti riutilizzabili dell'interfaccia.

  ==============================================================================
*/

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "BassXLookAndFeel.h"
#include "Assets.h"
#include "../DSP/WavetableBank.h"

namespace bassx
{

//==============================================================================
/**
    Sfondo a texture con riempimento a piastrelle.

    Le texture dei pannelli sono 700 px di larghezza; la finestra è più larga.
    Ripeterle a specchio invece che semplicemente affiancarle evita la cucitura
    verticale visibile che si formerebbe al bordo, perché la spazzolatura è
    orizzontale e continua attraverso la giunzione.
*/
class TexturedPanel  : public juce::Component
{
public:
    enum class Kind { Metal, Dark, Module, Orange };

    explicit TexturedPanel (Kind k) : kind (k) {}

    void setKind (Kind k) { kind = k; repaint(); }

    /** Aggiunge quattro viti agli angoli. */
    void setShowScrews (bool shouldShow) { showScrews = shouldShow; repaint(); }

    /** Bordo inciso attorno al pannello. */
    void setShowBorder (bool shouldShow) { showBorder = shouldShow; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();

        switch (kind)
        {
            case Kind::Dark:                                  // corpo: nero + micro-griglia
                g.fillAll (Palette::bg0);
                if (assets->panelDark.isValid())
                    tileMirrored (g, assets->panelDark, bounds);
                break;

            case Kind::Metal:                                 // testata e fila principale
                g.fillAll (Palette::bg1);
                if (assets->panelMetal.isValid())
                    tileMirrored (g, assets->panelMetal, bounds);
                g.setColour (Palette::line);
                g.fillRect (bounds.withTop (bounds.getBottom() - 1));
                break;

            case Kind::Orange:                                // banda: linea dati LED
            {
                g.fillAll (Palette::bg0);
                const float y = (float) bounds.getCentreY();
                g.setColour (Palette::orange.withAlpha (0.10f));
                g.fillRect (0.0f, y - 3.0f, (float) getWidth(), 6.0f);
                g.setColour (Palette::orange.withAlpha (0.85f));
                g.fillRect (0.0f, y - 0.5f, (float) getWidth(), 1.0f);
                // Segmenti tipo bus dati, tenui.
                g.setColour (Palette::orange.withAlpha (0.35f));
                for (int x = 6; x < getWidth(); x += 48)
                    g.fillRect ((float) x, y - 3.0f, 18.0f, 1.0f);
                break;
            }

            case Kind::Module:                                // riquadro di modulo
            {
                const auto b = bounds.toFloat().reduced (0.5f);
                auto shape = Shapes::chamfer (b, 10.0f);
                g.setColour (Palette::bg1.withAlpha (0.94f));
                g.fillPath (shape);
                g.setColour (Palette::line);
                g.strokePath (shape, juce::PathStrokeType (1.0f));

                // Tacca d'accento in alto a sinistra, sul bordo del modulo.
                g.setColour (Palette::orange.withAlpha (0.25f));
                g.fillRect (b.getX() + 10.0f, b.getY() - 1.0f, 26.0f, 4.0f);
                g.setColour (Palette::orange);
                g.fillRect (b.getX() + 10.0f, b.getY(), 26.0f, 2.0f);
                break;
            }
        }
    }

    static void tileMirrored (juce::Graphics& g, const juce::Image& img, juce::Rectangle<int> area)
    {
        const int iw = img.getWidth();
        const int ih = img.getHeight();

        if (iw <= 0 || ih <= 0 || area.isEmpty())
            return;

        // Il ritaglio va impostato UNA VOLTA nello spazio di destinazione,
        // prima di applicare qualunque trasformazione. Ritagliare dopo aver
        // specchiato significa ritagliare una regione riflessa, che cade fuori
        // dall'area e lascia un buco nero — il difetto classico delle
        // piastrellature con trasformazione.
        juce::Graphics::ScopedSaveState outer (g);
        g.reduceClipRegion (area);

        for (int ty = 0, row = 0; ty < area.getHeight(); ty += ih, ++row)
        {
            for (int tx = 0, col = 0; tx < area.getWidth(); tx += iw, ++col)
            {
                auto transform = juce::AffineTransform();

                // Specchiatura alternata: elimina la cucitura fra piastrelle,
                // perché il bordo destro di una piastrella coincide con il
                // bordo destro della successiva riflessa.
                if (col & 1)
                    transform = transform.scaled (-1.0f, 1.0f).translated ((float) iw, 0.0f);

                if (row & 1)
                    transform = transform.scaled (1.0f, -1.0f).translated (0.0f, (float) ih);

                transform = transform.translated ((float) (area.getX() + tx),
                                                  (float) (area.getY() + ty));

                g.drawImageTransformed (img, transform, false);
            }
        }
    }

private:
    Kind kind;
    bool showScrews = false;
    bool showBorder = false;
    SharedAssets assets;
};

//==============================================================================
/**
    Knob con etichetta serigrafata sopra e valore leggibile sotto.

    Il valore compare solo durante il trascinamento o al passaggio del mouse:
    un pannello con venti numeri sempre accesi non somiglia a un pannello
    hardware, e i numeri sono utili solo nel momento in cui si regola.
*/
class LabelledKnob  : public juce::Component,
                      private juce::Slider::Listener,
                      private juce::Timer
{
public:
    LabelledKnob (const juce::String& labelText,
                  BassXLookAndFeel::KnobStyle style = BassXLookAndFeel::KnobStyle::Chrome,
                  bool labelOnMetal = false)
        : caption (labelText), onMetal (labelOnMetal)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                    juce::MathConstants<float>::pi * 2.75f, true);
        slider.getProperties().set (BassXLookAndFeel::knobStyleProperty, (int) style);

        // Trascinamento verticale con velocità ridotta tenendo shift: senza,
        // un range di 18 kHz su 60 pixel rende impossibile la regolazione fine.
        slider.setVelocityBasedMode (false);
        slider.setDoubleClickReturnValue (true, 0.0);

        slider.addListener (this);
        addAndMakeVisible (slider);
    }

    juce::Slider& getSlider() noexcept { return slider; }

    void setCaption (const juce::String& text) { caption = text; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto text = caption.toUpperCase();
        g.setFont (BassXLookAndFeel::getCondensed (juce::jlimit (9.0f, 12.0f, (float) captionHeight * 0.82f), true));
        g.setColour (slider.isMouseOverOrDragging() ? Palette::darkText : Palette::darkText.withAlpha (0.78f));
        g.drawFittedText (text, getLocalBounds().removeFromTop (captionHeight), juce::Justification::centred, 1, 0.7f);

        // Lettura del valore sotto il knob, solo mentre lo si regola.
        if (showValue)
        {
            g.setFont (BassXLookAndFeel::getLcdFont (juce::jlimit (9.0f, 11.5f, (float) valueHeight * 0.8f)));
            Shapes::glowText (g, slider.getTextFromValue (slider.getValue()), valueArea,
                              juce::Justification::centred, Palette::lcdGreen, 0.25f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop (captionHeight);

        // Diametro fisso (limitato): knob uguali in tutte le pagine, mai giganti.
        const int side = juce::jmax (12, juce::jmin (maxKnob, area.getWidth(), area.getHeight() - valueHeight));
        slider.setBounds (area.removeFromTop (side).withSizeKeepingCentre (side, side));
        valueArea = area.removeFromTop (valueHeight).withSizeKeepingCentre (juce::jmax (side + 16, 54), valueHeight);
    }

    /** Diametro massimo del knob in pixel (default compatto). */
    void setMaxKnob (int px) { maxKnob = px; resized(); }

    /** Altezza minima che serve per mostrare caption, knob pieno e valore. */
    int getPreferredHeight() const noexcept { return captionHeight + maxKnob + valueHeight; }

    void setCaptionHeight (int h) { captionHeight = h; resized(); }

private:
    void sliderValueChanged (juce::Slider*) override
    {
        showValue = true;
        startTimer (900);
        repaint();
    }

    void sliderDragStarted (juce::Slider*) override { showValue = true; stopTimer(); repaint(); }
    void sliderDragEnded (juce::Slider*) override { startTimer (900); }

    void timerCallback() override
    {
        stopTimer();
        showValue = false;
        repaint();
    }

    juce::Slider slider;
    juce::String caption;
    bool onMetal = false;
    bool showValue = false;
    int captionHeight = 14;
    int valueHeight = 13;
    int maxKnob = 42;
    juce::Rectangle<int> valueArea;
};

//==============================================================================
/** LED a cupola. Il valore 0..1 pilota l'alfa dell'immagine accesa sopra
    quella spenta, così l'accensione è continua invece che binaria. */
class LedComponent  : public juce::Component
{
public:
    enum class Colour { Red, Amber, Green };

    explicit LedComponent (Colour c) : colour (c) {}

    void setLevel (float newLevel)
    {
        newLevel = juce::jlimit (0.0f, 1.0f, newLevel);

        // Ridisegna solo se la differenza è visibile: un LED che si ridisegna
        // 60 volte al secondo per variazioni di 1/255 spreca il thread grafico.
        if (std::abs (newLevel - level) > 0.02f)
        {
            level = newLevel;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto c = colour == Colour::Red ? Palette::danger
                     : colour == Colour::Amber ? Palette::warn : Palette::orange;
        const auto b = getLocalBounds().toFloat();
        auto core = b.withSizeKeepingCentre (b.getWidth() * 0.55f, b.getHeight() * 0.55f);

        g.setColour (Palette::bg0);
        g.fillRect (core.expanded (1.0f));
        g.setColour (Palette::line.brighter (0.2f));
        g.drawRect (core.expanded (1.0f), 1.0f);

        if (level > 0.01f)
        {
            g.setColour (c.withAlpha (0.28f * level));
            g.fillRect (b);
            g.setColour (c.withAlpha (juce::jmin (1.0f, 0.25f + level)));
            g.fillRect (core);
        }
    }

private:
    Colour colour;
    float level = 0.0f;
};

//==============================================================================
/** VU a barre con tenuta di picco. */
class LevelMeter  : public juce::Component
{
public:
    void setLevels (float l, float r)
    {
        left = l;
        right = r;

        peakLeft  = juce::jmax (peakLeft * 0.96f, l);
        peakRight = juce::jmax (peakRight * 0.96f, r);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour (Palette::bg0);
        g.fillRect (area);
        g.setColour (Palette::line);
        g.drawRect (area, 1.0f);

        auto inner = area.reduced (3.0f);
        const float half = inner.getHeight() * 0.5f;
        drawBar (g, inner.removeFromTop (half).reduced (0.0f, 1.0f), left, peakLeft);
        drawBar (g, inner.reduced (0.0f, 1.0f), right, peakRight);
    }

private:
    static void drawBar (juce::Graphics& g, juce::Rectangle<float> area, float level, float peak)
    {
        // Barra a segmenti LED in dB (-48..+6).
        auto toNorm = [] (float lin)
        {
            const float db = juce::jlimit (-48.0f, 6.0f, 20.0f * std::log10 (juce::jmax (lin, 1.0e-6f)));
            return juce::jmap (db, -48.0f, 6.0f, 0.0f, 1.0f);
        };

        constexpr int segments = 24;
        const float segW = area.getWidth() / (float) segments;
        const float lit = toNorm (level) * (float) segments;
        const int peakSeg = (int) (toNorm (peak) * (float) segments);

        for (int i = 0; i < segments; ++i)
        {
            const float n = (float) i / (float) segments;
            const auto c = n > 0.89f ? Palette::danger : n > 0.75f ? Palette::warn : Palette::orange;
            auto seg = juce::Rectangle<float> (area.getX() + segW * (float) i, area.getY(), segW - 1.0f, area.getHeight());
            const bool on = (float) i < lit || (i == peakSeg && peak > 0.001f);
            g.setColour (on ? c : c.withAlpha (0.10f));
            g.fillRect (seg);
        }
    }

    float left = 0.0f, right = 0.0f, peakLeft = 0.0f, peakRight = 0.0f;
};

//==============================================================================
/**
    Visualizzatore della wavetable.

    Disegna il frame corrente della tabella selezionata più una serie di frame
    dietro, in prospettiva, come fa Serum. Non è decorazione: vedere come la
    forma cambia lungo la position è l'unico modo pratico di capire cosa si sta
    per modulare.
*/
class WavetableDisplay  : public juce::Component
{
public:
    /** version: cambia quando la tabella cambia contenuto (campione nuovo). */
    void setSource (const Wavetable* table, float position, int version = 0, const juce::String& caption = {})
    {
        if (table == currentTableP && version == currentVersion && caption == currentCaption
            && std::abs (position - currentPosition) < 0.004f)
            return;

        currentTableP = table;
        currentVersion = version;
        currentCaption = caption;
        currentPosition = position;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat().reduced (2.0f);

        // Vetro scuro
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff0d0f14), 0.0f, area.getY(),
                                                 juce::Colour (0xff05070a), 0.0f, area.getBottom(), false));
        g.fillRoundedRectangle (area, 3.0f);

        g.setColour (juce::Colours::black);
        g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);

        if (currentCaption.isNotEmpty())
        {
            g.setFont (11.0f);
            g.setColour (Palette::lcdGreen.withAlpha (0.8f));
            g.drawText (currentCaption.toUpperCase(), area.reduced (5.0f, 2.0f), juce::Justification::topRight, true);
        }

        if (currentTableP == nullptr)
            return;

        const auto& table = *currentTableP;
        const int numFrames = table.getNumFrames();

        if (numFrames <= 0)
            return;

        // Griglia di riferimento
        g.setColour (juce::Colour (0x18ffffff));
        g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

        const int ghosts = juce::jmin (7, numFrames);

        for (int i = ghosts - 1; i >= 0; --i)
        {
            const float depth = (float) i / (float) juce::jmax (1, ghosts - 1);

            const int frameIndex = juce::jlimit (0, numFrames - 1,
                (int) std::round (currentPosition * (float) (numFrames - 1))
                    + (i == 0 ? 0 : (i - ghosts / 2) * juce::jmax (1, numFrames / 12)));

            const float inset = depth * area.getHeight() * 0.16f;
            const float xShift = depth * area.getWidth() * 0.06f;

            auto row = area.reduced (0.0f, inset).translated (xShift, -inset * 0.35f);

            drawFrame (g, table.frames[(size_t) frameIndex], row,
                       i == 0 ? Palette::orangeBright : Palette::orange.withAlpha (0.16f + 0.10f * (1.0f - depth)),
                       i == 0 ? 1.9f : 1.0f);
        }
    }

private:
    static void drawFrame (juce::Graphics& g, const WavetableFrame& frame,
                           juce::Rectangle<float> area, juce::Colour colour, float thickness)
    {
        // Si usa il livello di mip più ricco: è la rappresentazione più fedele
        // di ciò che l'oscillatore produce alle frequenze basse, che sono
        // quelle di un basso.
        const auto& data = frame.mip[0];

        if (data.empty())
            return;

        juce::Path path;

        const int points = juce::jlimit (64, 512, (int) area.getWidth());
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.42f;

        for (int i = 0; i < points; ++i)
        {
            const float t = (float) i / (float) (points - 1);
            const int index = juce::jlimit (0, kTableSize - 1, (int) (t * (float) kTableSize));
            const float y = midY - data[(size_t) index] * halfH;
            const float x = area.getX() + t * area.getWidth();

            if (i == 0) path.startNewSubPath (x, y);
            else        path.lineTo (x, y);
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (thickness));
    }

    const Wavetable* currentTableP = nullptr;
    int currentVersion = -1;
    juce::String currentCaption;
    float currentPosition = -1.0f;
};

//==============================================================================
/** Etichetta di sezione: testo su una barretta con linea di separazione. */
class SectionHeader  : public juce::Component
{
public:
    SectionHeader (const juce::String& t, juce::Colour accent = Palette::orange)
        : title (t.toUpperCase()), lcdAccent (accent == Palette::lcdGreen) {}

    void setTitle (const juce::String& t) { title = t.toUpperCase(); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();

        g.setFont (BassXLookAndFeel::getCondensed (juce::jlimit (10.0f, 14.0f,
                                                                 (float) getHeight() * 0.72f), true));

        const int textWidth = juce::GlyphArrangement::getStringWidthInt (
            BassXLookAndFeel::getCondensed (juce::jlimit (10.0f, 14.0f, (float) getHeight() * 0.72f), true),
            title) + 8;

        auto textArea = area.removeFromLeft (juce::jmin (textWidth, area.getWidth()));

        // Quadratino LED + titolo + filo sottile fino al bordo.
        auto mark = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ 3.0f, (float) area.getCentreY() });
        g.setColour (lcdAccent ? Palette::lcdGreen : Palette::orange);   // segue il tema a runtime
        g.fillRect (mark);
        textArea = textArea.withTrimmedLeft (10);
        g.setColour (Palette::darkText);
        g.drawText (title, textArea.withWidth (textArea.getWidth() + 10), juce::Justification::centredLeft, false);

        if (! area.isEmpty())
        {
            const float y = (float) area.getCentreY() + 0.5f;
            g.setColour (Palette::line);
            g.fillRect ((float) area.getX() + 14.0f, y, (float) area.getWidth() - 14.0f, 1.0f);
        }
    }

private:
    juce::String title;
    bool lcdAccent = false;
};

//==============================================================================
/** Display LCD per il nome del preset e i messaggi. */
class LcdDisplay  : public juce::Component
{
public:
    void setLines (const juce::String& top, const juce::String& bottom)
    {
        if (topLine != top || bottomLine != bottom)
        {
            topLine = top;
            bottomLine = bottom;
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        auto shape = Shapes::chamfer (b, 8.0f);

        g.setColour (Palette::bg0);
        g.fillPath (shape);

        // Bagliore del pannello e scanline.
        g.setGradientFill (juce::ColourGradient (Palette::orange.withAlpha (0.07f), b.getCentreX(), b.getCentreY(),
                                                 juce::Colours::transparentBlack, b.getX(), b.getY(), true));
        g.fillPath (shape);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        for (float y = b.getY() + 1.0f; y < b.getBottom(); y += 3.0f)
            g.fillRect (b.getX(), y, b.getWidth(), 1.0f);

        g.setColour (Palette::orange.withAlpha (0.5f));
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        auto area = getLocalBounds().reduced (12, 5);
        auto topArea = area.removeFromTop (area.getHeight() * 6 / 10);

        g.setFont (BassXLookAndFeel::getLcdFont (juce::jlimit (11.0f, 19.0f, (float) topArea.getHeight() * 0.8f)));
        Shapes::glowText (g, topLine, topArea, juce::Justification::centredLeft, Palette::lcdGreen, 0.3f);

        g.setFont (BassXLookAndFeel::getLcdFont (juce::jlimit (9.0f, 12.0f, (float) area.getHeight() * 0.78f)));
        g.setColour (Palette::lcdGreen.withAlpha (0.55f));
        g.drawText (bottomLine, area, juce::Justification::centredLeft, false);
    }

private:
    juce::String topLine, bottomLine;
};

} // namespace bassx
