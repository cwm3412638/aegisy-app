#include "extension_update_presentation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

QByteArray manifestBytes(const QByteArray &capabilities,
                         const QByteArray &version = QByteArrayLiteral("2.0.0"))
{
    return "{\"id\":\"acme.skill\",\"name\":\"Acme Skill\",\"version\":\""
        + version + "\",\"components\":"
        "[{\"id\":\"acme.one\",\"name\":\"One\",\"type\":\"skill\","
        "\"path\":\"skills/one\",\"capabilities\":" + capabilities + "},"
        "{\"id\":\"acme.two\",\"name\":\"Two\",\"type\":\"asset\","
        "\"path\":\"assets/two\",\"capabilities\":[]}]}";
}

bool buildBundle(const QString &root, const QByteArray &manifest)
{
    return writeFile(root + QStringLiteral("/aegisy-bundle.json"), manifest)
        && writeFile(root + QStringLiteral("/skills/one/SKILL.md"),
                     QByteArrayLiteral("# one\n"))
        && writeFile(root + QStringLiteral("/assets/two/readme.txt"),
                     QByteArrayLiteral("two\n"));
}

ExtensionRegistryRecord activeRecord()
{
    ExtensionRegistryRecord value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.skill");
    value.name = QStringLiteral("Acme Skill");
    value.version = QStringLiteral("1.0.0");
    value.sourceKind = ExtensionSourceKind::LocalDirectory;
    value.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('a'));
    value.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('a'));
    value.scope = QStringLiteral("user");
    value.installed = true;
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    value.requestedCapabilities = {QStringLiteral("skill-content")};
    return value;
}

ExtensionHostProfile host()
{
    ExtensionHostProfile profile;
    profile.codexVersion = QStringLiteral("1.0.0");
    profile.grantedCapabilities =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();
    return profile;
}

ExtensionUpdatePlan planFor(const QString &root,
                            const ExtensionRegistryRecord &active)
{
    const ExtensionUpdateCandidateResult candidate =
        ExtensionUpdateCandidateBuilder::build(active, root, host());
    const ExtensionUpdateVerdict verdict = ExtensionUpdatePolicy::evaluate(
        active, candidate.candidate, candidate.evidence);
    return ExtensionUpdatePresentation::build(active, candidate, verdict);
}

// 当前没有任何一次更新可以成立，而这件事必须被说清楚，不能被一个灰掉的按钮代替：一个只是
// 灰掉按钮的界面会让人以为是自己这个包有问题，于是反复重做包，而真正缺的是这台机器上根本
// 没有装签名权威。
void testBlockedUpdateNamesWhatIsMissing()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes("[\"skill-content\"]")),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdatePlan plan = planFor(root, activeRecord());
    expect(plan.state == ExtensionUpdatePlanState::Blocked,
           "an update with no signing authority was offered as stageable");
    expect(plan.evidenceIncomplete,
           "an update missing evidence is reported as fully evidenced");
    // 问题不在这个包上，屏幕必须能表达这一点。
    expect(plan.anyUnverifiable,
           "the plan does not say some evidence is beyond anyone's reach here");
    if (!expect(plan.evidence.size() == 5,
                "the plan does not list every evidence item")) {
        return;
    }
    // 每一项各自可分辨：哪一项没被确立，以及它是无法核查还是核查失败。
    int unverifiable = 0;
    int established = 0;
    for (const ExtensionUpdateEvidenceLine &item : plan.evidence) {
        expect(!item.label.isEmpty(), "an evidence line has no label");
        if (item.unverifiable) {
            ++unverifiable;
            expect(!item.established,
                   "an item nobody can verify is reported as established");
            expect(!item.diagnostic.isEmpty(),
                   "an unverifiable item carries no diagnostic");
        }
        if (item.established) {
            ++established;
            // 确立了还带诊断会让人去查一个不存在的问题。
            expect(item.diagnostic.isEmpty(),
                   "an established evidence item still carries a diagnostic");
        }
    }
    expect(unverifiable == 3,
           "the three items nobody can check are not all marked unverifiable");
    // 清单与兼容性这两项确实被确立了：只读能力加完整宿主证据。
    expect(established == 2,
           "the established evidence items are miscounted");
}

// "没有人能核查"与"核查失败"必须分开：一个把人送去装签名权威，一个把人送去修包。
void testUnverifiableIsNotFailedCheck()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    // 候选请求写文件：兼容性这一项被真的核查过，并且没通过。
    if (!expect(buildBundle(root, manifestBytes("[\"filesystem-write\"]")),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdatePlan plan = planFor(root, activeRecord());
    if (!expect(plan.evidence.size() == 5,
                "the plan does not list every evidence item")) {
        return;
    }
    bool sawFailedCheck = false;
    bool sawUnverifiable = false;
    for (const ExtensionUpdateEvidenceLine &item : plan.evidence) {
        if (item.label == QStringLiteral("兼容性")) {
            sawFailedCheck = true;
            expect(!item.established,
                   "a candidate requesting writes was called compatible");
            // 这一项是核查失败，不是无法核查：人应该去修这个包。
            expect(!item.unverifiable,
                   "a failed compatibility check is reported as unverifiable");
            expect(!item.diagnostic.isEmpty(),
                   "a failed compatibility check carries no reason");
        }
        if (item.label == QStringLiteral("签名")) {
            sawUnverifiable = true;
            expect(item.unverifiable,
                   "an absent signing authority is reported as a failed check");
        }
    }
    expect(sawFailedCheck && sawUnverifiable,
           "the plan does not distinguish a failed check from an absent authority");
}

// 暂存不是启用。界面若把"更新已暂存"说成"更新已完成"，人会认为新版本正在运行，而实际运行的
// 仍然是旧版本——或者什么都没在运行。
void testStagingIsNotEnabling()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes("[\"skill-content\"]")),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult candidate =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    // 用一份人工补齐的证据走到判定层放行的那条路径：这是这一层唯一会说"可以暂存"的情况，
    // 而即便那时它也绝不能说候选可以运行。
    ExtensionUpdateEvidence complete;
    complete.signatureValid = true;
    complete.manifestValid = true;
    complete.compatible = true;
    complete.dependenciesSatisfied = true;
    complete.healthy = true;
    const ExtensionUpdateVerdict verdict = ExtensionUpdatePolicy::evaluate(
        activeRecord(), candidate.candidate, complete);
    if (!expect(verdict.state == ExtensionUpdateState::StagedUnreviewed,
                "the fixture does not reach the staged verdict, so the test is empty")) {
        return;
    }
    ExtensionUpdateCandidateResult evidenced = candidate;
    evidenced.evidence = complete;
    // 故意留下一条过期的缺口诊断：一项已经确立的证据绝不能同时带着一句"这里缺什么"，那会把
    // 人送去查一个不存在的问题。
    evidenced.gaps = ExtensionUpdateEvidenceGaps{};
    evidenced.gaps.manifest = QStringLiteral("extension-update-stale-diagnostic");
    evidenced.evidenceComplete = true;
    const ExtensionUpdatePlan plan =
        ExtensionUpdatePresentation::build(activeRecord(), evidenced, verdict);
    expect(plan.state == ExtensionUpdatePlanState::Stageable,
           "a fully evidenced candidate is not stageable");
    expect(!plan.evidenceIncomplete,
           "a fully evidenced candidate is reported as missing evidence");
    expect(!plan.anyUnverifiable,
           "a fully evidenced candidate still claims something is unverifiable");
    // 这三个字段是这一层存在的理由的一半。
    for (const ExtensionUpdateEvidenceLine &item : plan.evidence) {
        expect(item.established, "a fully evidenced plan has an unestablished line");
        expect(item.diagnostic.isEmpty(),
               "an established evidence item still carries a diagnostic");
    }
    expect(plan.stagesOnly, "a stageable plan does not say it only stages");
    expect(!plan.replacesActiveVersion,
           "a stageable plan claims it replaces the active version");
    expect(!plan.grantsExecution,
           "a stageable plan claims it grants execution");
    // 屏幕文本自己也必须说清楚。
    expect(ExtensionUpdatePresentation::stateLabel(plan.state)
               .contains(QStringLiteral("不会让它运行")),
           "the stageable label does not say staging never runs the candidate");
}

// 每一条被拒绝的路径同样不替换当前版本、同样不授予执行权。把它们留给结构体默认值意味着
// 源码里没有任何一处声明这件事。
void testEveryPathClaimsNothing()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;

    // 标识不合法：连提问都不成立。
    ExtensionRegistryRecord broken = activeRecord();
    broken.id = QStringLiteral("../escape");
    const ExtensionUpdatePlan unpresentable =
        ExtensionUpdatePresentation::build(broken, ExtensionUpdateCandidateResult{},
                                          ExtensionUpdateVerdict{});
    expect(unpresentable.state == ExtensionUpdatePlanState::Unpresentable,
           "an unpresentable target produced a usable plan");
    expect(unpresentable.errorCode
               == QStringLiteral("extension-update-plan-id-invalid"),
           "an unpresentable target carries no diagnostic");

    // 还没有候选包。走完整入口进来同样必须落到"还没有候选"，而不是落到"这次更新不能进行"：
    // 后者会让人以为自己已经选过一个包并且那个包被拒了。
    ExtensionUpdateCandidateResult absent;
    absent.state = ExtensionUpdateCandidateState::Absent;
    const ExtensionUpdatePlan empty = ExtensionUpdatePresentation::build(
        activeRecord(), absent, ExtensionUpdateVerdict{});
    expect(empty.state == ExtensionUpdatePlanState::NoCandidate,
           "an absent candidate is not reported as absent");
    expect(empty.evidence.isEmpty(),
           "an absent candidate produced an evidence table out of nothing");
    expect(empty.components.isEmpty(),
           "an absent candidate disclosed components");

    // 缺省的候选结果是 Rejected 而不是 Absent：一份还没有被填过的结果绝不能被当成"还没有
    // 选包"，那会让一次失败的产出在屏幕上变成一次没发生过的操作。
    expect(ExtensionUpdateCandidateResult{}.state
               == ExtensionUpdateCandidateState::Rejected,
           "an unfilled candidate result defaults to something other than rejected");

    // 候选读不出来。
    ExtensionUpdateCandidateResult unreadable;
    unreadable.state = ExtensionUpdateCandidateState::Unreadable;
    unreadable.errorCode = QStringLiteral("extension-bundle-manifest-unreadable");
    const ExtensionUpdatePlan blocked = ExtensionUpdatePresentation::build(
        activeRecord(), unreadable, ExtensionUpdateVerdict{});
    expect(blocked.state == ExtensionUpdatePlanState::Blocked,
           "an unreadable candidate is not blocked");
    // 产出层自己的诊断原样带出。
    expect(blocked.errorCode
               == QStringLiteral("extension-bundle-manifest-unreadable"),
           "the candidate diagnostic was replaced by a locally invented one");
    expect(blocked.evidence.isEmpty(),
           "an unread candidate produced an evidence table describing nothing");

    for (const ExtensionUpdatePlan &plan : {unpresentable, empty, blocked}) {
        expect(plan.stagesOnly, "a plan does not say it only stages");
        expect(!plan.replacesActiveVersion,
               "a plan claims it replaces the active version");
        expect(!plan.grantsExecution, "a plan claims it grants execution");
        expect(plan.evidenceIncomplete,
               "a plan with no evidence table reports complete evidence");
    }
}

// 降级必须显式说出来：两个版本号并排放着不会让人注意到方向，而降级会重新引入已经被修复过的
// 内容。结论来自判定层，这一层只是把它带出来。
void testDowngradeIsStated()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes("[\"skill-content\"]",
                                                QByteArrayLiteral("0.9.0"))),
                "candidate fixture could not be written")) {
        return;
    }
    ExtensionRegistryRecord newer = activeRecord();
    newer.version = QStringLiteral("1.5.0");
    const ExtensionUpdateCandidateResult candidate =
        ExtensionUpdateCandidateBuilder::build(newer, root, host());
    ExtensionUpdateEvidence complete;
    complete.signatureValid = true;
    complete.manifestValid = true;
    complete.compatible = true;
    complete.dependenciesSatisfied = true;
    complete.healthy = true;
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(newer, candidate.candidate, complete);
    if (!expect(verdict.downgrade,
                "the fixture is not a downgrade, so the test proves nothing")) {
        return;
    }
    const ExtensionUpdatePlan plan =
        ExtensionUpdatePresentation::build(newer, candidate, verdict);
    expect(plan.downgrade, "a downgrade is not stated on the plan");
    // 两个版本号都必须在场：人要能看出方向。
    expect(plan.activeVersionLabel == QStringLiteral("1.5.0")
               && plan.candidateVersionLabel == QStringLiteral("0.9.0"),
           "the plan does not show both versions");
    // 两份内容的指纹都在场：人要能看出这确实是两份不同的内容。
    expect(!plan.activeFingerprint.isEmpty()
               && !plan.candidateFingerprint.isEmpty()
               && plan.activeFingerprint != plan.candidateFingerprint,
           "the plan does not show that these are two different contents");
}

// 逐组件披露原样带出：判定用并集，展示用逐组件。汇总会让两个组件各自请求"读文件"与"连网"
// 看起来与一个组件同时请求两者完全一样，而后者才是真正危险的组合。
void testComponentsStayPerComponent()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes("[\"filesystem-write\"]")),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdatePlan plan = planFor(root, activeRecord());
    if (!expect(plan.components.size() == 2,
                "the per-component disclosure was lost")) {
        return;
    }
    bool sawWriter = false;
    bool sawQuiet = false;
    for (const ExtensionBundleComponent &component : plan.components) {
        if (component.id == QStringLiteral("acme.one")) {
            sawWriter = true;
            expect(component.requestedCapabilities
                       == QStringList{QStringLiteral("filesystem-write")},
                   "a component was attributed the wrong capability");
        }
        if (component.id == QStringLiteral("acme.two")) {
            sawQuiet = true;
            // 这个组件什么都没请求。汇总会让它看起来也在请求写文件。
            expect(component.requestedCapabilities.isEmpty(),
                   "a component that requested nothing was attributed a capability");
        }
    }
    expect(sawWriter && sawQuiet,
           "the per-component disclosure lost one of the components");
}

// 名称来自不可信来源。不可展示时退回使用标识，而不是拒绝更新：否则一个扩展可以靠取一个
// 恶意名字让自己永远无法被更新。
void testUntrustedTextFallsBack()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ExtensionRegistryRecord hostile = activeRecord();
    hostile.name = QStringLiteral("Acme‮Skill");
    hostile.version = QStringLiteral("1.0\n0");
    const ExtensionUpdatePlan plan =
        ExtensionUpdatePresentation::buildEmpty(hostile);
    expect(plan.state == ExtensionUpdatePlanState::NoCandidate,
           "a hostile name made the extension unupdatable");
    expect(plan.title == hostile.id,
           "an unpresentable name did not fall back to the identifier");
    expect(plan.activeVersionLabel.isEmpty(),
           "an unpresentable version reached the plan verbatim");
}

// 每一个状态都有自己的文本。两个状态共用一句话就等于在屏幕上把它们并成一个。
void testEveryStateIsDistinct()
{
    const ExtensionUpdatePlanState states[] = {
        ExtensionUpdatePlanState::NoCandidate,
        ExtensionUpdatePlanState::Stageable,
        ExtensionUpdatePlanState::Blocked,
        ExtensionUpdatePlanState::Unpresentable,
    };
    QStringList seen;
    for (const ExtensionUpdatePlanState state : states) {
        const QString label = ExtensionUpdatePresentation::stateLabel(state);
        expect(!label.isEmpty(), "an update plan state has no label");
        expect(!seen.contains(label), "two update plan states share one label");
        seen.append(label);
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testBlockedUpdateNamesWhatIsMissing();
    testUnverifiableIsNotFailedCheck();
    testStagingIsNotEnabling();
    testEveryPathClaimsNothing();
    testDowngradeIsStated();
    testComponentsStayPerComponent();
    testUntrustedTextFallsBack();
    testEveryStateIsDistinct();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " extension update presentation guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "extension update presentation guards passed\n";
    return 0;
}
