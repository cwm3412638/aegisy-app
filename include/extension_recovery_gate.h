#ifndef EXTENSION_RECOVERY_GATE_H
#define EXTENSION_RECOVERY_GATE_H

#include "extension_enablement_ledger_store.h"

// 恢复门禁。前面几层都假设授权账本可读:复核回答"有人看过这份内容吗",审批回答"这个
// 回答是否构成授权",沙箱回答"系统能否强制边界"。这一层回答当账本本身不可信时该怎么办。
//
// 这不是一个可选的收尾功能,而是一道门禁,因为不可读的授权账本在没有恢复路径时是个死
// 胡同:一次被中断的发布会永久停住这台机器上的全部启用判定。已记录的激活恢复先例正是
// 这个形状——`RecoveryRequired` 曾经正确地拒绝猜测,却没有给操作者留出路。
//
// 恢复必须遵守三条铁律:
//   一、恢复不推断过去。一份自相矛盾的账本无法被"修复"成它大概曾经持有的授权集合,那
//       是伪造授权。唯一诚实的重建是空集合,而空集合是收回授权。
//   二、恢复只能减少授权,永不增加。任何能产出非空授权集合的恢复路径都是一条制造同意的
//       路径,比它试图修复的损坏更危险。
//   三、`Empty`、`Invalid`、`Unavailable` 与 `OutcomeUnknown` 是四个不同的结论,不能
//       互相降级。把不可读当成"从未授权过"会清掉看不见的授权;把损坏当成"从未授权过"会
//       把一次篡改表述成用户的选择。
//
// 这一层只做判定与计划。它不读盘、不写盘、不清空事务、不执行任何东西。
enum class ExtensionRecoveryNeed {
    // 账本状态明确,没有需要恢复的损坏。对健康账本提供"恢复"等于提供一条批量撤销的
    // 后门,因此这个结论必须拒绝一切恢复动作。
    None,
    // 当前读不到内容。必须什么都不做:清空读不到的东西会销毁看不见的授权。
    Blocked,
    // 证据自相矛盾。允许在显式确认后收回全部授权,因为无法知道哪一条曾经有效。
    ClearGrants,
    // 上一次发布的结果未知。必须重新读取以确立当下状态,而不是在未知之上写入:那可能
    // 覆盖一次其实已经提交的发布。
    Reconfirm,
};

struct ExtensionRecoveryAssessment {
    ExtensionRecoveryNeed need = ExtensionRecoveryNeed::Blocked;
    // 是否需要操作者显式确认。收回授权始终需要:它是一次真实的授权变更,即使方向是
    // 减少。
    bool operatorConfirmationRequired = false;
    // 恒为真的不变量:这一层能计划的每一个结果都只收回授权,从不授予。
    bool withdrawsAuthorityOnly = true;
    QString errorCode;
};

// 操作者在恢复界面上做出的决定,必须回传当时看到的结论与代号。
struct ExtensionRecoveryRequest {
    // 界面当时展示的结论。与重新评估的结论不一致时说明界面已经过期。
    ExtensionRecoveryNeed acknowledgedNeed = ExtensionRecoveryNeed::None;
    // 读取时的代号,交给存储做比较并交换:并发的授予不允许被恢复静默覆盖。
    qint64 expectedGeneration = 0;
    bool operatorConfirmed = false;
};

enum class ExtensionRecoveryPlanState {
    Ready,
    Refused,
};

struct ExtensionRecoveryPlan {
    ExtensionRecoveryPlanState state = ExtensionRecoveryPlanState::Refused;
    // 提交后应当存在的完整授权集合。恒为空:恢复只收回授权。
    QList<ExtensionEnablementGrant> grants;
    qint64 expectedGeneration = 0;
    // 事务是否可以就此清除。恒为假:必须在提交后重新读取并验证,部分完成的恢复绝不能
    // 被当成已完成的恢复。
    bool clearsTransaction = false;
    QString errorCode;
};

class ExtensionRecoveryGate
{
public:
    static ExtensionRecoveryAssessment assess(
        const ExtensionEnablementLedgerStoreResult &ledger);

    // ledger 必须是当前重新读取的结果,而不是发起恢复时缓存的那一份。
    static ExtensionRecoveryPlan plan(
        const ExtensionEnablementLedgerStoreResult &ledger,
        const ExtensionRecoveryRequest &request);

    // 恢复是否可以被判定为完成。只有重新读取的结果确实是"从未授权过"才算完成:
    // 任何残留的载荷、不可读的后端或未知的写入结果都必须让事务保持打开。
    static bool completed(const ExtensionEnablementLedgerStoreResult &reread);

    // 该状态是否表示授权集合当下可读。恢复只能在不可读时进行。
    static bool authoritative(ExtensionEnablementLedgerStoreState state);
};

#endif // EXTENSION_RECOVERY_GATE_H
