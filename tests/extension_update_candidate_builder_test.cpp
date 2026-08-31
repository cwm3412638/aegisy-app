#include "extension_update_candidate_builder.h"

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

QByteArray manifestBytes(const QByteArray &components,
                         const QByteArray &id = QByteArrayLiteral("acme.skill"),
                         const QByteArray &version = QByteArrayLiteral("2.0.0"))
{
    return "{\"id\":\"" + id + "\",\"name\":\"Acme Skill\",\"version\":\""
        + version + "\",\"components\":" + components + "}";
}

// 两个组件各自请求一项只读能力。
const QByteArray kReadOnlyComponents =
    "[{\"id\":\"acme.one\",\"name\":\"One\",\"type\":\"skill\","
    "\"path\":\"skills/one\",\"capabilities\":[\"skill-content\"]},"
    "{\"id\":\"acme.two\",\"name\":\"Two\",\"type\":\"asset\","
    "\"path\":\"assets/two\",\"capabilities\":[]}]";

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

// 证据必须被确立，绝不能被假定。默认填真是这一层唯一真正危险的失败方式，因为它不会报错——
// 它会成功：判定层会一路放行，而没有任何人真的验过签名、依赖或健康。
void testEvidenceIsEstablishedNotAssumed()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents)),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    if (!expect(result.state == ExtensionUpdateCandidateState::Ready,
                "a well-formed candidate directory was not read")) {
        return;
    }
    // 清单确实被严格解析过，这一项是真的被确立了。
    expect(result.evidence.manifestValid,
           "a strictly parsed manifest is not counted as established");
    // 这三项没有任何人能核查，因此它们必须保持假。
    expect(!result.evidence.signatureValid,
           "a signature nobody verified is reported as valid");
    expect(!result.evidence.dependenciesSatisfied,
           "dependencies nobody resolved are reported as satisfied");
    expect(!result.evidence.healthy,
           "health nobody probed is reported as healthy");
    expect(!result.evidenceComplete,
           "incomplete evidence is reported as complete");

    // 而且这次更新确实会被判定层拒绝。这条断言是这一层存在的意义:它不能自己把门打开。
    const ExtensionUpdateVerdict verdict = ExtensionUpdatePolicy::evaluate(
        activeRecord(), result.candidate, result.evidence);
    expect(verdict.state == ExtensionUpdateState::Rejected,
           "a candidate with unverifiable evidence was staged anyway");
    expect(!verdict.candidateExecutable && !verdict.inheritsTrust
               && !verdict.inheritsGrant,
           "a rejected candidate inherited authority");
    expect(verdict.activePreserved,
           "a rejected candidate did not leave the active version intact");
}

// "无法核查"与"核查失败"不是同一件事：一个把人送去装签名权威，一个把人送去修包。并成一句
// "证据不足"会让人无从判断该去哪里。
void testUnverifiableIsNotFailed()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents)),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    if (!expect(result.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    // 每一项各自说明它自己为什么没有被确立。
    expect(result.gaps.signature
               == QString::fromLatin1(
                      ExtensionUpdateCandidateBuilder::SignatureUnavailable),
           "the signature gap does not say nobody can verify it");
    expect(result.gaps.dependencies
               == QString::fromLatin1(
                      ExtensionUpdateCandidateBuilder::DependenciesUnavailable),
           "the dependency gap does not say nobody can resolve them");
    expect(result.gaps.health
               == QString::fromLatin1(
                      ExtensionUpdateCandidateBuilder::HealthUnavailable),
           "the health gap does not say nobody can probe it");
    // 三个代号必须互不相同，否则屏幕上分不出该去装哪个东西。
    expect(result.gaps.signature != result.gaps.dependencies
               && result.gaps.dependencies != result.gaps.health
               && result.gaps.signature != result.gaps.health,
           "two unverifiable evidence items share one diagnostic");
    // 已经确立的项不带缺口。
    expect(result.gaps.manifest.isEmpty(),
           "an established manifest still carries a gap");
}

// 判定用并集：兼容性门禁必须失败关闭，因此任何一个组件请求写文件就等于这个扩展请求写文件。
// 而披露仍然逐组件保留，因为人做决定看的是逐组件披露。
void testCapabilitiesUniteForTheGateAndStayPerComponentForThePerson()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    const QByteArray components =
        "[{\"id\":\"acme.one\",\"name\":\"One\",\"type\":\"skill\","
        "\"path\":\"skills/one\",\"capabilities\":[\"filesystem-write\"]},"
        "{\"id\":\"acme.two\",\"name\":\"Two\",\"type\":\"command\","
        "\"path\":\"assets/two\",\"capabilities\":[\"command-execution\"]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    if (!expect(result.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    // 判定输入是并集：两个组件各自请求的能力都必须出现，否则门禁看不到其中一个。
    expect(result.candidate.requestedCapabilities.contains(
               QStringLiteral("filesystem-write"))
               && result.candidate.requestedCapabilities.contains(
                      QStringLiteral("command-execution")),
           "the compatibility gate does not see every component's request");
    // 当前只读授权下这两项都没有被授予，因此候选确定不兼容。
    expect(!result.evidence.compatible,
           "a candidate requesting writes and execution is called compatible");
    expect(!result.gaps.compatibility.isEmpty(),
           "an incompatible candidate carries no reason");
    // 披露仍然逐组件：汇总看起来与一个组件同时请求两者完全一样，而后者才是危险的组合。
    if (!expect(result.manifest.components.size() == 2,
                "the per-component disclosure was lost")) {
        return;
    }
    for (const ExtensionBundleComponent &component : result.manifest.components) {
        expect(component.requestedCapabilities.size() == 1,
               "capabilities were merged into the per-component disclosure");
    }
}

// 候选必须描述同一个扩展。按名字放行会让任意内容顶替一份已经被复核过的内容。
void testCandidateMustDescribeTheSameExtension()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents,
                                                QByteArrayLiteral("other.skill"))),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    expect(result.state == ExtensionUpdateCandidateState::Rejected,
           "a candidate describing another extension was accepted");
    expect(result.errorCode
               == QStringLiteral("extension-update-candidate-target-mismatch"),
           "a mismatched candidate carries no diagnostic");
    expect(!result.evidence.manifestValid && !result.evidenceComplete,
           "a rejected candidate still established evidence");
}

// 摘要来自磁盘上的字节，因此改一个组件的内容就会改变候选身份。绑定的正是这个值。
void testIdentityComesFromDisk()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents)),
                "candidate fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult first =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    if (!expect(first.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    expect(first.candidate.contentIdentity.startsWith(
               QStringLiteral("extension-content:sha256:")),
           "the candidate content identity is not canonical");
    expect(first.candidate.sourceIdentity.startsWith(
               QStringLiteral("extension-source:sha256:")),
           "the candidate source identity is not canonical");
    // 候选内容与当前内容不同，否则这不是一次更新——判定层正是这样拒绝它的。
    expect(first.candidate.contentIdentity != activeRecord().contentIdentity,
           "the candidate carries the active version's content identity");
    if (!expect(writeFile(root + QStringLiteral("/skills/one/SKILL.md"),
                          QByteArrayLiteral("# one tampered\n")),
                "the tampered component could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult second =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    expect(second.candidate.contentIdentity != first.candidate.contentIdentity,
           "changing the candidate's bytes left its identity unchanged");
}

// 读取失败时不构造候选：一次失败读取里的清单是垃圾，而用它算出的摘要会被绑定成一份授权
// 的目标。目录不存在同样必须与畸形区分开。
void testFailedReadNeverProducesACandidate()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;

    const ExtensionUpdateCandidateResult absent =
        ExtensionUpdateCandidateBuilder::build(
            activeRecord(), temporary.path() + QStringLiteral("/never"), host());
    expect(absent.state == ExtensionUpdateCandidateState::Absent,
           "an absent candidate directory is reported as a failure");
    expect(absent.errorCode.isEmpty(),
           "an absent candidate carries a diagnostic as if something failed");
    expect(absent.candidate.contentIdentity.isEmpty(),
           "an absent candidate produced an identity");
    expect(!absent.evidence.manifestValid,
           "an absent candidate established manifest evidence");

    // 畸形的包：清单声明了一个未知字段。
    const QString malformed = temporary.path() + QStringLiteral("/malformed");
    const QByteArray declared =
        "{\"id\":\"acme.skill\",\"name\":\"Acme Skill\",\"version\":\"2.0.0\","
        "\"contentIdentity\":\"extension-content:sha256:"
        + QByteArray(64, 'f') + "\",\"components\":" + kReadOnlyComponents + "}";
    if (!expect(buildBundle(malformed, declared),
                "malformed fixture could not be written")) {
        return;
    }
    const ExtensionUpdateCandidateResult rejected =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), malformed, host());
    expect(rejected.state == ExtensionUpdateCandidateState::Rejected,
           "a malformed candidate was accepted");
    expect(rejected.state != absent.state,
           "an absent candidate is indistinguishable from a malformed one");
    // 读取层自己的诊断原样带出。落到"目标不匹配"上会把人送去查一个身份问题，而真正的问题
    // 是这个包畸形——一个畸形的包连身份都还没有被算出来。
    expect(rejected.errorCode
               == QStringLiteral("extension-bundle-manifest-fields-invalid"),
           "a malformed candidate reports something other than the read diagnostic");
    expect(rejected.candidate.contentIdentity.isEmpty(),
           "a malformed candidate produced an identity");
    expect(!rejected.evidenceComplete,
           "a malformed candidate reported complete evidence");
}

// 兼容性结论只有一个来源。这一层自己再判一遍必然会与判定层漂移，而漂移的方向是放行一个
// 判定层会拒绝的候选。
void testCompatibilityComesFromTheSharedPolicy()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents)),
                "candidate fixture could not be written")) {
        return;
    }
    // 宿主版本证据缺失时，判定层只能得出"未知"，而未知不是兼容。
    ExtensionHostProfile blind;
    blind.grantedCapabilities =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();
    ExtensionRegistryRecord codexActive = activeRecord();
    codexActive.kind = ExtensionKind::CodexPlugin;
    codexActive.sourceKind = ExtensionSourceKind::CodexCli;
    const ExtensionUpdateCandidateResult unknown =
        ExtensionUpdateCandidateBuilder::build(codexActive, root, blind);
    if (!expect(unknown.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    expect(!unknown.evidence.compatible,
           "an unknown compatibility verdict was counted as compatible");
    expect(!unknown.gaps.compatibility.isEmpty(),
           "an unknown compatibility verdict carries no reason");

    // 只读能力加完整宿主证据时，判定层给出兼容，这一层原样采用。
    const ExtensionUpdateCandidateResult compatible =
        ExtensionUpdateCandidateBuilder::build(activeRecord(), root, host());
    expect(compatible.evidence.compatible,
           "a compatible candidate was not counted as compatible");
    expect(compatible.gaps.compatibility.isEmpty(),
           "a compatible candidate still carries a compatibility gap");
    // 但它仍然不完整：签名、依赖与健康都没有任何人能核查。
    expect(!compatible.evidenceComplete,
           "a compatible candidate is treated as fully evidenced");
}

// 候选按定义未复核、未授权。兼容性判定绝不能读到当前版本的信任或启用状态：读到了就等于让
// 上一版的权威决定候选的结论。
void testCandidateNeverInheritsTheActiveVerdict()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    const QByteArray components =
        "[{\"id\":\"acme.one\",\"name\":\"One\",\"type\":\"skill\","
        "\"path\":\"skills/one\",\"capabilities\":[\"filesystem-write\"]},"
        "{\"id\":\"acme.two\",\"name\":\"Two\",\"type\":\"asset\","
        "\"path\":\"assets/two\",\"capabilities\":[]}]";
    if (!expect(buildBundle(root, manifestBytes(components)),
                "candidate fixture could not be written")) {
        return;
    }
    // 当前版本已被复核、已兼容、已生效。候选请求写文件，因此它必须不兼容——当前版本的
    // 结论一点都不能传递过去。
    ExtensionRegistryRecord trusted = activeRecord();
    trusted.effectiveEnabled = true;
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(trusted, root, host());
    if (!expect(result.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    expect(!result.evidence.compatible,
           "the active version's compatibility verdict carried over to the candidate");
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(trusted, result.candidate, result.evidence);
    expect(verdict.state == ExtensionUpdateState::Rejected,
           "a candidate requesting writes was staged under the active version's trust");
    expect(!verdict.inheritsTrust && !verdict.inheritsGrant,
           "the candidate inherited trust or a grant from the active version");
}

// 降级必须可见：它会重新引入已经被修复过的内容。这一层只负责如实报出候选版本号，结论仍然
// 由判定层给出。
void testDowngradeStaysVisible()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/candidate");
    if (!expect(buildBundle(root, manifestBytes(kReadOnlyComponents,
                                                QByteArrayLiteral("acme.skill"),
                                                QByteArrayLiteral("0.9.0"))),
                "candidate fixture could not be written")) {
        return;
    }
    ExtensionRegistryRecord newer = activeRecord();
    newer.version = QStringLiteral("1.5.0");
    const ExtensionUpdateCandidateResult result =
        ExtensionUpdateCandidateBuilder::build(newer, root, host());
    if (!expect(result.state == ExtensionUpdateCandidateState::Ready,
                "the candidate was not read")) {
        return;
    }
    expect(result.candidate.version == QStringLiteral("0.9.0"),
           "the candidate version was not reported as declared");
    // 证据齐备时判定层会把它标成降级。这里用一份人工补齐的证据只是为了证明版本号传下去了。
    ExtensionUpdateEvidence complete;
    complete.signatureValid = true;
    complete.manifestValid = true;
    complete.compatible = true;
    complete.dependenciesSatisfied = true;
    complete.healthy = true;
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(newer, result.candidate, complete);
    expect(verdict.downgrade,
           "a downgrade candidate is not visible as a downgrade");
    expect(verdict.state == ExtensionUpdateState::StagedUnreviewed,
           "a fully evidenced downgrade was rejected outright");
    // 即使被暂存，它仍然未复核、未授权。
    expect(!verdict.candidateExecutable && !verdict.inheritsTrust
               && !verdict.inheritsGrant,
           "a staged candidate became executable or inherited authority");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testEvidenceIsEstablishedNotAssumed();
    testUnverifiableIsNotFailed();
    testCapabilitiesUniteForTheGateAndStayPerComponentForThePerson();
    testCandidateMustDescribeTheSameExtension();
    testIdentityComesFromDisk();
    testFailedReadNeverProducesACandidate();
    testCompatibilityComesFromTheSharedPolicy();
    testCandidateNeverInheritsTheActiveVerdict();
    testDowngradeStaysVisible();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " extension update candidate guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "extension update candidate guards passed\n";
    return 0;
}
