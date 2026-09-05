#include "extension_staging_backup_capture.h"

#include "extension_staging_restore_plan.h"
#include "extension_staging_snapshot.h"
#include "mcp_configuration_inventory.h"

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
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool written = file.write(bytes) == bytes.size();
    file.close();
    return written;
}

// 独立于产品代码的域字面量副本:如果 McpConfigurationInventory::backupCaptureDomain()
// 的字节漂移了,这里算出的身份与捕获结果对不上,测试立刻失败。这正是共享树捕获层
// 使用的交叉核对方式。
ExtensionTreeCaptureDomain fixtureMcpBackupDomain()
{
    return {QByteArrayLiteral("aegisy-mcp-config-backup-content/0.1\0"),
            QStringLiteral("mcp-backup-content:sha256:"),
            QStringLiteral("mcp-backup")};
}

class FixedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *key, QString *) override
    {
        if (!key) return false;
        *key = QByteArray(32, 'm');
        return true;
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

// 一份真实的设置文件:mcpServers 里有多个服务器,外加与 MCP 完全无关的键——
// 整文件诚实性必须连这些无关键一起保住。
const QByteArray &settingsBytes()
{
    static const QByteArray bytes = QByteArrayLiteral(
        "{\n"
        "  \"theme\": \"dark\",\n"
        "  \"mcpServers\": {\n"
        "    \"my-server\": {\n"
        "      \"command\": \"npx\",\n"
        "      \"args\": [\"-y\", \"@example/server\"],\n"
        "      \"env\": {\"TOKEN\": \"abc\"}\n"
        "    },\n"
        "    \"other-server\": {\"url\": \"https://example.com/mcp\"}\n"
        "  },\n"
        "  \"unrelated\": {\"nested\": [1, 2, 3]}\n"
        "}\n");
    return bytes;
}

bool captureOrCode(const QString &subject, const QString &sourcePath,
                   const QString &backupRoot,
                   ConfigurationBackupKeyProvider *provider,
                   ExtensionStagingBackupCaptureResult *result, QString *error)
{
    return ExtensionStagingBackupCapture::capture(subject, sourcePath,
                                                  backupRoot, provider, result,
                                                  error);
}

void expectCaptureRefused(const QString &subject, const QString &sourcePath,
                          const QString &backupRoot,
                          ConfigurationBackupKeyProvider *provider,
                          const QString &expectedCode, const char *message)
{
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(!captureOrCode(subject, sourcePath, backupRoot, provider,
                               &result, &error),
                message)) {
        return;
    }
    if (!expect(error == expectedCode, message)) {
        QTextStream(stderr) << "  expected " << expectedCode << " but got "
                            << error << '\n';
    }
    expect(result.backupId.isEmpty() && result.treeIdentity.isEmpty()
               && result.manifestIdentity.isEmpty()
               && !result.coversSharedSettingsFile,
           "a refused capture still produced a partial result");
}

// 端到端契约链:真实临时设置文件 → mcp: 主体捕获 → 暂存域加密往返 → 快照验证 →
// 清单恰好一个 `settings.json` 文件条目 → 对空目标的恢复计划。每一环都是未来真实
// 调用方会走的路。
void testMcpWholeFileRoundTrip()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString settings = temporary.path() + QStringLiteral("/claude/settings.json");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeFile(settings, settingsBytes()),
                "the settings fixture could not be written")) {
        return;
    }
    const QString subject = QStringLiteral("mcp:my-server");
    FixedKeyProvider provider;
    ExtensionStagingBackupCaptureResult result;
    QString error;
    if (!expect(captureOrCode(subject, settings, backupRoot, &provider, &result,
                              &error),
                "a clean settings file could not be captured")) {
        QTextStream(stderr) << "  capture said: " << error << '\n';
        return;
    }
    expect(result.subject == subject,
           "the capture result does not bind the mcp subject");
    expect(result.treeIdentity.startsWith(
               QStringLiteral("mcp-backup-content:sha256:")),
           "the capture result does not report the mcp backup identity");
    expect(result.coversSharedSettingsFile,
           "an mcp capture did not flag the shared settings file semantics");
    expect(result.manifestIdentityKnown
               && result.manifestIdentityDiagnostic.isEmpty(),
           "the capture result does not report the manifest identity");
    expect(result.priorIdentity
                   == ExtensionStagingPriorIdentity::NoPriorBackup,
           "a first mcp capture did not report that no prior backup exists");

    // 身份交叉核对:测试侧独立域字面量 + 测试侧独立合成的单条目树,必须算出同一个
    // 身份——域字节或合成形状的漂移都会让这里失败。
    QVector<ExtensionTreeCaptureEntry> synthetic;
    ExtensionTreeCaptureEntry entry;
    entry.relativePath = QStringLiteral("settings.json");
    entry.directory = false;
    entry.bytes = settingsBytes();
    synthetic.append(entry);
    expect(ExtensionTreeCapture::contentIdentity(fixtureMcpBackupDomain(),
                                                 synthetic)
               == result.treeIdentity,
           "the mcp backup domain bytes or the synthetic tree shape drifted");

    // 未变化的再捕获(单一主体根):拿到新备份 id 且报告身份一致。
    ExtensionStagingBackupCaptureResult again;
    if (!expect(captureOrCode(subject, settings, backupRoot, &provider, &again,
                              &error),
                "the unchanged mcp re-capture failed")) {
        return;
    }
    expect(again.backupId != result.backupId
               && again.priorIdentity == ExtensionStagingPriorIdentity::Matched
               && again.priorIdentityDiagnostic.isEmpty()
               && again.treeIdentity == result.treeIdentity
               && again.coversSharedSettingsFile,
           "an unchanged mcp re-capture did not report the identity match");

    // 读回并验证:验证侧用测试自己的域字面量,与构建侧是两套常量。
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(subject, 0, {});
    if (!expect(inventory.state == ConfigurationBackupInventoryState::Ready
                    && inventory.entries.size() == 2,
                "the inventory does not show exactly the two backups")) {
        return;
    }
    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(subject, result.backupId, &readBack, &error),
                "the stored backup could not be read back")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!expect(ExtensionStagingSnapshot::verify(fixtureMcpBackupDomain(), subject,
                                                 readBack, &rebuilt, &error),
                "the stored backup did not verify")) {
        QTextStream(stderr) << "  verify said: " << error << '\n';
        return;
    }
    // 清单形状钉死:恰好一个文件条目,路径恒为字面量 settings.json。
    if (!expect(rebuilt.size() == 1 && !rebuilt.first().directory
                    && rebuilt.first().relativePath
                        == QStringLiteral("settings.json"),
                "the manifest does not hold exactly one settings.json entry")) {
        return;
    }
    // 整文件诚实性:备份字节与文件原始字节逐字节相等,包括与 MCP 无关的键。
    expect(rebuilt.first().bytes == settingsBytes(),
           "the backup bytes drifted from the exact file bytes");

    // 契约链最后一环:对第二个空的临时目录可以计划恢复。
    const QString destination = temporary.path() + QStringLiteral("/dest");
    if (!expect(QDir().mkpath(destination),
                "the destination fixture could not be created")) {
        return;
    }
    DiskObservation observation(destination);
    ExtensionStagingRestorePlan plan;
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    fixtureMcpBackupDomain(), subject, readBack,
                    observation.canonicalRoot(), &observation, &plan, &error),
                "a clean destination was refused")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }
    expect(plan.operations.size() == 1 && !plan.operations.at(0).directory
               && plan.operations.at(0).relativePath
                   == QStringLiteral("settings.json"),
           "the restore plan is not exactly the one settings.json write");
}

// 共享文件语义:两个不同的 mcp: 主体在同一个暂存根里独立捕获同一个文件。两份备份都
// 按 id 读回并验证通过,内容身份相同(内容身份只覆盖树,不含主体)。存储的按主体清点容忍
// 混合主体根:别人主体的完整备份先经完整验证再越出作用域,绝不退化结果——第二个主体的
// 捕获因此报告干净的 NoPriorBackup 与已知清单身份;既有主体在混合根里的再捕获同样报告
// 干净的 Matched。退化代号只为真正退化的存储保留。
void testSharedFileCapturedIndependentlyPerSubject()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString settings = temporary.path() + QStringLiteral("/settings.json");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeFile(settings, settingsBytes()),
                "the shared-file fixture could not be written")) {
        return;
    }
    FixedKeyProvider provider;
    QString error;
    ExtensionStagingBackupCaptureResult alpha;
    if (!expect(captureOrCode(QStringLiteral("mcp:my-server"), settings,
                              backupRoot, &provider, &alpha, &error),
                "the first subject's capture failed")) {
        return;
    }
    ExtensionStagingBackupCaptureResult beta;
    if (!expect(captureOrCode(QStringLiteral("mcp:other-server"), settings,
                              backupRoot, &provider, &beta, &error),
                "the second subject's capture failed")) {
        return;
    }
    expect(alpha.coversSharedSettingsFile && beta.coversSharedSettingsFile,
           "a shared-file capture did not flag the shared semantics");
    expect(alpha.treeIdentity == beta.treeIdentity,
           "two subjects over the same bytes got different tree identities");
    expect(alpha.backupId != beta.backupId,
           "two subjects share one backup id");
    // 混合主体根不再是退化理由:foreign-intact 备份经完整验证后越出作用域,第二个主体
    // 的捕获报告干净的 NoPriorBackup 与已知清单身份,而不是 manifest-identity-degraded。
    expect(alpha.manifestIdentityKnown
               && alpha.priorIdentity
                   == ExtensionStagingPriorIdentity::NoPriorBackup,
           "the first capture did not report a clean first-backup result");
    expect(beta.manifestIdentityKnown
               && beta.manifestIdentityDiagnostic.isEmpty()
               && beta.priorIdentity
                   == ExtensionStagingPriorIdentity::NoPriorBackup
               && beta.priorIdentityDiagnostic.isEmpty(),
           "a second subject's capture in a mixed root was not clean");

    // 既有主体在混合根里的再捕获:内容未变,报告干净的 Matched 与已知清单身份。
    ExtensionStagingBackupCaptureResult again;
    if (!expect(captureOrCode(QStringLiteral("mcp:my-server"), settings,
                              backupRoot, &provider, &again, &error),
                "the mixed-root re-capture failed")) {
        return;
    }
    expect(again.priorIdentity == ExtensionStagingPriorIdentity::Matched
               && again.priorIdentityDiagnostic.isEmpty()
               && again.manifestIdentityKnown
               && again.backupId != alpha.backupId,
           "a re-capture in a mixed root did not report a clean match");

    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    for (const ExtensionStagingBackupCaptureResult *result : {&alpha, &beta}) {
        ConfigurationBackupSnapshot readBack;
        if (!expect(store.read(result->subject, result->backupId, &readBack,
                               &error),
                    "a shared-file backup could not be read back")) {
            continue;
        }
        expect(ExtensionStagingSnapshot::verify(fixtureMcpBackupDomain(),
                                                result->subject, readBack,
                                                &error),
               "a shared-file backup does not verify under its own subject");
    }
}

// 文件级畸形各自独立诊断:缺失、目录冒充文件、超 1 MiB、符号链接,且任何一种都不
// 留下备份、不触碰备份根。缺失不是空备份——它是拒绝。
void testMcpSourceRefusalsAreDistinctAndLeaveNoBackup()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    const QString subject = QStringLiteral("mcp:my-server");
    FixedKeyProvider provider;

    // 缺失的设置文件。
    expectCaptureRefused(subject,
                         temporary.path() + QStringLiteral("/absent/settings.json"),
                         backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-mcp-source-missing"),
                         "a missing settings file was not refused");

    // 目录冒充文件。
    const QString directory = temporary.path() + QStringLiteral("/a-directory");
    if (!expect(QDir().mkpath(directory),
                "the directory fixture could not be created")) {
        return;
    }
    expectCaptureRefused(subject, directory, backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-mcp-source-invalid"),
                         "a directory as the settings file was not refused");

    // 超过清单的 1 MiB 上限(比捕获层 2 MiB 与暂存域 4 MiB 都紧,更紧的一侧获胜)。
    const QString oversized = temporary.path() + QStringLiteral("/oversized.json");
    if (!expect(writeFile(oversized, QByteArray(1024 * 1024 + 1, 'x')),
                "the oversize fixture could not be written")) {
        return;
    }
    expectCaptureRefused(
        subject, oversized, backupRoot, &provider,
        QStringLiteral("extension-staging-capture-mcp-source-oversized"),
        "an oversized settings file was not refused");

    // 符号链接在打开之前拒绝。
    const QString target = temporary.path() + QStringLiteral("/target.json");
    const QString link = temporary.path() + QStringLiteral("/link.json");
    if (!expect(writeFile(target, settingsBytes())
                    && QFile::link(target, link),
                "the symlink fixture could not be created")) {
        return;
    }
    expectCaptureRefused(subject, link, backupRoot, &provider,
                         QStringLiteral("extension-staging-capture-mcp-source-symlink"),
                         "a symlinked settings file was not refused");

    // 上述全部失败之后:备份根里一份备份都不该有,甚至根目录都不该被建立——所有
    // 拒绝都发生在存储写入之前。
    expect(!QFileInfo::exists(backupRoot),
           "a refused mcp capture still touched the backup root");
}

// codex-plugin 的拒绝保持原代号不变,即使给它一个完全合法的设置文件路径也一样:
// 它没有处于本应用权威内的可备份字节。
void testCodexPluginRefusalUnchanged()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString settings = temporary.path() + QStringLiteral("/settings.json");
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    if (!expect(writeFile(settings, settingsBytes()),
                "the codex-refusal fixture could not be written")) {
        return;
    }
    FixedKeyProvider provider;
    expectCaptureRefused(
        QStringLiteral("codex-plugin:example"), settings, backupRoot, &provider,
        QStringLiteral("extension-staging-capture-codex-plugin-without-tree-source"),
        "a codex-plugin subject was not refused with its pinned code");
    expect(!QFileInfo::exists(backupRoot),
           "a refused codex-plugin capture still touched the backup root");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testMcpWholeFileRoundTrip();
    testSharedFileCapturedIndependentlyPerSubject();
    testMcpSourceRefusalsAreDistinctAndLeaveNoBackup();
    testCodexPluginRefusalUnchanged();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging backup capture mcp guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
