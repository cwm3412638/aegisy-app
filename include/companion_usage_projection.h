#ifndef COMPANION_USAGE_PROJECTION_H
#define COMPANION_USAGE_PROJECTION_H

#include <QHash>
#include <QJsonObject>
#include <QString>

class CompanionUsageProjection
{
public:
    static QJsonObject fromConfiguration(
        const QJsonObject &configurationProjection,
        const QHash<QString, QJsonObject> &usageByKeyIdentity,
        QString *errorCode = nullptr);
    static bool validate(const QJsonObject &projection,
                         QString *errorCode = nullptr);
};

#endif // COMPANION_USAGE_PROJECTION_H
