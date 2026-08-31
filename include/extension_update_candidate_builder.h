#ifndef EXTENSION_UPDATE_CANDIDATE_BUILDER_H
#define EXTENSION_UPDATE_CANDIDATE_BUILDER_H

#include "extension_bundle_reader.h"
#include "extension_compatibility_policy.h"
#include "extension_registry.h"
#include "extension_update_policy.h"

#include <QString>
#include <QStringList>

// 从一个候选包目录产出 `ExtensionUpdateCandidate` 与 `ExtensionUpdateEvidence`。
// `ExtensionUpdatePolicy::evaluate` 与 `ExtensionLifecycleController::stageUpdate` 都已
// 存在,但没有任何东西能产出它们的输入,因此"更新这个扩展"这个决定到此为止无法在产品里被
// 提出。这一层产出它们。
//
// **证据必须被确立,绝不能被假定。** `ExtensionUpdateEvidence` 的每一项都从假开始,只有
// 这一层真的核查过并通过时才变真。一个把五项默认填真的构造器会让整个判定层变成装饰:
// evaluate 会一路放行,而没有任何人真的验过签名、依赖或健康。默认填真是这一层唯一真正
// 危险的失败方式,因为它不会报错——它会成功。
//
// **"无法核查"与"核查失败"不是同一件事。** 当前这个仓库里没有任何扩展签名权威:更新签名
// 密钥环签的是发布安装包,不是扩展。所以签名不是"无效",而是没有任何人能判断;依赖与健康
// 同理。两者都让更新失败,但它们把人送去不同的地方——一个去装签名权威,一个去修包。因此
// 每一项都带上它自己为什么没有被确立的固定代码,而不是并成一句"证据不足"。
//
// **能力在这里做并集,而披露仍然逐组件。** 这与导入披露层的规则方向相反,而两者都对:
// 兼容性门禁必须失败关闭,因此任何一个组件请求写文件就等于这个扩展请求写文件,并集是唯一
// 安全的判定输入;而人做决定看的是逐组件披露,因为两个组件各自请求"读文件"与"连网"时,
// 汇总看起来与一个组件同时请求两者完全一样。判定用并集,展示用逐组件。
//
// **候选的摘要来自磁盘上的字节。** 这一层不接受调用方传入的摘要,也不读清单里声明的摘要:
// 前者让调用方可以描述它并未携带的内容,后者是读取层已经拒绝的东西。
//
// 这一层不安装、不下载、不解包、不写盘、不执行任何东西。它只把一个已经存在的目录变成一份
// 可判定的候选与一份如实的证据。
enum class ExtensionUpdateCandidateState {
    // 目录不存在。这不是错误:还没有候选可以评估。
    Absent,
    // 候选与证据都已产出。证据里可能仍有未确立的项——那会让 evaluate 拒绝,这是对的。
    Ready,
    // 候选包读不出来。与畸形区分开:要去看的是权限,不是包。
    Unreadable,
    // 候选包畸形,或者它描述的不是这个扩展。
    Rejected,
};

// 每一项证据为什么没有被确立。空字符串表示这一项确实被确立了。
struct ExtensionUpdateEvidenceGaps {
    QString signature;
    QString manifest;
    QString compatibility;
    QString dependencies;
    QString health;
};

struct ExtensionUpdateCandidateResult {
    ExtensionUpdateCandidateState state = ExtensionUpdateCandidateState::Rejected;
    ExtensionUpdateCandidate candidate;
    ExtensionUpdateEvidence evidence;
    ExtensionUpdateEvidenceGaps gaps;
    // 逐组件披露用的读取结果,原样带出:判定用并集,展示用逐组件。
    ExtensionBundleManifest manifest;
    // 至少有一项证据没有被确立,因此这次更新一定会被判定层拒绝。这不是这一层的结论,
    // 而是对证据本身的陈述。
    bool evidenceComplete = false;
    QString errorCode;
};

class ExtensionUpdateCandidateBuilder
{
public:
    // active 是当前生效的记录:候选必须描述同一个 (kind, id),否则它不是这个扩展的更新。
    static ExtensionUpdateCandidateResult build(
        const ExtensionRegistryRecord &active,
        const QString &candidateRoot,
        const ExtensionHostProfile &host);

    // 这些代码陈述"没有任何人能核查这一项",而不是"这一项核查失败了"。
    static constexpr const char *SignatureUnavailable =
        "extension-update-signature-authority-absent";
    static constexpr const char *DependenciesUnavailable =
        "extension-update-dependency-resolver-absent";
    static constexpr const char *HealthUnavailable =
        "extension-update-health-probe-absent";
};

#endif // EXTENSION_UPDATE_CANDIDATE_BUILDER_H
