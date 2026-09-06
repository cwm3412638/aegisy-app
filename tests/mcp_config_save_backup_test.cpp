#include "mcp_config_dialog.h"

#include "configuration_backup_store.h"
#include "extension_staging_backup_inventory.h"
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
    static QString lastRetentionNote(const McpConfigDialog &dialog)
    {
        return dialog.m_lastRetentionNote;
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
    // 未捕获即未修剪：备注保持为空,绝不对"什么都没做"说话。
    expect(McpConfigDialogTestAccess::lastRetentionNote(dialog).isEmpty(),
           "a save without a capture still carried a retention note");
}

// 为该主体直接经存储种一份合法暂存备份（过去时间戳,保证对话框保存产生的新鲜捕获
// 恒为最新）。与清点测试同形：夹具只需要合法的存储记录。
bool seedBackup(const QString &backupRoot,
                ConfigurationBackupKeyProvider *provider, int index)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        provider);
    ConfigurationBackupSnapshot snapshot;
    snapshot.backupId = QStringLiteral("ext_20260901_%1_%2")
        .arg(300000 + index, 6, 10, QLatin1Char('0'))
        .arg(0xeee0000 + index, 8, 16, QLatin1Char('0'));
    snapshot.tool = kSubject;
    snapshot.createdAt = QDateTime::fromString(
        QStringLiteral("2026-09-01T%1:%2:00.000Z")
            .arg(index / 60, 2, 10, QLatin1Char('0'))
            .arg(index % 60, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
    snapshot.files = {{ 0, true, QByteArrayLiteral("seeded-bytes") }};
    QString error;
    return store.create(snapshot, &error);
}

int stagedEntryCount(const QString &backupRoot,
                     ConfigurationBackupKeyProvider *provider)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(kSubject, 0, {});
    return inventory.state == ConfigurationBackupInventoryState::Ready
        ? inventory.entries.size() : -1;
}

// 捕获成功后超上限自动修剪到上限：种 32 份 + 保存捕获第 33 份 → 修剪最旧 1 份,清点
// 回到 32；最新鲜的捕获（本次保存前字节）无条件保留并可验证；修剪备注如实上屏。
void testSavePrunesOverLimitBackupsToTheDomainCap()
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
    for (int i = 0; i < 32; ++i) {
        if (!expect(seedBackup(backupRoot, &provider, i),
                    "a seeded backup could not be created")) return;
    }
    if (!expect(stagedEntryCount(backupRoot, &provider) == 32,
                "the seeded fixture did not reach the domain cap")) return;

    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    expect(McpConfigDialogTestAccess::save(dialog),
           "an over-limit save was refused");
    expect(McpConfigDialogTestAccess::lastSaveError(dialog).isEmpty(),
           "a successful save carried a save error");
    // 修剪备注如实区分结果：1 份超上限旧备份被修剪。
    const QString note = McpConfigDialogTestAccess::lastRetentionNote(dialog);
    expect(note.contains(QStringLiteral("已按保留上限修剪"))
               && note.contains(QStringLiteral("1 份")),
           "the over-limit prune was not honestly noted on the save result");
    expect(!note.contains(QStringLiteral("修剪失败"))
               && !note.contains(QStringLiteral("无需修剪")),
           "a successful prune was worded as a failure or a no-op");
    // 修剪到上限：33 - 1 = 32,最旧的种入备份消失。
    if (!expect(stagedEntryCount(backupRoot, &provider) == 32,
                "the store was not pruned back to the domain cap")) return;
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, kSubject, &listing, &error)
                    && listing.state
                        == ExtensionStagingBackupListState::Ready,
                "the post-prune listing was not Ready")) return;
    bool oldestSeedPresent = false;
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.backupId == QStringLiteral("ext_20260901_300000_0eee0000")) {
            oldestSeedPresent = true;
        }
    }
    expect(!oldestSeedPresent,
           "the oldest backup survived the over-limit prune");
    // newestVerifiedKept 语义在接线路径上成立：最新一份（本次保存前字节的捕获）无条件
    // 保留且可完整验证。
    const ExtensionStagingBackupListEntry &newest = listing.entries.first();
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot snapshot;
    if (!expect(store.read(kSubject, newest.backupId, &snapshot, &error),
                "the freshest capture did not survive the prune")) return;
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!expect(ExtensionStagingSnapshot::verify(
                    McpConfigurationInventory::backupCaptureDomain(), kSubject,
                    snapshot, &rebuilt, &error)
                    && rebuilt.size() == 1,
                "the freshest capture did not verify after the prune")) return;
    expect(rebuilt.first().bytes == preSave,
           "the retained newest backup lost the honest pre-save bytes");
}

// 退化清点：修剪失败绝不翻转已成功的捕获与保存——保存照常成功、捕获的备份完整在盘上、
// 零删除,备注如实携带诊断并明说"不受影响"。
void testDegradedPruneDoesNotFlipASuccessfulSave()
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
    if (!expect(seedBackup(backupRoot, &provider, 0),
                "the seeded backup could not be created")) return;

    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    // 捕获成功后、修剪前把备份根弄退化：修剪计划必须失败关闭、零删除。
    McpConfigDialogTestAccess::setAfterBackupCaptureHook(
        dialog, [&backupRoot]() {
            writeBytes(backupRoot + QStringLiteral("/stray.txt"),
                       QByteArrayLiteral("junk"));
        });
    expect(McpConfigDialogTestAccess::save(dialog),
           "a degraded prune flipped the successful save into a failure");
    expect(McpConfigDialogTestAccess::lastSaveError(dialog).isEmpty(),
           "a degraded prune surfaced as a save error");
    const QString note = McpConfigDialogTestAccess::lastRetentionNote(dialog);
    expect(note.contains(QStringLiteral("修剪未能执行"))
               && note.contains(QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid"))
               && note.contains(QStringLiteral("不受影响")),
           "a degraded prune was not honestly noted with its diagnostic");
    // 零删除：种入备份与新鲜捕获都还在盘上（退化清点无法安全计数,直接数目录）。
    const QStringList dirs = QDir(backupRoot).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    expect(dirs.size() == 2,
           "a degraded prune deleted backups");
    // 保存本身真实发生。
    expect(readBytes(path).contains("\"two\""),
           "the save did not land even though only the prune degraded");
}

// 未超限：捕获成功、无需修剪,备注如实说"无需修剪"而不是沉默或谎称修剪过。
void testSaveWithoutPruneNeedSaysSo()
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
        "{\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    if (!expect(writeBytes(path, preSave), "fixture write failed")) return;

    FixedKeyProvider provider;
    if (!expect(seedBackup(backupRoot, &provider, 0),
                "the seeded backup could not be created")) return;

    McpConfigDialog dialog(&provider, backupRoot);
    McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
        {QStringLiteral("two"), QJsonObject{
            {QStringLiteral("command"), QStringLiteral("npx")}}}});
    expect(McpConfigDialogTestAccess::save(dialog),
           "an under-limit save was refused");
    const QString note = McpConfigDialogTestAccess::lastRetentionNote(dialog);
    expect(note.contains(QStringLiteral("无需修剪")),
           "a no-op prune was not honestly worded as such");
    expect(stagedEntryCount(backupRoot, &provider) == 2,
           "a no-op prune deleted backups");
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
    testSavePrunesOverLimitBackupsToTheDomainCap();
    testDegradedPruneDoesNotFlipASuccessfulSave();
    testSaveWithoutPruneNeedSaysSo();

    if (failures > 0) {
        QTextStream(stderr) << failures << " check(s) failed" << '\n';
        return 1;
    }
    return 0;
}
