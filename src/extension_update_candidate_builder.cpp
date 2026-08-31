#include "extension_update_candidate_builder.h"

#include "extension_display_safety.h"

#include <algorithm>

namespace {

using Safety = ExtensionDisplaySafety;

// 每一条返回路径都必须让五项证据保持假。一个被拒绝的候选没有确立任何证据,而把这件事留给
// 结构体默认值意味着源码里没有任何一处声明它。
ExtensionUpdateCandidateResult refuse(ExtensionUpdateCandidateState state,
                                     const QString &code)
{
    ExtensionUpdateCandidateResult result;
    result.state = state;
    result.evidence = ExtensionUpdateEvidence{};
    result.evidenceComplete = false;
    result.errorCode = code;
    return result;
}

// 判定用并集:兼容性门禁必须失败关闭,因此任何一个组件请求写文件就等于这个扩展请求写文件。
// 排序去重只是为了让诊断稳定,不改变判定。
QStringList unionOfCapabilities(const ExtensionBundleManifest &manifest)
{
    QStringList all;
    for (const ExtensionBundleComponent &component : manifest.components) {
        for (const QString &capability : component.requestedCapabilities) {
            if (!all.contains(capability)) all.append(capability);
        }
    }
    std::sort(all.begin(), all.end());
    return all;
}

} // namespace

ExtensionUpdateCandidateResult ExtensionUpdateCandidateBuilder::build(
    const ExtensionRegistryRecord &active,
    const QString &candidateRoot,
    const ExtensionHostProfile &host)
{
    const ExtensionBundleReadResult read = ExtensionBundleReader::read(candidateRoot);
    // 读取失败时不构造候选。一次失败读取里的清单是垃圾,而用它算出的摘要会被绑定成一份
    // 授权的目标。
    switch (read.state) {
    case ExtensionBundleReadState::Empty:
        return refuse(ExtensionUpdateCandidateState::Absent, QString());
    case ExtensionBundleReadState::Unavailable:
        return refuse(ExtensionUpdateCandidateState::Unreadable, read.errorCode);
    case ExtensionBundleReadState::Invalid:
        return refuse(ExtensionUpdateCandidateState::Rejected, read.errorCode);
    case ExtensionBundleReadState::Ready:
        break;
    }

    // 候选必须描述同一个扩展。一个描述别的 ID 的包不是这个扩展的更新,而按名字放行会让
    // 任意内容顶替一份已经被复核过的内容。
    if (!Safety::validId(active.id) || read.manifest.id != active.id) {
        return refuse(ExtensionUpdateCandidateState::Rejected,
                      QStringLiteral("extension-update-candidate-target-mismatch"));
    }

    ExtensionUpdateCandidateResult result;
    result.state = ExtensionUpdateCandidateState::Ready;
    result.manifest = read.manifest;
    result.candidate.kind = active.kind;
    result.candidate.id = read.manifest.id;
    result.candidate.version = read.manifest.version;
    // 摘要来自磁盘上的字节。调用方传入的摘要会让它描述它并未携带的内容;清单里声明的摘要
    // 已经被读取层拒绝。
    result.candidate.sourceIdentity = read.manifest.sourceIdentity;
    result.candidate.contentIdentity = read.manifest.contentIdentity;
    result.candidate.requestedCapabilities = unionOfCapabilities(read.manifest);

    // 清单已经被读取层严格解析并通过:未知字段、重复键、不安全文本、逃逸路径都已经被拒绝。
    // 这一项因此确实被确立了。
    result.evidence.manifestValid = true;

    // 兼容性由共享的判定层给出。这一层不复制那套规则:两份副本会各自漂移,而漂移的方向是
    // 放行一个判定层会拒绝的候选。
    ExtensionRegistryRecord probe = active;
    probe.version = result.candidate.version;
    probe.sourceIdentity = result.candidate.sourceIdentity;
    probe.contentIdentity = result.candidate.contentIdentity;
    probe.requestedCapabilities = result.candidate.requestedCapabilities;
    // 兼容性判定绝不能读到信任或启用状态:候选按定义未复核、未授权。
    probe.trust = ExtensionTrustState::Unverified;
    probe.effectiveEnabled = false;
    const ExtensionCompatibilityDecision compatibility =
        ExtensionCompatibilityPolicy::evaluate(probe, host);
    result.evidence.compatible =
        compatibility.state == ExtensionCompatibilityState::Compatible;
    if (!result.evidence.compatible) {
        // 判定层自己的理由原样带出。这一层再编一个代号会让人拿着一个查不到出处的东西。
        result.gaps.compatibility = compatibility.reason.isEmpty()
            ? QStringLiteral("extension-update-incompatible")
            : compatibility.reason;
    }

    // 这三项没有任何人能核查:这个仓库里不存在扩展签名权威——更新签名密钥环签的是发布
    // 安装包,不是扩展——也不存在依赖解析器或健康探针。因此它们保持假,并且各自说明的是
    // "无法核查"而不是"核查失败":一个把人送去装签名权威,一个把人送去修包。把它们默认
    // 填真会让整个判定层变成装饰,而那不会报错,它会成功。
    result.evidence.signatureValid = false;
    result.gaps.signature = QString::fromLatin1(SignatureUnavailable);
    result.evidence.dependenciesSatisfied = false;
    result.gaps.dependencies = QString::fromLatin1(DependenciesUnavailable);
    result.evidence.healthy = false;
    result.gaps.health = QString::fromLatin1(HealthUnavailable);

    result.evidenceComplete = result.evidence.signatureValid
        && result.evidence.manifestValid
        && result.evidence.compatible
        && result.evidence.dependenciesSatisfied
        && result.evidence.healthy;
    return result;
}
