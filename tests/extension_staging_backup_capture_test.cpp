#include "extension_staging_backup_capture.h"

#include "extension_staging_restore_plan.h"
#include "extension_staging_snapshot.h"
#include "skill_extension_inventory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

int failures = 0;

const QString kSubject = QStringLiteral("skill:example-skill");

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
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

// 独立于产品代码的域字面量副本：如果 SkillExtensionInventory::treeCaptureDomain()
// 的字节漂移了,这里算出的身份与捕获结果对不上,测试立刻失败。这正是共享树捕获层
// 抽取时使用的交叉核对方式。
ExtensionTreeCaptureDomain fixtureSkillDomain()
{
    return {QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
            QStringLiteral("extension-content:sha256:"),
            QStringLiteral("skill")};
}

class FixedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *key, QString *) override
    {
        if (!key) return false;
        *key = QByteArray(32, 'c');
        return true;
    }
};

class FailingKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *, QString *) override
    {
        return false;
    }
};

// 真实磁盘观察,只在测试临时目录上工作,只读。
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

// 一棵技能形状的树:清单、技能文档与一份子目录内容。
bool writeSkillTree(const QString &root)
{
    return writeFile(root + QStringLiteral("/SKILL.md"),
                     QByteArrayLiteral("# Example Skill\n"))
        && writeFile(root + QStringLiteral("/aegisy-skill.json"),
                     QByteArrayLiteral("{\"id\":\"example-skill\"}"))
        && writeFile(root + QStringLiteral("/sub/notes.txt"),
                     QByteArrayLiteral("notes\n"));
}

bool scan(const QString &root, QVector<ExtensionTreeCaptureEntry> *tree)
{
    ExtensionTreeCaptureBudget budget;
    ExtensionTreeCaptureError error;
    const QString canonical = QFileInfo(root).canonicalFilePath();
    return ExtensionTreeCapture::scanDirectory(
        fixtureSkillDomain(), canonical, canonical, QString(), 0, &budget, tree,
        &error);
}

bool captureOrCode(const QString &subject, const QString &sourceRoot,
                   const QString &backupRoot,
                   ConfigurationBackupKeyProvider *provider,
                   ExtensionStagingBackupCaptureResult *result, QString *error)
{
    return ExtensionStagingBackupCapture::capture(subject, sourceRoot,
                                                  backupRoot, provider, result,
                                                  error);
}

void expectCaptureRefused(const QString &subject, const QString &sourceRoot,
                          const QString &backupRoot,
                          ConfigurationBackupKeyProvider *provider,
                          const QString &expectedCode, const char *message)
{
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(!captureOrCode(subject, sourceRoot, backupRoot, provider,
                               &result, &error),
                message)) {
        return;
    }
    if (!expect(error == expectedCode, message)) {
        QTextStream(stderr) << "  expected " << expectedCode << " but got "
                            << error << '\n';
    }
    expect(result.backupId.isEmpty() && result.treeIdentity.isEmpty()
               && result.manifestIdentity.isEmpty(),
           "a refused capture still produced a partial result");
}

// 端到端契约链:真实临时目录的技能树 → 捕获 → 暂存域加密往返 → 快照验证 → 对空目标的
// 恢复计划。每一环都是未来真实调用方会走的路。
void testRoundTripContractsChain()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString source = temporary.path() + QStringLiteral("/tree");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeSkillTree(source),
                "the round-trip fixture could not be written")) {
        return;
    }
    FixedKeyProvider provider;
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(captureOrCode(kSubject, source, backupRoot, &provider, &result,
                              &error),
                "a clean skill tree could not be captured")) {
        QTextStream(stderr) << "  capture said: " << error << '\n';
        return;
    }
    expect(QRegularExpression(QStringLiteral("^ext_[0-9]{8}_[0-9]{6}_[0-9a-f]{8}$"))
                   .match(result.backupId).hasMatch(),
           "the assigned backup id does not match the staging grammar");
    expect(result.subject == kSubject,
           "the capture result does not bind the subject");
    expect(result.treeIdentity.startsWith(
               QStringLiteral("extension-content:sha256:")),
           "the capture result does not report the tree identity");
    expect(result.manifestIdentityKnown
               && result.manifestIdentity.startsWith(
                   QStringLiteral("extension-staging-backup-manifest:sha256:"))
               && result.manifestIdentityDiagnostic.isEmpty(),
           "the capture result does not report the manifest identity");
    expect(result.priorIdentity
                   == ExtensionStagingPriorIdentity::NoPriorBackup
               && result.priorIdentityDiagnostic.isEmpty(),
           "a first capture did not report that no prior backup exists");

    // 身份字节交叉核对:测试侧独立的域字面量必须对同一棵树算出同一个身份。
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(source, &tree), "the round-trip fixture rescan failed")) {
        return;
    }
    expect(ExtensionTreeCapture::contentIdentity(fixtureSkillDomain(), tree)
               == result.treeIdentity,
           "the skill capture domain bytes drifted from the inventory domain");

    // 清点必须恰好看到这一份,而且清单身份一致。
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(kSubject, 0, {});
    if (!expect(inventory.state == ConfigurationBackupInventoryState::Ready
                    && inventory.entries.size() == 1,
                "the inventory does not show exactly the new backup")) {
        return;
    }
    expect(inventory.entries.first().backupId == result.backupId
               && inventory.entries.first().identity
                   == result.manifestIdentity,
           "the inventory entry does not match the capture result");

    // 读回并验证:验证侧用测试自己的域字面量,与构建侧是两套常量。
    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSubject, result.backupId, &readBack, &error),
                "the stored backup could not be read back")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!expect(ExtensionStagingSnapshot::verify(fixtureSkillDomain(), kSubject,
                                                 readBack, &rebuilt, &error),
                "the stored backup did not verify")) {
        QTextStream(stderr) << "  verify said: " << error << '\n';
        return;
    }

    // 契约链最后一环:对第二个空的临时目录可以计划恢复。
    const QString destination = temporary.path() + QStringLiteral("/dest");
    if (!expect(QDir().mkpath(destination),
                "the destination fixture could not be created")) {
        return;
    }
    DiskObservation observation(destination);
    ExtensionStagingRestorePlan plan;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    fixtureSkillDomain(), kSubject, readBack,
                    observation.canonicalRoot(), &observation, &plan, &error),
                "a clean destination was refused")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }
    // 目录 sub 在前,三个文件在后。
    expect(plan.operations.size() == 4 && plan.operations.at(0).directory
               && plan.operations.at(0).relativePath == QStringLiteral("sub"),
           "the restore plan does not match the captured tree");
    expect(plan.subject == kSubject && plan.treeIdentity == result.treeIdentity
               && !plan.planIdentity.isEmpty(),
           "the plan identity does not bind the captured tree");
}

// 主体语法先于一切文件系统工作:畸形主体连金丝雀路径都不该被碰。
void testSubjectGrammarRefusedBeforeFilesystemWork()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString canarySource =
        temporary.path() + QStringLiteral("/never-touched-source");
    const QString canaryBackups =
        temporary.path() + QStringLiteral("/never-touched-backups");
    FixedKeyProvider provider;
    for (const QString &subject : {
             QStringLiteral("SKILL:example"),
             QStringLiteral("skill:"),
             QStringLiteral("skill:Bad Id"),
             QStringLiteral("skill:-leading-dash"),
             QStringLiteral("plugin:example"),
             QStringLiteral("codex-plugin:"),
             QStringLiteral("skill:example:extra"),
             QString()}) {
        expectCaptureRefused(
            subject, canarySource, canaryBackups, &provider,
            QStringLiteral("extension-staging-capture-subject-invalid"),
            "a malformed subject was not refused before filesystem work");
    }
    expect(!QFileInfo::exists(canarySource)
               && !QFileInfo::exists(canaryBackups),
           "a malformed subject still touched the canary paths");
}

// Codex 插件与 MCP 不是这一层可以捕获的树:各自独立诊断,且与捕获层失败不同名。
void testKindsWithoutTreeSourceAreRefusedDistinctly()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString source = temporary.path() + QStringLiteral("/tree");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeSkillTree(source),
                "the kind-refusal fixture could not be written")) {
        return;
    }
    FixedKeyProvider provider;
    const QString codexCode =
        QStringLiteral("extension-staging-capture-codex-plugin-without-tree-source");
    const QString mcpCode =
        QStringLiteral("extension-staging-capture-mcp-without-tree-source");
    expectCaptureRefused(QStringLiteral("codex-plugin:example"), source,
                         backupRoot, &provider, codexCode,
                         "a codex-plugin subject was not refused");
    expectCaptureRefused(QStringLiteral("mcp:example"), source, backupRoot,
                         &provider, mcpCode,
                         "an mcp subject was not refused");
    expect(codexCode != mcpCode,
           "the two kind refusals share one diagnostic code");
    // 拒绝发生在捕获之前:备份根甚至不该被建立。
    expect(!QFileInfo::exists(backupRoot),
           "a refused kind still touched the backup root");
}

// 再捕获:未变化的树报告身份一致,变了的树报告不一致,两者都拿到新的备份 id。
void testRecaptureReportsIdentityMatch()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString source = temporary.path() + QStringLiteral("/tree");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeSkillTree(source),
                "the re-capture fixture could not be written")) {
        return;
    }
    FixedKeyProvider provider;
    ExtensionStagingBackupCaptureResult first;
    QString error;
    if (!expect(captureOrCode(kSubject, source, backupRoot, &provider, &first,
                              &error),
                "the first capture failed")) {
        return;
    }
    ExtensionStagingBackupCaptureResult unchanged;
    if (!expect(captureOrCode(kSubject, source, backupRoot, &provider,
                              &unchanged, &error),
                "the unchanged re-capture failed")) {
        return;
    }
    expect(unchanged.backupId != first.backupId,
           "an unchanged re-capture did not get a fresh backup id");
    expect(unchanged.priorIdentity == ExtensionStagingPriorIdentity::Matched
               && unchanged.priorIdentityDiagnostic.isEmpty()
               && unchanged.treeIdentity == first.treeIdentity,
           "an unchanged re-capture did not report the identity match");
    expect(unchanged.manifestIdentityKnown
               && unchanged.manifestIdentity != first.manifestIdentity,
           "an unchanged re-capture shares the first backup's manifest identity");

    if (!expect(writeFile(source + QStringLiteral("/sub/notes.txt"),
                          QByteArrayLiteral("changed\n")),
                "the changed fixture could not be written")) {
        return;
    }
    ExtensionStagingBackupCaptureResult changed;
    if (!expect(captureOrCode(kSubject, source, backupRoot, &provider, &changed,
                              &error),
                "the changed re-capture failed")) {
        return;
    }
    expect(changed.priorIdentity == ExtensionStagingPriorIdentity::Mismatched
               && changed.treeIdentity != first.treeIdentity,
           "a changed tree did not report the identity mismatch");
    // 比对只对最近一次备份:现在最近的是未变化的那一份,而它与第一份同树。
    expect(changed.treeIdentity != unchanged.treeIdentity,
           "a changed tree kept the old tree identity");
}

// 清点退化不得静默变成"没有既有备份":在备份根里放一份外域清单,清点判定 Invalid,
// 捕获仍然写入(清点只是建议性输入),但结果必须显式报告比对与清单身份都不可知。
void testDegradedInventoryIsNotSilent()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString source = temporary.path() + QStringLiteral("/tree");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeSkillTree(source),
                "the degraded fixture could not be written")) {
        return;
    }
    const QString foreign = backupRoot + QStringLiteral("/ext_20260901_000000_00000000");
    if (!expect(writeFile(foreign + QStringLiteral("/manifest.json"),
                          QByteArrayLiteral("{\"format\":\"someone-elses-format\"}")),
                "the foreign manifest could not be planted")) {
        return;
    }
    FixedKeyProvider provider;
    {
        ConfigurationBackupStore probe(
            ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
            &provider);
        expect(probe.inventory(kSubject, 0, {}).state
                   == ConfigurationBackupInventoryState::Invalid,
               "the planted foreign manifest did not degrade the inventory");
    }
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(captureOrCode(kSubject, source, backupRoot, &provider, &result,
                              &error),
                "a degraded inventory blocked the backup write")) {
        QTextStream(stderr) << "  capture said: " << error << '\n';
        return;
    }
    expect(result.priorIdentity == ExtensionStagingPriorIdentity::Unknown
               && result.priorIdentityDiagnostic
                   == QStringLiteral(
                       "extension-staging-capture-prior-identity-degraded"),
           "a degraded inventory silently became 'no prior backup'");
    expect(!result.manifestIdentityKnown
               && result.manifestIdentity.isEmpty()
               && result.manifestIdentityDiagnostic
                   == QStringLiteral(
                       "extension-staging-capture-manifest-identity-degraded"),
           "a degraded inventory still claimed a manifest identity");
    // 备份本身完整在盘上:绕过清点按 id 直接读回并验证。
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot readBack;
    expect(store.read(kSubject, result.backupId, &readBack, &error)
               && ExtensionStagingSnapshot::verify(fixtureSkillDomain(),
                                                   kSubject, readBack, &error),
           "the backup written past a degraded inventory does not verify");
}

// 捕获层失败逐字透传且不留下任何备份;来源根纪律失败用本层独立诊断。
void testCaptureFailuresPropagateAndLeaveNoBackup()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;

    // 树内符号链接:捕获层原代号透传。
    const QString symlinkTree = temporary.path() + QStringLiteral("/symlink-tree");
    if (!expect(writeFile(symlinkTree + QStringLiteral("/SKILL.md"),
                          QByteArrayLiteral("# S\n"))
                    && writeFile(temporary.path() + QStringLiteral("/target.txt"),
                                 QByteArrayLiteral("t")),
                "the symlink fixture could not be written")) {
        return;
    }
    if (!expect(QFile::link(temporary.path() + QStringLiteral("/target.txt"),
                            symlinkTree + QStringLiteral("/link.txt")),
                "the symlink fixture could not be linked")) {
        return;
    }
    expectCaptureRefused(kSubject, symlinkTree, backupRoot, &provider,
                         QStringLiteral("skill-symlink-invalid"),
                         "a symlink inside the tree was not refused");

    // 超过捕获层单文件上限:原代号透传。
    const QString oversizeTree = temporary.path() + QStringLiteral("/big-tree");
    if (!expect(writeFile(oversizeTree + QStringLiteral("/big.bin"),
                          QByteArray(2 * 1024 * 1024 + 1, 'x')),
                "the oversize fixture could not be written")) {
        return;
    }
    expectCaptureRefused(kSubject, oversizeTree, backupRoot, &provider,
                         QStringLiteral("skill-file-oversized"),
                         "an oversize tree was not refused");

    // 来源根本身是符号链接:必须在规范化之前由本层拒绝。
    const QString linkRoot = temporary.path() + QStringLiteral("/root-link");
    if (!expect(QFile::link(symlinkTree, linkRoot),
                "the root symlink fixture could not be linked")) {
        return;
    }
    expectCaptureRefused(kSubject, linkRoot, backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-root-symlink"),
                         "a symlink source root was not refused");

    // 不存在的来源根。
    expectCaptureRefused(kSubject,
                         temporary.path() + QStringLiteral("/missing"),
                         backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-root-unavailable"),
                         "a missing source root was not refused");

    // 空来源根与空备份根:独立诊断。
    expectCaptureRefused(kSubject, QString(), backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-root-invalid"),
                         "an empty source root was not refused");
    expectCaptureRefused(kSubject, symlinkTree, QString(), &provider,
                         QStringLiteral("extension-staging-capture-request-invalid"),
                         "an empty backup root was not refused");

    // 密钥不可用:存储的原代号透传,而且不留下半份备份。
    const QString keyTree = temporary.path() + QStringLiteral("/key-tree");
    if (!expect(writeSkillTree(keyTree),
                "the key-failure fixture could not be written")) {
        return;
    }
    FailingKeyProvider failing;
    expectCaptureRefused(kSubject, keyTree, backupRoot, &failing,
                         QStringLiteral("extension-staging-backup-key-unavailable"),
                         "a key failure did not surface the store diagnostic");

    // 上述全部失败之后,备份根里一份备份都不该有。存储 create 是备份目录级原子的,
    // 这里用清点证明失败路径没有残留。
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    expect(store.inventory(kSubject, 0, {}).state
               == ConfigurationBackupInventoryState::Empty,
           "a refused capture left a partial backup behind");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testRoundTripContractsChain();
    testSubjectGrammarRefusedBeforeFilesystemWork();
    testKindsWithoutTreeSourceAreRefusedDistinctly();
    testRecaptureReportsIdentityMatch();
    testDegradedInventoryIsNotSilent();
    testCaptureFailuresPropagateAndLeaveNoBackup();
    if (failures == 0) {
        QTextStream(stdout) << "extension staging backup capture guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
