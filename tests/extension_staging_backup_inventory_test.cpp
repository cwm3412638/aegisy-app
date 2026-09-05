#include "extension_staging_backup_inventory.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QTemporaryDir>
#include <QTextStream>

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
        *key = QByteArray(32, 'i');
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

// 不经捕获工作流直接经存储造一份暂存备份:本切片测的是管理层,夹具只需要合法的存储记录。
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
        .arg(100000 + index, 6, 10, QLatin1Char('0'))
        .arg(0xaaa0000 + index, 8, 16, QLatin1Char('0'));
}

QString createdAtForIndex(int index)
{
    return QStringLiteral("2026-09-01T00:%1:00.000Z")
        .arg(index, 2, 10, QLatin1Char('0'));
}

// 独立于产品代码的身份重算:测试侧的域字面量副本。产品侧的身份字节漂移时,这里算出的身份
// 与清点结果对不上,测试立刻失败——与捕获测试交叉核对捕获域字面量是同一手法。
QString fixtureManifestIdentity(const QString &manifestPath)
{
    const QByteArray bytes = readFile(manifestPath);
    if (bytes.isEmpty()) return {};
    QByteArray material =
        QByteArrayLiteral("aegisy-extension-staging-backup-manifest-identity/0.1\0");
    material.append(bytes);
    return QStringLiteral("extension-staging-backup-manifest:sha256:")
        + QString::fromLatin1(
            QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

QString manifestPath(const QString &backupRoot, const QString &backupId)
{
    return backupRoot + QLatin1Char('/') + backupId
        + QStringLiteral("/manifest.json");
}

// 清单指纹:规划必须是纯数据,用这个映射证明规划前后存储零变化。
QMap<QString, QString> inventoryFingerprint(const QString &backupRoot)
{
    QMap<QString, QString> fingerprint;
    ExtensionStagingBackupListResult listing;
    QString error;
    if (!ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                               &listing, &error)) {
        return fingerprint;
    }
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        fingerprint.insert(entry.backupId, entry.manifestIdentity);
    }
    return fingerprint;
}

const ExtensionStagingBackupListEntry *findEntry(
    const ExtensionStagingBackupListResult &listing, const QString &backupId)
{
    for (const ExtensionStagingBackupListEntry &entry : listing.entries) {
        if (entry.backupId == backupId) return &entry;
    }
    return nullptr;
}

// 多主体清点与按主体作用域:顺序、字段与身份字节都钉住。
void testMultiSubjectListingAndScoping()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    if (!expect(createBackup(backupRoot, &provider, kAlpha,
                             QStringLiteral("ext_20260901_000000_aaaa0001"),
                             QStringLiteral("2026-09-01T00:00:00.000Z"))
                && createBackup(backupRoot, &provider, kAlpha,
                                QStringLiteral("ext_20260902_000000_aaaa0002"),
                                QStringLiteral("2026-09-02T00:00:00.000Z"))
                && createBackup(backupRoot, &provider, kBeta,
                                QStringLiteral("ext_20260903_000000_bbbb0001"),
                                QStringLiteral("2026-09-03T00:00:00.000Z")),
                "the listing fixture backups could not be created")) {
        return;
    }

    ExtensionStagingBackupListResult all;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                                      &all, &error),
                "the all-subjects listing failed")) {
        return;
    }
    if (!expect(all.state == ExtensionStagingBackupListState::Ready
                    && all.entries.size() == 3,
                "the all-subjects listing does not show all three backups")) {
        return;
    }
    // newest-first:beta 最新,随后是两份 alpha。
    expect(all.entries.at(0).backupId
                   == QStringLiteral("ext_20260903_000000_bbbb0001")
               && all.entries.at(1).backupId
                   == QStringLiteral("ext_20260902_000000_aaaa0002")
               && all.entries.at(2).backupId
                   == QStringLiteral("ext_20260901_000000_aaaa0001"),
           "the listing is not ordered newest-first");
    for (const ExtensionStagingBackupListEntry &entry : all.entries) {
        expect(entry.verification
                       == ExtensionStagingBackupEntryVerification::ListedIntact
                   && entry.verificationIssue.isEmpty()
                   && entry.createdAt.isValid(),
               "a clean backup was not listed as intact");
        expect(entry.manifestIdentity
                   == fixtureManifestIdentity(
                       manifestPath(backupRoot, entry.backupId)),
               "the listed manifest identity was not recomputed from the "
               "bytes on disk");
    }
    expect(all.entries.at(0).subject == kBeta
               && all.entries.at(1).subject == kAlpha,
           "the listing does not bind subjects");

    ExtensionStagingBackupListResult alphaOnly;
    if (!expect(ExtensionStagingBackupInventory::list(backupRoot, kAlpha,
                                                      &alphaOnly, &error),
                "the subject-scoped listing failed")) {
        return;
    }
    expect(alphaOnly.state == ExtensionStagingBackupListState::Ready
               && alphaOnly.entries.size() == 2
               && alphaOnly.entries.at(0).subject == kAlpha
               && alphaOnly.entries.at(1).subject == kAlpha,
           "the subject scope leaked another subject's backups");
    ExtensionStagingBackupListResult betaOnly;
    expect(ExtensionStagingBackupInventory::list(backupRoot, kBeta,
                                                 &betaOnly, &error)
               && betaOnly.entries.size() == 1,
           "the beta scope does not show exactly its own backup");

    // 不存在的根是 Empty,而且清点不创建它。
    const QString missingRoot = temporary.path() + QStringLiteral("/missing");
    ExtensionStagingBackupListResult empty;
    expect(ExtensionStagingBackupInventory::list(missingRoot, QString(),
                                                 &empty, &error)
               && empty.state == ExtensionStagingBackupListState::Empty
               && empty.entries.isEmpty()
               && !QFileInfo::exists(missingRoot),
           "a missing root is not Empty or the listing created it");

    // 主体语法先于一切存储工作:畸形主体连金丝雀根都不该被触碰。
    const QString canary = temporary.path() + QStringLiteral("/canary");
    for (const QString &subject : {
             QStringLiteral("SKILL:alpha"),
             QStringLiteral("skill:"),
             QStringLiteral("plugin:alpha"),
             QStringLiteral("skill:Bad Id"),
             QString() + QLatin1Char(' ')}) {
        ExtensionStagingBackupListResult refused;
        refused.entries.append(ExtensionStagingBackupListEntry());
        expect(!ExtensionStagingBackupInventory::list(canary, subject,
                                                      &refused, &error)
                   && error == QStringLiteral(
                       "extension-staging-inventory-subject-invalid")
                   && refused.entries.isEmpty(),
               "a malformed subject was not refused before store work");
    }
    expect(!QFileInfo::exists(canary),
           "a malformed subject still touched the canary root");
}

// 损坏可见性与存储退化:损坏条目留在清单里,根形状违例与锁冲突是各自独立的结果状态,
// 绝不伪装成空清单。
void testCorruptBackupsStayVisibleAndDegradedIsDistinct()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    if (!expect(createBackup(backupRoot, &provider, kAlpha,
                             QStringLiteral("ext_20260901_000000_aaaa0001"),
                             QStringLiteral("2026-09-01T00:00:00.000Z")),
                "the corruption fixture backup could not be created")) {
        return;
    }
    const QByteArray validManifest = readFile(manifestPath(
        backupRoot, QStringLiteral("ext_20260901_000000_aaaa0001")));
    if (!expect(!validManifest.isEmpty(),
                "the fixture manifest could not be read")) {
        return;
    }

    // 外域清单:结构校验失败,声称主体缺失。
    const QString foreign = QStringLiteral("ext_20260910_000000_0000f01e");
    if (!expect(writeFile(backupRoot + QLatin1Char('/') + foreign
                              + QStringLiteral("/manifest.json"),
                          QByteArrayLiteral(
                              "{\"format\":\"someone-elses-format\"}")),
                "the foreign manifest could not be planted")) {
        return;
    }
    // 目录名与清单内 backup_id 不符:结构校验失败,但声称主体可归类。
    const QString mismatched = QStringLiteral("ext_20260911_000000_0000f02d");
    if (!expect(writeFile(backupRoot + QLatin1Char('/') + mismatched
                              + QStringLiteral("/manifest.json"),
                          validManifest),
                "the mismatched manifest could not be planted")) {
        return;
    }
    // 目录形状违例:多出一份额外文件。
    const QString extraFile = QStringLiteral("ext_20260912_000000_0000f03c");
    if (!expect(writeFile(backupRoot + QLatin1Char('/') + extraFile
                              + QStringLiteral("/manifest.json"),
                          validManifest)
                && writeFile(backupRoot + QLatin1Char('/') + extraFile
                                 + QStringLiteral("/stray.txt"),
                             QByteArrayLiteral("stray")),
                "the extra-file fixture could not be planted")) {
        return;
    }

    ExtensionStagingBackupListResult all;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                                      &all, &error)
                    && all.state == ExtensionStagingBackupListState::Ready,
                "a root with corrupt entries did not list Ready")) {
        return;
    }
    // 三份损坏一份完整,一份都不能少:静默丢弃损坏备份正是回滚能力悄悄消失的方式。
    if (!expect(all.entries.size() == 4,
                "a corrupt backup was silently dropped from the listing")) {
        return;
    }
    const ExtensionStagingBackupListEntry *foreignEntry =
        findEntry(all, foreign);
    const ExtensionStagingBackupListEntry *mismatchedEntry =
        findEntry(all, mismatched);
    const ExtensionStagingBackupListEntry *extraEntry =
        findEntry(all, extraFile);
    if (!expect(foreignEntry && mismatchedEntry && extraEntry,
                "a planted corrupt backup is missing from the listing")) {
        return;
    }
    expect(foreignEntry->verification
                   == ExtensionStagingBackupEntryVerification::ListedCorrupt
               && foreignEntry->verificationIssue == QStringLiteral(
                   "extension-staging-inventory-entry-manifest-invalid")
               && foreignEntry->subject.isEmpty(),
           "the foreign manifest was not listed as corrupt");
    expect(mismatchedEntry->verification
                   == ExtensionStagingBackupEntryVerification::ListedCorrupt
               && mismatchedEntry->verificationIssue == QStringLiteral(
                   "extension-staging-inventory-entry-manifest-invalid")
               && mismatchedEntry->subject == kAlpha,
           "the mismatched-id manifest was not listed as corrupt");
    expect(extraEntry->verification
                   == ExtensionStagingBackupEntryVerification::ListedCorrupt
               && extraEntry->verificationIssue == QStringLiteral(
                   "extension-staging-inventory-entry-directory-invalid"),
           "the extra-file directory was not listed as corrupt");
    // 损坏条目的身份同样从字节重算:审计仍能指认"是哪一份"。
    expect(foreignEntry->manifestIdentity
               == fixtureManifestIdentity(
                   backupRoot + QLatin1Char('/') + foreign
                       + QStringLiteral("/manifest.json"))
               && !foreignEntry->manifestIdentity.isEmpty(),
           "a corrupt entry lost its recomputed manifest identity");

    // 作用域清点:声称主体可归类的损坏条目出现在该主体视野里(id 不符与目录形状违例
    // 两份的清单都可解析出 skill:alpha)。
    ExtensionStagingBackupListResult alphaOnly;
    expect(ExtensionStagingBackupInventory::list(backupRoot, kAlpha,
                                                 &alphaOnly, &error)
               && alphaOnly.entries.size() == 3
               && findEntry(alphaOnly, mismatched)
               && findEntry(alphaOnly, extraFile)
               && !findEntry(alphaOnly, foreign),
           "the scoped listing hid a classifiable corrupt entry");

    // 根形状违例 → 整体 Invalid,绝不是一份空清单。
    if (!expect(writeFile(backupRoot + QStringLiteral("/stray.txt"),
                          QByteArrayLiteral("junk")),
                "the root junk could not be planted")) {
        return;
    }
    ExtensionStagingBackupListResult degraded;
    expect(ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                                 &degraded, &error)
               && degraded.state == ExtensionStagingBackupListState::Invalid
               && degraded.issue == QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid"),
           "a root shape violation was not a distinct Invalid state");
    expect(degraded.state != ExtensionStagingBackupListState::Empty
               && degraded.entries.isEmpty(),
           "a degraded store was disguised as an empty listing");
    if (!expect(QFile::remove(backupRoot + QStringLiteral("/stray.txt")),
                "the root junk could not be removed")) {
        return;
    }

    // 锁冲突 → Unavailable,同样不是空清单。
    QLockFile held(backupRoot + QStringLiteral("/.backup.lock"));
    held.setStaleLockTime(30000);
    if (!expect(held.tryLock(100),
                "the test could not hold the store lock")) {
        return;
    }
    ExtensionStagingBackupListResult busy;
    expect(ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                                 &busy, &error)
               && busy.state == ExtensionStagingBackupListState::Unavailable
               && busy.issue
                   == QStringLiteral("extension-staging-inventory-busy"),
           "a locked store was not a distinct Unavailable state");
    held.unlock();
}

// 验证删除:精确 id 删除成功;id 畸形、id 不存在、条目损坏、清点退化、存储失败是五种
// 可区分的结果;其他主体的备份绝不被触碰。
void testVerifiedRemovalOutcomesAreDistinct()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    const QString alphaOld = QStringLiteral("ext_20260901_000000_aaaa0001");
    const QString alphaNew = QStringLiteral("ext_20260902_000000_aaaa0002");
    const QString betaOnly = QStringLiteral("ext_20260903_000000_bbbb0001");
    if (!expect(createBackup(backupRoot, &provider, kAlpha, alphaOld,
                             QStringLiteral("2026-09-01T00:00:00.000Z"))
                && createBackup(backupRoot, &provider, kAlpha, alphaNew,
                                QStringLiteral("2026-09-02T00:00:00.000Z"))
                && createBackup(backupRoot, &provider, kBeta, betaOnly,
                                QStringLiteral("2026-09-03T00:00:00.000Z")),
                "the removal fixture backups could not be created")) {
        return;
    }

    // id 畸形:在任何存储工作之前拒绝。
    ExtensionStagingBackupRemovalResult malformed =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider, QStringLiteral("not-a-backup-id"));
    expect(malformed.outcome
                   == ExtensionStagingBackupRemovalOutcome::IdMalformed
               && malformed.diagnostic == QStringLiteral(
                   "extension-staging-inventory-backup-id-invalid"),
           "a malformed id was not refused before store work");
    // 无密钥来源:请求本身无效。
    const ExtensionStagingBackupRemovalResult noProvider =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, nullptr, alphaOld);
    expect(noProvider.outcome
                   == ExtensionStagingBackupRemovalOutcome::RequestInvalid
               && noProvider.diagnostic == QStringLiteral(
                   "extension-staging-inventory-request-invalid"),
           "a missing key provider was not a distinct outcome");

    // id 合法但不存在。
    const ExtensionStagingBackupRemovalResult absent =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider,
            QStringLiteral("ext_20260101_000000_00000000"));
    expect(absent.outcome == ExtensionStagingBackupRemovalOutcome::NotFound
               && absent.diagnostic == QStringLiteral(
                   "extension-staging-inventory-backup-absent"),
           "an absent id is not distinguishable from a refusal");

    // 精确 id 删除成功,其他主体不动。
    const ExtensionStagingBackupRemovalResult removed =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider, alphaOld);
    expect(removed.outcome == ExtensionStagingBackupRemovalOutcome::Removed
               && removed.subject == kAlpha
               && removed.diagnostic.isEmpty(),
           "an exact-id verified removal failed");
    ExtensionStagingBackupListResult afterRemoval;
    QString error;
    expect(ExtensionStagingBackupInventory::list(backupRoot, QString(),
                                                 &afterRemoval, &error)
               && afterRemoval.entries.size() == 2
               && !findEntry(afterRemoval, alphaOld)
               && findEntry(afterRemoval, alphaNew)
               && findEntry(afterRemoval, betaOnly),
           "the removal touched more than the exact backup id");
    ConfigurationBackupStore probe(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    ConfigurationBackupSnapshot betaSnapshot;
    expect(probe.read(kBeta, betaOnly, &betaSnapshot, &error),
           "the other subject's backup no longer verifies after a removal");

    // 存储失败:清单结构合法但载荷不再通过 GCM 认证(created_at 在 AAD 里)。
    QByteArray tampered = readFile(manifestPath(backupRoot, alphaNew));
    if (!expect(tampered.contains("2026-09-02T00:00:00.000Z"),
                "the tamper fixture does not contain the timestamp")) {
        return;
    }
    tampered.replace("2026-09-02T00:00:00.000Z", "2026-09-02T00:00:01.000Z");
    if (!expect(writeFile(manifestPath(backupRoot, alphaNew), tampered),
                "the tampered manifest could not be planted")) {
        return;
    }
    const ExtensionStagingBackupRemovalResult storeFailed =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider, alphaNew);
    expect(storeFailed.outcome
                   == ExtensionStagingBackupRemovalOutcome::StoreFailed
               && storeFailed.diagnostic == QStringLiteral(
                   "extension-staging-backup-authentication-failed"),
           "a store refusal did not pass its diagnostic through verbatim");
    expect(QFileInfo::exists(backupRoot + QLatin1Char('/') + alphaNew),
           "a refused removal destroyed the evidence");

    // 条目结构级损坏:验证删除无法认证它,拒绝并原地保留。
    const QString mismatched = QStringLiteral("ext_20260911_000000_0000f02d");
    if (!expect(writeFile(manifestPath(backupRoot, mismatched),
                          readFile(manifestPath(backupRoot, alphaNew))),
                "the corrupt removal fixture could not be planted")) {
        return;
    }
    const ExtensionStagingBackupRemovalResult corrupt =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider, mismatched);
    expect(corrupt.outcome
                   == ExtensionStagingBackupRemovalOutcome::CorruptRefused
               && corrupt.diagnostic == QStringLiteral(
                   "extension-staging-inventory-backup-corrupt"),
           "a corrupt entry was not refused distinctly");
    expect(QFileInfo::exists(backupRoot + QLatin1Char('/') + mismatched),
           "a corrupt entry was deleted without the verified path");

    // 清点退化:拒绝删除,清点的原诊断逐字透传。
    if (!expect(writeFile(backupRoot + QStringLiteral("/stray.txt"),
                          QByteArrayLiteral("junk")),
                "the degradation fixture could not be planted")) {
        return;
    }
    const ExtensionStagingBackupRemovalResult degraded =
        ExtensionStagingBackupInventory::removeVerified(
            backupRoot, &provider, betaOnly);
    expect(degraded.outcome
                   == ExtensionStagingBackupRemovalOutcome::ListingDegraded
               && degraded.diagnostic == QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid"),
           "a degraded listing did not refuse the removal");
    expect(QFileInfo::exists(backupRoot + QLatin1Char('/') + betaOnly),
           "a removal proceeded over a degraded listing");
}

// 保留期规划:newest-first 保留、最近完整备份无条件保留且显式报告、损坏条目逐条列入、
// 未超限为空计划、规划零写入。
void testRetentionPlanningSemantics()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    QString error;

    // 未超限:空 prune,keep 即全部,newest-first。
    if (!expect(createBackup(backupRoot, &provider, kAlpha,
                             backupIdForIndex(0), createdAtForIndex(0))
                && createBackup(backupRoot, &provider, kAlpha,
                                backupIdForIndex(1), createdAtForIndex(1)),
                "the under-limit fixture could not be created")) {
        return;
    }
    ExtensionStagingRetentionPlan under;
    if (!expect(ExtensionStagingBackupInventory::planRetention(
                    backupRoot, kAlpha, &under, &error),
                "the under-limit planning failed")) {
        return;
    }
    expect(under.subject == kAlpha && under.maxBackups == 32
               && under.prune.isEmpty()
               && under.keepBackupIds.size() == 2
               && under.keepBackupIds.first() == backupIdForIndex(1)
               && under.newestVerifiedKept == backupIdForIndex(1),
           "an under-limit subject got a non-empty prune plan");

    // 超限:34 份完整备份 → 留最新 32,最旧 2 份逐条列入 OverLimit。
    const QString gamma = QStringLiteral("skill:gamma");
    for (int i = 0; i < 34; ++i) {
        if (!expect(createBackup(backupRoot, &provider, gamma,
                                 QStringLiteral("ext_20260902_%1_%2")
                                     .arg(100000 + i, 6, 10, QLatin1Char('0'))
                                     .arg(0xbbb0000 + i, 8, 16,
                                          QLatin1Char('0')),
                                 QStringLiteral("2026-09-02T00:%1:00.000Z")
                                     .arg(i, 2, 10, QLatin1Char('0'))),
                    "an over-limit fixture backup could not be created")) {
            return;
        }
    }
    const auto gammaId = [](int index) {
        return QStringLiteral("ext_20260902_%1_%2")
            .arg(100000 + index, 6, 10, QLatin1Char('0'))
            .arg(0xbbb0000 + index, 8, 16, QLatin1Char('0'));
    };

    // 该主体的两份损坏备份(目录名与清单内 id 不符,声称主体可归类):逐条列入 Corrupt。
    const QByteArray gammaManifest =
        readFile(manifestPath(backupRoot, gammaId(33)));
    const QString corruptOne = QStringLiteral("ext_20260911_000000_0000f02d");
    const QString corruptTwo = QStringLiteral("ext_20260912_000000_0000f03c");
    if (!expect(writeFile(manifestPath(backupRoot, corruptOne), gammaManifest)
                && writeFile(manifestPath(backupRoot, corruptTwo),
                             gammaManifest),
                "the corrupt retention fixtures could not be planted")) {
        return;
    }

    // 规划是纯数据:前后清单指纹逐字节一致。
    const QMap<QString, QString> before = inventoryFingerprint(backupRoot);
    ExtensionStagingRetentionPlan plan;
    if (!expect(ExtensionStagingBackupInventory::planRetention(
                    backupRoot, gamma, &plan, &error),
                "the over-limit planning failed")) {
        return;
    }
    expect(inventoryFingerprint(backupRoot) == before,
           "retention planning mutated the store");
    expect(plan.keepBackupIds.size() == 32
               && plan.keepBackupIds.first() == gammaId(33)
               && plan.keepBackupIds.last() == gammaId(2),
           "the keep set is not the newest 32 intact backups");
    // 最近一份完整备份无条件保留,且是显式单独报告的决策。
    expect(plan.newestVerifiedKept == gammaId(33)
               && plan.keepBackupIds.contains(plan.newestVerifiedKept),
           "the newest verified backup is not explicitly retained");
    if (!expect(plan.prune.size() == 4,
                "over-limit and corrupt entries are not all listed")) {
        return;
    }
    expect(plan.prune.at(0).backupId == gammaId(1)
               && plan.prune.at(0).reason
                   == ExtensionStagingPruneReason::OverLimit
               && plan.prune.at(1).backupId == gammaId(0)
               && plan.prune.at(1).reason
                   == ExtensionStagingPruneReason::OverLimit,
           "the over-limit prune entries are not the oldest two");
    expect(plan.prune.at(2).reason == ExtensionStagingPruneReason::Corrupt
               && plan.prune.at(3).reason
                   == ExtensionStagingPruneReason::Corrupt
               && ((plan.prune.at(2).backupId == corruptOne
                    && plan.prune.at(3).backupId == corruptTwo)
                   || (plan.prune.at(2).backupId == corruptTwo
                       && plan.prune.at(3).backupId == corruptOne)),
           "a corrupt backup was not explicitly listed as a prune candidate");
    for (const ExtensionStagingRetentionPruneEntry &prune : plan.prune) {
        expect(!prune.manifestIdentity.isEmpty(),
               "a prune candidate lost its manifest identity");
    }

    // 清点退化:不产出计划,清点的原诊断透传。
    if (!expect(writeFile(backupRoot + QStringLiteral("/stray.txt"),
                          QByteArrayLiteral("junk")),
                "the degradation fixture could not be planted")) {
        return;
    }
    ExtensionStagingRetentionPlan refused;
    expect(!ExtensionStagingBackupInventory::planRetention(
               backupRoot, gamma, &refused, &error)
               && error == QStringLiteral(
                   "extension-staging-inventory-store-shape-invalid")
               && refused.subject.isEmpty() && refused.prune.isEmpty(),
           "planning proceeded over a degraded listing");
    if (!expect(QFile::remove(backupRoot + QStringLiteral("/stray.txt")),
                "the degradation fixture could not be removed")) {
        return;
    }

    // 主体语法先于存储工作。
    expect(!ExtensionStagingBackupInventory::planRetention(
               backupRoot, QStringLiteral("skill:"), &refused, &error)
               && error == QStringLiteral(
                   "extension-staging-inventory-subject-invalid"),
           "a malformed subject reached the store during planning");
}

// apply 逐条组合验证删除:完整条目被移除,损坏条目各自报告拒绝,其他主体不动。
void testApplyRetentionComposesVerifiedRemoval()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString backupRoot = temporary.path() + QStringLiteral("/backups");
    FixedKeyProvider provider;
    const QString delta = QStringLiteral("skill:delta");
    const auto deltaId = [](int index) {
        return QStringLiteral("ext_20260903_%1_%2")
            .arg(100000 + index, 6, 10, QLatin1Char('0'))
            .arg(0xccc0000 + index, 8, 16, QLatin1Char('0'));
    };
    for (int i = 0; i < 33; ++i) {
        if (!expect(createBackup(backupRoot, &provider, delta, deltaId(i),
                                 QStringLiteral("2026-09-03T00:%1:00.000Z")
                                     .arg(i, 2, 10, QLatin1Char('0'))),
                    "an apply fixture backup could not be created")) {
            return;
        }
    }
    if (!expect(createBackup(backupRoot, &provider, kBeta,
                             QStringLiteral("ext_20260904_000000_bbbb0001"),
                             QStringLiteral("2026-09-04T00:00:00.000Z")),
                "the bystander backup could not be created")) {
        return;
    }
    const QString corrupt = QStringLiteral("ext_20260911_000000_0000f02d");
    if (!expect(writeFile(manifestPath(backupRoot, corrupt),
                          readFile(manifestPath(backupRoot, deltaId(32)))),
                "the corrupt apply fixture could not be planted")) {
        return;
    }

    ExtensionStagingRetentionPlan plan;
    QString error;
    if (!expect(ExtensionStagingBackupInventory::planRetention(
                    backupRoot, delta, &plan, &error)
                    && plan.prune.size() == 2,
                "the apply fixture plan is wrong")) {
        return;
    }
    const QList<ExtensionStagingRetentionApplyEntry> outcomes =
        ExtensionStagingBackupInventory::applyRetention(backupRoot, &provider,
                                                        plan);
    if (!expect(outcomes.size() == 2,
                "apply did not report one outcome per prune entry")) {
        return;
    }
    const ExtensionStagingRetentionApplyEntry *overLimit = nullptr;
    const ExtensionStagingRetentionApplyEntry *corruptEntry = nullptr;
    for (const ExtensionStagingRetentionApplyEntry &entry : outcomes) {
        if (entry.backupId == deltaId(0)) overLimit = &entry;
        if (entry.backupId == corrupt) corruptEntry = &entry;
    }
    if (!expect(overLimit && corruptEntry,
                "an apply outcome is missing")) {
        return;
    }
    expect(overLimit->outcome
                   == ExtensionStagingBackupRemovalOutcome::Removed
               && overLimit->diagnostic.isEmpty(),
           "the over-limit backup was not removed");
    // 损坏条目被如实报告为拒绝:验证删除路径无法认证它,绝不假装成功。
    expect(corruptEntry->outcome
                   == ExtensionStagingBackupRemovalOutcome::CorruptRefused
               && corruptEntry->diagnostic == QStringLiteral(
                   "extension-staging-inventory-backup-corrupt"),
           "a corrupt prune entry was not honestly reported");
    ExtensionStagingBackupListResult after;
    expect(ExtensionStagingBackupInventory::list(backupRoot, delta, &after,
                                                 &error)
               && after.entries.size() == 33
               && !findEntry(after, deltaId(0))
               && findEntry(after, corrupt),
           "the applied plan left the wrong residue");
    ExtensionStagingBackupListResult betaAfter;
    expect(ExtensionStagingBackupInventory::list(backupRoot, kBeta,
                                                 &betaAfter, &error)
               && betaAfter.entries.size() == 1,
           "an apply touched another subject's backups");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testMultiSubjectListingAndScoping();
    testCorruptBackupsStayVisibleAndDegradedIsDistinct();
    testVerifiedRemovalOutcomesAreDistinct();
    testRetentionPlanningSemantics();
    testApplyRetentionComposesVerifiedRemoval();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging backup inventory guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
