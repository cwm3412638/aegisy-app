#ifndef COMPANION_CONFIGURATION_CACHE_H
#define COMPANION_CONFIGURATION_CACHE_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QSet>
#include <QString>

class QSettings;

class CompanionConfigurationCacheLegacyCleaner
{
public:
    virtual ~CompanionConfigurationCacheLegacyCleaner() = default;
    virtual bool removeExact(QSettings *settings, const QString &key,
                             QString *errorCode) = 0;
};

class CompanionConfigurationCacheSecureStore
{
public:
    enum class ReadState {
        Missing,
        Found,
        Unavailable,
        Invalid,
    };
    enum class WriteOutcome {
        Committed,
        DefiniteFailure,
        OutcomeUnknown,
    };

    virtual ~CompanionConfigurationCacheSecureStore() = default;
    virtual ReadState readFresh(const QString &scope, QByteArray *value,
                                QString *errorCode) = 0;
    virtual WriteOutcome write(const QString &scope, const QByteArray &value,
                               QString *errorCode) = 0;
};

enum class CompanionConfigurationCacheState {
    Fresh,
    Stale,
    Expired,
    Invalid,
    Empty,
    LegacyUnverified,
    Unavailable,
    OutcomeUnknown,
    RecoveryRequired,
};

struct CompanionConfigurationCacheView
{
    CompanionConfigurationCacheState state =
        CompanionConfigurationCacheState::Unavailable;
    qint64 revision = 0;
    qint64 capturedAtMs = 0;
    qint64 validUntilMs = 0;
    qint64 staleUntilMs = 0;
    QString sourceObservationSha256;
    QString contentSha256;
    QJsonObject configuration;
    QJsonArray models;
    bool configurationAuthority = false;
    bool configurationApplied = false;
    bool modelSelectionAuthority = false;
    QString errorCode;
};

Q_DECLARE_METATYPE(CompanionConfigurationCacheView)

class CompanionConfigurationCache
{
public:
    static constexpr qint64 ConfigurationFreshMs = 24LL * 60 * 60 * 1000;
    static constexpr qint64 ConfigurationStaleMs = 7LL * 24 * 60 * 60 * 1000;
    static constexpr qint64 ModelFreshMs = 6LL * 60 * 60 * 1000;

    CompanionConfigurationCache(
        CompanionConfigurationCacheSecureStore *secureStore,
        QSettings *settings,
        const QString &lockFilePath,
        CompanionConfigurationCacheLegacyCleaner *legacyCleaner = nullptr);

    bool commitLiveConfiguration(const QString &accountIdentity,
                                 const QJsonObject &configurationProjection,
                                 qint64 nowMs,
                                 QString *errorCode = nullptr);
    bool mergeWebsiteModels(const QString &accountIdentity,
                            const QString &configurationObservationSha256,
                            const QString &platform,
                            const QJsonObject &modelProjection,
                            qint64 capturedAtMs,
                            qint64 nowMs,
                            QString *errorCode = nullptr);
    CompanionConfigurationCacheView view(const QString &accountIdentity,
                                          qint64 nowMs);
    QString lastWarning() const;

private:
    CompanionConfigurationCacheSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
    QString m_lockFilePath;
    CompanionConfigurationCacheLegacyCleaner *m_legacyCleaner = nullptr;
    QSet<QString> m_outcomeUnknownAccounts;
    QString m_lastWarning;
};

#endif // COMPANION_CONFIGURATION_CACHE_H
