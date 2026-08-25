#include "extension_review_ledger.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace {

const char kSchema[] = "aegisy-extension-review-ledger/0.1";
const char kMacDomain[] = "aegisy-extension-review-ledger-hmac/0.1\0";
const char kIdentityDomain[] = "aegisy-extension-review-ledger-identity/0.1\0";

// 长度前缀分帧，使拼接后的字节序列无法通过移动字段边界产生歧义。
void append(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

QString kindName(ExtensionKind kind)
{
    switch (kind) {
    case ExtensionKind::CodexPlugin: return QStringLiteral("codex-plugin");
    case ExtensionKind::Skill:       return QStringLiteral("skill");
    case ExtensionKind::Mcp:         return QStringLiteral("mcp");
    }
    return QString();
}

bool kindFromName(const QString &value, ExtensionKind *kind)
{
    if (value == QStringLiteral("codex-plugin")) {
        *kind = ExtensionKind::CodexPlugin;
    } else if (value == QStringLiteral("skill")) {
        *kind = ExtensionKind::Skill;
    } else if (value == QStringLiteral("mcp")) {
        *kind = ExtensionKind::Mcp;
    } else {
        return false;
    }
    return true;
}

bool hashIdentity(const QString &value, const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool validPin(const ExtensionReviewPin &pin)
{
    return !kindName(pin.kind).isEmpty()
        && QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
            .match(pin.id).hasMatch()
        && hashIdentity(pin.sourceIdentity,
                        QStringLiteral("extension-source:sha256:"))
        && hashIdentity(pin.contentIdentity,
                        QStringLiteral("extension-content:sha256:"));
}

// MAC 覆盖代号与整个复核集合的规范化预映像，因此代号不能被换到另一组复核记录上，
// 单条复核记录也不能被替换、删除或重排。
QByteArray macPreimage(qint64 generation, const QList<ExtensionReviewPin> &pins)
{
    QByteArray input(kMacDomain, sizeof(kMacDomain) - 1);
    append(&input, QByteArray::number(generation));
    append(&input, QByteArray::number(static_cast<qint64>(pins.size())));
    for (const ExtensionReviewPin &pin : pins) {
        append(&input, kindName(pin.kind).toUtf8());
        append(&input, pin.id.toUtf8());
        append(&input, pin.sourceIdentity.toUtf8());
        append(&input, pin.contentIdentity.toUtf8());
    }
    return input;
}

QString mac(const QByteArray &key, const QByteArray &preimage)
{
    unsigned char result[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (key.size() != 32
            || !HMAC(EVP_sha256(), key.constData(), key.size(),
                     reinterpret_cast<const unsigned char *>(preimage.constData()),
                     static_cast<size_t>(preimage.size()), result, &length)
            || length != 32) {
        OPENSSL_cleanse(result, sizeof(result));
        return {};
    }
    const QString value = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(result), 32).toHex());
    OPENSSL_cleanse(result, sizeof(result));
    return value;
}

bool equalMac(const QString &left, const QString &right)
{
    const QByteArray leftBytes = left.toLatin1();
    const QByteArray rightBytes = right.toLatin1();
    return leftBytes.size() == 64 && rightBytes.size() == 64
        && CRYPTO_memcmp(leftBytes.constData(), rightBytes.constData(), 64) == 0;
}

QString contentIdentity(qint64 generation, const QList<ExtensionReviewPin> &pins)
{
    QByteArray input(kIdentityDomain, sizeof(kIdentityDomain) - 1);
    append(&input, QByteArray::number(generation));
    for (const ExtensionReviewPin &pin : pins) {
        append(&input, kindName(pin.kind).toUtf8());
        append(&input, pin.id.toUtf8());
        append(&input, pin.sourceIdentity.toUtf8());
        append(&input, pin.contentIdentity.toUtf8());
    }
    return QStringLiteral("extension-review-ledger:sha256:")
        + QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

bool safeGeneration(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < 1
            || number > static_cast<double>(ExtensionReviewLedger::MaxGeneration)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

ExtensionReviewLedgerResult failure(ExtensionReviewLedgerState state,
                                    const QString &errorCode)
{
    ExtensionReviewLedgerResult result;
    result.state = state;
    result.errorCode = errorCode;
    return result;
}

} // namespace

QByteArray ExtensionReviewLedger::serialize(qint64 generation,
                                            const QList<ExtensionReviewPin> &pins,
                                            const QByteArray &key)
{
    if (generation < 1 || generation > MaxGeneration || pins.size() > MaxPins
            || key.size() != 32) {
        return {};
    }
    QSet<QString> seen;
    QJsonArray array;
    for (const ExtensionReviewPin &pin : pins) {
        // 同一 (kind, id) 出现两次会让信任判定得出冲突，因此不允许被写入。
        const QString recordKey = kindName(pin.kind) + QLatin1Char(':') + pin.id;
        if (!validPin(pin) || seen.contains(recordKey)) return {};
        seen.insert(recordKey);
        array.append(QJsonObject{
            {QStringLiteral("kind"), kindName(pin.kind)},
            {QStringLiteral("id"), pin.id},
            {QStringLiteral("source_identity"), pin.sourceIdentity},
            {QStringLiteral("content_identity"), pin.contentIdentity},
        });
    }
    const QString authenticator = mac(key, macPreimage(generation, pins));
    if (authenticator.isEmpty()) return {};
    const QJsonObject object{
        {QStringLiteral("schema"), QString::fromLatin1(kSchema)},
        {QStringLiteral("generation"), generation},
        {QStringLiteral("pins"), array},
        {QStringLiteral("mac"), authenticator},
    };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= MaxRecordBytes ? bytes : QByteArray();
}

ExtensionReviewLedgerResult ExtensionReviewLedger::parse(const QByteArray &bytes,
                                                         const QByteArray &key)
{
    // 只有确实没有载荷时才是 Empty。任何存在但不可信的载荷都是 Invalid。
    if (bytes.isEmpty()) {
        ExtensionReviewLedgerResult result;
        result.state = ExtensionReviewLedgerState::Empty;
        return result;
    }
    if (bytes.size() > MaxRecordBytes) {
        return failure(ExtensionReviewLedgerState::Invalid,
                       QStringLiteral("extension-review-ledger-oversized"));
    }
    if (key.size() != 32) {
        // 密钥不可用时当前内容无法判断，这不是"没有复核"。
        return failure(ExtensionReviewLedgerState::Unavailable,
                       QStringLiteral("extension-review-ledger-key-unavailable"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(ExtensionReviewLedgerState::Invalid,
                       QStringLiteral("extension-review-ledger-record-invalid"));
    }
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema"), QStringLiteral("generation"),
        QStringLiteral("pins"), QStringLiteral("mac")};
    const QStringList keys = object.keys();
    qint64 generation = 0;
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema")).toString()
                != QString::fromLatin1(kSchema)
            || !safeGeneration(object.value(QStringLiteral("generation")),
                               &generation)
            || !object.value(QStringLiteral("pins")).isArray()
            || !object.value(QStringLiteral("mac")).isString()) {
        return failure(ExtensionReviewLedgerState::Invalid,
                       QStringLiteral("extension-review-ledger-record-invalid"));
    }
    const QJsonArray array = object.value(QStringLiteral("pins")).toArray();
    if (array.size() > MaxPins) {
        return failure(ExtensionReviewLedgerState::Invalid,
                       QStringLiteral("extension-review-ledger-pin-limit"));
    }
    static const QSet<QString> pinKeys{
        QStringLiteral("kind"), QStringLiteral("id"),
        QStringLiteral("source_identity"), QStringLiteral("content_identity")};
    QList<ExtensionReviewPin> pins;
    QSet<QString> seen;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            return failure(ExtensionReviewLedgerState::Invalid,
                           QStringLiteral("extension-review-ledger-pin-invalid"));
        }
        const QJsonObject item = value.toObject();
        const QStringList itemKeys = item.keys();
        ExtensionReviewPin pin;
        if (QSet<QString>(itemKeys.cbegin(), itemKeys.cend()) != pinKeys
                || !item.value(QStringLiteral("kind")).isString()
                || !item.value(QStringLiteral("id")).isString()
                || !item.value(QStringLiteral("source_identity")).isString()
                || !item.value(QStringLiteral("content_identity")).isString()
                || !kindFromName(item.value(QStringLiteral("kind")).toString(),
                                 &pin.kind)) {
            return failure(ExtensionReviewLedgerState::Invalid,
                           QStringLiteral("extension-review-ledger-pin-invalid"));
        }
        pin.id = item.value(QStringLiteral("id")).toString();
        pin.sourceIdentity =
            item.value(QStringLiteral("source_identity")).toString();
        pin.contentIdentity =
            item.value(QStringLiteral("content_identity")).toString();
        if (!validPin(pin)) {
            return failure(ExtensionReviewLedgerState::Invalid,
                           QStringLiteral("extension-review-ledger-pin-invalid"));
        }
        const QString recordKey =
            kindName(pin.kind) + QLatin1Char(':') + pin.id;
        if (seen.contains(recordKey)) {
            return failure(ExtensionReviewLedgerState::Invalid,
                           QStringLiteral("extension-review-ledger-pin-duplicate"));
        }
        seen.insert(recordKey);
        pins.append(pin);
    }
    // MAC 在最后校验，且覆盖代号与整个集合，因此任何字段替换都会被发现。
    if (!equalMac(object.value(QStringLiteral("mac")).toString(),
                  mac(key, macPreimage(generation, pins)))) {
        return failure(ExtensionReviewLedgerState::Invalid,
                       QStringLiteral("extension-review-ledger-mac-mismatch"));
    }
    ExtensionReviewLedgerResult result;
    result.state = ExtensionReviewLedgerState::Ready;
    result.pins = pins;
    result.generation = generation;
    result.identity = contentIdentity(generation, pins);
    return result;
}
