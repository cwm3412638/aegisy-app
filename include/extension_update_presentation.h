#ifndef EXTENSION_UPDATE_PRESENTATION_H
#define EXTENSION_UPDATE_PRESENTATION_H

#include "extension_registry.h"
#include "extension_update_candidate_builder.h"
#include "extension_update_policy.h"

#include <QList>
#include <QString>

// 更新动作的呈现。它回答的问题与移除呈现不同:移除问"这次收回了什么",更新问"这次更新能不
// 能成立,如果不能,是缺什么"。
//
// **这一层存在的核心理由是:当前没有任何一次更新可以成立,而这件事必须被说清楚,不能被一个
// 灰掉的按钮代替。** 这个仓库里不存在扩展签名权威——更新签名密钥环签的是发布安装包,不是
// 扩展——也不存在依赖解析器或健康探针。因此每一次更新都会被判定层拒绝。一个只是灰掉按钮
// 的界面会让人以为是自己这个包有问题,于是反复重做包;而真正缺的是这台机器上根本没有装
// 签名权威。所以界面必须逐项列出哪一项没有被确立,以及那一项是"没有人能核查"还是"核查
// 失败了"。
//
// **暂存不是启用。** 即使有一天证据齐备,`StagedUnreviewed` 的含义仍然是候选可以被暂存,
// 而不是候选可以运行:候选按定义是另一份内容,因此它从未复核、未授权。界面若把"更新已暂存"
// 说成"更新已完成",人会认为新版本正在运行,而实际运行的仍然是旧版本——或者什么都没在运行。
// `stagesOnly`、`replacesActiveVersion`、`grantsExecution` 三个字段因此是显式暴露的恒定值,
// 而不是省略。
//
// **降级必须显式说出来,不能只靠版本号让人自己比。** 降级不被禁止,但它会重新引入已经被
// 修复过的内容,而两个版本号并排放着并不会让人注意到方向。
//
// **判定只有一个来源。** 这一层不重新判定更新是否成立:结论来自
// `ExtensionUpdatePolicy::evaluate`,证据来自 `ExtensionUpdateCandidateBuilder`。这里再判
// 一遍必然会与那两层漂移,而漂移的方向是界面提供一个判定层会拒绝的动作。
enum class ExtensionUpdatePlanState {
    // 还没有选择候选包。
    NoCandidate,
    // 候选可以被暂存。仍然未复核、未授权。
    Stageable,
    // 证据不齐或判定失败:当前版本保持不变,候选不执行。
    Blocked,
    // 候选或目标无法安全展示,因此连提问都不成立。
    Unpresentable,
};

// 一项证据在屏幕上的呈现。`establishedByUs` 与 `unverifiable` 不是同一件事的反面:
// 前者为假、后者为真表示没有人能核查;两者都为假表示核查过并且没通过。
struct ExtensionUpdateEvidenceLine {
    QString label;
    bool established = false;
    // 没有任何人能核查这一项。它与"核查失败"必须分开:一个把人送去装权威,一个把人送去修包。
    bool unverifiable = false;
    QString diagnostic;
};

struct ExtensionUpdatePlan {
    ExtensionUpdatePlanState state = ExtensionUpdatePlanState::Unpresentable;
    QString title;
    QString identifier;
    ExtensionKind kind = ExtensionKind::Skill;
    QString kindLabel;
    QString activeVersionLabel;
    QString candidateVersionLabel;
    // 当前内容与候选内容各自的指纹。两者必须都在场:人要能看出这确实是两份不同的内容。
    QString activeFingerprint;
    QString candidateFingerprint;
    // 候选相对当前版本是降级。必须显式说出来,不能只靠版本号让人自己比。
    bool downgrade = false;
    // 逐项证据。齐备时也全部列出:人有权看到这次更新凭什么成立。
    QList<ExtensionUpdateEvidenceLine> evidence;
    // 至少有一项没有被确立。
    bool evidenceIncomplete = true;
    // 至少有一项是"没有人能核查",因此问题不在这个包上。
    bool anyUnverifiable = false;
    // 恒为真:更新只暂存候选,不替换当前生效的版本。
    bool stagesOnly = true;
    // 恒为假:当前生效的版本不被这次操作替换。
    bool replacesActiveVersion = false;
    // 恒为假:暂存不授予执行权。候选必须重新经过人工复核并重新授权。
    bool grantsExecution = false;
    // 逐组件披露原样带出:判定用并集,展示用逐组件。
    QList<ExtensionBundleComponent> components;
    QString errorCode;
};

class ExtensionUpdatePresentation
{
public:
    static constexpr int MaxTitleCharacters = 128;

    // 还没有选择候选包时的计划。它仍然必须带上三个恒定字段:一个空计划同样不替换、不授权。
    static ExtensionUpdatePlan buildEmpty(const ExtensionRegistryRecord &active);

    static ExtensionUpdatePlan build(
        const ExtensionRegistryRecord &active,
        const ExtensionUpdateCandidateResult &candidate,
        const ExtensionUpdateVerdict &verdict);

    static QString stateLabel(ExtensionUpdatePlanState state);
};

#endif // EXTENSION_UPDATE_PRESENTATION_H
