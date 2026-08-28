#include "StemPreviewPanel.h"

StemPreviewPanel::StemPreviewPanel (juce::String title,
                                     juce::Colour accentColourToUse,
                                     PreviewPlayer::Channel channelToUse,
                                     juce::AudioFormatManager& formatManagerToUse,
                                     juce::AudioThumbnailCache& thumbnailCacheToUse,
                                     PreviewPlayer& playerToUse)
    : titleText (std::move (title)),
      accentColour (accentColourToUse),
      channel (channelToUse),
      thumbnail (512, formatManagerToUse, thumbnailCacheToUse),
      player (playerToUse),
      seekBar (accentColourToUse)
{
    thumbnail.addChangeListener (this);

    titleLabel.setText (titleText, juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    subtitleLabel.setText ("Extracted " + titleText, juce::dontSendNotification);
    subtitleLabel.setJustificationType (juce::Justification::centredLeft);
    subtitleLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    subtitleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7f8494));
    addAndMakeVisible (subtitleLabel);

    auto styleSmallButton = [this] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0b0f15));
        b.setColour (juce::TextButton::buttonOnColourId, accentColour.withAlpha (0.32f));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffaab0ba));
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    };

    styleSmallButton (soloButton);
    soloButton.onClick = [this]
    {
        if (onSoloToggled)
            onSoloToggled();
    };
    addAndMakeVisible (soloButton);

    styleSmallButton (muteButton);
    muteButton.onClick = [this] { updateEffectiveMute(); };
    addAndMakeVisible (muteButton);

    levelSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    levelSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    levelSlider.setRange (-24.0, 6.0, 0.1);
    levelSlider.setValue (0.0, juce::dontSendNotification);
    levelSlider.setColour (juce::Slider::rotarySliderFillColourId, accentColour);
    levelSlider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff272b37));
    levelSlider.setColour (juce::Slider::thumbColourId, juce::Colours::white);
    levelSlider.onValueChange = [this]
    {
        auto db = (float) levelSlider.getValue();
        player.setGainDb (channel, db);
        levelValueLabel.setText (juce::String (db, 1) + " dB", juce::dontSendNotification);
    };
    addAndMakeVisible (levelSlider);

    levelCaptionLabel.setText ("LEVEL", juce::dontSendNotification);
    levelCaptionLabel.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    levelCaptionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7e8490));
    levelCaptionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (levelCaptionLabel);

    levelValueLabel.setText ("0.0 dB", juce::dontSendNotification);
    levelValueLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    levelValueLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc6cad2));
    levelValueLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (levelValueLabel);

    playButton.setEnabled (false);
    playButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0e1219));
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff0e1219));
    playButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    playButton.onClick = [this]
    {
        player.togglePlayPause (channel);
        updatePlayButtonText();
    };
    addAndMakeVisible (playButton);

    seekBar.onSeek = [this] (double ratio)
    {
        player.seekToSeconds (channel, ratio * player.getLengthSeconds (channel));
    };
    addAndMakeVisible (seekBar);

    currentTimeLabel.setText ("00:00", juce::dontSendNotification);
    currentTimeLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    currentTimeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9197a3));
    currentTimeLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (currentTimeLabel);

    totalTimeLabel.setText ("00:00", juce::dontSendNotification);
    totalTimeLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    totalTimeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9197a3));
    totalTimeLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (totalTimeLabel);

    addAndMakeVisible (levelMeter);

    exportButton.setButtonText (titleText.startsWithIgnoreCase ("VOCALS") ? "EXPORT VOCALS" : "EXPORT INSTRUMENTAL");
    exportButton.setEnabled (false);
    exportButton.setColour (juce::TextButton::buttonColourId, accentColour.withAlpha (0.22f));
    exportButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    exportButton.onClick = [this] { exportToFile(); };
    addAndMakeVisible (exportButton);

    startTimerHz (20);
}

StemPreviewPanel::~StemPreviewPanel()
{
    thumbnail.removeChangeListener (this);
}

void StemPreviewPanel::setFile (const juce::File& newFile)
{
    file = newFile;
    thumbnail.setSource (new juce::FileInputSource (file));
    player.setFile (channel, file);

    playButton.setEnabled (true);
    exportButton.setEnabled (true);
    updatePlayButtonText();
    totalTimeLabel.setText (formatTime (player.getLengthSeconds (channel)), juce::dontSendNotification);
    seekBar.setProgress (0.0);
    repaint();
}

void StemPreviewPanel::clear()
{
    player.clear (channel);

    file = juce::File();
    thumbnail.clear();
    playButton.setEnabled (false);
    exportButton.setEnabled (false);
    updatePlayButtonText();
    currentTimeLabel.setText ("00:00", juce::dontSendNotification);
    totalTimeLabel.setText ("00:00", juce::dontSendNotification);
    seekBar.setProgress (0.0);
    repaint();
}

void StemPreviewPanel::setExternallyMuted (bool shouldBeMuted)
{
    externallyMuted = shouldBeMuted;
    updateEffectiveMute();
}

void StemPreviewPanel::updateEffectiveMute()
{
    player.setMuted (channel, muteButton.getToggleState() || externallyMuted);
}

void StemPreviewPanel::updatePlayButtonText()
{
    playButton.setButtonText (player.isPlaying (channel) ? "Pause" : "Play");
}

void StemPreviewPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    repaint();
}

void StemPreviewPanel::timerCallback()
{
    updatePlayButtonText();

    if (file == juce::File())
    {
        levelMeter.setLevels (0.0f, 0.0f);
        return;
    }

    repaint();

    auto length = player.getLengthSeconds (channel);
    auto position = player.getPositionSeconds (channel);
    seekBar.setProgress (length > 0.0 ? position / length : 0.0);
    currentTimeLabel.setText (formatTime (position), juce::dontSendNotification);

    levelMeter.setLevels (player.getLevel (channel, 0), player.getLevel (channel, 1));
}

void StemPreviewPanel::exportToFile()
{
    auto settings = getOutputSettings ? getOutputSettings() : StemExporter::OutputSettings();
    auto extension = StemExporter::getFileExtension (settings.format);

    auto chooser = std::make_shared<juce::FileChooser> (
        "Export " + titleText + " as...",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory)
            .getChildFile (file.getFileNameWithoutExtension() + extension),
        "*" + extension);

    auto chooserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting;
    chooser->launchAsync (chooserFlags, [this, chooser, settings] (const juce::FileChooser& fc)
    {
        auto target = fc.getResult();
        if (target == juce::File())
            return;

        auto result = StemExporter::exportStem (file, target, settings);
        if (result.isNotEmpty())
        {
            auto icon = result.startsWith ("Warning:") ? juce::MessageBoxIconType::InfoIcon
                                                         : juce::MessageBoxIconType::WarningIcon;
            juce::NativeMessageBox::showMessageBoxAsync (icon,
                result.startsWith ("Warning:") ? "Export Complete" : "Export Failed", result);
        }
    });
}

void StemPreviewPanel::mouseDown (const juce::MouseEvent&)
{
    dragCandidate = (file != juce::File());
}

void StemPreviewPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragCandidate || file == juce::File())
        return;

    if (e.getDistanceFromDragStart() < 8)
        return;

    dragCandidate = false; // only start one external drag per mouse press

    auto settings = getOutputSettings ? getOutputSettings() : StemExporter::OutputSettings();
    auto tempFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("DR-VoxSplit")
                       .getChildFile ("drag-out")
                       .getChildFile (titleText + StemExporter::getFileExtension (settings.format));

    auto result = StemExporter::exportStem (file, tempFile, settings);
    if (result.isNotEmpty() && ! result.startsWith ("Warning:"))
        return; // hard failure - don't start a drag with no valid file

    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ tempFile.getFullPathName() }, false, this);
}

juce::String StemPreviewPanel::formatTime (double seconds)
{
    if (seconds < 0.0 || std::isnan (seconds))
        seconds = 0.0;

    auto totalSeconds = (int) std::floor (seconds);
    return juce::String::formatted ("%02d:%02d", totalSeconds / 60, totalSeconds % 60);
}

void StemPreviewPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff0d1018));
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (juce::Colour (0xff202431));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 10.0f, 1.0f);

    auto iconArea = juce::Rectangle<float> (12.0f, 12.0f, 36.0f, 36.0f);
    g.setColour (accentColour.withAlpha (0.15f));
    g.fillRoundedRectangle (iconArea, 8.0f);

    if (file == juce::File())
    {
        g.setColour (juce::Colour (0xff7f8494));
        g.drawFittedText ("No result yet", waveformBounds, juce::Justification::centred, 1);
        return;
    }

    g.setColour (juce::Colour (0xff05070c));
    g.fillRoundedRectangle (waveformBounds.toFloat(), 6.0f);

    g.setColour (accentColour.withAlpha (0.95f));
    thumbnail.drawChannels (g, waveformBounds.reduced (4), 0.0, thumbnail.getTotalLength(), 1.0f);

    if (player.isPlaying (channel) && thumbnail.getTotalLength() > 0.0)
    {
        auto length = player.getLengthSeconds (channel);
        auto ratio = length > 0.0 ? player.getPositionSeconds (channel) / length : 0.0;
        auto x = waveformBounds.getX() + (float) ratio * waveformBounds.getWidth();
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawVerticalLine ((int) x, (float) waveformBounds.getY(), (float) waveformBounds.getBottom());
    }
}

void StemPreviewPanel::resized()
{
    auto bounds = getLocalBounds().reduced (10);

    // Header row: icon (painted) + title/subtitle, solo/mute, level knob.
    auto header = bounds.removeFromTop (48);
    auto textArea = header.removeFromLeft (juce::jmax (60, header.getWidth() - 190));
    textArea.removeFromLeft (42); // icon
    titleLabel.setBounds (textArea.removeFromTop (22));
    subtitleLabel.setBounds (textArea.removeFromTop (16));

    auto knobArea = header.removeFromRight (44);
    levelSlider.setBounds (knobArea.removeFromTop (34).withSizeKeepingCentre (34, 34));
    levelCaptionLabel.setBounds (knobArea.removeFromTop (11));
    levelValueLabel.setBounds (knobArea);

    header.removeFromRight (6);
    auto buttonsArea = header;
    muteButton.setBounds (buttonsArea.removeFromRight (52).reduced (0, 9));
    buttonsArea.removeFromRight (4);
    soloButton.setBounds (buttonsArea.removeFromRight (52).reduced (0, 9));

    bounds.removeFromTop (8);

    // Bottom-up: export button, meter, player row - waveform gets whatever's left.
    auto exportArea = bounds.removeFromBottom (36);
    exportButton.setBounds (exportArea);
    bounds.removeFromBottom (8);

    auto meterArea = bounds.removeFromBottom (40);
    levelMeter.setBounds (meterArea);
    bounds.removeFromBottom (6);

    auto playerRow = bounds.removeFromBottom (30);
    playButton.setBounds (playerRow.removeFromLeft (56));
    playerRow.removeFromLeft (6);
    totalTimeLabel.setBounds (playerRow.removeFromRight (44));
    currentTimeLabel.setBounds (playerRow.removeFromLeft (44));
    playerRow.reduce (6, 0);
    seekBar.setBounds (playerRow.withSizeKeepingCentre (playerRow.getWidth(), 5));
    bounds.removeFromBottom (8);

    waveformBounds = bounds;
}
