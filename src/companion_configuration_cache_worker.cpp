#include "companion_configuration_cache_worker.h"

#include "companion_configuration_cache_secure_storage_adapter.h"

#include <QSettings>

#include <utility>

CompanionConfigurationCacheWorker::CompanionConfigurationCacheWorker(
    const QString &appDataLocation, QObject *parent)
    : QObject(parent), m_appDataLocation(appDataLocation)
{
}

CompanionConfigurationCacheWorker::CompanionConfigurationCacheWorker(
    std::unique_ptr<QSettings> settings,
    std::unique_ptr<CompanionConfigurationCacheSecureStore> secureStore,
    const QString &lockFilePath,
    QObject *parent)
    : QObject(parent), m_lockFilePath(lockFilePath),
      m_settings(std::move(settings)), m_secureStore(std::move(secureStore))
{
}

CompanionConfigurationCacheWorker::~CompanionConfigurationCacheWorker() = default;

bool CompanionConfigurationCacheWorker::ensureInitialized(QString *errorCode)
{
    if (m_cache) {
        if (errorCode) errorCode->clear();
        return true;
    }
    if (m_initializeAttempted) {
        if (errorCode) *errorCode = m_initializationError;
        return false;
    }
    m_initializeAttempted = true;
    if (!m_settings) m_settings = std::make_unique<QSettings>();
    if (!m_secureStore) {
        m_secureStore =
            std::make_unique<SecureStorageCompanionConfigurationCacheAdapter>();
    }
    if (m_lockFilePath.isEmpty()) {
        m_lockFilePath =
            SecureStorageCompanionConfigurationCacheAdapter::prepareLockFilePath(
                m_appDataLocation, &m_initializationError);
    }
    if (m_lockFilePath.isEmpty() || !m_settings || !m_secureStore) {
        if (m_initializationError.isEmpty()) {
            m_initializationError =
                QStringLiteral("companion-cache-worker-initialization-failed");
        }
        if (errorCode) *errorCode = m_initializationError;
        return false;
    }
    m_cache = std::make_unique<CompanionConfigurationCache>(
        m_secureStore.get(), m_settings.get(), m_lockFilePath);
    if (errorCode) errorCode->clear();
    return true;
}

void CompanionConfigurationCacheWorker::initialize()
{
    QString errorCode;
    const bool available = ensureInitialized(&errorCode);
    emit initialized(available, errorCode);
}

void CompanionConfigurationCacheWorker::loadView(
    quint64 generation, const QString &accountIdentity, qint64 nowMs)
{
    QString errorCode;
    if (!ensureInitialized(&errorCode)) {
        emit viewLoaded(
            generation,
            static_cast<int>(CompanionConfigurationCacheState::Unavailable),
            0, errorCode);
        return;
    }
    const CompanionConfigurationCacheView view = m_cache->view(
        accountIdentity, nowMs);
    emit viewLoaded(
        generation, static_cast<int>(view.state),
        view.configuration.value(QStringLiteral("key_count")).toInt(),
        view.errorCode);
}

void CompanionConfigurationCacheWorker::commitLiveConfiguration(
    quint64 generation, const QString &accountIdentity,
    const QJsonObject &projection, qint64 nowMs)
{
    QString errorCode;
    if (!ensureInitialized(&errorCode)) {
        emit configurationCommitFinished(
            generation, false, errorCode, QString());
        return;
    }
    const bool committed = m_cache->commitLiveConfiguration(
        accountIdentity, projection, nowMs, &errorCode);
    emit configurationCommitFinished(
        generation, committed, errorCode, m_cache->lastWarning());
}

void CompanionConfigurationCacheWorker::mergeWebsiteModels(
    quint64 generation, const QString &accountIdentity,
    const QString &configurationSha256, const QString &keyIdentity,
    const QString &platform, const QJsonObject &projection,
    qint64 observedAtMs, qint64 nowMs)
{
    QString errorCode;
    if (!ensureInitialized(&errorCode)) {
        emit modelMergeFinished(generation, false, errorCode, QString());
        return;
    }
    if (projection.value(QStringLiteral("key_identity")).toString()
            != keyIdentity) {
        emit modelMergeFinished(
            generation, false,
            QStringLiteral("companion-cache-model-key-binding-invalid"),
            QString());
        return;
    }
    const bool merged = m_cache->mergeWebsiteModels(
        accountIdentity, configurationSha256, platform, projection,
        observedAtMs, nowMs, &errorCode);
    emit modelMergeFinished(
        generation, merged, errorCode, m_cache->lastWarning());
}
