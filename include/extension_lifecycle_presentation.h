#ifndef EXTENSION_LIFECYCLE_PRESENTATION_H
#define EXTENSION_LIFECYCLE_PRESENTATION_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 移除动作的呈现。它回答的问题与复核呈现、启用呈现都不同：那两个问的是"能不能问"，
// 这个问的是"这次移除到底收回了什么"。
//
// 这一层存在的核心理由是一句必须说清楚的话：**移除不删除任何文件**。
// `ExtensionLifecycleController::remove` 写两份账本、收回启用授权与复核记录，此外一个
// 字节都不动。界面如果写"删除扩展"，人会认为磁盘上那份内容已经消失，于是停止清理——而
// 内容还在原处，只要重新被复核和授权就会重新可用。因此呈现必须区分"授权已收回，因此它
// 现在不被允许运行"与"内容已从磁盘删除"，并明确指出后者没有发生。
//
// 移除没有门禁。内容漂移、复核被撤回、来源已消失的扩展都必须仍然可以移除，否则一个被
// 篡改的扩展将永远无法被收回授权。名称来自不可信来源，因此不可展示时退回使用标识本身，
// 而不是拒绝移除。
//
// 这一层只做呈现。它不写账本、不删除、不执行任何东西，移除本身的可判定性仍然来自
// `ExtensionUpdatePolicy::evaluateRemoval`，因此"这次移除是否成立"只有一个来源。
enum class ExtensionRemovalPlanState {
    Ready,
    // 标识不合法或目标与记录不一致：这时连提问都不成立。
    Unpresentable,
};

struct ExtensionRemovalPlan {
    ExtensionRemovalPlanState state = ExtensionRemovalPlanState::Unpresentable;
    QString title;
    QString identifier;
    ExtensionKind kind = ExtensionKind::Skill;
    QString kindLabel;
    // 来源已消失时为空。
    QString sourceIdentity;
    QString contentIdentity;
    // 来源已经不在清单里：移除仍然进行，但界面必须说明目标已不存在。
    bool targetAbsent = false;
    // 这次移除实际会收回哪几半。两者都没有时移除没有任何可收回的东西。
    bool withdrawsGrant = false;
    bool withdrawsReview = false;
    // 恒为假：这一层与它下面的控制器都不删除磁盘上的内容。必须作为字段暴露，界面才不能
    // 悄悄把"授权已收回"说成"内容已删除"。
    bool removesSourceContent = false;
    // 恒为真：不可变身份被保留。抹掉它会让"这份内容曾被授权运行过"的历史一并消失。
    bool retainsIdentity = true;
    QString retainedIdentity;
    QString errorCode;
};

class ExtensionLifecyclePresentation
{
public:
    static constexpr int MaxTitleCharacters = 128;

    // record 为空指针表示来源已经消失。hasReviewPin / hasGrant 描述两份账本里当前确实
    // 存在的记录，因此界面能够说明这次移除收回的是哪几半，而不是笼统地说"移除"。
    static ExtensionRemovalPlan buildRemoval(
        ExtensionKind kind, const QString &id,
        const ExtensionRegistryRecord *record,
        bool hasReviewPin, bool hasGrant);
};

#endif // EXTENSION_LIFECYCLE_PRESENTATION_H
