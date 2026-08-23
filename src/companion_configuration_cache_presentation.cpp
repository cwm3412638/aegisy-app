#include "companion_configuration_cache_presentation.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kMaximumJsonInteger = 9007199254740991LL;
constexpr int kMaximumConfigurationBytes = 2 * 1024 * 1024;
constexpr int kMaximumKeys = 1000;
constexpr int kMaximumModelsPerKey = 1000;
constexpr int kMaximumModelIdBytes = 128;

const char kAccountPrefix[] = "website-account-session:sha256:";
const char kKeyPrefix[] = "website-key:sha256:";
const char kConfigurationSchema[] =
    "aegisy-companion-cached-configuration/0.2";
const char kPresentationProvenance[] =
    "aegisy-companion-cache-read-only/0.1";
const char kStateOnlyProvenance[] =
    "aegisy-companion-cache-state-only/0.1";

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool lowerHex(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a')
                     && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validIdentity(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix) && lowerHex(value.mid(prefix.size()));
}

bool validAccountIdentity(const QString &value)
{
    return validIdentity(value, QString::fromLatin1(kAccountPrefix));
}

bool validKeyIdentity(const QString &value)
{
    return validIdentity(value, QString::fromLatin1(kKeyPrefix));
}

bool validTimestamp(qint64 value)
{
    return value > 0 && value <= kMaximumJsonInteger;
}

bool safeInteger(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < 0
            || number > static_cast<double>(kMaximumJsonInteger)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

QByteArray quoted(const QString &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value})
        .toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

bool appendCanonical(const QJsonValue &value, QByteArray *output)
{
    switch (value.type()) {
    case QJsonValue::Null:
        output->append("null");
        return true;
    case QJsonValue::Bool:
        output->append(value.toBool() ? "true" : "false");
        return true;
    case QJsonValue::Double: {
        qint64 number = 0;
        if (!safeInteger(value, &number)) return false;
        output->append(QByteArray::number(number));
        return true;
    }
    case QJsonValue::String:
        output->append(quoted(value.toString()));
        return true;
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
            if (!appendCanonical(object.value(keys.at(index)), output)) {
                return false;
            }
        }
        output->append('}');
        return true;
    }
    case QJsonValue::Undefined:
        return false;
    }
    return false;
}

QByteArray canonical(const QJsonObject &object)
{
    QByteArray result;
    return appendCanonical(object, &result) ? result : QByteArray();
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool trustedWebsiteOrigin(const QString &value)
{
    const QUrl url(value);
    const QString host = url.host().toLower();
    return url.isValid()
        && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && (host == QStringLiteral("aegisy.cc")
            || host == QStringLiteral("www.aegisy.cc"))
        && (url.port(-1) == -1 || url.port(-1) == 443)
        && url.userInfo().isEmpty()
        && (url.path().isEmpty() || url.path() == QStringLiteral("/"))
        && !url.hasQuery() && url.fragment().isEmpty();
}

bool safeDisplayText(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    if (value.isEmpty() || bytes.size() > 128) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.category() == QChar::Other_Surrogate
                || (character.category() == QChar::Other_Control
                    && character != QLatin1Char('\n')
                    && character != QLatin1Char('\t'))) {
            return false;
        }
    }
    const QString lower = value.toLower();
    if (lower.contains(QStringLiteral("bearer "))
            || lower.contains(QStringLiteral("api_key="))
            || lower.contains(QStringLiteral("api-key="))
            || lower.contains(QStringLiteral("access_token="))
            || lower.contains(QStringLiteral("credential="))
            || lower.contains(QStringLiteral("raw_id="))
            || lower.contains(QStringLiteral("raw-id="))) {
        return false;
    }
    for (const QString &part : value.split(
             QRegularExpression(QStringLiteral("\\s+")))) {
        const QString lowerPart = part.toLower();
        if ((lowerPart.startsWith(QStringLiteral("sk-")) && part.size() >= 12)
                || (lowerPart.startsWith(QStringLiteral("ghp_"))
                    && part.size() >= 20)
                || (lowerPart.startsWith(QStringLiteral("github_pat_"))
                    && part.size() >= 24)
                || (lowerPart.startsWith(QStringLiteral("xoxb-"))
                    && part.size() >= 20)
                || (part.count(QLatin1Char('.')) == 2 && part.size() >= 24)) {
            return false;
        }
    }
    return true;
}

bool validModelId(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    if (value.isEmpty() || bytes.size() > kMaximumModelIdBytes) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.isSpace()
                || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    const QString lower = value.toLower();
    return !lower.contains(QStringLiteral("bearer"))
        && !lower.contains(QStringLiteral("api_key"))
        && !lower.contains(QStringLiteral("api-key"))
        && !lower.contains(QStringLiteral("credential"))
        && !lower.contains(QStringLiteral("access_token"))
        && !lower.contains(QStringLiteral("access-token"))
        && !lower.contains(QStringLiteral("raw_id"))
        && !lower.contains(QStringLiteral("raw-id"))
        && !(lower.startsWith(QStringLiteral("sk-")) && value.size() >= 12)
        && !(lower.startsWith(QStringLiteral("ghp_")) && value.size() >= 20)
        && !(lower.startsWith(QStringLiteral("github_pat_"))
             && value.size() >= 20)
        && !(value.count(QLatin1Char('.')) == 2 && value.size() >= 24);
}

bool validErrorCode(const QString &value)
{
    if (value.isEmpty()) return true;
    static const QRegularExpression expression(
        QStringLiteral("^[a-z][a-z0-9-]{0,95}$"));
    return expression.match(value).hasMatch();
}

bool validDataMetadata(const CompanionConfigurationCacheView &view)
{
    return view.revision > 0 && view.revision <= kMaximumJsonInteger
        && validTimestamp(view.capturedAtMs)
        && validTimestamp(view.validUntilMs)
        && validTimestamp(view.staleUntilMs)
        && view.capturedAtMs
            <= kMaximumJsonInteger
                - CompanionConfigurationCache::ConfigurationFreshMs
        && view.validUntilMs
            == view.capturedAtMs
                + CompanionConfigurationCache::ConfigurationFreshMs
        && view.validUntilMs
            <= kMaximumJsonInteger
                - CompanionConfigurationCache::ConfigurationStaleMs
        && view.staleUntilMs
            == view.validUntilMs
                + CompanionConfigurationCache::ConfigurationStaleMs
        && lowerHex(view.sourceObservationSha256)
        && lowerHex(view.contentSha256)
        && view.errorCode.isEmpty();
}

bool validateConfiguration(
    const QJsonObject &configuration,
    const QString &expectedAccountIdentity,
    const QString &expectedContentSha256,
    QList<CompanionCachedKeyPresentation> *keys,
    QHash<QString, QString> *platformByKey)
{
    static const QSet<QString> configurationKeys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("source_origin"), QStringLiteral("key_count"),
        QStringLiteral("keys"),
    };
    static const QSet<QString> keyKeys{
        QStringLiteral("key_identity"), QStringLiteral("display_name"),
        QStringLiteral("group_label"), QStringLiteral("platform"),
        QStringLiteral("state"),
    };
    qint64 count = 0;
    const QJsonValue keysValue = configuration.value(QStringLiteral("keys"));
    const QJsonArray configurationKeysArray = keysValue.toArray();
    const QByteArray bytes = canonical(configuration);
    if (!exactKeys(configuration, configurationKeys)
            || configuration.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kConfigurationSchema)
            || configuration.value(QStringLiteral("account_identity")).toString()
                != expectedAccountIdentity
            || !trustedWebsiteOrigin(
                configuration.value(QStringLiteral("source_origin")).toString())
            || !keysValue.isArray()
            || !safeInteger(configuration.value(QStringLiteral("key_count")), &count)
            || count != configurationKeysArray.size()
            || configurationKeysArray.size() > kMaximumKeys
            || bytes.isEmpty() || bytes.size() > kMaximumConfigurationBytes
            || sha256(bytes) != expectedContentSha256) {
        return false;
    }

    QSet<QString> seen;
    for (const QJsonValue &value : configurationKeysArray) {
        if (!value.isObject()) return false;
        const QJsonObject key = value.toObject();
        const QString keyIdentity = key.value(QStringLiteral("key_identity")).toString();
        const QString displayName = key.value(QStringLiteral("display_name")).toString();
        const QString groupLabel = key.value(QStringLiteral("group_label")).toString();
        const QString platform = key.value(QStringLiteral("platform")).toString();
        const QString state = key.value(QStringLiteral("state")).toString();
        if (!exactKeys(key, keyKeys) || !validKeyIdentity(keyIdentity)
                || seen.contains(keyIdentity) || !safeDisplayText(displayName)
                || !safeDisplayText(groupLabel)
                || !QSet<QString>{QStringLiteral("openai"),
                                  QStringLiteral("anthropic"),
                                  QStringLiteral("gemini"),
                                  QStringLiteral("unknown")}.contains(platform)
                || !QSet<QString>{QStringLiteral("active"),
                                  QStringLiteral("inactive"),
                                  QStringLiteral("expired"),
                                  QStringLiteral("unknown")}.contains(state)) {
            return false;
        }
        seen.insert(keyIdentity);
        platformByKey->insert(keyIdentity, platform);
        keys->append(CompanionCachedKeyPresentation{
            keyIdentity, displayName, groupLabel, platform, state,
        });
    }
    return true;
}

bool validateModels(
    const QJsonArray &models,
    const QHash<QString, QString> &platformByKey,
    const CompanionConfigurationCacheView &view,
    qint64 nowMs,
    QList<CompanionCachedModelPresentation> *rows)
{
    static const QSet<QString> modelKeys{
        QStringLiteral("key_identity"), QStringLiteral("platform"),
        QStringLiteral("configuration_observation_sha256"),
        QStringLiteral("source_observation_sha256"),
        QStringLiteral("captured_at_ms"), QStringLiteral("valid_until_ms"),
        QStringLiteral("model_count"), QStringLiteral("models"),
    };
    if (models.size() > platformByKey.size()) return false;
    QSet<QString> seenKeys;
    for (const QJsonValue &value : models) {
        if (!value.isObject()) return false;
        const QJsonObject model = value.toObject();
        const QString keyIdentity = model.value(QStringLiteral("key_identity")).toString();
        const QString platform = model.value(QStringLiteral("platform")).toString();
        const QString configurationObservation = model.value(
            QStringLiteral("configuration_observation_sha256")).toString();
        const QString sourceObservation = model.value(
            QStringLiteral("source_observation_sha256")).toString();
        qint64 capturedAt = 0;
        qint64 validUntil = 0;
        qint64 count = 0;
        const QJsonValue modelListValue = model.value(QStringLiteral("models"));
        const QJsonArray modelList = modelListValue.toArray();
        if (!exactKeys(model, modelKeys)
                || !platformByKey.contains(keyIdentity)
                || seenKeys.contains(keyIdentity)
                || platformByKey.value(keyIdentity) != platform
                || (platform != QStringLiteral("openai")
                    && platform != QStringLiteral("anthropic")
                    && platform != QStringLiteral("gemini"))
                || configurationObservation != view.sourceObservationSha256
                || !lowerHex(sourceObservation)
                || !safeInteger(model.value(QStringLiteral("captured_at_ms")),
                                &capturedAt)
                || !safeInteger(model.value(QStringLiteral("valid_until_ms")),
                                &validUntil)
                || !safeInteger(model.value(QStringLiteral("model_count")), &count)
                || !modelListValue.isArray() || count != modelList.size()
                || modelList.size() > kMaximumModelsPerKey
                || capturedAt < view.capturedAtMs
                || capturedAt > nowMs
                || !validTimestamp(capturedAt) || !validTimestamp(validUntil)
                || capturedAt
                    > kMaximumJsonInteger
                        - CompanionConfigurationCache::ModelFreshMs
                || validUntil <= capturedAt
                || validUntil
                    > capturedAt + CompanionConfigurationCache::ModelFreshMs
                || validUntil > view.validUntilMs || nowMs >= validUntil) {
            return false;
        }
        QStringList modelIds;
        QSet<QString> seenModels;
        for (const QJsonValue &modelIdValue : modelList) {
            const QString modelId = modelIdValue.toString();
            if (!modelIdValue.isString() || !validModelId(modelId)
                    || seenModels.contains(modelId)) {
                return false;
            }
            seenModels.insert(modelId);
            modelIds.append(modelId);
        }
        seenKeys.insert(keyIdentity);
        rows->append(CompanionCachedModelPresentation{
            keyIdentity, platform, modelIds, capturedAt, validUntil,
            configurationObservation, sourceObservation,
        });
    }
    return true;
}

bool isErrorState(CompanionConfigurationCacheState state)
{
    switch (state) {
    case CompanionConfigurationCacheState::Invalid:
    case CompanionConfigurationCacheState::Empty:
    case CompanionConfigurationCacheState::LegacyUnverified:
    case CompanionConfigurationCacheState::Unavailable:
    case CompanionConfigurationCacheState::OutcomeUnknown:
    case CompanionConfigurationCacheState::RecoveryRequired:
        return true;
    case CompanionConfigurationCacheState::Fresh:
    case CompanionConfigurationCacheState::Stale:
    case CompanionConfigurationCacheState::Expired:
        return false;
    }
    return false;
}

bool knownState(CompanionConfigurationCacheState state)
{
    switch (state) {
    case CompanionConfigurationCacheState::Fresh:
    case CompanionConfigurationCacheState::Stale:
    case CompanionConfigurationCacheState::Expired:
    case CompanionConfigurationCacheState::Invalid:
    case CompanionConfigurationCacheState::Empty:
    case CompanionConfigurationCacheState::LegacyUnverified:
    case CompanionConfigurationCacheState::Unavailable:
    case CompanionConfigurationCacheState::OutcomeUnknown:
    case CompanionConfigurationCacheState::RecoveryRequired:
        return true;
    }
    return false;
}

} // namespace

bool CompanionConfigurationCachePresentationAdapter::build(
    const CompanionConfigurationCacheView &view,
    const QString &viewAccountIdentity,
    const QString &expectedAccountIdentity,
    qint64 nowMs,
    CompanionConfigurationCachePresentation *presentation,
    QString *errorCode)
{
    if (!presentation || !validTimestamp(nowMs)
            || !validAccountIdentity(viewAccountIdentity)
            || !validAccountIdentity(expectedAccountIdentity)
            || viewAccountIdentity != expectedAccountIdentity) {
        fail(errorCode, QStringLiteral("cache-presentation-account-binding-invalid"));
        return false;
    }
    if (!knownState(view.state)
            || view.configurationAuthority || view.configurationApplied
            || view.modelSelectionAuthority) {
        fail(errorCode, QStringLiteral("cache-presentation-authority-invalid"));
        return false;
    }

    CompanionConfigurationCachePresentation candidate;
    candidate.state = view.state;
    candidate.accountIdentity = expectedAccountIdentity;
    if (isErrorState(view.state)) {
        if (view.revision != 0 || view.capturedAtMs != 0
                || view.validUntilMs != 0 || view.staleUntilMs != 0
                || !view.sourceObservationSha256.isEmpty()
                || !view.contentSha256.isEmpty()
                || !view.configuration.isEmpty() || !view.models.isEmpty()
                || !validErrorCode(view.errorCode)) {
            fail(errorCode, QStringLiteral("cache-presentation-state-invalid"));
            return false;
        }
        candidate.provenance = QString::fromLatin1(kStateOnlyProvenance);
        *presentation = candidate;
        if (errorCode) errorCode->clear();
        return true;
    }

    if (!validDataMetadata(view)) {
        fail(errorCode, QStringLiteral("cache-presentation-metadata-invalid"));
        return false;
    }
    if (nowMs < view.capturedAtMs
            || (view.state == CompanionConfigurationCacheState::Fresh
            && nowMs >= view.validUntilMs)
            || (view.state == CompanionConfigurationCacheState::Stale
                && (nowMs < view.validUntilMs || nowMs >= view.staleUntilMs))
            || (view.state == CompanionConfigurationCacheState::Expired
                && nowMs < view.staleUntilMs)) {
        fail(errorCode, QStringLiteral("cache-presentation-state-time-invalid"));
        return false;
    }

    candidate.provenance = QString::fromLatin1(kPresentationProvenance);
    candidate.revision = view.revision;
    candidate.capturedAtMs = view.capturedAtMs;
    candidate.validUntilMs = view.validUntilMs;
    candidate.staleUntilMs = view.staleUntilMs;
    candidate.sourceObservationSha256 = view.sourceObservationSha256;
    candidate.contentSha256 = view.contentSha256;

    if (view.state == CompanionConfigurationCacheState::Expired) {
        if (!view.configuration.isEmpty() || !view.models.isEmpty()) {
            fail(errorCode, QStringLiteral("cache-presentation-expired-content-invalid"));
            return false;
        }
        *presentation = candidate;
        if (errorCode) errorCode->clear();
        return true;
    }

    QHash<QString, QString> platformByKey;
    if (!validateConfiguration(view.configuration, expectedAccountIdentity,
                               view.contentSha256, &candidate.keys,
                               &platformByKey)) {
        fail(errorCode, QStringLiteral("cache-presentation-configuration-invalid"));
        return false;
    }
    if (view.state == CompanionConfigurationCacheState::Stale) {
        if (!view.models.isEmpty()) {
            fail(errorCode, QStringLiteral("cache-presentation-stale-models-invalid"));
            return false;
        }
    } else if (!validateModels(view.models, platformByKey, view, nowMs,
                               &candidate.models)) {
        fail(errorCode, QStringLiteral("cache-presentation-models-invalid"));
        return false;
    }

    *presentation = candidate;
    if (errorCode) errorCode->clear();
    return true;
}

qint64 CompanionConfigurationCachePresentationAdapter::ageForDisplay(
    CompanionConfigurationCachePresentation *presentation,
    qint64 nowMs)
{
    if (!presentation || !validTimestamp(nowMs)) return 0;
    const bool dataState =
        presentation->state == CompanionConfigurationCacheState::Fresh
        || presentation->state == CompanionConfigurationCacheState::Stale
        || presentation->state == CompanionConfigurationCacheState::Expired;
    if (!dataState) return 0;
    const auto invalidate = [presentation]() {
        const QString accountIdentity = presentation->accountIdentity;
        *presentation = CompanionConfigurationCachePresentation{};
        presentation->state = CompanionConfigurationCacheState::Invalid;
        presentation->accountIdentity = accountIdentity;
        presentation->provenance = QString::fromLatin1(kStateOnlyProvenance);
    };
    if (!validTimestamp(presentation->capturedAtMs)
            || !validTimestamp(presentation->validUntilMs)
            || !validTimestamp(presentation->staleUntilMs)
            || presentation->capturedAtMs >= presentation->validUntilMs
            || presentation->validUntilMs >= presentation->staleUntilMs
            || nowMs < presentation->capturedAtMs) {
        invalidate();
        return 0;
    }
    if (presentation->state == CompanionConfigurationCacheState::Fresh) {
        for (qsizetype index = presentation->models.size() - 1;
             index >= 0; --index) {
            const CompanionCachedModelPresentation &model =
                presentation->models.at(index);
            if (!validTimestamp(model.capturedAtMs)
                    || !validTimestamp(model.validUntilMs)
                    || model.capturedAtMs < presentation->capturedAtMs
                    || model.validUntilMs <= model.capturedAtMs
                    || model.validUntilMs > presentation->validUntilMs
                    || nowMs >= model.validUntilMs) {
                presentation->models.removeAt(index);
            }
        }
        if (nowMs >= presentation->staleUntilMs) {
            presentation->state = CompanionConfigurationCacheState::Expired;
            presentation->keys.clear();
            presentation->models.clear();
            return 0;
        }
        if (nowMs >= presentation->validUntilMs) {
            presentation->state = CompanionConfigurationCacheState::Stale;
            presentation->models.clear();
            return presentation->staleUntilMs;
        }
        qint64 nextTransition = presentation->validUntilMs;
        for (const CompanionCachedModelPresentation &model
             : presentation->models) {
            if (model.validUntilMs > nowMs) {
                nextTransition = std::min(nextTransition, model.validUntilMs);
            }
        }
        return nextTransition;
    }
    if (presentation->state == CompanionConfigurationCacheState::Stale) {
        presentation->models.clear();
        if (nowMs >= presentation->staleUntilMs) {
            presentation->state = CompanionConfigurationCacheState::Expired;
            presentation->keys.clear();
            return 0;
        }
        return presentation->staleUntilMs;
    }
    presentation->keys.clear();
    presentation->models.clear();
    return 0;
}
