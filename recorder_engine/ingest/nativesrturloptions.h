#ifndef NATIVESRTURLOPTIONS_H
#define NATIVESRTURLOPTIONS_H

#include <QString>
#include <QUrl>

// Native SRT connection mode, selected by the `mode=` URL query parameter.
// Mirrors srt-live-transmit / libsrt semantics. Default is Caller (so URLs
// without `mode=` keep the pre-existing caller-only behavior unchanged).
enum class NativeSrtMode { Caller, Listener, Rendezvous };

struct NativeSrtUrlOptions {
    QString streamId;
    // Encryption: a non-empty passphrase enables AES with `pbKeyLen`-byte keys.
    QString passphrase;
    // Key length in bytes (16/24/32 = AES-128/192/256). Defaults to 16 when a
    // passphrase is present without an explicit pbkeylen; 0 when no passphrase.
    int pbKeyLen = 0;
    NativeSrtMode mode = NativeSrtMode::Caller;
    // True when `mode=` named something we don't recognise (neither caller,
    // listener, nor rendezvous), so callers can reject it as genuinely-bad input.
    bool unknownMode = false;
};

NativeSrtUrlOptions nativeSrtUrlOptionsFromUrl(const QUrl& url);

// SRT requires passphrases to be 10..79 bytes; libsrt silently ignores a shorter
// one (leaving the link UNENCRYPTED). Validate up-front so a bad passphrase is a
// hard, diagnosable rejection rather than a silent security downgrade.
constexpr int kNativeSrtMinPassphraseLen = 10;
constexpr int kNativeSrtMaxPassphraseLen = 79;
bool nativeSrtPassphraseIsValid(const QString& passphrase);
bool nativeSrtPbKeyLenIsValid(int pbKeyLen);

#endif // NATIVESRTURLOPTIONS_H
