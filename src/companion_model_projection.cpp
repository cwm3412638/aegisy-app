#include "companion_model_projection.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace {

constexpr int kMaximumModels = 1000;
constexpr int kMaximumModelIdBytes = 128;
constexpr int kMaximumProjectionBytes = 256 * 1024;

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool validLowerSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validKeyIdentity(const QString &value)
{
    for (const QString &prefix : {
             QStringLiteral("website-key:sha256:"),
             QStringLiteral("local-profile:sha256:"),
         }) {
        if (value.startsWith(prefix) && validLowerSha256(value.mid(prefix.size()))) {
            return true;
        }
    }
    return false;
}

bool validModelId(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (value.isEmpty() || utf8.size() > kMaximumModelIdBytes) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate
                || character.isSpace()) {
            return false;
        }
    }
    const QString lower = value.toLower();
    return !lower.contains(QStringLiteral("bearer"))
        && !lower.contains(QStringLiteral("api_key"))
        && !(value.startsWith(QStringLiteral("sk-")) && value.size() >= 12)
        && !(value.count(QLatin1Char('.')) == 2 && value.size() >= 24);
}

QString digest(QJsonObject projection)
{
    projection.remove(QStringLiteral("projection_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(projection).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

} // namespace

QJsonObject CompanionModelProjection::fromProviderResponse(
    const QString &keyIdentity, const QJsonObject &response, QString *errorCode)
{
    if (!validKeyIdentity(keyIdentity)) {
        fail(errorCode, QStringLiteral("model-projection-key-invalid"));
        return {};
    }
    const QJsonValue dataValue = response.value(QStringLiteral("data"));
    if (!dataValue.isArray() || dataValue.toArray().size() > kMaximumModels) {
        fail(errorCode, QStringLiteral("model-projection-response-invalid"));
        return {};
    }

    QJsonArray models;
    QSet<QString> seen;
    for (const QJsonValue &value : dataValue.toArray()) {
        const QString id = value.isObject()
            ? value.toObject().value(QStringLiteral("id")).toString()
            : value.toString();
        if (id != id.trimmed() || !validModelId(id) || seen.contains(id)) {
            fail(errorCode, QStringLiteral("model-projection-model-invalid"));
            return {};
        }
        seen.insert(id);
        models.append(id);
    }

    QJsonObject projection{
        { QStringLiteral("schema_version"),
          QStringLiteral("aegisy-companion-model-projection/0.1") },
        { QStringLiteral("key_identity"), keyIdentity },
        { QStringLiteral("state"), QStringLiteral("loaded") },
        { QStringLiteral("model_count"), models.size() },
        { QStringLiteral("models"), models },
        { QStringLiteral("provider_body_included"), false },
        { QStringLiteral("selection_authority"), false },
    };
    projection.insert(QStringLiteral("projection_sha256"), digest(projection));
    if (!validate(projection, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return projection;
}

bool CompanionModelProjection::validate(
    const QJsonObject &projection, QString *errorCode)
{
    static const QSet<QString> expected{
        QStringLiteral("schema_version"), QStringLiteral("key_identity"),
        QStringLiteral("state"), QStringLiteral("model_count"),
        QStringLiteral("models"), QStringLiteral("provider_body_included"),
        QStringLiteral("selection_authority"), QStringLiteral("projection_sha256"),
    };
    const QJsonValue modelsValue = projection.value(QStringLiteral("models"));
    const QJsonValue countValue = projection.value(QStringLiteral("model_count"));
    const QJsonArray models = modelsValue.toArray();
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    if (!exactKeys(projection, expected)
            || encoded.isEmpty() || encoded.size() > kMaximumProjectionBytes
            || projection.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("aegisy-companion-model-projection/0.1")
            || !validKeyIdentity(
                projection.value(QStringLiteral("key_identity")).toString())
            || projection.value(QStringLiteral("state")).toString()
                != QStringLiteral("loaded")
            || !modelsValue.isArray() || models.size() > kMaximumModels
            || !countValue.isDouble() || countValue.toInt(-1) != models.size()
            || projection.value(QStringLiteral("provider_body_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("selection_authority"))
                != QJsonValue(false)
            || !validLowerSha256(
                projection.value(QStringLiteral("projection_sha256")).toString())
            || projection.value(QStringLiteral("projection_sha256")).toString()
                != digest(projection)) {
        fail(errorCode, QStringLiteral("model-projection-invalid"));
        return false;
    }
    QSet<QString> seen;
    for (const QJsonValue &value : models) {
        const QString id = value.toString();
        if (!value.isString() || !validModelId(id) || seen.contains(id)) {
            fail(errorCode, QStringLiteral("model-projection-model-invalid"));
            return false;
        }
        seen.insert(id);
    }
    if (errorCode) errorCode->clear();
    return true;
}
