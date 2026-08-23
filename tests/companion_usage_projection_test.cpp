#include "companion_config_projection.h"
#include "companion_usage_projection.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QJsonArray rawKeys{
        QJsonObject{
            { QStringLiteral("id"), 17 },
            { QStringLiteral("key"), QStringLiteral("sk-usage-secret-value") },
            { QStringLiteral("name"), QStringLiteral("主用 Key") },
            { QStringLiteral("status"), QStringLiteral("active") },
            { QStringLiteral("group"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("Codex") },
                { QStringLiteral("platform"), QStringLiteral("openai") },
            } },
        },
    };
    const QString accountIdentity = QStringLiteral(
        "website-account-session:sha256:") + QString(64, QLatin1Char('a'));
    QString error;
    const QJsonObject configuration = CompanionConfigProjection::fromWebsiteApiKeys(
        rawKeys, accountIdentity, QStringLiteral("https://www.aegisy.cc"),
        1770000000000LL, &error);
    if (!require(!configuration.isEmpty(), "configuration fixture failed")) return 1;
    const QString keyIdentity = configuration.value(QStringLiteral("keys")).toArray()
        .at(0).toObject().value(QStringLiteral("key_identity")).toString();
    const QHash<QString, QJsonObject> metrics{
        { keyIdentity, QJsonObject{
            { QStringLiteral("today_actual_cost"), 1.25 },
            { QStringLiteral("total_actual_cost"), 9.5 },
            { QStringLiteral("quota_used"), 12.0 },
            { QStringLiteral("quota"), 100.0 },
        } },
    };
    const QJsonObject projection = CompanionUsageProjection::fromConfiguration(
        configuration, metrics, &error);
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    if (!require(!projection.isEmpty() && CompanionUsageProjection::validate(projection),
                 "valid usage projection was rejected")
            || !require(projection.value(QStringLiteral("raw_key_ids_included"))
                            == QJsonValue(false),
                        "usage projection claims raw Key IDs")
            || !require(!encoded.contains("sk-usage-secret-value"),
                        "usage projection contains credential plaintext")
            || !require(!encoded.contains("\"id\":17"),
                        "usage projection contains the raw website Key ID")) {
        return 1;
    }

    QJsonObject tampered = projection;
    QJsonArray keys = tampered.value(QStringLiteral("keys")).toArray();
    QJsonObject key = keys.at(0).toObject();
    key.insert(QStringLiteral("raw_key_id"), 17);
    keys.replace(0, key);
    tampered.insert(QStringLiteral("keys"), keys);
    if (!require(!CompanionUsageProjection::validate(tampered),
                 "raw website Key ID field was accepted")) {
        return 1;
    }

    QHash<QString, QJsonObject> invalidMetrics = metrics;
    QJsonObject invalid = invalidMetrics.value(keyIdentity);
    invalid.insert(QStringLiteral("quota"), -1.0);
    invalidMetrics.insert(keyIdentity, invalid);
    if (!require(CompanionUsageProjection::fromConfiguration(
                     configuration, invalidMetrics).isEmpty(),
                 "negative usage metric was accepted")) {
        return 1;
    }
    return 0;
}
