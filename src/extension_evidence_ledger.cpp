#include "extension_evidence_ledger.h"

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

bool validEntry(const ExtensionEvidenceEntry &entry)
{
    return !kindName(entry.kind).isEmpty()
        && QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
            .match(entry.id).hasMatch()
        && hashIdentity(entry.sourceIdentity,
                        QStringLiteral("extension-source:sha256:"))
        && hashIdentity(entry.contentIdentity,
                        QStringLiteral("extension-content:sha256:"));
}

// MAC 覆盖代号与整个集合的规范化预映像，因此代号不能被换到另一组条目上，单条条目也
// 不能被替换、删除或重排。调用方的域进入预映像，因此一类证据的 MAC 在另一类里不成立。
QByteArray macPreimage(const ExtensionEvidenceLedgerDomain &domain,
                       qint64 generation,
                       const QList<ExtensionEvidenceEntry> &entries)
{
    QByteArray input = domain.macDomain;
    append(&input, QByteArray::number(generation));
    append(&input, QByteArray::number(static_cast<qint64>(entries.size())));
    for (const ExtensionEvidenceEntry &entry : entries) {
        append(&input, kindName(entry.kind).toUtf8());
        append(&input, entry.id.toUtf8());
        append(&input, entry.sourceIdentity.toUtf8());
        append(&input, entry.contentIdentity.toUtf8());
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

QString contentIdentity(const ExtensionEvidenceLedgerDomain &domain,
                        qint64 generation,
                        const QList<ExtensionEvidenceEntry> &entries)
{
    QByteArray input = domain.identityDomain;
    append(&input, QByteArray::number(generation));
    for (const ExtensionEvidenceEntry &entry : entries) {
        append(&input, kindName(entry.kind).toUtf8());
        append(&input, entry.id.toUtf8());
        append(&input, entry.sourceIdentity.toUtf8());
        append(&input, entry.contentIdentity.toUtf8());
    }
    return domain.identityPrefix
        + QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

bool safeGeneration(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number < 1
            || number > static_cast<double>(ExtensionEvidenceLedger::MaxGeneration)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

ExtensionEvidenceLedgerResult failure(ExtensionEvidenceLedgerState state,
                                      const QString &errorCode)
{
    ExtensionEvidenceLedgerResult result;
    result.state = state;
    result.errorCode = errorCode;
    return result;
}

QString code(const ExtensionEvidenceLedgerDomain &domain, const char *suffix)
{
    return domain.errorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

// 条目相关的代码保留各调用方原有的名词，因此抽取不会改变已经被测试与文档固定的
// 诊断代码。
QString entryCode(const ExtensionEvidenceLedgerDomain &domain, const char *suffix)
{
    return domain.errorPrefix + QLatin1Char('-') + domain.entryCodeNoun
        + QLatin1Char('-') + QLatin1String(suffix);
}

} // namespace

QByteArray ExtensionEvidenceLedger::serialize(
    const ExtensionEvidenceLedgerDomain &domain,
    qint64 generation,
    const QList<ExtensionEvidenceEntry> &entries,
    const QByteArray &key)
{
    // 未配置的域被拒绝，而不是退回某个默认格式：默认格式会让两类证据共用同一套字节。
    if (!domain.configured()) return {};
    if (generation < 1 || generation > MaxGeneration || entries.size() > MaxEntries
            || key.size() != 32) {
        return {};
    }
    QSet<QString> seen;
    QJsonArray array;
    for (const ExtensionEvidenceEntry &entry : entries) {
        // 同一 (kind, id) 出现两次会让各自的策略层得出冲突，因此不允许被写入。
        const QString recordKey = kindName(entry.kind) + QLatin1Char(':') + entry.id;
        if (!validEntry(entry) || seen.contains(recordKey)) return {};
        seen.insert(recordKey);
        array.append(QJsonObject{
            {QStringLiteral("kind"), kindName(entry.kind)},
            {QStringLiteral("id"), entry.id},
            {QStringLiteral("source_identity"), entry.sourceIdentity},
            {QStringLiteral("content_identity"), entry.contentIdentity},
        });
    }
    const QString authenticator =
        mac(key, macPreimage(domain, generation, entries));
    if (authenticator.isEmpty()) return {};
    const QJsonObject object{
        {QStringLiteral("schema"), domain.schema},
        {QStringLiteral("generation"), generation},
        {domain.entriesKey, array},
        {QStringLiteral("mac"), authenticator},
    };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= MaxRecordBytes ? bytes : QByteArray();
}

ExtensionEvidenceLedgerResult ExtensionEvidenceLedger::parse(
    const ExtensionEvidenceLedgerDomain &domain,
    const QByteArray &bytes,
    const QByteArray &key)
{
    if (!domain.configured()) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       QStringLiteral("extension-evidence-ledger-domain-unconfigured"));
    }
    // 只有确实没有载荷时才是 Empty。任何存在但不可信的载荷都是 Invalid。
    if (bytes.isEmpty()) {
        ExtensionEvidenceLedgerResult result;
        result.state = ExtensionEvidenceLedgerState::Empty;
        return result;
    }
    if (bytes.size() > MaxRecordBytes) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       code(domain, "oversized"));
    }
    if (key.size() != 32) {
        // 密钥不可用时当前内容无法判断，这不是"没有记录"。
        return failure(ExtensionEvidenceLedgerState::Unavailable,
                       code(domain, "key-unavailable"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       code(domain, "record-invalid"));
    }
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema"), QStringLiteral("generation"),
        domain.entriesKey, QStringLiteral("mac")};
    const QStringList keys = object.keys();
    qint64 generation = 0;
    // 模式串必须与调用方的域完全一致：一类证据的记录因此在另一类里无法解析。
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema")).toString() != domain.schema
            || !safeGeneration(object.value(QStringLiteral("generation")),
                               &generation)
            || !object.value(domain.entriesKey).isArray()
            || !object.value(QStringLiteral("mac")).isString()) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       code(domain, "record-invalid"));
    }
    const QJsonArray array = object.value(domain.entriesKey).toArray();
    if (array.size() > MaxEntries) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       domain.errorPrefix + QLatin1Char('-') + domain.entryCodeNoun
                           + QStringLiteral("-limit"));
    }
    static const QSet<QString> entryKeys{
        QStringLiteral("kind"), QStringLiteral("id"),
        QStringLiteral("source_identity"), QStringLiteral("content_identity")};
    QList<ExtensionEvidenceEntry> entries;
    QSet<QString> seen;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            return failure(ExtensionEvidenceLedgerState::Invalid,
                           entryCode(domain, "invalid"));
        }
        const QJsonObject item = value.toObject();
        const QStringList itemKeys = item.keys();
        ExtensionEvidenceEntry entry;
        if (QSet<QString>(itemKeys.cbegin(), itemKeys.cend()) != entryKeys
                || !item.value(QStringLiteral("kind")).isString()
                || !item.value(QStringLiteral("id")).isString()
                || !item.value(QStringLiteral("source_identity")).isString()
                || !item.value(QStringLiteral("content_identity")).isString()
                || !kindFromName(item.value(QStringLiteral("kind")).toString(),
                                 &entry.kind)) {
            return failure(ExtensionEvidenceLedgerState::Invalid,
                           entryCode(domain, "invalid"));
        }
        entry.id = item.value(QStringLiteral("id")).toString();
        entry.sourceIdentity =
            item.value(QStringLiteral("source_identity")).toString();
        entry.contentIdentity =
            item.value(QStringLiteral("content_identity")).toString();
        if (!validEntry(entry)) {
            return failure(ExtensionEvidenceLedgerState::Invalid,
                           entryCode(domain, "invalid"));
        }
        const QString recordKey =
            kindName(entry.kind) + QLatin1Char(':') + entry.id;
        if (seen.contains(recordKey)) {
            return failure(ExtensionEvidenceLedgerState::Invalid,
                           entryCode(domain, "duplicate"));
        }
        seen.insert(recordKey);
        entries.append(entry);
    }
    // MAC 在最后校验，且覆盖代号与整个集合，因此任何字段替换都会被发现。
    if (!equalMac(object.value(QStringLiteral("mac")).toString(),
                  mac(key, macPreimage(domain, generation, entries)))) {
        return failure(ExtensionEvidenceLedgerState::Invalid,
                       code(domain, "mac-mismatch"));
    }
    ExtensionEvidenceLedgerResult result;
    result.state = ExtensionEvidenceLedgerState::Ready;
    result.entries = entries;
    result.generation = generation;
    result.identity = contentIdentity(domain, generation, entries);
    return result;
}
