#ifndef COMPANION_CREDENTIAL_BROKER_H
#define COMPANION_CREDENTIAL_BROKER_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class CompanionCredentialBroker
{
public:
    static QJsonObject stage(const QJsonArray &websiteKeys,
                             const QJsonObject &projection,
                             QString *errorCode = nullptr);

    static QString resolve(const QString &accountIdentity,
                           const QString &keyIdentity,
                           const QString &credentialHandle,
                           QString *errorCode = nullptr);
    static bool forget(const QString &accountIdentity,
                       const QString &keyIdentity,
                       const QString &credentialHandle);
};

#endif // COMPANION_CREDENTIAL_BROKER_H
