#ifndef EXTENSION_REVIEW_WORKFLOW_H
#define EXTENSION_REVIEW_WORKFLOW_H

#include "extension_review_ledger_store.h"

// 人工复核动作被翻译成"提交后的完整复核集合"，而不是就地修改存储。这样批准与撤销
// 的全部安全属性都能在没有任何持久化的情况下判定与测试。
//
// 这一层不安装、不启用、不写盘：它只产出一份候选集合与一个用于比较并交换的代号。
// 批准某份内容之后，扩展仍然只是 Verified，启用需要另一个独立动作。
enum class ExtensionReviewAction {
    Approve,
    Revoke,
};

// 批准必须携带人工复核时屏幕上确切显示的摘要。被复核的是"我看过的这份内容"，
// 不是"这个名字"，因此渲染与批准之间发生的任何内容变化都必须让批准失败，而不是
// 把结论悄悄套用到新内容上。
struct ExtensionReviewRequest {
    ExtensionReviewAction action = ExtensionReviewAction::Approve;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QString reviewedSourceIdentity;
    QString reviewedContentIdentity;
};

enum class ExtensionReviewPlanState {
    Ready,
    Rejected,
};

struct ExtensionReviewPlan {
    ExtensionReviewPlanState state = ExtensionReviewPlanState::Rejected;
    // 提交后应当存在的完整集合，而不是增量。
    QList<ExtensionReviewPin> pins;
    // 读取时的代号，交给存储做比较并交换：并发复核因此不会互相覆盖。
    qint64 expectedGeneration = 0;
    // 集合没有变化时不应提交：提交只会白白推进一个代号并改变身份摘要。
    bool changed = false;
    QString errorCode;
};

class ExtensionReviewWorkflow
{
public:
    static ExtensionReviewPlan plan(const ExtensionReviewRequest &request,
                                    const QList<ExtensionRegistryRecord> &records,
                                    const ExtensionReviewLedgerStoreResult &ledger);
};

#endif // EXTENSION_REVIEW_WORKFLOW_H
