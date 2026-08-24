#include "companion_activation_journal.h"

#include "configuration_backup_store.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVariant>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cmath>

namespace {

constexpr qint64 kMaximumSerial = 9007199254740991LL;
constexpr int kMaximumRecordBytes = 16 * 1024;
constexpr int kMaximumAuthorityBytes = 16 * 1024;

const char kRecordSchema[] = "aegisy-companion-activation-journal/0.3";
const char kAuthoritySchema[] =
    "aegisy-companion-activation-journal-authority/0.1";
const char kMacDomain[] = "aegisy-companion-activation-journal-hmac/0.3\0";

const QString kRecordKey =
    QStringLiteral("companion/activation-journal/record");
const QString kLegacyIdentityKey =
    QStringLiteral("companion/activation-journal/identity");

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

QString stageName(CompanionActivationStage stage)
{
    switch (stage) {
    case CompanionActivationStage::Prepared: return QStringLiteral("prepared");
    case CompanionActivationStage::FilesApplied:
        return QStringLiteral("files-applied");
    case CompanionActivationStage::GatewayCommitRequested:
        return QStringLiteral("gateway-commit-requested");
    case CompanionActivationStage::GatewayCommitted:
        return QStringLiteral("gateway-committed");
    case CompanionActivationStage::ProfileCommitRequested:
        return QStringLiteral("profile-commit-requested");
    case CompanionActivationStage::ProfileCommitted:
        return QStringLiteral("profile-committed");
    }
    return {};
}

bool parseStage(const QString &value, CompanionActivationStage *stage)
{
    if (value == QStringLiteral("prepared")) {
        *stage = CompanionActivationStage::Prepared;
    } else if (value == QStringLiteral("files-applied")) {
        *stage = CompanionActivationStage::FilesApplied;
    } else if (value == QStringLiteral("gateway-commit-requested")) {
        *stage = CompanionActivationStage::GatewayCommitRequested;
    } else if (value == QStringLiteral("gateway-committed")) {
        *stage = CompanionActivationStage::GatewayCommitted;
    } else if (value == QStringLiteral("profile-commit-requested")) {
        *stage = CompanionActivationStage::ProfileCommitRequested;
    } else if (value == QStringLiteral("profile-committed")) {
        *stage = CompanionActivationStage::ProfileCommitted;
    } else {
        return false;
    }
    return true;
}

bool validUuid(const QString &value, bool allowEmpty = false)
{
    if (allowEmpty && value.isEmpty()) return true;
    const QUuid uuid(value);
    return !uuid.isNull()
        && uuid.toString(QUuid::WithoutBraces) == value.toLower();
}

bool lowerHex(const QString &value, int size)
{
    if (value.size() != size) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a')
                     && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validHashIdentity(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix)
        && lowerHex(value.mid(prefix.size()), 64);
}

void append(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

bool safeSerial(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < 0 || number > static_cast<double>(kMaximumSerial)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool variantIsByteArray(const QVariant &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.metaType().id() == QMetaType::QByteArray;
#else
    return value.type() == QVariant::ByteArray;
#endif
}

QByteArray canonicalKey(const QByteArray &encoded)
{
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() == 32 && decoded.toBase64() == encoded
        ? decoded : QByteArray();
}

QString recordMac(const QByteArray &key, const QByteArray &recordBytes)
{
    QByteArray input(kMacDomain, sizeof(kMacDomain) - 1);
    append(&input, recordBytes);
    unsigned char result[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (key.size() != 32
            || !HMAC(EVP_sha256(), key.constData(), key.size(),
                     reinterpret_cast<const unsigned char *>(input.constData()),
                     static_cast<size_t>(input.size()), result, &length)
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

void cleanse(QByteArray *bytes)
{
    if (!bytes || bytes->isEmpty()) return;
    OPENSSL_cleanse(bytes->data(), static_cast<size_t>(bytes->size()));
    bytes->clear();
}

// 授权信封：MAC 密钥、单调序号、已提交记录锚点，以及一次进行中的预留目标。
struct Authority
{
    QByteArray bytes;
    QByteArray key;
    QByteArray keyEncoded;
    qint64 highestSerial = 0;
    bool committedPresent = false;
    QString committedRecordMac;
    qint64 committedSerial = 0;
    bool reservedPresent = false;
    // 预留目标为空表示这次事务打算删除记录字节。
    bool reservedTargetAbsent = false;
    QString reservedTargetRecordMac;
    qint64 reservedTargetSerial = 0;
};

QJsonValue committedJson(const Authority &authority)
{
    if (!authority.committedPresent) return QJsonValue::Null;
    return QJsonObject{
        {QStringLiteral("record_mac"), authority.committedRecordMac},
        {QStringLiteral("serial"), authority.committedSerial},
    };
}

QJsonValue reservedJson(const Authority &authority)
{
    if (!authority.reservedPresent) return QJsonValue::Null;
    return QJsonObject{
        {QStringLiteral("target_record_mac"), authority.reservedTargetAbsent
             ? QJsonValue::Null : QJsonValue(authority.reservedTargetRecordMac)},
        {QStringLiteral("target_serial"), authority.reservedTargetAbsent
             ? QJsonValue::Null : QJsonValue(authority.reservedTargetSerial)},
    };
}

QByteArray authorityBytes(const Authority &authority)
{
    const QJsonObject object{
        {QStringLiteral("schema_version"), QString::fromLatin1(kAuthoritySchema)},
        {QStringLiteral("hmac_key_base64"),
         QString::fromLatin1(authority.keyEncoded)},
        {QStringLiteral("phase"), authority.reservedPresent
             ? QStringLiteral("reserved") : QStringLiteral("committed")},
        {QStringLiteral("highest_serial"), authority.highestSerial},
        {QStringLiteral("committed"), committedJson(authority)},
        {QStringLiteral("reserved"), reservedJson(authority)},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool parseAuthority(const QByteArray &bytes, Authority *authority)
{
    if (!authority || bytes.isEmpty() || bytes.size() > kMaximumAuthorityBytes) {
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
        QStringLiteral("phase"), QStringLiteral("highest_serial"),
        QStringLiteral("committed"), QStringLiteral("reserved")};
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kAuthoritySchema)
            || !object.value(QStringLiteral("hmac_key_base64")).isString()
            || !object.value(QStringLiteral("phase")).isString()) {
        return false;
    }
    Authority parsed;
    parsed.keyEncoded = object.value(
        QStringLiteral("hmac_key_base64")).toString().toLatin1();
    parsed.key = canonicalKey(parsed.keyEncoded);
    if (parsed.key.isEmpty()
            || !safeSerial(object.value(QStringLiteral("highest_serial")),
                           &parsed.highestSerial)) {
        cleanse(&parsed.key);
        return false;
    }
    const QJsonValue committed = object.value(QStringLiteral("committed"));
    if (committed.isObject()) {
        const QJsonObject committedObject = committed.toObject();
        QStringList committedKeys = committedObject.keys();
        committedKeys.sort();
        if (committedKeys != QStringList{QStringLiteral("record_mac"),
                                         QStringLiteral("serial")}
                || !committedObject.value(QStringLiteral("record_mac")).isString()
                || !safeSerial(committedObject.value(QStringLiteral("serial")),
                               &parsed.committedSerial)) {
            cleanse(&parsed.key);
            return false;
        }
        parsed.committedPresent = true;
        parsed.committedRecordMac =
            committedObject.value(QStringLiteral("record_mac")).toString();
    } else if (!committed.isNull()) {
        cleanse(&parsed.key);
        return false;
    }
    const QJsonValue reserved = object.value(QStringLiteral("reserved"));
    if (reserved.isObject()) {
        const QJsonObject reservedObject = reserved.toObject();
        QStringList reservedKeys = reservedObject.keys();
        reservedKeys.sort();
        const QJsonValue target =
            reservedObject.value(QStringLiteral("target_record_mac"));
        const QJsonValue targetSerial =
            reservedObject.value(QStringLiteral("target_serial"));
        if (reservedKeys != QStringList{QStringLiteral("target_record_mac"),
                                        QStringLiteral("target_serial")}
                || target.isNull() != targetSerial.isNull()
                || !(target.isString() || target.isNull())
                || (!targetSerial.isNull()
                    && !safeSerial(targetSerial, &parsed.reservedTargetSerial))) {
            cleanse(&parsed.key);
            return false;
        }
        parsed.reservedPresent = true;
        parsed.reservedTargetAbsent = target.isNull();
        parsed.reservedTargetRecordMac = target.toString();
    } else if (!reserved.isNull()) {
        cleanse(&parsed.key);
        return false;
    }
    const QString phase = object.value(QStringLiteral("phase")).toString();
    // 预留目标序号必须严格大于已提交序号，且不得超过已保留的最高序号，
    // 这样崩溃恢复永远不会重用一个序号。
    if ((phase == QStringLiteral("reserved")) != parsed.reservedPresent
            || (phase != QStringLiteral("reserved")
                && phase != QStringLiteral("committed"))
            || (parsed.committedPresent
                && (!lowerHex(parsed.committedRecordMac, 64)
                    || parsed.committedSerial <= 0
                    || parsed.committedSerial > parsed.highestSerial))
            || (!parsed.committedPresent && parsed.committedSerial != 0)
            || (parsed.reservedPresent && !parsed.reservedTargetAbsent
                && (!lowerHex(parsed.reservedTargetRecordMac, 64)
                    || parsed.reservedTargetSerial <= parsed.committedSerial
                    || parsed.reservedTargetSerial > parsed.highestSerial
                    || equalMac(parsed.reservedTargetRecordMac,
                                parsed.committedRecordMac)))
            || (parsed.reservedPresent && parsed.reservedTargetAbsent
                && !parsed.committedPresent)) {
        cleanse(&parsed.key);
        return false;
    }
    parsed.bytes = bytes;
    cleanse(&authority->key);
    *authority = parsed;
    return true;
}

bool validateRecord(CompanionActivationRecord *record, QString *errorCode)
{
    if (!record || !validUuid(record->transactionId)
            || !validUuid(record->originalProfileId, true)
            || !validUuid(record->candidateProfileId)
            || record->serial <= 0 || record->serial > kMaximumSerial
            || !validHashIdentity(record->candidateProfileIdentity,
                                  QStringLiteral("profile-activation:sha256:"))
            || !ConfigurationBackupStore::isValidBackupId(record->receipt.backupId)
            || !validHashIdentity(
                record->receipt.backupManifestIdentity,
                QStringLiteral("configuration-backup-manifest:sha256:"))
            || !validHashIdentity(record->receipt.sourceFilesIdentity,
                                  QStringLiteral("configuration-files:sha256:"))
            || !validHashIdentity(record->receipt.candidateFilesIdentity,
                                  QStringLiteral("configuration-files:sha256:"))
            || (static_cast<int>(record->receipt.tool) < 0
                || static_cast<int>(record->receipt.tool) > 3)) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    if (record->candidateTemporary
            && (record->originalProfileId.isEmpty()
                || record->originalProfileId == record->candidateProfileId)) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    const bool applied = record->stage != CompanionActivationStage::Prepared;
    if (applied != validHashIdentity(
            record->receipt.appliedFilesIdentity,
            QStringLiteral("configuration-files:sha256:"))
            || (applied
                && record->receipt.appliedFilesIdentity
                    != record->receipt.candidateFilesIdentity)
            || ((record->stage == CompanionActivationStage::GatewayCommitRequested
                 || record->stage == CompanionActivationStage::GatewayCommitted)
                && !record->receipt.gatewayMode)) {
        fail(errorCode, QStringLiteral("activation-journal-stage-invalid"));
        return false;
    }
    const QString identity = CompanionActivationJournal::identityFor(*record);
    if (!record->identity.isEmpty() && record->identity != identity) {
        fail(errorCode, QStringLiteral("activation-journal-identity-invalid"));
        return false;
    }
    record->identity = identity;
    return true;
}

QByteArray serializeRecord(const CompanionActivationRecord &record)
{
    const QJsonObject object{
        {QStringLiteral("schema"), QString::fromLatin1(kRecordSchema)},
        {QStringLiteral("transaction_id"), record.transactionId},
        {QStringLiteral("original_profile_id"), record.originalProfileId},
        {QStringLiteral("candidate_profile_id"), record.candidateProfileId},
        {QStringLiteral("candidate_profile_identity"),
         record.candidateProfileIdentity},
        {QStringLiteral("candidate_temporary"), record.candidateTemporary},
        {QStringLiteral("stage"), stageName(record.stage)},
        {QStringLiteral("serial"), record.serial},
        {QStringLiteral("tool"), static_cast<int>(record.receipt.tool)},
        {QStringLiteral("backup_id"), record.receipt.backupId},
        {QStringLiteral("backup_manifest_identity"),
         record.receipt.backupManifestIdentity},
        {QStringLiteral("source_files_identity"),
         record.receipt.sourceFilesIdentity},
        {QStringLiteral("candidate_files_identity"),
         record.receipt.candidateFilesIdentity},
        {QStringLiteral("applied_files_identity"),
         record.receipt.appliedFilesIdentity},
        {QStringLiteral("gateway_mode"), record.receipt.gatewayMode},
        {QStringLiteral("identity"), record.identity},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool deserializeRecord(const QByteArray &bytes,
                       CompanionActivationRecord *record, QString *errorCode)
{
    if (!record || bytes.isEmpty() || bytes.size() > kMaximumRecordBytes) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema"), QStringLiteral("transaction_id"),
        QStringLiteral("original_profile_id"),
        QStringLiteral("candidate_profile_id"),
        QStringLiteral("candidate_profile_identity"),
        QStringLiteral("candidate_temporary"), QStringLiteral("stage"),
        QStringLiteral("serial"), QStringLiteral("tool"),
        QStringLiteral("backup_id"),
        QStringLiteral("backup_manifest_identity"),
        QStringLiteral("source_files_identity"),
        QStringLiteral("candidate_files_identity"),
        QStringLiteral("applied_files_identity"),
        QStringLiteral("gateway_mode"), QStringLiteral("identity")};
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema")).toString()
                != QString::fromLatin1(kRecordSchema)
            || !object.value(QStringLiteral("transaction_id")).isString()
            || !object.value(QStringLiteral("original_profile_id")).isString()
            || !object.value(QStringLiteral("candidate_profile_id")).isString()
            || !object.value(QStringLiteral("candidate_profile_identity")).isString()
            || !object.value(QStringLiteral("candidate_temporary")).isBool()
            || !object.value(QStringLiteral("stage")).isString()
            || !object.value(QStringLiteral("tool")).isDouble()
            || !object.value(QStringLiteral("backup_id")).isString()
            || !object.value(QStringLiteral("backup_manifest_identity")).isString()
            || !object.value(QStringLiteral("source_files_identity")).isString()
            || !object.value(QStringLiteral("candidate_files_identity")).isString()
            || !object.value(QStringLiteral("applied_files_identity")).isString()
            || !object.value(QStringLiteral("gateway_mode")).isBool()
            || !object.value(QStringLiteral("identity")).isString()) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    CompanionActivationRecord parsed;
    parsed.transactionId = object.value(QStringLiteral("transaction_id")).toString();
    parsed.originalProfileId =
        object.value(QStringLiteral("original_profile_id")).toString();
    parsed.candidateProfileId =
        object.value(QStringLiteral("candidate_profile_id")).toString();
    parsed.candidateProfileIdentity =
        object.value(QStringLiteral("candidate_profile_identity")).toString();
    parsed.candidateTemporary =
        object.value(QStringLiteral("candidate_temporary")).toBool();
    if (!parseStage(object.value(QStringLiteral("stage")).toString(), &parsed.stage)
            || !safeSerial(object.value(QStringLiteral("serial")), &parsed.serial)) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    const double toolNumber = object.value(QStringLiteral("tool")).toDouble(-1);
    if (toolNumber < 0 || toolNumber > 3
            || static_cast<int>(toolNumber) != toolNumber) {
        fail(errorCode, QStringLiteral("activation-journal-record-invalid"));
        return false;
    }
    parsed.receipt.tool = static_cast<AiTool>(static_cast<int>(toolNumber));
    parsed.receipt.backupId = object.value(QStringLiteral("backup_id")).toString();
    parsed.receipt.backupManifestIdentity =
        object.value(QStringLiteral("backup_manifest_identity")).toString();
    parsed.receipt.sourceFilesIdentity =
        object.value(QStringLiteral("source_files_identity")).toString();
    parsed.receipt.candidateFilesIdentity =
        object.value(QStringLiteral("candidate_files_identity")).toString();
    parsed.receipt.appliedFilesIdentity =
        object.value(QStringLiteral("applied_files_identity")).toString();
    parsed.receipt.gatewayMode =
        object.value(QStringLiteral("gateway_mode")).toBool();
    parsed.identity = object.value(QStringLiteral("identity")).toString();
    if (!validateRecord(&parsed, errorCode)) return false;
    *record = parsed;
    return true;
}

enum class AuthorityWriteResult {
    Expected,
    Previous,
    OutcomeUnknown,
    Invalid,
};

// 写入授权后必须重新读取，才能把"确定失败"与"结果未知"分开。
AuthorityWriteResult writeAuthority(
    CompanionActivationJournalSecureStore *store, const QByteArray &expected,
    const QByteArray &previous, bool previousMissing, QString *errorCode)
{
    using ReadState = CompanionActivationJournalSecureStore::ReadState;
    using WriteOutcome = CompanionActivationJournalSecureStore::WriteOutcome;
    QString writeError;
    const WriteOutcome outcome = store->write(expected, &writeError);
    QByteArray observed;
    QString readError;
    const ReadState readState = store->readFresh(&observed, &readError);
    if (readState == ReadState::Found && observed == expected) {
        return AuthorityWriteResult::Expected;
    }
    if ((readState == ReadState::Missing && previousMissing)
            || (readState == ReadState::Found && !previousMissing
                && observed == previous)) {
        fail(errorCode, writeError.isEmpty()
             ? QStringLiteral("activation-journal-authority-definite-failure")
             : writeError);
        return AuthorityWriteResult::Previous;
    }
    if (readState == ReadState::Unavailable) {
        fail(errorCode, readError.isEmpty()
             ? (outcome == WriteOutcome::DefiniteFailure
                    ? QStringLiteral(
                        "activation-journal-authority-unavailable-after-failure")
                    : QStringLiteral("activation-journal-authority-outcome-unknown"))
             : readError);
        return AuthorityWriteResult::OutcomeUnknown;
    }
    fail(errorCode, QStringLiteral("activation-journal-authority-write-drift"));
    return AuthorityWriteResult::Invalid;
}

CompanionActivationJournalState stateForWrite(AuthorityWriteResult result)
{
    switch (result) {
    case AuthorityWriteResult::Expected:
        return CompanionActivationJournalState::Ready;
    case AuthorityWriteResult::Previous:
        return CompanionActivationJournalState::RecoveryRequired;
    case AuthorityWriteResult::OutcomeUnknown:
        return CompanionActivationJournalState::OutcomeUnknown;
    case AuthorityWriteResult::Invalid:
        break;
    }
    return CompanionActivationJournalState::Invalid;
}

struct RecordBytes
{
    bool present = false;
    QByteArray bytes;
};

bool readRecordBytes(QSettings *settings, RecordBytes *record, QString *errorCode)
{
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("activation-journal-settings-unavailable"));
        return false;
    }
    const bool hasRecord = settings->contains(kRecordKey);
    // 旧版 `0.2` 只有本地身份键。它的残留不得被当作"没有事务"。
    const bool hasLegacyIdentity = settings->contains(kLegacyIdentityKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("activation-journal-settings-unavailable"));
        return false;
    }
    if (!hasRecord) {
        *record = RecordBytes{};
        if (hasLegacyIdentity) {
            record->present = true;
            record->bytes = QByteArrayLiteral("\x01");
        }
        return true;
    }
    const QVariant value = settings->value(kRecordKey);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("activation-journal-settings-unavailable"));
        return false;
    }
    if (!variantIsByteArray(value) || value.toByteArray().isEmpty()
            || value.toByteArray().size() > kMaximumRecordBytes) {
        record->present = true;
        record->bytes = QByteArrayLiteral("\x01");
        return true;
    }
    record->present = true;
    record->bytes = value.toByteArray();
    return true;
}

struct Resolved;
bool finishReservedPhase(CompanionActivationJournalSecureStore *store,
                         Resolved *resolved);

} // namespace

CompanionActivationJournal::CompanionActivationJournal(
    CompanionActivationJournalSecureStore *secureStore, QSettings *settings)
    : m_secureStore(secureStore)
    , m_settings(settings)
{
}

QString CompanionActivationJournal::identityFor(
    const CompanionActivationRecord &record)
{
    QByteArray input = QByteArrayLiteral("aegisy-companion-activation-journal/0.3\0");
    append(&input, record.transactionId.toUtf8());
    append(&input, record.originalProfileId.toUtf8());
    append(&input, record.candidateProfileId.toUtf8());
    append(&input, record.candidateProfileIdentity.toUtf8());
    append(&input, record.candidateTemporary ? QByteArrayLiteral("1")
                                              : QByteArrayLiteral("0"));
    append(&input, stageName(record.stage).toUtf8());
    append(&input, QByteArray::number(record.serial));
    append(&input, QByteArray::number(static_cast<int>(record.receipt.tool)));
    append(&input, record.receipt.backupId.toUtf8());
    append(&input, record.receipt.backupManifestIdentity.toUtf8());
    append(&input, record.receipt.sourceFilesIdentity.toUtf8());
    append(&input, record.receipt.candidateFilesIdentity.toUtf8());
    append(&input, record.receipt.appliedFilesIdentity.toUtf8());
    append(&input, record.receipt.gatewayMode ? QByteArrayLiteral("1")
                                               : QByteArrayLiteral("0"));
    return QStringLiteral("companion-activation-journal:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

namespace {

struct Resolved
{
    CompanionActivationJournalState state =
        CompanionActivationJournalState::Invalid;
    QString errorCode;
    Authority authority;
    RecordBytes record;
    CompanionActivationRecord parsed;
};

// 预留阶段崩溃恢复：记录字节必须精确等于预映像或预留候选，第三种状态不可推断。
bool finishReservedPhase(CompanionActivationJournalSecureStore *store,
                         Resolved *resolved)
{
    const Authority reserved = resolved->authority;
    const QString observed = resolved->record.present
        ? recordMac(reserved.key, resolved->record.bytes) : QString();
    const bool isPreimage = reserved.committedPresent
        ? (resolved->record.present
           && equalMac(observed, reserved.committedRecordMac))
        : !resolved->record.present;
    const bool isCandidate = reserved.reservedTargetAbsent
        ? !resolved->record.present
        : (resolved->record.present
           && equalMac(observed, reserved.reservedTargetRecordMac));
    Authority recovered = reserved;
    if (isPreimage) {
        // 目标写入没有生效。放弃这次预留，但绝不回收已保留的序号。
    } else if (isCandidate) {
        if (reserved.reservedTargetAbsent) {
            recovered.committedPresent = false;
            recovered.committedRecordMac.clear();
            recovered.committedSerial = 0;
        } else {
            recovered.committedPresent = true;
            recovered.committedRecordMac = reserved.reservedTargetRecordMac;
            recovered.committedSerial = reserved.reservedTargetSerial;
        }
    } else {
        cleanse(&resolved->authority.key);
        resolved->state = CompanionActivationJournalState::Invalid;
        resolved->errorCode =
            QStringLiteral("activation-journal-reserved-third-state");
        return false;
    }
    recovered.reservedPresent = false;
    recovered.reservedTargetAbsent = false;
    recovered.reservedTargetRecordMac.clear();
    recovered.reservedTargetSerial = 0;
    recovered.bytes = authorityBytes(recovered);
    QString resolveError;
    const AuthorityWriteResult resolution = writeAuthority(
        store, recovered.bytes, reserved.bytes, false, &resolveError);
    if (resolution != AuthorityWriteResult::Expected) {
        cleanse(&resolved->authority.key);
        resolved->state = resolution == AuthorityWriteResult::Previous
            ? CompanionActivationJournalState::RecoveryRequired
            : stateForWrite(resolution);
        resolved->errorCode = resolution == AuthorityWriteResult::Previous
            ? QStringLiteral("activation-journal-reserved-recovery-required")
            : resolveError;
        return false;
    }
    cleanse(&resolved->authority.key);
    resolved->authority = recovered;
    return true;
}

// 解析授权与记录字节，并确定性地完成任何进行中的预留阶段。
Resolved resolve(CompanionActivationJournalSecureStore *store, QSettings *settings)
{
    using ReadState = CompanionActivationJournalSecureStore::ReadState;
    Resolved resolved;
    if (!store || !settings) {
        resolved.state = CompanionActivationJournalState::Unavailable;
        resolved.errorCode = QStringLiteral("activation-journal-unavailable");
        return resolved;
    }
    if (!readRecordBytes(settings, &resolved.record, &resolved.errorCode)) {
        resolved.state = CompanionActivationJournalState::Unavailable;
        return resolved;
    }
    QByteArray authorityData;
    QString secureError;
    const ReadState authorityState = store->readFresh(&authorityData, &secureError);
    if (authorityState == ReadState::Unavailable) {
        resolved.state = CompanionActivationJournalState::Unavailable;
        resolved.errorCode = secureError.isEmpty()
            ? QStringLiteral("activation-journal-authority-unavailable")
            : secureError;
        return resolved;
    }
    if (authorityState == ReadState::Invalid) {
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode = secureError.isEmpty()
            ? QStringLiteral("activation-journal-authority-backend-invalid")
            : secureError;
        return resolved;
    }
    if (authorityState == ReadState::Missing) {
        // 授权从未建立：只有完全没有记录字节才是真正的空状态。
        resolved.state = resolved.record.present
            ? CompanionActivationJournalState::Invalid
            : CompanionActivationJournalState::Empty;
        if (resolved.record.present) {
            resolved.errorCode =
                QStringLiteral("activation-journal-record-without-authority");
        }
        return resolved;
    }
    if (!parseAuthority(authorityData, &resolved.authority)) {
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode = QStringLiteral("activation-journal-authority-invalid");
        return resolved;
    }
    if (resolved.authority.reservedPresent
            && !finishReservedPhase(store, &resolved)) {
        return resolved;
    }
    if (!resolved.authority.committedPresent) {
        if (resolved.record.present) {
            cleanse(&resolved.authority.key);
            resolved.state = CompanionActivationJournalState::Invalid;
            resolved.errorCode =
                QStringLiteral("activation-journal-record-without-authority");
            return resolved;
        }
        resolved.state = CompanionActivationJournalState::Empty;
        return resolved;
    }
    if (!resolved.record.present) {
        // 授权仍然锚定一条记录：删除 QSettings 不能退化为空状态。
        cleanse(&resolved.authority.key);
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode = QStringLiteral("activation-journal-record-deleted");
        return resolved;
    }
    if (!equalMac(recordMac(resolved.authority.key, resolved.record.bytes),
                  resolved.authority.committedRecordMac)) {
        cleanse(&resolved.authority.key);
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode =
            QStringLiteral("activation-journal-record-unauthenticated");
        return resolved;
    }
    QString parseError;
    if (!deserializeRecord(resolved.record.bytes, &resolved.parsed, &parseError)) {
        cleanse(&resolved.authority.key);
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode = parseError.isEmpty()
            ? QStringLiteral("activation-journal-invalid") : parseError;
        return resolved;
    }
    if (resolved.parsed.serial != resolved.authority.committedSerial) {
        cleanse(&resolved.authority.key);
        resolved.state = CompanionActivationJournalState::Invalid;
        resolved.errorCode = QStringLiteral("activation-journal-serial-drift");
        return resolved;
    }
    resolved.state = CompanionActivationJournalState::Ready;
    return resolved;
}

// 先在安全存储中预留序号与目标 MAC，再写 QSettings 记录，最后提交锚点。
// 任何中断都留下一个可确定性恢复的预留阶段。
bool commitMutation(CompanionActivationJournalSecureStore *store,
                    QSettings *settings, Resolved *resolved,
                    const QByteArray &targetBytes, bool targetAbsent,
                    qint64 targetSerial, QString *errorCode)
{
    Authority reserved = resolved->authority;
    reserved.reservedPresent = true;
    reserved.reservedTargetAbsent = targetAbsent;
    reserved.reservedTargetRecordMac = targetAbsent
        ? QString() : recordMac(reserved.key, targetBytes);
    reserved.reservedTargetSerial = targetAbsent ? 0 : targetSerial;
    if (!targetAbsent) {
        if (reserved.reservedTargetRecordMac.isEmpty()) {
            fail(errorCode, QStringLiteral("activation-journal-mac-failed"));
            return false;
        }
        reserved.highestSerial = targetSerial;
    }
    reserved.bytes = authorityBytes(reserved);
    QString reserveError;
    const AuthorityWriteResult reservation = writeAuthority(
        store, reserved.bytes, resolved->authority.bytes,
        !resolved->authority.committedPresent
            && resolved->authority.bytes.isEmpty(),
        &reserveError);
    if (reservation != AuthorityWriteResult::Expected) {
        fail(errorCode, reservation == AuthorityWriteResult::Previous
             ? QStringLiteral("activation-journal-reserve-failed") : reserveError);
        return false;
    }

    if (targetAbsent) {
        settings->remove(kRecordKey);
        settings->remove(kLegacyIdentityKey);
    } else {
        settings->setValue(kRecordKey, targetBytes);
        settings->remove(kLegacyIdentityKey);
    }
    settings->sync();
    RecordBytes observed;
    QString readError;
    if (settings->status() != QSettings::NoError
            || !readRecordBytes(settings, &observed, &readError)
            || observed.present != !targetAbsent
            || (!targetAbsent && observed.bytes != targetBytes)) {
        // 记录字节未确认。保留预留阶段，让下一次解析确定性地恢复。
        fail(errorCode, QStringLiteral("activation-journal-record-write-unknown"));
        return false;
    }

    Authority committed = reserved;
    committed.reservedPresent = false;
    committed.reservedTargetAbsent = false;
    committed.reservedTargetRecordMac.clear();
    committed.reservedTargetSerial = 0;
    if (targetAbsent) {
        committed.committedPresent = false;
        committed.committedRecordMac.clear();
        committed.committedSerial = 0;
    } else {
        committed.committedPresent = true;
        committed.committedRecordMac = reserved.reservedTargetRecordMac;
        committed.committedSerial = targetSerial;
    }
    committed.bytes = authorityBytes(committed);
    QString commitError;
    const AuthorityWriteResult commit = writeAuthority(
        store, committed.bytes, reserved.bytes, false, &commitError);
    if (commit != AuthorityWriteResult::Expected) {
        fail(errorCode, commit == AuthorityWriteResult::Previous
             ? QStringLiteral("activation-journal-commit-failed") : commitError);
        return false;
    }
    resolved->authority = committed;
    return true;
}

} // namespace

CompanionActivationJournalResult CompanionActivationJournal::load()
{
    Resolved resolved = resolve(m_secureStore, m_settings);
    cleanse(&resolved.authority.key);
    CompanionActivationJournalResult result;
    result.state = resolved.state;
    result.errorCode = resolved.errorCode;
    if (resolved.state == CompanionActivationJournalState::Ready) {
        result.record = resolved.parsed;
    }
    return result;
}

bool CompanionActivationJournal::create(
    const CompanionActivationRecord &record, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    Resolved resolved = resolve(m_secureStore, m_settings);
    if (resolved.state != CompanionActivationJournalState::Empty) {
        cleanse(&resolved.authority.key);
        fail(errorCode, resolved.state == CompanionActivationJournalState::Ready
             ? QStringLiteral("activation-journal-not-empty")
             : (resolved.errorCode.isEmpty()
                ? QStringLiteral("activation-journal-not-empty")
                : resolved.errorCode));
        return false;
    }
    if (!resolved.authority.committedPresent
            && resolved.authority.key.isEmpty()) {
        // 首次使用：生成一次性 MAC 密钥并把授权写入安全存储。
        unsigned char raw[32]{};
        if (RAND_bytes(raw, sizeof(raw)) != 1) {
            OPENSSL_cleanse(raw, sizeof(raw));
            fail(errorCode, QStringLiteral("activation-journal-key-generation-failed"));
            return false;
        }
        resolved.authority.key =
            QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
        OPENSSL_cleanse(raw, sizeof(raw));
        resolved.authority.keyEncoded = resolved.authority.key.toBase64();
        resolved.authority.bytes.clear();
    }
    CompanionActivationRecord candidate = record;
    candidate.stage = CompanionActivationStage::Prepared;
    candidate.receipt.appliedFilesIdentity.clear();
    candidate.serial = resolved.authority.highestSerial + 1;
    candidate.identity.clear();
    if (candidate.serial <= 0 || candidate.serial > kMaximumSerial
            || !validateRecord(&candidate, errorCode)) {
        cleanse(&resolved.authority.key);
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("activation-journal-serial-exhausted");
        }
        return false;
    }
    const bool committed = commitMutation(
        m_secureStore, m_settings, &resolved, serializeRecord(candidate), false,
        candidate.serial, errorCode);
    cleanse(&resolved.authority.key);
    return committed;
}

bool CompanionActivationJournal::advance(
    const QString &expectedIdentity, CompanionActivationStage nextStage,
    const ConfigurationApplyReceipt &receipt,
    CompanionActivationRecord *updated, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    Resolved resolved = resolve(m_secureStore, m_settings);
    if (resolved.state != CompanionActivationJournalState::Ready
            || resolved.parsed.identity != expectedIdentity) {
        cleanse(&resolved.authority.key);
        fail(errorCode, QStringLiteral("activation-journal-cas-conflict"));
        return false;
    }
    const CompanionActivationRecord current = resolved.parsed;
    // 每一步提交都必须先持久化"已请求"意图，恢复才能不做推断。
    CompanionActivationStage expectedNext =
        CompanionActivationStage::ProfileCommitted;
    switch (current.stage) {
    case CompanionActivationStage::Prepared:
        expectedNext = CompanionActivationStage::FilesApplied;
        break;
    case CompanionActivationStage::FilesApplied:
        expectedNext = current.receipt.gatewayMode
            ? CompanionActivationStage::GatewayCommitRequested
            : CompanionActivationStage::ProfileCommitRequested;
        break;
    case CompanionActivationStage::GatewayCommitRequested:
        expectedNext = CompanionActivationStage::GatewayCommitted;
        break;
    case CompanionActivationStage::GatewayCommitted:
        expectedNext = CompanionActivationStage::ProfileCommitRequested;
        break;
    case CompanionActivationStage::ProfileCommitRequested:
        expectedNext = CompanionActivationStage::ProfileCommitted;
        break;
    case CompanionActivationStage::ProfileCommitted:
        break;
    }
    if (current.stage == CompanionActivationStage::ProfileCommitted
            || nextStage != expectedNext
            || receipt.tool != current.receipt.tool
            || receipt.backupId != current.receipt.backupId
            || receipt.backupManifestIdentity
                != current.receipt.backupManifestIdentity
            || receipt.sourceFilesIdentity != current.receipt.sourceFilesIdentity
            || receipt.candidateFilesIdentity
                != current.receipt.candidateFilesIdentity
            || receipt.gatewayMode != current.receipt.gatewayMode) {
        cleanse(&resolved.authority.key);
        fail(errorCode, QStringLiteral("activation-journal-transition-invalid"));
        return false;
    }
    CompanionActivationRecord candidate = current;
    candidate.stage = nextStage;
    candidate.receipt = receipt;
    candidate.serial = resolved.authority.highestSerial + 1;
    candidate.identity.clear();
    if (candidate.serial <= 0 || candidate.serial > kMaximumSerial
            || !validateRecord(&candidate, errorCode)) {
        cleanse(&resolved.authority.key);
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("activation-journal-serial-exhausted");
        }
        return false;
    }
    const bool committed = commitMutation(
        m_secureStore, m_settings, &resolved, serializeRecord(candidate), false,
        candidate.serial, errorCode);
    cleanse(&resolved.authority.key);
    if (committed && updated) *updated = candidate;
    return committed;
}

bool CompanionActivationJournal::clear(
    const QString &expectedIdentity, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    Resolved resolved = resolve(m_secureStore, m_settings);
    if (resolved.state != CompanionActivationJournalState::Ready
            || resolved.parsed.identity != expectedIdentity) {
        cleanse(&resolved.authority.key);
        fail(errorCode, QStringLiteral("activation-journal-cas-conflict"));
        return false;
    }
    const bool committed = commitMutation(
        m_secureStore, m_settings, &resolved, QByteArray(), true, 0, errorCode);
    cleanse(&resolved.authority.key);
    return committed;
}
