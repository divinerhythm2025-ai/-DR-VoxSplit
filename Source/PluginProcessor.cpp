/*
  ==============================================================================

    This file contains the basic framework code for a JUCE plugin processor.

  ==============================================================================
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
DRVoxSplitAudioProcessor::DRVoxSplitAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
#endif
{
    separationEngine.addListener (this);
    separationEngine.prepare();
}

DRVoxSplitAudioProcessor::~DRVoxSplitAudioProcessor()
{
    separationEngine.removeListener (this);
}

//==============================================================================
const juce::String DRVoxSplitAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool DRVoxSplitAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool DRVoxSplitAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool DRVoxSplitAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double DRVoxSplitAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int DRVoxSplitAudioProcessor::getNumPrograms()
{
    return 1;
}

int DRVoxSplitAudioProcessor::getCurrentProgram()
{
    return 0;
}

void DRVoxSplitAudioProcessor::setCurrentProgram (int)
{
}

const juce::String DRVoxSplitAudioProcessor::getProgramName (int)
{
    return {};
}

void DRVoxSplitAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void DRVoxSplitAudioProcessor::prepareToPlay (double, int)
{
    // No live-audio state to prepare - see processBlock() below.
}

void DRVoxSplitAudioProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool DRVoxSplitAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

void DRVoxSplitAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // DR-VoxSplit does no live DSP: it's a file-processing utility, not a
    // track effect. The real work (drag-and-drop -> SeparationEngine, see
    // Source/Separation/SeparationEngine.h) happens entirely outside this
    // call, driven by the editor and running on a background thread. If this
    // plugin is inserted on a track, its audio should pass through
    // completely unaltered rather than silently mute or otherwise surprise
    // whoever put it there - hence a true passthrough (no-op) here, not even
    // a dry/wet mix or level metering tap.
    juce::ignoreUnused (buffer, midiMessages);
}

//==============================================================================
bool DRVoxSplitAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* DRVoxSplitAudioProcessor::createEditor()
{
    return new DRVoxSplitAudioProcessorEditor (*this);
}

//==============================================================================
void DRVoxSplitAudioProcessor::getStateInformation (juce::MemoryBlock&)
{
    // Nothing to persist: separation results live only for the lifetime of
    // this plugin instance (as temp WAV files - see SeparationEngine), and
    // are re-derivable from the source file the user drops in again.
}

void DRVoxSplitAudioProcessor::setStateInformation (const void*, int)
{
}

//==============================================================================
void DRVoxSplitAudioProcessor::separationProgress (float progress01, const juce::String& stageMessage)
{
    uiState.phase = UiState::Phase::processing;
    uiState.progress = progress01;
    uiState.statusMessage = stageMessage;

    if (activeUiListener != nullptr)
        activeUiListener->separationProgress (progress01, stageMessage);
}

void DRVoxSplitAudioProcessor::separationComplete (const SeparationEngine::Result& result)
{
    uiState.phase = UiState::Phase::done;
    uiState.progress = 1.0f;
    uiState.result = result;

    if (activeUiListener != nullptr)
        activeUiListener->separationComplete (result);
}

void DRVoxSplitAudioProcessor::separationFailed (const juce::String& errorMessage)
{
    uiState.phase = UiState::Phase::error;
    uiState.errorMessage = errorMessage;

    if (activeUiListener != nullptr)
        activeUiListener->separationFailed (errorMessage);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DRVoxSplitAudioProcessor();
}
