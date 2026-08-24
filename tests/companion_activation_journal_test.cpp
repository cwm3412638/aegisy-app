#include "companion_activation_journal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

QString hash(const QString &prefix, QChar fill)
{
    return prefix + QString(64, fill);
}

// 可注入的授权信封替身：可以脚本化单次写入失败、结果未知，以及后端不可用/损坏的读取。
class FakeSecureStore final : public CompanionActivationJournalSecureStore
{
public:
    struct WriteScript
    {
        int call = 0;
        WriteOutcome outcome = WriteOutcome::Committed;
        bool apply = false;
        int unavailableReads = 0;
        int invalidReads = 0;
    };

    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        if (!value) return ReadState::Invalid;
        value->clear();
        if (m_unavailableReads > 0) {
            --m_unavailableReads;
            return ReadState::Unavailable;
        }
        if (m_invalidReads > 0) {
            --m_invalidReads;
            return ReadState::Invalid;
        }
        if (!m_present) return ReadState::Missing;
        *value = m_value;
        return ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        ++m_writes;
        if (m_script.call == m_writes) {
            if (m_script.apply) {
                m_value = value;
                m_present = true;
            }
            m_unavailableReads = m_script.unavailableReads;
            m_invalidReads = m_script.invalidReads;
            const WriteOutcome outcome = m_script.outcome;
            m_script = WriteScript{};
            return outcome;
        }
        m_value = value;
        m_present = true;
        return WriteOutcome::Committed;
    }

    void script(const WriteScript &script) { m_script = script; m_writes = 0; }
    int writes() const { return m_writes; }
    bool present() const { return m_present; }
    QByteArray value() const { return m_value; }
    void setValue(const QByteArray &value) { m_value = value; m_present = true; }
    void clear() { m_value.clear(); m_present = false; }

private:
    WriteScript m_script;
    int m_writes = 0;
    int m_unavailableReads = 0;
    int m_invalidReads = 0;
    bool m_present = false;
    QByteArray m_value;
};

CompanionActivationRecord prepared(bool gateway)
{
    CompanionActivationRecord record;
    record.transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.originalProfileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.candidateProfileId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.candidateProfileIdentity = hash(
        QStringLiteral("profile-activation:sha256:"), QLatin1Char('a'));
    record.candidateTemporary = true;
    record.receipt.tool = AiTool::CodexCli;
    record.receipt.backupId = QStringLiteral("20260824_010203_004_deadbeef");
    record.receipt.backupManifestIdentity = hash(
        QStringLiteral("configuration-backup-manifest:sha256:"), QLatin1Char('b'));
    record.receipt.sourceFilesIdentity = hash(
        QStringLiteral("configuration-files:sha256:"), QLatin1Char('c'));
    record.receipt.candidateFilesIdentity = hash(
        QStringLiteral("configuration-files:sha256:"), QLatin1Char('d'));
    record.receipt.gatewayMode = gateway;
    return record;
}

const QString kRecordKey =
    QStringLiteral("companion/activation-journal/record");

QJsonObject authorityObject(const FakeSecureStore &store)
{
    return QJsonDocument::fromJson(store.value()).object();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    QSettings settings(root.filePath(QStringLiteral("journal.ini")), QSettings::IniFormat);
    FakeSecureStore store;
    CompanionActivationJournal journal(&store, &settings);
    if (!expect(journal.load().state == CompanionActivationJournalState::Empty,
                "fresh journal is not empty")) return 1;

    CompanionActivationRecord record = prepared(true);
    QString error;
    if (!expect(journal.create(record, &error), "prepared journal create failed")) return 1;
    if (!expect(store.present()
                    && authorityObject(store).value(QStringLiteral("phase")).toString()
                        == QStringLiteral("committed"),
                "create did not commit an authority envelope")) return 1;
    CompanionActivationJournalResult loaded = journal.load();
    if (!expect(loaded.state == CompanionActivationJournalState::Ready
                    && loaded.record.stage == CompanionActivationStage::Prepared
                    && loaded.record.serial == 1,
                "prepared journal did not round trip")) return 1;
    if (!expect(!journal.create(record, &error), "second journal create was accepted")) return 1;

    ConfigurationApplyReceipt applied = loaded.record.receipt;
    applied.appliedFilesIdentity = applied.candidateFilesIdentity;
    CompanionActivationRecord filesApplied;
    if (!expect(journal.advance(
                    loaded.record.identity, CompanionActivationStage::FilesApplied,
                    applied, &filesApplied, &error),
                "files-applied transition failed")) return 1;
    if (!expect(filesApplied.serial == 2, "advance did not consume a fresh serial")) return 1;
    if (!expect(!journal.advance(
                    loaded.record.identity, CompanionActivationStage::GatewayCommitted,
                    applied, nullptr, &error)
                    && error == QStringLiteral("activation-journal-cas-conflict"),
                "stale journal CAS was accepted")) return 1;
    // 网关提交必须先经过"已请求"意图，不能从 FilesApplied 直接跳到已提交。
    if (!expect(!journal.advance(
                    filesApplied.identity, CompanionActivationStage::GatewayCommitted,
                    applied, nullptr, &error)
                    && error == QStringLiteral("activation-journal-transition-invalid"),
                "gateway commit skipped the commit-requested intent")) return 1;
    if (!expect(journal.advance(
                    filesApplied.identity,
                    CompanionActivationStage::GatewayCommitRequested,
                    applied, &record, &error),
                "gateway commit-requested transition failed")) return 1;
    if (!expect(journal.advance(
                    record.identity, CompanionActivationStage::GatewayCommitted,
                    applied, &record, &error),
                "gateway transition failed")) return 1;
    if (!expect(!journal.advance(
                    record.identity, CompanionActivationStage::ProfileCommitted,
                    applied, nullptr, &error),
                "profile commit skipped the commit-requested intent")) return 1;
    if (!expect(journal.advance(
                    record.identity,
                    CompanionActivationStage::ProfileCommitRequested,
                    applied, &record, &error),
                "profile commit-requested transition failed")) return 1;
    if (!expect(journal.advance(
                    record.identity, CompanionActivationStage::ProfileCommitted,
                    applied, &record, &error),
                "profile transition failed")) return 1;
    if (!expect(!journal.advance(
                    record.identity, CompanionActivationStage::ProfileCommitted,
                    applied, nullptr, &error),
                "terminal journal advanced twice")) return 1;
    if (!expect(journal.clear(record.identity, &error)
                    && journal.load().state == CompanionActivationJournalState::Empty,
                "journal clear failed")) return 1;
    if (!expect(store.present()
                    && authorityObject(store).value(QStringLiteral("committed")).isNull()
                    && authorityObject(store).value(
                        QStringLiteral("highest_serial")).toDouble() >= 4,
                "clear did not retain a monotonic authority anchor")) return 1;

    CompanionActivationRecord direct = prepared(false);
    if (!expect(journal.create(direct, &error), "direct journal create failed")) return 1;
    loaded = journal.load();
    if (!expect(loaded.record.serial > 4, "reused a retired serial")) return 1;
    ConfigurationApplyReceipt directApplied = loaded.record.receipt;
    directApplied.appliedFilesIdentity = directApplied.candidateFilesIdentity;
    if (!expect(journal.advance(
                    loaded.record.identity, CompanionActivationStage::FilesApplied,
                    directApplied, &record, &error),
                "direct files-applied transition failed")) return 1;
    if (!expect(!journal.advance(
                    record.identity,
                    CompanionActivationStage::GatewayCommitRequested,
                    directApplied, nullptr, &error),
                "direct journal accepted a gateway commit intent")) return 1;
    if (!expect(journal.advance(
                    record.identity,
                    CompanionActivationStage::ProfileCommitRequested,
                    directApplied, &record, &error)
                    && journal.advance(
                        record.identity, CompanionActivationStage::ProfileCommitted,
                        directApplied, &record, &error),
                "direct journal transition failed")) return 1;
    if (!expect(!journal.advance(
                    record.identity, CompanionActivationStage::GatewayCommitted,
                    directApplied, nullptr, &error),
                "direct journal accepted a gateway-only transition")) return 1;

    const QByteArray authenticRecord =
        settings.value(kRecordKey).toByteArray();
    const QByteArray committedAuthority = store.value();
    settings.setValue(kRecordKey, QByteArrayLiteral("{}"));
    settings.sync();
    if (!expect(journal.load().state == CompanionActivationJournalState::Invalid,
                "tampered journal was accepted")) return 1;

    settings.remove(kRecordKey);
    settings.sync();
    CompanionActivationJournalResult deleted = journal.load();
    if (!expect(deleted.state == CompanionActivationJournalState::Invalid
                    && deleted.errorCode
                        == QStringLiteral("activation-journal-record-deleted"),
                "deleting the record degraded to an empty journal")) return 1;

    // 授权缺失但记录字节存在：无法认证的记录不得被当作可信事务或空状态。
    settings.setValue(kRecordKey, authenticRecord);
    settings.sync();
    store.clear();
    CompanionActivationJournalResult orphan = journal.load();
    if (!expect(orphan.state == CompanionActivationJournalState::Invalid
                    && orphan.errorCode
                        == QStringLiteral("activation-journal-record-without-authority"),
                "record bytes without an authority were accepted")) return 1;

    // 重新计算本地身份无法伪造事务：MAC 密钥只存在于安全存储中。
    store.setValue(committedAuthority);
    if (!expect(journal.load().state == CompanionActivationJournalState::Ready,
                "authentic record and authority did not reload")) return 1;
    QJsonObject forged = authorityObject(store);
    forged.insert(QStringLiteral("hmac_key_base64"),
                  QString::fromLatin1(QByteArray(32, '\x01').toBase64()));
    store.setValue(QJsonDocument(forged).toJson(QJsonDocument::Compact));
    CompanionActivationJournalResult rekeyed = journal.load();
    if (!expect(rekeyed.state == CompanionActivationJournalState::Invalid
                    && rekeyed.errorCode
                        == QStringLiteral("activation-journal-record-unauthenticated"),
                "a substituted MAC key still authenticated the record")) return 1;
    store.setValue(committedAuthority);

    // 后端锁定必须报告不可用,不能被误读成首次安装。
    QSettings lockedSettings(
        root.filePath(QStringLiteral("locked.ini")), QSettings::IniFormat);
    FakeSecureStore lockedStore;
    CompanionActivationJournal lockedJournal(&lockedStore, &lockedSettings);
    FakeSecureStore::WriteScript lockedReads;
    lockedReads.call = 1;
    lockedReads.unavailableReads = 2;
    lockedStore.script(lockedReads);
    lockedStore.write(QByteArrayLiteral("{}"), nullptr);
    if (!expect(lockedJournal.load().state
                    == CompanionActivationJournalState::Unavailable,
                "a locked authority backend was read as an empty journal")) return 1;
    if (!expect(!lockedJournal.create(prepared(true), &error),
                "a locked authority backend accepted a new transaction")) return 1;

    settings.remove(kRecordKey);
    settings.sync();
    store.clear();
    if (!expect(journal.load().state == CompanionActivationJournalState::Empty,
                "cleared fake store is not empty")) return 1;

    QSettings restartSettings(
        root.filePath(QStringLiteral("restart.ini")), QSettings::IniFormat);
    FakeSecureStore restartStore;
    CompanionActivationJournal restartJournal(&restartStore, &restartSettings);
    CompanionActivationRecord restartRecord = prepared(true);
    // 提交授权失败:预留阶段留在安全存储中,由下一次解析确定性恢复到候选态。
    FakeSecureStore::WriteScript commitLost;
    commitLost.call = 2;
    commitLost.outcome = CompanionActivationJournalSecureStore::WriteOutcome::DefiniteFailure;
    restartStore.script(commitLost);
    if (!expect(!restartJournal.create(restartRecord, &error)
                    && error == QStringLiteral("activation-journal-commit-failed"),
                "lost authority commit was reported as success")) return 1;
    if (!expect(authorityObject(restartStore).value(QStringLiteral("phase")).toString()
                    == QStringLiteral("reserved"),
                "interrupted create did not leave a reserved phase")) return 1;
    CompanionActivationJournalResult recovered = restartJournal.load();
    if (!expect(recovered.state == CompanionActivationJournalState::Ready
                    && recovered.record.stage == CompanionActivationStage::Prepared
                    && recovered.record.serial == 1,
                "reserved-phase candidate was not deterministically recovered")) return 1;
    if (!expect(authorityObject(restartStore).value(QStringLiteral("phase")).toString()
                    == QStringLiteral("committed"),
                "recovery did not settle the reserved phase")) return 1;

    // 第三种记录状态不可推断:既不是预映像也不是预留候选。
    QSettings thirdSettings(
        root.filePath(QStringLiteral("third.ini")), QSettings::IniFormat);
    FakeSecureStore thirdStore;
    CompanionActivationJournal thirdJournal(&thirdStore, &thirdSettings);
    CompanionActivationRecord thirdRecord = prepared(false);
    FakeSecureStore::WriteScript thirdScript;
    thirdScript.call = 2;
    thirdScript.outcome =
        CompanionActivationJournalSecureStore::WriteOutcome::DefiniteFailure;
    thirdStore.script(thirdScript);
    if (!expect(!thirdJournal.create(thirdRecord, &error),
                "interrupted third-state create was reported as success")) return 1;
    thirdSettings.setValue(kRecordKey, QByteArrayLiteral("{\"schema\":\"x\"}"));
    thirdSettings.sync();
    CompanionActivationJournalResult third = thirdJournal.load();
    if (!expect(third.state == CompanionActivationJournalState::Invalid
                    && third.errorCode
                        == QStringLiteral("activation-journal-reserved-third-state"),
                "an unclassifiable reserved record was resolved anyway")) return 1;

    // 授权写入结果未知时必须诚实报告,而不是假装事务已提交。
    QSettings unknownSettings(
        root.filePath(QStringLiteral("unknown.ini")), QSettings::IniFormat);
    FakeSecureStore unknownStore;
    CompanionActivationJournal unknownJournal(&unknownStore, &unknownSettings);
    FakeSecureStore::WriteScript unknownScript;
    unknownScript.call = 1;
    unknownScript.outcome =
        CompanionActivationJournalSecureStore::WriteOutcome::OutcomeUnknown;
    unknownScript.unavailableReads = 1;
    unknownStore.script(unknownScript);
    if (!expect(!unknownJournal.create(prepared(true), &error)
                    && error.startsWith(QStringLiteral("activation-journal-authority-")),
                "an unknown authority write outcome was reported as success")) return 1;
    if (!expect(!unknownSettings.contains(kRecordKey),
                "an unreserved transaction wrote record bytes")) return 1;

    // 旧版 0.2 只留下本地身份键:它的残留不得被读成"没有事务"。
    QSettings legacySettings(
        root.filePath(QStringLiteral("legacy.ini")), QSettings::IniFormat);
    FakeSecureStore legacyStore;
    CompanionActivationJournal legacyJournal(&legacyStore, &legacySettings);
    legacySettings.setValue(
        QStringLiteral("companion/activation-journal/identity"),
        QStringLiteral("companion-activation:sha256:")
            + QString(64, QLatin1Char('f')));
    legacySettings.sync();
    CompanionActivationJournalResult legacy = legacyJournal.load();
    if (!expect(legacy.state == CompanionActivationJournalState::Invalid
                    && legacy.errorCode
                        == QStringLiteral("activation-journal-record-without-authority"),
                "a legacy identity remnant was read as an empty journal")) return 1;

    // 没有安全存储就没有权威:必须报告不可用而不是空。
    CompanionActivationJournal detached(nullptr, &legacySettings);
    if (!expect(detached.load().state == CompanionActivationJournalState::Unavailable,
                "a journal without secure storage claimed to be empty")) return 1;
    return 0;
}
