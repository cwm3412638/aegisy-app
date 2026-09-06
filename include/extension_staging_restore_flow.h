#ifndef EXTENSION_STAGING_RESTORE_FLOW_H
#define EXTENSION_STAGING_RESTORE_FLOW_H

#include "extension_staging_backup_capture.h"
#include "extension_staging_backup_inventory.h"
#include "extension_staging_backup_retention.h"
#include "extension_staging_restore_approval.h"
#include "extension_staging_restore_controller.h"
#include "extension_staging_restore_executor.h"
#include "extension_staging_restore_presentation.h"
#include "extension_staging_restore_plan.h"

#include <QDateTime>
#include <QString>

// 扩展暂存恢复的用户发起编排器：把"人在扩展中心点了一份备份的恢复按钮"变成一条完整、
// 有序、不可跳步的恢复链。本组件是恢复链上第一个产品侧编排者——它自己不发明任何判定，
// 只做顺序纪律与诚实报告；全部判定留在各自已有的层里（捕获、清点、快照验证、计划、
// 呈现、审批策略、控制器、执行器）。
//
// 顺序是安全性质，每一步都不能由省掉：
//
//   参数校验 → 主体资格复核 → 恢复前捕获（目标存在时）→ 捕获成功后的保留期修剪
//   （共享唯一入口，结果独立字段如实携带，修剪失败绝不代表捕获失败）→
//   重新清点并取得当前验证状态 → 读回快照 → 规范化目标根 → 构建计划（内部重新验证
//   快照）→ 渲染提示
//
//   批准侧：逐项对齐的批准凭据 → 控制器记录（declined 也记录；策略拒绝零写入）→
//   凭据状态复核（Authorized）→ 执行器执行（执行开始再次重验证快照、pre-flight 重观察）
//   → 执行结果入审计链（绑定同一计划身份；记录失败单独报告，绝不改写执行结果）
//
// 门禁语义：
//
// 1. 资格是封闭的。本组件只为 `mcp:claude-settings` 主体提供恢复：它是唯一既有调用方
//    权威目标映射（MCP 设置文件路径由 ToolManager 配置权威给出）又有真实备份的主体。
//    `skill:` 主体没有调用方权威的目标映射（注册表不携带安装路径，本组件绝不发明）；
//    `codex-plugin:` 按设计没有备份。谓词 `isRestoreOffered` 是"哪一行配得恢复入口"
//    的唯一定义点，对话框按它决定恢复按钮是否在场（缺席而非禁用——在场但灰着的按钮
//    暗示"本来可以，现在不行"，而真相是"这些行不存在恢复入口"）。
//
// 2. 恢复前先捕获。目标文件存在时，先把当前状态捕获为一份新的暂存备份再谈恢复：
//    恢复是整文件覆盖语义，没有恢复前状态就没有回退路径。捕获失败即整体失败关闭，
//    绝不"先恢复再说"。目标文件不存在时诚实跳过（没有可捕获的状态），
//    `preRestoreCaptureSkipped` 如实记录这一点。
//
// 3. 呈现前重新清点。资格判定与验证状态必须来自准备时刻的清单，而不是按钮被渲染时
//    的旧清单——备份可能在两次清点之间损坏或消失，各自独立诊断。
//
// 4. 冲突即拒绝。计划层对"目标已有内容与计划内容逐字节不同"硬拒绝
//    （`extension-staging-restore-destination-conflict`），本编排器原样透传为
//    Refused 呈现，绝不降级成静默覆盖。此时恢复前捕获已经发生，当前内容安然保存在
//    新备份里——对话框如实说出这一点。
//
// 5. 执行只在"决定已记录且凭据 Authorized"之后。记录失败（含策略拒绝与审计链退
//    化）零执行；declined 如实不执行；批准但凭据未授权是防御性拒绝
//    （`extension-restore-flow-credential-not-authorized`）——控制器与执行器之间
//    不允许出现落差。
//
// 本组件不接触 UI（它由 MainWindow 的跟踪 worker 线程调用），不接触网络，不启动子
// 进程。诊断前缀 `extension-restore-flow-*`；下层组件的诊断逐字透传，不另造代号。
class ExtensionStagingRestoreDiskObservation final
    : public ExtensionStagingRestoreObservation
{
public:
    explicit ExtensionStagingRestoreDiskObservation(const QString &root);

    QString canonicalRoot() override;
    NodeKind nodeKind(const QString &relativePath) override;
    bool fileContent(const QString &relativePath, QByteArray *content) override;

private:
    QString m_root;
};

// 一次恢复准备的完整产出。`stage` 非空表示呈现前的门禁失败（`errorCode` 携带确切
// 诊断，下层代号逐字透传）；`stage` 为空时看 `prompt.state`：Ready 可进入批准对话，
// Refused 携带计划层拒绝理由，Unpresentable 携带呈现层诊断。`ok` 仅是
// "stage 为空且 prompt Ready" 的便利汇总。
struct ExtensionStagingRestorePreparation {
    QString stage;
    QString errorCode;
    ExtensionStagingRestorePrompt prompt;
    ConfigurationBackupSnapshot snapshot;
    ExtensionStagingRestorePlan plan;
    ExtensionStagingBackupEntryVerification verification =
        ExtensionStagingBackupEntryVerification::ListedCorrupt;
    // 恢复前捕获产出的新备份 id；目标文件不存在（无状态可捕获）时为空且
    // preRestoreCaptureSkipped 为真。
    QString preRestoreBackupId;
    bool preRestoreCaptureSkipped = false;
    // 恢复前捕获成功后的保留期修剪结果（共享唯一入口
    // ExtensionStagingBackupRetention::pruneAfterCapture）：修剪是捕获成功后的后续
    // 清理，作为独立字段如实携带——绝不与捕获或执行结果互相涂抹，修剪失败也绝不代表
    // 捕获失败。仅在捕获真实发生时为真（目标不存在而诚实跳过捕获时不修剪）。
    bool preRestoreRetentionAttempted = false;
    ExtensionStagingBackupRetentionRun preRestoreRetention;
    QString destinationRoot;
    QString subject;
    QString backupId;
    bool ok = false;
};

struct ExtensionStagingRestoreOutcome {
    // 决定（含 declined）已进入认证审计链。false 时零执行，errorCode 携带确切诊断。
    bool decisionRecorded = false;
    ExtensionStagingRestoreAuditDecision decision =
        ExtensionStagingRestoreAuditDecision::Declined;
    // 执行器真实跑过（declined、记录失败、凭据未授权时均为 false）。
    bool executed = false;
    ExtensionStagingRestoreExecutionResult execution;
    // 执行结果已进入认证审计链（仅 executed 时有意义）。false 且 executed 为真时，
    // outcomeAuditErrorCode 携带确切的审计失败诊断——审计失败不是执行失败：
    // execution 字段如实描述真实发生的执行，绝不被审计失败改写。
    bool outcomeRecorded = false;
    QString outcomeAuditErrorCode;
    QString errorCode;
    ExtensionStagingRestoreApprovalVerdict verdict;
};

class ExtensionStagingRestoreFlow
{
public:
    // "这一行备份是否配得恢复入口"的唯一定义点：通过清单身份级验证、且主体是
    // `mcp:claude-settings`——唯一既有调用方权威目标映射又有真实备份的主体。
    // 损坏行、skill 行、codex 行一律不提供入口。调用方（MainWindow）另有目标可解析
    // 门：设置路径为空或其父目录不存在时恢复不可能生效，入口同样缺席。
    static bool isRestoreOffered(const ExtensionStagingBackupListEntry &entry)
    {
        return entry.verification
                == ExtensionStagingBackupEntryVerification::ListedIntact
            && entry.subject == QStringLiteral("mcp:claude-settings");
    }

    // 准备一次恢复：走完捕获 → 清点 → 读回 → 计划 → 呈现全链。全部参数都是必需输入：
    // `settingsFilePath` 是调用方权威给出的 MCP 设置文件路径，`backupRoot` 与
    // `keyProvider` 是暂存域的唯一产品根与密钥来源，`now` 由调用方注入（本组件不自
    // 带时钟）。可在任意线程调用；只读目标文件，唯一写入是恢复前捕获（写入应用私有
    // 的加密暂存备份存储）。
    static ExtensionStagingRestorePreparation prepare(
        const QString &settingsFilePath,
        const QString &backupId,
        const QString &subject,
        const QString &backupRoot,
        ConfigurationBackupKeyProvider *keyProvider,
        const QDateTime &now);

    // 提交一份已回答的恢复决定：先记录（declined 同样记录），再且仅在记录成功、决定
    // 为 Approve 且凭据 Authorized 时执行；执行之后把执行结果（分类、失败点、计数与
    // 恢复前备份 id 回退指针）如实记录进同一条审计链。`preparation` 必须是 ok 的准备
    // 结果；`decidedAt` 是人做出决定的那一刻（UTC），`outcomeRecordedAt` 是执行结果
    // 落账的那一刻（UTC），都由调用方注入（本组件不自带时钟）；`store` 必须非空。
    // 结果记录失败绝不改写执行结果：execution 如实报告执行，outcomeAuditErrorCode
    // 单独报告审计失败。
    static ExtensionStagingRestoreOutcome commit(
        const ExtensionStagingRestorePreparation &preparation,
        const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement,
        const QDateTime &decidedAt,
        const QDateTime &outcomeRecordedAt,
        ExtensionStagingRestoreAuditLedgerStore *store);
};

#endif // EXTENSION_STAGING_RESTORE_FLOW_H
