#ifndef EXTENSION_STAGING_RESTORE_APPROVAL_H
#define EXTENSION_STAGING_RESTORE_APPROVAL_H

#include "extension_staging_restore_presentation.h"

// 暂存恢复审批策略。呈现层决定"能不能问"，这一层决定"这个回答是否构成恢复授权"。两者必须
// 分开：一份可以安全展示的恢复提示不等于一份有效的批准，而伪造或过期的批准正是把"工具输出
// 里的一段文字"变成"用户要求把这份备份写回目标"的路径。
//
// 因此批准不是一个布尔值，而是一份必须与渲染出的提示逐项对齐的凭据：主体、备份 id、目标
// 根、回显的计划身份与回显的树身份（两者都必须对齐——计划身份绑定目标根与全部操作，树身份
// 绑定内容本身；只批准其一就留下一个漂移通道：另一份内容可以套用同一份计划身份，或同一份
// 内容换一套操作）、确切的披露警告集合、以及高风险是否得到逐次显式确认。任何一项与渲染出的
// 提示不一致，批准即失效。
//
// 恢复相关的门禁作为必需输入进入判定，而不是留给调用方各自记住（准入门禁"必需参数、无默认
// 值"的先例）：提示本身的呈现状态之外，备份的清点验证状态是独立的必需参数——从一份
// 未通过清单身份级验证的备份恢复，等于从未经认证的字节恢复，比不恢复更糟，因此一律拒绝。
// 这一层绝不从原始字节重新推导计划有效性：提示回显的身份就是权威，但验证状态必须作为
// 自己的参数进场。
//
// 高风险永远逐次显式确认，绝不产生可复用规则。本层不提供任何"记住"范围：恢复是罕见的
// 一次性决定，而任何被记住的规则唯一的消费场景是自动放行未来的恢复提示——同一份备份对
// 另一个目标根重新计划就是另一份计划（计划身份绑定目标根），必须重新批准；既然任何宽于
// 确切计划身份的记住范围都会把对一份计划的同意转移到另一份计划上，而确切计划身份本身
// 就是凭据绑定的内容，可记住的范围没有存在的余地。
//
// 这一层不执行、不持久化、不写任何东西：批准产出的是绑定确切计划身份的凭据对象，它是纯
// 数据——今天没有任何东西消费它来执行恢复，本层也不暴露任何执行钩子。批准的恢复本身
// 绝不因此获得执行能力。
enum class ExtensionStagingRestoreApprovalDecision {
    Approve,
    Decline,
};

// 人在恢复复核界面上做出的决定，必须回传当时渲染出的全部内容。
struct ExtensionStagingRestoreApprovalAcknowledgement {
    ExtensionStagingRestoreApprovalDecision decision =
        ExtensionStagingRestoreApprovalDecision::Decline;
    // 屏幕上确切显示的主体、备份 id 与目标根，而不是"当前的值"。
    QString subject;
    QString backupId;
    QString destinationRoot;
    // 屏幕上回显的完整计划身份与树身份。批准绑定的是渲染出的那一份，渲染之后发生的
    // 任何漂移都让批准失效，而不是被套用到新内容上。
    QString approvedPlanIdentity;
    QString approvedTreeIdentity;
    // 屏幕上确切披露的警告集合。批准的是"我看到了这些风险并接受"：披露过的警告缺一
    // 不可，回传未披露的警告说明这份批准来自另一个界面状态，什么都不回传等于"我没看
    // 到任何风险"——三者分别拒绝。
    QList<ExtensionStagingRestoreWarning> acknowledgedWarnings;
    // 高风险恢复必须逐次显式确认。
    bool highRiskConfirmed = false;
};

enum class ExtensionStagingRestoreApprovalState {
    // 凭据此刻构成有效的恢复授权，绑定确切计划身份。
    Authorized,
    // 凭据不构成授权。errorCode 说明原因。
    Refused,
};

struct ExtensionStagingRestoreApprovalVerdict {
    ExtensionStagingRestoreApprovalState state =
        ExtensionStagingRestoreApprovalState::Refused;
    // 授权绑定的确切计划身份与树身份。拒绝时为空。凭据是纯数据：没有任何执行路径消费
    // 它。
    QString authorizedPlanIdentity;
    QString authorizedTreeIdentity;
    QString errorCode;
};

class ExtensionStagingRestoreApprovalPolicy
{
public:
    // prompt 必须是当前重新渲染的结果，backupVerification 必须是清点层当前对该备份的
    // 验证状态：两者都是必需参数，没有默认值，"忘记传入"不是可达状态。来自缓存的任何
    // 一项都会让批准描述一个已经不存在的状态。
    static ExtensionStagingRestoreApprovalVerdict evaluate(
        const ExtensionStagingRestorePrompt &prompt,
        ExtensionStagingBackupEntryVerification backupVerification,
        const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement);

    // 该警告在给定计划形态下是否要求逐次显式确认。`fileWriteCount` 是提示渲染出的待写
    // 文件数：DestinationNotEmpty 只有在计划真的会向非空目标写入文件时才是冲突邻接的
    // 高风险；非空完全由 already-in-place 文件证明时没有任何字节会被写入，是纯信息性
    // 披露。纯粹信息性的警告（不执行披露、already-in-place、大型、陈旧）不要求确认——
    // 人人都点的复选框会退化成一个摆设。
    static bool requiresExplicitConfirmation(
        ExtensionStagingRestoreWarning warning, int fileWriteCount);
};

#endif // EXTENSION_STAGING_RESTORE_APPROVAL_H
