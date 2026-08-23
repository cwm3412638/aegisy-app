#include "companion_config_projection.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QJsonObject key(const QJsonValue &id, const QString &name,
                const QString &credential, const QString &status)
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), name },
        { QStringLiteral("key"), credential },
        { QStringLiteral("status"), status },
        { QStringLiteral("group"), QJsonObject{
            { QStringLiteral("id"), 7 },
            { QStringLiteral("name"), QStringLiteral("Codex") },
        } },
        { QStringLiteral("models"), QJsonArray{ QStringLiteral("gpt-test") } },
    };
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyTest"));
    QCoreApplication::setApplicationName(QStringLiteral("CompanionProjection"));
    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

    const QString firstSecret = QStringLiteral("sk-live-credential-value-one");
    const QString secondSecret = QStringLiteral("sk-live-credential-value-two");
    const QJsonArray input{
        key(QStringLiteral("key-user-1"), QStringLiteral("工作 Codex"),
            firstSecret, QStringLiteral("active")),
        key(42, QStringLiteral("备用配置"), secondSecret,
            QStringLiteral("disabled")),
    };
    QString error;
    const QString accountIdentity =
        CompanionConfigProjection::accountIdentityForWebsiteId(
            QStringLiteral("account-1"));
    const QJsonObject projection =
        CompanionConfigProjection::fromWebsiteApiKeys(
            input, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
            100, &error);
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    if (!require(error.isEmpty() && !projection.isEmpty(),
                 "valid website metadata was rejected")
            || !require(CompanionConfigProjection::validate(projection, &error),
                        "generated projection did not validate")
            || !require(projection.value(QStringLiteral("key_count")).toInt() == 2,
                        "projection key count is wrong")
            || !require(!encoded.contains(firstSecret.toUtf8())
                        && !encoded.contains(secondSecret.toUtf8())
                        && !encoded.contains("key-user-1")
                        && !encoded.contains("account-1"),
                        "projection retained credential or raw website identity")
            || !require(!encoded.contains("gpt-test"),
                        "projection inferred model metadata from the Key response")
            || !require(!projection.value(QStringLiteral("credential_values_included")).toBool(true)
                        && !projection.value(QStringLiteral("configuration_authority")).toBool(true)
                        && !projection.value(QStringLiteral("configuration_applied")).toBool(true),
                        "projection claimed credential content or configuration authority")) {
        return 1;
    }

    QSettings settings;
    if (!require(CompanionConfigProjection::saveLastValid(&settings, projection, &error),
                 "valid projection cache write failed")
            || !require(CompanionConfigProjection::loadLastValid(
                            &settings, accountIdentity, &error) == projection,
                        "valid projection cache did not round trip")) {
        return 1;
    }
    const QString otherAccount =
        CompanionConfigProjection::accountIdentityForWebsiteId(
            QStringLiteral("account-2"));
    if (!require(CompanionConfigProjection::loadLastValid(
                     &settings, otherAccount, &error).isEmpty(),
                 "another account loaded the cached projection")) {
        return 1;
    }
    QJsonObject drifted = projection;
    drifted.insert(QStringLiteral("configuration_authority"), true);
    if (!require(!CompanionConfigProjection::saveLastValid(&settings, drifted, &error),
                 "authority-bearing projection replaced the cache")
            || !require(CompanionConfigProjection::loadLastValid(
                            &settings, accountIdentity, &error) == projection,
                        "invalid save damaged the last valid cache")) {
        return 1;
    }

    QJsonArray duplicate{ input.first(), input.first() };
    if (!require(CompanionConfigProjection::fromWebsiteApiKeys(
                     duplicate, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
                     101, &error).isEmpty(),
                 "duplicate website identity was accepted")) {
        return 1;
    }

    QJsonObject wrongType = projection;
    wrongType.insert(QStringLiteral("keys"), QJsonObject{});
    if (!require(!CompanionConfigProjection::validate(wrongType, &error),
                 "non-array projection keys were accepted")) {
        return 1;
    }
    QJsonArray secretName{
        key(QStringLiteral("safe-id"), QStringLiteral("sk-secret-shaped-name"),
            firstSecret, QStringLiteral("active")),
    };
    if (!require(CompanionConfigProjection::fromWebsiteApiKeys(
                     secretName, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
                     102, &error).isEmpty(),
                 "credential-shaped display text was accepted")) {
        return 1;
    }

    if (!require(CompanionConfigProjection::isTrustedWebsiteOrigin(
                     QStringLiteral("https://www.aegisy.cc")),
                 "canonical Aegisy origin was rejected")
            || !require(!CompanionConfigProjection::isTrustedWebsiteOrigin(
                            QStringLiteral("https://api.aegisy.cc/")),
                        "unreviewed Aegisy subdomain origin was accepted")
            || !require(!CompanionConfigProjection::isTrustedWebsiteOrigin(
                            QStringLiteral("http://www.aegisy.cc")),
                        "HTTP website origin was accepted")
            || !require(!CompanionConfigProjection::isTrustedWebsiteOrigin(
                            QStringLiteral("https://aegisy.cc.evil.example")),
                        "suffix-confused website origin was accepted")
            || !require(!CompanionConfigProjection::isTrustedWebsiteOrigin(
                            QStringLiteral("https://user@aegisy.cc/config")),
                        "credential/path-bearing website origin was accepted")) {
        return 1;
    }

    const QStringList cacheKeys = settings.allKeys().filter(
        QRegularExpression(QStringLiteral("^companion/config_projection_v1/")));
    if (!require(cacheKeys.size() == 1, "account-scoped cache key is missing")) return 1;
    settings.setValue(cacheKeys.first(), QByteArrayLiteral("{\"invalid\":true}"));
    if (!require(CompanionConfigProjection::loadLastValid(
                     &settings, accountIdentity, &error).isEmpty(),
                 "corrupt projection cache was accepted")) {
        return 1;
    }
    return 0;
}
