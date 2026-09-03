/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::String backendLabel (SeparationEngine::Backend backend)
    {
        switch (backend)
        {
            case SeparationEngine::Backend::cuda: return " (GPU-accelerated, CUDA)";
            case SeparationEngine::Backend::gpu: return " (GPU-accelerated)";
            case SeparationEngine::Backend::cpu: return " (CPU - no compatible GPU found)";
            case SeparationEngine::Backend::unknown:
            default: return {};
        }
    }

    juce::String formatDuration (double seconds)
    {
        auto total = (int) std::floor (seconds);
        return juce::String::formatted ("%02d:%02d", total / 60, total % 60);
    }
}

//==============================================================================
DRVoxSplitAudioProcessorEditor::DRVoxSplitAudioProcessorEditor (DRVoxSplitAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    formatManager.registerBasicFormats();

    //==========================================================
    // Header
    //==========================================================
    headerLogoLabel.setText ("DIVINE STEMS", juce::dontSendNotification);
    headerLogoLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    headerLogoLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (headerLogoLabel);

    headerSubtitleLabel.setText ("AI SONG SPLITTER", juce::dontSendNotification);
    headerSubtitleLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    headerSubtitleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff858a94));
    addAndMakeVisible (headerSubtitleLabel);

    licenseStatusButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff11151e));
    licenseStatusButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc5c9d4));
    licenseStatusButton.onClick = [this] { LicenseActivationDialog::show (this, audioProcessor.getLicenseManager()); };
    addAndMakeVisible (licenseStatusButton);

    //==========================================================
    // Input panel
    //==========================================================
    inputPanelTitle.setText ("INPUT", juce::dontSendNotification);
    inputPanelTitle.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    inputPanelTitle.setColour (juce::Label::textColourId, juce::Colour (0xffd8dade));
    addAndMakeVisible (inputPanelTitle);

    fileNameLabel.setText ("No file loaded", juce::dontSendNotification);
    fileNameLabel.setFont (juce::Font (juce::FontOptions (15.0f)));
    fileNameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (fileNameLabel);

    fileInfoLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    fileInfoLabel.setColour (juce::Label::textColourId, juce::Colour (0xff858a99));
    addAndMakeVisible (fileInfoLabel);

    browseButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0c1016));
    browseButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe2e4e8));
    browseButton.onClick = [this] { browseButtonClicked(); };
    addAndMakeVisible (browseButton);

    inputThumbnail.addChangeListener (this); // repaint once the async-loaded waveform is ready

    //==========================================================
    // Separation panel
    //==========================================================
    separationPanelTitle.setText ("SEPARATION", juce::dontSendNotification);
    separationPanelTitle.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    separationPanelTitle.setColour (juce::Label::textColourId, juce::Colour (0xffd8dade));
    addAndMakeVisible (separationPanelTitle);

    qualityLabel.setText ("QUALITY", juce::dontSendNotification);
    styleLabel (qualityLabel);
    addAndMakeVisible (qualityLabel);

    auto styleQualityButton = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff0b0f15));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff484c54));
        b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff9ba1ac));
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    };
    styleQualityButton (fastQualityButton);
    styleQualityButton (highQualityButton);
    fastQualityButton.setToggleState (true, juce::dontSendNotification);
    fastQualityButton.onClick = [this] { setQualityChoice (SeparationEngine::Quality::fast); };
    highQualityButton.onClick = [this] { setQualityChoice (SeparationEngine::Quality::highQuality); };
    if (! audioProcessor.getSeparationEngine().isHighQualityModelAvailable())
    {
        highQualityButton.setEnabled (false);
        highQualityButton.setTooltip ("htdemucs_ft.safetensors wasn't found in this install - High Quality unavailable.");
    }
    addAndMakeVisible (fastQualityButton);
    addAndMakeVisible (highQualityButton);

    deviceLabel.setText ("PROCESSING DEVICE", juce::dontSendNotification);
    styleLabel (deviceLabel);
    addAndMakeVisible (deviceLabel);

    deviceBox.addItem ("Auto (Best Available)", 1);
    deviceBox.addItem ("CPU", 2);
    deviceBox.addItem ("GPU", 3);
    deviceBox.setSelectedId (1, juce::dontSendNotification);
    deviceBox.onChange = [this] { deviceBoxChanged(); };
    styleComboBox (deviceBox);
    addAndMakeVisible (deviceBox);

    startButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff494c53));
    startButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    startButton.setEnabled (false);
    startButton.onClick = [this] { startButtonClicked(); };
    addAndMakeVisible (startButton);

    cancelButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff11151e));
    cancelButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc5c9d4));
    cancelButton.onClick = [this] { audioProcessor.getSeparationEngine().cancel(); };
    addChildComponent (cancelButton);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff181c24));
    progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xffd8dade));
    addChildComponent (progressBar);

    statusLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff777e8b));
    addAndMakeVisible (statusLabel);

    //==========================================================
    // Stem cards
    //==========================================================
    addAndMakeVisible (vocalsPanel);
    addAndMakeVisible (instrumentalPanel);

    vocalsPanel.onSoloToggled = [this] { instrumentalPanel.setExternallyMuted (vocalsPanel.isSoloed()); };
    instrumentalPanel.onSoloToggled = [this] { vocalsPanel.setExternallyMuted (instrumentalPanel.isSoloed()); };

    vocalsPanel.getOutputSettings = [this] { return getCurrentOutputSettings(); };
    instrumentalPanel.getOutputSettings = [this] { return getCurrentOutputSettings(); };

    //==========================================================
    // Output panel
    //==========================================================
    outputPanelTitle.setText ("OUTPUT", juce::dontSendNotification);
    outputPanelTitle.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
    outputPanelTitle.setColour (juce::Label::textColourId, juce::Colour (0xffd8dade));
    addAndMakeVisible (outputPanelTitle);

    formatLabel.setText ("FORMAT", juce::dontSendNotification);
    styleLabel (formatLabel);
    addAndMakeVisible (formatLabel);

    formatBox.addItem ("WAV", 1);
    formatBox.addItem ("FLAC", 2);
    formatBox.addItem ("MP3", 3);
    formatBox.setSelectedId (1, juce::dontSendNotification);
    styleComboBox (formatBox);
    addAndMakeVisible (formatBox);

    sampleRateLabel.setText ("SAMPLE RATE", juce::dontSendNotification);
    styleLabel (sampleRateLabel);
    addAndMakeVisible (sampleRateLabel);

    sampleRateBox.addItem ("Same as Input", 1);
    sampleRateBox.addItem ("48 kHz", 2);
    sampleRateBox.addItem ("96 kHz", 3);
    sampleRateBox.setSelectedId (1, juce::dontSendNotification);
    styleComboBox (sampleRateBox);
    addAndMakeVisible (sampleRateBox);

    bitDepthLabel.setText ("BIT DEPTH", juce::dontSendNotification);
    styleLabel (bitDepthLabel);
    addAndMakeVisible (bitDepthLabel);

    bitDepthBox.addItem ("24-bit", 1);
    bitDepthBox.addItem ("32-bit Float", 2);
    bitDepthBox.setSelectedId (1, juce::dontSendNotification);
    styleComboBox (bitDepthBox);
    addAndMakeVisible (bitDepthBox);

    exportAllButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff494c53));
    exportAllButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    exportAllButton.onClick = [this] { exportAllClicked(); };
    addAndMakeVisible (exportAllButton);

    //==========================================================
    if (! audioProcessor.getSeparationEngine().isAvailable())
        statusLabel.setText ("Separation engine unavailable - demucs.exe or the bundled model is missing from this install.",
                              juce::dontSendNotification);
    else if (! audioProcessor.getSeparationEngine().isGpuBuildPresent())
        statusLabel.setText ("Running on CPU - processing will take longer than with a compatible GPU.",
                              juce::dontSendNotification);
    else
        statusLabel.setText ("Ready.", juce::dontSendNotification);

    audioProcessor.setActiveUiListener (this);
    syncFromProcessorState();

    audioProcessor.getLicenseManager().addListener (this);
    updateLicenseStatusUi();
    audioProcessor.getLicenseManager().revalidateInBackground();

    setResizable (true, true);
    setResizeLimits (900, 700, 1700, 1150);
    setSize (1500, 920);
}

DRVoxSplitAudioProcessorEditor::~DRVoxSplitAudioProcessorEditor()
{
    audioProcessor.getLicenseManager().removeListener (this);
    inputThumbnail.removeChangeListener (this);
    audioProcessor.setActiveUiListener (nullptr);
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::updateLicenseStatusUi()
{
    const bool licensed = audioProcessor.getLicenseManager().isCurrentlyLicensed();
    licenseStatusButton.setButtonText (licensed ? "Licensed" : "Not Activated");
    licenseStatusButton.setColour (juce::TextButton::textColourOffId,
                                    licensed ? juce::Colour (0xff7fd88f) : juce::Colour (0xffc5c9d4));
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::syncFromProcessorState()
{
    const auto& state = audioProcessor.getUiState();

    switch (state.phase)
    {
        case DRVoxSplitAudioProcessor::UiState::Phase::processing:
            phase = UiPhase::processing;
            progressBarValue = (double) state.progress;
            statusLabel.setText (state.statusMessage, juce::dontSendNotification);
            break;

        case DRVoxSplitAudioProcessor::UiState::Phase::done:
            phase = UiPhase::done;
            vocalsPanel.setFile (state.result.vocalsFile);
            instrumentalPanel.setFile (state.result.instrumentalFile);
            statusLabel.setText ("Separation complete (" + juce::String (state.result.durationSeconds, 1) + "s)"
                                  + backendLabel (state.result.backendUsed) + ".",
                                  juce::dontSendNotification);
            break;

        case DRVoxSplitAudioProcessor::UiState::Phase::error:
        case DRVoxSplitAudioProcessor::UiState::Phase::idle:
        default:
            phase = UiPhase::idle;
            break;
    }
}

void DRVoxSplitAudioProcessorEditor::setIdleUi()
{
    phase = UiPhase::idle;
    progressBarValue = 0.0;
    resized();
    repaint();
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::loadInputFileInfo (const juce::File& file, double durationSeconds)
{
    currentInputFile = file;
    inputThumbnail.setSource (new juce::FileInputSource (file));

    fileNameLabel.setText (file.getFileName(), juce::dontSendNotification);

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    juce::String info = formatDuration (durationSeconds);
    if (reader != nullptr)
    {
        info << "   |   " << juce::String (reader->sampleRate / 1000.0, 1) << " kHz"
             << "   |   " << (reader->numChannels >= 2 ? "Stereo" : "Mono");
    }
    fileInfoLabel.setText (info, juce::dontSendNotification);
    repaint();
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::browseButtonClicked()
{
    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Select an audio file to split...",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        "*.wav;*.wave;*.mp3;*.flac;*.aiff;*.aif;*.m4a;*.aac");

    activeFileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file != juce::File())
                loadSelectedFile (file);
        });
}

void DRVoxSplitAudioProcessorEditor::loadSelectedFile (const juce::File& file)
{
    if (phase == UiPhase::processing)
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            "Already Processing", "A separation is already in progress. Cancel it first if you want to load another file.");
        return;
    }

    double durationSeconds = 0.0;
    auto error = audioProcessor.getSeparationEngine().validateInputFile (file, durationSeconds);
    if (error.isNotEmpty())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Can't Load This File", error);
        return;
    }

    loadInputFileInfo (file, durationSeconds);
    startButton.setEnabled (true);
    statusLabel.setText ("Ready - press Start Splitting.", juce::dontSendNotification);
}

void DRVoxSplitAudioProcessorEditor::startButtonClicked()
{
    if (! audioProcessor.getLicenseManager().isCurrentlyLicensed())
    {
        LicenseActivationDialog::show (this, audioProcessor.getLicenseManager());
        return;
    }

    if (! audioProcessor.getSeparationEngine().isAvailable())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Separation Engine Unavailable",
            "demucs.exe or the bundled model file is missing from this plugin install, so separation can't run.");
        return;
    }

    if (currentInputFile == juce::File())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            "No File Loaded", "Browse for or drop an audio file first.");
        return;
    }

    if (phase == UiPhase::processing)
        return;

    auto file = currentInputFile;
    double durationSeconds = 0.0;
    auto error = audioProcessor.getSeparationEngine().validateInputFile (file, durationSeconds);
    if (error.isNotEmpty())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Can't Process This File", error);
        return;
    }

    auto& engine = audioProcessor.getSeparationEngine();
    if (durationSeconds > engine.effectiveWarnDurationSeconds())
    {
        auto minutes = durationSeconds / 60.0;
        juce::String speedNote = engine.isGpuBuildPresent()
            ? "Separation uses GPU acceleration when available, but is still significantly slower than realtime."
            : "No compatible GPU acceleration was found, so separation runs on CPU and is significantly slower than realtime.";
        auto options = juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::QuestionIcon)
            .withTitle ("Long File")
            .withMessage ("This file is " + juce::String (minutes, 1) + " minutes long. " + speedNote + " This could take a while. Continue?")
            .withButton ("Continue").withButton ("Cancel");

        juce::AlertWindow::showAsync (options, [this, file] (int result)
        {
            if (result == 1) // "Continue" is button index 1 (buttons are numbered in reverse-add order by AlertWindow)
                beginProcessingConfirmed (file);
        });
        return;
    }

    beginProcessingConfirmed (file);
}

void DRVoxSplitAudioProcessorEditor::beginProcessingConfirmed (const juce::File& file)
{
    vocalsPanel.clear();
    instrumentalPanel.clear();

    if (! audioProcessor.getSeparationEngine().startSeparation (file))
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
            "Couldn't Start", "A separation is already running.");
        return;
    }

    phase = UiPhase::processing;
    progressBarValue = -1.0; // indeterminate until real progress arrives
    statusLabel.setText ("Starting...", juce::dontSendNotification);
    startButton.setEnabled (false);
    resized();
    repaint();
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::separationProgress (float progress01, const juce::String& stageMessage)
{
    phase = UiPhase::processing;
    progressBarValue = progress01 < 0.0f ? -1.0 : (double) progress01;
    statusLabel.setText (stageMessage, juce::dontSendNotification);
    resized();
}

void DRVoxSplitAudioProcessorEditor::separationComplete (const SeparationEngine::Result& result)
{
    phase = UiPhase::done;
    vocalsPanel.setFile (result.vocalsFile);
    instrumentalPanel.setFile (result.instrumentalFile);
    statusLabel.setText ("Separation complete (" + juce::String (result.durationSeconds, 1) + "s)"
                          + backendLabel (result.backendUsed) + ".", juce::dontSendNotification);
    startButton.setEnabled (currentInputFile != juce::File());
    resized();
    repaint();
}

void DRVoxSplitAudioProcessorEditor::separationFailed (const juce::String& errorMessage)
{
    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Separation Failed", errorMessage);
    setIdleUi();
    startButton.setEnabled (currentInputFile != juce::File());
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::setQualityChoice (SeparationEngine::Quality q)
{
    audioProcessor.getSeparationEngine().setQuality (q);
    fastQualityButton.setToggleState (q == SeparationEngine::Quality::fast, juce::dontSendNotification);
    highQualityButton.setToggleState (q == SeparationEngine::Quality::highQuality, juce::dontSendNotification);
}

void DRVoxSplitAudioProcessorEditor::deviceBoxChanged()
{
    using Pref = SeparationEngine::BackendPreference;
    auto& engine = audioProcessor.getSeparationEngine();

    switch (deviceBox.getSelectedId())
    {
        case 2:  engine.setBackendPreference (Pref::forceCpu); break;
        case 3:  engine.setBackendPreference (Pref::forceGpu); break;
        case 1:
        default: engine.setBackendPreference (Pref::automatic); break;
    }
}

StemExporter::OutputSettings DRVoxSplitAudioProcessorEditor::getCurrentOutputSettings() const
{
    StemExporter::OutputSettings settings;

    switch (formatBox.getSelectedId())
    {
        case 2:  settings.format = StemExporter::Format::flac; break;
        case 3:  settings.format = StemExporter::Format::mp3; break;
        case 1:
        default: settings.format = StemExporter::Format::wav; break;
    }

    switch (sampleRateBox.getSelectedId())
    {
        case 2:  settings.sampleRate = StemExporter::SampleRateChoice::sr48k; break;
        case 3:  settings.sampleRate = StemExporter::SampleRateChoice::sr96k; break;
        case 1:
        default: settings.sampleRate = StemExporter::SampleRateChoice::sameAsInput; break;
    }

    settings.bitDepth = bitDepthBox.getSelectedId() == 2 ? StemExporter::BitDepth::bit32Float
                                                          : StemExporter::BitDepth::bit24;
    return settings;
}

void DRVoxSplitAudioProcessorEditor::exportAllClicked()
{
    const auto& state = audioProcessor.getUiState();
    if (state.result.vocalsFile == juce::File() || state.result.instrumentalFile == juce::File())
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
            "Nothing To Export", "Run a separation first - there's no result yet.");
        return;
    }

    exportAllChooser = std::make_unique<juce::FileChooser> ("Choose a folder to export both stems into...",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory));

    exportAllChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this] (const juce::FileChooser& fc)
        {
            auto folder = fc.getResult();
            if (folder == juce::File())
                return;

            auto settings = getCurrentOutputSettings();
            auto extension = StemExporter::getFileExtension (settings.format);
            auto baseName = currentInputFile != juce::File() ? currentInputFile.getFileNameWithoutExtension()
                                                               : juce::String ("DR-VoxSplit");

            auto vocalsTarget = folder.getChildFile (baseName + " - Vocals" + extension);
            auto instrumentalTarget = folder.getChildFile (baseName + " - Instrumental" + extension);

            const auto& state = audioProcessor.getUiState();
            auto r1 = StemExporter::exportStem (state.result.vocalsFile, vocalsTarget, settings);
            auto r2 = StemExporter::exportStem (state.result.instrumentalFile, instrumentalTarget, settings);

            juce::String message;
            if (r1.isNotEmpty()) message << "Vocals: " << r1 << "\n";
            if (r2.isNotEmpty()) message << "Instrumental: " << r2 << "\n";

            if (message.isNotEmpty())
                juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export All", message.trim());
        });
}

//==============================================================================
bool DRVoxSplitAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (phase == UiPhase::processing || files.size() != 1)
        return false;

    return SeparationEngine::isSupportedExtension (juce::File (files[0]).getFileExtension());
}

void DRVoxSplitAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    isDragHovering = true;
    repaint();
}

void DRVoxSplitAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    isDragHovering = false;
    repaint();
}

void DRVoxSplitAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    isDragHovering = false;
    repaint();

    if (files.size() == 1)
        loadSelectedFile (juce::File (files[0]));
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff080a0e));

    auto bounds = getLocalBounds().toFloat();

    // Header bar
    g.setColour (juce::Colour (0xff0b0e16));
    g.fillRect (0.0f, 0.0f, bounds.getWidth(), 62.0f);
    g.setColour (juce::Colour (0xff242932));
    g.drawHorizontalLine (62, 0.0f, bounds.getWidth());

    // Footer bar
    g.setColour (juce::Colour (0xff0b0e16));
    g.fillRect (0.0f, bounds.getHeight() - 34.0f, bounds.getWidth(), 34.0f);
    g.drawHorizontalLine ((int) (bounds.getHeight() - 34.0f), 0.0f, bounds.getWidth());

    g.setColour (juce::Colour (0xff676d79));
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText ("DIVINE AUDIO LABS", 20, (int) bounds.getHeight() - 25, 200, 18, juce::Justification::left);

    juce::String phaseText = "Ready to split your track into Vocals and Instrumental.";
    if (phase == UiPhase::processing) phaseText = "Separating vocals and instrumental...";
    else if (phase == UiPhase::done)  phaseText = "Separation complete.";
    g.drawText (phaseText, 0, (int) bounds.getHeight() - 25, (int) bounds.getWidth(), 18, juce::Justification::centred);

    g.drawText ("v1.0.0", (int) bounds.getWidth() - 90, (int) bounds.getHeight() - 25, 70, 18, juce::Justification::right);

    // Panel backgrounds
    auto drawPanel = [&g] (juce::Rectangle<int> panelBounds)
    {
        auto r = panelBounds.toFloat();
        g.setColour (juce::Colour (0xff0d1016));
        g.fillRoundedRectangle (r, 10.0f);
        g.setColour (juce::Colour (0xff242932));
        g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);
    };

    drawPanel (inputPanelBounds);
    drawPanel (separationPanelBounds);
    drawPanel (outputPanelBounds);

    // Input waveform / drop target
    if (! inputWaveformBounds.isEmpty())
    {
        auto zone = inputWaveformBounds.toFloat();

        g.setColour (juce::Colour (0xff05070c));
        g.fillRoundedRectangle (zone, 8.0f);

        if (isDragHovering)
        {
            g.setColour (juce::Colour (0xffd8dade).withAlpha (0.12f));
            g.fillRoundedRectangle (zone, 8.0f);
        }

        g.setColour (isDragHovering ? juce::Colour (0xffd8dade) : juce::Colour (0xff2c3140));
        juce::Path dashedBorder;
        dashedBorder.addRoundedRectangle (zone.reduced (2.0f), 8.0f);
        juce::PathStrokeType stroke (2.0f);
        float dashLengths[] = { 8.0f, 6.0f };
        juce::Path dashedPath;
        stroke.createDashedStroke (dashedPath, dashedBorder, dashLengths, 2);
        g.fillPath (dashedPath);

        if (currentInputFile != juce::File() && inputThumbnail.getTotalLength() > 0.0)
        {
            g.setColour (juce::Colour (0xffd8dade).withAlpha (0.9f));
            inputThumbnail.drawChannels (g, inputWaveformBounds.reduced (10), 0.0, inputThumbnail.getTotalLength(), 1.0f);
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (16.0f)));
            g.drawFittedText ("Drop an audio file here", zone.reduced (16.0f).withTrimmedBottom (18.0f).toNearestInt(),
                               juce::Justification::centredBottom, 2);

            g.setFont (juce::Font (juce::FontOptions (12.0f)));
            g.setColour (juce::Colour (0xff858a99));
            auto subArea = zone.toNearestInt().removeFromBottom (26);
            g.drawFittedText ("WAV  /  MP3  /  FLAC  /  AIFF  /  M4A", subArea, juce::Justification::centred, 1);
        }
    }
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();

    auto header = full.removeFromTop (62).reduced (18, 0);
    headerLogoLabel.setBounds (header.getX(), 14, 300, 28);
    headerSubtitleLabel.setBounds (header.getX(), 42, 220, 16);
    licenseStatusButton.setBounds (header.getRight() - 170, 18, 170, 26);

    full.removeFromBottom (34); // footer, painted only

    auto content = full.reduced (16);

    constexpr int rightColWidth = 340;
    constexpr int gap = 14;
    constexpr int row1Height = 300;

    auto row1 = content.removeFromTop (row1Height);
    content.removeFromTop (gap);
    auto row2 = content;

    inputPanelBounds = row1.removeFromLeft (row1.getWidth() - rightColWidth - gap);
    row1.removeFromLeft (gap);
    separationPanelBounds = row1;

    auto stemsBounds = row2.removeFromLeft (row2.getWidth() - rightColWidth - gap);
    row2.removeFromLeft (gap);
    outputPanelBounds = row2;

    //==========================================================
    // Input panel contents
    //==========================================================
    {
        auto b = inputPanelBounds.reduced (16);
        inputPanelTitle.setBounds (b.removeFromTop (22));
        b.removeFromTop (6);

        auto fileRow = b.removeFromTop (48);
        browseButton.setBounds (fileRow.removeFromRight (110));
        fileRow.removeFromRight (10);
        fileNameLabel.setBounds (fileRow.removeFromTop (24));
        fileInfoLabel.setBounds (fileRow);

        b.removeFromTop (10);
        inputWaveformBounds = b;
    }

    //==========================================================
    // Separation panel contents
    //==========================================================
    {
        auto b = separationPanelBounds.reduced (16);
        separationPanelTitle.setBounds (b.removeFromTop (22));

        qualityLabel.setBounds (b.removeFromTop (28).removeFromBottom (16));
        auto qualityRow = b.removeFromTop (36);
        fastQualityButton.setBounds (qualityRow.removeFromLeft (qualityRow.getWidth() / 2 - 3));
        qualityRow.removeFromLeft (6);
        highQualityButton.setBounds (qualityRow);

        b.removeFromTop (12);
        deviceLabel.setBounds (b.removeFromTop (28).removeFromBottom (16));
        deviceBox.setBounds (b.removeFromTop (38));

        b.removeFromTop (14);

        bool showProcessingControls = (phase == UiPhase::processing);
        progressBar.setVisible (showProcessingControls);
        cancelButton.setVisible (showProcessingControls);
        startButton.setVisible (! showProcessingControls);

        if (showProcessingControls)
        {
            auto row = b.removeFromTop (40);
            cancelButton.setBounds (row.removeFromRight (90));
            row.removeFromRight (8);
            progressBar.setBounds (row);
        }
        else
        {
            startButton.setBounds (b.removeFromTop (40));
        }

        b.removeFromTop (10);
        statusLabel.setBounds (b.removeFromTop (18));
    }

    //==========================================================
    // Stem cards
    //==========================================================
    auto vocalsBounds = stemsBounds.removeFromLeft ((stemsBounds.getWidth() - gap) / 2);
    stemsBounds.removeFromLeft (gap);
    vocalsPanel.setBounds (vocalsBounds);
    instrumentalPanel.setBounds (stemsBounds);

    //==========================================================
    // Output panel contents
    //==========================================================
    {
        auto b = outputPanelBounds.reduced (16);
        outputPanelTitle.setBounds (b.removeFromTop (22));
        b.removeFromTop (14);

        formatLabel.setBounds (b.removeFromTop (16));
        formatBox.setBounds (b.removeFromTop (36));
        b.removeFromTop (12);

        sampleRateLabel.setBounds (b.removeFromTop (16));
        sampleRateBox.setBounds (b.removeFromTop (36));
        b.removeFromTop (12);

        bitDepthLabel.setBounds (b.removeFromTop (16));
        bitDepthBox.setBounds (b.removeFromTop (36));
        b.removeFromTop (18);

        exportAllButton.setBounds (b.removeFromTop (42));
    }
}

//==============================================================================
void DRVoxSplitAudioProcessorEditor::styleLabel (juce::Label& label)
{
    label.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
    label.setColour (juce::Label::textColourId, juce::Colour (0xff858a99));
}

void DRVoxSplitAudioProcessorEditor::styleComboBox (juce::ComboBox& box)
{
    box.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff0a0d13));
    box.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff292e38));
    box.setColour (juce::ComboBox::textColourId, juce::Colour (0xffd9dce2));
    box.setColour (juce::ComboBox::arrowColourId, juce::Colour (0xffd8dade));
}
