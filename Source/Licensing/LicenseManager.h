/*
  ==============================================================================
    LicenseManager.h

    Owns the plugin's activation state: talks to the activation server
    (thedivinerhythm.com's WooCommerce-driven REST API - see the WordPress-side
    plan this was built against) to turn a purchased license key into a
    locally-cached, RSA-signed token (see LicenseCrypto.h/LicensePublicKey.h),
    and answers isCurrentlyLicensed() entirely offline from that cache the rest
    of the time.

    WIRE CONTRACT
    -------------
    POST /activate, /deactivate, /validate all take a JSON body
    {"license_key": "...", "machine_id_hash": "..."}.

    /activate and /validate, on success, respond with
    {"payload": "<the exact JSON string that was signed>", "signature": "<base64 RSA signature>"}.
    `payload` is kept and re-verified as an opaque string (not re-serialized
    from parsed fields) precisely so client-side JSON formatting can never
    disagree with what the server actually signed - see LicenseCrypto.h.
    Once verified, its fields are parsed out for local use:
    {"license_key", "machine_id_hash", "product", "issued_at", "expires_at"}
    (all required; "product" must equal "DR-VoxSplit").

    On failure (any non-2xx, or a connection failure), the server is expected
    to respond with {"message": "human readable reason"} where possible - used
    directly as the error shown in the activation dialog.

    OFFLINE / GRACE BEHAVIOUR
    --------------------------
    isCurrentlyLicensed() only ever reads the local cache - no network call,
    safe to call on the message thread as often as needed (e.g. every time the
    Start button is pressed). A token remains valid until
    expires_at + kClockSkewGraceSeconds, so a machine that's offline for a
    while keeps working right up to that point, then locks out until the next
    successful activate()/validate(). revalidateInBackground() is what
    refreshes expires_at while the machine has a connection - see its own
    comment for the throttling/failure behaviour.

    THREADING
    ---------
    activate()/deactivate() kick off work via juce::Thread::launch() (blocking
    network I/O must never run on the message thread) and always deliver their
    callback back via juce::MessageManager::callAsync(), so callers (the
    editor) can safely touch UI directly from the callback. Only one such
    request may be in flight at a time - see requestInFlight.
    isCurrentlyLicensed()/getStatusMessage()/getMachineIdHash() are cheap, do
    no I/O, and are safe to call from the message thread at any time.
  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_cryptography/juce_cryptography.h>
#include <atomic>
#include <functional>
#include <memory>

class LicenseManager
{
public:
    LicenseManager();
    ~LicenseManager();

    //==============================================================================
    struct Listener
    {
        virtual ~Listener() = default;

        /** Called (on the message thread) whenever activation/deactivation/
            revalidation changes what isCurrentlyLicensed()/getStatusMessage()
            would return. Not called on a failed attempt that leaves state
            unchanged - use the ActivationCallback for that. */
        virtual void licenseStateChanged() = 0;
    };
    void addListener (Listener* l);
    void removeListener (Listener* l);

    //==============================================================================
    /** SHA-256 hash (hex) of juce::SystemStats::getUniqueDeviceID() - hashed
        before it ever leaves this process, so no real hardware fingerprint is
        ever transmitted or stored server-side. Computed once and cached;
        cheap to call repeatedly. */
    juce::String getMachineIdHash();

    /** True if the locally cached token is present, correctly signed for THIS
        machine, for this product, and not expired (with grace - see the file
        comment above). Pure local check, no network, safe on the message
        thread. */
    bool isCurrentlyLicensed() const;

    /** Short human-readable status for the UI, e.g. "Not activated",
        "Licensed - valid until 3 Oct 2026", "License expired - reconnect to
        renew". Derived entirely from local state, same cost as
        isCurrentlyLicensed(). */
    juce::String getStatusMessage() const;

    /** License key the currently-cached token was issued for, or an empty
        string if none is cached. For display ("Licensed to DRVX-...") and as
        the default target of deactivate(). */
    juce::String getCachedLicenseKey() const;

    //==============================================================================
    using ActivationCallback = std::function<void (bool success, juce::String errorMessage)>;

    /** Activates licenseKey for this machine. On success, verifies and
        persists the returned signed token and fires Listener::
        licenseStateChanged() before the callback. Returns false immediately
        (callback never called) if a request is already in flight. */
    bool activate (const juce::String& licenseKey, ActivationCallback callback);

    /** Deactivates the currently cached license for this machine (does
        nothing but call back with an error if none is cached). Clears the
        local token on ANY outcome that the server explicitly confirmed OR
        that was a genuine server-side rejection - but deliberately NOT on a
        bare connection failure, so a flaky connection can't be used to strand
        a customer's own activation slot; retry once you're back online. */
    bool deactivate (ActivationCallback callback);

    /** Fire-and-forget periodic re-validation: if a token is cached and
        enough time has passed since the last attempt (see
        kRevalidateMinIntervalSeconds - safe to call this far more often than
        that, e.g. once per editor open), asks the server to confirm the
        license is still active and, if so, refreshes the cached token's
        signature/expiry. Does nothing destructive on failure of any kind
        (network down, server error, revoked) beyond NOT refreshing - a
        revoked license simply stops being renewed and ages out via its
        existing expires_at; it is never cleared synchronously from here. */
    void revalidateInBackground();

private:
    //==============================================================================
    struct CachedToken
    {
        bool isPresent = false;
        bool signatureValid = false;
        juce::String licenseKey, machineIdHash, product;
        int64_t issuedAt = 0, expiresAt = 0;
    };
    CachedToken loadAndVerifyCachedToken() const;

    void storeToken (const juce::String& payloadJson, const juce::String& signatureBase64);
    void clearToken();

    struct RequestOutcome { bool ok = false; juce::String errorMessage; };
    /** Shared POST implementation for /activate and /validate - on success,
        verifies the response and stores it via storeToken(); on failure,
        state is left untouched and errorMessage explains why. Runs
        synchronously - callers are expected to already be off the message
        thread (see activate()/revalidateInBackground()). */
    RequestOutcome requestAndStoreToken (const juce::String& endpointPath,
                                          const juce::String& licenseKey,
                                          const juce::String& machineIdHash);

    void notifyListeners();

    std::unique_ptr<juce::PropertiesFile> propertiesFile;
    juce::CriticalSection listenerLock;
    juce::Array<Listener*> listeners;

    std::atomic<bool> requestInFlight { false };
    juce::int64 lastRevalidateAttemptMs = 0;

    juce::String cachedMachineIdHash; // computed lazily, once

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseManager)
};
