/*
  ==============================================================================
    LicenseActivationDialog.cpp
    See LicenseActivationDialog.h.
  ==============================================================================
*/

#include "LicenseActivationDialog.h"

LicenseActivationDialog::LicenseActivationDialog (LicenseManager& manager)
    : licenseManager (manager)
{
    titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    descriptionLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    descriptionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9ba1ac));
    descriptionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (descriptionLabel);

    keyEditor.setTextToShowWhenEmpty ("DRVX-XXXX-XXXX-XXXX-XXXX", juce::Colour (0xff5a5f6a));
    keyEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0c1016));
    keyEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    keyEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff33373f));
    keyEditor.setJustification (juce::Justification::centred);
    keyEditor.onReturnKey = [this] { activateClicked(); };
    addAndMakeVisible (keyEditor);

    activateButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff494c53));
    activateButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    activateButton.onClick = [this] { activateClicked(); };
    addAndMakeVisible (activateButton);

    deactivateButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff11151e));
    deactivateButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc5c9d4));
    deactivateButton.onClick = [this] { deactivateClicked(); };
    addChildComponent (deactivateButton);

    closeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff11151e));
    closeButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffc5c9d4));
    closeButton.onClick = [this] { closeDialog(); };
    addAndMakeVisible (closeButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff858a94));
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (statusLabel);

    refreshForCurrentState();
    setSize (420, 260);
}

LicenseActivationDialog::~LicenseActivationDialog() = default;

void LicenseActivationDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff11151e));
}

void LicenseActivationDialog::resized()
{
    auto area = getLocalBounds().reduced (24);

    titleLabel.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);
    descriptionLabel.setBounds (area.removeFromTop (36));
    area.removeFromTop (16);

    if (keyEditor.isVisible())
    {
        keyEditor.setBounds (area.removeFromTop (36));
        area.removeFromTop (12);
    }

    if (activateButton.isVisible())
        activateButton.setBounds (area.removeFromTop (36));

    if (deactivateButton.isVisible())
        deactivateButton.setBounds (area.removeFromTop (36));

    area.removeFromTop (12);
    statusLabel.setBounds (area.removeFromTop (32));

    closeButton.setBounds (getLocalBounds().reduced (24).removeFromBottom (32));
}

void LicenseActivationDialog::refreshForCurrentState()
{
    const bool licensed = licenseManager.isCurrentlyLicensed();

    titleLabel.setText (licensed ? "DR-VoxSplit is licensed" : "Activate DR-VoxSplit",
                         juce::dontSendNotification);
    descriptionLabel.setText (licensed
        ? "This machine is activated. Deactivate it here before moving your license to another machine."
        : "Enter the license key you received by email after purchase.",
        juce::dontSendNotification);

    keyEditor.setVisible (! licensed);
    activateButton.setVisible (! licensed);
    deactivateButton.setVisible (licensed);

    activateButton.setEnabled (true);
    deactivateButton.setEnabled (true);

    statusLabel.setText (licensed ? licenseManager.getStatusMessage() : juce::String(),
                          juce::dontSendNotification);

    resized();
}

void LicenseActivationDialog::activateClicked()
{
    const auto key = keyEditor.getText().trim();
    if (key.isEmpty())
    {
        statusLabel.setText ("Enter your license key first.", juce::dontSendNotification);
        return;
    }

    activateButton.setEnabled (false);
    statusLabel.setText ("Activating...", juce::dontSendNotification);

    // The activation request completes asynchronously, potentially well
    // after this dialog could have been closed - guard against touching a
    // destroyed Component with a SafePointer rather than capturing `this`
    // directly.
    juce::Component::SafePointer<LicenseActivationDialog> safeThis (this);

    const bool started = licenseManager.activate (key, [safeThis] (bool success, juce::String errorMessage)
    {
        if (safeThis == nullptr)
            return;

        if (success)
        {
            safeThis->refreshForCurrentState();
        }
        else
        {
            safeThis->activateButton.setEnabled (true);
            safeThis->statusLabel.setText (errorMessage, juce::dontSendNotification);
        }
    });

    if (! started)
    {
        activateButton.setEnabled (true);
        statusLabel.setText ("Already processing a request - please wait a moment and try again.",
                              juce::dontSendNotification);
    }
}

void LicenseActivationDialog::deactivateClicked()
{
    deactivateButton.setEnabled (false);
    statusLabel.setText ("Deactivating...", juce::dontSendNotification);

    juce::Component::SafePointer<LicenseActivationDialog> safeThis (this);

    const bool started = licenseManager.deactivate ([safeThis] (bool success, juce::String errorMessage)
    {
        if (safeThis == nullptr)
            return;

        if (success)
        {
            safeThis->refreshForCurrentState();
        }
        else
        {
            safeThis->deactivateButton.setEnabled (true);
            safeThis->statusLabel.setText (errorMessage, juce::dontSendNotification);
        }
    });

    if (! started)
    {
        deactivateButton.setEnabled (true);
        statusLabel.setText ("Already processing a request - please wait a moment and try again.",
                              juce::dontSendNotification);
    }
}

void LicenseActivationDialog::closeDialog()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

void LicenseActivationDialog::show (juce::Component* parentComponent, LicenseManager& manager)
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "DR-VoxSplit License";
    options.dialogBackgroundColour = juce::Colour (0xff11151e);
    options.content.setOwned (new LicenseActivationDialog (manager));
    options.componentToCentreAround = parentComponent;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    options.launchAsync();
}
