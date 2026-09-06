#include "extension_staging_restore_flow.h"

#include "mcp_configuration_inventory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

// 暂存恢复编排器测试：资格谓词、呈现前门禁（捕获/清点/读回/目标）、记录纪律
// （declined 记录、策略拒绝零执行、审计链退化冻结）与真实端到端恢复（整文件写回、
// already-in-place 零写入、冲突硬拒绝）。全部在临时目录与固定密钥上跑真实加密往返。
namespace {

int failures = 0;

const QString kSubject = QStringLiteral("mcp:claude-settings");
const QString kBackupIdSyntax = QStringLiteral("ext_20260906_120000_ffffffff");

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QDateTime now()
{
    return QDateTime::fromString(QStringLiteral("2026-09-06T12:00:00.000Z"),
                                 Qt::ISODateWithMs);
}

// 固定密钥 provider：暂存域的加密往返不依赖真实安全存储。
class FixedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *key, QString *) override
    {
        if (!key) return false;
        *key = QByteArray(32, 's');
        return true;
    }
};

// 密钥不可得的 provider：恢复前捕获在它上面确定性失败。
class FailingKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *, QString *error) override
    {
        if (error) *error = QStringLiteral("extension-staging-backup-key-unavailable");
        return false;
    }
};

// 可注入的审计安全存储：与控制器测试同一个形状。
class FakeSecureStore final : public ExtensionStagingRestoreAuditSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        if (readState != ReadState::Found) {
            if (errorCode && readState != ReadState::Missing) {
                *errorCode = QStringLiteral("fake-read-unavailable");
            }
            return readState;
        }
        if (stored.isEmpty()) return ReadState::Missing;
        if (value) *value = stored;
        return ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        if (errorCode) errorCode->clear();
        stored = value;
        return WriteOutcome::Committed;
    }

    QByteArray stored;
    ReadState readState = ReadState::Found;
};

struct AuditFixture {
    explicit AuditFixture(const QString &path)
        : settings(path, QSettings::IniFormat)
    {
    }

    FakeSecureStore secure;
    QSettings settings;

    ExtensionStagingRestoreAuditLedgerStore store()
    {
        return ExtensionStagingRestoreAuditLedgerStore(&secure, &settings);
    }
};

// 一份合格的准备结果所经的完整夹具：临时目录 + 设置文件 + 暂存备份根。
struct CaseDir {
    QTemporaryDir temporary;
    QString settingsPath;
    QString backupRoot;

    bool valid()
    {
        return temporary.isValid();
    }

    void layOut(const QString &name)
    {
        settingsPath = temporary.path() + QLatin1Char('/') + name
            + QStringLiteral("/claude/settings.json");
        backupRoot = temporary.path() + QLatin1Char('/') + name
            + QStringLiteral("/backups");
    }
};

// 捕获一份内容为 content 的设置文件备份，返回备份 id。
QString captureBackup(const QString &settingsPath, const QString &backupRoot,
                      const QByteArray &content)
{
    if (!expect(writeFile(settingsPath, content),
                "the settings fixture could not be written")) {
        return {};
    }
    FixedKeyProvider provider;
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(ExtensionStagingBackupCapture::capture(
                    kSubject, settingsPath, backupRoot, &provider, &result,
                    &error),
                "the fixture backup could not be captured")) {
        return {};
    }
    return result.backupId;
}

ExtensionStagingRestoreApprovalAcknowledgement acknowledge(
    const ExtensionStagingRestorePrompt &prompt,
    ExtensionStagingRestoreApprovalDecision decision)
{
    ExtensionStagingRestoreApprovalAcknowledgement value;
    value.decision = decision;
    value.subject = prompt.subject;
    value.backupId = prompt.backupId;
    value.destinationRoot = prompt.destinationRoot;
    value.approvedPlanIdentity = prompt.echoedPlanIdentity;
    value.approvedTreeIdentity = prompt.echoedTreeIdentity;
    value.acknowledgedWarnings = prompt.warnings;
    value.highRiskConfirmed = true;
    return value;
}

bool hasWarning(const ExtensionStagingRestorePrompt &prompt,
                ExtensionStagingRestoreWarning warning)
{
    return prompt.warnings.contains(warning);
}

// 资格谓词：唯一合格的行是"清单身份级验证通过 + mcp:claude-settings"。
void testRestoreOfferedPredicate()
{
    ExtensionStagingBackupListEntry entry;
    entry.backupId = QStringLiteral("ext_20260906_120000_00000001");
    entry.subject = kSubject;
    entry.verification = ExtensionStagingBackupEntryVerification::ListedIntact;
    expect(ExtensionStagingRestoreFlow::isRestoreOffered(entry),
           "an intact mcp:claude-settings row was not offered restore");

    entry.verification = ExtensionStagingBackupEntryVerification::ListedCorrupt;
    expect(!ExtensionStagingRestoreFlow::isRestoreOffered(entry),
           "a corrupt row was offered restore");

    entry.verification = ExtensionStagingBackupEntryVerification::ListedIntact;
    entry.subject = QStringLiteral("skill:alpha");
    expect(!ExtensionStagingRestoreFlow::isRestoreOffered(entry),
           "a skill row was offered restore");

    entry.subject = QStringLiteral("codex-plugin:alpha");
    expect(!ExtensionStagingRestoreFlow::isRestoreOffered(entry),
           "a codex-plugin row was offered restore");

    entry.subject = QStringLiteral("mcp:other-server");
    expect(!ExtensionStagingRestoreFlow::isRestoreOffered(entry),
           "another mcp subject was offered restore");
}

// 参数校验与主体复核先于一切文件系统工作。
void testRequestAndSubjectGates()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("gates"));
    FixedKeyProvider provider;

    ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(
            QString(), QStringLiteral("ext_20260906_120000_00000001"), kSubject,
            dir.backupRoot, &provider, now());
    expect(preparation.stage == QStringLiteral("request")
               && preparation.errorCode
                   == QStringLiteral("extension-restore-flow-request-invalid")
               && !preparation.ok,
           "an empty settings path was not refused at the request gate");

    preparation = ExtensionStagingRestoreFlow::prepare(
        dir.settingsPath, QStringLiteral("ext_20260906_120000_00000001"),
        QStringLiteral("skill:alpha"), dir.backupRoot, &provider, now());
    expect(preparation.stage == QStringLiteral("subject")
               && preparation.errorCode
                   == QStringLiteral("extension-restore-flow-subject-unsupported")
               && !preparation.ok,
           "a skill subject was not refused at the subject gate");
}

// 恢复前捕获失败即整体失败关闭：不动目标、不增备份。
void testCaptureFailureFailsClosed()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("capture-fails"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    if (!expect(writeFile(dir.settingsPath, content),
                "the settings fixture could not be written")) {
        return;
    }
    FailingKeyProvider failing;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(
            dir.settingsPath, QStringLiteral("ext_20260906_120000_00000001"),
            kSubject, dir.backupRoot, &failing, now());
    expect(preparation.stage == QStringLiteral("capture")
               && !preparation.errorCode.isEmpty() && !preparation.ok,
           "a capture failure did not fail closed at the capture stage");
    expect(readFile(dir.settingsPath) == content,
           "the target file was touched by a failed preparation");
    // 没有留下半份备份：清点回答"确实一份都没有"。
    ExtensionStagingBackupListResult listing;
    QString error;
    expect(ExtensionStagingBackupInventory::list(dir.backupRoot, kSubject,
                                                 &listing, &error)
               && listing.state == ExtensionStagingBackupListState::Empty,
           "a failed capture left backup state behind");
}

// 备份在点击与准备之间消失：合法语法但不存在的 id。
void testBackupVanished()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("vanished"));
    // 目标文件不存在：捕获诚实跳过，清点直接回答"没有这份备份"。
    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(
            dir.settingsPath, kBackupIdSyntax, kSubject, dir.backupRoot,
            &provider, now());
    expect(preparation.stage == QStringLiteral("listing")
               && preparation.errorCode
                   == QStringLiteral("extension-restore-flow-backup-vanished")
               && !preparation.ok,
           "a vanished backup was not reported at the listing stage");
    expect(preparation.preRestoreCaptureSkipped,
           "a missing target was not reported as capture-skipped");
}

// 端到端真实恢复：目标文件缺失 → 准备（诚实跳过捕获）→ 逐项对齐批准 → 记录 →
// 执行 → 目标字节与备份逐字节一致，审计条目绑定确切计划与树身份。
void testEndToEndRestore()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("end-to-end"));
    const QByteArray content =
        QByteArrayLiteral("{\"mcpServers\":{\"alpha\":{\"command\":\"a\"}}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    // 目标消失：从备份重建。
    if (!expect(QFile::remove(dir.settingsPath),
                "the settings fixture could not be removed")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    if (!expect(preparation.stage.isEmpty() && preparation.ok
                    && preparation.prompt.state
                        == ExtensionStagingRestorePromptState::Ready,
                "a clean preparation was not ready")) {
        return;
    }
    expect(preparation.preRestoreCaptureSkipped
               && preparation.preRestoreBackupId.isEmpty(),
           "a missing target was not honestly reported as capture-skipped");
    expect(preparation.prompt.fileWriteCount == 1
               && preparation.prompt.alreadyInPlaceCount == 0,
           "the prompt did not plan exactly one file write");
    expect(hasWarning(preparation.prompt,
                      ExtensionStagingRestoreWarning::SharedSettingsFileRestore)
               && !hasWarning(
                   preparation.prompt,
                   ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet)
               && preparation.prompt.doesNotExecuteNote.isEmpty(),
           "the wired prompt still carried the no-execution disclosure");
    expect(preparation.destinationRoot
               == QFileInfo(QFileInfo(dir.settingsPath).absolutePath())
                      .canonicalFilePath(),
           "the destination root was not the canonical settings directory");

    AuditFixture audit(dir.temporary.filePath(QStringLiteral("e2e.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const QDateTime decidedAt = now();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt, &store);
    expect(outcome.decisionRecorded
               && outcome.decision
                   == ExtensionStagingRestoreAuditDecision::Approved
               && outcome.errorCode.isEmpty(),
           "an aligned approval was not recorded");
    expect(outcome.executed
               && outcome.execution.state
                   == ExtensionStagingRestoreExecutionState::Complete
               && outcome.execution.doneCount == 1
               && outcome.execution.errorCode.isEmpty(),
           "the approved restore did not execute to completion");
    expect(readFile(dir.settingsPath) == content,
           "the restored file does not match the backup byte for byte");

    // 审计条目逐项绑定渲染出的提示。
    const ExtensionStagingRestoreAuditStoreResult ledger =
        audit.store().load();
    if (!expect(ledger.state == ExtensionStagingRestoreAuditStoreState::Ready
                    && ledger.entries.size() == 1,
                "the ledger did not hold exactly one entry")) {
        return;
    }
    const ExtensionStagingRestoreAuditEntry &entry = ledger.entries.first();
    expect(entry.subject == kSubject && entry.backupId == backupId
               && entry.planIdentity == preparation.prompt.echoedPlanIdentity
               && entry.treeIdentity == preparation.prompt.echoedTreeIdentity
               && entry.decision
                   == ExtensionStagingRestoreAuditDecision::Approved
               && entry.decidedAt == decidedAt,
           "the audit entry does not bind the exact plan and tree identities");
}

// already-in-place：目标内容与备份逐字节一致 → 恢复前捕获照常发生，执行零写入，
// 恢复前备份可按 id 读回并通过五参验证。
void testAlreadyInPlaceRestore()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("in-place"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    const QFileInfo before(dir.settingsPath);

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    if (!expect(preparation.stage.isEmpty() && preparation.ok,
                "an already-in-place preparation was not ready")) {
        return;
    }
    expect(!preparation.preRestoreCaptureSkipped
               && !preparation.preRestoreBackupId.isEmpty()
               && preparation.preRestoreBackupId != backupId,
           "the pre-restore capture did not produce a fresh backup");
    expect(preparation.prompt.fileWriteCount == 0
               && preparation.prompt.alreadyInPlaceCount == 1
               && hasWarning(preparation.prompt,
                             ExtensionStagingRestoreWarning::AlreadyInPlaceFiles),
           "the prompt did not mark the file already in place");

    AuditFixture audit(dir.temporary.filePath(QStringLiteral("inplace.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            now(), &store);
    expect(outcome.decisionRecorded && outcome.executed
               && outcome.execution.state
                   == ExtensionStagingRestoreExecutionState::Complete
               && outcome.execution.doneCount == 0
               && outcome.execution.skippedVerifiedCount == 1,
           "the already-in-place restore was not a zero-write completion");
    const QFileInfo after(dir.settingsPath);
    expect(readFile(dir.settingsPath) == content
               && after.lastModified() == before.lastModified(),
           "an already-in-place restore touched the target file");

    // 恢复前备份按 id 读回并通过五参验证，重建树字节就是当前内容。
    FixedKeyProvider readerProvider;
    ConfigurationBackupStore backupStore(
        ConfigurationBackupStore::extensionStagingDomain(), dir.backupRoot,
        &readerProvider);
    ConfigurationBackupSnapshot preRestore;
    QString error;
    if (!expect(backupStore.read(kSubject, preparation.preRestoreBackupId,
                                 &preRestore, &error),
                "the pre-restore backup could not be read back")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!expect(ExtensionStagingSnapshot::verify(
                    McpConfigurationInventory::backupCaptureDomain(), kSubject,
                    preRestore, &rebuilt, &error),
                "the pre-restore backup failed verification")) {
        return;
    }
    expect(rebuilt.size() == 1
               && rebuilt.first().relativePath
                   == QStringLiteral("settings.json")
               && rebuilt.first().bytes == content,
           "the pre-restore backup does not hold the current content");
}

// 冲突硬拒绝：目标内容与备份不同 → 捕获已发生，计划层拒绝，呈现 Refused，文件
// 原样保留；对非 ok 的准备提交是 not-prepared。
void testDestinationConflictRefused()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("conflict"));
    const QByteArray backupContent =
        QByteArrayLiteral("{\"mcpServers\":{\"alpha\":{\"command\":\"a\"}}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, backupContent);
    if (backupId.isEmpty()) return;
    // 备份之后人改动了设置文件：当前内容与备份逐字节不同。
    const QByteArray current =
        QByteArrayLiteral("{\"mcpServers\":{\"beta\":{\"command\":\"b\"}}}\n");
    if (!expect(writeFile(dir.settingsPath, current),
                "the conflicting fixture could not be written")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    expect(preparation.stage.isEmpty() && !preparation.ok
               && preparation.prompt.state
                   == ExtensionStagingRestorePromptState::Refused
               && preparation.prompt.refusalReason
                   == QStringLiteral(
                       "extension-staging-restore-destination-conflict"),
           "a conflicting destination was not rendered as a refusal");
    expect(!preparation.preRestoreBackupId.isEmpty(),
           "the conflicted content was not captured before the refusal");
    expect(readFile(dir.settingsPath) == current,
           "a refused restore touched the target file");

    AuditFixture audit(dir.temporary.filePath(QStringLiteral("conflict.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            now(), &store);
    expect(!outcome.decisionRecorded && !outcome.executed
               && outcome.errorCode
                   == QStringLiteral("extension-restore-flow-not-prepared"),
           "a non-ready preparation reached commit");
    expect(audit.store().load().state
               == ExtensionStagingRestoreAuditStoreState::Empty,
           "a refused preparation wrote to the audit ledger");
}

// declined 同样记录：问题被问过并被回答了"不"，零执行。
void testDeclinedRecordedWithoutExecution()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("declined"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    if (!expect(QFile::remove(dir.settingsPath),
                "the settings fixture could not be removed")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    if (!expect(preparation.ok, "the preparation was not ready")) return;

    AuditFixture audit(dir.temporary.filePath(QStringLiteral("declined.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Decline),
            now(), &store);
    expect(outcome.decisionRecorded
               && outcome.decision
                   == ExtensionStagingRestoreAuditDecision::Declined
               && !outcome.executed,
           "a declined decision was not recorded without execution");
    expect(!QFileInfo::exists(dir.settingsPath),
           "a declined restore wrote the target file");
    const ExtensionStagingRestoreAuditStoreResult ledger =
        audit.store().load();
    expect(ledger.entries.size() == 1
               && ledger.entries.first().decision
                   == ExtensionStagingRestoreAuditDecision::Declined,
           "the ledger does not hold exactly one declined entry");
}

// 审计链退化：决定无法记录，零执行，目标原样。
void testLedgerDegradedFreezesCommit()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("degraded"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    if (!expect(QFile::remove(dir.settingsPath),
                "the settings fixture could not be removed")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    if (!expect(preparation.ok, "the preparation was not ready")) return;

    AuditFixture audit(dir.temporary.filePath(QStringLiteral("degraded.ini")));
    audit.secure.readState =
        ExtensionStagingRestoreAuditSecureStore::ReadState::Unavailable;
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            now(), &store);
    expect(!outcome.decisionRecorded && !outcome.executed
               && !outcome.errorCode.isEmpty(),
           "a degraded ledger did not freeze the commit");
    expect(!QFileInfo::exists(dir.settingsPath),
           "a frozen commit wrote the target file");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testRestoreOfferedPredicate();
    testRequestAndSubjectGates();
    testCaptureFailureFailsClosed();
    testBackupVanished();
    testEndToEndRestore();
    testAlreadyInPlaceRestore();
    testDestinationConflictRefused();
    testDeclinedRecordedWithoutExecution();
    testLedgerDegradedFreezesCommit();
    if (failures == 0) {
        QTextStream(stdout) << "PASS: extension_staging_restore_flow" << '\n';
        return 0;
    }
    return 1;
}
