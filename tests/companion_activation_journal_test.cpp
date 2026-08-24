#include "companion_activation_journal.h"

#include <QCoreApplication>
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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    QSettings settings(root.filePath(QStringLiteral("journal.ini")), QSettings::IniFormat);
    CompanionActivationJournal journal(&settings);
    if (!expect(journal.load().state == CompanionActivationJournalState::Empty,
                "fresh journal is not empty")) return 1;

    CompanionActivationRecord record = prepared(true);
    QString error;
    if (!expect(journal.create(record, &error), "prepared journal create failed")) return 1;
    CompanionActivationJournalResult loaded = journal.load();
    if (!expect(loaded.state == CompanionActivationJournalState::Ready
                    && loaded.record.stage == CompanionActivationStage::Prepared,
                "prepared journal did not round trip")) return 1;
    if (!expect(!journal.create(record, &error), "second journal create was accepted")) return 1;

    ConfigurationApplyReceipt applied = loaded.record.receipt;
    applied.appliedFilesIdentity = applied.candidateFilesIdentity;
    CompanionActivationRecord filesApplied;
    if (!expect(journal.advance(
                    loaded.record.identity, CompanionActivationStage::FilesApplied,
                    applied, &filesApplied, &error),
                "files-applied transition failed")) return 1;
    if (!expect(!journal.advance(
                    loaded.record.identity, CompanionActivationStage::GatewayCommitted,
                    applied, nullptr, &error),
                "stale journal CAS was accepted")) return 1;
    if (!expect(journal.advance(
                    filesApplied.identity, CompanionActivationStage::GatewayCommitted,
                    applied, &record, &error),
                "gateway transition failed")) return 1;
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

    CompanionActivationRecord direct = prepared(false);
    if (!expect(journal.create(direct, &error), "direct journal create failed")) return 1;
    loaded = journal.load();
    ConfigurationApplyReceipt directApplied = loaded.record.receipt;
    directApplied.appliedFilesIdentity = directApplied.candidateFilesIdentity;
    if (!expect(journal.advance(
                    loaded.record.identity, CompanionActivationStage::FilesApplied,
                    directApplied, &record, &error)
                    && journal.advance(
                        record.identity, CompanionActivationStage::ProfileCommitted,
                        directApplied, &record, &error),
                "direct journal transition failed")) return 1;
    if (!expect(!journal.advance(
                    record.identity, CompanionActivationStage::GatewayCommitted,
                    directApplied, nullptr, &error),
                "direct journal accepted a gateway-only transition")) return 1;
    settings.setValue(
        QStringLiteral("companion/activation-journal/record"), QByteArrayLiteral("{}"));
    settings.sync();
    if (!expect(journal.load().state == CompanionActivationJournalState::Invalid,
                "tampered journal was accepted")) return 1;

    settings.remove(QStringLiteral("companion/activation-journal/record"));
    settings.sync();
    if (!expect(journal.load().state == CompanionActivationJournalState::Invalid,
                "partial journal deletion was accepted")) return 1;
    return 0;
}
