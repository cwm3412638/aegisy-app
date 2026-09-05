#include "extension_staging_restore_approval.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

using Safety = ExtensionDisplaySafety;

const QString kPrefix = QStringLiteral("extension-restore-approval");

QString code(const char *suffix)
{
    return kPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

ExtensionStagingRestoreApprovalVerdict refuse(const QString &codeValue)
{
    ExtensionStagingRestoreApprovalVerdict verdict;
    verdict.state = ExtensionStagingRestoreApprovalState::Refused;
    verdict.errorCode = codeValue;
    return verdict;
}

// 树身份的形状检查枚举两个已知身份域，而不是重新推导"主体种类 → 身份域"的映射：主体与
// 身份域的绑定已由呈现层在渲染时强制执行，这里再按字节相等对齐回显身份即可重新绑紧；
// 审批层只负责拒绝一个不属于任何已知域的畸形身份。
bool knownTreeIdentity(const QString &identity)
{
    return Safety::hashIdentity(identity,
                                QStringLiteral("extension-content:sha256:"))
        || Safety::hashIdentity(identity,
                                QStringLiteral("mcp-backup-content:sha256:"));
}

} // namespace

bool ExtensionStagingRestoreApprovalPolicy::requiresExplicitConfirmation(
    ExtensionStagingRestoreWarning warning, int fileWriteCount)
{
    switch (warning) {
    // 共享设置文件恢复是整文件覆盖：恢复会覆盖整个共享设置文件，包括其他服务器的
    // 配置。它越过本主体自己的数据边界，必须逐次显式确认。
    case ExtensionStagingRestoreWarning::SharedSettingsFileRestore:
        return true;
    // 目标非空只有在计划真的会写入文件时才是冲突邻接的：向一棵已有内容的树写入，
    // 恢复出的状态是新旧内容的混合，这正是恢复可能悄悄造成损害的边界。非空完全由
    // already-in-place 文件证明时不写入任何字节，是纯信息性披露。
    case ExtensionStagingRestoreWarning::DestinationNotEmpty:
        return fileWriteCount > 0;
    // 纯信息性披露不要求确认：不执行披露在每一份提示上都在场（要求确认等于让所有人
    // 每次都点同一个复选框，确认因此退化成摆设）；already-in-place 文件不写入任何
    // 字节；大型与陈旧说的是规模与年龄，计划身份已经把每一个字节绑死，它们不携带
    // 额外的授权维度。
    case ExtensionStagingRestoreWarning::AlreadyInPlaceFiles:
    case ExtensionStagingRestoreWarning::LargeRestore:
    case ExtensionStagingRestoreWarning::OldBackup:
    case ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet:
        return false;
    }
    // 未知警告按需要确认处理：新增的警告类别不应默认变成无需确认的。
    return true;
}

ExtensionStagingRestoreApprovalVerdict
ExtensionStagingRestoreApprovalPolicy::evaluate(
    const ExtensionStagingRestorePrompt &prompt,
    ExtensionStagingBackupEntryVerification backupVerification,
    const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement)
{
    // 拒绝就是拒绝：它不产生任何授权。先于其他检查处理，因为一个格式有问题的拒绝不
    // 应该被报告成"批准失败"。
    if (acknowledgement.decision
            == ExtensionStagingRestoreApprovalDecision::Decline) {
        return refuse(code("declined"));
    }

    // 不可展示的提示不能被批准：人不可能看过一份无法呈现的内容。
    if (prompt.state == ExtensionStagingRestorePromptState::Unpresentable) {
        return refuse(code("prompt-unpresentable"));
    }
    // 计划构建失败的提示上没有可批准的标的物：呈现层已经拒绝渲染计划摘要，走到这里
    // 的批准要么来自过期的界面，要么是伪造的——两种情况都不能通过。
    if (prompt.state != ExtensionStagingRestorePromptState::Ready) {
        return refuse(code("prompt-refused"));
    }

    // 备份的验证状态是必需输入而不是可默认的假设：从一份未通过清单身份级验证的备份
    // 恢复，等于从未经认证的字节恢复，比不恢复更糟。等值比较同时让任何未归类的未来
    // 状态失败关闭。
    if (backupVerification
            != ExtensionStagingBackupEntryVerification::ListedIntact) {
        return refuse(code("backup-unverified"));
    }

    // 主体、备份 id、目标根逐项与渲染出的提示对齐：任何一项漂移都说明这份批准对应的
    // 是另一份提示。
    if (acknowledgement.subject.isEmpty()
            || acknowledgement.subject != prompt.subject) {
        return refuse(code("subject-mismatch"));
    }
    if (acknowledgement.backupId.isEmpty()
            || acknowledgement.backupId != prompt.backupId) {
        return refuse(code("backup-mismatch"));
    }
    if (acknowledgement.destinationRoot.isEmpty()
            || acknowledgement.destinationRoot != prompt.destinationRoot) {
        return refuse(code("destination-mismatch"));
    }

    // 回传的身份必须是完整的规范形式：截断或畸形的身份无法与任何内容对齐。
    if (!Safety::hashIdentity(
            acknowledgement.approvedPlanIdentity,
            QStringLiteral("extension-staging-restore-plan:sha256:"))
            || !knownTreeIdentity(acknowledgement.approvedTreeIdentity)) {
        return refuse(code("identity-invalid"));
    }
    // 两个身份都必须与回显逐字节相等：计划身份绑定目标根与全部操作，树身份绑定内容
    // 本身。只对齐其一就留下一个漂移通道。
    if (acknowledgement.approvedPlanIdentity != prompt.echoedPlanIdentity) {
        return refuse(code("plan-drift"));
    }
    if (acknowledgement.approvedTreeIdentity != prompt.echoedTreeIdentity) {
        return refuse(code("tree-drift"));
    }

    // 批准的是"我看到了这些警告并接受"。提示披露的每一项警告都必须在回传集合里出现，
    // 回传集合里也不得出现提示未曾披露的警告——前者对应一个风险更少的界面，后者来自
    // 另一个界面状态。
    const QSet<ExtensionStagingRestoreWarning> acknowledged(
        acknowledgement.acknowledgedWarnings.cbegin(),
        acknowledgement.acknowledgedWarnings.cend());
    if (acknowledged.size() != acknowledgement.acknowledgedWarnings.size()) {
        return refuse(code("warning-duplicate"));
    }
    bool requiresConfirmation = false;
    for (const ExtensionStagingRestoreWarning warning : prompt.warnings) {
        if (!acknowledged.contains(warning)) {
            return refuse(code("warning-undisclosed"));
        }
        if (requiresExplicitConfirmation(warning, prompt.fileWriteCount)) {
            requiresConfirmation = true;
        }
    }
    for (const ExtensionStagingRestoreWarning warning
             : acknowledgement.acknowledgedWarnings) {
        if (!prompt.warnings.contains(warning)) {
            return refuse(code("warning-unknown"));
        }
    }

    // 高风险不接受概括性批准：必须逐次显式确认。
    if (requiresConfirmation && !acknowledgement.highRiskConfirmed) {
        return refuse(code("confirmation-required"));
    }

    ExtensionStagingRestoreApprovalVerdict verdict;
    verdict.state = ExtensionStagingRestoreApprovalState::Authorized;
    // 凭据绑定确切的计划身份与树身份。它是纯数据：今天没有任何执行路径消费它，本层
    // 也不暴露任何执行钩子。
    verdict.authorizedPlanIdentity = prompt.echoedPlanIdentity;
    verdict.authorizedTreeIdentity = prompt.echoedTreeIdentity;
    return verdict;
}
