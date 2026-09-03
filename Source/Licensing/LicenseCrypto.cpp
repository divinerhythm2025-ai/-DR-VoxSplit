/*
  ==============================================================================
    LicenseCrypto.cpp
    See LicenseCrypto.h.
  ==============================================================================
*/

#include "LicenseCrypto.h"

namespace LicenseCrypto
{
    namespace
    {
        // RFC 8017 section 9.2, the fixed DER prefix (AlgorithmIdentifier +
        // OCTET STRING tag/length) that precedes the 32-byte digest itself
        // inside a SHA-256 DigestInfo. Not derived from anything - this exact
        // byte sequence is the standard for every SHA-256 PKCS#1 v1.5
        // signature, from any implementation.
        constexpr uint8_t sha256DigestInfoPrefix[] = {
            0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
            0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
        };

        constexpr size_t sha256DigestSize = 32;

        // juce::BigInteger::loadFromMemoryBlock()/toMemoryBlock() are
        // little-endian (see juce_BigInteger.h); OpenSSL's signature bytes,
        // and the EM byte string we reconstruct from applyToValue()'s result,
        // are both big-endian. This is the one conversion point between them.
        juce::MemoryBlock byteReversed (const void* data, size_t numBytes)
        {
            auto* src = static_cast<const uint8_t*> (data);
            juce::MemoryBlock out (numBytes);
            auto* dst = static_cast<uint8_t*> (out.getData());

            for (size_t i = 0; i < numBytes; ++i)
                dst[i] = src[numBytes - 1 - i];

            return out;
        }
    }

    bool verifySha256 (const juce::RSAKey& publicKey,
                        int modulusBytes,
                        const void* message,
                        size_t messageLength,
                        const juce::MemoryBlock& signature)
    {
        if (! publicKey.isValid())
        {
            jassertfalse; // programmer error: no valid key configured - never reachable from network input
            return false;
        }

        const size_t digestInfoSize = sizeof (sha256DigestInfoPrefix) + sha256DigestSize;

        if (modulusBytes <= 0 || (size_t) modulusBytes < 3 + digestInfoSize)
        {
            jassertfalse; // key too small to hold a SHA-256 PKCS#1 v1.5 signature - misconfiguration, not attacker input
            return false;
        }

        // A signature of any other length can never be valid for this key -
        // reject before doing any big-number math on attacker-controlled bytes.
        if (signature.getSize() != (size_t) modulusBytes)
            return false;

        juce::BigInteger value;
        value.loadFromMemoryBlock (byteReversed (signature.getData(), signature.getSize()));

        if (value.isZero())
            return false; // applyToValue() requires a strictly positive value

        if (! publicKey.applyToValue (value))
            return false;

        // value now holds s^e mod n (little-endian, minimal length - JUCE's
        // BigInteger never keeps leading/high zero bytes, and a valid EM's
        // most-significant byte is always 0x00 by construction below, so this
        // is routinely shorter than modulusBytes).
        auto emReversed = value.toMemoryBlock();
        if (emReversed.getSize() > (size_t) modulusBytes)
            return false; // s^e mod n can never exceed n in byte length for a valid key/signature pair

        juce::MemoryBlock em ((size_t) modulusBytes, true); // zero-filled; left-pads the high (missing) bytes
        auto* emReversedBytes = static_cast<const uint8_t*> (emReversed.getData());
        auto* emBytes = static_cast<uint8_t*> (em.getData());

        for (size_t i = 0; i < emReversed.getSize(); ++i)
            emBytes[(size_t) modulusBytes - 1 - i] = emReversedBytes[i]; // undo the reversal, right-aligned into em

        // EMSA-PKCS1-v1_5 (RFC 8017 section 9.2):
        //   EM = 0x00 || 0x01 || PS (0xFF, repeated) || 0x00 || DigestInfo
        // Every byte of the padding is checked explicitly - no shortcuts that
        // could accept a malformed-but-close-enough block.
        const size_t psSize = (size_t) modulusBytes - 3 - digestInfoSize;
        size_t pos = 0;

        if (emBytes[pos++] != 0x00) return false;
        if (emBytes[pos++] != 0x01) return false;

        for (size_t i = 0; i < psSize; ++i)
            if (emBytes[pos++] != 0xFF)
                return false;

        if (emBytes[pos++] != 0x00) return false;

        if (std::memcmp (emBytes + pos, sha256DigestInfoPrefix, sizeof (sha256DigestInfoPrefix)) != 0)
            return false;
        pos += sizeof (sha256DigestInfoPrefix);

        const auto actualDigest = juce::SHA256 (message, messageLength).getRawData();
        jassert (actualDigest.getSize() == sha256DigestSize);

        return std::memcmp (emBytes + pos, actualDigest.getData(), sha256DigestSize) == 0;
    }
}
