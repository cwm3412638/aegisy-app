#include "extension_staging_restore_presentation.h"

#include "extension_display_safety.h"
#include "mcp_configuration_inventory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
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

bool scan(const QString &root, QVector<ExtensionTreeCaptureEntry> *tree)
{
    ExtensionTreeCaptureBudget budget;
    ExtensionTreeCaptureError error;
    const QString canonical = QFileInfo(root).canonicalFilePath();
    return ExtensionTreeCapture::scanDirectory(
        skillDomain(), canonical, canonical, QString(), 0, &budget, tree, &error);
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

ConfigurationBackupSnapshot buildOrDie(
        const ExtensionTreeCaptureDomain &domain,
        const QVector<ExtensionTreeCaptureEntry> &tree, const QString &subject,
        int variant)
{
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!ExtensionStagingSnapshot::build(domain, tree, subject,
                                         fixtureBackupId(variant),
                                         fixtureCreatedAt(), &snapshot,
                                         &error)) {
        QTextStream(stderr) << "FAIL: fixture snapshot refused: " << error
                            << '\n';
        ++failures;
    }
    return snapshot;
}

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

// 注入式假观察：模拟任意目标状态（一致内容、脏树）而不触碰真实产品目录。
class FakeObservation final : public ExtensionStagingRestoreObservation
{
public:
    struct Node {
        NodeKind kind = NodeKind::Missing;
        QByteArray content;
    };

    QString root;
    QMap<QString, Node> nodes;

    QString canonicalRoot() override { return root; }

    NodeKind nodeKind(const QString &relativePath) override
    {
        const auto it = nodes.constFind(relativePath);
        return it == nodes.constEnd() ? NodeKind::Missing : it->kind;
    }

    bool fileContent(const QString &relativePath, QByteArray *content) override
    {
        if (!content) return false;
        const auto it = nodes.constFind(relativePath);
        if (it == nodes.constEnd() || it->kind != NodeKind::File) return false;
        *content = it->content;
        return true;
    }
};

FakeObservation emptyDestination(const QString &root)
{
    FakeObservation observation;
    observation.root = root;
    observation.nodes.insert(
        QString(),
        {ExtensionStagingRestoreObservation::NodeKind::Directory, {}});
    return observation;
}

bool warns(const ExtensionStagingRestorePrompt &prompt,
           ExtensionStagingRestoreWarning warning)
{
    return prompt.warnings.contains(warning);
}

ExtensionStagingRestoreBackupDescriptor descriptorFor(const QString &subject,
                                                      int variant)
{
    ExtensionStagingRestoreBackupDescriptor descriptor;
    descriptor.backupId = fixtureBackupId(variant);
    descriptor.subject = subject;
    descriptor.createdAt = fixtureCreatedAt();
    descriptor.verification =
        ExtensionStagingBackupEntryVerification::ListedIntact;
    return descriptor;
}

// 手工构造的形状合法计划：身份只校验形式（呈现层不重算身份——它绑定的是计划层给出
// 的身份本身），因此合成身份可以驱动截断与拒绝路径的测试。
QString syntheticIdentity(const QString &prefix, const QByteArray &seed)
{
    return prefix + sha256Hex(seed);
}

ExtensionStagingRestorePlan handPlan(const QString &subject,
                                     const QString &treePrefix, int fileCount)
{
    ExtensionStagingRestorePlan plan;
    plan.destinationRoot = QStringLiteral("/dest");
    plan.subject = subject;
    plan.treeIdentity = syntheticIdentity(treePrefix, subject.toUtf8() + "-tree");
    plan.planIdentity = syntheticIdentity(
        QStringLiteral("extension-staging-restore-plan:sha256:"),
        subject.toUtf8() + "-plan");
    for (int index = 0; index < fileCount; ++index) {
        ExtensionStagingRestoreOperation operation;
        operation.directory = false;
        operation.relativePath = QStringLiteral("f%1.txt").arg(index);
        operation.byteCount = 1;
        operation.sha256 = sha256Hex(
            QByteArrayLiteral("x") + QByteArray::number(index));
        operation.sourceSlot = index + 1;
        plan.operations.append(operation);
    }
    return plan;
}

// 完整链路：真实临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 验证并计划 →
// 呈现。渲染字段必须与计划逐字段一致。
void testFullChainRendersExactly()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("beta\n"))
                    && QDir().mkpath(root + QStringLiteral("/zdir")),
                "full-chain fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "full-chain fixture could not be captured")) {
        return;
    }
    const ConfigurationBackupSnapshot built =
        buildOrDie(skillDomain(), tree, kSkillSubject, 1);
    if (built.files.isEmpty()) return;

    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    QString error;
    if (!expect(store.create(built, &error),
                "the full-chain fixture could not be stored")) {
        return;
    }
    // 备份描述来自清点层，与真实调用方的来源一致。
    ExtensionStagingBackupListResult listing;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, kSkillSubject, &listing, &error)
                    && listing.state == ExtensionStagingBackupListState::Ready
                    && listing.entries.size() == 1,
                "the full-chain backup could not be listed intact")) {
        return;
    }
    const ExtensionStagingBackupListEntry &entry = listing.entries.first();
    ExtensionStagingRestoreBackupDescriptor descriptor;
    descriptor.backupId = entry.backupId;
    descriptor.subject = entry.subject;
    descriptor.createdAt = entry.createdAt;
    descriptor.verification = entry.verification;

    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSkillSubject, fixtureBackupId(1), &readBack,
                           &error),
                "the full-chain fixture could not be read back")) {
        return;
    }
    const QString destination = temporary.path() + QStringLiteral("/dest");
    if (!expect(QDir().mkpath(destination),
                "the destination fixture could not be created")) {
        return;
    }
    DiskObservation observation(destination);
    ExtensionStagingRestorePlan plan;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    skillDomain(), kSkillSubject, readBack,
                    observation.canonicalRoot(), &observation, &plan, &error),
                "the full-chain fixture could not be planned")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }

    const ExtensionStagingRestorePrompt prompt =
        ExtensionStagingRestorePresentation::build(
            plan, descriptor, observation.canonicalRoot(), fixtureNow());
    if (!expect(prompt.state == ExtensionStagingRestorePromptState::Ready,
                "a clean full-chain plan was not presentable")) {
        QTextStream(stderr) << "  presentation said: " << prompt.errorCode
                            << '\n';
        return;
    }
    expect(prompt.approvable, "a ready restore prompt is not approvable");
    expect(prompt.subject == kSkillSubject
               && prompt.backupId == fixtureBackupId(1)
               && prompt.createdAtLabel
                   == fixtureCreatedAt().toUTC().toString(Qt::ISODateWithMs)
               && prompt.destinationRoot == observation.canonicalRoot(),
           "the rendered backup fields do not match the descriptor and plan");
    // 身份：完整身份原样回显，指纹是两端形式。
    expect(prompt.planIdentity == plan.planIdentity
               && prompt.treeIdentity == plan.treeIdentity
               && prompt.echoedPlanIdentity == plan.planIdentity
               && prompt.echoedTreeIdentity == plan.treeIdentity,
           "the rendered identities do not echo the plan identities");
    expect(prompt.planFingerprint
               == ExtensionDisplaySafety::fingerprint(plan.planIdentity)
               && prompt.treeFingerprint
                   == ExtensionDisplaySafety::fingerprint(plan.treeIdentity),
           "the rendered fingerprints are not the both-ends form");
    const QString planHex = plan.planIdentity.section(QLatin1Char(':'), -1);
    expect(prompt.planFingerprint.startsWith(planHex.left(8))
               && prompt.planFingerprint.endsWith(planHex.right(8)),
           "the plan fingerprint does not keep both ends of the identity");
    // 统计与计划逐字段一致：两条目录、两条待写文件、零 already-in-place、11 字节。
    expect(prompt.directoryCount == 2 && prompt.fileWriteCount == 2
               && prompt.alreadyInPlaceCount == 0 && prompt.totalBytes == 11,
           "the rendered operation counts do not match the plan");
    expect(prompt.entries.size() == 4 && !prompt.listingTruncated
               && prompt.omittedEntryCount == 0
               && prompt.truncationNote.isEmpty(),
           "a four-operation plan was truncated");
    expect(prompt.identityBindingNote.contains(QStringLiteral("4"))
               && !prompt.identityBindingNote.isEmpty(),
           "the binding statement does not cover the complete plan");
    expect(prompt.entries.at(0).directory
               && prompt.entries.at(0).relativePath == QStringLiteral("sub")
               && prompt.entries.at(2).relativePath == QStringLiteral("a.txt")
               && prompt.entries.at(2).byteCount == 6
               && prompt.entries.at(2).sha256
                   == sha256Hex(QByteArrayLiteral("alpha\n")),
           "the rendered entry rows do not match the plan operations");
    // 技能主体：无共享文件警告；不执行披露必须在场。
    expect(!warns(prompt, ExtensionStagingRestoreWarning::SharedSettingsFileRestore),
           "a skill restore carried the shared settings file warning");
    expect(prompt.sharedFileOverwriteNote.isEmpty(),
           "a skill restore carried shared-file overwrite prose");
    expect(warns(prompt, ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet)
               && !prompt.doesNotExecuteNote.isEmpty(),
           "the does-not-execute-yet disclosure is missing");
    expect(!warns(prompt, ExtensionStagingRestoreWarning::AlreadyInPlaceFiles)
               && !warns(prompt, ExtensionStagingRestoreWarning::DestinationNotEmpty),
           "an empty destination was reported as non-empty");
    expect(prompt.errorCode.isEmpty() && prompt.refusalReason.isEmpty(),
           "a ready prompt carried a refusal or error");
}

// mcp 主体的共享文件警告是强制的：恢复覆盖整个共享设置文件，包括其他服务器的配置。
void testMcpSharedFileWarning()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QByteArray settings = QByteArrayLiteral(
        "{\"mcpServers\":{\"my-server\":{\"command\":\"srv\"}},\"other\":1}\n");
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("settings.json"), false, settings});
    const ExtensionTreeCaptureDomain &mcpDomain =
        McpConfigurationInventory::backupCaptureDomain();
    const ConfigurationBackupSnapshot built =
        buildOrDie(mcpDomain, tree, kMcpSubject, 2);
    if (built.files.isEmpty()) return;

    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    QString error;
    if (!expect(store.create(built, &error),
                "the mcp fixture could not be stored")) {
        return;
    }
    ExtensionStagingBackupListResult listing;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, kMcpSubject, &listing, &error)
                    && listing.entries.size() == 1,
                "the mcp backup could not be listed")) {
        return;
    }
    ExtensionStagingRestoreBackupDescriptor descriptor;
    descriptor.backupId = listing.entries.first().backupId;
    descriptor.subject = listing.entries.first().subject;
    descriptor.createdAt = listing.entries.first().createdAt;
    descriptor.verification = listing.entries.first().verification;

    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kMcpSubject, fixtureBackupId(2), &readBack, &error),
                "the mcp fixture could not be read back")) {
        return;
    }
    FakeObservation observation = emptyDestination(
        temporary.path() + QStringLiteral("/dest"));
    ExtensionStagingRestorePlan plan;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    mcpDomain, kMcpSubject, readBack, observation.root,
                    &observation, &plan, &error),
                "the mcp fixture could not be planned")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }

    const ExtensionStagingRestorePrompt prompt =
        ExtensionStagingRestorePresentation::build(
            plan, descriptor, observation.root, fixtureNow());
    if (!expect(prompt.state == ExtensionStagingRestorePromptState::Ready,
                "an mcp restore plan was not presentable")) {
        QTextStream(stderr) << "  presentation said: " << prompt.errorCode
                            << '\n';
        return;
    }
    expect(warns(prompt, ExtensionStagingRestoreWarning::SharedSettingsFileRestore),
           "an mcp restore lost its mandatory shared settings file warning");
    expect(prompt.sharedFileOverwriteNote.contains(
               QStringLiteral("整个共享设置文件"))
               && prompt.sharedFileOverwriteNote.contains(
                   QStringLiteral("其他服务器")),
           "the shared-file warning does not state whole-file semantics");
    expect(prompt.treeIdentity.startsWith(
               QStringLiteral("mcp-backup-content:sha256:")),
           "the mcp restore rendered a foreign tree identity prefix");
    expect(prompt.fileWriteCount == 1
               && prompt.totalBytes == settings.size(),
           "the mcp restore counts do not match the whole-file plan");
}

// already-in-place 计划：警告如实报告"无需写入"，统计如实。
void testAlreadyInPlaceTruthful()
{
    const QByteArray alpha = QByteArrayLiteral("alpha\n");
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("a.txt"), false, alpha});
    const ConfigurationBackupSnapshot built =
        buildOrDie(skillDomain(), tree, kSkillSubject, 3);
    if (built.files.isEmpty()) return;

    FakeObservation observation = emptyDestination(
        QStringLiteral("/nonexistent/dest"));
    observation.nodes.insert(QStringLiteral("a.txt"),
                             {FakeObservation::NodeKind::File, alpha});
    ExtensionStagingRestorePlan plan;
    QString error;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    skillDomain(), kSkillSubject, built, observation.root,
                    &observation, &plan, &error),
                "an identical-content destination was refused")) {
        return;
    }
    const ExtensionStagingRestorePrompt prompt =
        ExtensionStagingRestorePresentation::build(
            plan, descriptorFor(kSkillSubject, 3), observation.root,
            fixtureNow());
    if (!expect(prompt.state == ExtensionStagingRestorePromptState::Ready,
                "an already-in-place plan was not presentable")) {
        return;
    }
    expect(warns(prompt, ExtensionStagingRestoreWarning::AlreadyInPlaceFiles)
               && warns(prompt, ExtensionStagingRestoreWarning::DestinationNotEmpty),
           "an already-in-place restore did not warn truthfully");
    expect(prompt.fileWriteCount == 0 && prompt.alreadyInPlaceCount == 1
               && prompt.totalBytes == alpha.size(),
           "the already-in-place counts are not truthful");
    expect(prompt.entries.size() == 1 && prompt.entries.at(0).alreadyInPlace
               && prompt.entries.at(0).sha256 == sha256Hex(alpha),
           "the already-in-place row lost its expected digest");
}

// 截断：超过上限的清单渲染显式标记，身份回声仍绑定完整计划，绑定声明在场。
void testTruncationBindsCompletePlan()
{
    const int total = ExtensionStagingRestorePresentation::MaxListedEntries + 4;
    const ExtensionStagingRestorePlan plan = handPlan(
        kSkillSubject, QStringLiteral("extension-content:sha256:"), total);
    const ExtensionStagingRestorePrompt prompt =
        ExtensionStagingRestorePresentation::build(
            plan, descriptorFor(kSkillSubject, 4), plan.destinationRoot,
            fixtureNow());
    if (!expect(prompt.state == ExtensionStagingRestorePromptState::Ready,
                "an over-cap plan was not presentable")) {
        QTextStream(stderr) << "  presentation said: " << prompt.errorCode
                            << '\n';
        return;
    }
    expect(prompt.entries.size()
               == ExtensionStagingRestorePresentation::MaxListedEntries
               && prompt.listingTruncated && prompt.omittedEntryCount == 4,
           "the listing was not bounded with an explicit omitted count");
    expect(prompt.truncationNote.contains(QStringLiteral("4")),
           "the truncation marker does not state the omitted count");
    expect(prompt.echoedPlanIdentity == plan.planIdentity
               && !prompt.echoedPlanIdentity.isEmpty(),
           "truncation dropped the echoed plan identity");
    expect(prompt.identityBindingNote.contains(
               QString::number(total))
               && prompt.identityBindingNote.contains(
                   QStringLiteral("未列出")),
           "the binding statement does not cover the unlisted entries");
    expect(prompt.approvable, "a truncated listing is not approvable");
}

// 不可展示输入：展示不安全的文本与内部不一致各自独立代号，整体拒绝而不是清洗。
void testUnpresentableInputs()
{
    const QString prefix =
        QStringLiteral("extension-restore-presentation-");
    // 双向控制字符路径：捕获层会拒绝它，因此手工构造计划来驱动这条防线。
    {
        ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        plan.operations[0].relativePath =
            QStringLiteral("a") + QChar(0x202e) + QStringLiteral("txt");
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptorFor(kSkillSubject, 5), plan.destinationRoot,
                fixtureNow());
        expect(prompt.state == ExtensionStagingRestorePromptState::Unpresentable
                   && prompt.errorCode
                       == prefix + QStringLiteral("entry-path-unsafe")
                   && !prompt.approvable,
               "a bidi path was not refused with the entry-path code");
    }
    // 计划身份畸形：无法与任何内容对齐。
    {
        ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        plan.planIdentity = QStringLiteral("extension-staging-restore-plan:sha256:abcd");
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptorFor(kSkillSubject, 5), plan.destinationRoot,
                fixtureNow());
        expect(prompt.errorCode == prefix + QStringLiteral("plan-identity-invalid"),
               "a malformed plan identity was rendered");
    }
    // 描述与计划主体不符：这份计划不是从所描述的备份构建的。
    {
        const ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptorFor(QStringLiteral("skill:other"), 5),
                plan.destinationRoot, fixtureNow());
        expect(prompt.errorCode == prefix + QStringLiteral("descriptor-mismatch"),
               "a descriptor/plan subject mismatch was rendered");
    }
    // 清点状态不是完整验证通过：无法诚实说明正在恢复哪一份备份。
    {
        const ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        ExtensionStagingRestoreBackupDescriptor descriptor =
            descriptorFor(kSkillSubject, 5);
        descriptor.verification =
            ExtensionStagingBackupEntryVerification::ListedCorrupt;
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptor, plan.destinationRoot, fixtureNow());
        expect(prompt.errorCode == prefix + QStringLiteral("descriptor-corrupt"),
               "a corrupt-listed backup was rendered as restorable");
    }
    // 目标根漂移：调用方给出的目标与计划绑定的目标不同。
    {
        const ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptorFor(kSkillSubject, 5),
                QStringLiteral("/elsewhere"), fixtureNow());
        expect(prompt.errorCode == prefix + QStringLiteral("destination-mismatch"),
               "a drifted destination root was rendered");
    }
    // 目录操作排在文件之后：计划契约的内部不一致。
    {
        ExtensionStagingRestorePlan plan = handPlan(
            kSkillSubject, QStringLiteral("extension-content:sha256:"), 1);
        ExtensionStagingRestoreOperation directory;
        directory.directory = true;
        directory.relativePath = QStringLiteral("late");
        plan.operations.append(directory);
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::build(
                plan, descriptorFor(kSkillSubject, 5), plan.destinationRoot,
                fixtureNow());
        expect(prompt.errorCode == prefix + QStringLiteral("operations-unordered"),
               "a file-before-directory plan was rendered");
    }
    // 不可安全展示的拒绝理由：整体拒绝而不是清洗。
    {
        const ExtensionStagingRestorePrompt prompt =
            ExtensionStagingRestorePresentation::buildRefusal(
                QStringLiteral("bad") + QChar(0x200b));
        expect(prompt.state == ExtensionStagingRestorePromptState::Unpresentable
                   && prompt.errorCode
                       == prefix + QStringLiteral("refusal-invalid"),
               "an unsafe refusal code was sanitized instead of refused");
    }
}

// 构建失败的计划：渲染拒绝理由，绝不渲染计划摘要，没有可批准标记。
void testRefusalRendering()
{
    const ExtensionStagingRestorePrompt prompt =
        ExtensionStagingRestorePresentation::buildRefusal(
            QStringLiteral("extension-staging-restore-destination-conflict"));
    expect(prompt.state == ExtensionStagingRestorePromptState::Refused,
           "a failed plan was not rendered as a refusal");
    expect(prompt.refusalReason
               == QStringLiteral("extension-staging-restore-destination-conflict"),
           "the refusal reason was not forwarded verbatim");
    expect(!prompt.approvable,
           "a refused plan carried an approvable affordance");
    expect(prompt.planIdentity.isEmpty() && prompt.echoedPlanIdentity.isEmpty()
               && prompt.entries.isEmpty() && prompt.fileWriteCount == 0
               && prompt.directoryCount == 0,
           "a refused plan rendered a plan summary");
    expect(warns(prompt, ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet)
               && !prompt.doesNotExecuteNote.isEmpty(),
           "the refusal state dropped the does-not-execute disclosure");
    expect(prompt.errorCode.isEmpty(),
           "a cleanly rendered refusal carried an error code");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testFullChainRendersExactly();
    testMcpSharedFileWarning();
    testAlreadyInPlaceTruthful();
    testTruncationBindsCompletePlan();
    testUnpresentableInputs();
    testRefusalRendering();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore presentation guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
