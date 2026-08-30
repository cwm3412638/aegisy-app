#ifndef EXTENSION_ADMISSION_GATE_H
#define EXTENSION_ADMISSION_GATE_H

#include "execution_sandbox_gate.h"
#include "extension_approval_policy.h"
#include "extension_enablement_ledger_store.h"

// 准入门禁。前面四层各自回答一个问题:呈现回答"能不能问",审批回答"这个回答是否构成
// 授权",沙箱回答"系统能否强制边界",恢复回答"账本本身可信吗"。四个结论都正确,却没有
// 任何一层保证它们被同时征询过。
//
// 这正是真正的危险所在:一个只查询了其中三道门的调用方会静默通过。四道门分散在四个类型
// 里时,漏查一道不会产生任何编译错误,也不会产生任何诊断——它只是让一份授权在缺少一项
// 前提的情况下成立。因此四道门的合取本身必须是一个被实现、被测试的对象,而不是一份留给
// 每个调用方各自记住的约定。
//
// 这一层不安装、不写盘、不执行、不修改任何门禁的结论。它只把四份已有结论合成一个准入
// 判定,并且要求每一份结论都由调用方显式提供:没有默认值,因此没有"忘记传入"这种状态。
enum class ExtensionAdmissionState {
    // 四道门全部满足,可以据此规划一份授权。
    Admitted,
    // 至少一道门不满足。errorCode 指明第一道未满足的门。
    Refused,
};

struct ExtensionAdmissionVerdict {
    ExtensionAdmissionState state = ExtensionAdmissionState::Refused;
    // 被准入的确切内容摘要,来自审批结论。被拒绝时为空。
    QString authorizedContentIdentity;
    // 是否授予可复用规则。高风险始终为假,由审批层判定,这一层不放宽。
    bool ruleGranted = false;
    // 这份内容运行所需要的强制级别,由人在屏幕上看到的能力披露推导。
    SandboxExecutionAuthority requiredAuthority =
        SandboxExecutionAuthority::ReadOnly;
    QString errorCode;
};

class ExtensionAdmissionGate
{
public:
    // 四份结论全部为必需参数。ledger 必须是当前重新读取的结果,prompt 必须是当前重新
    // 渲染的结果,sandbox 必须是当前平台的强制结论:任何一项来自缓存都会让准入判定
    // 描述一个已经不存在的状态。
    static ExtensionAdmissionVerdict evaluate(
        const ExtensionEnablementLedgerStoreResult &ledger,
        const ExtensionEnablementPrompt &prompt,
        const ExtensionApprovalAcknowledgement &acknowledgement,
        const SandboxVerdict &sandbox);

    // 这份提问所披露的内容需要哪一级强制。关键在于它读的是**呈现给人的披露**而不是
    // 重新读取记录:否则一条在渲染之后被改写的记录可以借此降低自己的强制要求。
    static SandboxExecutionAuthority requiredAuthority(
        const ExtensionEnablementPrompt &prompt);
};

#endif // EXTENSION_ADMISSION_GATE_H
