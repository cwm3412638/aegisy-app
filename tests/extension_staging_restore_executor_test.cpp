#include "extension_staging_restore_executor.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <utility>

namespace {

int failures = 0;

const QString kSubject = QStringLiteral("skill:example");

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

// 真实磁盘观察：只在测试临时目录上工作，只读。生产接线注入的正是这个形状。
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

// 受控观察：对 trapPath 的成功内容读取超过 trapAfter 次之后，任何路径的
// nodeKind 查询都报告 Unavailable。用它把一次确定性失败注入到执行中段
// （某条文件操作写后重读完成、下一条操作的逐条复查必然失败），不依赖文件系统
// 权限位——权限位在特权进程下不失效，不能当测试机制。
class FlipObservation final : public ExtensionStagingRestoreObservation
{
public:
    FlipObservation(const QString &root, QString trapPath, int trapAfter)
        : m_disk(root), m_trapPath(std::move(trapPath)),
          m_trapAfter(trapAfter)
    {
    }

    QString canonicalRoot() override { return m_disk.canonicalRoot(); }

    NodeKind nodeKind(const QString &relativePath) override
    {
        if (!relativePath.isEmpty() && m_reads > m_trapAfter) {
            return NodeKind::Unavailable;
        }
        return m_disk.nodeKind(relativePath);
    }

    bool fileContent(const QString &relativePath, QByteArray *content) override
    {
        const bool ok = m_disk.fileContent(relativePath, content);
        if (ok && relativePath == m_trapPath) ++m_reads;
        return ok;
    }

private:
    DiskObservation m_disk;
    QString m_trapPath;
    int m_trapAfter;
    int m_reads = 0;
};

QDateTime fixtureCreatedAt()
{
    return QDateTime::fromString(QStringLiteral("2026-09-06T12:00:00.123Z"),
                                 Qt::ISODateWithMs);
}

QDateTime fixtureNow()
{
    return fixtureCreatedAt().addDays(1);
}

QString fixtureBackupId(int variant)
{
    return QStringLiteral("ext_20260906_120000_%1")
        .arg(variant, 8, 16, QLatin1Char('0'));
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

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
        skillDomain(), canonical, canonical, QString(), 0, &budget, tree,
        &error);
}

// 一棵真实目录树的确定性快照：排序后的 "路径=种类=摘要" 行。前后两次快照逐字节相等，
// 就是"零写入"的证明。
QStringList treeSnapshot(const QString &root)
{
    QStringList lines;
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                        | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString absolute = it.next();
        const QString relative = QDir(root).relativeFilePath(absolute);
        const QFileInfo info = it.fileInfo();
        if (info.isDir()) {
            lines.append(relative + QStringLiteral("=dir"));
            continue;
        }
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly)) {
            lines.append(relative + QStringLiteral("=unreadable"));
            continue;
        }
        lines.append(relative + QStringLiteral("=file=")
                     + sha256Hex(file.readAll()));
    }
    lines.sort();
    return lines;
}

// 一条走完的链路留下的全部中间产物：快照、描述、计划、提示与凭据。
struct ChainFixture {
    ConfigurationBackupSnapshot snapshot;
    ExtensionStagingRestoreBackupDescriptor descriptor;
    QString canonicalDestination;
    ExtensionStagingRestorePlan plan;
    ExtensionStagingRestorePrompt prompt;
    ExtensionStagingRestoreApprovalVerdict verdict;
};

// 完整链路：真实临时目录树 → 捕获 → 加密暂存往返 → 清点层取描述 → 读回并计划 →
// 呈现 → 逐项对齐批准。这正是未来真实调用方的输入链路，执行器消费它的全部产物。
bool buildChain(QTemporaryDir &temporary, const QString &name, int variant,
                const TreeSpec &source, const TreeSpec &destination,
                ChainFixture *fixture)
{
    const QString base = temporary.path() + QLatin1Char('/') + name;
    const QString sourceRoot = base + QStringLiteral("/tree");
    if (!expect(writeTree(sourceRoot, source),
                "the source fixture could not be written")) {
        return false;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(sourceRoot, &tree),
                "the source fixture could not be captured")) {
        return false;
    }
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!ExtensionStagingSnapshot::build(skillDomain(), tree, kSubject,
                                         fixtureBackupId(variant),
                                         fixtureCreatedAt(), &snapshot,
                                         &error)) {
        return expect(false, "fixture snapshot refused");
    }
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(),
        base + QStringLiteral("/backups"), &provider);
    if (!expect(store.create(snapshot, &error),
                "the fixture backup could not be stored")) {
        return false;
    }
    // 备份描述来自清点层，与真实调用方的来源一致。
    ExtensionStagingBackupListResult listing;
    if (!expect(ExtensionStagingBackupInventory::list(
                    base + QStringLiteral("/backups"), kSubject, &listing,
                    &error)
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

    if (!expect(store.read(kSubject, fixtureBackupId(variant),
                           &fixture->snapshot, &error),
                "the fixture backup could not be read back")) {
        return false;
    }
    const QString destinationRoot = base + QStringLiteral("/dest");
    if (!expect(writeTree(destinationRoot, destination),
                "the destination fixture could not be written")) {
        return false;
    }
    DiskObservation observation(destinationRoot);
    fixture->canonicalDestination = observation.canonicalRoot();
    if (!expect(ExtensionStagingRestorePlanBuilder::plan(
                    skillDomain(), kSubject, fixture->snapshot,
                    fixture->canonicalDestination, &observation, &fixture->plan,
                    &error),
                "the fixture could not be planned")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return false;
    }
    fixture->prompt = ExtensionStagingRestorePresentation::build(
        fixture->plan, fixture->descriptor, fixture->canonicalDestination,
        fixtureNow());
    if (!expect(fixture->prompt.state
                    == ExtensionStagingRestorePromptState::Ready,
                "a clean fixture plan was not presentable")) {
        QTextStream(stderr) << "  presentation said: "
                            << fixture->prompt.errorCode << '\n';
        return false;
    }
    // 与提示逐项对齐的批准：唯一应当被授权的形状。
    ExtensionStagingRestoreApprovalAcknowledgement acknowledgement;
    acknowledgement.decision = ExtensionStagingRestoreApprovalDecision::Approve;
    acknowledgement.subject = fixture->prompt.subject;
    acknowledgement.backupId = fixture->prompt.backupId;
    acknowledgement.destinationRoot = fixture->prompt.destinationRoot;
    acknowledgement.approvedPlanIdentity = fixture->prompt.echoedPlanIdentity;
    acknowledgement.approvedTreeIdentity = fixture->prompt.echoedTreeIdentity;
    acknowledgement.acknowledgedWarnings = fixture->prompt.warnings;
    bool highRisk = false;
    for (const ExtensionStagingRestoreWarning warning :
         fixture->prompt.warnings) {
        if (ExtensionStagingRestoreApprovalPolicy::
                requiresExplicitConfirmation(warning,
                                             fixture->prompt.fileWriteCount)) {
            highRisk = true;
        }
    }
    acknowledgement.highRiskConfirmed = highRisk;
    fixture->verdict = ExtensionStagingRestoreApprovalPolicy::evaluate(
        fixture->prompt, fixture->descriptor.verification, acknowledgement);
    if (!expect(fixture->verdict.state
                    == ExtensionStagingRestoreApprovalState::Authorized,
                "an aligned fixture approval was not authorized")) {
        QTextStream(stderr) << "  approval said: "
                            << fixture->verdict.errorCode << '\n';
        return false;
    }
    return true;
}

using Executor = ExtensionStagingRestoreExecutor;
using State = ExtensionStagingRestoreExecutionState;
using Outcome = ExtensionStagingRestoreOperationOutcome;

ExtensionStagingRestoreExecutionResult execute(
        const ChainFixture &fixture,
        ExtensionStagingRestoreObservation *observation)
{
    return Executor::execute(skillDomain(), kSubject, fixture.snapshot,
                             fixture.plan, fixture.verdict, observation);
}

void testHappyPathComplete()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const TreeSpec source{
        {{QStringLiteral("a.txt"), QByteArrayLiteral("alpha\n")},
         {QStringLiteral("sub/b.txt"), QByteArrayLiteral("beta\n")}},
        {QStringLiteral("sub"), QStringLiteral("zdir")}};
    ChainFixture fixture;
    if (!buildChain(temporary, QStringLiteral("happy"), 1, source, TreeSpec{},
                    &fixture)) {
        return;
    }
    const QStringList before = treeSnapshot(fixture.canonicalDestination);
    DiskObservation observation(fixture.canonicalDestination);
    const ExtensionStagingRestoreExecutionResult result =
        execute(fixture, &observation);

    expect(result.state == State::Complete && result.errorCode.isEmpty()
               && result.failureIndex == -1,
           "a clean approved restore did not complete");
    // 四条操作：两个目录创建 + 两个文件写入，全部 Done 且逐条验证。
    expect(result.operations.size() == 4 && result.doneCount == 4
               && result.skippedVerifiedCount == 0 && result.failedCount == 0,
           "the completed restore did not verify every operation");
    for (const ExtensionStagingRestoreOperationResult &operation :
         result.operations) {
        if (!expect(operation.outcome == Outcome::Done,
                    "a completed restore holds a non-done operation")) {
            break;
        }
    }
    expect(result.planIdentity == fixture.plan.planIdentity
               && result.treeIdentity == fixture.plan.treeIdentity,
           "the result does not echo the executed identities");
    // 目标树与来源树逐字节一致。
    const QStringList after = treeSnapshot(fixture.canonicalDestination);
    expect(after != before, "the restore wrote nothing at all");
    const QString sourceRoot =
        temporary.path() + QStringLiteral("/happy/tree");
    expect(treeSnapshot(QFileInfo(sourceRoot).canonicalFilePath()) == after,
           "the destination is not byte-identical to the source tree");
}

void testCredentialMismatch()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture first;
    ChainFixture second;
    if (!buildChain(temporary, QStringLiteral("first"), 2,
                    {{{QStringLiteral("a.txt"), QByteArrayLiteral("one\n")}},
                     {}},
                    TreeSpec{}, &first)
            || !buildChain(temporary, QStringLiteral("second"), 3,
                           {{{QStringLiteral("b.txt"),
                              QByteArrayLiteral("two\n")}},
                            {}},
                           TreeSpec{}, &second)) {
        return;
    }

    // 给计划 A 签发的凭据执行计划 B：拒绝，零写入（目标前后快照逐字节一致）。
    {
        const QStringList before = treeSnapshot(second.canonicalDestination);
        DiskObservation observation(second.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            Executor::execute(skillDomain(), kSubject, second.snapshot,
                              second.plan, first.verdict, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-credential-plan-mismatch"),
               "plan B executed under plan A's credential");
        expect(treeSnapshot(second.canonicalDestination) == before,
               "a credential mismatch still wrote bytes");
    }
    // 计划身份对齐但树身份来自另一份内容：双身份缺一不可。
    {
        ExtensionStagingRestoreApprovalVerdict doctored = second.verdict;
        doctored.authorizedTreeIdentity = first.verdict.authorizedTreeIdentity;
        DiskObservation observation(second.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            Executor::execute(skillDomain(), kSubject, second.snapshot,
                              second.plan, doctored, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-credential-tree-mismatch"),
               "a tree-identity mismatch was not refused");
    }
    // 非授权凭据（被拒绝的决定不携带任何授权）。
    {
        ExtensionStagingRestoreApprovalVerdict declined;
        declined.state = ExtensionStagingRestoreApprovalState::Refused;
        DiskObservation observation(second.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            Executor::execute(skillDomain(), kSubject, second.snapshot,
                              second.plan, declined, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-credential-not-authorized"),
               "a declined verdict granted execution authority");
    }
    // 观察不可用：绝不盲写。
    {
        const ExtensionStagingRestoreExecutionResult result =
            Executor::execute(skillDomain(), kSubject, second.snapshot,
                              second.plan, second.verdict, nullptr);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-destination-unavailable"),
               "a missing observation did not fail closed");
    }
    // 篡改过的快照：执行开始时的重新验证原样透传快照诊断，零写入。
    {
        ChainFixture tampered = second;
        QByteArray content = tampered.snapshot.files.at(1).content;
        content[0] = content.at(0) == 'A' ? 'B' : 'A';
        tampered.snapshot.files[1].content = content;
        const QStringList before = treeSnapshot(second.canonicalDestination);
        DiskObservation observation(second.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            Executor::execute(skillDomain(), kSubject, tampered.snapshot,
                              tampered.plan, tampered.verdict, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-staging-snapshot-content-digest-mismatch"),
               "a tampered snapshot reached execution");
        expect(treeSnapshot(second.canonicalDestination) == before,
               "a tampered snapshot still wrote bytes");
    }
}

void testDriftBetweenPlanAndExecute()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QByteArray alpha = QByteArrayLiteral("alpha\n");

    // 计划之后目标被弄脏：应为缺失的路径上出现了内容。
    {
        ChainFixture fixture;
        if (!buildChain(temporary, QStringLiteral("drift-add"), 4,
                        {{{QStringLiteral("a.txt"), alpha}}, {}}, TreeSpec{},
                        &fixture)) {
            return;
        }
        if (!expect(writeFile(fixture.canonicalDestination
                                  + QStringLiteral("/unplanned.txt"),
                              QByteArrayLiteral("intruder\n")),
                    "the drift fixture could not be written")) {
            return;
        }
        // 未计划路径上的新文件不在计划的观察面上——它不是漂移；把它挪到计划
        // 路径上才是。
        if (!expect(writeFile(fixture.canonicalDestination
                                  + QStringLiteral("/a.txt"),
                              QByteArrayLiteral("dirty\n")),
                    "the drift fixture could not dirty the planned path")) {
            return;
        }
        const QStringList before = treeSnapshot(fixture.canonicalDestination);
        DiskObservation observation(fixture.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            execute(fixture, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-destination-drift"),
               "a drifted destination was executed against");
        expect(treeSnapshot(fixture.canonicalDestination) == before,
               "a drifted destination still received writes");
    }
    // already-in-place 条目自计划以来被改动：那是冲突，不是跳过。
    {
        ChainFixture fixture;
        if (!buildChain(temporary, QStringLiteral("drift-inplace"), 5,
                        {{{QStringLiteral("a.txt"), alpha}}, {}},
                        {{{QStringLiteral("a.txt"), alpha}}, {}},
                        &fixture)) {
            return;
        }
        if (!expect(fixture.plan.operations.size() == 1
                        && fixture.plan.operations.at(0).alreadyInPlace,
                    "the in-place drift fixture was not already-in-place")) {
            return;
        }
        if (!expect(writeFile(fixture.canonicalDestination
                                  + QStringLiteral("/a.txt"),
                              QByteArrayLiteral("drifted\n")),
                    "the in-place drift fixture could not be modified")) {
            return;
        }
        const QStringList before = treeSnapshot(fixture.canonicalDestination);
        DiskObservation observation(fixture.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            execute(fixture, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-destination-drift"),
               "a drifted already-in-place entry was silently skipped");
        expect(treeSnapshot(fixture.canonicalDestination) == before,
               "a drifted already-in-place entry still received writes");
    }
    // 计划与执行之间引入符号链接：独立拒绝，零写入。
    {
        ChainFixture fixture;
        if (!buildChain(temporary, QStringLiteral("drift-symlink"), 6,
                        {{{QStringLiteral("a.txt"), alpha}}, {}}, TreeSpec{},
                        &fixture)) {
            return;
        }
        const QString linkPath =
            fixture.canonicalDestination + QStringLiteral("/a.txt");
        if (!expect(QFile::link(QStringLiteral("/etc/hosts"), linkPath),
                    "the symlink fixture could not be created")) {
            return;
        }
        const QStringList before = treeSnapshot(fixture.canonicalDestination);
        DiskObservation observation(fixture.canonicalDestination);
        const ExtensionStagingRestoreExecutionResult result =
            execute(fixture, &observation);
        expect(result.state == State::Refused
                   && result.errorCode == QStringLiteral(
                       "extension-restore-execution-symlink-component"),
               "a symlink introduced after planning was executed over");
        expect(treeSnapshot(fixture.canonicalDestination) == before,
               "a symlinked destination still received writes");
        QFile::remove(linkPath);
    }
}

void testMidExecutionFailure()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QByteArray alpha = QByteArrayLiteral("alpha\n");
    const QByteArray beta = QByteArrayLiteral("beta\n");
    const TreeSpec source{
        {{QStringLiteral("a.txt"), alpha},
         {QStringLiteral("sub/b.txt"), beta},
         {QStringLiteral("c.txt"), QByteArrayLiteral("gamma\n")}},
        {QStringLiteral("sub")}};
    ChainFixture fixture;
    if (!buildChain(temporary, QStringLiteral("partial"), 7, source,
                    TreeSpec{}, &fixture)) {
        return;
    }
    // 计划顺序：创建 sub，写 a.txt、c.txt、sub/b.txt（捕获层按目录层序，同层
    // 原始 UTF-8 排序）。陷阱挂在 c.txt 上：它的写后重读（第一次 fileContent）
    // 之后，任何 nodeKind 查询都报告 Unavailable——下一条操作 sub/b.txt 的逐条
    // 祖先复查因此确定性失败。
    if (!expect(fixture.plan.operations.size() == 4
                    && fixture.plan.operations.at(3).relativePath
                        == QStringLiteral("sub/b.txt"),
                "the plan order moved; re-anchor the mid-execution trap")) {
        return;
    }
    FlipObservation observation(fixture.canonicalDestination,
                                QStringLiteral("c.txt"), 0);
    const ExtensionStagingRestoreExecutionResult result =
        execute(fixture, &observation);

    expect(result.state == State::Partial,
           "a mid-execution failure was not reported as a mixed state");
    expect(result.errorCode == QStringLiteral(
               "extension-restore-execution-destination-unavailable"),
           "the failure point does not carry its exact code");
    expect(result.failureIndex == 3 && result.operations.size() == 4,
           "the failure point is not exactly the fourth operation");
    expect(result.doneCount == 3 && result.failedCount == 1
               && result.skippedVerifiedCount == 0,
           "the completed operations were not reported exactly");
    expect(result.operations.at(0).outcome == Outcome::Done
               && result.operations.at(1).outcome == Outcome::Done
               && result.operations.at(2).outcome == Outcome::Done
               && result.operations.at(3).outcome == Outcome::Failed,
           "the per-operation outcomes do not match the failure point");
    // 失败点之前的字节在盘上保持完整；失败的操作不留半截字节。
    expect(treeSnapshot(fixture.canonicalDestination)
               == QStringList{
                   QStringLiteral("a.txt=file=") + sha256Hex(alpha),
                   QStringLiteral("c.txt=file=")
                       + sha256Hex(QByteArrayLiteral("gamma\n")),
                   QStringLiteral("sub=dir")},
           "the completed prefix is not intact and the failed write left "
           "residue");
}

void testPathEscapeHandBuiltPlan()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    ChainFixture fixture;
    if (!buildChain(temporary, QStringLiteral("escape"), 8,
                    {{{QStringLiteral("a.txt"), QByteArrayLiteral("x\n")}},
                     {}},
                    TreeSpec{}, &fixture)) {
        return;
    }
    // 手工构造的计划：身份、主体、目标根全部照抄（凭据逐项对齐），但混入一条
    // 逃逸路径。凭据绑定防的是换计划，防不了计划本身带毒——逐条包含性重查是
    // 最后一道防线。
    ExtensionStagingRestorePlan forged = fixture.plan;
    ExtensionStagingRestoreOperation escape;
    escape.directory = false;
    escape.relativePath = QStringLiteral("../escape.txt");
    escape.byteCount = 2;
    escape.sha256 = sha256Hex(QByteArrayLiteral("x\n"));
    escape.sourceSlot = 1;
    forged.operations.prepend(escape);
    const QStringList before = treeSnapshot(fixture.canonicalDestination);
    DiskObservation observation(fixture.canonicalDestination);
    const ExtensionStagingRestoreExecutionResult result =
        Executor::execute(skillDomain(), kSubject, fixture.snapshot, forged,
                          fixture.verdict, &observation);
    expect(result.state == State::Refused
               && result.errorCode == QStringLiteral(
                   "extension-restore-execution-path-escapes-destination"),
           "a hand-built escaping plan reached the filesystem");
    expect(treeSnapshot(fixture.canonicalDestination) == before,
           "an escaping plan still wrote bytes");
    expect(!QFile::exists(temporary.path()
                              + QStringLiteral("/escape/escape.txt")),
           "an escaping plan created a file outside the destination");
}

void testAlreadyInPlaceNoWrite()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QByteArray alpha = QByteArrayLiteral("alpha\n");
    ChainFixture fixture;
    if (!buildChain(temporary, QStringLiteral("inplace"), 9,
                    {{{QStringLiteral("a.txt"), alpha}}, {}},
                    {{{QStringLiteral("a.txt"), alpha}}, {}}, &fixture)) {
        return;
    }
    if (!expect(fixture.plan.operations.size() == 1
                    && fixture.plan.operations.at(0).alreadyInPlace,
                "the in-place fixture was not planned as already-in-place")) {
        return;
    }
    // 把既有文件的 mtime 钉到一个已知的过去时刻：跳过并复核之后它必须不变，
    // 这才是"没有发生写入"的证据。
    const QString filePath =
        fixture.canonicalDestination + QStringLiteral("/a.txt");
    const QDateTime pinned = QDateTime::fromSecsSinceEpoch(1600000000);
    QFile file(filePath);
    if (!expect(file.open(QIODevice::ReadOnly)
                    && file.setFileTime(pinned,
                                        QFileDevice::FileModificationTime),
                "the in-place fixture mtime could not be pinned")) {
        return;
    }
    file.close();

    DiskObservation observation(fixture.canonicalDestination);
    const ExtensionStagingRestoreExecutionResult result =
        execute(fixture, &observation);
    expect(result.state == State::Complete && result.doneCount == 0
               && result.skippedVerifiedCount == 1
               && result.operations.at(0).outcome == Outcome::SkippedVerified,
           "an already-in-place restore did not skip-and-verify");
    expect(QFileInfo(filePath).lastModified() == pinned,
           "an already-in-place entry was rewritten");
    expect(QFileInfo(filePath).size() == alpha.size(),
           "an already-in-place entry changed size");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testHappyPathComplete();
    testCredentialMismatch();
    testDriftBetweenPlanAndExecute();
    testMidExecutionFailure();
    testPathEscapeHandBuiltPlan();
    testAlreadyInPlaceNoWrite();
    if (failures == 0) {
        QTextStream(stdout)
            << "extension staging restore executor guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
