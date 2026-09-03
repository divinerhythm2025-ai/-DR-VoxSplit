/*
  ==============================================================================
    LicenseCrypto.h

    Verifies RSASSA-PKCS1-v1_5 (SHA-256) signatures (RFC 8017 section 8.2.2) -
    the same signature format PHP's openssl_sign($data, $sig, $key,
    OPENSSL_ALGO_SHA256) produces server-side, which is what the activation
    server (thedivinerhythm.com) signs license tokens with.

    juce::RSAKey (juce_cryptography) is NOT a PKCS#1 implementation - it only
    exposes raw modular exponentiation (applyToValue()) with no padding or
    ASN.1 handling, and juce::BigInteger stores values little-endian while
    OpenSSL's signature bytes are big-endian. This file is the one place that
    bridges those two facts; everywhere else in the codebase should just call
    verifySha256() and treat the byte-level details as opaque.

    Deliberately standalone - no networking, no JUCE GUI/event-loop
    dependencies, no I/O - so it can be exercised directly against known-
    answer {message, signature} fixtures generated with the real
    `openssl dgst -sha256 -sign` (see scratchpad/license-keys at the time this
    was written) independently of LicenseManager/the rest of the licensing
    flow.
  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace LicenseCrypto
{
    /** Verifies that `signature` is a valid RSASSA-PKCS1-v1_5 SHA-256 signature
        over `message`, under `publicKey`.

        `signature` must be exactly `modulusBytes` bytes, big-endian - exactly
        the raw bytes OpenSSL/PHP's openssl_sign() produce, no pre-processing
        by the caller required.

        `modulusBytes` is the RSA key size in bytes (256 for RSA-2048).
        juce::RSAKey doesn't expose its modulus, so this has to be passed in
        separately by the caller - see LicensePublicKey::modulusBytes.

        Never throws and never asserts on attacker-controlled input (a
        malformed/wrong-length/forged signature just returns false); the only
        jassertfalse in the implementation is for a genuinely misconfigured
        key, i.e. a programmer error, not something a network response can
        trigger. */
    bool verifySha256 (const juce::RSAKey& publicKey,
                        int modulusBytes,
                        const void* message,
                        size_t messageLength,
                        const juce::MemoryBlock& signature);
}
