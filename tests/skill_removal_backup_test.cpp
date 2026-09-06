#include "skills_dialog.h"

#include "configuration_backup_store.h"
#include "extension_staging_backup_inventory.h"
#include "extension_staging_snapshot.h"
#include "skill_extension_inventory.h"
#include "skill_manager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include <optional>

class SkillsDialogTestAccess
{
public:
    static bool removeSkill(SkillsDialog &dialog, const QString &id)
    {
        return dialog.removeSkillWithBackup(id);
    }
    static QString lastDeleteError(const SkillsDialog &dialog)
    {
        return dialog.m_lastDeleteError;
    }
    static QString lastDeleteBackupId(const SkillsDialog &dialog)
    {
        return dialog.m_lastDeleteBackupId;
    }
    static QString lastRetentionNote(const SkillsDialog &dialog)
    {
        return dialog.m_lastRetentionNote;
    }
    static void setAfterBackupCaptureHook(SkillsDialog &dialog,
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

const QByteArray kSkillNotes = QByteArrayLiteral("skill-notes-under-test");

// 造一个真实的 Skill 目录夹具：SKILL.md + 清单 + 一个内容已知的附件。
bool createSkill(const QString &skillsRoot, const QString &id, bool builtin)
{
    const QString dir = skillsRoot + QLatin1Char('/') + id;
    if (!writeBytes(dir + QStringLiteral("/SKILL.md"),
                    QByteArrayLiteral("---\nname: Fixture Skill\n---\nbody"))) {
        return false;
    }
    if (!writeBytes(dir + QStringLiteral("/notes.txt"), kSkillNotes)) return false;
    const QJsonObject manifest{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), QStringLiteral("Fixture Skill ") + id},
        {QStringLiteral("executor"), QStringLiteral("instruction")},
        {QStringLiteral("builtin"), builtin},
    };
    return writeBytes(dir + QStringLiteral("/aegisy-skill.json"),
                      QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

QString skillSubject(const QString &id)
{
    return QStringLiteral("skill:") + id;
}

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

int stagedEntryCount(const QString &backupRoot,
                     ConfigurationBackupKeyProvider *provider,
                     const QString &subject)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(subject, 0, {});
    return inventory.state == ConfigurationBackupInventoryState::Ready
        ? inventory.entries.size() : -1;
}

// 从暂存域读回该主体的唯一备份并完整验证，返回重建树里 `notes.txt` 条目的字节。
// 任何一步失败都返回 nullopt——测试据此断言"备份真实存在且可被验证"。
std::optional<QByteArray> verifiedSingleBackupNotes(
        const QString &backupRoot, ConfigurationBackupKeyProvider *provider,
        const QString &subject)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot, provider);
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(subject, 0, {});
    if (inventory.state != ConfigurationBackupInventoryState::Ready
            || inventory.entries.size() != 1) {
        return std::nullopt;
    }
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!store.read(subject, inventory.entries.first().backupId, &snapshot,
                    &error)) {
        return std::nullopt;
    }
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!ExtensionStagingSnapshot::verify(
            SkillExtensionInventory::treeCaptureDomain(), subject, snapshot,
            &rebuilt, &error)) {
        return std::nullopt;
    }
    for (const ExtensionTreeCaptureEntry &entry : rebuilt) {
        if (!entry.directory
                && entry.relativePath == QStringLiteral("notes.txt")) {
            return entry.bytes;
        }
    }
    return std::nullopt;
}

// 为指定主体直接经存储种一份合法暂存备份（过去时间戳，保证删除产生的新鲜捕获恒为
// 最新）。与清点测试同形：夹具只需要合法的存储记录。
bool seedBackup(const QString &backupRoot,
                ConfigurationBackupKeyProvider *provider,
                const QString &subject, int index)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        provider);
    ConfigurationBackupSnapshot snapshot;
    snapshot.backupId = QStringLiteral("ext_20260901_%1_%2")
        .arg(300000 + index, 6, 10, QLatin1Char('0'))
        .arg(0xeee0000 + index, 8, 16, QLatin1Char('0'));
    snapshot.tool = subject;
    snapshot.createdAt = QDateTime::fromString(
        QStringLiteral("2026-09-01T%1:%2:00.000Z")
            .arg(index / 60, 2, 10, QLatin1Char('0'))
            .arg(index % 60, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
    snapshot.files = {{ 0, true, QByteArrayLiteral("seeded-bytes") }};
    QString error;
    return store.create(snapshot, &error);
}

// 删除前捕获成功且删除完成：暂存域里必须有一份经验证的备份，其字节与被删目录的原文
// 逐字节相等；旁观 Skill 的目录逐字不动。
void testRemovalCapturesVerifiedBackupAndDeletes()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    if (!expect(createSkill(skillsRoot, QStringLiteral("doomed-skill"), false)
                    && createSkill(skillsRoot, QStringLiteral("sibling-skill"),
                                   false),
                "fixture write failed")) return;
    const QByteArray siblingNotes = readBytes(
        skillsRoot + QStringLiteral("/sibling-skill/notes.txt"));

    FixedKeyProvider provider;
    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("doomed-skill")),
           "a wired valid removal was refused");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog).isEmpty(),
           "a successful removal carried a delete error");
    expect(!QFileInfo::exists(skillsRoot + QStringLiteral("/doomed-skill")),
           "the wired removal did not delete the skill directory");
    expect(readBytes(skillsRoot + QStringLiteral("/sibling-skill/notes.txt"))
               == siblingNotes,
           "the removal touched a bystander skill directory");

    const QString subject = skillSubject(QStringLiteral("doomed-skill"));
    const std::optional<QByteArray> backupBytes =
        verifiedSingleBackupNotes(backupRoot, &provider, subject);
    expect(backupBytes.has_value(),
           "the removal did not leave exactly one verified staging backup");
    expect(backupBytes.has_value() && *backupBytes == kSkillNotes,
           "the staging backup bytes differ from the pre-removal files");
    // 备份 id 如实回显，供对话框提示为回退路径。
    ExtensionStagingBackupListResult listing;
    QString error;
    expect(ExtensionStagingBackupInventory::list(backupRoot, subject, &listing,
                                                 &error)
               && listing.state == ExtensionStagingBackupListState::Ready
               && listing.entries.size() == 1
               && listing.entries.first().backupId
                      == SkillsDialogTestAccess::lastDeleteBackupId(dialog),
           "the reported backup id does not name the captured backup");
    // 未超上限：备注如实说"无需修剪"。
    expect(SkillsDialogTestAccess::lastRetentionNote(dialog)
               .contains(QStringLiteral("无需修剪")),
           "a no-op prune was not honestly worded as such");
}

// 备份不可用（密钥来源失败）即拒绝删除：与 MCP 保存"备份失败阻挡写入"的 fail-closed
// 先例逐字一致；目录原字节不动，原因如实可见，什么都没丢。
void testBackupFailureBlocksRemoval()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    if (!expect(createSkill(skillsRoot, QStringLiteral("guarded-skill"), false),
                "fixture write failed")) return;

    FailingKeyProvider provider;
    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(!SkillsDialogTestAccess::removeSkill(dialog,
                                                QStringLiteral("guarded-skill")),
           "a removal proceeded even though the staging backup failed");
    expect(readBytes(skillsRoot + QStringLiteral("/guarded-skill/notes.txt"))
               == kSkillNotes,
           "the skill directory changed even though the removal was refused");
    const QString reason = SkillsDialogTestAccess::lastDeleteError(dialog);
    expect(reason.contains(QStringLiteral("删除前备份失败"))
               && reason.contains(
                   QStringLiteral("extension-staging-backup-key-unavailable")),
           "the refusal did not surface the truthful backup failure reason");
    expect(SkillsDialogTestAccess::lastDeleteBackupId(dialog).isEmpty(),
           "a refused removal still claimed a captured backup");
    FixedKeyProvider verification;
    expect(stagedEntryCount(backupRoot, &verification,
                            skillSubject(QStringLiteral("guarded-skill"))) != 1,
           "a refused removal still left a readable staging backup");
}

// 内置 Skill 删除守卫先于一切备份工作：内置本来就不能删，不该产生备份。
void testBuiltinSkillAttemptsNoBackup()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    if (!expect(createSkill(skillsRoot, QStringLiteral("builtin-skill"), true),
                "fixture write failed")) return;

    FixedKeyProvider provider;
    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(!SkillsDialogTestAccess::removeSkill(dialog,
                                                QStringLiteral("builtin-skill")),
           "a built-in skill removal was not refused");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog)
               .contains(QStringLiteral("内置")),
           "the built-in refusal did not keep the manager's guard reason");
    expect(readBytes(skillsRoot + QStringLiteral("/builtin-skill/notes.txt"))
               == kSkillNotes,
           "a refused built-in removal changed the directory");
    expect(SkillsDialogTestAccess::lastDeleteBackupId(dialog).isEmpty(),
           "a built-in guard produced a backup");
    expect(!QFileInfo::exists(backupRoot),
           "a built-in guard attempted a backup");
}

// 不存在的 Skill：removeSkill 幂等成功，无可丢失的内容，不产生备份（先查 skill(id)
// 再决定）。
void testMissingSkillIsIdempotentWithoutBackup()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    if (!expect(createSkill(skillsRoot, QStringLiteral("existing-skill"), false),
                "fixture write failed")) return;

    FixedKeyProvider provider;
    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("no-such-skill")),
           "an idempotent removal of an absent skill was refused");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog).isEmpty(),
           "an idempotent removal carried a delete error");
    expect(SkillsDialogTestAccess::lastDeleteBackupId(dialog).isEmpty(),
           "an absent skill fabricated a backup id");
    expect(SkillsDialogTestAccess::lastRetentionNote(dialog).isEmpty(),
           "an absent skill carried a retention note");
    expect(!QFileInfo::exists(backupRoot),
           "an absent skill fabricated a backup of nothing");
}

// 捕获成功后超上限自动修剪到上限：种 32 份 + 删除捕获第 33 份 → 修剪最旧 1 份，清点
// 回到 32；最新鲜的捕获（被删目录原文）无条件保留并可验证；修剪备注如实上屏。
void testRemovalPrunesOverLimitBackupsToTheDomainCap()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QString subject = skillSubject(QStringLiteral("capped-skill"));
    if (!expect(createSkill(skillsRoot, QStringLiteral("capped-skill"), false),
                "fixture write failed")) return;

    FixedKeyProvider provider;
    for (int i = 0; i < 32; ++i) {
        if (!expect(seedBackup(backupRoot, &provider, subject, i),
                    "a seeded backup could not be created")) return;
    }
    if (!expect(stagedEntryCount(backupRoot, &provider, subject) == 32,
                "the seeded fixture did not reach the domain cap")) return;

    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("capped-skill")),
           "an over-limit removal was refused");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog).isEmpty(),
           "a successful removal carried a delete error");
    // 修剪备注如实区分结果：1 份超上限旧备份被修剪。
    const QString note = SkillsDialogTestAccess::lastRetentionNote(dialog);
    expect(note.contains(QStringLiteral("已按保留上限修剪"))
               && note.contains(QStringLiteral("1 份")),
           "the over-limit prune was not honestly noted on the removal result");
    expect(!note.contains(QStringLiteral("修剪失败"))
               && !note.contains(QStringLiteral("无需修剪")),
           "a successful prune was worded as a failure or a no-op");
    // 修剪到上限：33 - 1 = 32，最旧的种入备份消失。
    if (!expect(stagedEntryCount(backupRoot, &provider, subject) == 32,
                "the store was not pruned back to the domain cap")) return;
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, subject, &listing, &error)
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
    // 最新一份（被删目录原文的捕获）无条件保留且可完整验证。
    const ExtensionStagingBackupListEntry &newest = listing.entries.first();
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot snapshot;
    if (!expect(store.read(subject, newest.backupId, &snapshot, &error),
                "the freshest capture did not survive the prune")) return;
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    if (!expect(ExtensionStagingSnapshot::verify(
                    SkillExtensionInventory::treeCaptureDomain(), subject,
                    snapshot, &rebuilt, &error),
                "the freshest capture did not verify after the prune")) return;
    bool notesRetained = false;
    for (const ExtensionTreeCaptureEntry &entry : rebuilt) {
        if (!entry.directory
                && entry.relativePath == QStringLiteral("notes.txt")
                && entry.bytes == kSkillNotes) {
            notesRetained = true;
        }
    }
    expect(notesRetained,
           "the retained newest backup lost the honest pre-removal bytes");
}

// 退化清点：修剪失败绝不翻转已成功的捕获与删除——删除照常完成、捕获的备份完整在盘上、
// 零删除，备注如实携带诊断并明说"不受影响"。
void testDegradedPruneDoesNotFlipRemoval()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QString subject = skillSubject(QStringLiteral("degraded-skill"));
    if (!expect(createSkill(skillsRoot, QStringLiteral("degraded-skill"), false),
                "fixture write failed")) return;

    FixedKeyProvider provider;
    if (!expect(seedBackup(backupRoot, &provider, subject, 0),
                "the seeded backup could not be created")) return;

    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    // 捕获成功后、删除与修剪前把备份根弄退化：修剪计划必须失败关闭、零删除。
    SkillsDialogTestAccess::setAfterBackupCaptureHook(
        dialog, [&backupRoot]() {
            writeBytes(backupRoot + QStringLiteral("/stray.txt"),
                       QByteArrayLiteral("junk"));
        });
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("degraded-skill")),
           "a degraded prune flipped the successful removal into a failure");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog).isEmpty(),
           "a degraded prune surfaced as a delete error");
    expect(!QFileInfo::exists(skillsRoot + QStringLiteral("/degraded-skill")),
           "the removal did not land even though only the prune degraded");
    const QString note = SkillsDialogTestAccess::lastRetentionNote(dialog);
    expect(note.contains(QStringLiteral("修剪未能执行"))
               && note.contains(QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid"))
               && note.contains(QStringLiteral("不受影响")),
           "a degraded prune was not honestly noted with its diagnostic");
    // 零删除：种入备份与新鲜捕获都还在盘上（退化清点无法安全计数，直接数目录）。
    const QStringList dirs = QDir(backupRoot).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    expect(dirs.size() == 2,
           "a degraded prune deleted backups");
}

// 未接线（未注入密钥来源与备份根）时保持接线前行为：删除照常，零备份，绝不为
// "没有备份能力"伪造任何东西——与 MCP 先例的未接线行为逐字一致。
void testUnwiredRemovalKeepsPreWiringBehavior()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    if (!expect(createSkill(skillsRoot, QStringLiteral("unwired-skill"), false),
                "fixture write failed")) return;

    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager);
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("unwired-skill")),
           "an unwired removal was refused");
    expect(!QFileInfo::exists(skillsRoot + QStringLiteral("/unwired-skill")),
           "the unwired removal did not delete the skill directory");
    expect(SkillsDialogTestAccess::lastDeleteError(dialog).isEmpty(),
           "an unwired removal carried a delete error");
    expect(SkillsDialogTestAccess::lastDeleteBackupId(dialog).isEmpty(),
           "an unwired removal claimed a captured backup");
    expect(SkillsDialogTestAccess::lastRetentionNote(dialog).isEmpty(),
           "an unwired removal carried a retention note");
    expect(!QFileInfo::exists(backupRoot),
           "an unwired removal fabricated a backup");
}

// 严格只动该主体的备份：其他主体的暂存备份在删除与修剪全程逐字不动、可读回。
void testOtherSubjectsBackupsAreUntouched()
{
    QTemporaryDir skillsHome;
    QTemporaryDir backupHome;
    if (!expect(skillsHome.isValid() && backupHome.isValid(),
                "temporary directories unavailable")) return;
    const QString skillsRoot = skillsHome.path() + QStringLiteral("/skills");
    const QString backupRoot = backupHome.path() + QStringLiteral("/staging");
    const QString subject = skillSubject(QStringLiteral("scoped-skill"));
    if (!expect(createSkill(skillsRoot, QStringLiteral("scoped-skill"), false),
                "fixture write failed")) return;

    FixedKeyProvider provider;
    if (!expect(seedBackup(backupRoot, &provider, subject, 0),
                "the subject seed could not be created")) return;
    if (!expect(seedBackup(backupRoot, &provider,
                           QStringLiteral("skill:bystander-skill"), 1),
                "the bystander skill seed could not be created")) return;
    if (!expect(seedBackup(backupRoot, &provider,
                           QStringLiteral("mcp:claude-settings"), 2),
                "the bystander mcp seed could not be created")) return;

    SkillManager manager(nullptr, skillsRoot);
    SkillsDialog dialog(&manager, &provider, backupRoot);
    expect(SkillsDialogTestAccess::removeSkill(dialog,
                                               QStringLiteral("scoped-skill")),
           "a wired removal was refused");
    // 该主体：种入 1 份 + 捕获 1 份 = 2 份（未超上限，零删除）。
    expect(stagedEntryCount(backupRoot, &provider, subject) == 2,
           "the removed subject does not hold exactly the seed and the capture");
    // 旁观主体逐字不动且可按 id 读回（GCM 认证通过）。
    for (const QString &bystander : {QStringLiteral("skill:bystander-skill"),
                                     QStringLiteral("mcp:claude-settings")}) {
        if (!expect(stagedEntryCount(backupRoot, &provider, bystander) == 1,
                    "a bystander subject was pruned by another subject's "
                    "removal")) continue;
        ConfigurationBackupStore store(
            ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
            &provider);
        const ConfigurationBackupInventoryResult inventory =
            store.inventory(bystander, 0, {});
        ConfigurationBackupSnapshot snapshot;
        QString error;
        expect(store.read(bystander, inventory.entries.first().backupId,
                          &snapshot, &error),
               "a bystander backup no longer reads back after another "
               "subject's removal");
    }
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);

    testRemovalCapturesVerifiedBackupAndDeletes();
    testBackupFailureBlocksRemoval();
    testBuiltinSkillAttemptsNoBackup();
    testMissingSkillIsIdempotentWithoutBackup();
    testRemovalPrunesOverLimitBackupsToTheDomainCap();
    testDegradedPruneDoesNotFlipRemoval();
    testUnwiredRemovalKeepsPreWiringBehavior();
    testOtherSubjectsBackupsAreUntouched();

    if (failures > 0) {
        QTextStream(stderr) << failures << " check(s) failed" << '\n';
        return 1;
    }
    return 0;
}
