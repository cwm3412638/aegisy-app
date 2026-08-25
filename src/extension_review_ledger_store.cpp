#include "extension_review_ledger_store.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QVariant>

#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace {

const char kAuthoritySchema[] = "aegisy-extension-review-ledger-authority/0.1";
const QString kRecordKey =
    QStringLiteral("extensions/review-ledger/record");

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

void cleanse(QByteArray *value)
{
    if (!value || value->isEmpty()) return;
    OPENSSL_cleanse(value->data(), static_cast<size_t>(value->size()));
    value->clear();
}

bool canonicalBase64Key(const QByteArray &encoded, QByteArray *key)
{
    if (!key) return false;
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    // 必须是规范编码：重新编码不一致说明授权字节被改动过。
    if (decoded.size() != 32 || decoded.toBase64() != encoded) return false;
    *key = decoded;
    return true;
}

bool safeGeneration(const QJsonValue &value, qint64 *generation)
{
    if (!generation || !value.isDouble()) return false;
    const double raw = value.toDouble();
    if (raw < 0.0
            || raw > static_cast<double>(ExtensionReviewLedger::MaxGeneration)
            || raw != static_cast<double>(static_cast<qint64>(raw))) {
        return false;
    }
    *generation = static_cast<qint64>(raw);
    return true;
}

struct Authority
{
    QByteArray key;
    QByteArray keyEncoded;
    // 0 表示授权已建立但尚未提交任何载荷。
    qint64 committedGeneration = 0;
    QString committedIdentity;
    // 预留阶段在写入载荷字节之前持久化"打算提交哪一份"，因此恢复时可以区分
    // "载荷写入从未生效"（确定性回滚）与"载荷已经落盘"（确定性完成提交），
    // 而不需要推断。
    bool reservedPresent = false;
    qint64 reservedGeneration = 0;
    QString reservedIdentity;
};

QJsonValue reservedJson(const Authority &authority)
{
    if (!authority.reservedPresent) return QJsonValue::Null;
    return QJsonObject{
        {QStringLiteral("generation"), authority.reservedGeneration},
        {QStringLiteral("identity"), authority.reservedIdentity},
    };
}

QByteArray authorityBytes(const Authority &authority)
{
    const QJsonObject object{
        {QStringLiteral("schema_version"), QString::fromLatin1(kAuthoritySchema)},
        {QStringLiteral("hmac_key_base64"),
         QString::fromLatin1(authority.keyEncoded)},
        {QStringLiteral("committed_generation"), authority.committedGeneration},
        {QStringLiteral("committed_identity"), authority.committedIdentity},
        {QStringLiteral("reserved"), reservedJson(authority)},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool parseAuthority(const QByteArray &bytes, Authority *authority)
{
    if (!authority || bytes.isEmpty()
            || bytes.size() > ExtensionReviewLedgerStore::MaxAuthorityBytes) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema_version"), QStringLiteral("hmac_key_base64"),
        QStringLiteral("committed_generation"),
        QStringLiteral("committed_identity"), QStringLiteral("reserved")};
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kAuthoritySchema)
            || !object.value(QStringLiteral("hmac_key_base64")).isString()
            || !object.value(QStringLiteral("committed_identity")).isString()) {
        return false;
    }
    Authority parsed;
    parsed.keyEncoded =
        object.value(QStringLiteral("hmac_key_base64")).toString().toLatin1();
    if (!canonicalBase64Key(parsed.keyEncoded, &parsed.key)
            || !safeGeneration(object.value(QStringLiteral("committed_generation")),
                               &parsed.committedGeneration)) {
        cleanse(&parsed.key);
        return false;
    }
    parsed.committedIdentity =
        object.value(QStringLiteral("committed_identity")).toString();
    // 代号与身份必须同时存在或同时缺失，否则授权自相矛盾。
    if ((parsed.committedGeneration == 0)
            != parsed.committedIdentity.isEmpty()) {
        cleanse(&parsed.key);
        return false;
    }
    const QJsonValue reserved = object.value(QStringLiteral("reserved"));
    if (reserved.isObject()) {
        const QJsonObject reservedObject = reserved.toObject();
        QStringList reservedKeys = reservedObject.keys();
        reservedKeys.sort();
        if (reservedKeys != QStringList{QStringLiteral("generation"),
                                        QStringLiteral("identity")}
                || !reservedObject.value(QStringLiteral("identity")).isString()
                || !safeGeneration(reservedObject.value(
                                       QStringLiteral("generation")),
                                   &parsed.reservedGeneration)) {
            cleanse(&parsed.key);
            return false;
        }
        parsed.reservedIdentity =
            reservedObject.value(QStringLiteral("identity")).toString();
        // 预留必须严格前进：代号不大于已提交代号说明授权被回放或改写。
        if (parsed.reservedGeneration <= parsed.committedGeneration
                || parsed.reservedIdentity.isEmpty()) {
            cleanse(&parsed.key);
            return false;
        }
        parsed.reservedPresent = true;
    } else if (!reserved.isNull()) {
        cleanse(&parsed.key);
        return false;
    }
    *authority = parsed;
    return true;
}

struct RecordBytes
{
    bool present = false;
    QByteArray bytes;
};

bool variantIsByteArray(const QVariant &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.typeId() == QMetaType::QByteArray;
#else
    return value.type() == QVariant::ByteArray;
#endif
}

bool readRecordBytes(QSettings *settings, RecordBytes *record, QString *errorCode)
{
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("extension-review-store-settings-unavailable"));
        return false;
    }
    const bool hasRecord = settings->contains(kRecordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("extension-review-store-settings-unavailable"));
        return false;
    }
    if (!hasRecord) {
        *record = RecordBytes{};
        return true;
    }
    const QVariant value = settings->value(kRecordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("extension-review-store-settings-unavailable"));
        return false;
    }
    // 存在但类型或体积不合法：记为"存在且损坏"，绝不当作缺失。
    if (!variantIsByteArray(value) || value.toByteArray().isEmpty()
            || value.toByteArray().size() > ExtensionReviewLedger::MaxRecordBytes) {
        record->present = true;
        record->bytes = QByteArrayLiteral("\x01");
        return true;
    }
    record->present = true;
    record->bytes = value.toByteArray();
    return true;
}

ExtensionReviewLedgerStoreResult invalid(const QString &code)
{
    ExtensionReviewLedgerStoreResult result;
    result.state = ExtensionReviewLedgerStoreState::Invalid;
    result.errorCode = code;
    return result;
}

ExtensionReviewLedgerStoreResult unavailable(const QString &code)
{
    ExtensionReviewLedgerStoreResult result;
    result.state = ExtensionReviewLedgerStoreState::Unavailable;
    result.errorCode = code;
    return result;
}

} // namespace

ExtensionReviewLedgerStore::ExtensionReviewLedgerStore(
    ExtensionReviewLedgerSecureStore *secureStore, QSettings *settings)
    : m_secureStore(secureStore)
    , m_settings(settings)
{
}

QString ExtensionReviewLedgerStore::recordSettingsKey()
{
    return kRecordKey;
}

ExtensionReviewLedgerStoreResult ExtensionReviewLedgerStore::load()
{
    using ReadState = ExtensionReviewLedgerSecureStore::ReadState;
    if (!m_secureStore || !m_settings) {
        return unavailable(QStringLiteral("extension-review-store-unavailable"));
    }
    RecordBytes record;
    QString settingsError;
    if (!readRecordBytes(m_settings, &record, &settingsError)) {
        return unavailable(settingsError);
    }
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState =
        m_secureStore->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable) {
        return unavailable(secureError.isEmpty()
            ? QStringLiteral("extension-review-store-authority-unavailable")
            : secureError);
    }
    if (authorityState == ReadState::Invalid) {
        return invalid(secureError.isEmpty()
            ? QStringLiteral("extension-review-store-authority-backend-invalid")
            : secureError);
    }
    if (authorityState == ReadState::Missing) {
        // 授权从未建立。只有载荷也不存在才是真正的"从未复核过"；否则载荷是孤立的，
        // 说明授权被删除或从未与它配对，不能当作空白重新开始。
        if (record.present) {
            return invalid(
                QStringLiteral("extension-review-store-record-without-authority"));
        }
        ExtensionReviewLedgerStoreResult result;
        result.state = ExtensionReviewLedgerStoreState::Empty;
        return result;
    }
    Authority authority;
    if (!parseAuthority(authorityData, &authority)) {
        return invalid(QStringLiteral("extension-review-store-authority-invalid"));
    }
    if (authority.reservedPresent) {
        // 上一次写入在预留之后被打断。载荷字节是唯一的裁决依据：它要么正是预留的
        // 那一份（提交生效，完成提交），要么不是（写入从未生效，回滚预留）。
        const bool reservedLanded = record.present
            && ExtensionReviewLedger::parse(record.bytes, authority.key).identity
                == authority.reservedIdentity;
        if (reservedLanded) {
            authority.committedGeneration = authority.reservedGeneration;
            authority.committedIdentity = authority.reservedIdentity;
        }
        authority.reservedPresent = false;
        authority.reservedGeneration = 0;
        authority.reservedIdentity.clear();
        QString finishError;
        const ExtensionReviewLedgerSecureStore::WriteOutcome outcome =
            m_secureStore->write(authorityBytes(authority), &finishError);
        if (outcome != ExtensionReviewLedgerSecureStore::WriteOutcome::Committed) {
            cleanse(&authority.key);
            // 授权仍处于预留阶段，当前有效内容因此无法确定。
            ExtensionReviewLedgerStoreResult result;
            result.state = outcome
                    == ExtensionReviewLedgerSecureStore::WriteOutcome::OutcomeUnknown
                ? ExtensionReviewLedgerStoreState::OutcomeUnknown
                : ExtensionReviewLedgerStoreState::Unavailable;
            result.errorCode = finishError.isEmpty()
                ? QStringLiteral("extension-review-store-reserved-unresolved")
                : finishError;
            return result;
        }
    }
    if (authority.committedGeneration == 0) {
        // 授权已建立但没有提交过载荷。残留载荷同样不能被忽略。
        const bool orphaned = record.present;
        cleanse(&authority.key);
        if (orphaned) {
            return invalid(
                QStringLiteral("extension-review-store-record-without-authority"));
        }
        ExtensionReviewLedgerStoreResult result;
        result.state = ExtensionReviewLedgerStoreState::Empty;
        return result;
    }
    if (!record.present) {
        // 授权仍然锚定一份载荷：删除 QSettings 不能退化成"没有复核"。
        cleanse(&authority.key);
        return invalid(QStringLiteral("extension-review-store-record-deleted"));
    }
    const ExtensionReviewLedgerResult parsed =
        ExtensionReviewLedger::parse(record.bytes, authority.key);
    const qint64 expectedGeneration = authority.committedGeneration;
    const QString expectedIdentity = authority.committedIdentity;
    cleanse(&authority.key);
    if (parsed.state == ExtensionReviewLedgerState::Unavailable) {
        return unavailable(parsed.errorCode);
    }
    if (parsed.state != ExtensionReviewLedgerState::Ready) {
        // 授权锚定了载荷，所以载荷为空或损坏都是 Invalid，绝不是 Empty。
        return invalid(parsed.errorCode.isEmpty()
            ? QStringLiteral("extension-review-store-record-invalid")
            : parsed.errorCode);
    }
    // 载荷自身可认证还不够：它必须正是授权提交的那一份。否则一份旧的、当时合法
    // 签发过的载荷可以被放回原处，把已撤销的复核重新变成有效。
    if (parsed.generation != expectedGeneration
            || parsed.identity != expectedIdentity) {
        return invalid(QStringLiteral("extension-review-store-record-superseded"));
    }
    ExtensionReviewLedgerStoreResult result;
    result.state = ExtensionReviewLedgerStoreState::Ready;
    result.pins = parsed.pins;
    result.generation = parsed.generation;
    result.identity = parsed.identity;
    return result;
}

bool ExtensionReviewLedgerStore::replace(
    const QList<ExtensionReviewPin> &pins, qint64 expectedGeneration,
    ExtensionReviewLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionReviewLedgerStoreResult{};
    if (!m_secureStore || !m_settings) {
        fail(errorCode, QStringLiteral("extension-review-store-unavailable"));
        return false;
    }
    if (expectedGeneration < 0) {
        fail(errorCode, QStringLiteral("extension-review-store-generation-invalid"));
        return false;
    }
    // 先把任何未决的预留阶段解决掉，再判断调用者的代号预期。
    const ExtensionReviewLedgerStoreResult current = load();
    if (current.state == ExtensionReviewLedgerStoreState::Unavailable
            || current.state == ExtensionReviewLedgerStoreState::OutcomeUnknown
            || current.state == ExtensionReviewLedgerStoreState::Invalid) {
        fail(errorCode, current.errorCode.isEmpty()
             ? QStringLiteral("extension-review-store-unavailable")
             : current.errorCode);
        return false;
    }
    // 比较并交换：调用者必须提交它实际读到的那一份，否则并发复核会静默覆盖。
    if (current.generation != expectedGeneration) {
        fail(errorCode, QStringLiteral("extension-review-store-generation-conflict"));
        return false;
    }
    const qint64 nextGeneration = expectedGeneration + 1;
    if (nextGeneration > ExtensionReviewLedger::MaxGeneration) {
        fail(errorCode, QStringLiteral("extension-review-store-generation-exhausted"));
        return false;
    }

    using ReadState = ExtensionReviewLedgerSecureStore::ReadState;
    using WriteOutcome = ExtensionReviewLedgerSecureStore::WriteOutcome;
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState =
        m_secureStore->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable
            || authorityState == ReadState::Invalid) {
        fail(errorCode, secureError.isEmpty()
             ? QStringLiteral("extension-review-store-authority-unavailable")
             : secureError);
        return false;
    }
    Authority authority;
    if (authorityState == ReadState::Missing) {
        // 首次使用：生成一次性 MAC 密钥。密钥只存在于安全存储中。
        unsigned char raw[32]{};
        if (RAND_bytes(raw, sizeof(raw)) != 1) {
            OPENSSL_cleanse(raw, sizeof(raw));
            fail(errorCode,
                 QStringLiteral("extension-review-store-key-generation-failed"));
            return false;
        }
        authority.key = QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
        OPENSSL_cleanse(raw, sizeof(raw));
        authority.keyEncoded = authority.key.toBase64();
    } else if (!parseAuthority(authorityData, &authority)) {
        fail(errorCode, QStringLiteral("extension-review-store-authority-invalid"));
        return false;
    }
    if (authority.reservedPresent
            || authority.committedGeneration != expectedGeneration) {
        // load() 已经解决过预留阶段，因此这里出现不一致意味着并发改动。
        cleanse(&authority.key);
        fail(errorCode, QStringLiteral("extension-review-store-generation-conflict"));
        return false;
    }
    const QByteArray bytes =
        ExtensionReviewLedger::serialize(nextGeneration, pins, authority.key);
    if (bytes.isEmpty()) {
        cleanse(&authority.key);
        fail(errorCode, QStringLiteral("extension-review-store-pins-invalid"));
        return false;
    }
    const ExtensionReviewLedgerResult candidate =
        ExtensionReviewLedger::parse(bytes, authority.key);
    if (candidate.state != ExtensionReviewLedgerState::Ready) {
        cleanse(&authority.key);
        fail(errorCode, QStringLiteral("extension-review-store-pins-invalid"));
        return false;
    }

    // 阶段一：先持久化"打算提交哪一份"。这一步失败时载荷字节还没有被改动。
    Authority reserved = authority;
    reserved.reservedPresent = true;
    reserved.reservedGeneration = nextGeneration;
    reserved.reservedIdentity = candidate.identity;
    QString reserveError;
    const WriteOutcome reserveOutcome =
        m_secureStore->write(authorityBytes(reserved), &reserveError);
    if (reserveOutcome != WriteOutcome::Committed) {
        cleanse(&authority.key);
        cleanse(&reserved.key);
        fail(errorCode, reserveError.isEmpty()
             ? QStringLiteral("extension-review-store-reserve-failed")
             : reserveError);
        return false;
    }

    // 阶段二：写入载荷字节。被打断时 load() 会依据落盘的字节裁决。
    m_settings->setValue(kRecordKey, bytes);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        cleanse(&authority.key);
        cleanse(&reserved.key);
        fail(errorCode,
             QStringLiteral("extension-review-store-settings-unavailable"));
        return false;
    }

    // 阶段三：完成提交。这一步失败不会丢失内容——载荷已经落盘，下一次 load()
    // 会看到预留身份与它一致并完成提交。
    Authority committed = authority;
    committed.committedGeneration = nextGeneration;
    committed.committedIdentity = candidate.identity;
    QString commitError;
    const WriteOutcome commitOutcome =
        m_secureStore->write(authorityBytes(committed), &commitError);
    cleanse(&authority.key);
    cleanse(&reserved.key);
    cleanse(&committed.key);
    if (commitOutcome != WriteOutcome::Committed) {
        fail(errorCode, commitError.isEmpty()
             ? QStringLiteral("extension-review-store-commit-unresolved")
             : commitError);
        return false;
    }
    if (updated) {
        updated->state = ExtensionReviewLedgerStoreState::Ready;
        updated->pins = candidate.pins;
        updated->generation = candidate.generation;
        updated->identity = candidate.identity;
    }
    return true;
}
