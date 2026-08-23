#ifndef COMPANION_CONFIGURATION_CACHE_SECURE_STORAGE_ADAPTER_H
#define COMPANION_CONFIGURATION_CACHE_SECURE_STORAGE_ADAPTER_H

#include "companion_configuration_cache.h"

class SecureStorageCompanionConfigurationCacheAdapter final
    : public CompanionConfigurationCacheSecureStore
{
public:
    ReadState readFresh(const QString &scope, QByteArray *value,
                        QString *errorCode) override;
    WriteOutcome write(const QString &scope, const QByteArray &value,
                       QString *errorCode) override;

    static QString prepareLockFilePath(const QString &appDataLocation,
                                       QString *errorCode = nullptr);
};

#endif // COMPANION_CONFIGURATION_CACHE_SECURE_STORAGE_ADAPTER_H
