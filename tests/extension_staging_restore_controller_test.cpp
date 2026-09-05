#include "extension_staging_restore_controller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

int failures = 0;

const QString kSkillSubject = QStringLiteral("skill:example");
const QString kDecidedAtText = QStringLiteral("2026-09-05T13:00:00.000Z");

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

QDateTime decidedAt()
{
    return QDateTime::fromString(kDecidedAtText, Qt::ISODateWithMs);
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

// 一条走完的链路留下的全部中间产物。
struct ChainFixture {
    ConfigurationBackupSnapshot snapshot;
    ExtensionStagingRestoreBackupDescriptor descriptor;
    QString canonicalDestination;
    ExtensionStagingRestorePlan plan;
    ExtensionStagingRestorePrompt prompt;
};

// 完整链路：真实临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 读回并计划 →
// 呈现。
bool buildSkillChain(QTemporaryDir &temporary, const QString &name, int variant,
                     const TreeSpec &source, ChainFixture *fixture)
{
    const QString base = temporary.path() + QLatin1Char('/') + name;
    const QString treeRoot = base + QStringLiteral("/tree");
    if (!expect(writeTree(treeRoot, source),
                "the source fixture could not be written")) {
        return false;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(treeRoot, &tree),
                "the source fixture could not be captured")) {
        return false;
    }
    const QString backupRoot = base + QStringLiteral("/backups");
    QString error;
    if (!ExtensionStagingSnapshot::build(skillDomain(), tree, kSkillSubject,
                                         fixtureBackupId(variant),
                                         fixtureCreatedAt(), &fixture->snapshot,
                                         &error)) {
        return expect(false, "fixture snapshot refused");
    }
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), backupRoot,
        &provider);
    if (!expect(store.create(fixture->snapshot, &error),
                "the fixture backup could not be stored")) {
        return false;
    }
    ExtensionStagingBackupListResult listing;
    if (!expect(ExtensionStagingBackupInventory::list(
                    backupRoot, kSkillSubject, &listing, &error)
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

    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSkillSubject, fixtureBackupId(variant), &readBack,
                           &error),
                "the fixture backup could not be read back")) {
        return false;
    }
    const QString destination = base + QStringLiteral("/dest");
    if (!expect(QDir().mkpath(destination),
                "the destination fixture could not be created")) {
        return false;
    }
    DiskObservation observation(destination);
    fixture->canonicalDestination = observation.canonicalRoot();
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    skillDomain(), kSkillSubject, readBack,
                    fixture->canonicalDestination, &observation, &fixture->plan,
                    &error),
                "the fixture could not be planned")) {
        return false;
    }
    fixture->prompt = ExtensionStagingRestorePresentation::build(
        fixture->plan, fixture->descriptor, fixture->canonicalDestination,
        fixtureCreatedAt().addDays(1));
    return expect(fixture->prompt.state
                      == ExtensionStagingRestorePromptState::Ready,
                  "a clean fixture plan was not presentable");
}

// 与提示逐项对齐的回传。
ExtensionStagingRestoreApprovalAcknowledgement acknowledge(
    const ExtensionStagingRestorePrompt &prompt,
    ExtensionStagingRestoreApprovalDecision decision)
{
    ExtensionStagingRestoreApprovalAcknowledgement value;
    value.decision = decision;
    value.subject = prompt.subject;
    value.backupId = prompt.backupId;
    value.destinationRoot = prompt.destinationRoot;
    value.approvedPlanIdentity = prompt.echoedPlanIdentity;
    value.approvedTreeIdentity = prompt.echoedTreeIdentity;
    value.acknowledgedWarnings = prompt.warnings;
    value.highRiskConfirmed = true;
    return value;
}

// 可注入的审计安全存储，额外携带两个并发/破坏钩子：
// - `conflictAuthority` 在第 `conflictOnRead` 次读取起替换已存授权，模拟"控制器
//   读取之后、提交之前另一次记录已推进代号"；
// - `corruptAfterWrites` 在第 N 次写入落地后把已存授权改成垃圾，破坏提交与重读
//   之间的世界。
class FakeSecureStore final : public ExtensionStagingRestoreAuditSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *errorCode) override
    {
        ++reads;
        if (errorCode) errorCode->clear();
        if (readState != ReadState::Found) {
            if (errorCode && readState != ReadState::Missing) {
                *errorCode = QStringLiteral("fake-read-%1")
                    .arg(readState == ReadState::Unavailable
                         ? QStringLiteral("unavailable")
                         : QStringLiteral("invalid"));
            }
            return readState;
        }
        const QByteArray current =
            (!conflictAuthority.isEmpty() && reads == conflictOnRead)
            ? conflictAuthority : stored;
        if (current.isEmpty()) return ReadState::Missing;
        if (value) *value = current;
        return ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *errorCode) override
    {
        ++writes;
        if (errorCode) errorCode->clear();
        stored = value;
        if (writes == corruptAfterWrites) {
            stored = QByteArrayLiteral("{\"schema_version\":\"tampered\"}");
        }
        return WriteOutcome::Committed;
    }

    QByteArray stored;
    ReadState readState = ReadState::Found;
    QByteArray conflictAuthority;
    int conflictOnRead = -1;
    int corruptAfterWrites = -1;
    int reads = 0;
    int writes = 0;
};

struct Fixture {
    explicit Fixture(const QString &path)
        : settings(path, QSettings::IniFormat)
    {
    }

    FakeSecureStore secure;
    QSettings settings;

    ExtensionStagingRestoreAuditLedgerStore store()
    {
        return ExtensionStagingRestoreAuditLedgerStore(&secure, &settings);
    }
};

QString digest(const QString &prefix, const QByteArray &seed)
{
    return prefix + sha256Hex(seed);
}

// 直接构造的合法审计条目，用于把账本预填到上限（不经由控制器）。
ExtensionStagingRestoreAuditEntry directEntry(int index)
{
    ExtensionStagingRestoreAuditEntry value;
    value.subject = kSkillSubject;
    value.backupId = QStringLiteral("backup-%1").arg(index);
    value.destinationRoot = QStringLiteral("/tmp/restore-target");
    value.planIdentity = digest(
        QStringLiteral("extension-staging-restore-plan:sha256:"),
        QByteArray::number(index) + "-plan");
    value.treeIdentity = digest(QStringLiteral("extension-content:sha256:"),
                                QByteArray::number(index) + "-tree");
    value.warnings = {
        ExtensionStagingRestoreWarning::RestoreDoesNotExecuteYet};
    value.decision = ExtensionStagingRestoreAuditDecision::Approved;
    value.decidedAt = QDateTime::fromString(
        QStringLiteral("2026-09-05T12:%1:00.000Z")
            .arg(index % 60, 2, 10, QLatin1Char('0')),
        Qt::ISODateWithMs);
    return value;
}

// 完整链路 → 逐项对齐的批准经由控制器记录：账本包含每个字段都被绑定的批准条目，
// 凭据绑定确切计划身份与树身份。
void testFullChainApprovedDecisionRecorded()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("approved"), 1,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}},
                                  {QStringLiteral("zdir")}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("approved.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();

    const ExtensionStagingRestoreRecordResult result =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &store);
    if (!expect(result.recorded
                    && result.decision
                        == ExtensionStagingRestoreAuditDecision::Approved
                    && result.errorCode.isEmpty(),
                "an aligned approval was not recorded")) {
        return;
    }
    // 凭据原样透传：纯数据，绑定确切计划身份与树身份。
    expect(result.verdict.state
                   == ExtensionStagingRestoreApprovalState::Authorized
               && result.verdict.authorizedPlanIdentity
                   == chain.plan.planIdentity
               && result.verdict.authorizedTreeIdentity
                   == chain.plan.treeIdentity,
           "the credential did not bind the exact identities");
    // 返回的账本来自提交后的重读：唯一条目逐项绑定渲染出的提示。
    expect(result.ledger.state == ExtensionStagingRestoreAuditStoreState::Ready
               && result.ledger.generation == 1
               && result.ledger.entries.size() == 1,
           "the re-read ledger did not hold exactly one entry");
    if (result.ledger.entries.size() != 1) return;
    const ExtensionStagingRestoreAuditEntry &entry =
        result.ledger.entries.first();
    expect(entry.subject == chain.prompt.subject
               && entry.backupId == chain.prompt.backupId
               && entry.destinationRoot == chain.prompt.destinationRoot
               && entry.planIdentity == chain.prompt.echoedPlanIdentity
               && entry.planIdentity == chain.plan.planIdentity
               && entry.treeIdentity == chain.prompt.echoedTreeIdentity
               && entry.treeIdentity == chain.plan.treeIdentity
               && entry.warnings == chain.prompt.warnings
               && entry.decision == ExtensionStagingRestoreAuditDecision::Approved
               && entry.decidedAt == decidedAt(),
           "the recorded entry does not bind the rendered prompt field by "
           "field");
    // 独立的加载看到同一份历史。
    expect(fixture.store().load().identity == result.ledger.identity,
           "an independent load did not see the recorded decision");
}

// 被问过并且被回答了"不"：拒绝同样被记录，但条目不携带任何授权。
void testDeclinedDecisionRecordedWithoutAuthority()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("declined"), 2,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("declined.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    const QString beforeIdentity = fixture.store().load().identity;

    const ExtensionStagingRestoreRecordResult result =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Decline),
            decidedAt(), &store);
    if (!expect(result.recorded
                    && result.decision
                        == ExtensionStagingRestoreAuditDecision::Declined,
                "a genuine decline was not recorded")) {
        return;
    }
    // 策略按"declined"拒绝透传：拒绝条目不携带任何授权。
    expect(result.verdict.state == ExtensionStagingRestoreApprovalState::Refused
               && result.verdict.errorCode
                   == QStringLiteral("extension-restore-approval-declined")
               && result.verdict.authorizedPlanIdentity.isEmpty()
               && result.verdict.authorizedTreeIdentity.isEmpty(),
           "a declined decision carried restore authority");
    expect(result.ledger.identity != beforeIdentity
               && result.ledger.entries.size() == 1
               && result.ledger.entries.first().decision
                   == ExtensionStagingRestoreAuditDecision::Declined
               && result.ledger.entries.first().decidedAt == decidedAt(),
           "the declined entry was not recorded truthfully");
}

// 策略拒绝零写入：没有一个有效的问题被回答过，账本一个字节也不动。这包括任何
// 维度漂移、未验证备份，以及对一份本就不可能批准的提示说"不"。
void testPolicyRefusalsWriteNothing()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("refusals"), 3,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("refusals.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    // 先记录一笔真实决定，使"零写入"可以由身份前后比较证明（空载荷与缺席载荷
    // 的身份都为空，无法区分）。
    const ExtensionStagingRestoreRecordResult seed =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &store);
    if (!expect(seed.recorded, "the refusal fixture failed to seed")) return;
    const QString identityBefore = seed.ledger.identity;
    const qint64 generationBefore = seed.ledger.generation;
    const int readsBefore = fixture.secure.reads;
    const int writesBefore = fixture.secure.writes;

    // 计划身份漂移：这份批准对应另一份提示。
    ExtensionStagingRestoreApprovalAcknowledgement drifted =
        acknowledge(chain.prompt,
                    ExtensionStagingRestoreApprovalDecision::Approve);
    drifted.approvedPlanIdentity = digest(
        QStringLiteral("extension-staging-restore-plan:sha256:"), "other-plan");
    const ExtensionStagingRestoreRecordResult drift =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification, drifted, decidedAt(),
            &store);
    expect(!drift.recorded
               && drift.errorCode == QStringLiteral(
                   "extension-restore-approval-plan-drift"),
           "a drifted approval was recorded");

    // 未通过清单身份级验证的备份：即使其余逐项对齐也必须拒绝。
    const ExtensionStagingRestoreRecordResult unverified =
        ExtensionStagingRestoreController::record(
            chain.prompt,
            ExtensionStagingBackupEntryVerification::ListedCorrupt,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &store);
    expect(!unverified.recorded
               && unverified.errorCode == QStringLiteral(
                   "extension-restore-approval-backup-unverified"),
           "an unverified backup approval was recorded");

    // 对一份构建失败的提示说"不"：问题从未被有效地问过，因此也不是决定。
    const ExtensionStagingRestorePrompt refusedPrompt =
        ExtensionStagingRestorePresentation::buildRefusal(
            QStringLiteral("extension-staging-restore-destination-conflict"));
    ExtensionStagingRestoreApprovalAcknowledgement declineRefused;
    declineRefused.decision =
        ExtensionStagingRestoreApprovalDecision::Decline;
    const ExtensionStagingRestoreRecordResult noQuestion =
        ExtensionStagingRestoreController::record(
            refusedPrompt, chain.descriptor.verification, declineRefused,
            decidedAt(), &store);
    expect(!noQuestion.recorded
               && noQuestion.errorCode == QStringLiteral(
                   "extension-restore-approval-declined"),
           "a decline against an unpresentable question was recorded");

    // 三次拒绝都没有触碰账本：既不读也不写，身份与代号前后一致。
    expect(fixture.secure.reads == readsBefore
               && fixture.secure.writes == writesBefore,
           "a policy refusal touched the audit ledger");
    const ExtensionStagingRestoreAuditStoreResult after =
        fixture.store().load();
    expect(after.identity == identityBefore
               && after.generation == generationBefore
               && after.entries.size() == 1,
           "a policy refusal rewrote recorded history");
}

// 读不出、损坏或被锁定的审计链以各自独立的状态阻止记录，并保持冻结直到调用方
// 重新加载（后端恢复健康）。
void testUnreadableLedgerBlocksRecording()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("blocked"), 4,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("blocked.ini")));
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    const ExtensionStagingRestoreApprovalAcknowledgement approval =
        acknowledge(chain.prompt,
                    ExtensionStagingRestoreApprovalDecision::Approve);

    // 后端被锁定：当前内容未知，不是"没有记录"，决定不能被写入。
    fixture.secure.readState = FakeSecureStore::ReadState::Unavailable;
    ExtensionStagingRestoreRecordResult result =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification, approval, decidedAt(),
            &store);
    expect(!result.recorded
               && result.ledger.state
                   == ExtensionStagingRestoreAuditStoreState::Unavailable
               && result.errorCode == QStringLiteral("fake-read-unavailable"),
           "a locked backend did not block recording");

    // 后端报告授权损坏：同样不能写入。
    fixture.secure.readState = FakeSecureStore::ReadState::Invalid;
    result = ExtensionStagingRestoreController::record(
        chain.prompt, chain.descriptor.verification, approval, decidedAt(),
        &store);
    expect(!result.recorded
               && result.ledger.state
                   == ExtensionStagingRestoreAuditStoreState::Invalid
               && result.errorCode == QStringLiteral("fake-read-invalid"),
           "an invalid authority did not block recording");

    // 后端恢复健康的瞬间冻结解除：控制器无状态，每次记录都重新加载。
    fixture.secure.readState = FakeSecureStore::ReadState::Found;
    result = ExtensionStagingRestoreController::record(
        chain.prompt, chain.descriptor.verification, approval, decidedAt(),
        &store);
    if (!expect(result.recorded, "a healthy backend stayed frozen")) return;

    // 两半互相矛盾（授权被删、载荷残留）：以反降级代号阻止记录。
    fixture.secure.stored.clear();
    result = ExtensionStagingRestoreController::record(
        chain.prompt, chain.descriptor.verification, approval, decidedAt(),
        &store);
    expect(!result.recorded
               && result.errorCode == QStringLiteral(
                   "extension-restore-audit-store-record-without-authority"),
           "a contradictory ledger did not block recording");

    // 没有存储：独立代号，不崩溃也不记录。
    result = ExtensionStagingRestoreController::record(
        chain.prompt, chain.descriptor.verification, approval, decidedAt(),
        nullptr);
    expect(!result.recorded
               && result.errorCode == QStringLiteral(
                   "extension-restore-controller-store-unavailable"),
           "a missing store did not block recording");
    expect(ExtensionStagingRestoreController::inspect(nullptr).state
               == ExtensionStagingRestoreAuditStoreState::Unavailable,
           "a missing store was not reported as unavailable");
}

// 并发决定由代号比较并交换裁决：控制器读取之后、提交之前另一个记录者推进了
// 代号，冲突以独立代号报告——不静默重试，也不是最后写入者获胜。
void testCasConflictReportedDistinctly()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("cas"), 5,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("cas.ini")));

    // 预演两次真实提交，捕获第二次提交后的授权字节（代号 2），随后把世界回滚
    // 到代号 1：这就是"另一个记录者已经提交"的见证。
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    if (!expect(store.replace({directEntry(0)}, 0, &updated, &errorCode)
                    && updated.generation == 1,
                "the CAS fixture failed its first commit")) {
        return;
    }
    const QByteArray generationOneAuthority = fixture.secure.stored;
    const QByteArray generationOnePayload = fixture.settings
        .value(ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey())
        .toByteArray();
    ExtensionStagingRestoreAuditLedgerStore store2 = fixture.store();
    if (!expect(store2.replace({directEntry(0), directEntry(1)}, 1, &updated,
                               &errorCode)
                    && updated.generation == 2,
                "the CAS fixture failed its second commit")) {
        return;
    }
    const QByteArray generationTwoAuthority = fixture.secure.stored;
    fixture.secure.stored = generationOneAuthority;
    fixture.settings.setValue(
        ExtensionStagingRestoreAuditLedgerStore::recordSettingsKey(),
        generationOnePayload);
    fixture.settings.sync();

    // 控制器的读取与其提交内部的授权复查之间，世界前进到代号 2：本次记录的第
    // 三次读取（控制器 load、replace 内部 load 之后的授权复查）返回前进后的
    // 授权。
    fixture.secure.conflictAuthority = generationTwoAuthority;
    fixture.secure.conflictOnRead = fixture.secure.reads + 3;
    ExtensionStagingRestoreAuditLedgerStore recording = fixture.store();
    const ExtensionStagingRestoreRecordResult conflicted =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &recording);
    expect(!conflicted.recorded
               && conflicted.errorCode == QStringLiteral(
                   "extension-restore-audit-store-generation-conflict"),
           "a concurrent decision was silently overwritten");

    // 历史保持代号 1 的那一份：调用方重新加载后可重新提问并成功。
    fixture.secure.conflictAuthority.clear();
    ExtensionStagingRestoreAuditLedgerStore reloading = fixture.store();
    const ExtensionStagingRestoreAuditStoreResult reloaded = reloading.load();
    if (!expect(reloaded.state == ExtensionStagingRestoreAuditStoreState::Ready
                    && reloaded.generation == 1
                    && reloaded.entries.size() == 1,
                "the conflicting attempt rewrote recorded history")) {
        return;
    }
    ExtensionStagingRestoreAuditLedgerStore retrying = fixture.store();
    const ExtensionStagingRestoreRecordResult retried =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &retrying);
    expect(retried.recorded && retried.ledger.generation == 2
               && retried.ledger.entries.size() == 2,
           "a reloaded retry did not record");
}

// 提交之后重新读取：返回的状态来自重新加载的字节，而不是追加时的预期。提交与
// 重读之间破坏授权字节，控制器必须如实报告失败，而不是报告"决定已记录"。
void testRereadAfterCommitIsTheAuthority()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("reread"), 6,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("reread.ini")));
    // 第二次写入（三阶段提交的最后一笔）落地后破坏授权：重读必须看见损坏。
    fixture.secure.corruptAfterWrites = 2;
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    const ExtensionStagingRestoreRecordResult result =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &store);
    expect(!result.recorded
               && result.ledger.state
                   == ExtensionStagingRestoreAuditStoreState::Invalid
               && result.errorCode == QStringLiteral(
                   "extension-restore-audit-store-authority-invalid"),
           "a sabotaged post-commit ledger was reported as recorded");
}

// 上限边界：MaxEntries 是编译期常量（1024），不可注入，因此直接用存储把账本
// 预填到上限，再经由控制器追加第 1025 条——以独立代号拒绝，历史一字节不动。
void testEntriesCapBoundary()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("cap"), 7,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("cap.ini")));
    QList<ExtensionStagingRestoreAuditEntry> full;
    for (int index = 0;
         index < ExtensionStagingRestoreAuditLedgerStore::MaxEntries;
         ++index) {
        full.append(directEntry(index));
    }
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    ExtensionStagingRestoreAuditStoreResult updated;
    QString errorCode;
    if (!expect(store.replace(full, 0, &updated, &errorCode)
                    && updated.entries.size()
                        == ExtensionStagingRestoreAuditLedgerStore::MaxEntries,
                "the cap fixture failed to fill the ledger")) {
        return;
    }
    const QString identityBefore = updated.identity;
    ExtensionStagingRestoreAuditLedgerStore overCapStore = fixture.store();
    const ExtensionStagingRestoreRecordResult overCap =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            decidedAt(), &overCapStore);
    expect(!overCap.recorded
               && overCap.errorCode == QStringLiteral(
                   "extension-restore-audit-store-entries-cap"),
           "a full audit ledger did not refuse with the cap code");
    expect(fixture.store().load().identity == identityBefore,
           "a rejected over-cap record disturbed the recorded history");
}

// 决定时间必须是规范 UTC：歧义的本地墙钟时间不属于审计记录，被存储以独立
// 代号拒绝，控制器原样透传。
void testNonUtcDecisionTimeRejected()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture chain;
    if (!buildSkillChain(temporary, QStringLiteral("time"), 8,
                         TreeSpec{{{QStringLiteral("a.txt"),
                                    QByteArrayLiteral("alpha\n")}}, {}},
                         &chain)) {
        return;
    }
    Fixture fixture(temporary.filePath(QStringLiteral("time.ini")));
    const QDateTime local = QDateTime::fromString(
        QStringLiteral("2026-09-05T13:00:00.000"), Qt::ISODateWithMs);
    if (!expect(local.isValid() && local.timeSpec() != Qt::UTC,
                "the local-time fixture is not local")) {
        return;
    }
    ExtensionStagingRestoreAuditLedgerStore store = fixture.store();
    const ExtensionStagingRestoreRecordResult result =
        ExtensionStagingRestoreController::record(
            chain.prompt, chain.descriptor.verification,
            acknowledge(chain.prompt,
                        ExtensionStagingRestoreApprovalDecision::Approve),
            local, &store);
    expect(!result.recorded
               && result.errorCode == QStringLiteral(
                   "extension-restore-audit-store-entries-invalid"),
           "an ambiguous local decision time entered the audit record");
    expect(fixture.store().load().state
               == ExtensionStagingRestoreAuditStoreState::Empty,
           "a rejected decision time left history behind");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testFullChainApprovedDecisionRecorded();
    testDeclinedDecisionRecordedWithoutAuthority();
    testPolicyRefusalsWriteNothing();
    testUnreadableLedgerBlocksRecording();
    testCasConflictReportedDistinctly();
    testRereadAfterCommitIsTheAuthority();
    testEntriesCapBoundary();
    testNonUtcDecisionTimeRejected();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore controller guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
