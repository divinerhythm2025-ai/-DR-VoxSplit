/*
  ==============================================================================
    LicensePublicKey.h

    RSA-2048 PUBLIC key used to verify signed license tokens issued by the
    activation server (thedivinerhythm.com). Public key only - safe to embed
    in the shipped binary. The matching PRIVATE key lives only in that servers
    wp-config.php and must never be committed to this repo. See
    Source/Licensing/LicenseCrypto.h for how these are used to verify a
    signature; the key pair is otherwise opaque, generator-agnostic hex.
  ==============================================================================
*/

#pragma once

namespace LicensePublicKey
{
    // Generated server-side (openssl_pkey_new) by the DR-VoxSplit License
    // Server WPCode snippet on thedivinerhythm.com on first run - the
    // matching private key never left that server (see LicenseManager.h's
    // wire-contract comment). This is that key's PUBLIC half, read back via
    // `wp option get drvx_license_public_exponent_hex` / `..._modulus_hex`.
    inline constexpr const char* exponentHex = "10001";

    inline constexpr const char* modulusHex = "A55B587774BC7B87A63246E84B578538456BDD0FA2234EEA3745882DE9CE2944E8103F5D408A67F41DCE8CE8088040F8EBD2AF84E15ECA9098542BF6C93116429E48BB0EBF1C88D85C2A8F7C4196B98162256566ACBCA7E133A3DF71DB73C844952F59EBEE3F985BD9877B2F0C9490731ABE316542EF38B6797EAFC56B9980996A5394153571EBD0AB46A09C59E98EBECEC4C7CA2EE403DE6783282B7593C6A0990032D4C2DDA24B027F9A7ED53596CA222A4791B429F48954464D6414076EE31FF8CF7D28944A4FC4BD7DECBD28DEF0373E1575D3E9430334005B1C05401241CBDD0CB022E7ED2AC5B6C14FDFAF1FCB0FD9EF828F943DC8B3F3DED551CD9FCB";

    // 2048-bit modulus = 256 bytes. A verified signature must be exactly this
    // many bytes; anything else is rejected before any crypto is attempted.
    inline constexpr int modulusBytes = 256;
}
