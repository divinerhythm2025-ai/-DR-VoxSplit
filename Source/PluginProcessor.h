/*
  ==============================================================================

    DR-VoxSplit - drag-and-drop vocal/instrumental stem separator.

    This is a file-processing utility, not a real-time effect: processBlock()
    below is a deliberate passthrough (see its definition for why that's the
    right shape for this plugin). All the real work happens through the
    editor's drag-and-drop UI, driving SeparationEngine on a background
    thread. See Source/Separation/SeparationEngine.h for the separation
    pipeline itself.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "Separation/SeparationEngine.h"

//==============================================================================
class DRVoxSplitAudioProcessor  : public juce::AudioProcessor,
                                   private SeparationEngine::Listener
{
public:
    //==============================================================================
    DRVoxSplitAudioProcessor();
    ~DRVoxSplitAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    SeparationEngine& getSeparationEngine() noexcept { return separationEngine; }

    /** Snapshot of separation state, persisted here (not in the editor) so
        results/progress survive the editor being closed and reopened. */
    struct UiState
    {
        enum class Phase { idle, processing, done, error };
        Phase phase = Phase::idle;
        float progress = 0.0f;
        juce::String statusMessage;
        juce::String errorMessage;
        SeparationEngine::Result result;
    };
    const UiState& getUiState() const noexcept { return uiState; }
    void resetUiState() { uiState = UiState(); }

    /** The currently-open editor registers itself here to get live callbacks;
        only one can be active at a time (JUCE only ever has one open editor
        per processor instance). Pass nullptr when the editor closes. */
    void setActiveUiListener (SeparationEngine::Listener* listener) noexcept { activeUiListener = listener; }

private:
    //==============================================================================
    void separationProgress (float progress01, const juce::String& stageMessage) override;
    void separationComplete (const SeparationEngine::Result& result) override;
    void separationFailed (const juce::String& errorMessage) override;

    SeparationEngine separationEngine;
    UiState uiState;
    SeparationEngine::Listener* activeUiListener = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DRVoxSplitAudioProcessor)
};
