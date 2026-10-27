/*
  ==============================================================================

    BASSX — BassXLookAndFeel.h

    Look and feel "cyber": superfici vettoriali scure, angoli smussati, LED
    nel colore del tema.

    Il corpo del knob resta uno sprite (metallo antracite, riflessi
    precalcolati che un disegno vettoriale non riproduce); tacche, lancetta e
    arco sono vettoriali, così seguono il tema e si accendono col valore.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Assets.h"

namespace bassx
{

//==============================================================================
/** Palette. Le tinte neutre sono fisse; l'accento (storicamente "orange") e i
    colori LCD dipendono dal tema scelto dall'utente e cambiano a runtime. */
namespace Palette
{
    inline juce::Colour orange       { 0xff33ff6a };   // accento del tema
    inline juce::Colour orangeBright { 0xffaaffc2 };
    inline juce::Colour lcdGreen     { 0xff6dff95 };
    inline juce::Colour lcdDim       { 0xff1f6b38 };

    inline const juce::Colour silverText { 0xffc9d1d9 };
    inline const juce::Colour darkText   { 0xffc9d1d9 };
    inline const juce::Colour dimText    { 0xff6b747e };
    inline const juce::Colour panelLine  { 0xff000000 };

    // Superfici del tema cyber, dal fondo verso i controlli.
    inline const juce::Colour bg0  { 0xff060709 };
    inline const juce::Colour bg1  { 0xff0c0e11 };
    inline const juce::Colour bg2  { 0xff13161a };
    inline const juce::Colour line { 0xff252a31 };
    inline const juce::Colour danger { 0xffff3b5c };
    inline const juce::Colour warn   { 0xffffb020 };
}

//==============================================================================
/** Temi colore dell'interfaccia (solo GUI, salvati nello stato del plugin). */
namespace Theme
{
    inline juce::StringArray names() { return { "NEON", "CYAN", "PLASMA", "AMBER", "ICE", "CUSTOM" }; }

    /** Indice della voce "colore a scelta". */
    inline constexpr int kCustom = 5;

    inline int current = 0;
    inline juce::Colour custom { 0xff33ff6a };   // colore scelto dall'utente per CUSTOM

    inline void apply (int index)
    {
        static const juce::uint32 accents[] { 0xff33ff6a, 0xff00e5ff, 0xffff2bd6, 0xffffa21f, 0xffd8e6f2 };
        current = juce::jlimit (0, kCustom, index);

        // Il colore scelto viene reso opaco e abbastanza luminoso da restare un LED.
        auto a = current == kCustom ? custom.withAlpha (1.0f) : juce::Colour (accents[current]);
        if (a.getBrightness() < 0.55f)
            a = a.withBrightness (0.55f);
        Palette::orange       = a;
        Palette::orangeBright = a.interpolatedWith (juce::Colours::white, 0.55f);
        Palette::lcdGreen     = a.interpolatedWith (juce::Colours::white, 0.25f);
        Palette::lcdDim       = a.withMultipliedBrightness (0.42f).withMultipliedSaturation (0.8f);
    }
}

//==============================================================================
/** Forme comuni del tema: rettangolo con angoli smussati in diagonale. */
namespace Shapes
{
    inline juce::Path chamfer (juce::Rectangle<float> b, float cut)
    {
        cut = juce::jmin (cut, b.getWidth() * 0.3f, b.getHeight() * 0.45f);
        juce::Path p;
        p.startNewSubPath (b.getX() + cut, b.getY());
        p.lineTo (b.getRight(), b.getY());
        p.lineTo (b.getRight(), b.getBottom() - cut);
        p.lineTo (b.getRight() - cut, b.getBottom());
        p.lineTo (b.getX(), b.getBottom());
        p.lineTo (b.getX(), b.getY() + cut);
        p.closeSubPath();
        return p;
    }

    /** Testo con alone: due passate tenui sotto quella piena. */
    inline void glowText (juce::Graphics& g, const juce::String& t, juce::Rectangle<int> r,
                          juce::Justification j, juce::Colour c, float glow = 0.35f)
    {
        g.setColour (c.withAlpha (glow * 0.5f));
        g.drawText (t, r.translated (-1, 0), j, false);
        g.drawText (t, r.translated (1, 0), j, false);
        g.setColour (c.withAlpha (glow));
        g.drawText (t, r.translated (0, 1), j, false);
        g.setColour (c);
        g.drawText (t, r, j, false);
    }
}

//==============================================================================
class BassXLookAndFeel  : public juce::LookAndFeel_V4
{
public:
    enum class KnobStyle { Chrome, Dark, Orange };

    /** Property da impostare su un Slider per scegliere lo stile del knob. */
    static constexpr const char* knobStyleProperty = "bassxKnobStyle";

    /** Mantenuta per compatibilità: nel tema cyber le etichette sono tutte chiare. */
    static constexpr const char* onMetalProperty = "bassxOnMetal";

    //==========================================================================
    BassXLookAndFeel() { refreshColours(); }

    /** Da chiamare dopo Theme::apply: i colori di sistema JUCE sono copie. */
    void refreshColours()
    {
        setColour (juce::Label::textColourId, Palette::darkText);
        setColour (juce::ComboBox::backgroundColourId, Palette::bg1);
        setColour (juce::ComboBox::textColourId, Palette::darkText);
        setColour (juce::ComboBox::outlineColourId, Palette::line);
        setColour (juce::ComboBox::arrowColourId, Palette::orange);
        setColour (juce::PopupMenu::backgroundColourId, Palette::bg1);
        setColour (juce::PopupMenu::textColourId, Palette::darkText);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::orange.withAlpha (0.18f));
        setColour (juce::PopupMenu::highlightedTextColourId, Palette::orangeBright);
        setColour (juce::TextButton::buttonColourId, Palette::bg2);
        setColour (juce::TextButton::textColourOffId, Palette::darkText);
        setColour (juce::TextButton::textColourOnId, Palette::orangeBright);
        setColour (juce::TextEditor::backgroundColourId, Palette::bg0);
        setColour (juce::TextEditor::textColourId, Palette::lcdGreen);
        setColour (juce::TextEditor::highlightColourId, Palette::orange.withAlpha (0.35f));
        setColour (juce::TextEditor::outlineColourId, Palette::line);
        setColour (juce::TextEditor::focusedOutlineColourId, Palette::orange);
        setColour (juce::ScrollBar::thumbColourId, Palette::line.brighter (0.4f));
        setColour (juce::Slider::backgroundColourId, Palette::bg0);
        setColour (juce::Slider::trackColourId, Palette::orange);
        setColour (juce::Slider::thumbColourId, Palette::orangeBright);
        setColour (juce::Slider::textBoxTextColourId, Palette::lcdGreen);
        setColour (juce::Slider::textBoxBackgroundColourId, Palette::bg0);
        setColour (juce::Slider::textBoxOutlineColourId, Palette::line);
        setColour (juce::AlertWindow::backgroundColourId, Palette::bg1);
        setColour (juce::AlertWindow::textColourId, Palette::darkText);
        setColour (juce::AlertWindow::outlineColourId, Palette::orange);
        setColour (juce::ListBox::backgroundColourId, Palette::bg0);
        setColour (juce::ListBox::textColourId, Palette::darkText);
        setColour (juce::ResizableWindow::backgroundColourId, Palette::bg0);
    }

    //==========================================================================
    // Knob rotativi: corpo da sprite (neutro) + LED vettoriali nel colore del tema
    //==========================================================================

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider& slider) override
    {
        juce::ignoreUnused (rotaryStartAngle, rotaryEndAngle);

        const auto& sheet = assets->knobChrome;
        const int side = juce::jmin (width, height) - 6;   // margine per l'arco LED esterno
        if (side < 8)
            return;

        const float cx = (float) x + (float) width * 0.5f;
        const float cy = (float) y + (float) height * 0.5f;
        const float R  = (float) side * 0.5f;

        if (sheet.isValid())
        {
            const int frameSize = sheet.getWidth();
            const int numFrames = sheet.getHeight() / juce::jmax (1, frameSize);
            const int frame = juce::jlimit (0, numFrames - 1, (int) std::round (sliderPos * (float) (numFrames - 1)));

            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (sheet, juce::roundToInt (cx - R), juce::roundToInt (cy - R), side, side,
                         0, frame * frameSize, frameSize, frameSize, false);
        }

        if (! slider.isEnabled())
            return;

        const bool hot = slider.isMouseOverOrDragging();
        const auto accent = hot ? Palette::orangeBright : Palette::orange;
        constexpr float pi = juce::MathConstants<float>::pi;

        // Angolo del valore: 0 = in alto, orario, +/-135 gradi (come lo sprite).
        const float valueA = (sliderPos - 0.5f) * 1.5f * pi;
        auto polar = [cx, cy] (float a, float r) { return juce::Point<float> (cx + r * std::sin (a), cy - r * std::cos (a)); };

        // Tacche della ghiera: si accendono in sequenza col valore (dissolvenza 15 gradi).
        const float tickW = juce::jmax (1.2f, R * 0.075f);
        for (int k = -4; k <= 4; ++k)
        {
            const float a = (float) k * pi / 6.0f;
            const float level = juce::jlimit (0.0f, 1.0f, (valueA - a) / (pi / 12.0f) + 1.0f);
            if (level <= 0.0f)
                continue;

            const juce::Line<float> tick (polar (a, R * 0.66f), polar (a, R * 0.88f));
            g.setColour (accent.withAlpha (0.25f * level));
            g.drawLine (tick, tickW * 2.6f);
            g.setColour (accent.withAlpha (level));
            g.drawLine (tick, tickW);
        }

        // Lancetta sul cappuccio.
        {
            const juce::Line<float> ptr (polar (valueA, R * 0.24f), polar (valueA, R * 0.43f));
            g.setColour (accent.withAlpha (0.3f));
            g.drawLine (ptr, tickW * 2.8f);
            g.setColour (accent.interpolatedWith (juce::Colours::white, 0.35f));
            g.drawLine (ptr, tickW * 1.1f);
        }

        // Arco LED esterno (bipolare dal centro).
        const float arcR = R + 2.5f;
        const float start = -0.75f * pi, end = 0.75f * pi;
        juce::Path track;
        track.addCentredArc (cx, cy, arcR, arcR, 0.0f, start, end, true);
        g.setColour (Palette::line);
        g.strokePath (track, juce::PathStrokeType (1.5f));

        const bool bipolar = slider.getMinimum() < -0.0001 && slider.getMaximum() > 0.0001
                          && std::abs (slider.getMinimum() + slider.getMaximum()) < 0.0001;
        const float origin = bipolar ? 0.0f : start;

        juce::Path value;
        value.addCentredArc (cx, cy, arcR, arcR, 0.0f, juce::jmin (origin, valueA), juce::jmax (origin, valueA), true);
        g.setColour (accent.withAlpha (0.22f));
        g.strokePath (value, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.strokePath (value, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    //==========================================================================
    // Fader lineari: guida sottile + cursore a barra luminosa
    //==========================================================================

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        juce::ignoreUnused (minSliderPos, maxSliderPos);

        if (style == juce::Slider::LinearBarVertical)
        {
            // Barre del sequencer: colonna di vetro con riempimento tenue e "tappo" LED.
            const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
            g.setColour (Palette::bg0);
            g.fillRect (b);
            g.setColour (Palette::line);
            g.drawRect (b, 1.0f);

            const bool bip = slider.getMinimum() < -0.0001 && slider.getMaximum() > 0.0001;
            const float zeroY = bip ? (float) slider.getPositionOfValue (0.0) : b.getBottom();
            auto fill = juce::Rectangle<float>::leftTopRightBottom (b.getX() + 2.0f, juce::jmin (zeroY, sliderPos),
                                                                    b.getRight() - 2.0f, juce::jmax (zeroY, sliderPos));
            const auto accent = slider.isMouseOverOrDragging() ? Palette::orangeBright : Palette::orange;
            g.setColour (accent.withAlpha (0.16f));
            g.fillRect (fill);
            g.setColour (accent.withAlpha (0.3f));
            g.fillRect (fill.getX(), sliderPos - 2.0f, fill.getWidth(), 4.0f);
            g.setColour (accent);
            g.fillRect (fill.getX(), sliderPos - 1.0f, fill.getWidth(), 2.0f);
            if (bip)
            {
                g.setColour (Palette::line.brighter (0.4f));
                g.fillRect (b.getX(), zeroY, b.getWidth(), 1.0f);
            }
            return;
        }

        if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal
            && style != juce::Slider::LinearBar)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                              minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
        const auto accent = slider.isMouseOverOrDragging() ? Palette::orangeBright : Palette::orange;

        const bool bipolar = slider.getMinimum() < -0.0001 && slider.getMaximum() > 0.0001;

        if (style == juce::Slider::LinearVertical)
        {
            auto track = b.withSizeKeepingCentre (4.0f, b.getHeight());
            g.setColour (Palette::bg0);
            g.fillRect (track);
            g.setColour (Palette::line);
            g.drawRect (track, 1.0f);

            const float zeroY = bipolar ? (float) slider.getPositionOfValue (0.0) : b.getBottom();
            auto fill = juce::Rectangle<float>::leftTopRightBottom (track.getX(), juce::jmin (zeroY, sliderPos),
                                                                    track.getRight(), juce::jmax (zeroY, sliderPos));
            g.setColour (accent.withAlpha (0.85f));
            g.fillRect (fill.reduced (1.0f, 0.0f));

            auto thumb = juce::Rectangle<float> (b.getWidth() * 0.8f, 4.0f).withCentre ({ b.getCentreX(), sliderPos });
            g.setColour (accent.withAlpha (0.25f));
            g.fillRect (thumb.expanded (2.0f));
            g.setColour (accent);
            g.fillRect (thumb);
        }
        else
        {
            auto track = style == juce::Slider::LinearBar ? b
                                                          : b.withSizeKeepingCentre (b.getWidth(), juce::jmin (6.0f, b.getHeight()));
            g.setColour (Palette::bg0);
            g.fillRect (track);
            g.setColour (Palette::line);
            g.drawRect (track, 1.0f);

            const float zeroX = bipolar ? (float) slider.getPositionOfValue (0.0) : b.getX();
            auto fill = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zeroX, sliderPos), track.getY(),
                                                                    juce::jmax (zeroX, sliderPos), track.getBottom());
            if (style == juce::Slider::LinearBar)
            {
                // Barra con testo sopra: riempimento tenue, filo luminoso in basso.
                g.setColour (accent.withAlpha (0.18f));
                g.fillRect (fill);
                g.setColour (accent);
                g.fillRect (fill.withTop (fill.getBottom() - 2.0f));
            }
            else
            {
                g.setColour (accent.withAlpha (0.22f));
                g.fillRect (fill.expanded (0.0f, 2.0f));
                g.setColour (accent);
                g.fillRect (fill.reduced (0.0f, 1.0f));
            }
        }
    }

    //==========================================================================
    // Etichette
    //==========================================================================

    void drawLabel (juce::Graphics& g, juce::Label& label) override
    {
        if (label.isBeingEdited())
            return;

        const auto area = label.getLocalBounds().reduced (label.getBorderSize().getLeft(),
                                                          label.getBorderSize().getTop());
        const auto font = label.getFont();
        const auto text = label.getText();

        if (text.isEmpty())
            return;

        g.setFont (font);
        g.setColour (label.findColour (juce::Label::textColourId));

        const int maxLines = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
        if (maxLines > 1)
            g.drawFittedText (text, area, label.getJustificationType(), maxLines, 1.0f);
        else
            g.drawText (text, area, label.getJustificationType(), false);
    }

    //==========================================================================
    // Tasti
    //==========================================================================

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& /*backgroundColour*/,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        auto b = button.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = button.getToggleState();
        const bool tab = button.getRadioGroupId() != 0;

        if (tab)
        {
            // Tab: nessun riquadro, solo testo e barra LED sotto quella attiva.
            if (on || shouldDrawButtonAsHighlighted)
            {
                auto bar = b.withTop (b.getBottom() - 2.0f).reduced (b.getWidth() * 0.12f, 0.0f);
                const auto c = on ? Palette::orange : Palette::line.brighter (0.6f);
                if (on)
                {
                    g.setColour (c.withAlpha (0.08f));
                    g.fillRect (b);
                    g.setColour (c.withAlpha (0.3f));
                    g.fillRect (bar.expanded (0.0f, 2.0f));
                }
                g.setColour (c);
                g.fillRect (bar);
            }
            return;
        }

        auto shape = Shapes::chamfer (b, 5.0f);
        g.setColour (on ? Palette::orange.withAlpha (0.12f) : Palette::bg2);
        g.fillPath (shape);

        if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            g.setColour (juce::Colours::white.withAlpha (shouldDrawButtonAsDown ? 0.08f : 0.04f));
            g.fillPath (shape);
        }

        g.setColour (on ? Palette::orange.withAlpha (0.9f) : Palette::line.brighter (0.15f));
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        // Spia LED a sinistra per i tasti on/off.
        if (button.getClickingTogglesState() && b.getWidth() > 34.0f)
        {
            auto led = juce::Rectangle<float> (3.0f, juce::jmax (6.0f, b.getHeight() * 0.42f))
                           .withCentre ({ b.getX() + 6.0f, b.getCentreY() });
            if (on)
            {
                g.setColour (Palette::orange.withAlpha (0.3f));
                g.fillRect (led.expanded (2.0f));
            }
            g.setColour (on ? Palette::orange : Palette::line.brighter (0.3f));
            g.fillRect (led);
        }
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& button,
                         bool isHighlighted, bool /*isDown*/) override
    {
        g.setFont (getTextButtonFont (button, button.getHeight()));

        auto area = button.getLocalBounds();
        const bool on = button.getToggleState();
        const bool tab = button.getRadioGroupId() != 0;

        if (button.getClickingTogglesState() && ! tab && area.getWidth() > 34)
            area.removeFromLeft (8);

        if (on)
            Shapes::glowText (g, button.getButtonText(), area, juce::Justification::centred, Palette::orangeBright, 0.25f);
        else
        {
            g.setColour (isHighlighted ? Palette::darkText : Palette::darkText.withAlpha (0.72f));
            g.drawText (button.getButtonText(), area, juce::Justification::centred, false);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return getCondensed (juce::jlimit (10.0f, 13.5f, (float) buttonHeight * 0.44f), true);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        drawButtonBackground (g, button, juce::Colours::black,
                              shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

        g.setFont (getCondensed (juce::jlimit (9.0f, 13.0f, (float) button.getHeight() * 0.44f), true));
        g.setColour (button.getToggleState() ? Palette::orangeBright : Palette::dimText);
        g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred, false);
    }

    //==========================================================================
    // Combo box e menu
    //==========================================================================

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                       int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/,
                       juce::ComboBox& box) override
    {
        const auto b = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
        auto shape = Shapes::chamfer (b, 4.0f);

        g.setColour (Palette::bg1);
        g.fillPath (shape);
        g.setColour (isButtonDown || box.isMouseOver (true) ? Palette::orange.withAlpha (0.7f)
                                                             : Palette::line.brighter (0.1f));
        g.strokePath (shape, juce::PathStrokeType (1.0f));

        // Chevron
        const float ax = (float) width - 11.0f, ay = (float) height * 0.5f;
        juce::Path arrow;
        arrow.startNewSubPath (ax - 3.5f, ay - 1.8f);
        arrow.lineTo (ax, ay + 1.8f);
        arrow.lineTo (ax + 3.5f, ay - 1.8f);
        g.setColour (box.isEnabled() ? Palette::orange : Palette::dimText);
        g.strokePath (arrow, juce::PathStrokeType (1.5f));
    }

    juce::Font getComboBoxFont (juce::ComboBox& box) override
    {
        return getCondensed (juce::jlimit (10.0f, 13.0f, (float) box.getHeight() * 0.52f), false);
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (7, 0, box.getWidth() - 22, box.getHeight());
        label.setFont (getComboBoxFont (box));
        label.setJustificationType (juce::Justification::centredLeft);
    }

    juce::Font getPopupMenuFont() override { return getCondensed (13.0f, false); }

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.fillAll (Palette::bg1);
        g.setColour (Palette::orange.withAlpha (0.6f));
        g.drawRect (0, 0, width, height, 1);
    }

    //==========================================================================
    // Barre di scorrimento sottili
    //==========================================================================

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isVertical, int thumbStart, int thumbSize, bool isMouseOver, bool) override
    {
        auto thumb = isVertical ? juce::Rectangle<int> (x + width / 2 - 1, thumbStart, 3, thumbSize)
                                : juce::Rectangle<int> (thumbStart, y + height / 2 - 1, thumbSize, 3);
        g.setColour (isMouseOver ? Palette::orange : Palette::line.brighter (0.5f));
        g.fillRect (thumb);
    }

    //==========================================================================
    // Font
    //==========================================================================

    /** Font condensato per le etichette di pannello.

        La serigrafia dei pannelli hardware usa quasi sempre un grottesco
        condensato in maiuscolo: sta in poco spazio e resta leggibile inciso.
        Chiediamo al sistema una famiglia condensata e ricadiamo sul sans di
        default se non è disponibile, invece di incorporare un font — che
        avrebbe implicazioni di licenza da verificare per ogni piattaforma. */
    static juce::Font getCondensed (float height, bool bold)
    {
        static const juce::String family = findCondensedFamily();

        auto font = juce::Font (juce::FontOptions (family, height,
                                                   bold ? juce::Font::bold : juce::Font::plain));
        font.setExtraKerningFactor (0.045f);
        return font;
    }

    static juce::Font getLcdFont (float height)
    {
        static const juce::String family = findMonoFamily();
        return juce::Font (juce::FontOptions (family, height, juce::Font::bold));
    }

private:
    static juce::String findCondensedFamily()
    {
        const juce::StringArray preferred
        {
            "Helvetica Neue Condensed Bold", "HelveticaNeue-CondensedBold",
            "Roboto Condensed", "Liberation Sans Narrow", "Arial Narrow",
            "DejaVu Sans Condensed", "Oswald", "Helvetica Neue", "Arial"
        };

        const auto available = juce::Font::findAllTypefaceNames();

        for (const auto& name : preferred)
            if (available.contains (name))
                return name;

        return juce::Font::getDefaultSansSerifFontName();
    }

    static juce::String findMonoFamily()
    {
        const juce::StringArray preferred
        {
            "Menlo", "Consolas", "DejaVu Sans Mono", "Liberation Mono", "Courier New"
        };

        const auto available = juce::Font::findAllTypefaceNames();

        for (const auto& name : preferred)
            if (available.contains (name))
                return name;

        return juce::Font::getDefaultMonospacedFontName();
    }

    SharedAssets assets;
};

} // namespace bassx
