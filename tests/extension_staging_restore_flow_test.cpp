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
        ++writes;
        if (writes == failOnWrite) {
            if (errorCode) *errorCode = QStringLiteral("fake-write-failed");
            return WriteOutcome::DefiniteFailure;
        }
        stored = value;
        return WriteOutcome::Committed;
    }

    QByteArray stored;
    ReadState readState = ReadState::Found;
    // 第 N 次写入确定性失败（1 起计）：用于把结果记录阶段的提交打成失败。
    int failOnWrite = -1;
    int writes = 0;
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

// 为该主体直接经存储种一份合法暂存备份（过去时间戳，保证恢复前捕获产生的新鲜备份
// 恒为最新）。与清点测试同形：夹具只需要合法的存储记录。
bool seedBackup(const QString &backupRoot, const QString &subject, int index)
{
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot snapshot;
    snapshot.backupId = QStringLiteral("ext_20260901_%1_%2")
        .arg(400000 + index, 6, 10, QLatin1Char('0'))
        .arg(0xfff0000 + index, 8, 16, QLatin1Char('0'));
    snapshot.tool = subject;
    snapshot.createdAt = QDateTime::fromString(
        QStringLiteral("2026-09-01T%1:%2:00.000Z")
            .arg(index / 60, 2, 10, QLatin1Char('0'))
            .arg(index % 60, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
    snapshot.files = {{ 0, true, QByteArrayLiteral("seeded-bytes") }};
    QString error;
    return store.create(snapshot, &error);
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
    const QDateTime recordedAt = now().addSecs(1);
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt, recordedAt, &store);
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

    // 执行结果作为独立条目入链：与决定条目经计划身份逐字节绑定，分类与计数如实，
    // 回退指针为空（本用例目标原本不存在，preRestoreCaptureSkipped）。
    expect(outcome.outcomeRecorded && outcome.outcomeAuditErrorCode.isEmpty(),
           "the execution outcome was not recorded in the audit ledger");
    if (!expect(ledger.outcomes.size() == 1,
                "the ledger did not hold exactly one outcome entry")) {
        return;
    }
    const ExtensionStagingRestoreOutcomeEntry &outcomeEntry =
        ledger.outcomes.first();
    expect(outcomeEntry.subject == entry.subject
               && outcomeEntry.backupId == entry.backupId
               && outcomeEntry.destinationRoot == entry.destinationRoot
               && outcomeEntry.planIdentity == entry.planIdentity
               && outcomeEntry.treeIdentity == entry.treeIdentity,
           "the outcome entry is not bound to the decision by plan identity");
    expect(outcomeEntry.outcome
                   == ExtensionStagingRestoreExecutionState::Complete
               && outcomeEntry.failureIndex == -1
               && outcomeEntry.doneCount == 1
               && outcomeEntry.skippedVerifiedCount == 0
               && outcomeEntry.failedCount == 0
               && outcomeEntry.preRestoreBackupId.isEmpty()
               && outcomeEntry.recordedAt == recordedAt,
           "the outcome entry does not report the execution truthfully");
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
            now(), now(), &store);
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

    // 执行结果入链且回退指针（恢复前备份 id）就在结果条目里。
    expect(outcome.outcomeRecorded,
           "the zero-write completion outcome was not recorded");
    const ExtensionStagingRestoreAuditStoreResult ledger =
        audit.store().load();
    if (!expect(ledger.outcomes.size() == 1,
                "the ledger did not hold exactly one outcome entry")) {
        return;
    }
    const ExtensionStagingRestoreOutcomeEntry &outcomeEntry =
        ledger.outcomes.first();
    expect(outcomeEntry.outcome
                   == ExtensionStagingRestoreExecutionState::Complete
               && outcomeEntry.skippedVerifiedCount == 1
               && outcomeEntry.doneCount == 0
               && outcomeEntry.preRestoreBackupId
                   == preparation.preRestoreBackupId,
           "the outcome entry does not carry the pre-restore backup pointer");
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
            now(), now(), &store);
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
            now(), now(), &store);
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
                   == ExtensionStagingRestoreAuditDecision::Declined
               && ledger.outcomes.isEmpty(),
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
            now(), now(), &store);
    expect(!outcome.decisionRecorded && !outcome.executed
               && !outcome.errorCode.isEmpty(),
           "a degraded ledger did not freeze the commit");
    expect(!QFileInfo::exists(dir.settingsPath),
           "a frozen commit wrote the target file");
}

// 结果记录失败 ≠ 执行失败：决定落账、执行真实完成后，把结果提交阶段的安全存储写入
// 打成失败——执行结果如实报告（盘上字节已恢复），审计失败由独立字段单独报告，
// outcome.errorCode 保持为空，审计链里只有决定条目（预留被如实回滚）。
void testOutcomeRecordingFailureKeepsExecutionTruth()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("outcome-failure"));
    const QByteArray content =
        QByteArrayLiteral("{\"mcpServers\":{\"alpha\":{\"command\":\"a\"}}}\n");
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

    AuditFixture audit(
        dir.temporary.filePath(QStringLiteral("outcome-failure.ini")));
    // 决定记录的两次安全存储写入（预留、提交）成功；结果记录的第一次写入（第 3 次）
    // 确定性失败。
    audit.secure.failOnWrite = 3;
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            now(), now(), &store);
    // 执行真相不被审计失败改写：真实完成、盘上字节逐字节一致。
    expect(outcome.decisionRecorded && outcome.executed
               && outcome.execution.state
                   == ExtensionStagingRestoreExecutionState::Complete
               && outcome.execution.doneCount == 1
               && outcome.errorCode.isEmpty(),
           "an audit failure rewrote the truthful execution result");
    expect(readFile(dir.settingsPath) == content,
           "the executed restore did not land on disk");
    // 审计失败单独成字段报告。
    expect(!outcome.outcomeRecorded
               && outcome.outcomeAuditErrorCode
                   == QStringLiteral("fake-write-failed"),
           "the audit failure was not surfaced distinctly");
    // 审计链如实：一条决定、零结果条目（失败的预留被回滚，不留半成品）。
    const ExtensionStagingRestoreAuditStoreResult ledger =
        audit.store().load();
    expect(ledger.state == ExtensionStagingRestoreAuditStoreState::Ready
               && ledger.entries.size() == 1 && ledger.outcomes.isEmpty(),
           "a failed outcome recording left a half-written ledger");
}

// 批准后执行前目标被改动：执行器 pre-flight 以 destination-drift 拒绝（零写入），
// 这次拒绝本身作为 Refused 结果条目入链——"被批准过"与"执行被拒绝"各自成事实。
void testExecutionRefusalRecordedAsOutcome()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("refusal-outcome"));
    const QByteArray content =
        QByteArrayLiteral("{\"mcpServers\":{\"alpha\":{\"command\":\"a\"}}}\n");
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
    // 批准之后、执行之前：目标位置长出了计划之外的内容（计划期望它缺失）。
    const QByteArray drifted =
        QByteArrayLiteral("{\"mcpServers\":{\"evil\":{\"command\":\"e\"}}}\n");
    if (!expect(writeFile(dir.settingsPath, drifted),
                "the drift fixture could not be written")) {
        return;
    }

    AuditFixture audit(
        dir.temporary.filePath(QStringLiteral("refusal-outcome.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = audit.store();
    const ExtensionStagingRestoreOutcome outcome =
        ExtensionStagingRestoreFlow::commit(
            preparation,
            acknowledge(preparation.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            now(), now(), &store);
    expect(outcome.decisionRecorded && outcome.executed
               && outcome.execution.state
                   == ExtensionStagingRestoreExecutionState::Refused
               && outcome.execution.errorCode
                   == QStringLiteral(
                       "extension-restore-execution-destination-drift"),
           "a drifted destination was not refused at execution time");
    expect(readFile(dir.settingsPath) == drifted,
           "a refused execution touched the drifted target");
    expect(outcome.outcomeRecorded,
           "the execution refusal was not recorded as an outcome");
    const ExtensionStagingRestoreAuditStoreResult ledger =
        audit.store().load();
    if (!expect(ledger.entries.size() == 1 && ledger.outcomes.size() == 1,
                "the ledger did not hold the decision plus refusal outcome")) {
        return;
    }
    const ExtensionStagingRestoreOutcomeEntry &outcomeEntry =
        ledger.outcomes.first();
    expect(outcomeEntry.outcome
                   == ExtensionStagingRestoreExecutionState::Refused
               && outcomeEntry.failureIndex == -1
               && outcomeEntry.doneCount == 0
               && outcomeEntry.skippedVerifiedCount == 0
               && outcomeEntry.failedCount == 0
               && outcomeEntry.planIdentity
                   == ledger.entries.first().planIdentity,
           "the refusal outcome entry is not exact");
}

// 恢复前捕获成功后的保留期修剪（接线路径）：捕获把该主体推到 33 份 → 修剪最旧 1 份
// 回到 32；结果作为准备结果的独立字段如实携带，准备照常继续；最近完整备份（刚捕获的
// 新鲜备份）无条件保留；旁观主体的备份绝不被触碰。
void testPreRestoreCapturePrunesOverLimit()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("prune-over-limit"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    for (int i = 0; i < 31; ++i) {
        if (!expect(seedBackup(dir.backupRoot, kSubject, i),
                    "a seeded backup could not be created")) {
            return;
        }
    }
    // 旁观主体：修剪绝不允许越过主体边界。
    if (!expect(seedBackup(dir.backupRoot, QStringLiteral("skill:alpha"), 100),
                "the bystander backup could not be created")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    if (!expect(preparation.stage.isEmpty() && preparation.ok,
                "an over-limit preparation was not ready")) {
        return;
    }
    // 修剪字段如实：尝试过、计划成功、恰好删除最旧 1 份、无失败。
    expect(preparation.preRestoreRetentionAttempted
               && !preparation.preRestoreRetention.planFailed
               && preparation.preRestoreRetention.removedCount == 1
               && preparation.preRestoreRetention.corruptKeptCount == 0
               && preparation.preRestoreRetention.failures.isEmpty(),
           "the pre-restore prune was not honestly carried on the "
           "preparation");
    // newestVerifiedKept 无条件保留语义在接线路径上成立：保留的最近一份就是刚捕获的
    // 恢复前备份。
    expect(preparation.preRestoreRetention.newestVerifiedKept
               == preparation.preRestoreBackupId,
           "the newest verified backup kept is not the fresh pre-restore "
           "capture");
    // 修剪到上限：该主体 32 份，最旧的种入备份消失，恢复目标备份与新鲜捕获都在。
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::list(
                    dir.backupRoot, kSubject, &listing, &error)
                    && listing.state == ExtensionStagingBackupListState::Ready
                    && listing.entries.size() == 32,
                "the subject was not pruned back to the domain cap")) {
        return;
    }
    bool oldestSeedPresent = false;
    bool targetPresent = false;
    bool freshPresent = false;
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.backupId == QStringLiteral("ext_20260901_400000_0fff0000")) {
            oldestSeedPresent = true;
        }
        if (entry.backupId == backupId) targetPresent = true;
        if (entry.backupId == preparation.preRestoreBackupId) {
            freshPresent = true;
        }
    }
    expect(!oldestSeedPresent && targetPresent && freshPresent,
           "the prune removed the wrong backups");
    // 其他主体的备份绝不被触碰。
    ExtensionStagingBackupListResult bystander;
    expect(ExtensionStagingBackupInventory::list(
               dir.backupRoot, QStringLiteral("skill:alpha"), &bystander,
               &error)
               && bystander.entries.size() == 1,
           "the pre-restore prune touched another subject's backups");
}

// 退化清点：修剪计划失败 = 零删除 + 诊断透传,且绝不翻转已成功的捕获——新鲜备份完整
// 在盘上,修剪失败作为独立字段如实携带,准备随后在呈现前清点阶段如实失败关闭。
void testDegradedPruneDoesNotFlipTheCapture()
{
    CaseDir dir;
    if (!expect(dir.valid(), "temporary directory unavailable")) return;
    dir.layOut(QStringLiteral("prune-degraded"));
    const QByteArray content = QByteArrayLiteral("{\"mcpServers\":{}}\n");
    const QString backupId =
        captureBackup(dir.settingsPath, dir.backupRoot, content);
    if (backupId.isEmpty()) return;
    // 根形状违例：捕获的降级语义不阻断写入（既有契约）,但保留期计划必须失败关闭。
    if (!expect(writeFile(dir.backupRoot + QStringLiteral("/stray.txt"),
                          QByteArrayLiteral("junk")),
                "the degradation fixture could not be planted")) {
        return;
    }

    FixedKeyProvider provider;
    const ExtensionStagingRestorePreparation preparation =
        ExtensionStagingRestoreFlow::prepare(dir.settingsPath, backupId,
                                             kSubject, dir.backupRoot,
                                             &provider, now());
    // 捕获真实成功：新备份 id 在场且目录完整在盘上——修剪失败绝不代表捕获失败。
    expect(!preparation.preRestoreBackupId.isEmpty()
               && QFileInfo::exists(dir.backupRoot + QLatin1Char('/')
                                    + preparation.preRestoreBackupId),
           "a degraded prune erased or hid the successful capture");
    // 修剪如实失败：计划失败、零删除、诊断逐字透传。
    expect(preparation.preRestoreRetentionAttempted
               && preparation.preRestoreRetention.planFailed
               && preparation.preRestoreRetention.planError == QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid")
               && preparation.preRestoreRetention.removedCount == 0
               && preparation.preRestoreRetention.failures.isEmpty(),
           "a degraded prune was not honestly reported on the preparation");
    // 准备随后在呈现前清点阶段失败关闭（退化清点是独立的既有门禁）,三份备份一份
    // 没少。
    expect(preparation.stage == QStringLiteral("listing")
               && preparation.errorCode
                   == QStringLiteral("extension-restore-flow-listing-degraded")
               && !preparation.ok,
           "a degraded store did not fail closed at the listing stage");
    expect(QFileInfo::exists(dir.backupRoot + QLatin1Char('/') + backupId)
               && QDir(dir.backupRoot).entryList(
                      QDir::Dirs | QDir::NoDotAndDotDot).size() == 2,
           "a degraded prune deleted backups");
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
    testOutcomeRecordingFailureKeepsExecutionTruth();
    testExecutionRefusalRecordedAsOutcome();
    testPreRestoreCapturePrunesOverLimit();
    testDegradedPruneDoesNotFlipTheCapture();
    if (failures == 0) {
        QTextStream(stdout) << "PASS: extension_staging_restore_flow" << '\n';
        return 0;
    }
    return 1;
}
