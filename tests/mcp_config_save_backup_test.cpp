#include "mcp_config_dialog.h"

#include "configuration_backup_store.h"
#include "extension_staging_snapshot.h"
#include "mcp_configuration_inventory.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include <optional>

class McpConfigDialogTestAccess
{
public:
    static bool sourceValid(const McpConfigDialog &dialog)
    {
        return dialog.m_sourceValid;
    }
    static bool save(McpConfigDialog &dialog) { return dialog.saveToSettings(); }
    static void setServers(McpConfigDialog &dialog, const QJsonObject &servers)
    {
        dialog.m_mcpServers = servers;
    }
    static QString lastSaveError(const McpConfigDialog &dialog)
    {
        return dialog.m_lastSaveError;
    }
    static void setAfterBackupCaptureHook(McpConfigDialog &dialog,
                                          std::function<void()> hook)
    {
        dialog.m_afterBackupCaptureHook = std::move(hook);
    }
};

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

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const QString kSubject = QStringLiteral("mcp:claude-settings");

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

class FailingKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *, QString *error) override
    {
        if (error) *error = QStringLiteral("extension-staging-backup-key-unavailable");
        return false;
    }
};

// 从暂存域读回该主体的唯一备份并完整验证，返回重建树里 `settings.json` 条目的字节。
// 任何一步失败都返回 nullopt——测试据此断言"备份真实存在且可被验证"。
std::optional<QByteArray> verifiedSingleBackupBytes(
        const QString &backupRoot, ConfigurationBackupKeyProvider *provider)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot, provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(kSubject, 0, {});
    if (inventory.state != ConfigurationBackupInventoryState::Ready
            || inventory.entries.size() != 1) {
        return std::nullopt;
    }
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!store.read(kSubject, inventory.entries.first().backupId, &snapshot,
                    &error)) {
        return std::nullopt;
    }
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!ExtensionStagingSnapshot::verify(
            McpConfigurationInventory::backupCaptureDomain(), kSubject, snapshot,
            &rebuilt, &error)) {
        return std::nullopt;
    }
    if (rebuilt.size() != 1 || rebuilt.first().directory
            || rebuilt.first().relativePath != QStringLiteral("settings.json")) {
        return std::nullopt;
    }
    return rebuilt.first().bytes;
}

// 保存成功时，暂存域里必须有一份经验证的备份，其字节与保存前的文件原文逐字节相等
// （整文件诚实性：含与 MCP 无关的键）。
void testSaveCapturesVerifiedBackup()
{
    QTemporaryDir configHome;
    QTemporaryDir backupHome;
    if (!expect(configHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
    const QString path = QDir(configHome.path()).filePath(
        QStringLiteral(".claude/settings.json"));
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QByteArray preSave = QByteArrayLiteral(
        "{\"other\":true,\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    if (!expect(writeBytes(path, preSave), "fixture write failed")) return;

    FixedKeyProvider provider;
    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("url"), QStringLiteral("https://example.com/sse")}}}});
    expect(McpConfigDialogTestAccess::save(dialog),
           "a wired valid save was refused");
    const QByteArray saved = readBytes(path);
    expect(saved.contains("\"two\"") && saved.contains("\"other\": true"),
           "the wired save lost the new server or non-MCP data");

    const std::optional<QByteArray> backupBytes =
        verifiedSingleBackupBytes(backupRoot, &provider);
    expect(backupBytes.has_value(),
           "the save did not leave exactly one verified staging backup");
    expect(backupBytes.has_value() && *backupBytes == preSave,
           "the staging backup bytes differ from the pre-save file");
}

// 备份存储不可用（密钥来源失败）即拒绝保存：与激活路径"无法创建可验证安全备份则
// 未修改配置"的 fail-closed 先例一致；文件原字节不动，原因如实可见，来源不被误判
// 为损坏（不是冻结，是可重试的失败）。
void testBackupFailureBlocksSave()
{
    QTemporaryDir configHome;
    QTemporaryDir backupHome;
    if (!expect(configHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
    const QString path = QDir(configHome.path()).filePath(
        QStringLiteral(".claude/settings.json"));
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QByteArray preSave = QByteArrayLiteral(
        "{\"other\":true,\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    if (!expect(writeBytes(path, preSave), "fixture write failed")) return;

    FailingKeyProvider provider;
    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    expect(!McpConfigDialogTestAccess::save(dialog),
           "a save proceeded even though the staging backup failed");
    expect(readBytes(path) == preSave,
           "the settings file changed even though the save was refused");
    const QString reason = McpConfigDialogTestAccess::lastSaveError(dialog);
    expect(reason.contains(QStringLiteral("保存前备份失败"))
               && reason.contains(
                   QStringLiteral("extension-staging-backup-key-unavailable")),
           "the refusal did not surface the truthful backup failure reason");
    expect(McpConfigDialogTestAccess::sourceValid(dialog),
           "a backup failure was misreported as source corruption");
    FixedKeyProvider verification;
    expect(!verifiedSingleBackupBytes(backupRoot, &verification).has_value(),
           "a refused save still left a readable staging backup");
}

// 损坏与不可用来源保持冻结：保存被拒绝且绝不尝试备份。
void testFrozenSourcesAttemptNoBackup()
{
    // Invalid：非法 JSON。
    {
        QTemporaryDir configHome;
        QTemporaryDir backupHome;
        if (!expect(configHome.isValid() && backupHome.isValid(),
                    "temporary directories unavailable")) return;
        qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
        const QString path = QDir(configHome.path()).filePath(
            QStringLiteral(".claude/settings.json"));
        const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
        const QByteArray invalid = QByteArrayLiteral("{invalid-json");
        if (!expect(writeBytes(path, invalid), "fixture write failed")) return;

        FixedKeyProvider provider;
        McpConfigDialog dialog(&provider, backupRoot);
        expect(!McpConfigDialogTestAccess::sourceValid(dialog),
               "an invalid source was not frozen");
        expect(!McpConfigDialogTestAccess::save(dialog),
               "a frozen invalid source saved");
        expect(readBytes(path) == invalid,
               "a frozen invalid source was overwritten");
        expect(!QFileInfo::exists(backupRoot),
               "a frozen invalid source attempted a backup");
    }
    // Unavailable：文件存在但不可读。
    {
        QTemporaryDir configHome;
        QTemporaryDir backupHome;
        if (!expect(configHome.isValid() && backupHome.isValid(),
                    "temporary directories unavailable")) return;
        qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
        const QString path = QDir(configHome.path()).filePath(
            QStringLiteral(".claude/settings.json"));
        const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
        const QByteArray bytes = QByteArrayLiteral("{\"mcpServers\":{}}");
        if (!expect(writeBytes(path, bytes), "fixture write failed")) return;
        if (!expect(QFile::setPermissions(path, QFileDevice::Permissions()),
                    "could not strip fixture permissions")) return;

        FixedKeyProvider provider;
        McpConfigDialog dialog(&provider, backupRoot);
        expect(!McpConfigDialogTestAccess::sourceValid(dialog),
               "an unavailable source was not frozen");
        expect(!McpConfigDialogTestAccess::save(dialog),
               "a frozen unavailable source saved");
        expect(!QFileInfo::exists(backupRoot),
               "a frozen unavailable source attempted a backup");
        expect(QFile::setPermissions(
                   path, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
               "could not restore fixture permissions");
    }
}

// 捕获与写入之间文件被换掉（测试钩子在捕获完成后改写文件）：写入前的身份复查拒绝
// 保存，文件保持漂移后的内容不动；已捕获的备份仍是复查时刻真实字节的诚实备份。
void testDriftBetweenCaptureAndWriteRefused()
{
    QTemporaryDir configHome;
    QTemporaryDir backupHome;
    if (!expect(configHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
    const QString path = QDir(configHome.path()).filePath(
        QStringLiteral(".claude/settings.json"));
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QByteArray preSave = QByteArrayLiteral(
        "{\"other\":true,\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    const QByteArray drifted = QByteArrayLiteral(
        "{\"other\":\"drifted\",\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    if (!expect(writeBytes(path, preSave), "fixture write failed")) return;

    FixedKeyProvider provider;
    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    McpConfigDialogTestAccess::setAfterBackupCaptureHook(
        dialog, [&path, &drifted]() { writeBytes(path, drifted); });
    expect(!McpConfigDialogTestAccess::save(dialog),
           "a save proceeded even though the source drifted after the backup");
    expect(readBytes(path) == drifted,
           "the save overwrote the externally drifted file");
    expect(!McpConfigDialogTestAccess::sourceValid(dialog),
           "a post-backup drift did not freeze the source");
    const QString reason = McpConfigDialogTestAccess::lastSaveError(dialog);
    expect(reason.contains(QStringLiteral("备份后发生变化")),
           "the drift refusal did not surface a truthful reason");
    const std::optional<QByteArray> backupBytes =
        verifiedSingleBackupBytes(backupRoot, &provider);
    expect(backupBytes.has_value() && *backupBytes == preSave,
           "the captured backup does not hold the honest pre-drift bytes");
}

// 空来源（设置文件不存在）没有可丢失的字节：诚实跳过捕获，保存照常创建文件。
void testEmptySourceSkipsBackupAndSaves()
{
    QTemporaryDir configHome;
    QTemporaryDir backupHome;
    if (!expect(configHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    qputenv("AEGISY_CONFIG_HOME", configHome.path().toUtf8());
    const QString path = QDir(configHome.path()).filePath(
        QStringLiteral(".claude/settings.json"));
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");

    FixedKeyProvider provider;
    McpConfigDialog dialog(&provider, backupRoot);
    expect(McpConfigDialogTestAccess::sourceValid(dialog),
           "a missing settings file was not treated as an empty source");
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    expect(McpConfigDialogTestAccess::save(dialog),
           "an empty-source save was refused");
    expect(readBytes(path).contains("\"two\""),
           "the empty-source save did not create the settings file");
    expect(!QFileInfo::exists(backupRoot),
           "an empty source fabricated a backup of nothing");
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);

    testSaveCapturesVerifiedBackup();
    testBackupFailureBlocksSave();
    testFrozenSourcesAttemptNoBackup();
    testDriftBetweenCaptureAndWriteRefused();
    testEmptySourceSkipsBackupAndSaves();

    if (failures > 0) {
        QTextStream(stderr) << failures << " check(s) failed" << '\n';
        return 1;
    }
    return 0;
}
