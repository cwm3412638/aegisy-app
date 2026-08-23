#ifndef COMPANION_KEY_MANAGEMENT_PROJECTION_H
#define COMPANION_KEY_MANAGEMENT_PROJECTION_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class CompanionKeyManagementProjection
{
public:
    static QJsonObject fromConfiguration(
        const QJsonObject &configurationProjection,
        const QHash<QString, QJsonObject> &metadataByKeyIdentity,
        const QJsonArray &groups,
        QString *errorCode = nullptr);
    static bool validate(const QJsonObject &projection,
                         QString *errorCode = nullptr);
};

#endif // COMPANION_KEY_MANAGEMENT_PROJECTION_H
