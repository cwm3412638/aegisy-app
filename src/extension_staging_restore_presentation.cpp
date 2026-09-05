#include "extension_staging_restore_presentation.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

using Safety = ExtensionDisplaySafety;

const QString kPrefix = QStringLiteral("extension-restore-presentation");

QString code(const char *suffix)
{
    return kPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

ExtensionStagingRestorePrompt reject(const QString &codeValue)
{
    ExtensionStagingRestorePrompt prompt;
    prompt.state = ExtensionStagingRestorePromptState::Unpresentable;
    prompt.errorCode = codeValue;
    return prompt;
}

// 披露文案是固定字面量而不是每次拼装：它们的措辞本身就是契约的一部分。
const QString kDoesNotExecuteNote = QStringLiteral(
    "当前不存在任何恢复执行路径：此呈现仅供人工复核，复核或批准它不会创建任何目录，"
    "也不会写入任何内容");
const QString kSharedFileNote = QStringLiteral(
    "此恢复覆盖整个共享设置文件，包括其中其他服务器的配置，而不只是该服务器自己的"
    "条目");

// 树身份的展示前缀由主体种类决定：技能树与 MCP 整文件备份是两个不同的身份域，两者
// 绝不互认。无法归类的主体种类没有可校验的前缀，整体拒绝。
QString treeIdentityPrefix(const QString &subject)
{
    if (subject.startsWith(QStringLiteral("skill:"))) {
        return QStringLiteral("extension-content:sha256:");
    }
    if (subject.startsWith(QStringLiteral("mcp:"))) {
        return QStringLiteral("mcp-backup-content:sha256:");
    }
    return QString();
}

// 目标非空的可证明判据：计划只包含需要创建的目录，于是一条文件操作若带着不在目录
// 创建集里的祖先目录，那个祖先必然已存在于目标；already-in-place 文件同理证明目标
// 已有内容。
bool destinationProvablyNonEmpty(const ExtensionStagingRestorePlan &plan,
                                 int alreadyInPlaceCount)
{
    if (alreadyInPlaceCount > 0) return true;
    QSet<QString> plannedDirectories;
    for (const ExtensionStagingRestoreOperation &operation : plan.operations) {
        if (operation.directory) plannedDirectories.insert(operation.relativePath);
    }
    for (const ExtensionStagingRestoreOperation &operation : plan.operations) {
        if (operation.directory) continue;
        QString ancestor = operation.relativePath.section(
            QLatin1Char('/'), 0, -2);
        while (!ancestor.isEmpty()) {
            if (!plannedDirectories.contains(ancestor)) return true;
            ancestor = ancestor.section(QLatin1Char('/'), 0, -2);
        }
    }
    return false;
}

} // namespace

ExtensionStagingRestorePrompt ExtensionStagingRestorePresentation::build(
    const ExtensionStagingRestorePlan &plan,
    const ExtensionStagingRestoreBackupDescriptor &descriptor,
    const QString &destinationRoot,
    const QDateTime &now)
{
    // 描述字段缺失或清点状态不是完整验证通过：无法诚实说明正在恢复的是哪一份备份。
    if (descriptor.verification
            != ExtensionStagingBackupEntryVerification::ListedIntact) {
        return reject(code("descriptor-corrupt"));
    }
    if (descriptor.backupId.isEmpty() || descriptor.subject.isEmpty()
            || !descriptor.createdAt.isValid()) {
        return reject(code("descriptor-invalid"));
    }
    if (!Safety::safeDisplayText(descriptor.backupId, MaxLabelCharacters)) {
        return reject(code("descriptor-invalid"));
    }
    if (!now.isValid()) {
        return reject(code("now-invalid"));
    }
    // 描述与计划必须指向同一份东西：主体不符说明这份计划不是从所描述的备份构建的。
    if (descriptor.subject != plan.subject) {
        return reject(code("descriptor-mismatch"));
    }
    // 调用方给出的目标根必须与计划的规范化目标根逐字节相等：任何漂移都意味着渲染的
    // 目标与计划绑定的目标不是同一个。
    if (destinationRoot.isEmpty()
            || destinationRoot != plan.destinationRoot) {
        return reject(code("destination-mismatch"));
    }
    const QString identityPrefix = treeIdentityPrefix(plan.subject);
    if (identityPrefix.isEmpty()) {
        return reject(code("subject-invalid"));
    }
    // 主体与目标根都要进入屏幕，因此同样过共享展示安全层。
    if (!Safety::safeDisplayText(plan.subject, MaxPathCharacters)) {
        return reject(code("subject-unsafe"));
    }
    if (!Safety::safeDisplayText(destinationRoot, MaxPathCharacters)) {
        return reject(code("destination-unsafe"));
    }
    // 人看到的身份就是复核所绑定的身份：畸形身份无法与任何内容对齐，整体拒绝。
    if (!Safety::hashIdentity(plan.treeIdentity, identityPrefix)) {
        return reject(code("tree-identity-invalid"));
    }
    if (!Safety::hashIdentity(
            plan.planIdentity,
            QStringLiteral("extension-staging-restore-plan:sha256:"))) {
        return reject(code("plan-identity-invalid"));
    }

    // 逐条重查：路径要可安全展示，文件条目要携带形状合法的期望摘要，目录条目不得夹
    // 带文件字段，目录创建必须先于文件写入——这些都是计划契约，呈现层再守一次，因为
    // 屏幕上的一条错误条目就是人会复核的一条错误条目。
    bool filePhase = false;
    for (const ExtensionStagingRestoreOperation &operation : plan.operations) {
        if (operation.relativePath.isEmpty()
                || !Safety::safeDisplayText(operation.relativePath,
                                            MaxPathCharacters)) {
            return reject(code("entry-path-unsafe"));
        }
        if (operation.directory) {
            if (filePhase) {
                return reject(code("operations-unordered"));
            }
            if (operation.byteCount != 0 || !operation.sha256.isEmpty()
                    || operation.alreadyInPlace) {
                return reject(code("entry-inconsistent"));
            }
            continue;
        }
        filePhase = true;
        if (operation.byteCount < 0 || operation.sourceSlot < 1) {
            return reject(code("entry-inconsistent"));
        }
        if (!Safety::hashIdentity(operation.sha256, QString())) {
            return reject(code("entry-digest-invalid"));
        }
    }

    ExtensionStagingRestorePrompt prompt;
    prompt.subject = plan.subject;
    prompt.backupId = descriptor.backupId;
    prompt.createdAtLabel = descriptor.createdAt.toUTC().toString(
        Qt::ISODateWithMs);
    prompt.destinationRoot = destinationRoot;
    prompt.planIdentity = plan.planIdentity;
    prompt.planFingerprint = Safety::fingerprint(plan.planIdentity);
    prompt.treeIdentity = plan.treeIdentity;
    prompt.treeFingerprint = Safety::fingerprint(plan.treeIdentity);

    for (const ExtensionStagingRestoreOperation &operation : plan.operations) {
        if (operation.directory) {
            ++prompt.directoryCount;
            continue;
        }
        prompt.totalBytes += operation.byteCount;
        if (operation.alreadyInPlace) {
            ++prompt.alreadyInPlaceCount;
        } else {
            ++prompt.fileWriteCount;
        }
    }

    // 有界清单：只列出前 MaxListedEntries 条，超出以显式截断标记交代。截断仅作用于
    // 清单；身份回声绑定的是完整计划，绑定声明如实说出这一点。
    const int total = plan.operations.size();
    const int listed = qMin(total, MaxListedEntries);
    for (int index = 0; index < listed; ++index) {
        const ExtensionStagingRestoreOperation &operation =
            plan.operations.at(index);
        ExtensionStagingRestoreEntryRow row;
        row.directory = operation.directory;
        row.relativePath = operation.relativePath;
        row.byteCount = operation.byteCount;
        row.sha256 = operation.sha256;
        row.alreadyInPlace = operation.alreadyInPlace;
        prompt.entries.append(row);
    }
    prompt.omittedEntryCount = total - listed;
    prompt.listingTruncated = prompt.omittedEntryCount > 0;
    if (prompt.listingTruncated) {
        prompt.truncationNote = QStringLiteral("…以及另外 %1 条操作未列出")
            .arg(prompt.omittedEntryCount);
    }
    prompt.identityBindingNote = QStringLiteral(
        "计划指纹绑定完整计划：以上指纹覆盖全部 %1 条操作，包括因截断而未列出的条目")
        .arg(total);
    // 回显的身份就是展示的身份：复核流程按它们检测渲染与批准之间的漂移。
    prompt.echoedPlanIdentity = plan.planIdentity;
    prompt.echoedTreeIdentity = plan.treeIdentity;

    // 风险按固定顺序输出，避免排版顺序影响人的判断。
    if (destinationProvablyNonEmpty(plan, prompt.alreadyInPlaceCount)) {
        prompt.warnings.append(
            ExtensionStagingRestoreWarning::DestinationNotEmpty);
    }
    if (prompt.alreadyInPlaceCount > 0) {
        prompt.warnings.append(
            ExtensionStagingRestoreWarning::AlreadyInPlaceFiles);
    }
    // 共享设置文件恢复对 mcp 主体是强制警告：整文件语义意味着恢复覆盖整个共享设置
    // 文件，包括其他服务器的配置。缺失这条警告是呈现失败，而不是少了点缀。
    if (plan.subject.startsWith(QStringLiteral("mcp:"))) {
        prompt.warnings.append(
            ExtensionStagingRestoreWarning::SharedSettingsFileRestore);
        prompt.sharedFileOverwriteNote = kSharedFileNote;
    }
    if (prompt.fileWriteCount + prompt.alreadyInPlaceCount > LargeRestoreFiles
            || prompt.totalBytes > LargeRestoreBytes) {
        prompt.warnings.append(ExtensionStagingRestoreWarning::LargeRestore);
    }
    if (descriptor.createdAt.daysTo(now) > OldBackupDays) {
        prompt.warnings.append(ExtensionStagingRestoreWarning::OldBackup);
    }
    // 当前不存在任何恢复执行路径。必须显式说明，否则沉默会让人以为恢复已经能执行。
    prompt.warnings.append(
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet);
    prompt.doesNotExecuteNote = kDoesNotExecuteNote;

    prompt.approvable = true;
    prompt.state = ExtensionStagingRestorePromptState::Ready;
    return prompt;
}

ExtensionStagingRestorePrompt ExtensionStagingRestorePresentation::buildRefusal(
    const QString &refusalCode)
{
    // 拒绝理由来自计划层诊断，但同样要进入屏幕：不可安全展示的拒绝理由整体拒绝，
    // 而不是清洗成一个近似串。
    if (refusalCode.isEmpty()
            || !Safety::safeDisplayText(refusalCode, MaxLabelCharacters)) {
        return reject(code("refusal-invalid"));
    }
    ExtensionStagingRestorePrompt prompt;
    prompt.state = ExtensionStagingRestorePromptState::Refused;
    prompt.refusalReason = refusalCode;
    // 构建失败的计划上没有可批准的标的物：没有计划摘要、没有可批准标记，但"不执行"
    // 披露依然在场。
    prompt.warnings.append(
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet);
    prompt.doesNotExecuteNote = kDoesNotExecuteNote;
    return prompt;
}
