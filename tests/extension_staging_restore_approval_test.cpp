#include "extension_staging_restore_approval.h"

#include "mcp_configuration_inventory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

int failures = 0;

const QString kSkillSubject = QStringLiteral("skill:example");
const QString kMcpSubject = QStringLiteral("mcp:my-server");

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

ExtensionTreeCaptureDomain skillDomain()
{
    return {QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
            QStringLiteral("extension-content:sha256:"),
            QStringLiteral("skill")};
}

// 固定密钥 provider：暂存域的加密往返不依赖真实安全存储。
class FixedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *key, QString *) override
    {
        if (!key) return false;
        *key = QByteArray(32, 's');
        return true;
    }
};

// 真实磁盘观察：只在测试临时目录上工作，只读。
class DiskObservation final : public ExtensionStagingRestoreObservation
{
public:
    explicit DiskObservation(const QString &root) : m_root(root) {}

    QString canonicalRoot() override
    {
        return QFileInfo(m_root).canonicalFilePath();
    }

    NodeKind nodeKind(const QString &relativePath) override
    {
        const QString absolute = relativePath.isEmpty()
            ? m_root : m_root + QLatin1Char('/') + relativePath;
        const QFileInfo info(absolute);
        if (!info.exists()) return NodeKind::Missing;
        if (info.isSymLink()) return NodeKind::Symlink;
        if (info.isDir()) return NodeKind::Directory;
        if (info.isFile()) return NodeKind::File;
        return NodeKind::Other;
    }

    bool fileContent(const QString &relativePath, QByteArray *content) override
    {
        if (!content) return false;
        QFile file(m_root + QLatin1Char('/') + relativePath);
        if (!file.open(QIODevice::ReadOnly)) return false;
        *content = file.readAll();
        return true;
    }

private:
    QString m_root;
};

QDateTime fixtureCreatedAt()
{
    return QDateTime::fromString(QStringLiteral("2026-09-05T12:00:00.123Z"),
                                 Qt::ISODateWithMs);
}

QDateTime fixtureNow()
{
    return fixtureCreatedAt().addDays(1);
}

QString fixtureBackupId(int variant)
{
    return QStringLiteral("ext_20260905_120000_%1")
        .arg(variant, 8, 16, QLatin1Char('0'));
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

QString syntheticIdentity(const QString &prefix, const QByteArray &seed)
{
    return prefix + sha256Hex(seed);
}

// 一棵夹具树：若干文件加若干空目录。
struct TreeSpec {
    QList<QPair<QString, QByteArray>> files;
    QStringList directories;
};

bool writeTree(const QString &root, const TreeSpec &spec)
{
    if (!QDir().mkpath(root)) return false;
    for (const QString &directory : spec.directories) {
        if (!QDir().mkpath(root + QLatin1Char('/') + directory)) return false;
    }
    for (const auto &file : spec.files) {
        if (!writeFile(root + QLatin1Char('/') + file.first, file.second)) {
            return false;
        }
    }
    return true;
}

bool scan(const QString &root, QVector<ExtensionTreeCaptureEntry> *tree)
{
    ExtensionTreeCaptureBudget budget;
    ExtensionTreeCaptureError error;
    const QString canonical = QFileInfo(root).canonicalFilePath();
    return ExtensionTreeCapture::scanDirectory(
        skillDomain(), canonical, canonical, QString(), 0, &budget, tree, &error);
}

// 一条走完的链路留下的全部中间产物：后续测试既要提示本身，也要计划、描述与读回的
// 快照（例如把同一份备份对另一个目标根重新计划）。
struct ChainFixture {
    ConfigurationBackupSnapshot snapshot;
    ExtensionStagingRestoreBackupDescriptor descriptor;
    QString canonicalDestination;
    ExtensionStagingRestorePlan plan;
    ExtensionStagingRestorePrompt prompt;
};

// 完整链路：真实临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 读回并计划 →
// 呈现。目标根在计划之前按 destination 夹具预置内容。
bool buildChain(QTemporaryDir &temporary, const QString &name, int variant,
                const QString &subject,
                const ExtensionTreeCaptureDomain &domain,
                const QVector<ExtensionTreeCaptureEntry> &tree,
                const TreeSpec &destinationSpec, const QDateTime &now,
                ChainFixture *fixture)
{
    const QString base = temporary.path() + QLatin1Char('/') + name;
    const QString backupRoot = base + QStringLiteral("/backups");
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!ExtensionStagingSnapshot::build(domain, tree, subject,
                                         fixtureBackupId(variant),
                                         fixtureCreatedAt(), &snapshot,
                                         &error)) {
        return expect(false, "fixture snapshot refused");
    }
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    if (!expect(store.create(snapshot, &error),
                "the fixture backup could not be stored")) {
        return false;
    }
    // 备份描述来自清点层，与真实调用方的来源一致。
    ExtensionStagingBackupListResult listing;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, subject, &listing, &error)
                    && listing.state == ExtensionStagingBackupListState::Ready
                    && listing.entries.size() == 1,
                "the fixture backup could not be listed intact")) {
        return false;
    }
    const ExtensionStagingBackupListEntry &entry = listing.entries.first();
    fixture->descriptor.backupId = entry.backupId;
    fixture->descriptor.subject = entry.subject;
    fixture->descriptor.createdAt = entry.createdAt;
    fixture->descriptor.verification = entry.verification;

    if (!expect(store.read(subject, fixtureBackupId(variant),
                           &fixture->snapshot, &error),
                "the fixture backup could not be read back")) {
        return false;
    }
    const QString destination = base + QStringLiteral("/dest");
    if (!expect(writeTree(destination, destinationSpec),
                "the destination fixture could not be written")) {
        return false;
    }
    DiskObservation observation(destination);
    fixture->canonicalDestination = observation.canonicalRoot();
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    domain, subject, fixture->snapshot,
                    fixture->canonicalDestination, &observation, &fixture->plan,
                    &error),
                "the fixture could not be planned")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return false;
    }
    fixture->prompt = ExtensionStagingRestorePresentation::build(
        fixture->plan, fixture->descriptor, fixture->canonicalDestination,
        now);
    if (!expect(fixture->prompt.state
                    == ExtensionStagingRestorePromptState::Ready,
                "a clean fixture plan was not presentable")) {
        QTextStream(stderr) << "  presentation said: "
                            << fixture->prompt.errorCode << '\n';
        return false;
    }
    return true;
}

// 技能链路：源树夹具先落盘再捕获。
bool buildSkillChain(QTemporaryDir &temporary, const QString &name, int variant,
                     const TreeSpec &source, const TreeSpec &destination,
                     const QDateTime &now, ChainFixture *fixture)
{
    const QString root =
        temporary.path() + QLatin1Char('/') + name + QStringLiteral("/tree");
    if (!expect(writeTree(root, source),
                "the source fixture could not be written")) {
        return false;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "the source fixture could not be captured")) {
        return false;
    }
    return buildChain(temporary, name, variant, kSkillSubject, skillDomain(),
                      tree, destination, now, fixture);
}

// mcp 链路：整文件语义，固定单条目 settings.json 树。
bool buildMcpChain(QTemporaryDir &temporary, const QString &name, int variant,
                   const QDateTime &now, ChainFixture *fixture)
{
    const QByteArray settings = QByteArrayLiteral(
        "{\"mcpServers\":{\"my-server\":{\"command\":\"srv\"}},\"other\":1}\n");
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("settings.json"), false, settings});
    return buildChain(temporary, name, variant, kMcpSubject,
                      McpConfigurationInventory::backupCaptureDomain(), tree,
                      TreeSpec{}, now, fixture);
}

// 与提示逐项对齐的批准：这是唯一应当被授权的形状。
ExtensionStagingRestoreApprovalAcknowledgement acknowledge(
    const ExtensionStagingRestorePrompt &prompt, bool highRiskConfirmed)
{
    ExtensionStagingRestoreApprovalAcknowledgement value;
    value.decision = ExtensionStagingRestoreApprovalDecision::Approve;
    value.subject = prompt.subject;
    value.backupId = prompt.backupId;
    value.destinationRoot = prompt.destinationRoot;
    value.approvedPlanIdentity = prompt.echoedPlanIdentity;
    value.approvedTreeIdentity = prompt.echoedTreeIdentity;
    value.acknowledgedWarnings = prompt.warnings;
    value.highRiskConfirmed = highRiskConfirmed;
    return value;
}

ExtensionStagingRestoreApprovalVerdict evaluate(
    const ChainFixture &fixture,
    const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement)
{
    return ExtensionStagingRestoreApprovalPolicy::evaluate(
        fixture.prompt, fixture.descriptor.verification, acknowledgement);
}

bool refusedWith(const ExtensionStagingRestoreApprovalVerdict &verdict,
                 const QString &code)
{
    return verdict.state == ExtensionStagingRestoreApprovalState::Refused
        && verdict.errorCode == code
        && verdict.authorizedPlanIdentity.isEmpty()
        && verdict.authorizedTreeIdentity.isEmpty();
}

// 完整链路 + 逐项对齐的批准 → 签发绑定确切计划身份的凭据；拒绝不产生授权。
void testFullChainAlignedApprovalIssuesCredential()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const TreeSpec source{{{QStringLiteral("a.txt"), QByteArrayLiteral("alpha\n")},
                           {QStringLiteral("sub/b.txt"), QByteArrayLiteral("beta\n")}},
                          {QStringLiteral("zdir")}};
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("full"), 1, source,
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }
    // 空目标上的技能恢复只携带不执行披露：纯信息性警告不要求确认。
    expect(fixture.prompt.warnings.size() == 1
               && fixture.prompt.warnings.contains(
                   ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet),
           "an empty-destination skill restore carried unexpected warnings");

    const ExtensionStagingRestoreApprovalVerdict verdict =
        evaluate(fixture, acknowledge(fixture.prompt, false));
    expect(verdict.state == ExtensionStagingRestoreApprovalState::Authorized,
           "an approval aligned with its restore prompt was refused");
    expect(verdict.errorCode.isEmpty(),
           "an authorized verdict carried an error code");
    // 凭据绑定确切的计划身份与树身份。
    expect(verdict.authorizedPlanIdentity == fixture.plan.planIdentity
               && !verdict.authorizedPlanIdentity.isEmpty(),
           "the credential does not bind the exact plan identity");
    expect(verdict.authorizedTreeIdentity == fixture.plan.treeIdentity
               && !verdict.authorizedTreeIdentity.isEmpty(),
           "the credential does not bind the exact tree identity");

    // 拒绝不产生授权，也不留下任何身份。
    ExtensionStagingRestoreApprovalAcknowledgement declined =
        acknowledge(fixture.prompt, true);
    declined.decision = ExtensionStagingRestoreApprovalDecision::Decline;
    expect(refusedWith(evaluate(fixture, declined),
                       QStringLiteral("extension-restore-approval-declined")),
           "a declined decision produced restore authority");
}

// 非 Ready 提示不能被批准：Refused 提示上没有可批准的标的物，Unpresentable 提示没有
// 人可能看过——走到审批的这类批准要么过期要么伪造。
void testPromptStateRefusals()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("states"), 2,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }

    // 构建失败的计划渲染成 Refused：即使其余字段尽量对齐，也不能被批准。
    const ExtensionStagingRestorePrompt refused =
        ExtensionStagingRestorePresentation::buildRefusal(
            QStringLiteral("extension-staging-restore-destination-conflict"));
    if (!expect(refused.state == ExtensionStagingRestorePromptState::Refused,
                "the refusal fixture did not render as refused")) {
        return;
    }
    ExtensionStagingRestoreApprovalAcknowledgement acknowledgement;
    acknowledgement.decision = ExtensionStagingRestoreApprovalDecision::Approve;
    acknowledgement.subject = fixture.prompt.subject;
    acknowledgement.backupId = fixture.prompt.backupId;
    acknowledgement.destinationRoot = fixture.prompt.destinationRoot;
    acknowledgement.approvedPlanIdentity = fixture.prompt.echoedPlanIdentity;
    acknowledgement.approvedTreeIdentity = fixture.prompt.echoedTreeIdentity;
    acknowledgement.acknowledgedWarnings = refused.warnings;
    acknowledgement.highRiskConfirmed = true;
    expect(ExtensionStagingRestoreApprovalPolicy::evaluate(
               refused, fixture.descriptor.verification, acknowledgement)
                   .errorCode
               == QStringLiteral("extension-restore-approval-prompt-refused"),
           "an approval against a refused restore prompt was accepted");

    // 内容无法展示的提示：人不可能看过它，批准必然过期或伪造。
    ExtensionStagingRestoreBackupDescriptor corruptDescriptor =
        fixture.descriptor;
    corruptDescriptor.verification =
        ExtensionStagingBackupEntryVerification::ListedCorrupt;
    const ExtensionStagingRestorePrompt unpresentable =
        ExtensionStagingRestorePresentation::build(
            fixture.plan, corruptDescriptor, fixture.canonicalDestination,
            fixtureNow());
    if (!expect(unpresentable.state
                    == ExtensionStagingRestorePromptState::Unpresentable,
                "the corrupt-descriptor fixture was presentable")) {
        return;
    }
    expect(ExtensionStagingRestoreApprovalPolicy::evaluate(
               unpresentable, fixture.descriptor.verification, acknowledgement)
                   .errorCode
               == QStringLiteral(
                   "extension-restore-approval-prompt-unpresentable"),
           "an approval against an unpresentable restore prompt was accepted");
}

// 备份的清点验证状态是必需输入：从一份未通过清单身份级验证的备份恢复等于从未经认证
// 的字节恢复，比不恢复更糟，即使批准与提示逐项对齐也必须拒绝。
void testUnverifiedBackupRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("verify"), 3,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }
    const ExtensionStagingRestoreApprovalAcknowledgement aligned =
        acknowledge(fixture.prompt, false);
    expect(ExtensionStagingRestoreApprovalPolicy::evaluate(
               fixture.prompt,
               ExtensionStagingBackupEntryVerification::ListedCorrupt, aligned)
                   .errorCode
               == QStringLiteral("extension-restore-approval-backup-unverified"),
           "a restore from a corrupt-listed backup was approved");
    // 未归类的未来验证状态同样失败关闭：等值比较不给默认值留余地。
    expect(ExtensionStagingRestoreApprovalPolicy::evaluate(
               fixture.prompt,
               static_cast<ExtensionStagingBackupEntryVerification>(-1),
               aligned)
                   .errorCode
               == QStringLiteral("extension-restore-approval-backup-unverified"),
           "an unclassified verification state was treated as verified");
}

// 每一个对齐维度各自独立拒绝：主体、备份 id、目标根、计划身份、树身份，以及身份
// 本身的形状。
void testMismatchDimensionsRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("mismatch"), 4,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }
    const QString prefix = QStringLiteral("extension-restore-approval-");

    ExtensionStagingRestoreApprovalAcknowledgement wrongSubject =
        acknowledge(fixture.prompt, false);
    wrongSubject.subject = QStringLiteral("skill:other");
    expect(refusedWith(evaluate(fixture, wrongSubject),
                       prefix + QStringLiteral("subject-mismatch")),
           "an approval for another subject was accepted");

    ExtensionStagingRestoreApprovalAcknowledgement wrongBackup =
        acknowledge(fixture.prompt, false);
    wrongBackup.backupId = fixtureBackupId(9);
    expect(refusedWith(evaluate(fixture, wrongBackup),
                       prefix + QStringLiteral("backup-mismatch")),
           "an approval for another backup id was accepted");

    ExtensionStagingRestoreApprovalAcknowledgement wrongDestination =
        acknowledge(fixture.prompt, false);
    wrongDestination.destinationRoot = QStringLiteral("/elsewhere");
    expect(refusedWith(evaluate(fixture, wrongDestination),
                       prefix + QStringLiteral("destination-mismatch")),
           "an approval for another destination root was accepted");

    // 计划身份形状合法但与回显不同：渲染与批准之间发生了漂移。
    ExtensionStagingRestoreApprovalAcknowledgement planDrift =
        acknowledge(fixture.prompt, false);
    planDrift.approvedPlanIdentity = syntheticIdentity(
        QStringLiteral("extension-staging-restore-plan:sha256:"), "other-plan");
    expect(refusedWith(evaluate(fixture, planDrift),
                       prefix + QStringLiteral("plan-drift")),
           "a drifted plan identity was accepted");

    // 树身份形状合法但与回显不同：同一份计划被套用到另一份内容上。
    ExtensionStagingRestoreApprovalAcknowledgement treeDrift =
        acknowledge(fixture.prompt, false);
    treeDrift.approvedTreeIdentity = syntheticIdentity(
        QStringLiteral("extension-content:sha256:"), "other-tree");
    expect(refusedWith(evaluate(fixture, treeDrift),
                       prefix + QStringLiteral("tree-drift")),
           "a drifted tree identity was accepted");

    // 畸形身份无法与任何内容对齐：先于漂移检查整体拒绝。
    ExtensionStagingRestoreApprovalAcknowledgement malformedPlan =
        acknowledge(fixture.prompt, false);
    malformedPlan.approvedPlanIdentity = QStringLiteral("not-a-hash");
    expect(refusedWith(evaluate(fixture, malformedPlan),
                       prefix + QStringLiteral("identity-invalid")),
           "a malformed plan identity was accepted");
    ExtensionStagingRestoreApprovalAcknowledgement malformedTree =
        acknowledge(fixture.prompt, false);
    malformedTree.approvedTreeIdentity =
        QStringLiteral("extension-content:sha256:abcd");
    expect(refusedWith(evaluate(fixture, malformedTree),
                       prefix + QStringLiteral("identity-invalid")),
           "a malformed tree identity was accepted");
}

// 警告集合是凭据的一部分：披露过的警告缺一不可，回传未披露的警告来自另一个界面
// 状态，重复回传与什么都不回传同样拒绝。
void testWarningAcknowledgementDiscipline()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("warnings"), 5,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }
    const QString prefix = QStringLiteral("extension-restore-approval-");

    // 什么都不回传等于"我没看到任何风险"，而每一份 Ready 提示都至少携带不执行披露。
    ExtensionStagingRestoreApprovalAcknowledgement none =
        acknowledge(fixture.prompt, false);
    none.acknowledgedWarnings.clear();
    expect(refusedWith(evaluate(fixture, none),
                       prefix + QStringLiteral("warning-undisclosed")),
           "an approval acknowledging nothing was accepted");

    // 少回传一项披露的警告：这份批准对应一个风险更少的界面。
    ExtensionStagingRestoreApprovalAcknowledgement missing =
        acknowledge(fixture.prompt, false);
    missing.acknowledgedWarnings.removeAll(
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet);
    expect(refusedWith(evaluate(fixture, missing),
                       prefix + QStringLiteral("warning-undisclosed")),
           "an approval missing a disclosed warning was accepted");

    // 回传提示未曾披露的警告：这份批准来自另一个界面状态。
    ExtensionStagingRestoreApprovalAcknowledgement extra =
        acknowledge(fixture.prompt, false);
    extra.acknowledgedWarnings.append(
        ExtensionStagingRestoreWarning::OldBackup);
    expect(refusedWith(evaluate(fixture, extra),
                       prefix + QStringLiteral("warning-unknown")),
           "an approval acknowledging an undisclosed warning was accepted");

    // 重复回传：集合与清单不一致说明回传方状态损坏。
    ExtensionStagingRestoreApprovalAcknowledgement duplicated =
        acknowledge(fixture.prompt, false);
    duplicated.acknowledgedWarnings.append(
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet);
    expect(refusedWith(evaluate(fixture, duplicated),
                       prefix + QStringLiteral("warning-duplicate")),
           "an approval with a duplicated warning was accepted");
}

// 高风险逐次显式确认：共享设置文件恢复与"向非空目标写入"必须确认；纯信息性警告
// （不执行披露、already-in-place、陈旧、大型）不要求确认。
void testHighRiskConfirmation()
{
    using Warning = ExtensionStagingRestoreWarning;
    using Policy = ExtensionStagingRestoreApprovalPolicy;
    // 分类本身的直接钉：高风险为真，信息性为假，未归类失败关闭。
    expect(Policy::requiresExplicitConfirmation(
               Warning::SharedSettingsFileRestore, 0),
           "a shared settings file restore needs no confirmation");
    expect(Policy::requiresExplicitConfirmation(
               Warning::DestinationNotEmpty, 1)
               && !Policy::requiresExplicitConfirmation(
                   Warning::DestinationNotEmpty, 0),
           "destination-non-empty risk does not track pending writes");
    expect(!Policy::requiresExplicitConfirmation(
               Warning::AlreadyInPlaceFiles, 3)
               && !Policy::requiresExplicitConfirmation(
                   Warning::LargeRestore, 40)
               && !Policy::requiresExplicitConfirmation(
                   Warning::OldBackup, 10)
               && !Policy::requiresExplicitConfirmation(
                   Warning::RestoreDoesNotExecuteYet, 0),
           "an informational warning demands confirmation");
    expect(Policy::requiresExplicitConfirmation(
               static_cast<Warning>(-7), 0),
           "an unclassified warning defaults to needing no confirmation");

    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;

    // mcp 主体：共享设置文件警告在场，未确认即拒绝，确认后授权。
    ChainFixture mcp;
    if (!buildMcpChain(temporary, QStringLiteral("mcp"), 6, fixtureNow(),
                       &mcp)) {
        return;
    }
    if (!expect(mcp.prompt.warnings.contains(
                    Warning::SharedSettingsFileRestore),
                "the mcp fixture lost its shared settings file warning")) {
        return;
    }
    expect(evaluate(mcp, acknowledge(mcp.prompt, false)).errorCode
               == QStringLiteral(
                   "extension-restore-approval-confirmation-required"),
           "an mcp restore approval succeeded without explicit confirmation");
    const ExtensionStagingRestoreApprovalVerdict confirmed =
        evaluate(mcp, acknowledge(mcp.prompt, true));
    expect(confirmed.state == ExtensionStagingRestoreApprovalState::Authorized
               && confirmed.authorizedPlanIdentity == mcp.plan.planIdentity,
           "a confirmed mcp restore approval was refused");

    // 冲突邻接：目标已有与计划一致的内容，且计划仍要写入其他文件。目标非空因此
    // 伴随真实写入，是高风险。
    const TreeSpec source{{{QStringLiteral("a.txt"), QByteArrayLiteral("alpha\n")},
                           {QStringLiteral("b.txt"), QByteArrayLiteral("beta\n")}},
                          {}};
    const TreeSpec partial{{{QStringLiteral("a.txt"),
                             QByteArrayLiteral("alpha\n")}}, {}};
    ChainFixture adjacent;
    if (!buildSkillChain(temporary, QStringLiteral("adjacent"), 7, source,
                         partial, fixtureNow(), &adjacent)) {
        return;
    }
    if (!expect(adjacent.prompt.warnings.contains(Warning::DestinationNotEmpty)
                    && adjacent.prompt.fileWriteCount == 1
                    && adjacent.prompt.alreadyInPlaceCount == 1,
                "the conflict-adjacent fixture did not warn truthfully")) {
        return;
    }
    expect(evaluate(adjacent, acknowledge(adjacent.prompt, false)).errorCode
               == QStringLiteral(
                   "extension-restore-approval-confirmation-required"),
           "a restore writing into a non-empty destination needed no "
           "confirmation");
    expect(evaluate(adjacent, acknowledge(adjacent.prompt, true)).state
               == ExtensionStagingRestoreApprovalState::Authorized,
           "a confirmed conflict-adjacent restore approval was refused");

    // 非空完全由 already-in-place 文件证明：不写入任何字节，纯信息性。
    ChainFixture inPlace;
    if (!buildSkillChain(temporary, QStringLiteral("inplace"), 8, source,
                         source, fixtureNow(), &inPlace)) {
        return;
    }
    if (!expect(inPlace.prompt.warnings.contains(Warning::DestinationNotEmpty)
                    && inPlace.prompt.warnings.contains(
                        Warning::AlreadyInPlaceFiles)
                    && inPlace.prompt.fileWriteCount == 0,
                "the already-in-place fixture did not warn truthfully")) {
        return;
    }
    expect(evaluate(inPlace, acknowledge(inPlace.prompt, false)).state
               == ExtensionStagingRestoreApprovalState::Authorized,
           "a purely already-in-place restore demanded confirmation");

    // 陈旧备份：年龄是信息性披露，计划身份已经把每一个字节绑死。
    ChainFixture old;
    if (!buildSkillChain(temporary, QStringLiteral("old"), 9,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureCreatedAt().addDays(100), &old)) {
        return;
    }
    if (!expect(old.prompt.warnings.contains(Warning::OldBackup),
                "the stale-backup fixture did not warn about its age")) {
        return;
    }
    expect(evaluate(old, acknowledge(old.prompt, false)).state
               == ExtensionStagingRestoreApprovalState::Authorized,
           "an old backup restore demanded confirmation");
}

// 同一份备份对另一个目标根重新计划就是另一份计划：旧凭据绑定旧计划身份，对新提示
// 不再匹配，必须重新批准。
void testReplanAgainstDifferentDestinationRequiresFreshApproval()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture first;
    if (!buildSkillChain(temporary, QStringLiteral("first"), 10,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &first)) {
        return;
    }
    const ExtensionStagingRestoreApprovalVerdict firstVerdict =
        evaluate(first, acknowledge(first.prompt, false));
    if (!expect(firstVerdict.state
                    == ExtensionStagingRestoreApprovalState::Authorized,
                "the first destination approval was refused")) {
        return;
    }

    // 同一份读回快照、同一份描述，对第二个目标根重新计划并呈现。
    const QString second = temporary.path()
        + QStringLiteral("/first/dest-second");
    if (!expect(QDir().mkpath(second),
                "the second destination fixture could not be created")) {
        return;
    }
    DiskObservation observation(second);
    ExtensionStagingRestorePlan replanned;
    QString error;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    skillDomain(), kSkillSubject, first.snapshot,
                    observation.canonicalRoot(), &observation, &replanned,
                    &error),
                "the same backup could not be re-planned")) {
        return;
    }
    const ExtensionStagingRestorePrompt secondPrompt =
        ExtensionStagingRestorePresentation::build(
            replanned, first.descriptor, observation.canonicalRoot(),
            fixtureNow());
    if (!expect(secondPrompt.state
                    == ExtensionStagingRestorePromptState::Ready,
                "the re-planned restore was not presentable")) {
        return;
    }
    expect(replanned.planIdentity != first.plan.planIdentity,
           "re-planning against another destination kept the plan identity");

    // 拿着旧凭据对应的内容批准新提示：计划身份漂移，拒绝。
    ExtensionStagingRestoreApprovalAcknowledgement stale =
        acknowledge(secondPrompt, false);
    stale.approvedPlanIdentity = first.prompt.echoedPlanIdentity;
    expect(ExtensionStagingRestoreApprovalPolicy::evaluate(
               secondPrompt, first.descriptor.verification, stale).errorCode
               == QStringLiteral("extension-restore-approval-plan-drift"),
           "the old credential matched a re-planned restore");

    // 逐项对齐新提示才签发新凭据，且新凭据绑定新的计划身份。
    const ExtensionStagingRestoreApprovalVerdict secondVerdict =
        ExtensionStagingRestoreApprovalPolicy::evaluate(
            secondPrompt, first.descriptor.verification,
            acknowledge(secondPrompt, false));
    expect(secondVerdict.state
               == ExtensionStagingRestoreApprovalState::Authorized
               && secondVerdict.authorizedPlanIdentity
                   == replanned.planIdentity
               && secondVerdict.authorizedPlanIdentity
                   != firstVerdict.authorizedPlanIdentity,
           "the re-planned restore did not require and receive a fresh "
           "approval");
}

// 未归类的警告类别必须默认要求确认：它落在穷尽 switch 之后的失败关闭分支上，普通
// 测试观察不到，因此直接把界外值注入提示与回传集合驱动它。
void testUnclassifiedWarningFailsClosed()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildSkillChain(temporary, QStringLiteral("unclassified"), 11,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         TreeSpec{}, fixtureNow(), &fixture)) {
        return;
    }
    const ExtensionStagingRestoreWarning outOfRange =
        static_cast<ExtensionStagingRestoreWarning>(-7);
    fixture.prompt.warnings.append(outOfRange);
    expect(evaluate(fixture, acknowledge(fixture.prompt, false)).errorCode
               == QStringLiteral(
                   "extension-restore-approval-confirmation-required"),
           "an unclassified warning needed no confirmation");
    expect(evaluate(fixture, acknowledge(fixture.prompt, true)).state
               == ExtensionStagingRestoreApprovalState::Authorized,
           "a confirmed unclassified warning was still refused");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testFullChainAlignedApprovalIssuesCredential();
    testPromptStateRefusals();
    testUnverifiedBackupRefused();
    testMismatchDimensionsRefused();
    testWarningAcknowledgementDiscipline();
    testHighRiskConfirmation();
    testReplanAgainstDifferentDestinationRequiresFreshApproval();
    testUnclassifiedWarningFailsClosed();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore approval guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
