#include "extension_staging_restore_audit_ledger_store.h"

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

// 恢复审计自己的持久化域。授权模式串、QSettings 键与载荷域全部与复核记录、启用授权
// 不同，因此那两个子系统的授权信封或载荷都无法被当作恢复审计记录采用。
const QString kAuthoritySchema =
    QStringLiteral("aegisy-extension-restore-audit-ledger-authority/0.1");
const QString kRecordKey =
    QStringLiteral("extensions/restore-audit-ledger/record");
const QString kErrorPrefix =
    QStringLiteral("extension-restore-audit-store");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

void fail(QString *errorCode, const QString &value)
{
    if (errorCode) *errorCode = value;
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
            || raw > static_cast<double>(
                   ExtensionStagingRestoreAuditLedger::MaxGeneration)
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
        {QStringLiteral("schema_version"), kAuthoritySchema},
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
            || bytes.size()
                > ExtensionStagingRestoreAuditLedgerStore::MaxAuthorityBytes) {
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
                != kAuthoritySchema
            || !object.value(QStringLiteral("hmac_key_base64")).isString()
            || !object.value(QStringLiteral("committed_identity")).isString()) {
        return false;
    }
    Authority parsed;
    parsed.keyEncoded =
        object.value(QStringLiteral("hmac_key_base64")).toString().toLatin1();
    if (!canonicalBase64Key(parsed.keyEncoded, &parsed.key)
            || !safeGeneration(object.value(
                                   QStringLiteral("committed_generation")),
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

bool readRecordBytes(QSettings *settings, RecordBytes *record,
                     QString *errorCode)
{
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code("settings-unavailable"));
        return false;
    }
    const bool hasRecord = settings->contains(kRecordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code("settings-unavailable"));
        return false;
    }
    if (!hasRecord) {
        *record = RecordBytes{};
        return true;
    }
    const QVariant value = settings->value(kRecordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code("settings-unavailable"));
        return false;
    }
    // 存在但类型或体积不合法：记为"存在且损坏"，绝不当作缺失。
    if (!variantIsByteArray(value) || value.toByteArray().isEmpty()
            || value.toByteArray().size()
                > ExtensionStagingRestoreAuditLedger::MaxRecordBytes) {
        record->present = true;
        record->bytes = QByteArrayLiteral("\x01");
        return true;
    }
    record->present = true;
    record->bytes = value.toByteArray();
    return true;
}

ExtensionStagingRestoreAuditStoreResult invalid(const QString &errorCode)
{
    ExtensionStagingRestoreAuditStoreResult result;
    result.state = ExtensionStagingRestoreAuditStoreState::Invalid;
    result.errorCode = errorCode;
    return result;
}

ExtensionStagingRestoreAuditStoreResult unavailable(const QString &errorCode)
{
    ExtensionStagingRestoreAuditStoreResult result;
    result.state = ExtensionStagingRestoreAuditStoreState::Unavailable;
    result.errorCode = errorCode;
    return result;
}

} // namespace

ExtensionStagingRestoreAuditLedgerStore::ExtensionStagingRestoreAuditLedgerStore(
    ExtensionStagingRestoreAuditSecureStore *secureStore, QSettings *settings)
    : m_secureStore(secureStore)
    , m_settings(settings)
{
}

QString ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey()
{
    return kRecordKey;
}

ExtensionStagingRestoreAuditStoreResult
ExtensionStagingRestoreAuditLedgerStore::load()
{
    using ReadState = ExtensionStagingRestoreAuditSecureStore::ReadState;
    if (!m_secureStore || !m_settings) {
        return unavailable(code("unavailable"));
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
            ? code("authority-unavailable")
            : secureError);
    }
    if (authorityState == ReadState::Invalid) {
        return invalid(secureError.isEmpty()
            ? code("authority-backend-invalid")
            : secureError);
    }
    if (authorityState == ReadState::Missing) {
        // 授权从未建立。只有载荷也不存在才是真正的"从未记录过"；否则载荷是孤立的，
        // 说明授权被删除或从未与它配对，不能当作空白重新开始。
        if (record.present) {
            return invalid(code("record-without-authority"));
        }
        ExtensionStagingRestoreAuditStoreResult result;
        result.state = ExtensionStagingRestoreAuditStoreState::Empty;
        return result;
    }
    Authority authority;
    if (!parseAuthority(authorityData, &authority)) {
        return invalid(code("authority-invalid"));
    }
    if (authority.reservedPresent) {
        // 上一次写入在预留之后被打断。载荷字节是唯一的裁决依据：它要么正是预留的
        // 那一份（提交生效，完成提交），要么不是（写入从未生效，回滚预留）。
        const bool reservedLanded = record.present
            && ExtensionStagingRestoreAuditLedger::parse(
                   record.bytes, authority.key).identity
                == authority.reservedIdentity;
        if (reservedLanded) {
            authority.committedGeneration = authority.reservedGeneration;
            authority.committedIdentity = authority.reservedIdentity;
        }
        authority.reservedPresent = false;
        authority.reservedGeneration = 0;
        authority.reservedIdentity.clear();
        QString finishError;
        const ExtensionStagingRestoreAuditSecureStore::WriteOutcome outcome =
            m_secureStore->write(authorityBytes(authority), &finishError);
        if (outcome
                != ExtensionStagingRestoreAuditSecureStore::WriteOutcome::Committed) {
            cleanse(&authority.key);
            // 授权仍处于预留阶段，当前有效内容因此无法确定。
            ExtensionStagingRestoreAuditStoreResult result;
            result.state = outcome
                    == ExtensionStagingRestoreAuditSecureStore::WriteOutcome::
                        OutcomeUnknown
                ? ExtensionStagingRestoreAuditStoreState::OutcomeUnknown
                : ExtensionStagingRestoreAuditStoreState::Unavailable;
            result.errorCode = finishError.isEmpty()
                ? code("reserved-unresolved")
                : finishError;
            return result;
        }
    }
    if (authority.committedGeneration == 0) {
        // 授权已建立但没有提交过载荷。残留载荷同样不能被忽略。
        const bool orphaned = record.present;
        cleanse(&authority.key);
        if (orphaned) {
            return invalid(code("record-without-authority"));
        }
        ExtensionStagingRestoreAuditStoreResult result;
        result.state = ExtensionStagingRestoreAuditStoreState::Empty;
        return result;
    }
    if (!record.present) {
        // 授权仍然锚定一份载荷：删除 QSettings 不能退化成"从未记录过"。
        cleanse(&authority.key);
        return invalid(code("record-deleted"));
    }
    const ExtensionStagingRestoreAuditLedgerResult parsed =
        ExtensionStagingRestoreAuditLedger::parse(record.bytes, authority.key);
    const qint64 expectedGeneration = authority.committedGeneration;
    const QString expectedIdentity = authority.committedIdentity;
    cleanse(&authority.key);
    if (parsed.state == ExtensionStagingRestoreAuditLedgerState::Unavailable) {
        return unavailable(parsed.errorCode);
    }
    if (parsed.state != ExtensionStagingRestoreAuditLedgerState::Ready) {
        // 授权锚定了载荷，所以载荷为空或损坏都是 Invalid，绝不是 Empty。
        return invalid(parsed.errorCode.isEmpty()
            ? code("record-invalid")
            : parsed.errorCode);
    }
    // 载荷自身可认证还不够：它必须正是授权提交的那一份。否则一份旧的、当时合法
    // 签发过的载荷可以被放回原处，把已清空的审计历史重新变成有效。
    if (parsed.generation != expectedGeneration
            || parsed.identity != expectedIdentity) {
        return invalid(code("record-superseded"));
    }
    ExtensionStagingRestoreAuditStoreResult result;
    result.state = ExtensionStagingRestoreAuditStoreState::Ready;
    result.entries = parsed.entries;
    result.outcomes = parsed.outcomes;
    result.generation = parsed.generation;
    result.identity = parsed.identity;
    return result;
}

bool ExtensionStagingRestoreAuditLedgerStore::discard(
    ExtensionStagingRestoreAuditStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionStagingRestoreAuditStoreResult{};
    if (!m_secureStore || !m_settings) {
        fail(errorCode, code("unavailable"));
        return false;
    }
    // 只有确实自相矛盾的审计链才能被丢弃。可读的审计链不得被这条路径触碰：能作用在
    // 健康账本上的清空是一条不经授权就销毁审计历史的路径。不可读与结果未知同样不行
    // ——清空一份读不到的集合会销毁看不见的记录。
    const ExtensionStagingRestoreAuditStoreResult current = load();
    if (current.state != ExtensionStagingRestoreAuditStoreState::Invalid) {
        fail(errorCode, code("discard-not-required"));
        return false;
    }

    // 阶段一：销毁授权。写入一份全新的密钥、零代号、无预留，于是任何残留的载荷字节
    // 从此无法被任何人认证。这一步先做，因为它让这次清空不可逆：反过来先删载荷、旧
    // 密钥仍在，则任何能把那些字节放回去的人都能让被清空的审计历史复活。
    using WriteOutcome = ExtensionStagingRestoreAuditSecureStore::WriteOutcome;
    unsigned char raw[32]{};
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        OPENSSL_cleanse(raw, sizeof(raw));
        fail(errorCode, code("key-generation-failed"));
        return false;
    }
    Authority fresh;
    fresh.key = QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
    OPENSSL_cleanse(raw, sizeof(raw));
    fresh.keyEncoded = fresh.key.toBase64();
    QString authorityError;
    const WriteOutcome authorityOutcome =
        m_secureStore->write(authorityBytes(fresh), &authorityError);
    cleanse(&fresh.key);
    if (authorityOutcome != WriteOutcome::Committed) {
        fail(errorCode, authorityError.isEmpty()
             ? code("discard-authority-failed")
             : authorityError);
        return false;
    }

    // 阶段二：删除载荷字节。这一步失败时审计链仍然是 `Invalid`（授权已建立但零代号，
    // 而载荷仍在，也就是孤立载荷），因此一次没做完的丢弃绝不会被当成做完了。
    m_settings->remove(kRecordKey);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        fail(errorCode, code("settings-unavailable"));
        return false;
    }

    // 结论只能来自重新读出来的字节。一次被确认的写入不是证据：一个确认了写入却没有
    // 真的持久化的后端会让"记录已全部清空"成为一句谎报。
    const ExtensionStagingRestoreAuditStoreResult reread = load();
    if (updated) *updated = reread;
    if (reread.state != ExtensionStagingRestoreAuditStoreState::Empty
            || !reread.entries.isEmpty()) {
        fail(errorCode, reread.errorCode.isEmpty()
             ? code("discard-incomplete") : reread.errorCode);
        return false;
    }
    return true;
}

bool ExtensionStagingRestoreAuditLedgerStore::replace(
    const QList<ExtensionStagingRestoreAuditEntry> &entries,
    qint64 expectedGeneration,
    ExtensionStagingRestoreAuditStoreResult *updated, QString *errorCode,
    const QList<ExtensionStagingRestoreOutcomeEntry> &outcomes)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionStagingRestoreAuditStoreResult{};
    if (!m_secureStore || !m_settings) {
        fail(errorCode, code("unavailable"));
        return false;
    }
    if (expectedGeneration < 0) {
        fail(errorCode, code("generation-invalid"));
        return false;
    }
    // 集合已满时以独立代号拒绝：审计链绝不静默驱逐历史来腾位置。
    if (entries.size() > MaxEntries) {
        fail(errorCode, code("entries-cap"));
        return false;
    }
    if (outcomes.size() > ExtensionStagingRestoreAuditLedger::MaxOutcomeEntries) {
        fail(errorCode, code("outcomes-cap"));
        return false;
    }
    // 先把任何未决的预留阶段解决掉，再判断调用者的代号预期。
    const ExtensionStagingRestoreAuditStoreResult current = load();
    if (current.state == ExtensionStagingRestoreAuditStoreState::Unavailable
            || current.state == ExtensionStagingRestoreAuditStoreState::OutcomeUnknown
            || current.state == ExtensionStagingRestoreAuditStoreState::Invalid) {
        fail(errorCode, current.errorCode.isEmpty()
             ? code("unavailable")
             : current.errorCode);
        return false;
    }
    // 比较并交换：调用者必须提交它实际读到的那一份，否则并发记录会静默覆盖。
    if (current.generation != expectedGeneration) {
        fail(errorCode, code("generation-conflict"));
        return false;
    }
    const qint64 nextGeneration = expectedGeneration + 1;
    if (nextGeneration > ExtensionStagingRestoreAuditLedger::MaxGeneration) {
        fail(errorCode, code("generation-exhausted"));
        return false;
    }

    using ReadState = ExtensionStagingRestoreAuditSecureStore::ReadState;
    using WriteOutcome = ExtensionStagingRestoreAuditSecureStore::WriteOutcome;
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState =
        m_secureStore->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable
            || authorityState == ReadState::Invalid) {
        fail(errorCode, secureError.isEmpty()
             ? code("authority-unavailable")
             : secureError);
        return false;
    }
    Authority authority;
    if (authorityState == ReadState::Missing) {
        // 首次使用：生成一次性 MAC 密钥。密钥只存在于安全存储中。
        unsigned char raw[32]{};
        if (RAND_bytes(raw, sizeof(raw)) != 1) {
            OPENSSL_cleanse(raw, sizeof(raw));
            fail(errorCode, code("key-generation-failed"));
            return false;
        }
        authority.key = QByteArray(reinterpret_cast<const char *>(raw),
                                   sizeof(raw));
        OPENSSL_cleanse(raw, sizeof(raw));
        authority.keyEncoded = authority.key.toBase64();
    } else if (!parseAuthority(authorityData, &authority)) {
        fail(errorCode, code("authority-invalid"));
        return false;
    }
    if (authority.reservedPresent
            || authority.committedGeneration != expectedGeneration) {
        // load() 已经解决过预留阶段，因此这里出现不一致意味着并发改动。
        cleanse(&authority.key);
        fail(errorCode, code("generation-conflict"));
        return false;
    }
    const QByteArray bytes = ExtensionStagingRestoreAuditLedger::serialize(
        nextGeneration, entries, authority.key, outcomes);
    if (bytes.isEmpty()) {
        cleanse(&authority.key);
        fail(errorCode, code("entries-invalid"));
        return false;
    }
    const ExtensionStagingRestoreAuditLedgerResult candidate =
        ExtensionStagingRestoreAuditLedger::parse(bytes, authority.key);
    if (candidate.state != ExtensionStagingRestoreAuditLedgerState::Ready) {
        cleanse(&authority.key);
        fail(errorCode, code("entries-invalid"));
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
             ? code("reserve-failed")
             : reserveError);
        return false;
    }

    // 阶段二：写入载荷字节。被打断时 load() 会依据落盘的字节裁决。
    m_settings->setValue(kRecordKey, bytes);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        cleanse(&authority.key);
        cleanse(&reserved.key);
        fail(errorCode, code("settings-unavailable"));
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
             ? code("commit-unresolved")
             : commitError);
        return false;
    }
    if (updated) {
        updated->state = ExtensionStagingRestoreAuditStoreState::Ready;
        updated->entries = candidate.entries;
        updated->outcomes = candidate.outcomes;
        updated->generation = candidate.generation;
        updated->identity = candidate.identity;
    }
    return true;
}
