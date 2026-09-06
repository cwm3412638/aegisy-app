#include "extension_staging_restore_flow.h"

#include "mcp_configuration_inventory.h"

#include <QFile>
#include <QFileInfo>

namespace {

const QString kPrefix = QStringLiteral("extension-restore-flow");

QString code(const char *suffix)
{
    return kPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

// 恢复链只为这一个主体接线：唯一既有调用方权威目标映射（MCP 设置文件路径）又有真实
// 备份的主体。与 isRestoreOffered 共用同一字面量定义点。
const QString kRestorableSubject = QStringLiteral("mcp:claude-settings");

ExtensionStagingRestorePreparation failPreparation(const QString &stage,
                                                   const QString &errorCode)
{
    ExtensionStagingRestorePreparation preparation;
    preparation.stage = stage;
    preparation.errorCode = errorCode;
    return preparation;
}

} // namespace

ExtensionStagingRestoreDiskObservation::ExtensionStagingRestoreDiskObservation(
    const QString &root)
    : m_root(root)
{
}

QString ExtensionStagingRestoreDiskObservation::canonicalRoot()
{
    return QFileInfo(m_root).canonicalFilePath();
}

ExtensionStagingRestoreObservation::NodeKind
ExtensionStagingRestoreDiskObservation::nodeKind(const QString &relativePath)
{
    const QString absolute = relativePath.isEmpty()
        ? m_root : m_root + QLatin1Char('/') + relativePath;
    const QFileInfo info(absolute);
    if (!info.exists()) return NodeKind::Missing;
    // 符号链接不跟随：它在任何计划位置都是拒绝理由。
    if (info.isSymLink()) return NodeKind::Symlink;
    if (info.isDir()) return NodeKind::Directory;
    if (info.isFile()) return NodeKind::File;
    return NodeKind::Other;
}

bool ExtensionStagingRestoreDiskObservation::fileContent(
    const QString &relativePath, QByteArray *content)
{
    if (!content) return false;
    QFile file(m_root + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    *content = file.readAll();
    return file.error() == QFileDevice::NoError;
}

ExtensionStagingRestorePreparation ExtensionStagingRestoreFlow::prepare(
    const QString &settingsFilePath,
    const QString &backupId,
    const QString &subject,
    const QString &backupRoot,
    ConfigurationBackupKeyProvider *keyProvider,
    const QDateTime &now)
{
    // 参数校验先于一切文件系统工作：调用方权威输入缺失或畸形时什么都不该被触碰。
    if (settingsFilePath.isEmpty() || backupId.isEmpty() || subject.isEmpty()
            || backupRoot.isEmpty() || !keyProvider || !now.isValid()) {
        return failPreparation(QStringLiteral("request"),
                               code("request-invalid"));
    }
    // 主体资格复核：编排器与谓词共享同一封闭答案，绝不从按钮状态继承信任。
    if (subject != kRestorableSubject) {
        return failPreparation(QStringLiteral("subject"),
                               code("subject-unsupported"));
    }

    ExtensionStagingRestorePreparation preparation;
    preparation.subject = subject;
    preparation.backupId = backupId;
    // 失败返回必须带上已累计的诚实现场（恢复前捕获可能已成功产出新备份、或已如实
    // 跳过）：调用方据此刷新清单与如实报告，绝不让一份新备份在界面上隐形。
    const auto fail = [&preparation](const QString &stage,
                                     const QString &errorCode) {
        preparation.stage = stage;
        preparation.errorCode = errorCode;
        preparation.ok = false;
        return preparation;
    };

    // 恢复前先捕获：目标文件存在时，当前状态先变成一份新的暂存备份，恢复才有回退
    // 路径。捕获失败即整体失败关闭——绝不"先恢复再说"。目标不存在时诚实跳过：没有
    // 可捕获的状态，MCP 保存链对不存在的设置文件也是同一语义。
    if (QFileInfo::exists(settingsFilePath)) {
        ExtensionStagingBackupCaptureResult captureResult;
        QString captureError;
        if (!ExtensionStagingBackupCapture::capture(
                subject, settingsFilePath, backupRoot, keyProvider,
                &captureResult, &captureError)) {
            return fail(QStringLiteral("capture"), captureError);
        }
        preparation.preRestoreBackupId = captureResult.backupId;
    } else {
        preparation.preRestoreCaptureSkipped = true;
    }

    // 呈现前重新清点：资格与验证状态必须来自准备时刻的清单。备份可能在按钮渲染与
    // 点击之间损坏或消失，各自独立诊断。退化（Invalid/Unavailable）与"确实一份都
    // 没有"（Empty）是两种不同的现实：后者如实落到 backup-vanished。
    ExtensionStagingBackupListResult listing;
    QString listingError;
    if (!ExtensionStagingBackupInventory::list(
            backupRoot, subject, &listing, &listingError)) {
        return fail(QStringLiteral("listing"), code("listing-failed"));
    }
    if (listing.state != ExtensionStagingBackupListState::Ready
            && listing.state != ExtensionStagingBackupListState::Empty) {
        return fail(QStringLiteral("listing"), code("listing-degraded"));
    }
    const ExtensionStagingBackupListEntry *entry = nullptr;
    for (const ExtensionStagingBackupListEntry &candidate : listing.entries) {
        if (candidate.backupId == backupId) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        return fail(QStringLiteral("listing"), code("backup-vanished"));
    }
    if (entry->verification
            != ExtensionStagingBackupEntryVerification::ListedIntact) {
        return fail(QStringLiteral("listing"), code("backup-not-intact"));
    }
    preparation.verification = entry->verification;

    // 读回快照：载荷字节的 GCM 认证在这一步发生，读不出即失败关闭。
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        keyProvider);
    QString readError;
    if (!store.read(subject, backupId, &preparation.snapshot, &readError)) {
        return fail(QStringLiteral("read"), readError);
    }

    // 目标根 = 设置文件所在目录的规范化形式。mcp 备份是固定单条目树
    // （settings.json），恢复语义是整文件写回该目录。
    const QString destinationRoot =
        QFileInfo(QFileInfo(settingsFilePath).absolutePath())
            .canonicalFilePath();
    if (destinationRoot.isEmpty()) {
        return fail(QStringLiteral("destination"),
                    code("destination-unresolvable"));
    }
    preparation.destinationRoot = destinationRoot;

    // 构建计划：内部重新验证快照（绝不相信"已验证"的声称），逐条重查路径形状与目标
    // 现状。目标已有内容与计划内容逐字节不同是硬拒绝——恢复前捕获已在上面发生，当前
    // 内容安然保存在新备份里，拒绝因此是安全的默认。
    ExtensionStagingRestoreDiskObservation observation(destinationRoot);
    QString planError;
    if (!ExtensionStagingRestorePlanBuilder::plan(
            McpConfigurationInventory::backupCaptureDomain(), subject,
            preparation.snapshot, destinationRoot, &observation,
            &preparation.plan, &planError)) {
        preparation.prompt =
            ExtensionStagingRestorePresentation::buildRefusal(planError);
        return preparation;
    }

    ExtensionStagingRestoreBackupDescriptor descriptor;
    descriptor.backupId = backupId;
    descriptor.subject = subject;
    descriptor.createdAt = entry->createdAt;
    descriptor.verification = entry->verification;
    // 本编排器就是执行路径的接线方：提示如实不再携带"仅供复核、不会执行"披露。
    preparation.prompt = ExtensionStagingRestorePresentation::build(
        preparation.plan, descriptor, destinationRoot, now,
        /*executionAvailable=*/true);
    preparation.ok = preparation.prompt.state
        == ExtensionStagingRestorePromptState::Ready;
    return preparation;
}

ExtensionStagingRestoreOutcome ExtensionStagingRestoreFlow::commit(
    const ExtensionStagingRestorePreparation &preparation,
    const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement,
    const QDateTime &decidedAt,
    ExtensionStagingRestoreAuditLedgerStore *store)
{
    ExtensionStagingRestoreOutcome outcome;
    // 只对 ok 的准备结果提交：拒绝的、不可呈现的、门禁失败的准备上没有可记录的
    // 标的物。
    if (!preparation.ok || !store || !decidedAt.isValid()) {
        outcome.errorCode = code("not-prepared");
        return outcome;
    }

    // 记录先于一切执行：declined 同样记录（证明问题被问过）；策略拒绝或审计链退化
    // 时 recorded 为假，零执行。
    const ExtensionStagingRestoreRecordResult recordResult =
        ExtensionStagingRestoreController::record(
            preparation.prompt, preparation.verification, acknowledgement,
            decidedAt, store);
    outcome.decisionRecorded = recordResult.recorded;
    outcome.decision = recordResult.decision;
    outcome.verdict = recordResult.verdict;
    if (!recordResult.recorded) {
        outcome.errorCode = recordResult.errorCode;
        return outcome;
    }
    // declined 如实不执行：问题被问过并被回答了"不"，这就是终点。
    if (recordResult.decision
            != ExtensionStagingRestoreAuditDecision::Approved) {
        return outcome;
    }
    // 凭据状态复核：控制器与执行器之间不允许出现落差。记录为 approved 而凭据未授权
    // 是防御性拒绝，零执行。
    if (recordResult.verdict.state
            != ExtensionStagingRestoreApprovalState::Authorized) {
        outcome.errorCode = code("credential-not-authorized");
        return outcome;
    }

    // 执行：执行器在执行开始时再次重验证快照、pre-flight 重观察目标，任何漂移在
    // 第一个字节写入之前拒绝。
    ExtensionStagingRestoreDiskObservation observation(
        preparation.destinationRoot);
    outcome.execution = ExtensionStagingRestoreExecutor::execute(
        McpConfigurationInventory::backupCaptureDomain(), preparation.subject,
        preparation.snapshot, preparation.plan, recordResult.verdict,
        &observation);
    outcome.executed = true;
    return outcome;
}
