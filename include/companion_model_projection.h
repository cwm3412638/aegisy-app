#ifndef COMPANION_MODEL_PROJECTION_H
#define COMPANION_MODEL_PROJECTION_H

#include <QJsonObject>
#include <QString>

class CompanionModelProjection
{
public:
    static QJsonObject fromProviderResponse(const QString &keyIdentity,
                                            const QJsonObject &response,
                                            QString *errorCode = nullptr);
    static bool validate(const QJsonObject &projection,
                         QString *errorCode = nullptr);
};

#endif // COMPANION_MODEL_PROJECTION_H
