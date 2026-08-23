#ifndef COMPANION_CONFIGURATION_CACHE_PRESENTATION_H
#define COMPANION_CONFIGURATION_CACHE_PRESENTATION_H

#include "companion_configuration_cache.h"

#include <QList>
#include <QString>
#include <QStringList>

struct CompanionCachedKeyPresentation
{
    QString keyIdentity;
    QString displayName;
    QString groupLabel;
    QString platform;
    QString state;
};

struct CompanionCachedModelPresentation
{
    QString keyIdentity;
    QString platform;
    QStringList modelIds;
    qint64 capturedAtMs = 0;
    qint64 validUntilMs = 0;
    QString configurationObservationSha256;
    QString sourceObservationSha256;
};

struct CompanionConfigurationCachePresentation
{
    CompanionConfigurationCacheState state =
        CompanionConfigurationCacheState::Unavailable;
    QString accountIdentity;
    QString provenance;
    qint64 revision = 0;
    qint64 capturedAtMs = 0;
    qint64 validUntilMs = 0;
    qint64 staleUntilMs = 0;
    QString sourceObservationSha256;
    QString contentSha256;
    QList<CompanionCachedKeyPresentation> keys;
    QList<CompanionCachedModelPresentation> models;
};

class CompanionConfigurationCachePresentationAdapter
{
public:
    static bool build(
        const CompanionConfigurationCacheView &view,
        const QString &viewAccountIdentity,
        const QString &expectedAccountIdentity,
        qint64 nowMs,
        CompanionConfigurationCachePresentation *presentation,
        QString *errorCode = nullptr);
    static qint64 ageForDisplay(
        CompanionConfigurationCachePresentation *presentation,
        qint64 nowMs);
};

#endif // COMPANION_CONFIGURATION_CACHE_PRESENTATION_H
