#include "extension_staging_backup_retention.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <QTextStream>

// 保留期修剪共享入口（ExtensionStagingBackupRetention::pruneAfterCapture）的聚焦测试：
// 真实临时目录 + 真实加密暂存存储。覆盖：超上限修剪到上限、无需修剪零删除、退化清点
// 计划失败零删除且诊断透传、单条删除失败如实汇总、其他主体绝不被触碰、最近完整备份
// 无条件保留（newestVerifiedKept）在接线路径上成立。
namespace {

int failures = 0;

const QString kAlpha = QStringLiteral("skill:alpha");
const QString kBeta = QStringLiteral("skill:beta");

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

class FixedKeyProvider final : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &, bool, QByteArray *key, QString *) override
    {
        if (!key) return false;
        *key = QByteArray(32, 'r');
        return true;
    }
};

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

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// 与清点测试同形：不经捕获工作流直接经存储造一份暂存备份（本层测修剪,夹具只需要合法
// 的存储记录）。
bool createBackup(const QString &backupRoot,
                  ConfigurationBackupKeyProvider *provider,
                  const QString &subject, const QString &backupId,
                  const QString &createdAtText)
{
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        provider);
    ConfigurationBackupSnapshot snapshot;
    snapshot.backupId = backupId;
    snapshot.tool = subject;
    snapshot.createdAt = QDateTime::fromString(createdAtText,
                                               Qt::ISODateWithMs);
    snapshot.files = {{ 0, true, QByteArrayLiteral("payload-bytes") }};
    QString error;
    return store.create(snapshot, &error);
}

QString backupIdForIndex(int index)
{
    return QStringLiteral("ext_20260901_%1_%2")
        .arg(200000 + index, 6, 10, QLatin1Char('0'))
        .arg(0xddd0000 + index, 8, 16, QLatin1Char('0'));
}

QString createdAtForIndex(int index)
{
    return QStringLiteral("2026-09-01T%1:%2:00.000Z")
        .arg(index / 60, 2, 10, QLatin1Char('0'))
        .arg(index % 60, 2, 10, QLatin1Char('0'));
}

QString manifestPath(const QString &backupRoot, const QString &backupId)
{
    return backupRoot + QLatin1Char('/') + backupId
        + QStringLiteral("/manifest.json");
}

int entryCount(const QString &backupRoot, const QString &subject)
{
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!ExtensionStagingBackupInventory::list(backupRoot, subject, &listing,
                                               &error)
            || listing.state != ExtensionStagingBackupListState::Ready) {
        return -1;
    }
    return listing.entries.size();
}

bool hasEntry(const QString &backupRoot, const QString &subject,
              const QString &backupId)
{
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!ExtensionStagingBackupInventory::list(backupRoot, subject, &listing,
                                               &error)) {
        return false;
    }
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.backupId == backupId) return true;
    }
    return false;
}

// 超上限自动修剪到上限：34 份完整 + 1 份损坏 → 留最新 32,最旧 2 份删除,损坏条目如实
// 原地保留;旁观主体绝不被触碰;最近完整备份无条件保留且仍可按 id 读回验证。
void testPruneAfterCaptureTrimsOverLimit()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    for (int i = 0; i < 34; ++i) {
        if (!expect(createBackup(backupRoot, &provider, kAlpha,
                                 backupIdForIndex(i), createdAtForIndex(i)),
                    "an over-limit fixture backup could not be created")) {
            return;
        }
    }
    if (!expect(createBackup(backupRoot, &provider, kBeta,
                             QStringLiteral("ext_20260902_000000_bbbb0001"),
                             QStringLiteral("2026-09-02T00:00:00.000Z"))
                && createBackup(backupRoot, &provider, kBeta,
                                QStringLiteral("ext_20260903_000000_bbbb0002"),
                                QStringLiteral("2026-09-03T00:00:00.000Z")),
                "the bystander backups could not be created")) {
        return;
    }
    // 该主体的一份损坏备份（目录名与清单内 id 不符,声称主体可归类）。
    const QString corrupt = QStringLiteral("ext_20260911_000000_0000f02d");
    if (!expect(writeFile(manifestPath(backupRoot, corrupt),
                          readFile(manifestPath(backupRoot,
                                                backupIdForIndex(33)))),
                "the corrupt fixture could not be planted")) {
        return;
    }

    const ExtensionStagingBackupRetentionRun run =
        ExtensionStagingBackupRetention::pruneAfterCapture(backupRoot, &provider,
                                                           kAlpha);
    expect(!run.planFailed && run.planError.isEmpty(),
           "an over-limit prune did not plan");
    expect(run.removedCount == 2 && run.failures.isEmpty(),
           "the over-limit prune did not remove exactly the oldest two");
    expect(run.corruptKeptCount == 1,
           "the corrupt backup was not honestly kept in place");
    // 最近完整备份无条件保留,且是显式回显的决策。
    expect(run.newestVerifiedKept == backupIdForIndex(33),
           "the newest verified backup was not explicitly retained");
    // 修剪后：32 份完整 + 1 份损坏,最旧两份消失。
    expect(entryCount(backupRoot, kAlpha) == 33
               && !hasEntry(backupRoot, kAlpha, backupIdForIndex(0))
               && !hasEntry(backupRoot, kAlpha, backupIdForIndex(1))
               && hasEntry(backupRoot, kAlpha, backupIdForIndex(2))
               && hasEntry(backupRoot, kAlpha, corrupt),
           "the pruned subject does not hold exactly the newest 32 plus the "
           "corrupt evidence");
    // 保留的最近一份仍可按 id 读回并通过 GCM 验证。
    ConfigurationBackupStore probe(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot snapshot;
    QString error;
    expect(probe.read(kAlpha, backupIdForIndex(33), &snapshot, &error),
           "the retained newest backup no longer verifies after pruning");
    // 其他主体的备份绝不被触碰。
    expect(entryCount(backupRoot, kBeta) == 2
               && probe.read(kBeta,
                             QStringLiteral("ext_20260902_000000_bbbb0001"),
                             &snapshot, &error),
           "the prune touched another subject's backups");
}

// 未超限：零删除,计划成功,什么都不少。
void testNothingToPruneIsZeroDeletion()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    for (int i = 0; i < 3; ++i) {
        if (!expect(createBackup(backupRoot, &provider, kAlpha,
                                 backupIdForIndex(i), createdAtForIndex(i)),
                    "an under-limit fixture backup could not be created")) {
            return;
        }
    }
    const ExtensionStagingBackupRetentionRun run =
        ExtensionStagingBackupRetention::pruneAfterCapture(backupRoot, &provider,
                                                           kAlpha);
    expect(!run.planFailed && run.removedCount == 0
               && run.corruptKeptCount == 0 && run.failures.isEmpty()
               && run.newestVerifiedKept == backupIdForIndex(2),
           "an under-limit subject was pruned");
    expect(entryCount(backupRoot, kAlpha) == 3,
           "a no-op prune deleted backups");
}

// 退化清点：计划失败 = 零删除 + 清点的原诊断逐字透传。
void testDegradedListingFailsPlanWithZeroDeletions()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    if (!expect(createBackup(backupRoot, &provider, kAlpha,
                             backupIdForIndex(0), createdAtForIndex(0))
                && createBackup(backupRoot, &provider, kAlpha,
                                backupIdForIndex(1), createdAtForIndex(1)),
                "the degraded fixture backups could not be created")) {
        return;
    }
    if (!expect(writeFile(backupRoot + QStringLiteral("/stray.txt"),
                          QByteArrayLiteral("junk")),
                "the degradation fixture could not be planted")) {
        return;
    }
    const ExtensionStagingBackupRetentionRun run =
        ExtensionStagingBackupRetention::pruneAfterCapture(backupRoot, &provider,
                                                           kAlpha);
    expect(run.planFailed
               && run.planError == QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid"),
           "a degraded listing did not fail the plan with the verbatim "
           "diagnostic");
    expect(run.removedCount == 0 && run.corruptKeptCount == 0
               && run.failures.isEmpty() && run.newestVerifiedKept.isEmpty(),
           "a failed plan still deleted something");
    expect(QFileInfo::exists(
               backupRoot + QLatin1Char('/') + backupIdForIndex(0))
               && QFileInfo::exists(
                   backupRoot + QLatin1Char('/') + backupIdForIndex(1)),
           "a failed plan deleted backups");
}

// 单条删除失败如实汇总：另一条照删,失败条目携带 id 与逐字诊断,证据原地保留,最近完整
// 备份仍在。
void testSingleRemovalFailureIsHonestlySummarized()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    for (int i = 0; i < 34; ++i) {
        if (!expect(createBackup(backupRoot, &provider, kAlpha,
                                 backupIdForIndex(i), createdAtForIndex(i)),
                    "a failure fixture backup could not be created")) {
            return;
        }
    }
    // 篡改最旧一份的 created_at（AAD 成员）：清单结构仍合法（清点照常列为完整）,但
    // 存储的验证删除做 GCM 认证时失败。
    QByteArray tampered = readFile(manifestPath(backupRoot, backupIdForIndex(0)));
    if (!expect(tampered.contains("2026-09-01T00:00:00.000Z"),
                "the tamper fixture does not contain the timestamp")) {
        return;
    }
    tampered.replace("2026-09-01T00:00:00.000Z", "2026-09-01T00:00:01.000Z");
    if (!expect(writeFile(manifestPath(backupRoot, backupIdForIndex(0)),
                          tampered),
                "the tampered manifest could not be planted")) {
        return;
    }

    const ExtensionStagingBackupRetentionRun run =
        ExtensionStagingBackupRetention::pruneAfterCapture(backupRoot, &provider,
                                                           kAlpha);
    expect(!run.planFailed,
           "a single removal failure was misreported as a plan failure");
    expect(run.removedCount == 1,
           "the removable over-limit entry was not removed");
    if (!expect(run.failures.size() == 1,
                "the failed removal was not honestly summarized")) {
        return;
    }
    expect(run.failures.first().backupId == backupIdForIndex(0)
               && run.failures.first().outcome
                   == ExtensionStagingBackupRemovalOutcome::StoreFailed
               && run.failures.first().diagnostic == QStringLiteral(
                   "extension-staging-backup-authentication-failed"),
           "the failed removal lost its id, outcome, or verbatim diagnostic");
    expect(run.newestVerifiedKept == backupIdForIndex(33)
               && hasEntry(backupRoot, kAlpha, backupIdForIndex(33)),
           "the newest verified backup did not survive a partial prune");
    // 33 份在场：32 份保留 + 1 份删除失败的证据原地保留。
    expect(entryCount(backupRoot, kAlpha) == 33
               && hasEntry(backupRoot, kAlpha, backupIdForIndex(0)),
           "a failed removal destroyed the evidence");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testPruneAfterCaptureTrimsOverLimit();
    testNothingToPruneIsZeroDeletion();
    testDegradedListingFailsPlanWithZeroDeletions();
    testSingleRemovalFailureIsHonestlySummarized();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging backup retention guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
