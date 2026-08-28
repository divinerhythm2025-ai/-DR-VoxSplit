/*
  ==============================================================================
    StemPreviewPanel.h

    One result card in the editor (used twice: "Vocals" and "Instrumental").
    Shows a real waveform once a stem file is set (juce::AudioThumbnail reading
    the actual file - not simulated), with:
      - Play/Pause + a draggable seek bar + elapsed/total time
      - SOLO/MUTE (cross-wired between the two sibling panels by the editor -
        see onSoloToggled/setExternallyMuted)
      - A LEVEL knob controlling this stem's PREVIEW gain only (never the
        exported file)
      - A live L/R level meter reflecting actual post-gain/mute playback level
      - An Export button (format/rate/bit-depth aware - see StemExporter.h)
      - Drag-out-to-DAW: press-drag from the waveform area exports to a temp
        file (using the same output settings as Export) and starts a native
        OS file drag, same idea as dragging a sample out of a browser plugin

    Playback goes through the shared PreviewPlayer (see PreviewPlayer.h),
    which now mixes both stems' channels together so solo/mute are real.
  ==============================================================================
*/

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PreviewPlayer.h"
#include "StemExporter.h"
#include <functional>

class StemPreviewPanel : public juce::Component,
                          private juce::ChangeListener,
                          private juce::Timer
{
public:
    StemPreviewPanel (juce::String title,
                       juce::Colour accentColourToUse,
                       PreviewPlayer::Channel channelToUse,
                       juce::AudioFormatManager& formatManagerToUse,
                       juce::AudioThumbnailCache& thumbnailCacheToUse,
                       PreviewPlayer& playerToUse);
    ~StemPreviewPanel() override;

    /** Sets the stem file this panel previews/exports, and loads its waveform. */
    void setFile (const juce::File& file);

    /** Clears back to the empty "no result yet" state. */
    void clear();

    void paint (juce::Graphics&) override;
    void resized() override;

    //==============================================================================
    // Solo/mute cross-wiring - the editor owns the relationship between the two
    // sibling panels (this panel doesn't know its sibling exists).
    bool isSoloed() const noexcept { return soloButton.getToggleState(); }
    void setExternallyMuted (bool shouldBeMuted);
    std::function<void()> onSoloToggled;

    /** Editor supplies the live Output-panel settings; used by both Export and drag-out. */
    std::function<StemExporter::OutputSettings()> getOutputSettings;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updatePlayButtonText();
    void updateEffectiveMute();
    void exportToFile();

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    static juce::String formatTime (double seconds);

    //==============================================================================
    class SeekBar : public juce::Component
    {
    public:
        explicit SeekBar (juce::Colour fillColourToUse) : fillColour (fillColourToUse) {}

        std::function<void (double ratio01)> onSeek;

        void setProgress (double ratio01)
        {
            auto clamped = juce::jlimit (0.0, 1.0, ratio01);
            if (clamped != progress) { progress = clamped; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff202530));
            g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);

            auto fillWidth = (float) progress * bounds.getWidth();
            if (fillWidth > 0.5f)
            {
                g.setColour (fillColour);
                g.fillRoundedRectangle (bounds.withWidth (fillWidth), bounds.getHeight() * 0.5f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override { seekFromEvent (e); }
        void mouseDrag (const juce::MouseEvent& e) override { seekFromEvent (e); }

    private:
        void seekFromEvent (const juce::MouseEvent& e)
        {
            if (getWidth() <= 0)
                return;

            auto ratio = juce::jlimit (0.0, 1.0, (double) e.position.x / (double) getWidth());
            setProgress (ratio);
            if (onSeek)
                onSeek (ratio);
        }

        double progress = 0.0;
        juce::Colour fillColour;
    };

    class LevelMeter : public juce::Component
    {
    public:
        void setLevels (float left, float right)
        {
            levelL = left; levelR = right;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff090c12));
            g.fillRoundedRectangle (bounds, 6.0f);
            g.setColour (juce::Colour (0xff202630));
            g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

            auto inner = getLocalBounds().reduced (8, 6);
            auto rowHeight = inner.getHeight() / 2;
            drawRow (g, inner.removeFromTop (rowHeight).reduced (0, 1), levelL);
            drawRow (g, inner.reduced (0, 1), levelR);
        }

    private:
        static void drawRow (juce::Graphics& g, juce::Rectangle<int> row, float level)
        {
            constexpr int numSegments = 30;
            constexpr float gap = 2.0f;
            auto segmentWidth = (row.getWidth() - gap * (numSegments - 1)) / (float) numSegments;
            auto lit = juce::roundToInt (juce::jlimit (0.0f, 1.0f, level) * numSegments);

            for (int i = 0; i < numSegments; ++i)
            {
                auto x = row.getX() + i * (segmentWidth + gap);
                auto segment = juce::Rectangle<float> (x, (float) row.getY(), segmentWidth, (float) row.getHeight());

                juce::Colour c = juce::Colour (0xff1a2029); // unlit
                if (i < lit)
                {
                    if (i > 24)      c = juce::Colour (0xffd93d4d);
                    else if (i > 20) c = juce::Colour (0xffd9c735);
                    else             c = juce::Colour (0xff38c96d);
                }

                g.setColour (c);
                g.fillRoundedRectangle (segment, 1.0f);
            }
        }

        float levelL = 0.0f, levelR = 0.0f;
    };

    //==============================================================================
    juce::String titleText;
    juce::Colour accentColour;
    PreviewPlayer::Channel channel;
    juce::File file;

    juce::AudioThumbnail thumbnail;
    PreviewPlayer& player;

    juce::Label titleLabel;
    juce::Label subtitleLabel;

    juce::TextButton soloButton { "SOLO" };
    juce::TextButton muteButton { "MUTE" };
    bool externallyMuted = false;

    juce::Slider levelSlider;
    juce::Label levelCaptionLabel;
    juce::Label levelValueLabel;

    juce::TextButton playButton { "Play" };
    SeekBar seekBar;
    juce::Label currentTimeLabel;
    juce::Label totalTimeLabel;

    LevelMeter levelMeter;

    juce::TextButton exportButton;

    juce::Rectangle<int> waveformBounds;
    bool dragCandidate = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StemPreviewPanel)
};
