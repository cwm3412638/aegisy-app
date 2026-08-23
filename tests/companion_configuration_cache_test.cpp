#include "companion_configuration_cache.h"

#include "companion_config_projection.h"
#include "companion_model_projection.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <openssl/hmac.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

const QString kAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('a'));
const QString kOtherAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('c'));
const qint64 kNow = 1800000000000LL;
const char kTestMacDomain[] =
    "aegisy-companion-configuration-cache-hmac/0.2\0";

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

QByteArray quoted(const QString &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value})
        .toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

bool safeInteger(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < 0 || number > 9007199254740991.0) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool appendCanonical(const QJsonValue &value, QByteArray *output)
{
    switch (value.type()) {
    case QJsonValue::Null: output->append("null"); return true;
    case QJsonValue::Bool:
        output->append(value.toBool() ? "true" : "false"); return true;
    case QJsonValue::Double: {
        qint64 number = 0;
        if (!safeInteger(value, &number)) return false;
        output->append(QByteArray::number(number));
        return true;
    }
    case QJsonValue::String: output->append(quoted(value.toString())); return true;
    case QJsonValue::Array: {
        output->append('[');
        const QJsonArray array = value.toArray();
        for (int index = 0; index < array.size(); ++index) {
            if (index != 0) output->append(',');
            if (!appendCanonical(array.at(index), output)) return false;
        }
        output->append(']');
        return true;
    }
    case QJsonValue::Object: {
        output->append('{');
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end(), [](const QString &left,
                                               const QString &right) {
            return left.toUtf8() < right.toUtf8();
        });
        for (int index = 0; index < keys.size(); ++index) {
            if (index != 0) output->append(',');
            output->append(quoted(keys.at(index)));
            output->append(':');
            if (!appendCanonical(object.value(keys.at(index)), output)) return false;
        }
        output->append('}');
        return true;
    }
    case QJsonValue::Undefined: return false;
    }
    return false;
}

QByteArray canonical(const QJsonObject &object)
{
    QByteArray result;
    return appendCanonical(object, &result) ? result : QByteArray();
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray mac(const QByteArray &key, const QByteArray &message)
{
    QByteArray input(kTestMacDomain, sizeof(kTestMacDomain) - 1);
    input.append(message);
    unsigned char result[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.constData(), key.size(),
              reinterpret_cast<const unsigned char *>(input.constData()),
              static_cast<size_t>(input.size()), result, &length)
            || length != 32) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char *>(result), 32);
}

QJsonObject object(const QByteArray &bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

class FakeSecureStore final : public CompanionConfigurationCacheSecureStore
{
public:
    struct WriteScript {
        int call = 0;
        WriteOutcome outcome = WriteOutcome::Committed;
        bool apply = false;
        int unavailableReads = 0;
        int invalidReads = 0;
    };

    ReadState readFresh(const QString &scope, QByteArray *value,
                        QString *errorCode) override
    {
        if (invalidReads > 0) {
            --invalidReads;
            if (errorCode) *errorCode = QStringLiteral("fake-secure-invalid");
            return ReadState::Invalid;
        }
        if (unavailable || unavailableReads > 0) {
            if (unavailableReads > 0) --unavailableReads;
            if (errorCode) *errorCode = QStringLiteral("fake-secure-unavailable");
            return ReadState::Unavailable;
        }
        if (invalid) {
            if (errorCode) *errorCode = QStringLiteral("fake-secure-invalid");
            return ReadState::Invalid;
        }
        if (!values.contains(scope)) return ReadState::Missing;
        *value = values.value(scope);
        if (errorCode) errorCode->clear();
        return ReadState::Found;
    }

    WriteOutcome write(const QString &scope, const QByteArray &value,
                       QString *errorCode) override
    {
        ++writeCalls;
        WriteScript selected;
        bool scripted = false;
        for (const WriteScript &entry : scripts) {
            if (entry.call == writeCalls) {
                selected = entry;
                scripted = true;
                break;
            }
        }
        if (!scripted) {
            values.insert(scope, value);
            if (errorCode) errorCode->clear();
            return WriteOutcome::Committed;
        }
        if (selected.apply) values.insert(scope, value);
        unavailableReads += selected.unavailableReads;
        invalidReads += selected.invalidReads;
        if (errorCode) *errorCode = QStringLiteral("fake-secure-write-script");
        return selected.outcome;
    }

    QString onlyScope() const
    {
        return values.isEmpty() ? QString() : values.constBegin().key();
    }

    QHash<QString, QByteArray> values;
    QList<WriteScript> scripts;
    int writeCalls = 0;
    int unavailableReads = 0;
    int invalidReads = 0;
    bool unavailable = false;
    bool invalid = false;
};

class FailingLegacyCleaner final
    : public CompanionConfigurationCacheLegacyCleaner
{
public:
    bool removeExact(QSettings *, const QString &, QString *errorCode) override
    {
        if (errorCode) *errorCode = QStringLiteral("fake-legacy-cleanup-failed");
        return false;
    }
};

QString cacheBase(const QString &account)
{
    return QStringLiteral("companion/revisioned_configuration_cache_v2/")
        + account.mid(QStringLiteral("website-account-session:sha256:").size());
}

QString slotKey(const QString &account, const QString &slot)
{
    return cacheBase(account) + QStringLiteral("/slot_") + slot;
}

QString legacyKey(const QString &account)
{
    return QStringLiteral("companion/config_projection_v1/")
        + account.mid(QStringLiteral("website-account-session:sha256:").size());
}

QString lockPath(QSettings *settings)
{
    return QFileInfo(settings->fileName()).dir().filePath(
        QStringLiteral("companion-cache-authority.lock"));
}

QJsonObject configuration(const QString &account, qint64 capturedAt,
                          const QString &rawKey = QStringLiteral("website-key-1"),
                          const QString &name = QStringLiteral("Primary"),
                          const QString &groupName = QStringLiteral("Codex"))
{
    QJsonArray apiKeys{
        QJsonObject{
            { QStringLiteral("id"), rawKey },
            { QStringLiteral("name"), name },
            { QStringLiteral("status"), QStringLiteral("active") },
            { QStringLiteral("group"), QJsonObject{
                { QStringLiteral("name"), groupName },
                { QStringLiteral("platform"), QStringLiteral("openai") },
            } },
        },
    };
    QString error;
    const QJsonObject projection = CompanionConfigProjection::fromWebsiteApiKeys(
        apiKeys, account, QStringLiteral("https://aegisy.cc"), capturedAt, &error);
    if (projection.isEmpty()) std::cerr << error.toStdString() << '\n';
    return projection;
}

QJsonObject modelProjection(const QString &keyIdentity)
{
    QString error;
    return CompanionModelProjection::fromProviderResponse(
        keyIdentity,
        QJsonObject{{QStringLiteral("data"), QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-5.6")}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("gpt-5.6-mini")}},
        }}}, &error);
}

QString projectionSha(const QJsonObject &configurationProjection)
{
    return configurationProjection.value(QStringLiteral("projection_sha256")).toString();
}

QString configKeyIdentity(const QJsonObject &configurationProjection)
{
    return configurationProjection.value(QStringLiteral("keys")).toArray().first()
        .toObject().value(QStringLiteral("key_identity")).toString();
}

bool reset(QSettings *settings, FakeSecureStore *secure)
{
    settings->clear();
    settings->sync();
    secure->values.clear();
    secure->scripts.clear();
    secure->writeCalls = 0;
    secure->unavailableReads = 0;
    secure->invalidReads = 0;
    secure->unavailable = false;
    secure->invalid = false;
    return settings->status() == QSettings::NoError;
}

bool fixedCanonicalAndHmacVectors()
{
    const QJsonObject vector{
        { QStringLiteral("z"), QJsonArray{true, QJsonValue::Null} },
        { QStringLiteral("b"), 2 },
        { QStringLiteral("a"), QStringLiteral("x") },
    };
    const QByteArray expectedCanonical =
        QByteArrayLiteral("{\"a\":\"x\",\"b\":2,\"z\":[true,null]}");
    QByteArray key;
    for (int value = 0; value < 32; ++value) {
        key.append(static_cast<char>(value));
    }
    return require(canonical(vector) == expectedCanonical,
                   "fixed canonical JSON vector drifted")
        && require(mac(key, expectedCanonical).toHex()
                       == QByteArrayLiteral(
                           "5ff1389cc6708107a5ee9a0df431cd592"
                           "74670992fefe574813169da52a56f62"),
                   "fixed HMAC-SHA256 vector drifted");
}

bool baseLifecycle(QSettings *settings, FakeSecureStore *secure)
{
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Empty,
                 "missing state was not Empty")) return false;
    secure->unavailable = true;
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Unavailable,
                 "unavailable secure store was confused with missing")) return false;
    secure->unavailable = false;
    secure->invalid = true;
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "invalid secure store was not Invalid")) return false;
    secure->invalid = false;

    settings->setValue(legacyKey(kAccount), QByteArray("legacy-unverified"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::LegacyUnverified,
                 "legacy state was not explicitly unverified")
            || !require(secure->values.isEmpty(), "legacy cache was re-signed")) {
        return false;
    }

    const QJsonObject original = configuration(kAccount, kNow);
    QHash<QString, QString> handles;
    handles.insert(original.value(QStringLiteral("keys")).toArray().first()
                       .toObject().value(QStringLiteral("key_identity")).toString(),
                   QStringLiteral("website-credential:sha256:")
                       + QString(64, QLatin1Char('d')));
    QString projectionError;
    const QJsonObject withHandle = CompanionConfigProjection::withCredentialHandles(
        original, handles, &projectionError);
    QString error;
    if (!require(cache.commitLiveConfiguration(kAccount, withHandle, kNow, &error),
                 "initial configuration commit failed")) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }
    if (!require(!settings->contains(legacyKey(kAccount)),
                 "successful v2 commit did not remove exact legacy v1 key")
            || !require(cache.lastWarning().isEmpty(),
                        "successful legacy cleanup reported a warning")) {
        return false;
    }
    const CompanionConfigurationCacheView fresh = cache.view(kAccount, kNow);
    if (!require(fresh.state == CompanionConfigurationCacheState::Fresh,
                 "committed cache was not Fresh")
            || !require(fresh.revision == 1, "first revision was not one")
            || !require(!fresh.configurationAuthority
                            && !fresh.configurationApplied
                            && !fresh.modelSelectionAuthority,
                        "cache view granted authority")) {
        return false;
    }
    const QByteArray persisted = settings->value(slotKey(kAccount,
        object(secure->values.constBegin().value())
            .value(QStringLiteral("committed")).toObject()
            .value(QStringLiteral("slot")).toString())).toByteArray();
    if (!require(!persisted.contains("credential_handle")
                     && !persisted.contains("website-credential")
                     && !persisted.contains("credential_value")
                     && !persisted.contains("raw_id"),
                 "cache payload retained a credential handle/value/raw id")) {
        return false;
    }
    if (!require(persisted.contains(
                     "aegisy-companion-configuration-cache-envelope/0.2")
                     && persisted.contains(
                         "aegisy-companion-configuration-cache-payload/0.2")
                     && persisted.contains(
                         "aegisy-companion-cached-configuration/0.2"),
                 "persisted cache did not use explicit 0.2 schemas")) return false;
    const QJsonObject authority = object(secure->values.constBegin().value());
    const QByteArray key = authority.value(QStringLiteral("hmac_key_base64"))
        .toString().toLatin1();
    if (!require(QByteArray::fromBase64(key).size() == 32
                     && QByteArray::fromBase64(key).toBase64() == key,
                 "authority HMAC key was not canonical Base64 32-byte data")) {
        return false;
    }
    settings->setValue(legacyKey(kAccount), QByteArray("cleanup-failure-evidence"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Fresh,
                 "authenticated v2 cache fell back to remaining legacy evidence")) {
        return false;
    }
    return true;
}

bool modelAndTimeLifecycle(QSettings *settings, FakeSecureStore *secure)
{
    if (!reset(settings, secure)) return false;
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    const QJsonObject config = configuration(kAccount, kNow);
    QString error;
    if (!cache.commitLiveConfiguration(kAccount, config, kNow, &error)) return false;
    if (!require(!cache.mergeWebsiteModels(
                     kAccount, projectionSha(config), QStringLiteral("anthropic"),
                     modelProjection(configKeyIdentity(config)),
                     kNow + 1, kNow + 1, &error),
                 "model result accepted a mismatched platform binding")) return false;
    if (!cache.mergeWebsiteModels(
                kAccount, projectionSha(config), QStringLiteral("openai"),
                modelProjection(configKeyIdentity(config)),
                kNow + 1, kNow + 1, &error)) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }
    CompanionConfigurationCacheView view = cache.view(kAccount, kNow + 1);
    if (!require(view.models.size() == 1,
                 "exact website model result was not cached")
            || !require(view.models.first().toObject()
                            .value(QStringLiteral("platform")).toString()
                            == QStringLiteral("openai"),
                        "model cache row omitted its exact platform binding")) {
        return false;
    }
    const QByteArray viewBytes = canonical(QJsonObject{
        {QStringLiteral("configuration"), view.configuration},
        {QStringLiteral("models"), view.models},
    });
    if (!require(!viewBytes.contains("credential_handle")
                     && !viewBytes.contains("credential_value")
                     && !viewBytes.contains("raw_id"),
                 "operational view exposed a handle/value/raw id")) return false;

    const QJsonObject localModels = modelProjection(
        QStringLiteral("local-profile:sha256:") + QString(64, QLatin1Char('e')));
    if (!require(!cache.mergeWebsiteModels(
                     kAccount, projectionSha(config), QStringLiteral("openai"),
                     localModels,
                     kNow + 2, kNow + 2, &error),
                 "local profile model result entered website cache")) return false;
    QString secretError;
    const QJsonObject secretModels = CompanionModelProjection::fromProviderResponse(
        configKeyIdentity(config),
        QJsonObject{{QStringLiteral("data"), QJsonArray{
            QJsonObject{{QStringLiteral("id"),
                         QStringLiteral("github_pat_12345678901234567890")}},
        }}}, &secretError);
    if (!require(!secretModels.isEmpty(),
                 "secret-shaped model fixture was rejected before cache validation")
            || !require(!cache.mergeWebsiteModels(
                            kAccount, projectionSha(config),
                            QStringLiteral("openai"), secretModels,
                            kNow + 3, kNow + 3, &error),
                        "secret-shaped model ID entered the cache")) return false;

    view = cache.view(kAccount,
        kNow + CompanionConfigurationCache::ModelFreshMs + 2);
    if (!require(view.state == CompanionConfigurationCacheState::Fresh
                     && view.models.isEmpty(),
                 "expired model remained available past six hours")) return false;
    view = cache.view(kAccount,
        kNow + CompanionConfigurationCache::ConfigurationFreshMs);
    if (!require(view.state == CompanionConfigurationCacheState::Stale
                     && view.models.isEmpty(),
                 "configuration did not become status-only stale at 24h")) return false;
    view = cache.view(kAccount,
        kNow + CompanionConfigurationCache::ConfigurationFreshMs
             + CompanionConfigurationCache::ConfigurationStaleMs);
    return require(view.state == CompanionConfigurationCacheState::Expired
                       && view.configuration.isEmpty() && view.models.isEmpty(),
                   "configuration did not expire after stale retention");
}

bool modelInvalidation(QSettings *settings, FakeSecureStore *secure)
{
    if (!reset(settings, secure)) return false;
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    const QJsonObject first = configuration(kAccount, kNow);
    QString error;
    if (!cache.commitLiveConfiguration(kAccount, first, kNow, &error)
            || !cache.mergeWebsiteModels(
                kAccount, projectionSha(first), QStringLiteral("openai"),
                modelProjection(configKeyIdentity(first)),
                kNow + 1, kNow + 1, &error)) return false;
    if (!require(cache.commitLiveConfiguration(kAccount, first, kNow + 2, &error),
                 "exact configuration observation replay failed")
            || !require(cache.view(kAccount, kNow + 2).models.size() == 1,
                        "exact configuration observation discarded fresh models")) {
        return false;
    }
    const QJsonObject sameContentNewObservation = configuration(kAccount, kNow + 3);
    if (!require(cache.commitLiveConfiguration(
                     kAccount, sameContentNewObservation, kNow + 3, &error),
                 "same-content new observation commit failed")
            || !require(cache.view(kAccount, kNow + 3).models.isEmpty(),
                        "new configuration observation SHA retained old models")) {
        return false;
    }
    if (!cache.mergeWebsiteModels(
            kAccount, projectionSha(sameContentNewObservation),
            QStringLiteral("openai"),
            modelProjection(configKeyIdentity(sameContentNewObservation)),
            kNow + 4, kNow + 4, &error)) return false;
    const QJsonObject changed = configuration(
        kAccount, kNow + 5, QStringLiteral("website-key-2"),
        QStringLiteral("Replacement"));
    if (!require(cache.commitLiveConfiguration(kAccount, changed, kNow + 5, &error),
                 "changed configuration commit failed")
            || !require(cache.view(kAccount, kNow + 5).models.isEmpty(),
                        "configuration change did not invalidate models")) return false;
    return require(!cache.mergeWebsiteModels(
                       kAccount, projectionSha(sameContentNewObservation),
                       QStringLiteral("openai"),
                       modelProjection(configKeyIdentity(sameContentNewObservation)),
                       kNow + 6, kNow + 6, &error),
                   "stale configuration observation accepted a model result");
}

bool tamperMatrix(QSettings *settings, FakeSecureStore *secure)
{
    QString error;
    auto create = [&]() {
        reset(settings, secure);
        CompanionConfigurationCache cache(secure, settings, lockPath(settings));
        return cache.commitLiveConfiguration(
            kAccount, configuration(kAccount, kNow), kNow, &error);
    };

    if (!create()) return false;
    QByteArray &authorityBytes = secure->values[secure->onlyScope()];
    QJsonObject authority = object(authorityBytes);
    const QString active = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    QByteArray bytes = settings->value(slotKey(kAccount, active)).toByteArray();
    bytes[bytes.size() / 2] = bytes.at(bytes.size() / 2) == 'a' ? 'b' : 'a';
    settings->setValue(slotKey(kAccount, active), bytes);
    settings->sync();
    CompanionConfigurationCache cache1(secure, settings, lockPath(settings));
    if (!require(cache1.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "MAC/ciphertext tamper was accepted")) return false;

    if (!create()) return false;
    authorityBytes = secure->values[secure->onlyScope()];
    authority = object(authorityBytes);
    const QString active2 = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    QJsonObject envelope = object(
        settings->value(slotKey(kAccount, active2)).toByteArray());
    QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
    QJsonObject cachedConfig = payload.value(QStringLiteral("configuration")).toObject();
    QJsonArray keys = cachedConfig.value(QStringLiteral("keys")).toArray();
    QJsonObject key = keys.first().toObject();
    key.insert(QStringLiteral("display_name"), QStringLiteral("SHA recomputed"));
    keys[0] = key;
    cachedConfig.insert(QStringLiteral("keys"), keys);
    payload.insert(QStringLiteral("configuration"), cachedConfig);
    payload.insert(QStringLiteral("content_sha256"), digest(canonical(cachedConfig)));
    envelope.insert(QStringLiteral("payload"), payload);
    envelope.insert(QStringLiteral("payload_sha256"), digest(canonical(payload)));
    settings->setValue(slotKey(kAccount, active2), canonical(envelope));
    settings->sync();
    CompanionConfigurationCache cache2(secure, settings, lockPath(settings));
    if (!require(cache2.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "ordinary SHA recomputation bypassed HMAC/anchor")) return false;

    if (!create()) return false;
    authorityBytes = secure->values[secure->onlyScope()];
    authority = object(authorityBytes);
    const QString originalSlot = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    const QString otherSlot = originalSlot == QStringLiteral("a")
        ? QStringLiteral("b") : QStringLiteral("a");
    envelope = object(settings->value(slotKey(kAccount, originalSlot)).toByteArray());
    envelope.insert(QStringLiteral("slot"), otherSlot);
    QJsonObject unsignedEnvelope = envelope;
    unsignedEnvelope.remove(QStringLiteral("mac"));
    const QByteArray hmacKey = QByteArray::fromBase64(
        authority.value(QStringLiteral("hmac_key_base64")).toString().toLatin1());
    envelope.insert(QStringLiteral("mac"),
                    QString::fromLatin1(mac(hmacKey, canonical(unsignedEnvelope)).toBase64()));
    settings->setValue(slotKey(kAccount, otherSlot), canonical(envelope));
    settings->sync();
    CompanionConfigurationCache cache3(secure, settings, lockPath(settings));
    if (!require(cache3.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "same revision drift was accepted")) return false;

    reset(settings, secure);
    CompanionConfigurationCache rollbackCache(
        secure, settings, lockPath(settings));
    const QJsonObject rollbackFirst = configuration(kAccount, kNow);
    const QJsonObject rollbackSecond = configuration(kAccount, kNow + 1);
    if (!rollbackCache.commitLiveConfiguration(
            kAccount, rollbackFirst, kNow, &error)
            || !rollbackCache.commitLiveConfiguration(
                kAccount, rollbackSecond, kNow + 1, &error)) return false;
    authority = object(secure->values.value(secure->onlyScope()));
    const QString rollbackActive = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    const QString rollbackPrevious = rollbackActive == QStringLiteral("a")
        ? QStringLiteral("b") : QStringLiteral("a");
    settings->remove(slotKey(kAccount, rollbackPrevious));
    settings->sync();
    if (!require(rollbackCache.view(kAccount, kNow + 1).state
                     == CompanionConfigurationCacheState::Invalid,
                 "missing exact previous slot was accepted")) return false;

    reset(settings, secure);
    CompanionConfigurationCache activeRollbackCache(
        secure, settings, lockPath(settings));
    if (!activeRollbackCache.commitLiveConfiguration(
            kAccount, rollbackFirst, kNow, &error)
            || !activeRollbackCache.commitLiveConfiguration(
                kAccount, rollbackSecond, kNow + 1, &error)) return false;
    authority = object(secure->values.value(secure->onlyScope()));
    const QString activeRollbackSlot = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    const QString previousRollbackSlot = activeRollbackSlot == QStringLiteral("a")
        ? QStringLiteral("b") : QStringLiteral("a");
    settings->setValue(slotKey(kAccount, activeRollbackSlot),
                       settings->value(slotKey(kAccount, previousRollbackSlot)));
    settings->sync();
    if (!require(activeRollbackCache.view(kAccount, kNow + 1).state
                     == CompanionConfigurationCacheState::Invalid,
                 "active slot revision rollback was accepted")) return false;

    if (!create()) return false;
    const QString scope = secure->onlyScope();
    const QByteArray accountAuthority = secure->values.value(scope);
    settings->setValue(slotKey(kOtherAccount, QStringLiteral("a")),
                       settings->value(slotKey(kAccount, QStringLiteral("a"))));
    secure->values.insert(
        QStringLiteral("companion/configuration-cache-authority/v1/")
            + QString(64, QLatin1Char('c')),
        accountAuthority);
    settings->sync();
    CompanionConfigurationCache cache4(secure, settings, lockPath(settings));
    return require(cache4.view(kOtherAccount, kNow).state
                       == CompanionConfigurationCacheState::Invalid,
                   "cross-account authority/slot copy was accepted");
}

bool preparedRecovery(QSettings *settings, FakeSecureStore *secure)
{
    QString error;
    reset(settings, secure);
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    const QJsonObject first = configuration(kAccount, kNow);
    if (!cache.commitLiveConfiguration(kAccount, first, kNow, &error)) return false;

    const int finalWrite = secure->writeCalls + 2;
    secure->scripts.append({finalWrite,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::DefiniteFailure,
                            false, 0});
    const QJsonObject second = configuration(kAccount, kNow + 1);
    if (!require(!cache.commitLiveConfiguration(
                     kAccount, second, kNow + 1, &error),
                 "definite finalization failure reported success")) return false;
    const QJsonObject preparedAuthority = object(
        secure->values.value(secure->onlyScope()));
    if (!require(preparedAuthority.value(QStringLiteral("phase")).toString()
                     == QStringLiteral("prepared"),
                 "failed finalization did not retain Prepared authority")) return false;
    secure->scripts.clear();
    CompanionConfigurationCacheView recovered = cache.view(kAccount, kNow + 1);
    if (!require(recovered.state == CompanionConfigurationCacheState::Fresh
                     && recovered.revision == 2,
                 "Prepared candidate was not finalized on recovery")) return false;

    reset(settings, secure);
    CompanionConfigurationCache abortCache(
        secure, settings, lockPath(settings));
    if (!abortCache.commitLiveConfiguration(kAccount, first, kNow, &error)) return false;
    const QString scope = secure->onlyScope();
    QJsonObject authority = object(secure->values.value(scope));
    const QJsonObject committed = authority.value(QStringLiteral("committed")).toObject();
    const QString target = committed.value(QStringLiteral("slot")).toString()
        == QStringLiteral("a") ? QStringLiteral("b") : QStringLiteral("a");
    authority.insert(QStringLiteral("phase"), QStringLiteral("prepared"));
    authority.insert(QStringLiteral("highest_reserved_revision"), 2);
    authority.insert(QStringLiteral("prepared"), QJsonObject{
        { QStringLiteral("target_slot"), target },
        { QStringLiteral("reserved_revision"), 2 },
        { QStringLiteral("target_preimage_envelope_sha256"), QJsonValue::Null },
        { QStringLiteral("candidate_envelope_sha256"), QString(64, QLatin1Char('d')) },
        { QStringLiteral("candidate_payload_sha256"), QString(64, QLatin1Char('e')) },
        { QStringLiteral("high_water_ms"), kNow + 1 },
    });
    secure->values[scope] = canonical(authority);
    recovered = abortCache.view(kAccount, kNow + 1);
    if (!require(recovered.state == CompanionConfigurationCacheState::Fresh
                     && recovered.revision == 1,
                 "Prepared preimage was not safely aborted")) return false;
    authority = object(secure->values.value(scope));
    if (!require(authority.value(QStringLiteral("highest_reserved_revision")).toInt() == 2,
                 "aborted reservation lost highest reserved revision")) return false;

    const QJsonObject afterAbort = configuration(kAccount, kNow + 2);
    if (!require(abortCache.commitLiveConfiguration(
                     kAccount, afterAbort, kNow + 2, &error),
                 "commit after Prepared abort failed")
            || !require(abortCache.view(kAccount, kNow + 2).revision == 3,
                        "aborted reserved revision was reused")) return false;

    authority = object(secure->values.value(scope));
    const QString activeAfterAbort = authority.value(QStringLiteral("committed"))
        .toObject().value(QStringLiteral("slot")).toString();
    const QString thirdTarget = activeAfterAbort == QStringLiteral("a")
        ? QStringLiteral("b") : QStringLiteral("a");
    const QByteArray thirdPreimage = settings->value(
        slotKey(kAccount, thirdTarget)).toByteArray();
    authority.insert(QStringLiteral("phase"), QStringLiteral("prepared"));
    authority.insert(QStringLiteral("highest_reserved_revision"), 4);
    authority.insert(QStringLiteral("prepared"), QJsonObject{
        { QStringLiteral("target_slot"), thirdTarget },
        { QStringLiteral("reserved_revision"), 4 },
        { QStringLiteral("target_preimage_envelope_sha256"), digest(thirdPreimage) },
        { QStringLiteral("candidate_envelope_sha256"), QString(64, QLatin1Char('f')) },
        { QStringLiteral("candidate_payload_sha256"), QString(64, QLatin1Char('1')) },
        { QStringLiteral("high_water_ms"), kNow + 3 },
    });
    secure->values[scope] = canonical(authority);
    settings->setValue(slotKey(kAccount, thirdTarget), QByteArray("third-state"));
    settings->sync();
    if (!require(abortCache.view(kAccount, kNow + 3).state
                     == CompanionConfigurationCacheState::Invalid,
                 "Prepared target third state was not Invalid")) return false;

    secure->values.remove(scope);
    return require(abortCache.view(kAccount, kNow + 3).state
                       == CompanionConfigurationCacheState::Invalid,
                   "slot without authority was auto-promoted");
}

bool staleSettingsPreparedRecovery(QSettings *settings, FakeSecureStore *secure)
{
    QString error;
    reset(settings, secure);
    CompanionConfigurationCache reader(
        secure, settings, lockPath(settings));
    if (!reader.commitLiveConfiguration(
            kAccount, configuration(kAccount, kNow), kNow, &error)) return false;

    QSettings writerSettings(settings->fileName(), QSettings::IniFormat);
    CompanionConfigurationCache writer(
        secure, &writerSettings, lockPath(&writerSettings));
    secure->scripts.append({secure->writeCalls + 2,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::DefiniteFailure,
                            false, 0});
    if (!require(!writer.commitLiveConfiguration(
                     kAccount, configuration(kAccount, kNow + 1),
                     kNow + 1, &error),
                 "stale-settings Prepared fixture unexpectedly committed")) return false;
    secure->scripts.clear();
    const CompanionConfigurationCacheView recovered = reader.view(
        kAccount, kNow + 1);
    return require(recovered.state == CompanionConfigurationCacheState::Fresh
                       && recovered.revision == 2,
                   "locked QSettings sync did not observe and finalize Prepared candidate");
}

bool outcomeAndClock(QSettings *settings, FakeSecureStore *secure)
{
    QString error;
    reset(settings, secure);
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    const int finalize = 3;
    secure->scripts.append({finalize,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::OutcomeUnknown,
                            true, 1});
    if (!require(!cache.commitLiveConfiguration(
                     kAccount, configuration(kAccount, kNow), kNow, &error),
                 "unknown finalization outcome reported success")) return false;
    secure->unavailable = true;
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::OutcomeUnknown,
                 "unknown write outcome was not surfaced")) return false;
    secure->unavailable = false;
    secure->scripts.clear();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Fresh,
                 "outcome-unknown Prepared candidate did not recover")) return false;

    reset(settings, secure);
    CompanionConfigurationCache invalidPriority(
        secure, settings, lockPath(settings));
    secure->scripts.append({3,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::OutcomeUnknown,
                            false, 0, 1});
    if (!require(!invalidPriority.commitLiveConfiguration(
                     kAccount, configuration(kAccount, kNow), kNow, &error),
                 "Invalid fresh read after unknown write reported success")
            || !require(error == QStringLiteral("cache-authority-write-drift"),
                        "Invalid fresh read was downgraded to outcome-unknown")) {
        return false;
    }

    reset(settings, secure);
    CompanionConfigurationCache highWaterCache(
        secure, settings, lockPath(settings));
    const QJsonObject current = configuration(kAccount, kNow);
    if (!highWaterCache.commitLiveConfiguration(kAccount, current, kNow, &error)) {
        return false;
    }
    secure->scripts.append({secure->writeCalls + 1,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::DefiniteFailure,
                            false, 0});
    if (!require(highWaterCache.view(kAccount, kNow + 10).state
                     == CompanionConfigurationCacheState::Unavailable,
                 "definite high-water write failure was not unavailable")) return false;
    secure->scripts.clear();
    if (!require(highWaterCache.view(kAccount, kNow + 10).state
                     == CompanionConfigurationCacheState::Fresh,
                 "high-water retry did not recover")) return false;

    secure->scripts.append({secure->writeCalls + 1,
                            CompanionConfigurationCacheSecureStore::WriteOutcome::OutcomeUnknown,
                            true, 1});
    if (!require(highWaterCache.view(kAccount, kNow + 20).state
                     == CompanionConfigurationCacheState::OutcomeUnknown,
                 "unknown high-water write was not OutcomeUnknown")) return false;
    secure->scripts.clear();
    CompanionConfigurationCache reopened(
        secure, settings, lockPath(settings));
    if (!require(reopened.view(kAccount, kNow + 20).state
                     == CompanionConfigurationCacheState::Fresh,
                 "reopen did not resolve committed unknown high-water write")) return false;
    if (!require(reopened.view(kAccount, kNow + 100).state
                     == CompanionConfigurationCacheState::Fresh,
                 "high-water advance failed")) return false;

    const QJsonObject beforeHighWater = configuration(kAccount, kNow + 50);
    if (!require(!reopened.commitLiveConfiguration(
                     kAccount, beforeHighWater, kNow + 101, &error),
                 "configuration observation below high-water was accepted")
            || !require(!reopened.mergeWebsiteModels(
                            kAccount, projectionSha(current),
                            QStringLiteral("openai"),
                            modelProjection(configKeyIdentity(current)),
                            kNow + 50, kNow + 101, &error),
                        "model observation below high-water was accepted")) {
        return false;
    }
    if (!require(reopened.view(kAccount, kNow + 50).state
                     == CompanionConfigurationCacheState::Invalid,
                 "clock rollback was accepted")) return false;

    QSettings expirySettings(settings->fileName(), QSettings::IniFormat);
    CompanionConfigurationCache expiryCache(
        secure, &expirySettings, lockPath(&expirySettings));
    return require(expiryCache.view(
                       kAccount,
                       kNow + CompanionConfigurationCache::ConfigurationFreshMs
                           + CompanionConfigurationCache::ConfigurationStaleMs)
                       .state == CompanionConfigurationCacheState::Expired,
                   "expired state did not survive reopen");
}

bool partialAndCanonical(QSettings *settings, FakeSecureStore *secure)
{
    QString error;
    reset(settings, secure);
    CompanionConfigurationCache cache(secure, settings, lockPath(settings));
    if (!cache.commitLiveConfiguration(
            kAccount, configuration(kAccount, kNow), kNow, &error)) return false;
    const QString scope = secure->onlyScope();
    QByteArray authority = secure->values.value(scope);
    authority.prepend(' ');
    secure->values[scope] = authority;
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "non-canonical authority was accepted")) return false;

    reset(settings, secure);
    CompanionConfigurationCache malformedCache(
        secure, settings, lockPath(settings));
    if (!malformedCache.commitLiveConfiguration(
            kAccount, configuration(kAccount, kNow), kNow, &error)) return false;
    const QString malformedScope = secure->onlyScope();
    QJsonObject malformed = object(secure->values.value(malformedScope));
    malformed.remove(QStringLiteral("hmac_key_base64"));
    secure->values[malformedScope] = canonical(malformed);
    if (!require(malformedCache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "authority with missing key field was accepted")) return false;

    reset(settings, secure);
    settings->setValue(slotKey(kAccount, QStringLiteral("a")), QByteArray("orphan"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "slot-only partial state was not Invalid")) return false;

    reset(settings, secure);
    settings->setValue(cacheBase(kAccount) + QStringLiteral("/slot_c"),
                       QByteArray("unknown-reserved-entry"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "unknown account-cache namespace entry was accepted")) return false;

    reset(settings, secure);
    settings->setValue(cacheBase(kAccount), QByteArray("reserved-prefix-value"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "exact reserved account-cache prefix was accepted")) return false;

    reset(settings, secure);
    settings->setValue(slotKey(kAccount, QStringLiteral("a")),
                       QStringLiteral("wrong-type"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "wrong-type reserved slot was accepted")) return false;

    reset(settings, secure);
    settings->setValue(legacyKey(kAccount), QStringLiteral("wrong-type-legacy"));
    settings->sync();
    if (!require(cache.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Invalid,
                 "wrong-type legacy evidence was treated as unverified legacy")) {
        return false;
    }

    reset(settings, secure);
    CompanionConfigurationCache invalidLock(
        secure, settings, QStringLiteral("relative-cache.lock"));
    if (!require(invalidLock.view(kAccount, kNow).state
                     == CompanionConfigurationCacheState::Unavailable,
                 "relative lock path was accepted")) return false;

    reset(settings, secure);
    QLockFile held(lockPath(settings));
    held.setStaleLockTime(30000);
    if (!require(held.tryLock(), "test could not acquire cross-process lock")) return false;
    const CompanionConfigurationCacheView locked = cache.view(kAccount, kNow);
    held.unlock();
    return require(locked.state == CompanionConfigurationCacheState::Unavailable,
                   "cross-process lock contention was not fail-closed");
}

bool crashedLockRecovery(QSettings *settings, FakeSecureStore *secure)
{
    reset(settings, secure);
    const QString path = lockPath(settings);
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                { QStringLiteral("--crash-with-cache-lock"), path });
    if (!require(child.waitForStarted(5000),
                 "cache-lock crash child did not start")
            || !require(child.waitForFinished(5000),
                        "cache-lock crash child did not finish")
            || !require(child.exitStatus() == QProcess::NormalExit
                            && child.exitCode() == 0,
                        "cache-lock crash child failed")
            || !require(QFile::exists(path),
                        "cache-lock crash child did not leave lock evidence")) {
        return false;
    }
    CompanionConfigurationCache cache(secure, settings, path);
    const CompanionConfigurationCacheView view = cache.view(kAccount, kNow);
    return require(view.state == CompanionConfigurationCacheState::Empty,
                   "dead-owner cache lock did not recover through PID/host policy")
        && require(!QFile::exists(path),
                   "recovered cache lock evidence was not released");
}

bool preBootstrapValidationAndCleanupWarning(
    QSettings *settings, FakeSecureStore *secure)
{
    auto requireLegacyPreserved = [&](const QJsonObject &projection,
                                      qint64 nowMs,
                                      const char *message) {
        if (!reset(settings, secure)) return false;
        settings->setValue(legacyKey(kAccount), QByteArray("legacy-evidence"));
        settings->sync();
        CompanionConfigurationCache cache(
            secure, settings, lockPath(settings));
        QString error;
        return require(!projection.isEmpty(), "invalid projection test fixture")
            && require(!cache.commitLiveConfiguration(
                           kAccount, projection, nowMs, &error), message)
            && require(secure->values.isEmpty(),
                       "pure validation failure bootstrapped empty authority")
            && require(cache.view(kAccount, nowMs).state
                           == CompanionConfigurationCacheState::LegacyUnverified,
                       "pure validation failure replaced legacy-only state");
    };

    const QJsonObject secretDisplay = configuration(
        kAccount, kNow, QStringLiteral("website-key-display"),
        QStringLiteral("github_pat_123456789012345678901234"));
    if (!requireLegacyPreserved(secretDisplay, kNow,
                                "secret-shaped display name entered cache")) {
        return false;
    }
    const QJsonObject secretGroup = configuration(
        kAccount, kNow, QStringLiteral("website-key-group"),
        QStringLiteral("Primary"),
        QStringLiteral("xoxb-123456789012345678901234"));
    if (!requireLegacyPreserved(secretGroup, kNow,
                                "secret-shaped group label entered cache")) {
        return false;
    }
    constexpr qint64 nearMaximumJsonTime = 9007199254740000LL;
    if (!requireLegacyPreserved(
            configuration(kAccount, nearMaximumJsonTime), nearMaximumJsonTime,
            "overflowing cache validity window was accepted")) {
        return false;
    }

    if (!reset(settings, secure)) return false;
    settings->setValue(legacyKey(kAccount), QByteArray("legacy-cleanup-evidence"));
    settings->sync();
    FailingLegacyCleaner cleaner;
    CompanionConfigurationCache cache(
        secure, settings, lockPath(settings), &cleaner);
    QString error;
    if (!require(cache.commitLiveConfiguration(
                     kAccount, configuration(kAccount, kNow), kNow, &error),
                 "legacy cleanup failure revoked v2 commit")
            || !require(error.isEmpty(),
                        "legacy cleanup warning replaced primary success")
            || !require(cache.lastWarning()
                            == QStringLiteral("fake-legacy-cleanup-failed"),
                        "legacy cleanup failure was not visible")
            || !require(settings->contains(legacyKey(kAccount)),
                        "failing cleaner removed legacy evidence")) {
        return false;
    }
    QSettings reopenedSettings(settings->fileName(), QSettings::IniFormat);
    CompanionConfigurationCache reopened(
        secure, &reopenedSettings, lockPath(&reopenedSettings));
    return require(reopened.view(kAccount, kNow).state
                       == CompanionConfigurationCacheState::Fresh,
                   "v2 cache fell back after visible legacy cleanup failure");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc == 3
            && QString::fromLocal8Bit(argv[1])
                == QStringLiteral("--crash-with-cache-lock")) {
        QLockFile lock(QString::fromLocal8Bit(argv[2]));
        lock.setStaleLockTime(30000);
        if (!lock.tryLock(5000)) return 2;
        std::_Exit(0);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("AegisyCacheTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AegisyCacheTest"));
    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary directory unavailable")) return 1;
    QSettings settings(directory.filePath(QStringLiteral("cache.ini")),
                       QSettings::IniFormat);
    FakeSecureStore secure;

    if (!fixedCanonicalAndHmacVectors()
            || !baseLifecycle(&settings, &secure)
            || !modelAndTimeLifecycle(&settings, &secure)
            || !modelInvalidation(&settings, &secure)
            || !tamperMatrix(&settings, &secure)
            || !preparedRecovery(&settings, &secure)
            || !staleSettingsPreparedRecovery(&settings, &secure)
            || !outcomeAndClock(&settings, &secure)
            || !partialAndCanonical(&settings, &secure)
            || !crashedLockRecovery(&settings, &secure)
            || !preBootstrapValidationAndCleanupWarning(&settings, &secure)) {
        return 1;
    }
    return 0;
}
