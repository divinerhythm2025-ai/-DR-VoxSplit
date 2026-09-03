/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin editor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "Separation/StemPreviewPanel.h"
#include "Separation/PreviewPlayer.h"
#include "Separation/StemExporter.h"
#include "Licensing/LicenseActivationDialog.h"

//==============================================================================
class DRVoxSplitAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                         public juce::FileDragAndDropTarget,
                                         private juce::ChangeListener,
                                         private SeparationEngine::Listener,
                                         private LicenseManager::Listener
{
public:
    explicit DRVoxSplitAudioProcessorEditor (DRVoxSplitAudioProcessor&);
    ~DRVoxSplitAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    //==============================================================================
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    //==============================================================================
    // SeparationEngine::Listener - called back (already on the message thread,
    // see PluginProcessor::separation*() / SeparationEngine::notify*()) either
    // live from the engine, or forwarded through the processor if a run was
    // already in progress when this editor opened.
    void separationProgress (float progress01, const juce::String& stageMessage) override;
    void separationComplete (const SeparationEngine::Result& result) override;
    void separationFailed (const juce::String& errorMessage) override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    // LicenseManager::Listener - fires when activate()/deactivate()/
    // revalidateInBackground() actually change licensing state; independent
    // of whether a LicenseActivationDialog is currently open (see
    // LicenseActivationDialog.h for why the dialog itself doesn't report
    // back through a callback instead).
    void licenseStateChanged() override { updateLicenseStatusUi(); }

    //==============================================================================
    // Loading a file (browse or drop) only validates it and shows its info/waveform
    // - it does NOT start separation. That's startButtonClicked()'s job, matching
    // the reference design's separate "load, then press START SPLITTING" flow.
    void loadSelectedFile (const juce::File& file);
    void startButtonClicked();
    void beginProcessingConfirmed (const juce::File& file);
    void browseButtonClicked();
    void syncFromProcessorState();
    void setIdleUi();
    void updateLicenseStatusUi();
    void loadInputFileInfo (const juce::File& file, double durationSeconds);

    void setQualityChoice (SeparationEngine::Quality q);
    void deviceBoxChanged();
    StemExporter::OutputSettings getCurrentOutputSettings() const;
    void exportAllClicked();

    void styleLabel (juce::Label&);
    void styleComboBox (juce::ComboBox&);

    DRVoxSplitAudioProcessor& audioProcessor;

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 4 };
    PreviewPlayer previewPlayer { formatManager };

    enum class UiPhase { idle, processing, done, error };
    UiPhase phase = UiPhase::idle;

    bool isDragHovering = false;

    //==============================================================================
    // Header
    juce::Label headerLogoLabel;
    juce::Label headerSubtitleLabel;
    juce::TextButton licenseStatusButton; // shows "Not Activated" / "Licensed" - click opens LicenseActivationDialog

    //==============================================================================
    // Input panel
    juce::Rectangle<int> inputPanelBounds, inputWaveformBounds;
    juce::Label inputPanelTitle;
    juce::Label fileNameLabel;
    juce::Label fileInfoLabel;
    juce::TextButton browseButton { "BROWSE" };
    juce::AudioThumbnail inputThumbnail { 512, formatManager, thumbnailCache };
    juce::File currentInputFile;

    //==============================================================================
    // Separation panel
    juce::Rectangle<int> separationPanelBounds;
    juce::Label separationPanelTitle;
    juce::Label qualityLabel;
    juce::TextButton fastQualityButton { "FAST" };
    juce::TextButton highQualityButton { "HIGH QUALITY" };
    juce::Label deviceLabel;
    juce::ComboBox deviceBox;
    juce::TextButton startButton { "START SPLITTING" };
    juce::TextButton cancelButton { "Cancel" };
    double progressBarValue = 0.0;
    juce::ProgressBar progressBar { progressBarValue };
    juce::Label statusLabel;

    //==============================================================================
    // Stem cards
    StemPreviewPanel vocalsPanel { "Vocals", juce::Colour (0xffd8dade), PreviewPlayer::Channel::vocals,
                                    formatManager, thumbnailCache, previewPlayer };
    StemPreviewPanel instrumentalPanel { "Instrumental", juce::Colour (0xff35a8ff), PreviewPlayer::Channel::instrumental,
                                          formatManager, thumbnailCache, previewPlayer };

    //==============================================================================
    // Output panel
    juce::Rectangle<int> outputPanelBounds;
    juce::Label outputPanelTitle;
    juce::Label formatLabel, sampleRateLabel, bitDepthLabel;
    juce::ComboBox formatBox, sampleRateBox, bitDepthBox;
    juce::TextButton exportAllButton { "EXPORT ALL" };

    std::unique_ptr<juce::FileChooser> activeFileChooser;
    std::unique_ptr<juce::FileChooser> exportAllChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DRVoxSplitAudioProcessorEditor)
};
