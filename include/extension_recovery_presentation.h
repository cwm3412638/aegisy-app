#ifndef EXTENSION_RECOVERY_PRESENTATION_H
#define EXTENSION_RECOVERY_PRESENTATION_H

#include "extension_recovery_controller.h"

#include <QString>

// 恢复的呈现层。判定层能得出结论,执行层能收回授权,但没有任何东西把结论摆给人看,于是
// "授权账本坏了,需要你决定"这句话到此为止无法被说出口——而恢复恰好是四道门禁里唯一必须
// 由人在看清损坏之后才能推进的一道。
//
// **四个结论是四句不同的话,不是一个状态的四种程度。** `None` 说"没坏",`Blocked` 说
// "现在读不到,什么都别做",`Reconfirm` 说"上次的结果未知,要重新读",`ClearGrants` 说
// "证据自相矛盾,只能把授权归零"。把它们折叠成"有问题/没问题"会让前三种情况都长出一个
// 清空按钮,而其中两种情况下清空会销毁看不见的授权。
//
// **一个能被确认的动作就是一个会被执行的动作。** 因此可确认性不由这一层推导:它只能来自
// 判定层的 `operatorConfirmationRequired`。这一层自己再判一遍必然会与它漂移,而漂移的
// 方向是界面对一份读不到的账本提供清空动作。
//
// **这一层必须说出恢复会做什么,以及它不会做什么。** 收回授权不删除磁盘上的任何内容,也
// 不清除复核记录。人如果以为"恢复"把扩展清理干净了,就会停止清理,而内容还在原处;人如果
// 以为复核记录也被清掉了,就会以为自己失去了审计线索,而那份线索恰恰被完整保留着。
//
// **无法读出的授权条数不显示。** 一份自相矛盾或读不到的账本里"有几条授权"这个数字是编
// 出来的,而屏幕上的一个具体数字会让人相信自己知道这次清空的范围。
//
// 这一层只做呈现。它不读盘、不写盘、不清空事务、不执行任何东西。
enum class ExtensionRecoveryPromptState {
    // 存在需要人决定的损坏,并且此刻可以请求确认。
    Actionable,
    // 可以说明当下状况,但不允许任何恢复动作:读不到内容,或结果未知,或账本本来就是好的。
    Informational,
};

struct ExtensionRecoveryPrompt {
    ExtensionRecoveryPromptState state =
        ExtensionRecoveryPromptState::Informational;
    ExtensionRecoveryNeed need = ExtensionRecoveryNeed::Blocked;
    // 屏幕上那句话。四个结论对应四句不同的话。
    QString headline;
    // 现在该做什么。`Informational` 时它说明为什么没有动作可做,而不是留空——留空会让人
    // 以为界面出错了,于是去找别的路径清理。
    QString guidance;
    // 判定层的诊断,原样带出。这一层再编一个代号会让人拿着一个查不到出处的东西。
    QString errorCode;
    // 是否要求人显式确认。只能来自判定层。
    bool confirmationRequired = false;
    // 人必须回传的代号。一份自相矛盾的账本没有可信代号,读出来就是 0,而这个 0 必须被
    // 原样回传:界面若改用自己记着的上一个代号,判定层会以为界面看的是另一份账本。
    qint64 expectedGeneration = 0;
    // 当前能读到的授权条数,并且只在真的读得出来时才有意义。
    bool grantCountKnown = false;
    int visibleGrants = 0;
    // 恒为真:恢复只收回授权,从不授予。
    bool withdrawsAuthorityOnly = true;
    // 恒为假:恢复不删除磁盘上的内容。
    bool removesSourceContent = false;
    // 恒为假:恢复不清除复核记录——它是事后审计唯一的证据来源,而清掉它并不减少任何授权。
    bool clearsReviewRecords = false;
};

class ExtensionRecoveryPresentation
{
public:
    static ExtensionRecoveryPrompt build(const ExtensionRecoveryView &view);
};

#endif // EXTENSION_RECOVERY_PRESENTATION_H
