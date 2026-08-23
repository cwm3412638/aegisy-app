#ifndef COMPANION_CONFIG_PROJECTION_H
#define COMPANION_CONFIG_PROJECTION_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class QSettings;

class CompanionConfigProjection
{
public:
    static QJsonObject fromWebsiteApiKeys(const QJsonArray &apiKeys,
                                          const QString &accountIdentity,
                                          const QString &sourceOrigin,
                                          qint64 receivedAtMs,
                                          QString *errorCode = nullptr);
    static bool validate(const QJsonObject &projection,
                         QString *errorCode = nullptr);

    static bool saveLastValid(QSettings *settings,
                              const QJsonObject &projection,
                              QString *errorCode = nullptr);
    static QJsonObject loadLastValid(QSettings *settings,
                                     const QString &accountIdentity,
                                     QString *errorCode = nullptr);

    static bool isTrustedWebsiteOrigin(const QString &baseUrl);
    static QString accountIdentityForWebsiteId(const QJsonValue &accountId);
};

#endif // COMPANION_CONFIG_PROJECTION_H
