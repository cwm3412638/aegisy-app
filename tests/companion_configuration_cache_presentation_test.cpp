#include "companion_configuration_cache_presentation.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

const QString kAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('a'));
const QString kOtherAccount = QStringLiteral("website-account-session:sha256:")
    + QString(64, QLatin1Char('b'));
const QString kKey = QStringLiteral("website-key:sha256:")
    + QString(64, QLatin1Char('c'));
const qint64 kNow = 1800000000000LL;

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
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

QString digest(const QJsonObject &object)
{
    QByteArray bytes;
    if (!appendCanonical(object, &bytes)) return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QJsonObject configuration(const QString &account = kAccount)
{
    return QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-companion-cached-configuration/0.2")},
        {QStringLiteral("account_identity"), account},
        {QStringLiteral("source_origin"), QStringLiteral("https://aegisy.cc")},
        {QStringLiteral("key_count"), 1},
        {QStringLiteral("keys"), QJsonArray{
            QJsonObject{
                {QStringLiteral("key_identity"), kKey},
                {QStringLiteral("display_name"), QStringLiteral("Primary")},
                {QStringLiteral("group_label"), QStringLiteral("Codex")},
                {QStringLiteral("platform"), QStringLiteral("openai")},
                {QStringLiteral("state"), QStringLiteral("active")},
            },
        }},
    };
}

CompanionConfigurationCacheView freshView()
{
    CompanionConfigurationCacheView view;
    view.state = CompanionConfigurationCacheState::Fresh;
    view.revision = 7;
    view.capturedAtMs = kNow - 1000;
    view.validUntilMs = view.capturedAtMs
        + CompanionConfigurationCache::ConfigurationFreshMs;
    view.staleUntilMs = view.validUntilMs
        + CompanionConfigurationCache::ConfigurationStaleMs;
    view.sourceObservationSha256 = QString(64, QLatin1Char('d'));
    view.configuration = configuration();
    view.contentSha256 = digest(view.configuration);
    view.models = QJsonArray{
        QJsonObject{
            {QStringLiteral("key_identity"), kKey},
            {QStringLiteral("platform"), QStringLiteral("openai")},
            {QStringLiteral("configuration_observation_sha256"),
             view.sourceObservationSha256},
            {QStringLiteral("source_observation_sha256"),
             QString(64, QLatin1Char('e'))},
            {QStringLiteral("captured_at_ms"), kNow - 500},
            {QStringLiteral("valid_until_ms"), kNow + 5000},
            {QStringLiteral("model_count"), 2},
            {QStringLiteral("models"),
             QJsonArray{QStringLiteral("gpt-5"), QStringLiteral("gpt-5-mini")}},
        },
    };
    return view;
}

bool build(const CompanionConfigurationCacheView &view, qint64 nowMs,
           CompanionConfigurationCachePresentation *presentation,
           QString *errorCode = nullptr,
           const QString &viewAccount = kAccount,
           const QString &expectedAccount = kAccount)
{
    return CompanionConfigurationCachePresentationAdapter::build(
        view, viewAccount, expectedAccount, nowMs, presentation, errorCode);
}

bool freshAndStaleProjection()
{
    const CompanionConfigurationCacheView fresh = freshView();
    CompanionConfigurationCachePresentation presentation;
    QString error;
    if (!require(build(fresh, kNow, &presentation, &error),
                 "valid Fresh view was rejected")
            || !require(presentation.state
                            == CompanionConfigurationCacheState::Fresh,
                        "Fresh state was not preserved")
            || !require(presentation.revision == 7
                            && presentation.accountIdentity == kAccount
                            && presentation.keys.size() == 1
                            && presentation.models.size() == 1,
                        "Fresh rows or revision were not projected")
            || !require(presentation.keys.first().keyIdentity == kKey
                            && presentation.keys.first().displayName
                                == QStringLiteral("Primary")
                            && presentation.models.first().modelIds
                                == QStringList{QStringLiteral("gpt-5"),
                                               QStringLiteral("gpt-5-mini")},
                        "Fresh DTO fields drifted")
            || !require(!presentation.provenance.isEmpty()
                            && presentation.sourceObservationSha256
                                == fresh.sourceObservationSha256
                            && presentation.contentSha256 == fresh.contentSha256,
                        "Fresh provenance was not retained")) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }

    CompanionConfigurationCacheView stale = fresh;
    stale.state = CompanionConfigurationCacheState::Stale;
    stale.models = {};
    const qint64 staleNow = stale.validUntilMs;
    presentation = {};
    if (!require(build(stale, staleNow, &presentation, &error),
                 "valid Stale view was rejected")
            || !require(presentation.keys.size() == 1
                            && presentation.models.isEmpty(),
                        "Stale view exposed model rows or lost safe keys")) {
        std::cerr << error.toStdString() << '\n';
        return false;
    }
    stale.models = fresh.models;
    return require(!build(stale, staleNow, &presentation, &error),
                   "Stale view accepted model rows");
}

bool expiredAndErrorStates()
{
    CompanionConfigurationCacheView expired = freshView();
    expired.state = CompanionConfigurationCacheState::Expired;
    expired.configuration = {};
    expired.models = {};
    CompanionConfigurationCachePresentation presentation;
    QString error;
    if (!require(build(expired, expired.staleUntilMs, &presentation, &error),
                 "valid Expired view was rejected")
            || !require(presentation.keys.isEmpty()
                            && presentation.models.isEmpty()
                            && presentation.revision == expired.revision,
                        "Expired view exposed rows or lost metadata")) {
        return false;
    }

    const QList<CompanionConfigurationCacheState> states{
        CompanionConfigurationCacheState::Invalid,
        CompanionConfigurationCacheState::Empty,
        CompanionConfigurationCacheState::LegacyUnverified,
        CompanionConfigurationCacheState::Unavailable,
        CompanionConfigurationCacheState::OutcomeUnknown,
        CompanionConfigurationCacheState::RecoveryRequired,
    };
    for (const CompanionConfigurationCacheState state : states) {
        CompanionConfigurationCacheView view;
        view.state = state;
        view.errorCode = QStringLiteral("cache-state-unavailable");
        presentation = {};
        if (!require(build(view, kNow, &presentation, &error),
                     "valid state-only view was rejected")
                || !require(presentation.state == state
                                && presentation.revision == 0
                                && presentation.keys.isEmpty()
                                && presentation.models.isEmpty(),
                            "state-only DTO exposed cached data")) {
            return false;
        }
    }

    CompanionConfigurationCacheView malformed;
    malformed.state = CompanionConfigurationCacheState::Unavailable;
    malformed.configuration = configuration();
    if (!require(!build(malformed, kNow, &presentation, &error),
                 "error state accepted configuration rows")) return false;
    malformed.configuration = {};
    malformed.errorCode = QStringLiteral("Bearer secret-value");
    return require(!build(malformed, kNow, &presentation, &error),
                   "error state accepted secret-shaped diagnostics");
}

bool accountAuthorityAndStateBinding()
{
    CompanionConfigurationCacheView view = freshView();
    CompanionConfigurationCachePresentation presentation;
    QString error;
    if (!require(!build(view, kNow, &presentation, &error,
                        kOtherAccount, kAccount),
                 "source account mismatch was accepted")) return false;
    view.configuration = configuration(kOtherAccount);
    view.contentSha256 = digest(view.configuration);
    if (!require(!build(view, kNow, &presentation, &error),
                 "embedded account mismatch was accepted")) return false;
    view = freshView();
    view.configurationAuthority = true;
    if (!require(!build(view, kNow, &presentation, &error),
                 "configuration authority entered presentation")) return false;
    view = freshView();
    view.configurationApplied = true;
    if (!require(!build(view, kNow, &presentation, &error),
                 "applied authority entered presentation")) return false;
    view = freshView();
    view.modelSelectionAuthority = true;
    if (!require(!build(view, kNow, &presentation, &error),
                 "model-selection authority entered presentation")) return false;
    view = freshView();
    view.state = static_cast<CompanionConfigurationCacheState>(99);
    return require(!build(view, kNow, &presentation, &error),
                   "unknown cache state was accepted");
}

bool malformedContentAndSecretShapes()
{
    CompanionConfigurationCachePresentation presentation;
    QString error;
    CompanionConfigurationCacheView view = freshView();
    QJsonObject key = view.configuration.value(QStringLiteral("keys"))
        .toArray().first().toObject();
    key.insert(QStringLiteral("credential_handle"),
               QStringLiteral("website-credential:sha256:")
                   + QString(64, QLatin1Char('f')));
    view.configuration.insert(QStringLiteral("keys"), QJsonArray{key});
    view.contentSha256 = digest(view.configuration);
    if (!require(!build(view, kNow, &presentation, &error),
                 "credential handle field entered presentation")) return false;

    view = freshView();
    key = view.configuration.value(QStringLiteral("keys"))
        .toArray().first().toObject();
    key.insert(QStringLiteral("raw_id"), QStringLiteral("server-row-1"));
    view.configuration.insert(QStringLiteral("keys"), QJsonArray{key});
    view.contentSha256 = digest(view.configuration);
    if (!require(!build(view, kNow, &presentation, &error),
                 "raw ID field entered presentation")) return false;

    view = freshView();
    key = view.configuration.value(QStringLiteral("keys"))
        .toArray().first().toObject();
    key.insert(QStringLiteral("display_name"),
               QStringLiteral("Bearer super-secret-value"));
    view.configuration.insert(QStringLiteral("keys"), QJsonArray{key});
    view.contentSha256 = digest(view.configuration);
    if (!require(!build(view, kNow, &presentation, &error),
                 "secret-shaped display value entered presentation")) return false;

    view = freshView();
    QJsonObject model = view.models.first().toObject();
    model.insert(QStringLiteral("models"),
                 QJsonArray{QStringLiteral("github_pat_12345678901234567890")});
    model.insert(QStringLiteral("model_count"), 1);
    view.models = QJsonArray{model};
    if (!require(!build(view, kNow, &presentation, &error),
                 "secret-shaped model ID entered presentation")) return false;

    view = freshView();
    model = view.models.first().toObject();
    model.insert(QStringLiteral("credential_value"), QStringLiteral("secret"));
    view.models = QJsonArray{model};
    if (!require(!build(view, kNow, &presentation, &error),
                 "credential model field entered presentation")) return false;

    view = freshView();
    model = view.models.first().toObject();
    model.insert(QStringLiteral("valid_until_ms"), kNow);
    view.models = QJsonArray{model};
    return require(!build(view, kNow, &presentation, &error),
                   "expired model row entered Fresh presentation");
}

bool malformedMetadata()
{
    CompanionConfigurationCachePresentation presentation;
    QString error;
    CompanionConfigurationCacheView view = freshView();
    view.contentSha256 = QString(64, QLatin1Char('f'));
    if (!require(!build(view, kNow, &presentation, &error),
                 "content digest mismatch was accepted")) return false;
    view = freshView();
    view.revision = 0;
    if (!require(!build(view, kNow, &presentation, &error),
                 "zero revision was accepted")) return false;
    view = freshView();
    view.validUntilMs += 1;
    if (!require(!build(view, kNow, &presentation, &error),
                 "configuration lifetime drift was accepted")) return false;
    view = freshView();
    view.capturedAtMs = kNow + 1;
    view.validUntilMs = view.capturedAtMs
        + CompanionConfigurationCache::ConfigurationFreshMs;
    view.staleUntilMs = view.validUntilMs
        + CompanionConfigurationCache::ConfigurationStaleMs;
    if (!require(!build(view, kNow, &presentation, &error),
                 "future configuration capture was accepted")) return false;
    view = freshView();
    view.state = CompanionConfigurationCacheState::Fresh;
    if (!require(!build(view, view.validUntilMs, &presentation, &error),
                 "Fresh state beyond its deadline was accepted")) return false;
    view = freshView();
    QJsonObject model = view.models.first().toObject();
    model.insert(QStringLiteral("captured_at_ms"), kNow + 1);
    model.insert(QStringLiteral("valid_until_ms"), kNow + 5000);
    view.models = QJsonArray{model};
    if (!require(!build(view, kNow, &presentation, &error),
                 "future model observation was accepted")) return false;
    view = freshView();
    model = view.models.first().toObject();
    model.insert(QStringLiteral("configuration_observation_sha256"),
                 QString(64, QLatin1Char('f')));
    view.models = QJsonArray{model};
    return require(!build(view, kNow, &presentation, &error),
                   "model/configuration provenance drift was accepted");
}

bool displayAging()
{
    CompanionConfigurationCachePresentation presentation;
    QString error;
    const CompanionConfigurationCacheView view = freshView();
    if (!require(build(view, kNow, &presentation, &error),
                 "aging fixture was rejected")) return false;
    const qint64 modelDeadline = presentation.models.first().validUntilMs;
    qint64 next = CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &presentation, modelDeadline);
    if (!require(presentation.state == CompanionConfigurationCacheState::Fresh
                     && presentation.models.isEmpty()
                     && next == presentation.validUntilMs,
                 "model deadline did not remove only cached models")) return false;
    next = CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &presentation, presentation.validUntilMs);
    if (!require(presentation.state == CompanionConfigurationCacheState::Stale
                     && presentation.keys.size() == 1
                     && presentation.models.isEmpty()
                     && next == presentation.staleUntilMs,
                 "Fresh deadline did not produce Key-only Stale state")) return false;
    next = CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &presentation, presentation.staleUntilMs);
    if (!require(presentation.state == CompanionConfigurationCacheState::Expired
                     && presentation.keys.isEmpty()
                     && presentation.models.isEmpty() && next == 0,
                 "Stale deadline did not remove all cached rows")) return false;

    presentation = {};
    if (!require(build(view, kNow, &presentation, &error),
                 "clock rollback fixture was rejected")) return false;
    CompanionConfigurationCachePresentationAdapter::ageForDisplay(
        &presentation, presentation.capturedAtMs - 1);
    return require(presentation.state == CompanionConfigurationCacheState::Invalid
                       && presentation.keys.isEmpty()
                       && presentation.models.isEmpty()
                       && presentation.accountIdentity == kAccount,
                   "clock rollback did not invalidate display rows");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (!freshAndStaleProjection()
            || !expiredAndErrorStates()
            || !accountAuthorityAndStateBinding()
            || !malformedContentAndSecretShapes()
            || !malformedMetadata()
            || !displayAging()) {
        return EXIT_FAILURE;
    }
    std::cout << "companion configuration cache presentation tests passed\n";
    return EXIT_SUCCESS;
}
