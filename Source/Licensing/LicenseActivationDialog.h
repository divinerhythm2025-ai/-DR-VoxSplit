/*
  ==============================================================================
    LicenseActivationDialog.h

    Small modal dialog for entering/activating a license key, or (once
    licensed) deactivating this machine to free the slot before switching to
    another one. Content-only Component - show() wraps it in a
    juce::DialogWindow via LaunchOptions, matching the async-modal pattern
    already used elsewhere in this plugin (see PluginEditor::startButtonClicked()'s
    AlertWindow::showAsync call for the long-file warning).

    Deliberately doesn't hand the caller a "dialog closed" callback - the
    editor is expected to observe LicenseManager::Listener directly instead
    (see PluginEditor.h), which fires exactly when activation/deactivation
    actually succeeds, independent of whether/how this dialog gets closed.
    That's simpler and more robust than threading a callback through
    DialogWindow's modal-state lifecycle for something the editor can already
    learn about the source of truth (LicenseManager) directly.
  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "LicenseManager.h"

class LicenseActivationDialog : public juce::Component
{
public:
    explicit LicenseActivationDialog (LicenseManager& manager);
    ~LicenseActivationDialog() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    /** Shows this dialog modally over parentComponent (may be nullptr).
        Returns immediately - never blocks the message thread. The dialog
        (and its DialogWindow) delete themselves once closed. */
    static void show (juce::Component* parentComponent, LicenseManager& manager);

private:
    void refreshForCurrentState();
    void activateClicked();
    void deactivateClicked();
    void closeDialog();

    LicenseManager& licenseManager;

    juce::Label titleLabel, descriptionLabel, statusLabel;
    juce::TextEditor keyEditor;
    juce::TextButton activateButton { "Activate" };
    juce::TextButton deactivateButton { "Deactivate This Machine" };
    juce::TextButton closeButton { "Close" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseActivationDialog)
};
