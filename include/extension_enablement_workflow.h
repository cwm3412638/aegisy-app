#ifndef EXTENSION_ENABLEMENT_WORKFLOW_H
#define EXTENSION_ENABLEMENT_WORKFLOW_H

#include "extension_enablement_ledger_store.h"

// 人工启用/停用动作被翻译成"提交后的完整授权集合"，而不是就地修改存储。这样授予与
// 撤销的全部安全属性都能在没有任何持久化的情况下判定与测试。
//
// 这一层不安装、不启用、不写盘、不执行：它只产出一份候选授权集合与一个用于比较并
// 交换的代号。即使提交成功，记录也只是"被请求启用"——`ExtensionEnablementPolicy`
// 仍然独立要求已复核、兼容且已安装，注册表还要再判一次同样的门。
enum class ExtensionEnablementAction {
    Enable,
    Disable,
};

// 授予必须携带人工在屏幕上确切看到的摘要。被授权的是"我要求运行的这份内容"，不是
// "这个名字"，因此渲染与授予之间发生的任何内容变化都必须让授予失败，而不是把决定
// 悄悄套用到新内容上。
struct ExtensionEnablementRequest {
    ExtensionEnablementAction action = ExtensionEnablementAction::Enable;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QString reviewedSourceIdentity;
    QString reviewedContentIdentity;
};

enum class ExtensionEnablementPlanState {
    Ready,
    Rejected,
};

struct ExtensionEnablementPlan {
    ExtensionEnablementPlanState state = ExtensionEnablementPlanState::Rejected;
    // 提交后应当存在的完整集合，而不是增量。
    QList<ExtensionEnablementGrant> grants;
    // 读取时的代号，交给存储做比较并交换：并发的授予与撤销因此不会互相覆盖。
    qint64 expectedGeneration = 0;
    // 集合没有变化时不应提交：提交只会白白推进一个代号并改变身份摘要。
    bool changed = false;
    QString errorCode;
};

class ExtensionEnablementWorkflow
{
public:
    static ExtensionEnablementPlan plan(
        const ExtensionEnablementRequest &request,
        const QList<ExtensionRegistryRecord> &records,
        const ExtensionEnablementLedgerStoreResult &ledger);
};

#endif // EXTENSION_ENABLEMENT_WORKFLOW_H
