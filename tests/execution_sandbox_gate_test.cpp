#include "execution_sandbox_gate.h"

#include <QCoreApplication>
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

// 三个维度都已强制、门禁报告已签署的假设证据。它在当前产品里并不存在——正因为如此,
// 它必须由测试构造,而不是由产品常量提供。
SandboxEnforcementEvidence enforced()
{
    SandboxEnforcementEvidence value;
    value.platform = SandboxPlatform::MacOS;
    value.filesystem = SandboxEnforcement::Enforced;
    value.process = SandboxEnforcement::Enforced;
    value.network = SandboxEnforcement::Enforced;
    value.escapeRegressionOpen = false;
    value.releaseGateSigned = true;
    return value;
}

// 当前产品的真实证据:没有任何平台交付过被验证的强制。
void currentEvidenceTests()
{
    const SandboxEnforcementEvidence evidence =
        ExecutionSandboxGate::currentEvidence();
    // 关键:产品不得自称已强制。三个维度都必须是未验证。
    expect(evidence.filesystem == SandboxEnforcement::Unverified,
           "the product claims verified filesystem enforcement");
    expect(evidence.process == SandboxEnforcement::Unverified,
           "the product claims verified process enforcement");
    expect(evidence.network == SandboxEnforcement::Unverified,
           "the product claims verified network enforcement");
    expect(!evidence.releaseGateSigned,
           "the product claims a signed write-capable release gate");

    // 因此当前构建的结论必须是只读,并且完全访问不可提供。
    const SandboxVerdict verdict = ExecutionSandboxGate::evaluate(evidence);
    expect(verdict.state == SandboxVerdictState::ReadOnlyFallback,
           "the current build does not fall back to read-only");
    expect(verdict.authority == SandboxExecutionAuthority::ReadOnly,
           "the current build grants execution authority beyond read-only");
    expect(!ExecutionSandboxGate::beyondReadOnly(verdict.authority),
           "the current authority reaches beyond read-only");
    expect(!verdict.fullAccessOfferable,
           "full access is offerable without a signed release gate");
}

// 每一个强制维度都必须独立成立:只挡住一部分等于让越界换一条路径发生。
void dimensionTests()
{
    for (int index = 0; index < 3; ++index) {
        SandboxEnforcementEvidence partial = enforced();
        const char *label = "";
        switch (index) {
        case 0:
            partial.filesystem = SandboxEnforcement::Unverified;
            label = "sandbox-filesystem-unverified";
            break;
        case 1:
            partial.process = SandboxEnforcement::Unverified;
            label = "sandbox-process-unverified";
            break;
        default:
            partial.network = SandboxEnforcement::Unverified;
            label = "sandbox-network-unverified";
            break;
        }
        const SandboxVerdict verdict = ExecutionSandboxGate::evaluate(partial);
        expect(verdict.authority == SandboxExecutionAuthority::ReadOnly,
               "a single unverified enforcement dimension still granted writes");
        expect(verdict.errorCode
                   == QStringLiteral("sandbox-enforcement-incomplete"),
               "an incomplete sandbox did not report incomplete enforcement");
        expect(verdict.missingEnforcement
                   == QStringList{QString::fromLatin1(label)},
               "the verdict does not name the exact missing dimension");
    }

    // 缺多项时要全部报告:只报第一项会让人以为补上它就够了。
    SandboxEnforcementEvidence none = enforced();
    none.filesystem = SandboxEnforcement::Unverified;
    none.process = SandboxEnforcement::Unavailable;
    none.network = SandboxEnforcement::Unverified;
    expect(ExecutionSandboxGate::evaluate(none).missingEnforcement
               == QStringList{QStringLiteral("sandbox-filesystem-unverified"),
                              QStringLiteral("sandbox-process-unavailable"),
                              QStringLiteral("sandbox-network-unverified")},
           "the verdict does not report every missing enforcement dimension");

    // "确定做不到"与"还没验证"必须可区分:前者是待完成的工作,后者是这个平台上永远
    // 不该开放可写执行的结论。
    SandboxEnforcementEvidence impossible = enforced();
    impossible.network = SandboxEnforcement::Unavailable;
    expect(ExecutionSandboxGate::evaluate(impossible).missingEnforcement
               == QStringList{QStringLiteral("sandbox-network-unavailable")},
           "an impossible dimension is reported as merely unverified");

    // 未知的强制状态必须按未验证处理,不能默认被当成已强制。
    SandboxEnforcementEvidence unknown = enforced();
    unknown.filesystem = static_cast<SandboxEnforcement>(9999);
    expect(ExecutionSandboxGate::evaluate(unknown).authority
               == SandboxExecutionAuthority::ReadOnly,
           "an unclassified enforcement state defaulted to enforced");
}

// 强制齐备仍然不够:该平台可写发布通道的门禁报告必须已经通过。
void releaseGateTests()
{
    SandboxEnforcementEvidence unsigned_ = enforced();
    unsigned_.releaseGateSigned = false;
    const SandboxVerdict verdict = ExecutionSandboxGate::evaluate(unsigned_);
    expect(verdict.authority == SandboxExecutionAuthority::ReadOnly,
           "verified enforcement alone granted writes without a release gate");
    expect(verdict.errorCode == QStringLiteral("sandbox-release-gate-unsigned"),
           "an unsigned release gate was not reported");
    expect(!verdict.fullAccessOfferable,
           "full access is offerable without a signed release gate");

    // 三项齐备且门禁已签署时才授权工作区可写,并且这仍然不是完全访问。
    const SandboxVerdict granted = ExecutionSandboxGate::evaluate(enforced());
    expect(granted.state == SandboxVerdictState::Enforced,
           "a fully verified platform was not reported as enforced");
    expect(granted.authority == SandboxExecutionAuthority::WorkspaceWrite,
           "a fully verified platform did not grant workspace write");
    expect(granted.missingEnforcement.isEmpty(),
           "a fully verified platform reported missing enforcement");
    expect(granted.errorCode.isEmpty(),
           "an enforced verdict carried an error code");
}

// 不支持的平台没有强制证据可谈,而不是"缺三项"。
void platformTests()
{
    SandboxEnforcementEvidence other = enforced();
    other.platform = SandboxPlatform::Unsupported;
    const SandboxVerdict verdict = ExecutionSandboxGate::evaluate(other);
    expect(verdict.authority == SandboxExecutionAuthority::ReadOnly,
           "an unsupported platform granted execution authority");
    expect(verdict.errorCode == QStringLiteral("sandbox-platform-unsupported"),
           "an unsupported platform was not reported as unsupported");
    expect(!verdict.fullAccessOfferable,
           "full access is offerable on a platform with no enforcement mechanism");

    // Windows 与 macOS 都必须走同一条判定路径:任何一个平台被硬编码成已强制,都会让
    // 该平台在没有围栏的地方放开写入。
    for (const SandboxPlatform platform : {SandboxPlatform::MacOS,
                                           SandboxPlatform::Windows}) {
        SandboxEnforcementEvidence bare;
        bare.platform = platform;
        expect(ExecutionSandboxGate::evaluate(bare).authority
                   == SandboxExecutionAuthority::ReadOnly,
               "a platform grants writes without any enforcement evidence");
    }
}

// 已证实的策略绕过必须阻断可写通道,而不是退回到"缺哪几项"的讨论。
void escapeRegressionTests()
{
    SandboxEnforcementEvidence escaped = enforced();
    escaped.escapeRegressionOpen = true;
    const SandboxVerdict verdict = ExecutionSandboxGate::evaluate(escaped);
    expect(verdict.state == SandboxVerdictState::Blocked,
           "a demonstrated policy bypass did not block the write-capable channel");
    expect(verdict.authority == SandboxExecutionAuthority::ReadOnly,
           "a demonstrated policy bypass still granted writes");
    expect(verdict.errorCode == QStringLiteral("sandbox-escape-regression-open"),
           "a demonstrated policy bypass was not reported as such");
    // 关键:此时完全访问也不可提供。那只是把已知可绕过的边界换成没有边界。
    expect(!verdict.fullAccessOfferable,
           "a demonstrated policy bypass left full access offerable");
}

// 被沙箱拒绝的动作不得在沙箱之外自动重试,也不得被报告成模型失败。
void denialTests()
{
    const SandboxDenial denial =
        ExecutionSandboxGate::denial(QStringLiteral("filesystem-write"));
    expect(denial.errorCode
               == QStringLiteral("sandbox-denied-filesystem-write"),
           "a sandbox denial is not identifiable as a sandbox denial");
    expect(!denial.retryOutsideSandbox,
           "a sandbox denial may be retried outside the sandbox");
    expect(!denial.attributableToModel,
           "a sandbox denial may be reported as a model failure");
    expect(ExecutionSandboxGate::denial(QString()).errorCode
               == QStringLiteral("sandbox-denied"),
           "an unlabeled sandbox denial lost its sandbox attribution");
}

// 只读是可用的下界;写入、命令执行与 Git mutation 都在其外。
void authorityTests()
{
    expect(!ExecutionSandboxGate::beyondReadOnly(
               SandboxExecutionAuthority::ReadOnly),
           "read-only was classified as beyond read-only");
    for (const SandboxExecutionAuthority authority : {
             SandboxExecutionAuthority::WorkspaceWrite,
             SandboxExecutionAuthority::FullAccess}) {
        expect(ExecutionSandboxGate::beyondReadOnly(authority),
               "a write-capable authority was classified as read-only");
    }
    // 未知权限级别必须按越出只读处理:新增级别不应默认被当成无害的。
    expect(ExecutionSandboxGate::beyondReadOnly(
               static_cast<SandboxExecutionAuthority>(9999)),
           "an unclassified authority level defaulted to read-only");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    currentEvidenceTests();
    dimensionTests();
    releaseGateTests();
    platformTests();
    escapeRegressionTests();
    denialTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "execution sandbox gate tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
