#ifndef EXTENSION_STAGING_RESTORE_EXECUTOR_H
#define EXTENSION_STAGING_RESTORE_EXECUTOR_H

#include "extension_staging_restore_approval.h"
#include "extension_staging_restore_plan.h"

#include <QString>
#include <QVector>

// 扩展暂存恢复执行器契约：把（凭据 + 计划 + 已验证快照）变成文件系统现实。这是暂存恢复链
// 上第一个真实写扩展目标树的组件——与 ToolManager 经审查的配置写入、MCP 对话框保存同属
// COMPANION 侧、用户主导（凭据由人逐项对齐批准而来）的写入类；它不是 agent 编写的变更，
// 不改变任何 agent/adapter 的能力面，Agent/Codex 保持只读。
//
// 本组件当前【没有任何产品调用方】：没有 UI、对话框、工作台或 ToolManager 引用它，产品里
// 没有任何东西能触达它。执行结果也不进审计链——执行结果的审计记录是另一道接线决定，本层
// 不自带存储。
//
// 信任边界有三条，每条都不能省：
//
// 1. 凭据绑定是全部意义所在。执行入口要求三件套同时成立：一份【执行开始时重新验证】的快照
//    （绝不相信调用方"已验证"的声称）、一份从该快照构建的计划、一份
//    `authorizedPlanIdentity` 与 `authorizedTreeIdentity` 与该计划/快照逐字节相等的批准
//    凭据。任何一项不符都是各自独立的拒绝，零写入。给计划 A 签发的凭据永远执行不了计划 B。
//
// 2. 执行前重观察（pre-flight）。计划是针对某一个被观察到的目标状态决定的；对另一个状态执
//    行同一份计划等于伪造复核。因此在第一个字节写入之前，执行器用与计划层完全相同的纪律重
//    新观察目标：规范化根、根非符号链接、逐条操作的祖先符号链接复查、以及对照计划期望的冲
//    突复查（already-in-place 条目重读重哈希、应为缺失的条目仍缺失）。计划与执行之间发生过
//    任何漂移（含 already-in-place 文件被改动——那是冲突而不是跳过）都以
//    `extension-restore-execution-destination-drift` 拒绝，零写入。
//
// 3. 每条操作的包含性重查。计划层已经查过路径形状，但执行器是最后一道防线：每一条操作在
//    被执行之前重新验证其相对路径不逃出目标根（`..`、绝对路径、空段一律拒绝），写入只落在
//    计划的精确路径上。验证过的快照携带不了逃逸路径，因此这一代号由守卫本身与策略 pin 守
//    住——与计划层 `path-escapes-destination` 同一代号，同一语义。
//
// 执行纪律：目录创建在前（清单顺序），文件写入在后（计划顺序）；每一次文件写入都是原子的
// （QSaveFile——本仓库既有的原子写惯例），只写计划的精确路径；already-in-place 条目走
// 跳过并复核（重读既有字节、对期望摘要重哈希），验证永不被静默跳过。每一次写入之后立即
// 重读重哈希，摘要不符即执行失败。
//
// 失败补偿语义：执行中途失败时立即停下，绝不越过失败点继续；结果如实报告每一条已完成的
// 操作与确切的失败点，部分应用的恢复必须可与完整恢复区分（`Partial` 明确意味着目标处于
// 混合状态）。本层【不做自动回滚】：回滚一份恢复本身就是另一份决定——恢复前的目标状态并
// 没有在这里被捕获，凭空发明它就是第二次未经复核的变更。诚实的契约是：恢复前先捕获是调
// 用方有据可查的责任（暂存备份链正是为此存在），执行器则精确报告它改动了什么，使得反向
// 恢复成为可能。
//
// 结果分类：`Complete` 要求每一条操作都被验证（写入后重读一致或跳过复核一致）；`Partial`
// 表示至少一条操作已完成后失败，目标处于混合状态；`Refused` 表示在任何写入之前被拒绝，
// 零写入；`NotStarted` 表示执行已越过全部门禁但第一条操作就失败，没有任何操作完成。
enum class ExtensionStagingRestoreExecutionState {
    Complete,
    Partial,
    Refused,
    NotStarted,
};

enum class ExtensionStagingRestoreOperationOutcome {
    // 未触达：执行在它之前停止，或整体被拒绝。
    NotAttempted,
    // 已执行并验证（目录已创建 / 文件已写入且写后重读一致）。
    Done,
    // already-in-place：未写入任何字节，既有内容已通过摘要复核。
    SkippedVerified,
    // 本条操作失败；errorCode 携带确切原因。执行在此停止。
    Failed,
};

struct ExtensionStagingRestoreOperationResult {
    ExtensionStagingRestoreOperationOutcome outcome =
        ExtensionStagingRestoreOperationOutcome::NotAttempted;
    bool directory = false;
    QString relativePath;
    // 仅 Failed：本条操作的失败诊断（`extension-restore-execution-*`）。
    QString errorCode;
};

struct ExtensionStagingRestoreExecutionResult {
    ExtensionStagingRestoreExecutionState state =
        ExtensionStagingRestoreExecutionState::Refused;
    // 整体诊断：Refused / Partial / NotStarted 时的拒绝或失败代号；Complete 时为空。
    QString errorCode;
    // 失败点在计划操作序列中的下标；无失败（Complete / Refused）时为 -1。
    int failureIndex = -1;
    int doneCount = 0;
    int skippedVerifiedCount = 0;
    int failedCount = 0;
    // 被执行的计划与树身份，原样回显：供调用方把执行结果与它批准的对象对账（未来的
    // 审计接线消费它们；本层不自带存储）。
    QString planIdentity;
    QString treeIdentity;
    // 逐条操作结果，与计划操作同序等长。
    QVector<ExtensionStagingRestoreOperationResult> operations;
};

class ExtensionStagingRestoreExecutor
{
public:
    // 执行一份已批准的暂存恢复。`captureDomain` 必须就是构建快照时所用的捕获域；
    // `observation` 必须非空且是目标根的真实只读观察（生产接线注入磁盘观察；测试可注入
    // 受控观察）。任何拒绝都发生在第一个字节写入之前。
    static ExtensionStagingRestoreExecutionResult execute(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QString &expectedSubject,
        const ConfigurationBackupSnapshot &snapshot,
        const ExtensionStagingRestorePlan &plan,
        const ExtensionStagingRestoreApprovalVerdict &credential,
        ExtensionStagingRestoreObservation *observation);
};

#endif // EXTENSION_STAGING_RESTORE_EXECUTOR_H
