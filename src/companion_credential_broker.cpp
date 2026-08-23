#include "companion_credential_broker.h"

#include "companion_config_projection.h"
#include "secure_storage.h"

#include <QCryptographicHash>
#include <QHash>

namespace {

const char kHandlePrefix[] = "website-credential:sha256:";

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

void appendLengthFramed(QByteArray *output, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->append(static_cast<char>((size >> shift) & 0xff));
    }
    output->append(value);
}

QString handleFor(const QString &accountIdentity, const QString &keyIdentity)
{
    QByteArray input = QByteArrayLiteral("aegisy-companion-credential-handle/0.1\0");
    appendLengthFramed(&input, accountIdentity.toUtf8());
    appendLengthFramed(&input, keyIdentity.toUtf8());
    return QString::fromLatin1(kHandlePrefix)
        + QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex());
}

bool validHandle(const QString &value)
{
    const QString prefix = QString::fromLatin1(kHandlePrefix);
    if (!value.startsWith(prefix) || value.size() != prefix.size() + 64) return false;
    for (const QChar character : value.mid(prefix.size())) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

QString storageRef(const QString &handle)
{
    return QStringLiteral("companion/website/%1/api-key").arg(
        handle.mid(QString::fromLatin1(kHandlePrefix).size()));
}

bool validCredential(const QString &credential)
{
    const QByteArray utf8 = credential.toUtf8();
    if (utf8.isEmpty() || utf8.size() > 16 * 1024) return false;
    for (const QChar character : credential) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    return true;
}

struct PreviousValue {
    QString storageRef;
    QString value;
    bool existed = false;
};

void rollback(const QList<PreviousValue> &previous)
{
    for (auto it = previous.crbegin(); it != previous.crend(); ++it) {
        if (it->existed) {
            SecureStorage::saveEncrypted(it->storageRef, it->value);
        } else {
            SecureStorage::remove(it->storageRef);
        }
    }
}

} // namespace

QJsonObject CompanionCredentialBroker::stage(
    const QJsonArray &websiteKeys, const QJsonObject &projection, QString *errorCode)
{
    QString validationError;
    if (!CompanionConfigProjection::validate(projection, &validationError)
            || websiteKeys.size()
                != projection.value(QStringLiteral("keys")).toArray().size()) {
        fail(errorCode, QStringLiteral("credential-broker-projection-invalid"));
        return {};
    }
    if (!SecureStorage::isAvailable()) {
        fail(errorCode, QStringLiteral("credential-broker-storage-unavailable"));
        return {};
    }

    const QString accountIdentity = projection.value(
        QStringLiteral("account_identity")).toString();
    const QJsonArray projectedKeys = projection.value(QStringLiteral("keys")).toArray();
    QHash<QString, QString> bindings;
    QList<PreviousValue> previous;
    for (int index = 0; index < websiteKeys.size(); ++index) {
        const QJsonObject raw = websiteKeys.at(index).toObject();
        const QString keyIdentity = CompanionConfigProjection::websiteKeyIdentity(
            raw.value(QStringLiteral("id")));
        if (keyIdentity.isEmpty()
                || keyIdentity != projectedKeys.at(index).toObject()
                    .value(QStringLiteral("key_identity")).toString()) {
            rollback(previous);
            fail(errorCode, QStringLiteral("credential-broker-key-binding-invalid"));
            return {};
        }
        const QString credential = raw.value(QStringLiteral("key")).toString();
        if (credential.isEmpty()) continue;
        if (!validCredential(credential)) {
            rollback(previous);
            fail(errorCode, QStringLiteral("credential-broker-credential-invalid"));
            return {};
        }
        const QString handle = handleFor(accountIdentity, keyIdentity);
        const QString reference = storageRef(handle);
        PreviousValue prior;
        prior.storageRef = reference;
        prior.existed = SecureStorage::contains(reference);
        if (prior.existed) prior.value = SecureStorage::loadEncrypted(reference);
        if ((prior.existed && prior.value.isEmpty())
                || !SecureStorage::saveEncrypted(reference, credential)) {
            rollback(previous);
            fail(errorCode, QStringLiteral("credential-broker-storage-failed"));
            return {};
        }
        previous.append(prior);
        bindings.insert(keyIdentity, handle);
    }

    const QJsonObject result = CompanionConfigProjection::withCredentialHandles(
        projection, bindings, &validationError);
    if (result.isEmpty()) {
        rollback(previous);
        fail(errorCode, validationError.isEmpty()
            ? QStringLiteral("credential-broker-result-invalid") : validationError);
        return {};
    }
    if (errorCode) errorCode->clear();
    return result;
}

QString CompanionCredentialBroker::resolve(
    const QString &accountIdentity, const QString &keyIdentity,
    const QString &credentialHandle, QString *errorCode)
{
    if (!validHandle(credentialHandle)
            || credentialHandle != handleFor(accountIdentity, keyIdentity)) {
        fail(errorCode, QStringLiteral("credential-broker-binding-invalid"));
        return {};
    }
    const QString credential = SecureStorage::loadEncrypted(storageRef(credentialHandle));
    if (!validCredential(credential)) {
        fail(errorCode, QStringLiteral("credential-broker-credential-unavailable"));
        return {};
    }
    if (errorCode) errorCode->clear();
    return credential;
}

bool CompanionCredentialBroker::forget(
    const QString &accountIdentity, const QString &keyIdentity,
    const QString &credentialHandle)
{
    return validHandle(credentialHandle)
        && credentialHandle == handleFor(accountIdentity, keyIdentity)
        && SecureStorage::remove(storageRef(credentialHandle));
}
