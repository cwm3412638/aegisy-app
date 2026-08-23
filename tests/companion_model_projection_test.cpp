#include "companion_model_projection.h"

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

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QString keyIdentity = QStringLiteral("website-key:sha256:")
        + QString(64, QLatin1Char('a'));
    const QJsonObject response{
        { QStringLiteral("object"), QStringLiteral("list") },
        { QStringLiteral("provider_secret"), QStringLiteral("sk-do-not-project") },
        { QStringLiteral("data"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-test")},
                        {QStringLiteral("opaque"), QStringLiteral("provider-body")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-review")}},
        } },
    };
    QString error;
    const QJsonObject projection = CompanionModelProjection::fromProviderResponse(
        keyIdentity, response, &error);
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    if (!require(error.isEmpty() && !projection.isEmpty(),
                 "valid provider model response was rejected")
            || !require(CompanionModelProjection::validate(projection, &error),
                        "model projection did not validate")
            || !require(projection.value(QStringLiteral("model_count")).toInt() == 2,
                        "model projection count is wrong")
            || !require(encoded.contains("gpt-test") && encoded.contains("gpt-review"),
                        "model projection omitted model IDs")
            || !require(!encoded.contains("sk-do-not-project")
                        && !encoded.contains("provider-body"),
                        "model projection retained provider body")) {
        return 1;
    }

    QJsonObject duplicate = response;
    duplicate.insert(QStringLiteral("data"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-test")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-test")}},
    });
    if (!require(CompanionModelProjection::fromProviderResponse(
                     keyIdentity, duplicate, &error).isEmpty(),
                 "duplicate model ID was accepted")) {
        return 1;
    }
    QJsonObject secret = response;
    secret.insert(QStringLiteral("data"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("sk-secret-model-id")}},
    });
    if (!require(CompanionModelProjection::fromProviderResponse(
                     keyIdentity, secret, &error).isEmpty(),
                 "credential-shaped model ID was accepted")) {
        return 1;
    }
    const QString localIdentity = QStringLiteral("local-profile:sha256:")
        + QString(64, QLatin1Char('d'));
    if (!require(!CompanionModelProjection::fromProviderResponse(
                      localIdentity, response, &error).isEmpty(),
                 "valid local Profile model binding was rejected")) {
        return 1;
    }
    QJsonObject whitespace = response;
    whitespace.insert(QStringLiteral("data"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), QStringLiteral(" gpt-test")}},
    });
    if (!require(CompanionModelProjection::fromProviderResponse(
                     keyIdentity, whitespace, &error).isEmpty(),
                 "whitespace-normalized provider model identity was accepted")) {
        return 1;
    }
    QJsonObject drift = projection;
    drift.insert(QStringLiteral("selection_authority"), true);
    if (!require(!CompanionModelProjection::validate(drift, &error),
                 "authority-bearing model projection was accepted")) {
        return 1;
    }
    return 0;
}
