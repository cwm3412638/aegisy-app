#include "companion_activation_journal.h"

#include "configuration_backup_store.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

namespace {

const QString kRecordKey = QStringLiteral("companion/activation-journal/record");
const QString kIdentityKey = QStringLiteral("companion/activation-journal/identity");

QString stageName(CompanionActivationStage stage)
{
    switch (stage) {
    case CompanionActivationStage::Prepared: return QStringLiteral("prepared");
    case CompanionActivationStage::FilesApplied: return QStringLiteral("files-applied");
    case CompanionActivationStage::GatewayCommitted:
        return QStringLiteral("gateway-committed");
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
    } else if (value == QStringLiteral("gateway-committed")) {
        *stage = CompanionActivationStage::GatewayCommitted;
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

bool validHashIdentity(const QString &value, const QString &prefix)
{
    if (!value.startsWith(prefix) || value.size() != prefix.size() + 64) return false;
    for (const QChar character : value.mid(prefix.size())) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

void append(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

} // namespace

CompanionActivationJournal::CompanionActivationJournal(QSettings *settings)
    : m_settings(settings)
{
}

QString CompanionActivationJournal::identityFor(
    const CompanionActivationRecord &record)
{
    QByteArray input = QByteArrayLiteral("aegisy-companion-activation-journal/0.2\0");
    append(&input, record.transactionId.toUtf8());
    append(&input, record.originalProfileId.toUtf8());
    append(&input, record.candidateProfileId.toUtf8());
    append(&input, record.candidateProfileIdentity.toUtf8());
    append(&input, record.candidateTemporary ? QByteArrayLiteral("1")
                                              : QByteArrayLiteral("0"));
    append(&input, stageName(record.stage).toUtf8());
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

bool CompanionActivationJournal::validate(
    CompanionActivationRecord *record, QString *errorCode)
{
    if (!record || !validUuid(record->transactionId)
            || !validUuid(record->originalProfileId, true)
            || !validUuid(record->candidateProfileId)
            || !validHashIdentity(record->candidateProfileIdentity,
                                  QStringLiteral("profile-activation:sha256:"))
            || !ConfigurationBackupStore::isValidBackupId(record->receipt.backupId)
            || !validHashIdentity(record->receipt.backupManifestIdentity,
                                  QStringLiteral("configuration-backup-manifest:sha256:"))
            || !validHashIdentity(record->receipt.sourceFilesIdentity,
                                  QStringLiteral("configuration-files:sha256:"))
            || !validHashIdentity(record->receipt.candidateFilesIdentity,
                                  QStringLiteral("configuration-files:sha256:"))
            || (static_cast<int>(record->receipt.tool) < 0
                || static_cast<int>(record->receipt.tool) > 3)) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-record-invalid");
        return false;
    }
    if (record->candidateTemporary
            && (record->originalProfileId.isEmpty()
                || record->originalProfileId == record->candidateProfileId)) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-record-invalid");
        return false;
    }
    const bool applied = record->stage != CompanionActivationStage::Prepared;
    if (applied != validHashIdentity(
            record->receipt.appliedFilesIdentity,
            QStringLiteral("configuration-files:sha256:"))
            || (applied
                && record->receipt.appliedFilesIdentity
                    != record->receipt.candidateFilesIdentity)
            || (record->stage == CompanionActivationStage::GatewayCommitted
                && !record->receipt.gatewayMode)) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-stage-invalid");
        return false;
    }
    const QString identity = identityFor(*record);
    if (!record->identity.isEmpty() && record->identity != identity) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-identity-invalid");
        return false;
    }
    record->identity = identity;
    return true;
}

QByteArray CompanionActivationJournal::serialize(
    const CompanionActivationRecord &record)
{
    const QJsonObject object{
        {QStringLiteral("schema"), QStringLiteral("aegisy-companion-activation-journal/0.2")},
        {QStringLiteral("transaction_id"), record.transactionId},
        {QStringLiteral("original_profile_id"), record.originalProfileId},
        {QStringLiteral("candidate_profile_id"), record.candidateProfileId},
        {QStringLiteral("candidate_profile_identity"), record.candidateProfileIdentity},
        {QStringLiteral("candidate_temporary"), record.candidateTemporary},
        {QStringLiteral("stage"), stageName(record.stage)},
        {QStringLiteral("tool"), static_cast<int>(record.receipt.tool)},
        {QStringLiteral("backup_id"), record.receipt.backupId},
        {QStringLiteral("backup_manifest_identity"), record.receipt.backupManifestIdentity},
        {QStringLiteral("source_files_identity"), record.receipt.sourceFilesIdentity},
        {QStringLiteral("candidate_files_identity"),
         record.receipt.candidateFilesIdentity},
        {QStringLiteral("applied_files_identity"), record.receipt.appliedFilesIdentity},
        {QStringLiteral("gateway_mode"), record.receipt.gatewayMode},
        {QStringLiteral("identity"), record.identity},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool CompanionActivationJournal::deserialize(
    const QByteArray &bytes, CompanionActivationRecord *record, QString *errorCode)
{
    if (!record || bytes.isEmpty() || bytes.size() > 16 * 1024) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema"), QStringLiteral("transaction_id"),
        QStringLiteral("original_profile_id"), QStringLiteral("candidate_profile_id"),
        QStringLiteral("candidate_profile_identity"), QStringLiteral("stage"),
        QStringLiteral("candidate_temporary"),
        QStringLiteral("tool"), QStringLiteral("backup_id"),
        QStringLiteral("backup_manifest_identity"), QStringLiteral("source_files_identity"),
        QStringLiteral("candidate_files_identity"),
        QStringLiteral("applied_files_identity"), QStringLiteral("gateway_mode"),
        QStringLiteral("identity")};
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema")).toString()
                != QStringLiteral("aegisy-companion-activation-journal/0.2")
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
            || !object.value(QStringLiteral("identity")).isString()) return false;
    CompanionActivationRecord parsed;
    parsed.transactionId = object.value(QStringLiteral("transaction_id")).toString();
    parsed.originalProfileId = object.value(QStringLiteral("original_profile_id")).toString();
    parsed.candidateProfileId = object.value(QStringLiteral("candidate_profile_id")).toString();
    parsed.candidateProfileIdentity = object.value(
        QStringLiteral("candidate_profile_identity")).toString();
    parsed.candidateTemporary = object.value(
        QStringLiteral("candidate_temporary")).toBool();
    if (!parseStage(object.value(QStringLiteral("stage")).toString(), &parsed.stage)) return false;
    const double toolNumber = object.value(QStringLiteral("tool")).toDouble(-1);
    if (toolNumber < 0 || toolNumber > 3 || static_cast<int>(toolNumber) != toolNumber) return false;
    parsed.receipt.tool = static_cast<AiTool>(static_cast<int>(toolNumber));
    parsed.receipt.backupId = object.value(QStringLiteral("backup_id")).toString();
    parsed.receipt.backupManifestIdentity = object.value(
        QStringLiteral("backup_manifest_identity")).toString();
    parsed.receipt.sourceFilesIdentity = object.value(
        QStringLiteral("source_files_identity")).toString();
    parsed.receipt.candidateFilesIdentity = object.value(
        QStringLiteral("candidate_files_identity")).toString();
    parsed.receipt.appliedFilesIdentity = object.value(
        QStringLiteral("applied_files_identity")).toString();
    parsed.receipt.gatewayMode = object.value(QStringLiteral("gateway_mode")).toBool();
    parsed.identity = object.value(QStringLiteral("identity")).toString();
    if (!validate(&parsed, errorCode)) return false;
    *record = parsed;
    return true;
}

CompanionActivationJournalResult CompanionActivationJournal::load() const
{
    if (!m_settings) return {CompanionActivationJournalState::Unavailable, {},
                             QStringLiteral("activation-journal-unavailable")};
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        return {CompanionActivationJournalState::Unavailable, {},
                QStringLiteral("activation-journal-unavailable")};
    }
    const bool hasRecord = m_settings->contains(kRecordKey);
    const bool hasIdentity = m_settings->contains(kIdentityKey);
    if (!hasRecord && !hasIdentity) return {CompanionActivationJournalState::Empty, {}, {}};
    if (!hasRecord || !hasIdentity) {
        return {CompanionActivationJournalState::Invalid, {},
                QStringLiteral("activation-journal-partial")};
    }
    const QByteArray bytes = m_settings->value(kRecordKey).toByteArray();
    CompanionActivationRecord record;
    QString error;
    if (!deserialize(bytes, &record, &error)
            || m_settings->value(kIdentityKey).toString() != record.identity) {
        return {CompanionActivationJournalState::Invalid, {},
                error.isEmpty() ? QStringLiteral("activation-journal-invalid") : error};
    }
    return {CompanionActivationJournalState::Ready, record, {}};
}

bool CompanionActivationJournal::create(
    const CompanionActivationRecord &record, QString *errorCode)
{
    if (!m_settings || load().state != CompanionActivationJournalState::Empty) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-not-empty");
        return false;
    }
    CompanionActivationRecord candidate = record;
    candidate.stage = CompanionActivationStage::Prepared;
    candidate.receipt.appliedFilesIdentity.clear();
    candidate.identity.clear();
    if (!validate(&candidate, errorCode)) return false;
    const QByteArray bytes = serialize(candidate);
    m_settings->setValue(kRecordKey, bytes);
    m_settings->setValue(kIdentityKey, candidate.identity);
    m_settings->sync();
    const CompanionActivationJournalResult verified = load();
    if (verified.state != CompanionActivationJournalState::Ready
            || verified.record.identity != candidate.identity
            || m_settings->value(kRecordKey).toByteArray() != bytes) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-write-unknown");
        return false;
    }
    return true;
}

bool CompanionActivationJournal::advance(
    const QString &expectedIdentity, CompanionActivationStage nextStage,
    const ConfigurationApplyReceipt &receipt,
    CompanionActivationRecord *updated, QString *errorCode)
{
    const CompanionActivationJournalResult current = load();
    if (current.state != CompanionActivationJournalState::Ready
            || current.record.identity != expectedIdentity) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-cas-conflict");
        return false;
    }
    const CompanionActivationStage expectedNext =
        current.record.stage == CompanionActivationStage::Prepared
        ? CompanionActivationStage::FilesApplied
        : (current.record.stage == CompanionActivationStage::FilesApplied
            ? (current.record.receipt.gatewayMode
                ? CompanionActivationStage::GatewayCommitted
                : CompanionActivationStage::ProfileCommitted)
            : CompanionActivationStage::ProfileCommitted);
    if (current.record.stage == CompanionActivationStage::ProfileCommitted
            || nextStage != expectedNext
            || receipt.tool != current.record.receipt.tool
            || receipt.backupId != current.record.receipt.backupId
            || receipt.backupManifestIdentity
                != current.record.receipt.backupManifestIdentity
            || receipt.sourceFilesIdentity != current.record.receipt.sourceFilesIdentity
            || receipt.candidateFilesIdentity
                != current.record.receipt.candidateFilesIdentity
            || receipt.gatewayMode != current.record.receipt.gatewayMode) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-transition-invalid");
        return false;
    }
    CompanionActivationRecord candidate = current.record;
    candidate.stage = nextStage;
    candidate.receipt = receipt;
    candidate.identity.clear();
    if (!validate(&candidate, errorCode)) return false;
    const QByteArray bytes = serialize(candidate);
    m_settings->setValue(kRecordKey, bytes);
    m_settings->setValue(kIdentityKey, candidate.identity);
    m_settings->sync();
    const CompanionActivationJournalResult verified = load();
    if (verified.state != CompanionActivationJournalState::Ready
            || verified.record.identity != candidate.identity
            || m_settings->value(kRecordKey).toByteArray() != bytes) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-write-unknown");
        return false;
    }
    if (updated) *updated = candidate;
    return true;
}

bool CompanionActivationJournal::clear(
    const QString &expectedIdentity, QString *errorCode)
{
    const CompanionActivationJournalResult current = load();
    if (current.state != CompanionActivationJournalState::Ready
            || current.record.identity != expectedIdentity) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-cas-conflict");
        return false;
    }
    m_settings->remove(kRecordKey);
    m_settings->remove(kIdentityKey);
    m_settings->sync();
    if (load().state != CompanionActivationJournalState::Empty) {
        if (errorCode) *errorCode = QStringLiteral("activation-journal-clear-unknown");
        return false;
    }
    return true;
}
