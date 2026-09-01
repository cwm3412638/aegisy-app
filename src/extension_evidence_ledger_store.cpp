#include "extension_evidence_ledger_store.h"

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

// 固定诊断代码由调用方的前缀构成，因此抽取不会改变任何已经被测试与文档固定的代码。
QString code(const ExtensionEvidenceLedgerStoreDomain &domain, const char *suffix)
{
    return domain.errorPrefix + QLatin1Char('-') + QLatin1String(suffix);
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
            || raw > static_cast<double>(ExtensionEvidenceLedger::MaxGeneration)
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

QByteArray authorityBytes(const ExtensionEvidenceLedgerStoreDomain &domain,
                          const Authority &authority)
{
    const QJsonObject object{
        {QStringLiteral("schema_version"), domain.authoritySchema},
        {QStringLiteral("hmac_key_base64"),
         QString::fromLatin1(authority.keyEncoded)},
        {QStringLiteral("committed_generation"), authority.committedGeneration},
        {QStringLiteral("committed_identity"), authority.committedIdentity},
        {QStringLiteral("reserved"), reservedJson(authority)},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool parseAuthority(const ExtensionEvidenceLedgerStoreDomain &domain,
                    const QByteArray &bytes, Authority *authority)
{
    if (!authority || bytes.isEmpty()
            || bytes.size() > ExtensionEvidenceLedgerStore::MaxAuthorityBytes) {
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
                != domain.authoritySchema
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

bool readRecordBytes(const ExtensionEvidenceLedgerStoreDomain &domain,
                     QSettings *settings, RecordBytes *record, QString *errorCode)
{
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code(domain, "settings-unavailable"));
        return false;
    }
    const bool hasRecord = settings->contains(domain.recordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code(domain, "settings-unavailable"));
        return false;
    }
    if (!hasRecord) {
        *record = RecordBytes{};
        return true;
    }
    const QVariant value = settings->value(domain.recordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, code(domain, "settings-unavailable"));
        return false;
    }
    // 存在但类型或体积不合法：记为"存在且损坏"，绝不当作缺失。
    if (!variantIsByteArray(value) || value.toByteArray().isEmpty()
            || value.toByteArray().size() > ExtensionEvidenceLedger::MaxRecordBytes) {
        record->present = true;
        record->bytes = QByteArrayLiteral("\x01");
        return true;
    }
    record->present = true;
    record->bytes = value.toByteArray();
    return true;
}

ExtensionEvidenceLedgerStoreResult invalid(const QString &errorCode)
{
    ExtensionEvidenceLedgerStoreResult result;
    result.state = ExtensionEvidenceLedgerStoreState::Invalid;
    result.errorCode = errorCode;
    return result;
}

ExtensionEvidenceLedgerStoreResult unavailable(const QString &errorCode)
{
    ExtensionEvidenceLedgerStoreResult result;
    result.state = ExtensionEvidenceLedgerStoreState::Unavailable;
    result.errorCode = errorCode;
    return result;
}

} // namespace

ExtensionEvidenceLedgerStore::ExtensionEvidenceLedgerStore(
    const ExtensionEvidenceLedgerStoreDomain &domain,
    ExtensionEvidenceLedgerSecureStore *secureStore, QSettings *settings)
    : m_domain(domain)
    , m_secureStore(secureStore)
    , m_settings(settings)
{
}

ExtensionEvidenceLedgerStoreResult ExtensionEvidenceLedgerStore::load()
{
    using ReadState = ExtensionEvidenceLedgerSecureStore::ReadState;
    const ExtensionEvidenceLedgerStoreDomain &domain = m_domain;
    // 未配置的域被拒绝，而不是退回某个默认格式：默认格式会让两类证据共用同一套字节。
    if (!domain.configured()) {
        return invalid(QStringLiteral(
            "extension-evidence-store-domain-unconfigured"));
    }
    if (!m_secureStore || !m_settings) {
        return unavailable(code(domain, "unavailable"));
    }
    RecordBytes record;
    QString settingsError;
    if (!readRecordBytes(domain, m_settings, &record, &settingsError)) {
        return unavailable(settingsError);
    }
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState =
        m_secureStore->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable) {
        return unavailable(secureError.isEmpty()
            ? code(domain, "authority-unavailable")
            : secureError);
    }
    if (authorityState == ReadState::Invalid) {
        return invalid(secureError.isEmpty()
            ? code(domain, "authority-backend-invalid")
            : secureError);
    }
    if (authorityState == ReadState::Missing) {
        // 授权从未建立。只有载荷也不存在才是真正的"从未复核过"；否则载荷是孤立的，
        // 说明授权被删除或从未与它配对，不能当作空白重新开始。
        if (record.present) {
            return invalid(
                code(domain, "record-without-authority"));
        }
        ExtensionEvidenceLedgerStoreResult result;
        result.state = ExtensionEvidenceLedgerStoreState::Empty;
        return result;
    }
    Authority authority;
    if (!parseAuthority(domain, authorityData, &authority)) {
        return invalid(code(domain, "authority-invalid"));
    }
    if (authority.reservedPresent) {
        // 上一次写入在预留之后被打断。载荷字节是唯一的裁决依据：它要么正是预留的
        // 那一份（提交生效，完成提交），要么不是（写入从未生效，回滚预留）。
        const bool reservedLanded = record.present
            && ExtensionEvidenceLedger::parse(domain.ledger, record.bytes,
                                              authority.key).identity
                == authority.reservedIdentity;
        if (reservedLanded) {
            authority.committedGeneration = authority.reservedGeneration;
            authority.committedIdentity = authority.reservedIdentity;
        }
        authority.reservedPresent = false;
        authority.reservedGeneration = 0;
        authority.reservedIdentity.clear();
        QString finishError;
        const ExtensionEvidenceLedgerSecureStore::WriteOutcome outcome =
            m_secureStore->write(authorityBytes(domain, authority), &finishError);
        if (outcome != ExtensionEvidenceLedgerSecureStore::WriteOutcome::Committed) {
            cleanse(&authority.key);
            // 授权仍处于预留阶段，当前有效内容因此无法确定。
            ExtensionEvidenceLedgerStoreResult result;
            result.state = outcome
                    == ExtensionEvidenceLedgerSecureStore::WriteOutcome::OutcomeUnknown
                ? ExtensionEvidenceLedgerStoreState::OutcomeUnknown
                : ExtensionEvidenceLedgerStoreState::Unavailable;
            result.errorCode = finishError.isEmpty()
                ? code(domain, "reserved-unresolved")
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
                code(domain, "record-without-authority"));
        }
        ExtensionEvidenceLedgerStoreResult result;
        result.state = ExtensionEvidenceLedgerStoreState::Empty;
        return result;
    }
    if (!record.present) {
        // 授权仍然锚定一份载荷：删除 QSettings 不能退化成"没有复核"。
        cleanse(&authority.key);
        return invalid(code(domain, "record-deleted"));
    }
    const ExtensionEvidenceLedgerResult parsed =
        ExtensionEvidenceLedger::parse(domain.ledger, record.bytes, authority.key);
    const qint64 expectedGeneration = authority.committedGeneration;
    const QString expectedIdentity = authority.committedIdentity;
    cleanse(&authority.key);
    if (parsed.state == ExtensionEvidenceLedgerState::Unavailable) {
        return unavailable(parsed.errorCode);
    }
    if (parsed.state != ExtensionEvidenceLedgerState::Ready) {
        // 授权锚定了载荷，所以载荷为空或损坏都是 Invalid，绝不是 Empty。
        return invalid(parsed.errorCode.isEmpty()
            ? code(domain, "record-invalid")
            : parsed.errorCode);
    }
    // 载荷自身可认证还不够：它必须正是授权提交的那一份。否则一份旧的、当时合法
    // 签发过的载荷可以被放回原处，把已撤销的复核重新变成有效。
    if (parsed.generation != expectedGeneration
            || parsed.identity != expectedIdentity) {
        return invalid(code(domain, "record-superseded"));
    }
    ExtensionEvidenceLedgerStoreResult result;
    result.state = ExtensionEvidenceLedgerStoreState::Ready;
    result.entries = parsed.entries;
    result.generation = parsed.generation;
    result.identity = parsed.identity;
    return result;
}

bool ExtensionEvidenceLedgerStore::discard(
    ExtensionEvidenceLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionEvidenceLedgerStoreResult{};
    const ExtensionEvidenceLedgerStoreDomain &domain = m_domain;
    if (!domain.configured()) {
        fail(errorCode, QStringLiteral(
            "extension-evidence-store-domain-unconfigured"));
        return false;
    }
    if (!m_secureStore || !m_settings) {
        fail(errorCode, code(domain, "unavailable"));
        return false;
    }
    // 只有确实自相矛盾的账本才能被丢弃。可读的账本不得被这条路径触碰：能作用在健康账本上
    // 的清空是一条不经审批就撤销一切的后门。不可读与结果未知同样不行——清空一份读不到的
    // 集合会销毁看不见的记录。
    const ExtensionEvidenceLedgerStoreResult current = load();
    if (current.state != ExtensionEvidenceLedgerStoreState::Invalid) {
        fail(errorCode, code(domain, "discard-not-required"));
        return false;
    }

    // 阶段一：销毁授权。写入一份全新的密钥、零代号、无预留，于是任何残留的载荷字节从此
    // 无法被任何人认证。这一步先做，因为它让这次清空不可逆：反过来先删载荷、旧密钥仍在，
    // 则任何能把那些字节放回去的人都能让被收回的授权复活。
    using WriteOutcome = ExtensionEvidenceLedgerSecureStore::WriteOutcome;
    unsigned char raw[32]{};
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        OPENSSL_cleanse(raw, sizeof(raw));
        fail(errorCode, code(domain, "key-generation-failed"));
        return false;
    }
    Authority fresh;
    fresh.key = QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
    OPENSSL_cleanse(raw, sizeof(raw));
    fresh.keyEncoded = fresh.key.toBase64();
    QString authorityError;
    const WriteOutcome authorityOutcome =
        m_secureStore->write(authorityBytes(domain, fresh), &authorityError);
    cleanse(&fresh.key);
    if (authorityOutcome != WriteOutcome::Committed) {
        fail(errorCode, authorityError.isEmpty()
             ? code(domain, "discard-authority-failed")
             : authorityError);
        return false;
    }

    // 阶段二：删除载荷字节。这一步失败时账本仍然是 `Invalid`（授权已建立但零代号，而载荷
    // 仍在，也就是孤立载荷），因此一次没做完的丢弃绝不会被当成做完了。
    m_settings->remove(domain.recordKey);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        fail(errorCode, code(domain, "settings-unavailable"));
        return false;
    }

    // 结论只能来自重新读出来的字节。一次被确认的写入不是证据：一个确认了写入却没有真的
    // 持久化的后端会让"记录已全部清空"成为一句谎报。
    const ExtensionEvidenceLedgerStoreResult reread = load();
    if (updated) *updated = reread;
    if (reread.state != ExtensionEvidenceLedgerStoreState::Empty
            || !reread.entries.isEmpty()) {
        fail(errorCode, reread.errorCode.isEmpty()
             ? code(domain, "discard-incomplete") : reread.errorCode);
        return false;
    }
    return true;
}

bool ExtensionEvidenceLedgerStore::replace(
    const QList<ExtensionEvidenceEntry> &entries, qint64 expectedGeneration,
    ExtensionEvidenceLedgerStoreResult *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (updated) *updated = ExtensionEvidenceLedgerStoreResult{};
    const ExtensionEvidenceLedgerStoreDomain &domain = m_domain;
    // 未配置的域被拒绝，而不是退回某个默认格式。
    if (!domain.configured()) {
        fail(errorCode, QStringLiteral(
            "extension-evidence-store-domain-unconfigured"));
        return false;
    }
    if (!m_secureStore || !m_settings) {
        fail(errorCode, code(domain, "unavailable"));
        return false;
    }
    if (expectedGeneration < 0) {
        fail(errorCode, code(domain, "generation-invalid"));
        return false;
    }
    // 先把任何未决的预留阶段解决掉，再判断调用者的代号预期。
    const ExtensionEvidenceLedgerStoreResult current = load();
    if (current.state == ExtensionEvidenceLedgerStoreState::Unavailable
            || current.state == ExtensionEvidenceLedgerStoreState::OutcomeUnknown
            || current.state == ExtensionEvidenceLedgerStoreState::Invalid) {
        fail(errorCode, current.errorCode.isEmpty()
             ? code(domain, "unavailable")
             : current.errorCode);
        return false;
    }
    // 比较并交换：调用者必须提交它实际读到的那一份，否则并发复核会静默覆盖。
    if (current.generation != expectedGeneration) {
        fail(errorCode, code(domain, "generation-conflict"));
        return false;
    }
    const qint64 nextGeneration = expectedGeneration + 1;
    if (nextGeneration > ExtensionEvidenceLedger::MaxGeneration) {
        fail(errorCode, code(domain, "generation-exhausted"));
        return false;
    }

    using ReadState = ExtensionEvidenceLedgerSecureStore::ReadState;
    using WriteOutcome = ExtensionEvidenceLedgerSecureStore::WriteOutcome;
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState =
        m_secureStore->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable
            || authorityState == ReadState::Invalid) {
        fail(errorCode, secureError.isEmpty()
             ? code(domain, "authority-unavailable")
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
                 code(domain, "key-generation-failed"));
            return false;
        }
        authority.key = QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
        OPENSSL_cleanse(raw, sizeof(raw));
        authority.keyEncoded = authority.key.toBase64();
    } else if (!parseAuthority(domain, authorityData, &authority)) {
        fail(errorCode, code(domain, "authority-invalid"));
        return false;
    }
    if (authority.reservedPresent
            || authority.committedGeneration != expectedGeneration) {
        // load() 已经解决过预留阶段，因此这里出现不一致意味着并发改动。
        cleanse(&authority.key);
        fail(errorCode, code(domain, "generation-conflict"));
        return false;
    }
    const QByteArray bytes =
        ExtensionEvidenceLedger::serialize(domain.ledger, nextGeneration,
                                          entries, authority.key);
    if (bytes.isEmpty()) {
        cleanse(&authority.key);
        fail(errorCode, domain.errorPrefix + QLatin1Char('-') + domain.entriesCodeNoun
             + QStringLiteral("-invalid"));
        return false;
    }
    const ExtensionEvidenceLedgerResult candidate =
        ExtensionEvidenceLedger::parse(domain.ledger, bytes, authority.key);
    if (candidate.state != ExtensionEvidenceLedgerState::Ready) {
        cleanse(&authority.key);
        fail(errorCode, domain.errorPrefix + QLatin1Char('-') + domain.entriesCodeNoun
             + QStringLiteral("-invalid"));
        return false;
    }

    // 阶段一：先持久化"打算提交哪一份"。这一步失败时载荷字节还没有被改动。
    Authority reserved = authority;
    reserved.reservedPresent = true;
    reserved.reservedGeneration = nextGeneration;
    reserved.reservedIdentity = candidate.identity;
    QString reserveError;
    const WriteOutcome reserveOutcome =
        m_secureStore->write(authorityBytes(domain, reserved), &reserveError);
    if (reserveOutcome != WriteOutcome::Committed) {
        cleanse(&authority.key);
        cleanse(&reserved.key);
        fail(errorCode, reserveError.isEmpty()
             ? code(domain, "reserve-failed")
             : reserveError);
        return false;
    }

    // 阶段二：写入载荷字节。被打断时 load() 会依据落盘的字节裁决。
    m_settings->setValue(domain.recordKey, bytes);
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        cleanse(&authority.key);
        cleanse(&reserved.key);
        fail(errorCode,
             code(domain, "settings-unavailable"));
        return false;
    }

    // 阶段三：完成提交。这一步失败不会丢失内容——载荷已经落盘，下一次 load()
    // 会看到预留身份与它一致并完成提交。
    Authority committed = authority;
    committed.committedGeneration = nextGeneration;
    committed.committedIdentity = candidate.identity;
    QString commitError;
    const WriteOutcome commitOutcome =
        m_secureStore->write(authorityBytes(domain, committed), &commitError);
    cleanse(&authority.key);
    cleanse(&reserved.key);
    cleanse(&committed.key);
    if (commitOutcome != WriteOutcome::Committed) {
        fail(errorCode, commitError.isEmpty()
             ? code(domain, "commit-unresolved")
             : commitError);
        return false;
    }
    if (updated) {
        updated->state = ExtensionEvidenceLedgerStoreState::Ready;
        updated->entries = candidate.entries;
        updated->generation = candidate.generation;
        updated->identity = candidate.identity;
    }
    return true;
}
