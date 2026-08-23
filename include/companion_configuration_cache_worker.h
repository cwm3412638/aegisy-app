#ifndef COMPANION_CONFIGURATION_CACHE_WORKER_H
#define COMPANION_CONFIGURATION_CACHE_WORKER_H

#include "companion_configuration_cache.h"

#include <QObject>
#include <QJsonObject>
#include <QString>

#include <memory>

class QSettings;

class CompanionConfigurationCacheWorker final : public QObject
{
    Q_OBJECT

public:
    explicit CompanionConfigurationCacheWorker(
        const QString &appDataLocation, QObject *parent = nullptr);
    CompanionConfigurationCacheWorker(
        std::unique_ptr<QSettings> settings,
        std::unique_ptr<CompanionConfigurationCacheSecureStore> secureStore,
        const QString &lockFilePath,
        QObject *parent = nullptr);
    ~CompanionConfigurationCacheWorker() override;

public slots:
    void initialize();
    void loadView(quint64 generation, const QString &accountIdentity,
                  qint64 nowMs);
    void commitLiveConfiguration(quint64 generation,
                                 const QString &accountIdentity,
                                 const QJsonObject &projection,
                                 qint64 nowMs);
    void mergeWebsiteModels(quint64 generation,
                            const QString &accountIdentity,
                            const QString &configurationSha256,
                            const QString &keyIdentity,
                            const QString &platform,
                            const QJsonObject &projection,
                            qint64 observedAtMs,
                            qint64 nowMs);

signals:
    void initialized(bool available, const QString &errorCode);
    void viewLoaded(quint64 generation, const QString &accountIdentity,
                    qint64 evaluatedAtMs,
                    const CompanionConfigurationCacheView &view);
    void configurationCommitFinished(quint64 generation, bool committed,
                                     const QString &errorCode,
                                     const QString &warningCode);
    void modelMergeFinished(quint64 generation, bool merged,
                            const QString &errorCode,
                            const QString &warningCode);

private:
    bool ensureInitialized(QString *errorCode);

    QString m_appDataLocation;
    QString m_lockFilePath;
    std::unique_ptr<QSettings> m_settings;
    std::unique_ptr<CompanionConfigurationCacheSecureStore> m_secureStore;
    std::unique_ptr<CompanionConfigurationCache> m_cache;
    bool m_initializeAttempted = false;
    QString m_initializationError;
};

#endif // COMPANION_CONFIGURATION_CACHE_WORKER_H
