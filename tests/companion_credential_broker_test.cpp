#include "companion_credential_broker.h"

#include "companion_config_projection.h"
#include "secure_storage.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QJsonObject websiteKey(const QString &id, const QString &name,
                       const QString &credential)
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), name },
        { QStringLiteral("key"), credential },
        { QStringLiteral("status"), QStringLiteral("active") },
        { QStringLiteral("group"), QJsonObject{
            { QStringLiteral("name"), QStringLiteral("Codex") },
            { QStringLiteral("platform"), QStringLiteral("openai") },
        } },
    };
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (!SecureStorage::isAvailable()) return 0;

    const QString accountIdentity =
        CompanionConfigProjection::accountIdentityForWebsiteId(
            QStringLiteral("broker-account-fixture"));
    const QString firstCredential = QStringLiteral("sk-broker-first-credential");
    const QString secondCredential = QStringLiteral("sk-broker-second-credential");
    const QJsonArray raw{
        websiteKey(QStringLiteral("broker-key-1"), QStringLiteral("Primary"),
                   firstCredential),
        websiteKey(QStringLiteral("broker-key-2"), QStringLiteral("Secondary"),
                   secondCredential),
    };
    QString error;
    const QJsonObject base = CompanionConfigProjection::fromWebsiteApiKeys(
        raw, accountIdentity, QStringLiteral("https://www.aegisy.cc"), 100, &error);
    const QJsonObject staged = CompanionCredentialBroker::stage(raw, base, &error);
    const QByteArray encoded = QJsonDocument(staged).toJson(QJsonDocument::Compact);
    if (!require(error.isEmpty() && !staged.isEmpty(),
                 "valid credential batch did not stage")
            || !require(!encoded.contains(firstCredential.toUtf8())
                        && !encoded.contains(secondCredential.toUtf8()),
                        "staged projection contains credential plaintext")) {
        return 1;
    }

    const QJsonArray candidates = staged.value(QStringLiteral("keys")).toArray();
    if (!require(candidates.size() == 2, "staged candidate count is wrong")) return 1;
    for (int index = 0; index < candidates.size(); ++index) {
        const QJsonObject candidate = candidates.at(index).toObject();
        const QString keyIdentity = candidate.value(QStringLiteral("key_identity")).toString();
        const QString handle = candidate.value(QStringLiteral("credential_handle")).toString();
        const QString expected = index == 0 ? firstCredential : secondCredential;
        if (!require(candidate.value(QStringLiteral("credential_state")).toString()
                        == QStringLiteral("available-in-secure-storage"),
                    "candidate does not report secure-storage availability")
                || !require(CompanionCredentialBroker::resolve(
                                accountIdentity, keyIdentity, handle, &error) == expected,
                            "exact credential handle did not resolve")
                || !require(CompanionCredentialBroker::resolve(
                                CompanionConfigProjection::accountIdentityForWebsiteId(
                                    QStringLiteral("other-account")),
                                keyIdentity, handle, &error).isEmpty(),
                            "cross-account credential handle resolved")
                || !require(CompanionCredentialBroker::resolve(
                                accountIdentity,
                                CompanionConfigProjection::websiteKeyIdentity(
                                    QStringLiteral("other-key")),
                                handle, &error).isEmpty(),
                            "cross-Key credential handle resolved")) {
            return 1;
        }
    }

    QJsonArray redactedRaw = raw;
    for (int index = 0; index < redactedRaw.size(); ++index) {
        QJsonObject key = redactedRaw.at(index).toObject();
        key.insert(QStringLiteral("key"), QString());
        redactedRaw.replace(index, key);
    }
    const QJsonObject redactedBase = CompanionConfigProjection::fromWebsiteApiKeys(
        redactedRaw, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
        101, &error);
    const QJsonObject rebound = CompanionCredentialBroker::stage(
        redactedRaw, redactedBase, &error);
    const QJsonArray reboundCandidates = rebound.value(QStringLiteral("keys")).toArray();
    if (!require(reboundCandidates.size() == candidates.size(),
                 "redacted inventory did not rebind stored credentials")) return 1;
    for (int index = 0; index < reboundCandidates.size(); ++index) {
        if (!require(reboundCandidates.at(index).toObject().value(
                         QStringLiteral("credential_handle"))
                        == candidates.at(index).toObject().value(
                            QStringLiteral("credential_handle")),
                     "redacted inventory changed the stored credential handle")) {
            return 1;
        }
    }

    QJsonArray reordered{ raw.at(1), raw.at(0) };
    if (!require(CompanionCredentialBroker::stage(reordered, base, &error).isEmpty(),
                 "cross-ordered credential batch was accepted")) {
        return 1;
    }

    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        if (!require(CompanionCredentialBroker::forget(
                         accountIdentity,
                         candidate.value(QStringLiteral("key_identity")).toString(),
                         candidate.value(QStringLiteral("credential_handle")).toString()),
                     "staged credential cleanup failed")) {
            return 1;
        }
    }
    return 0;
}
