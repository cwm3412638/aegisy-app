#ifndef EXTENSION_STAGING_RESTORE_PRESENTATION_H
#define EXTENSION_STAGING_RESTORE_PRESENTATION_H

#include "extension_staging_backup_inventory.h"
#include "extension_staging_restore_plan.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QVector>

// 暂存恢复呈现契约：一份已构建的恢复计划如何变成人可复核的提示。恢复计划只有在"渲染出来
// 的正是将会执行的"时才可复核，因此这一层的职责是把计划的每一个字段如实搬到屏幕上，一处
// 都不许美化、截断身份或悄悄省略风险。
//
// 这一层只做呈现：纯数据加格式化。它不创建目录、不写任何字节、不接触存储、不判定信任、
// 不授予任何权限，也没有"确定"按钮的处理逻辑。`executionAvailable` 是调用方关于执行路径
// 是否在场的显式声明：为 false（默认）时每一份提示都必须携带 RestoreDoesNotExecuteYet
// 披露，否则沉默会让人以为恢复已经能执行；为 true 时调用方声明恢复将在批准后真实执行，
// 该披露不再渲染——调用方绝不能在不存在执行路径时把它置真，那是把"仅供复核"表述成
// "将会执行"。被拒绝的计划（buildRefusal）无论调用方声明如何都携带该披露：一份被拒绝的
// 计划真的不会执行。
//
// 信任边界：
//
// 1. 输入是一份【已成功构建】的计划、它来源的备份描述（来自清点层的备份 id、主体、创建
//    时间与验证状态），以及调用方给出的目标根字符串。构建失败的计划绝不能被渲染成一份可
//    批准的恢复——`buildRefusal` 把计划层的拒绝理由渲染成独立的 Refused 状态，没有计划
//    摘要、没有可批准标记。
//
// 2. 人看到的身份就是复核所绑定的身份。计划身份以两端指纹形式展示，同时把完整身份原样回
//    显到 `echoedPlanIdentity` / `echoedTreeIdentity`（与启用呈现的 reviewed* 字段同一先
//    例），使渲染与批准之间的任何漂移都可检测。
//
// 3. 逐条清单是有界的，而绑定不受界。计划最多容纳 255 条目录加文件操作，屏幕只列出前
//    MaxListedEntries 条；超出时渲染显式的"以及另外 N 条"截断标记（它是回显内容的一部
//    分），并渲染绑定声明：指纹覆盖完整计划，包括未列出的条目。截断的只是清单，身份回声
//    仍绑定完整计划。
//
// 4. 风险是有序的显式警告，而不是排版暗示：目标非空（无冲突但已有内容）、already-in-place
//    文件在场（无需写入，但执行侧仍要复核摘要）、共享设置文件恢复（`mcp:` 主体的恢复是整
//    文件语义——恢复会覆盖整个共享设置文件，包括其他服务器的配置；此警告对 mcp 主体是强
//    制的，缺失即呈现失败而不是少了点缀）、大型恢复、陈旧备份。
//
// 任何展示不安全的文本、缺失的备份描述字段或内部不一致（计划身份畸形、条目摘要畸形、
// 描述与计划不符、目标根漂移）都以独立的 `extension-restore-presentation-*` 代号整体拒
// 绝渲染：绝不清洗、绝不猜测。
enum class ExtensionStagingRestorePromptState {
    // 计划完整、可安全展示，人可以复核它。
    Ready,
    // 计划构建失败：渲染拒绝理由，绝不渲染计划摘要。
    Refused,
    // 内容无法安全展示或输入内部不一致。
    Unpresentable,
};

// 需要在恢复界面上显式标记的风险，按固定顺序输出。
enum class ExtensionStagingRestoreWarning {
    // 目标无冲突但非空：计划路径的祖先目录或一致文件已存在于目标。
    DestinationNotEmpty,
    // 目标已有与计划逐字节一致的文件：无需写入，但执行侧仍必须复核其摘要。
    AlreadyInPlaceFiles,
    // `mcp:` 主体的恢复是整文件语义：恢复覆盖整个共享设置文件，包括其他服务器的配置。
    // 对 mcp 主体此警告是强制的；缺失即呈现失败。
    SharedSettingsFileRestore,
    // 文件数或总字节数越过大型恢复阈值。
    LargeRestore,
    // 备份创建时间距今越过陈旧阈值。
    OldBackup,
    // 当前不存在任何恢复执行路径：此呈现仅供人工复核。当且仅当调用方未声明执行路径
    // 在场（`executionAvailable` 为 false）时必须显式说明，否则沉默会让人以为恢复已经
    // 能执行。被拒绝的计划真的不会执行，因此 buildRefusal 恒携带它。
    RestoreDoesNotExecuteYet,
};

// 呈现在屏幕上的一条计划操作。全部文本已通过共享展示安全层。
struct ExtensionStagingRestoreEntryRow {
    bool directory = false;
    QString relativePath;
    // 仅文件：期望字节数与期望 SHA-256（小写十六进制）。
    qint64 byteCount = 0;
    QString sha256;
    bool alreadyInPlace = false;
};

// 计划来源备份的描述，取自清点层。呈现层据此说明正在恢复的是哪一份备份。
struct ExtensionStagingRestoreBackupDescriptor {
    QString backupId;
    QString subject;
    QDateTime createdAt;
    ExtensionStagingBackupEntryVerification verification =
        ExtensionStagingBackupEntryVerification::ListedCorrupt;
};

struct ExtensionStagingRestorePrompt {
    ExtensionStagingRestorePromptState state =
        ExtensionStagingRestorePromptState::Unpresentable;
    // 仅 Ready 为真：本提示携带一份可复核的完整计划摘要。Refused 与 Unpresentable
    // 永远为假——失败或不可展示的计划上没有可批准的标的物。
    bool approvable = false;

    // 以下为可安全展示的文本，长度有界。Refused / Unpresentable 状态下保持为空。
    QString subject;
    QString backupId;
    QString createdAtLabel;
    QString destinationRoot;
    // 完整身份与两端指纹（仅展示用；复核绑定完整身份）。
    QString planIdentity;
    QString planFingerprint;
    QString treeIdentity;
    QString treeFingerprint;
    // 操作统计：目录创建数、需写入文件数、already-in-place 文件数、文件总字节数
    // （含 already-in-place 文件的内容字节）。
    int directoryCount = 0;
    int fileWriteCount = 0;
    int alreadyInPlaceCount = 0;
    qint64 totalBytes = 0;
    // 逐条清单，最多 MaxListedEntries 条，保持计划顺序。
    QVector<ExtensionStagingRestoreEntryRow> entries;
    // 截断语义：标记文本本身是回显内容的一部分。
    bool listingTruncated = false;
    int omittedEntryCount = 0;
    QString truncationNote;
    // 绑定声明：指纹覆盖完整计划，包括未列出的条目。
    QString identityBindingNote;

    QList<ExtensionStagingRestoreWarning> warnings;
    // SharedSettingsFileRestore 警告的完整文案：恢复覆盖整个共享设置文件，包括其他
    // 服务器的配置。仅在该警告在场时填写。
    QString sharedFileOverwriteNote;
    // RestoreDoesNotExecuteYet 的完整文案。仅在调用方声明不存在执行路径
    // （executionAvailable 为 false）或提示为 Refused 时填写；Refused 恒填写——被
    // 拒绝的计划真的不会执行。
    QString doesNotExecuteNote;

    // 回显的身份就是展示的身份：复核流程按它们检测渲染与批准之间的漂移。截断清单不
    // 影响它们——它们绑定的是完整计划。
    QString echoedPlanIdentity;
    QString echoedTreeIdentity;

    // Refused 状态：计划层的拒绝诊断，原样透传。
    QString refusalReason;
    // Unpresentable 状态：本层诊断，前缀 `extension-restore-presentation-`。
    QString errorCode;
};

class ExtensionStagingRestorePresentation
{
public:
    // 逐条清单的固定上限。计划最多 255 条操作；屏幕只列出前 16 条，其余以显式截断
    // 标记交代。截断仅作用于清单，身份回声仍绑定完整计划。
    static constexpr int MaxListedEntries = 16;
    // 单条路径与目标根展示文本的上界：超出即拒绝渲染而不是截断。
    static constexpr int MaxPathCharacters = 256;
    // 备份 id 与拒绝理由的展示上界。
    static constexpr int MaxLabelCharacters = 128;
    // 大型恢复阈值：文件数或总字节数任一越过即标记。
    static constexpr int LargeRestoreFiles = 32;
    static constexpr qint64 LargeRestoreBytes = 1024 * 1024;
    // 陈旧备份阈值（天）。
    static constexpr int OldBackupDays = 90;

    // 渲染一份已成功构建的恢复计划。`descriptor` 描述计划来源的备份（清点层语义），
    // `destinationRoot` 是调用方给出的目标根，必须与计划的规范化目标根逐字节相等；
    // `now` 由调用方注入（陈旧备份判定不自带时钟）；`executionAvailable` 是调用方关于
    // 执行路径是否在场的显式声明——为 true 时不再渲染"仅供复核、不会执行"披露，调用方
    // 绝不能在不存在执行路径时置真。任何不一致都以 Unpresentable 失败关闭。
    static ExtensionStagingRestorePrompt build(
        const ExtensionStagingRestorePlan &plan,
        const ExtensionStagingRestoreBackupDescriptor &descriptor,
        const QString &destinationRoot,
        const QDateTime &now,
        bool executionAvailable = false);

    // 渲染一份构建失败的计划：拒绝理由原样透传为 Refused 状态，没有计划摘要，没有
    // 可批准标记。拒绝理由本身不可安全展示时整体 Unpresentable。
    static ExtensionStagingRestorePrompt buildRefusal(const QString &refusalCode);
};

#endif // EXTENSION_STAGING_RESTORE_PRESENTATION_H
