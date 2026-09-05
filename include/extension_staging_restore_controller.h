#ifndef EXTENSION_STAGING_RESTORE_CONTROLLER_H
#define EXTENSION_STAGING_RESTORE_CONTROLLER_H

#include "extension_staging_restore_approval.h"
#include "extension_staging_restore_audit_ledger_store.h"

// 暂存恢复审批控制器：把审批策略与认证审计链接在一起，让一份恢复决定被如实记录。
// 形状与复核/启用控制器同构：读取 → 判定 → 通过代号比较并交换提交 → 重新读取。
//
// 这一层拥有的只是顺序纪律，不拥有任何判定：
//
// 1. 批准是否有效完全由 `ExtensionStagingRestoreApprovalPolicy::evaluate` 决定。本层
//    绝不从提示字段重新推导批准（两份判定副本会各自漂移），也不改写策略的结论。
//
// 2. 记录的是"人做的决定"，不是"策略的结论"。两者必须精确区分：一个逐项对齐后被
//    批准的恢复记录为 approved（携带授权）；一个被问过并且被回答了"不"的恢复记录
//    为 declined（不携带任何授权，只证明问题被问过）；而策略的**拒绝**（提示不可
//    展示、验证状态不对、任何维度漂移、缺少确认……）意味着没有一个有效的问题被
//    回答过——它不是决定，一个字节也不写。把拒绝记录成 declined 会把"从未问出
//    一个有效问题"伪造成"用户拒绝了"。
//
// 3. 提交之后重新读取，而不是相信追加：只有重新读到的字节是真正生效的记录。在
//    "决定已记录"的字样下面躺着一份读不出的日志，等于把一次篡改表述成历史。
//
// 4. 读不出的审计链阻止记录，并且以独立状态呈现：当前内容未知时绝不能把这次决定
//    写成历史。失败的记录保持冻结，直到调用方重新加载。
//
// 5. 并发决定由代号比较并交换裁决：冲突以独立代号报告（不静默重试，也不是最后
//    写入者获胜），由调用方重新加载并重新提问。
//
// 这一层绝不执行恢复，也不暴露任何执行路径：批准决定返回的凭据对象是纯数据，
// 绑定确切计划身份，今天没有任何东西消费它。
struct ExtensionStagingRestoreRecordResult
{
    // 决定已提交进审计链，并从提交后重新读取的字节确认。只有这时 recordedEntry
    // 与 ledger 描述生效的历史。
    bool recorded = false;
    // 记录的是哪一个决定。仅 recorded 时有意义：declined 条目不携带任何授权。
    ExtensionStagingRestoreAuditDecision decision =
        ExtensionStagingRestoreAuditDecision::Declined;
    // 审批策略的判定原样透传。策略拒绝（Refused）时本结果 recorded == false 且
    // 审计链一个字节未被触碰；errorCode 透传策略代号。
    ExtensionStagingRestoreApprovalVerdict verdict;
    // 提交后重新读取的审计链状态：只有这些字节是"生效"的权威。未走到提交时它是
    // 记录前读到的状态（供调用方比较身份以验证零写入）；策略拒绝时保持默认。
    ExtensionStagingRestoreAuditStoreResult ledger;
    QString errorCode;
};

class ExtensionStagingRestoreController
{
public:
    // 读取当前审计链。没有存储时报告 Unavailable，绝不退化成"从未记录过"。
    static ExtensionStagingRestoreAuditStoreResult inspect(
        ExtensionStagingRestoreAuditLedgerStore *store);

    // 记录一份恢复决定。prompt 必须是当前重新渲染的结果，backupVerification 必须
    // 是清点层当前对该备份的验证状态，decidedAt 由调用方注入（本层不自带时钟，
    // 且必须是有效的 UTC 时间——歧义的本地墙钟时间不属于审计记录）。全部判定
    // 委托给审批策略；本层只负责如实记录与诚实报告。
    static ExtensionStagingRestoreRecordResult record(
        const ExtensionStagingRestorePrompt &prompt,
        ExtensionStagingBackupEntryVerification backupVerification,
        const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement,
        const QDateTime &decidedAt,
        ExtensionStagingRestoreAuditLedgerStore *store);
};

#endif // EXTENSION_STAGING_RESTORE_CONTROLLER_H
