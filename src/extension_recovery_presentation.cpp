#include "extension_recovery_presentation.h"

namespace {

// 四个恒定字段必须写在每一条返回路径上,而不是留给结构体默认值:源码里没有任何一处声明
// 这件事,等于这些不变量只存在于注释里。
void fixInvariants(ExtensionRecoveryPrompt *prompt)
{
    prompt->withdrawsAuthorityOnly = true;
    prompt->removesSourceContent = false;
    prompt->clearsReviewRecords = false;
}

} // namespace

ExtensionRecoveryPrompt ExtensionRecoveryPresentation::build(
    const ExtensionRecoveryView &view)
{
    ExtensionRecoveryPrompt prompt;
    prompt.need = view.assessment.need;
    // 诊断原样带出。恢复恰好是最需要能查出处的那条路径。
    prompt.errorCode = view.assessment.errorCode;
    // 可确认性只有一个来源。这一层自己再判一遍必然与判定层漂移,而漂移的方向是界面对一份
    // 读不到的账本提供清空动作。
    prompt.confirmationRequired = view.assessment.operatorConfirmationRequired;
    // 代号原样回传,包括 0:一份自相矛盾的账本没有可信代号,而界面若改用自己记着的上一个
    // 代号,判定层会以为界面看的是另一份账本而拒绝这次恢复。
    prompt.expectedGeneration = view.generation;
    // 读不出来的账本不显示条数:那个数字是编出来的,而屏幕上的一个具体数字会让人相信自己
    // 知道这次清空的范围。可读时即使是 0 也是真的 0,因此"已知"与"非零"是两件事。
    prompt.grantCountKnown = ExtensionRecoveryGate::authoritative(view.grantState);
    prompt.visibleGrants = prompt.grantCountKnown ? view.visibleGrants : 0;
    fixInvariants(&prompt);

    // 四个结论是四句不同的话。折叠成"有问题/没问题"会让读不到与结果未知这两种情况也长出
    // 一个清空按钮,而那两种情况下清空会销毁看不见的授权。
    switch (view.assessment.need) {
    case ExtensionRecoveryNeed::None:
        prompt.state = ExtensionRecoveryPromptState::Informational;
        prompt.headline = QStringLiteral("授权账本状态正常,无需恢复。");
        // 必须明确说明这里没有动作,而不是留空:留空会让人以为界面出错了,于是去找别的
        // 路径清理。同时要说清健康账本上没有恢复动作是刻意的——否则它看起来像缺失的功能。
        prompt.guidance = QStringLiteral(
            "没有可执行的恢复动作。恢复只用于处理损坏的账本;"
            "对正常账本提供恢复等于提供一条不经审批就收回全部授权的路径。");
        return prompt;
    case ExtensionRecoveryNeed::Blocked:
        prompt.state = ExtensionRecoveryPromptState::Informational;
        prompt.headline = QStringLiteral("当前读不到授权账本,无法判断有哪些授权。");
        prompt.guidance = QStringLiteral(
            "请先恢复安全存储的访问,再回到这里。"
            "此刻不执行任何清空:清空一份读不到的授权集合会销毁看不见的授权,"
            "而它们现在无法摆给你确认。");
        return prompt;
    case ExtensionRecoveryNeed::Reconfirm:
        prompt.state = ExtensionRecoveryPromptState::Informational;
        prompt.headline = QStringLiteral("上一次授权写入的结果未知,当前有效内容无从判断。");
        // 与 Blocked 是两句不同的话:这里的正确动作是重新读取,而不是等存储恢复。把两者
        // 说成同一句会让人去修一个没有坏的后端。
        prompt.guidance = QStringLiteral(
            "请重新读取以确立当下状态。此刻不在未知之上写入:"
            "那可能覆盖一次其实已经提交的授权。");
        return prompt;
    case ExtensionRecoveryNeed::ClearGrants:
        prompt.state = ExtensionRecoveryPromptState::Actionable;
        prompt.headline = QStringLiteral("授权账本自相矛盾,无法判断哪一条授权曾经有效。");
        // 必须说清三件事:唯一可做的重建是空集合、它不删除磁盘内容、它不清除复核记录。
        // 少说第一件会让人以为账本能被修回原样;少说后两件会让人以为扩展已被清理干净,
        // 或以为审计线索已经没了。
        prompt.guidance = QStringLiteral(
            "唯一诚实的重建是把授权归零:无法知道哪一条曾经有效,"
            "而把它猜回一份非空的授权集合等于伪造你的同意。"
            "确认后将收回全部授权。这不会删除磁盘上的任何扩展内容,"
            "也不会清除复核记录——复核记录是事后审计唯一的证据来源,"
            "而保留它不会让任何内容重新获得运行授权。");
        return prompt;
    }

    // 未知结论按不可读处理,并且不提供任何动作:新增的判定结果不应默认长出一个清空按钮。
    prompt.state = ExtensionRecoveryPromptState::Informational;
    prompt.need = ExtensionRecoveryNeed::Blocked;
    prompt.confirmationRequired = false;
    prompt.grantCountKnown = false;
    prompt.visibleGrants = 0;
    prompt.headline = QStringLiteral("无法识别授权账本的状况。");
    prompt.guidance = QStringLiteral(
        "没有可执行的恢复动作。请报告这一诊断代号。");
    if (prompt.errorCode.isEmpty()) {
        prompt.errorCode = QStringLiteral("extension-recovery-prompt-state-unknown");
    }
    fixInvariants(&prompt);
    return prompt;
}
