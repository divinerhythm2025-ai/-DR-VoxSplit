/*
  ==============================================================================
    LicenseManager.cpp
    See LicenseManager.h.
  ==============================================================================
*/

#include "LicenseManager.h"
#include "LicenseCrypto.h"
#include "LicensePublicKey.h"

#include <juce_events/juce_events.h>

namespace
{
    constexpr const char* kProductName = "DR-VoxSplit";
    constexpr const char* kBaseUrl = "https://thedivinerhythm.com/wp-json/dr-license/v1";
    constexpr int kConnectionTimeoutMs = 10000;

    // Cached token stays usable up to this long past its server-issued
    // expires_at, so an honestly-wrong local clock (or a machine that's
    // simply offline for a bit) isn't punished the instant expires_at ticks
    // over - see LicenseManager.h's OFFLINE / GRACE BEHAVIOUR note.
    constexpr int64_t kClockSkewGraceSeconds = 48 * 3600;

    // Throttle for revalidateInBackground() - callers (e.g. "every time the
    // editor opens") can call it as often as they like; this just makes sure
    // that doesn't turn into a network request every single time.
    constexpr juce::int64 kRevalidateMinIntervalMs = 6 * 3600 * 1000;

    juce::String propKeyPayload()   { return "token_payload"; }
    juce::String propKeySignature() { return "token_signature_b64"; }

    juce::RSAKey getServerPublicKey()
    {
        return juce::RSAKey (juce::String (LicensePublicKey::exponentHex) + "," + LicensePublicKey::modulusHex);
    }

    /** Builds the {license_key, machine_id_hash} request body every endpoint uses. */
    juce::String buildRequestBody (const juce::String& licenseKey, const juce::String& machineIdHash)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("license_key", licenseKey);
        obj->setProperty ("machine_id_hash", machineIdHash);
        return juce::JSON::toString (juce::var (obj), true);
    }

    /** Synchronous POST - callers must already be off the message thread.
        Returns an empty body with statusCode left at -1 on a connection
        failure (DNS/timeout/refused), as distinct from a valid HTTP error
        response. */
    struct HttpResponse { int statusCode = -1; juce::String body; };

    HttpResponse postJson (const juce::String& path, const juce::String& jsonBody)
    {
        juce::URL url (juce::String (kBaseUrl) + path);
        url = url.withPOSTData (jsonBody);

        int statusCode = -1;
        auto stream = url.createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                .withExtraHeaders ("Content-Type: application/json")
                .withConnectionTimeoutMs (kConnectionTimeoutMs)
                .withStatusCode (&statusCode));

        HttpResponse response;
        response.statusCode = statusCode;
        if (stream != nullptr)
            response.body = stream->readEntireStreamAsString();
        return response;
    }

    /** Pulls a human-readable reason out of a (possibly absent/malformed)
        error response body, falling back to something generic. */
    juce::String extractErrorMessage (const HttpResponse& response)
    {
        if (response.statusCode < 0)
            return "Couldn't reach the activation server. Check your internet connection and try again.";

        auto parsed = juce::JSON::parse (response.body);
        if (auto* obj = parsed.getDynamicObject())
            if (obj->hasProperty ("message"))
                return obj->getProperty ("message").toString();

        return "Activation server returned an unexpected error (HTTP " + juce::String (response.statusCode) + ").";
    }
}

//==============================================================================
LicenseManager::LicenseManager()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "DR-VoxSplit";
    options.filenameSuffix = "license";
    options.folderName = "DR-VoxSplit";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    // License state changes are rare (activate/deactivate/occasional
    // revalidate) and must not be lost if the host crashes shortly after -
    // save immediately rather than JUCE's default multi-second debounce.
    options.millisecondsBeforeSaving = 0;

    propertiesFile = std::make_unique<juce::PropertiesFile> (options);
}

LicenseManager::~LicenseManager()
{
    if (propertiesFile != nullptr)
        propertiesFile->saveIfNeeded();
}

void LicenseManager::addListener (Listener* l)
{
    const juce::ScopedLock sl (listenerLock);
    listeners.addIfNotAlreadyThere (l);
}

void LicenseManager::removeListener (Listener* l)
{
    const juce::ScopedLock sl (listenerLock);
    listeners.removeAllInstancesOf (l);
}

void LicenseManager::notifyListeners()
{
    juce::Array<Listener*> listenersCopy;
    {
        const juce::ScopedLock sl (listenerLock);
        listenersCopy = listeners;
    }
    for (auto* l : listenersCopy)
        l->licenseStateChanged();
}

//==============================================================================
juce::String LicenseManager::getMachineIdHash()
{
    if (cachedMachineIdHash.isEmpty())
    {
        const auto rawId = juce::SystemStats::getUniqueDeviceID();
        cachedMachineIdHash = juce::SHA256 (rawId.toUTF8()).toHexString();
    }
    return cachedMachineIdHash;
}

LicenseManager::CachedToken LicenseManager::loadAndVerifyCachedToken() const
{
    CachedToken token;

    if (propertiesFile == nullptr)
        return token;

    const auto payloadJson = propertiesFile->getValue (propKeyPayload());
    const auto signatureB64 = propertiesFile->getValue (propKeySignature());
    if (payloadJson.isEmpty() || signatureB64.isEmpty())
        return token;

    token.isPresent = true;

    juce::MemoryOutputStream decodedSignature;
    if (! juce::Base64::convertFromBase64 (decodedSignature, signatureB64))
        return token; // corrupt local state - treat as unlicensed rather than crash/assert

    const auto signatureBytes = decodedSignature.getMemoryBlock();

    token.signatureValid = LicenseCrypto::verifySha256 (
        getServerPublicKey(), LicensePublicKey::modulusBytes,
        payloadJson.toRawUTF8(), payloadJson.getNumBytesAsUTF8(), signatureBytes);

    if (! token.signatureValid)
        return token;

    auto parsedPayload = juce::JSON::parse (payloadJson);
    if (auto* obj = parsedPayload.getDynamicObject())
    {
        token.licenseKey    = obj->getProperty ("license_key").toString();
        token.machineIdHash = obj->getProperty ("machine_id_hash").toString();
        token.product       = obj->getProperty ("product").toString();
        token.issuedAt      = (int64_t) static_cast<juce::int64> (obj->getProperty ("issued_at"));
        token.expiresAt     = (int64_t) static_cast<juce::int64> (obj->getProperty ("expires_at"));
    }
    else
    {
        token.signatureValid = false; // signature checked out but payload wasn't the JSON object we expect - fail closed
    }

    return token;
}

bool LicenseManager::isCurrentlyLicensed() const
{
    const auto token = loadAndVerifyCachedToken();
    if (! token.signatureValid)
        return false;

    if (token.product != juce::String (kProductName))
        return false;

    // Not const-qualified (getMachineIdHash() lazily caches) - isCurrentlyLicensed()
    // is logically const from the caller's point of view (no observable state
    // changes), so cast away constness here rather than on the whole method.
    if (token.machineIdHash != const_cast<LicenseManager*> (this)->getMachineIdHash())
        return false;

    const auto nowSeconds = juce::Time::currentTimeMillis() / 1000;
    return nowSeconds <= token.expiresAt + kClockSkewGraceSeconds;
}

juce::String LicenseManager::getStatusMessage() const
{
    const auto token = loadAndVerifyCachedToken();

    if (! token.isPresent)
        return "Not activated";

    if (! token.signatureValid)
        return "Local license data is invalid - please activate again";

    if (token.product != juce::String (kProductName)
        || token.machineIdHash != const_cast<LicenseManager*> (this)->getMachineIdHash())
        return "Not activated";

    const auto nowSeconds = juce::Time::currentTimeMillis() / 1000;
    const juce::Time expiry (token.expiresAt * 1000);

    if (nowSeconds <= token.expiresAt + kClockSkewGraceSeconds)
        return "Licensed - valid until " + expiry.toString (true, false);

    return "License expired " + expiry.toString (true, false) + " - reconnect to renew";
}

juce::String LicenseManager::getCachedLicenseKey() const
{
    return loadAndVerifyCachedToken().licenseKey;
}

//==============================================================================
void LicenseManager::storeToken (const juce::String& payloadJson, const juce::String& signatureBase64)
{
    if (propertiesFile == nullptr)
        return;

    propertiesFile->setValue (propKeyPayload(), payloadJson);
    propertiesFile->setValue (propKeySignature(), signatureBase64);
    propertiesFile->saveIfNeeded();
}

void LicenseManager::clearToken()
{
    if (propertiesFile == nullptr)
        return;

    propertiesFile->removeValue (propKeyPayload());
    propertiesFile->removeValue (propKeySignature());
    propertiesFile->saveIfNeeded();
}

LicenseManager::RequestOutcome LicenseManager::requestAndStoreToken (const juce::String& endpointPath,
                                                                      const juce::String& licenseKey,
                                                                      const juce::String& machineIdHash)
{
    const auto response = postJson (endpointPath, buildRequestBody (licenseKey, machineIdHash));

    if (response.statusCode != 200)
    {
        RequestOutcome outcome;
        outcome.ok = false;
        outcome.errorMessage = extractErrorMessage (response);
        return outcome;
    }

    auto parsed = juce::JSON::parse (response.body);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr || ! obj->hasProperty ("payload") || ! obj->hasProperty ("signature"))
    {
        RequestOutcome outcome;
        outcome.ok = false;
        outcome.errorMessage = "Activation server returned an unexpected response.";
        return outcome;
    }

    const auto payloadJson = obj->getProperty ("payload").toString();
    const auto signatureB64 = obj->getProperty ("signature").toString();

    juce::MemoryOutputStream decodedSignature;
    if (! juce::Base64::convertFromBase64 (decodedSignature, signatureB64))
    {
        RequestOutcome outcome;
        outcome.ok = false;
        outcome.errorMessage = "Activation server returned a malformed signature.";
        return outcome;
    }

    const bool signatureValid = LicenseCrypto::verifySha256 (
        getServerPublicKey(), LicensePublicKey::modulusBytes,
        payloadJson.toRawUTF8(), payloadJson.getNumBytesAsUTF8(), decodedSignature.getMemoryBlock());

    if (! signatureValid)
    {
        RequestOutcome outcome;
        outcome.ok = false;
        outcome.errorMessage = "Activation server response failed verification. If this persists, please contact support.";
        return outcome;
    }

    storeToken (payloadJson, signatureB64);

    RequestOutcome outcome;
    outcome.ok = true;
    return outcome;
}

//==============================================================================
bool LicenseManager::activate (const juce::String& licenseKey, ActivationCallback callback)
{
    if (requestInFlight.exchange (true))
        return false;

    const auto trimmedKey = licenseKey.trim();
    const auto machineIdHash = getMachineIdHash();

    juce::Thread::launch ([this, trimmedKey, machineIdHash, callback]
    {
        const auto outcome = requestAndStoreToken ("/activate", trimmedKey, machineIdHash);

        juce::MessageManager::callAsync ([this, outcome, callback]
        {
            requestInFlight = false;
            if (outcome.ok)
                notifyListeners();
            if (callback)
                callback (outcome.ok, outcome.errorMessage);
        });
    });

    return true;
}

bool LicenseManager::deactivate (ActivationCallback callback)
{
    if (requestInFlight.exchange (true))
        return false;

    const auto licenseKey = getCachedLicenseKey();
    if (licenseKey.isEmpty())
    {
        requestInFlight = false;
        if (callback)
            callback (false, "No license is currently activated on this machine.");
        return true;
    }

    const auto machineIdHash = getMachineIdHash();

    juce::Thread::launch ([this, licenseKey, machineIdHash, callback]
    {
        const auto response = postJson ("/deactivate", buildRequestBody (licenseKey, machineIdHash));

        // Clear local state on an explicit server response (success OR a
        // real rejection, e.g. "not found") but NOT on a bare connection
        // failure (statusCode < 0) - see the header comment for why.
        const bool serverResponded = response.statusCode >= 0;
        const bool ok = response.statusCode == 200;

        juce::String errorMessage;
        if (! ok)
            errorMessage = extractErrorMessage (response);

        juce::MessageManager::callAsync ([this, serverResponded, ok, errorMessage, callback]
        {
            requestInFlight = false;
            if (serverResponded)
            {
                clearToken();
                notifyListeners();
            }
            if (callback)
                callback (ok, errorMessage);
        });
    });

    return true;
}

void LicenseManager::revalidateInBackground()
{
    if (requestInFlight.exchange (true))
        return;

    const auto now = juce::Time::currentTimeMillis();
    if (now - lastRevalidateAttemptMs < kRevalidateMinIntervalMs)
    {
        requestInFlight = false;
        return;
    }
    lastRevalidateAttemptMs = now;

    const auto licenseKey = getCachedLicenseKey();
    if (licenseKey.isEmpty())
    {
        requestInFlight = false;
        return; // nothing to revalidate
    }

    const auto machineIdHash = getMachineIdHash();

    juce::Thread::launch ([this, licenseKey, machineIdHash]
    {
        // Deliberately ignores the outcome beyond "did it refresh the token" -
        // see the header comment: failure of any kind just means the existing
        // cached token keeps counting down to its own expiry, nothing here
        // ever clears it synchronously.
        const auto outcome = requestAndStoreToken ("/validate", licenseKey, machineIdHash);

        juce::MessageManager::callAsync ([this, outcome]
        {
            requestInFlight = false;
            if (outcome.ok)
                notifyListeners();
        });
    });
}
