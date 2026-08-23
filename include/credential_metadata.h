#ifndef CREDENTIAL_METADATA_H
#define CREDENTIAL_METADATA_H

#include <QCryptographicHash>
#include <QString>

inline QString credentialFingerprint(const QString &credential)
{
    const QByteArray value = credential.trimmed().toUtf8();
    if (value.isEmpty()) return {};
    QByteArray input = QByteArrayLiteral("aegisy-credential-fingerprint/0.1\0");
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.append(static_cast<char>((size >> shift) & 0xff));
    }
    input.append(value);
    return QString::fromLatin1(QCryptographicHash::hash(
        input, QCryptographicHash::Sha256).toHex().left(8));
}

#endif // CREDENTIAL_METADATA_H
