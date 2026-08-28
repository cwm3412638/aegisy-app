#ifndef EXTENSION_APPROVAL_POLICY_H
#define EXTENSION_APPROVAL_POLICY_H

#include "extension_enablement_presentation.h"

// 审批门禁。呈现层决定"能不能问"，这一层决定"这个回答是否构成授权"。两者必须分开：
// 一个可以安全展示的提问不等于一份有效的批准，而伪造或过期的批准正是把"工具输出里的
// 一段文字"变成"用户要求运行这份内容"的路径。
//
// 因此批准不是一个布尔值，而是一份必须与当时屏幕上的内容逐项对齐的凭据：目标、来源与
// 内容摘要、被披露的风险集合、以及高风险是否得到显式确认。任何一项与当前状态不一致，
// 批准即失效——这比"批准过就一直有效"更严格，因为后者会让人对一份内容的同意被套用到
// 另一份内容或另一组风险上。
//
// 这一层不持久化、不启用、不执行任何东西：它只判定一份凭据此刻是否仍然构成授权。
enum class ExtensionApprovalDecision {
    Approve,
    Decline,
};

// 记住选择的范围。没有比"确切内容摘要"更宽的选项：任何按名称或按标识记住的规则都会在
// 内容变更后继续生效，也就是把对一份内容的同意转移到从未被看过的另一份内容上。
enum class ExtensionApprovalScope {
    // 仅本次决定，不留下可复用的规则。
    OnceForThisContent,
    // 记住这份确切内容的决定；内容摘要变化后自动失效。
    RememberForThisContent,
};

// 人在审批界面上做出的决定，必须回传当时展示的全部内容。
struct ExtensionApprovalAcknowledgement {
    ExtensionApprovalDecision decision = ExtensionApprovalDecision::Decline;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 屏幕上确切显示的摘要，而不是"当前记录的摘要"。
    QString approvedSourceIdentity;
    QString approvedContentIdentity;
    // 屏幕上确切披露的风险集合。批准的是"我看到了这些风险并接受"，因此渲染之后新增的
    // 风险必须让批准失效，而不是被静默继承。
    QList<ExtensionEnablementWarning> acknowledgedWarnings;
    // 高风险操作必须逐次显式确认，不能由"记住我的选择"覆盖。
    bool highRiskConfirmed = false;
    ExtensionApprovalScope scope = ExtensionApprovalScope::OnceForThisContent;
};

enum class ExtensionApprovalState {
    // 凭据此刻构成有效授权。
    Authorized,
    // 凭据不构成授权。errorCode 说明原因。
    Refused,
};

struct ExtensionApprovalVerdict {
    ExtensionApprovalState state = ExtensionApprovalState::Refused;
    // 授权绑定的确切内容摘要，供后续启用请求使用。失效时为空。
    QString authorizedContentIdentity;
    // 可复用规则是否被授予。高风险始终为 false：它必须逐次确认。
    bool ruleGranted = false;
    QString errorCode;
};

class ExtensionApprovalPolicy
{
public:
    // prompt 必须是当前重新渲染的结果，而不是发起审批时缓存的那一份：只有重新渲染才能
    // 发现渲染之后发生的漂移。
    static ExtensionApprovalVerdict evaluate(
        const ExtensionEnablementPrompt &prompt,
        const ExtensionApprovalAcknowledgement &acknowledgement);

    // 该风险是否要求逐次显式确认。写入与执行类能力即使已被复核也必须逐次确认：它们是
    // 当前只读边界之外的授权，不能由一条记住的规则批量放行。
    static bool requiresExplicitConfirmation(ExtensionEnablementWarning warning);
};

#endif // EXTENSION_APPROVAL_POLICY_H
