#include "companion_configuration_cache_worker.h"

#include "companion_config_projection.h"
#include "companion_configuration_cache_secure_storage_adapter.h"
#include "companion_model_projection.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <memory>

namespace {

const QString kAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('a'));
const qint64 kNow = 1800000000000LL;

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

class FakeSecureStore final : public CompanionConfigurationCacheSecureStore
{
public:
    ReadState readFresh(const QString &scope, QByteArray *value,
                        QString *errorCode) override
    {
        if (forceReadState) {
            value->clear();
            if (errorCode) *errorCode = QStringLiteral("fake-cache-read-state");
            return forcedReadState;
        }
        if (readUnavailable) {
            value->clear();
            if (errorCode) *errorCode = QStringLiteral("fake-cache-read-unavailable");
            return ReadState::Unavailable;
        }
        const auto it = authorities.constFind(scope);
        if (it == authorities.cend()) return ReadState::Missing;
        *value = it.value();
        if (errorCode) errorCode->clear();
        return ReadState::Found;
    }

    WriteOutcome write(const QString &scope, const QByteArray &value,
                       QString *errorCode) override
    {
        if (failWrites) {
            readUnavailable = true;
            if (errorCode) *errorCode = QStringLiteral("fake-cache-write-failed");
            return WriteOutcome::OutcomeUnknown;
        }
        authorities.insert(scope, value);
        if (errorCode) errorCode->clear();
        return WriteOutcome::Committed;
    }

    QHash<QString, QByteArray> authorities;
    bool failWrites = false;
    bool readUnavailable = false;
    bool forceReadState = false;
    ReadState forcedReadState = ReadState::Missing;
};

QJsonObject configuration()
{
    QString error;
    return CompanionConfigProjection::fromWebsiteApiKeys(
        QJsonArray{QJsonObject{
            { QStringLiteral("id"), QStringLiteral("website-key-1") },
            { QStringLiteral("name"), QStringLiteral("Primary") },
            { QStringLiteral("status"), QStringLiteral("active") },
            { QStringLiteral("group"), QJsonObject{
                { QStringLiteral("name"), QStringLiteral("Codex") },
                { QStringLiteral("platform"), QStringLiteral("openai") },
            } },
        }},
        kAccount, QStringLiteral("https://aegisy.cc"), kNow, &error);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary directory unavailable")) return 1;

    SecureStorageCompanionConfigurationCacheAdapter adapter;
    QByteArray adapterValue;
    QString adapterError;
    if (!require(
            adapter.readFresh(QStringLiteral("auth_token"), &adapterValue,
                              &adapterError)
                == CompanionConfigurationCacheSecureStore::ReadState::Invalid
                && !adapterError.isEmpty(),
            "adapter accepted an adjacent secure-storage scope")) return 1;
    const QString uppercaseScope =
        QStringLiteral("companion/configuration-cache-authority/v1/")
        + QString(64, QLatin1Char('A'));
    if (!require(
            adapter.readFresh(uppercaseScope, &adapterValue, &adapterError)
                == CompanionConfigurationCacheSecureStore::ReadState::Invalid,
            "adapter accepted a non-canonical authority identity")) return 1;
    const QString validScope =
        QStringLiteral("companion/configuration-cache-authority/v1/")
        + QString(64, QLatin1Char('a'));
    if (!require(
            adapter.write(validScope, QByteArray("invalid\0value", 13),
                          &adapterError)
                == CompanionConfigurationCacheSecureStore::WriteOutcome::DefiniteFailure,
            "adapter accepted invalid authority bytes")) return 1;
    if (!require(
            SecureStorageCompanionConfigurationCacheAdapter::prepareLockFilePath(
                QStringLiteral("relative/cache/path"), &adapterError).isEmpty(),
            "adapter accepted a relative cache root")) return 1;
    const QString preparedLock =
        SecureStorageCompanionConfigurationCacheAdapter::prepareLockFilePath(
            directory.path(), &adapterError);
    if (!require(!preparedLock.isEmpty() && QFileInfo(preparedLock).isAbsolute()
                     && QFileInfo(preparedLock).fileName()
                         == QStringLiteral("cache-authority.lock"),
                 "adapter did not derive a stable absolute lock path")) return 1;
#ifdef Q_OS_UNIX
    QTemporaryDir symlinkDirectory;
    if (!require(symlinkDirectory.isValid(),
                 "symlink fixture directory unavailable")) return 1;
    const QString symlinkTarget = symlinkDirectory.filePath(QStringLiteral("target"));
    if (!require(QDir().mkpath(symlinkTarget)
                     && QFile::link(
                         symlinkTarget,
                         symlinkDirectory.filePath(
                             QStringLiteral("companion-configuration-cache-v2"))),
                 "cache symlink fixture could not be created")) return 1;
    if (!require(
            SecureStorageCompanionConfigurationCacheAdapter::prepareLockFilePath(
                symlinkDirectory.path(), &adapterError).isEmpty(),
            "adapter accepted a symlinked lock directory")) return 1;
#endif

    auto settings = std::make_unique<QSettings>(
        directory.filePath(QStringLiteral("cache.ini")), QSettings::IniFormat);
    auto secureStore = std::make_unique<FakeSecureStore>();
    FakeSecureStore *secure = secureStore.get();
    CompanionConfigurationCacheWorker worker(
        std::move(settings), std::move(secureStore),
        directory.filePath(QStringLiteral("cache.lock")));

    bool initialized = false;
    bool available = false;
    QObject::connect(&worker, &CompanionConfigurationCacheWorker::initialized,
                     &worker, [&](bool ready, const QString &) {
        initialized = true;
        available = ready;
    });
    worker.initialize();
    if (!require(initialized && available, "fake worker did not initialize")) return 1;

    bool committed = true;
    QString commitError;
    QObject::connect(
        &worker,
        &CompanionConfigurationCacheWorker::configurationCommitFinished,
        &worker,
        [&](quint64, bool cacheCommitted, const QString &error,
            const QString &) {
            committed = cacheCommitted;
            commitError = error;
        });
    const QJsonObject config = configuration();
    if (!require(!config.isEmpty(), "configuration fixture invalid")) return 1;
    int viewState = -1;
    int keyCount = -1;
    QString viewAccountIdentity;
    qint64 viewEvaluatedAtMs = 0;
    QObject::connect(
        &worker, &CompanionConfigurationCacheWorker::viewLoaded,
        &worker, [&](quint64, const QString &accountIdentity,
                     qint64 evaluatedAtMs,
                     const CompanionConfigurationCacheView &view) {
            if (accountIdentity != kAccount
                    && !accountIdentity.endsWith(QString(64, QLatin1Char('b')))) {
                return;
            }
            viewAccountIdentity = accountIdentity;
            viewEvaluatedAtMs = evaluatedAtMs;
            viewState = static_cast<int>(view.state);
            keyCount = view.configuration.value(
                QStringLiteral("key_count")).toInt();
        });
    secure->failWrites = true;
    worker.commitLiveConfiguration(1, kAccount, config, kNow);
    if (!require(!committed && !commitError.isEmpty(),
                 "cache failure was not reported as a cache-only degradation")) return 1;
    worker.loadView(1, kAccount, kNow);
    if (!require(viewState == static_cast<int>(
                         CompanionConfigurationCacheState::OutcomeUnknown)
                     && keyCount == 0,
                 "worker collapsed an unknown write outcome")) return 1;

    secure->failWrites = false;
    secure->readUnavailable = false;
    worker.commitLiveConfiguration(1, kAccount, config, kNow);
    if (!require(committed, "worker did not persist live configuration")) return 1;

    worker.loadView(1, kAccount, kNow);
    if (!require(viewState == static_cast<int>(
                         CompanionConfigurationCacheState::Fresh)
                     && keyCount == 1 && viewAccountIdentity == kAccount
                     && viewEvaluatedAtMs == kNow,
                 "worker did not return Fresh display-only cache")) return 1;
    worker.loadView(
        1, QStringLiteral("website-account-session:sha256:")
            + QString(64, QLatin1Char('b')),
        kNow);
    if (!require(viewState == static_cast<int>(
                         CompanionConfigurationCacheState::Empty)
                     && keyCount == 0,
                 "worker did not isolate another account's cache")) return 1;

    const QString keyIdentity = config.value(QStringLiteral("keys")).toArray()
        .first().toObject().value(QStringLiteral("key_identity")).toString();
    QString projectionError;
    const QJsonObject models = CompanionModelProjection::fromProviderResponse(
        keyIdentity,
        QJsonObject{{QStringLiteral("data"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-5.6")}},
        }}}, &projectionError);
    bool modelMerged = false;
    QObject::connect(
        &worker, &CompanionConfigurationCacheWorker::modelMergeFinished,
        &worker, [&](quint64, bool merged, const QString &, const QString &) {
            modelMerged = merged;
        });
    worker.mergeWebsiteModels(
        1, kAccount,
        config.value(QStringLiteral("projection_sha256")).toString(),
        keyIdentity, QStringLiteral("openai"), models,
        kNow + 1, kNow + 1);
    if (!require(modelMerged, "worker did not merge website model observation")) {
        return 1;
    }

    worker.loadView(
        1, kAccount,
        kNow + CompanionConfigurationCache::ConfigurationFreshMs);
    if (!require(viewState == static_cast<int>(
                         CompanionConfigurationCacheState::Stale)
                     && keyCount == 1,
                 "worker did not return Stale display-only cache")) return 1;
    worker.loadView(
        1, kAccount,
        kNow + CompanionConfigurationCache::ConfigurationFreshMs
            + CompanionConfigurationCache::ConfigurationStaleMs);
    if (!require(viewState == static_cast<int>(
                         CompanionConfigurationCacheState::Expired)
                     && keyCount == 0,
                 "worker did not return Expired display-only cache")) return 1;

    int stateFixture = 0;
    const auto verifyForcedReadState = [&](
        CompanionConfigurationCacheSecureStore::ReadState readState,
        CompanionConfigurationCacheState expected,
        const char *message) {
        auto fixtureSettings = std::make_unique<QSettings>(
            directory.filePath(QStringLiteral("state-%1.ini").arg(++stateFixture)),
            QSettings::IniFormat);
        auto fixtureStore = std::make_unique<FakeSecureStore>();
        fixtureStore->forceReadState = true;
        fixtureStore->forcedReadState = readState;
        CompanionConfigurationCacheWorker fixtureWorker(
            std::move(fixtureSettings), std::move(fixtureStore),
            directory.filePath(QStringLiteral("state.lock")));
        int observedState = -1;
        int observedCount = -1;
        QObject::connect(
            &fixtureWorker, &CompanionConfigurationCacheWorker::viewLoaded,
            &fixtureWorker,
            [&](quint64, const QString &accountIdentity,
                qint64,
                const CompanionConfigurationCacheView &view) {
                if (accountIdentity != kAccount) return;
                observedState = static_cast<int>(view.state);
                observedCount = view.configuration.value(
                    QStringLiteral("key_count")).toInt();
            });
        fixtureWorker.loadView(2, kAccount, kNow);
        return require(
            observedState == static_cast<int>(expected) && observedCount == 0,
            message);
    };
    if (!verifyForcedReadState(
            CompanionConfigurationCacheSecureStore::ReadState::Unavailable,
            CompanionConfigurationCacheState::Unavailable,
            "worker collapsed secure-storage unavailability")) return 1;
    if (!verifyForcedReadState(
            CompanionConfigurationCacheSecureStore::ReadState::Invalid,
            CompanionConfigurationCacheState::Invalid,
            "worker collapsed invalid secure-storage state")) return 1;
    return 0;
}
