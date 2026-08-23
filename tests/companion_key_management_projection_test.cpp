#include "companion_config_projection.h"
#include "companion_key_management_projection.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QString projectionDigest(QJsonObject projection)
{
    projection.remove(QStringLiteral("projection_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(projection).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

QJsonObject reidentify(QJsonObject projection)
{
    projection.insert(QStringLiteral("projection_sha256"),
                      projectionDigest(projection));
    return projection;
}

QString keyUpdateHandle(QChar digit)
{
    return QStringLiteral("website-key-update:opaque:")
        + QString(64, digit);
}

QString keyDeleteHandle(QChar digit)
{
    return QStringLiteral("website-key-delete:opaque:")
        + QString(64, digit);
}

QString keyTestHandle(QChar digit)
{
    return QStringLiteral("website-key-test:opaque:")
        + QString(64, digit);
}

QString groupHandle(QChar digit)
{
    return QStringLiteral("website-group-management:opaque:")
        + QString(64, digit);
}

QString groupCreateHandle(QChar digit)
{
    return QStringLiteral("website-group-create:opaque:")
        + QString(64, digit);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QString secret = QStringLiteral("sk-management-secret-value");
    const QString rawKeyId = QStringLiteral("raw-key-id-42");
    const QJsonArray rawKeys{
        QJsonObject{
            { QStringLiteral("id"), rawKeyId },
            { QStringLiteral("key"), secret },
            { QStringLiteral("name"), QStringLiteral("主用 Codex") },
            { QStringLiteral("status"), QStringLiteral("active") },
            { QStringLiteral("group"), QJsonObject{
                { QStringLiteral("id"), 71 },
                { QStringLiteral("name"), QStringLiteral("Codex") },
                { QStringLiteral("platform"), QStringLiteral("openai") },
            } },
        },
        QJsonObject{
            { QStringLiteral("id"), 84 },
            { QStringLiteral("key"), QStringLiteral("sk-second-secret-value") },
            { QStringLiteral("name"), QStringLiteral("备用 Gemini") },
            { QStringLiteral("status"), QStringLiteral("disabled") },
            { QStringLiteral("group"), QJsonObject{
                { QStringLiteral("id"), 72 },
                { QStringLiteral("name"), QStringLiteral("Gemini") },
                { QStringLiteral("platform"), QStringLiteral("gemini") },
            } },
        },
    };
    const QString accountIdentity = QStringLiteral(
        "website-account-session:sha256:") + QString(64, QLatin1Char('a'));
    QString error;
    const QJsonObject configuration =
        CompanionConfigProjection::fromWebsiteApiKeys(
            rawKeys, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
            1770000000000LL, &error);
    if (!require(!configuration.isEmpty(), "configuration fixture failed")) return 1;
    const QJsonArray candidates = configuration.value(
        QStringLiteral("keys")).toArray();
    const QString firstIdentity = candidates.at(0).toObject().value(
        QStringLiteral("key_identity")).toString();
    const QString secondIdentity = candidates.at(1).toObject().value(
        QStringLiteral("key_identity")).toString();

    const QJsonArray groups{
        QJsonObject{
            { QStringLiteral("group_handle"), groupHandle(QLatin1Char('b')) },
            { QStringLiteral("display_name"), QStringLiteral("Codex") },
            { QStringLiteral("platform"), QStringLiteral("openai") },
            { QStringLiteral("create_handle"),
              groupCreateHandle(QLatin1Char('1')) },
        },
        QJsonObject{
            { QStringLiteral("group_handle"), groupHandle(QLatin1Char('c')) },
            { QStringLiteral("display_name"), QStringLiteral("Gemini") },
            { QStringLiteral("platform"), QStringLiteral("gemini") },
            { QStringLiteral("create_handle"),
              groupCreateHandle(QLatin1Char('2')) },
        },
    };
    const QHash<QString, QJsonObject> metadata{
        { firstIdentity, QJsonObject{
            { QStringLiteral("update_handle"), keyUpdateHandle(QLatin1Char('d')) },
            { QStringLiteral("delete_handle"), keyDeleteHandle(QLatin1Char('e')) },
            { QStringLiteral("test_handle"), keyTestHandle(QLatin1Char('f')) },
            { QStringLiteral("group_handle"), groupHandle(QLatin1Char('b')) },
            { QStringLiteral("quota"), 1000.0 },
            { QStringLiteral("quota_used"), 125.5 },
            { QStringLiteral("created_at"),
              QStringLiteral("2026-08-01T09:30:00.000Z") },
            { QStringLiteral("expires_at"),
              QStringLiteral("2027-08-01T09:30:00Z") },
        } },
        { secondIdentity, QJsonObject{
            { QStringLiteral("update_handle"), keyUpdateHandle(QLatin1Char('3')) },
            { QStringLiteral("delete_handle"), keyDeleteHandle(QLatin1Char('4')) },
            { QStringLiteral("test_handle"), keyTestHandle(QLatin1Char('5')) },
            { QStringLiteral("group_handle"), groupHandle(QLatin1Char('c')) },
            { QStringLiteral("quota"), 0.0 },
            { QStringLiteral("quota_used"), 9.0 },
            { QStringLiteral("created_at"), QJsonValue::Null },
            { QStringLiteral("expires_at"), QJsonValue::Null },
        } },
    };
    const QJsonObject projection =
        CompanionKeyManagementProjection::fromConfiguration(
            configuration, metadata, groups, &error);
    const QByteArray encoded = QJsonDocument(projection).toJson(
        QJsonDocument::Compact);
    if (!require(error.isEmpty() && !projection.isEmpty(),
                 "valid management projection was rejected")
            || !require(CompanionKeyManagementProjection::validate(projection),
                        "generated management projection did not validate")
            || !require(projection.value(QStringLiteral("schema_version")).toString()
                            == QStringLiteral(
                                "aegisy-companion-key-management-projection/0.1"),
                        "management projection schema is wrong")
            || !require(projection.value(QStringLiteral("account_identity"))
                            == configuration.value(QStringLiteral("account_identity"))
                        && projection.value(QStringLiteral(
                            "configuration_projection_sha256"))
                            == configuration.value(QStringLiteral("projection_sha256")),
                        "management projection lost its configuration binding")
            || !require(projection.value(QStringLiteral("key_count")).toInt() == 2
                        && projection.value(QStringLiteral("group_count")).toInt() == 2,
                        "management projection counts are wrong")
            || !require(projection.value(QStringLiteral("raw_key_ids_included"))
                            == QJsonValue(false)
                        && projection.value(QStringLiteral("raw_group_ids_included"))
                            == QJsonValue(false)
                        && projection.value(QStringLiteral("credential_values_included"))
                            == QJsonValue(false)
                        && projection.value(QStringLiteral("credential_fragments_included"))
                            == QJsonValue(false)
                        && projection.value(QStringLiteral("configuration_authority"))
                            == QJsonValue(false)
                        && projection.value(QStringLiteral("mutation_authority"))
                            == QJsonValue(false),
                        "management projection claims raw data or authority")
            || !require(!encoded.contains(secret.toUtf8())
                        && !encoded.contains("sk-second-secret-value")
                        && !encoded.contains(rawKeyId.toUtf8())
                        && !encoded.contains("\"id\":71")
                        && !encoded.contains("\"id\":72")
                        && !encoded.contains("credential_handle")
                        && !encoded.contains("credential_state"),
                        "management projection retained a raw ID, credential, or credential handle")) {
        return 1;
    }

    QJsonObject rawField = projection;
    QJsonArray projectedKeys = rawField.value(QStringLiteral("keys")).toArray();
    QJsonObject firstKey = projectedKeys.at(0).toObject();
    firstKey.insert(QStringLiteral("raw_key_id"), rawKeyId);
    projectedKeys.replace(0, firstKey);
    rawField.insert(QStringLiteral("keys"), projectedKeys);
    if (!require(!CompanionKeyManagementProjection::validate(reidentify(rawField)),
                 "raw website Key ID field was accepted")) return 1;

    QHash<QString, QJsonObject> badMetadata = metadata;
    QJsonObject badFirst = badMetadata.value(firstIdentity);
    badFirst.insert(QStringLiteral("update_handle"),
                    QStringLiteral("website-key-update:opaque:")
                        + QString(64, QLatin1Char('A')));
    badMetadata.insert(firstIdentity, badFirst);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, badMetadata, groups).isEmpty(),
                 "uppercase management handle was accepted")) return 1;

    badMetadata = metadata;
    badFirst = badMetadata.value(firstIdentity);
    badFirst.insert(QStringLiteral("update_handle"),
                    keyUpdateHandle(QLatin1Char('4')));
    badMetadata.insert(firstIdentity, badFirst);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, badMetadata, groups).isEmpty(),
                 "reused action-handle token was accepted")) return 1;

    QJsonArray badGroups = groups;
    QJsonObject badGroup = badGroups.at(0).toObject();
    badGroup.insert(QStringLiteral("group_handle"),
                    QStringLiteral("website-group-management:opaque:short"));
    badGroups.replace(0, badGroup);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, metadata, badGroups).isEmpty(),
                 "short group handle was accepted")) return 1;

    badGroups = groups;
    badGroup = badGroups.at(0).toObject();
    badGroup.insert(QStringLiteral("create_handle"),
                    groupCreateHandle(QLatin1Char('c')));
    badGroups.replace(0, badGroup);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, metadata, badGroups).isEmpty(),
                 "reused group/create handle token was accepted")) return 1;

    badMetadata = metadata;
    badFirst = badMetadata.value(firstIdentity);
    badFirst.insert(QStringLiteral("group_handle"), groupHandle(QLatin1Char('c')));
    badMetadata.insert(firstIdentity, badFirst);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, badMetadata, groups).isEmpty(),
                 "cross-group handle binding was accepted")) return 1;

    badMetadata = metadata;
    badFirst = badMetadata.value(firstIdentity);
    badFirst.insert(QStringLiteral("quota"), -1.0);
    badMetadata.insert(firstIdentity, badFirst);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, badMetadata, groups).isEmpty(),
                 "negative quota was accepted")) return 1;

    badMetadata = metadata;
    badFirst = badMetadata.value(firstIdentity);
    badFirst.insert(QStringLiteral("expires_at"),
                    QStringLiteral("2025-08-01T09:30:00Z"));
    badMetadata.insert(firstIdentity, badFirst);
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, badMetadata, groups).isEmpty(),
                 "expiry before creation was accepted")) return 1;

    QJsonObject authority = projection;
    authority.insert(QStringLiteral("mutation_authority"), true);
    if (!require(!CompanionKeyManagementProjection::validate(reidentify(authority)),
                 "mutation authority was accepted")) return 1;

    QJsonObject secretDisplay = projection;
    projectedKeys = secretDisplay.value(QStringLiteral("keys")).toArray();
    firstKey = projectedKeys.at(0).toObject();
    firstKey.insert(QStringLiteral("display_name"),
                    QStringLiteral("sk-secret-shaped-display"));
    projectedKeys.replace(0, firstKey);
    secretDisplay.insert(QStringLiteral("keys"), projectedKeys);
    if (!require(!CompanionKeyManagementProjection::validate(
                     reidentify(secretDisplay)),
                 "credential-shaped display metadata was accepted")) return 1;

    firstKey.insert(QStringLiteral("display_name"),
                    QStringLiteral("SK-UPPERCASE-SECRET-DISPLAY"));
    projectedKeys.replace(0, firstKey);
    secretDisplay.insert(QStringLiteral("keys"), projectedKeys);
    if (!require(!CompanionKeyManagementProjection::validate(
                     reidentify(secretDisplay)),
                 "uppercase credential-shaped display metadata was accepted")) return 1;

    QJsonObject fractionalCount = projection;
    fractionalCount.insert(QStringLiteral("key_count"), 2.5);
    if (!require(!CompanionKeyManagementProjection::validate(
                     reidentify(fractionalCount)),
                 "fractional Key count was accepted")) return 1;
    fractionalCount = projection;
    fractionalCount.insert(QStringLiteral("group_count"), 2.5);
    if (!require(!CompanionKeyManagementProjection::validate(
                     reidentify(fractionalCount)),
                 "fractional group count was accepted")) return 1;

    QJsonObject invalidState = projection;
    projectedKeys = invalidState.value(QStringLiteral("keys")).toArray();
    firstKey = projectedKeys.at(0).toObject();
    firstKey.insert(QStringLiteral("state"), QStringLiteral("revoked"));
    projectedKeys.replace(0, firstKey);
    invalidState.insert(QStringLiteral("keys"), projectedKeys);
    if (!require(!CompanionKeyManagementProjection::validate(
                     reidentify(invalidState)),
                 "unknown state metadata was accepted")) return 1;

    QHash<QString, QJsonObject> extraMetadata = metadata;
    extraMetadata.insert(QStringLiteral("website-key:sha256:")
                             + QString(64, QLatin1Char('f')),
                         metadata.value(firstIdentity));
    if (!require(CompanionKeyManagementProjection::fromConfiguration(
                     configuration, extraMetadata, groups).isEmpty(),
                 "extra Key metadata was accepted")) return 1;

    return 0;
}
