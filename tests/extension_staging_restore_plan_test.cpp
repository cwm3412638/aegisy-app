#include "extension_staging_restore_plan.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

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

QString fixtureBackupId(int variant)
{
    return QStringLiteral("ext_20260905_120000_%1")
        .arg(variant, 8, 16, QLatin1Char('0'));
}

ConfigurationBackupSnapshot buildOrDie(
        const QVector<ExtensionTreeCaptureEntry> &tree, int variant)
{
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!ExtensionStagingSnapshot::build(skillDomain(), tree, kSubject,
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

// 注入式假观察：测试用它模拟任意目标状态（脏树、符号链接、不可用）而不触碰真实产品
// 目录。
class FakeObservation final : public ExtensionStagingRestoreObservation
{
public:
    struct Node {
        NodeKind kind = NodeKind::Missing;
        QByteArray content;
    };

    QString root;
    bool rootUnavailable = false;
    QMap<QString, Node> nodes;
    QSet<QString> unavailablePaths;

    QString canonicalRoot() override
    {
        return rootUnavailable ? QString() : root;
    }

    NodeKind nodeKind(const QString &relativePath) override
    {
        if (unavailablePaths.contains(relativePath)) {
            return NodeKind::Unavailable;
        }
        const auto it = nodes.constFind(relativePath);
        return it == nodes.constEnd() ? NodeKind::Missing : it->kind;
    }

    bool fileContent(const QString &relativePath, QByteArray *content) override
    {
        if (!content || unavailablePaths.contains(relativePath)) return false;
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

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

bool planOrCode(const ConfigurationBackupSnapshot &snapshot,
                const QString &destination,
                ExtensionStagingRestoreObservation *observation,
                ExtensionStagingRestorePlan *plan, QString *error)
{
    return ExtensionStagingRestorePlanBuilder::plan(
        skillDomain(), kSubject, snapshot, destination, observation, plan,
        error);
}

void expectPlanRefused(const ConfigurationBackupSnapshot &snapshot,
                       FakeObservation *observation,
                       const QString &expectedCode, const char *message)
{
    ExtensionStagingRestorePlan plan;
    QString error;
    if (!expect(!planOrCode(snapshot, observation->root, observation, &plan,
                            &error),
                message)) {
        return;
    }
    if (!expect(error == expectedCode, message)) {
        QTextStream(stderr) << "  expected " << expectedCode << " but got "
                            << error << '\n';
    }
    expect(plan.operations.isEmpty() && plan.planIdentity.isEmpty(),
           "a refused plan still produced operations");
}

void testRoundTrip()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("beta\n"))
                    && QDir().mkpath(root + QStringLiteral("/zdir")),
                "round-trip fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "round-trip fixture could not be captured")) {
        return;
    }
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 1);
    if (built.files.isEmpty()) return;

    // 穿过加密暂存域往返，再验证，再计划：计划层的输入链路与未来真实调用方一致。
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(),
        temporary.path() + QStringLiteral("/backups"), &provider);
    QString error;
    if (!expect(store.create(built, &error),
                "the round-trip fixture could not be stored")) {
        return;
    }
    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSubject, fixtureBackupId(1), &readBack, &error),
                "the round-trip fixture could not be read back")) {
        return;
    }

    // 目标：第二个空的临时目录。
    const QString destination = temporary.path() + QStringLiteral("/dest");
    if (!expect(QDir().mkpath(destination),
                "the destination fixture could not be created")) {
        return;
    }
    DiskObservation observation(destination);
    ExtensionStagingRestorePlan plan;
    if (!expect(planOrCode(readBack, observation.canonicalRoot(),
                           &observation, &plan, &error),
                "a clean destination was refused")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }

    // 目录创建在前、文件写入在后；目录两条（sub、zdir），文件两条（a.txt、sub/b.txt）。
    if (!expect(plan.operations.size() == 4,
                "the plan does not hold exactly four operations")) {
        return;
    }
    expect(plan.operations.at(0).directory
               && plan.operations.at(0).relativePath
                   == QStringLiteral("sub")
               && !plan.operations.at(0).alreadyInPlace,
           "the first operation is not the sub directory creation");
    expect(plan.operations.at(1).directory
               && plan.operations.at(1).relativePath
                   == QStringLiteral("zdir"),
           "the second operation is not the zdir directory creation");
    const ExtensionStagingRestoreOperation &first = plan.operations.at(2);
    const ExtensionStagingRestoreOperation &second = plan.operations.at(3);
    expect(!first.directory && first.relativePath == QStringLiteral("a.txt")
               && first.byteCount == 6
               && first.sha256 == sha256Hex(QByteArrayLiteral("alpha\n"))
               && first.sourceSlot == 1 && !first.alreadyInPlace,
           "the a.txt write does not carry the manifest digest and slot");
    expect(!second.directory
               && second.relativePath == QStringLiteral("sub/b.txt")
               && second.byteCount == 5
               && second.sha256 == sha256Hex(QByteArrayLiteral("beta\n"))
               && second.sourceSlot == 2 && !second.alreadyInPlace,
           "the sub/b.txt write does not carry the manifest digest and slot");
    expect(plan.destinationRoot == observation.canonicalRoot()
               && plan.subject == kSubject
               && plan.treeIdentity.startsWith(
                   QStringLiteral("extension-content:sha256:"))
               && plan.planIdentity.startsWith(
                   QStringLiteral("extension-staging-restore-plan:sha256:")),
           "the plan identity fields do not match the verified snapshot");

    // 确定性：对同一份快照与同一个目标计划两次，身份逐字节一致。
    DiskObservation againObservation(destination);
    ExtensionStagingRestorePlan again;
    expect(planOrCode(readBack, observation.canonicalRoot(),
                      &againObservation, &again, &error)
               && again.planIdentity == plan.planIdentity,
           "replanning the same inputs produced a different identity");
}

void testAlreadyInPlace()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    const QByteArray alpha = QByteArrayLiteral("alpha\n");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"), alpha),
                "in-place fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "in-place fixture could not be captured")) {
        return;
    }
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 2);
    if (built.files.isEmpty()) return;

    // 目标已经持有逐字节一致的内容：可计划，且唯一操作标记为 already-in-place。
    FakeObservation inPlace = emptyDestination(
        temporary.path() + QStringLiteral("/dest"));
    inPlace.nodes.insert(QStringLiteral("a.txt"),
                         {FakeObservation::NodeKind::File, alpha});
    ExtensionStagingRestorePlan plan;
    QString error;
    if (!expect(planOrCode(built, inPlace.root, &inPlace, &plan, &error),
                "an identical-content destination was refused")) {
        QTextStream(stderr) << "  plan said: " << error << '\n';
        return;
    }
    expect(plan.operations.size() == 1 && plan.operations.at(0).alreadyInPlace,
           "an identical file was not marked already-in-place");

    // already-in-place 是计划身份的输入之一：同一快照对空目标与一致目标的计划身份不同。
    FakeObservation empty = emptyDestination(inPlace.root);
    ExtensionStagingRestorePlan emptyPlan;
    expect(planOrCode(built, empty.root, &empty, &emptyPlan, &error)
               && emptyPlan.planIdentity != plan.planIdentity,
           "an already-in-place flag did not change the plan identity");
}

void testDestinationValidation()
{
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("a.txt"), false, QByteArrayLiteral("x")});
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 3);
    if (built.files.isEmpty()) return;
    const QString invalid =
        QStringLiteral("extension-staging-restore-destination-invalid");
    const QString unavailable =
        QStringLiteral("extension-staging-restore-destination-unavailable");

    // 空目标与相对目标在触碰观察接口之前拒绝。
    {
        FakeObservation observation =
            emptyDestination(QStringLiteral("/nonexistent"));
        ExtensionStagingRestorePlan plan;
        QString error;
        expect(!planOrCode(built, QString(), &observation, &plan, &error)
                   && error == invalid,
               "an empty destination root was not refused");
        expect(!planOrCode(built, QStringLiteral("relative/dir"),
                           &observation, &plan, &error)
                   && error == invalid,
               "a relative destination root was not refused");
    }
    // 目标与观察报告的规范化形式不一致：非规范化目标拒绝。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        ExtensionStagingRestorePlan plan;
        QString error;
        expect(!planOrCode(built,
                           QStringLiteral("/nonexistent/../nonexistent/dest"),
                           &observation, &plan, &error)
                   && error == invalid,
               "a non-canonical destination root was not refused");
    }
    // 观察不可用：规范化形式给不出、目标根状态读不出、路径状态读不出，都拒绝而不是
    // 盲计划。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.rootUnavailable = true;
        expectPlanRefused(built, &observation, unavailable,
                          "an unavailable root observation was planned blind");
    }
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.unavailablePaths.insert(QString());
        expectPlanRefused(built, &observation, unavailable,
                          "an unreadable root node was planned blind");
    }
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.unavailablePaths.insert(QStringLiteral("a.txt"));
        expectPlanRefused(built, &observation, unavailable,
                          "an unreadable planned path was planned blind");
    }
    // 目标根本身是符号链接：独立诊断。
    {
        FakeObservation observation;
        observation.root = QStringLiteral("/nonexistent/dest");
        observation.nodes.insert(QString(),
                                 {FakeObservation::NodeKind::Symlink, {}});
        expectPlanRefused(
            built, &observation,
            QStringLiteral("extension-staging-restore-root-symlink"),
            "a symlink destination root was not refused");
    }
    // 目标根缺失或是普通文件：不是可恢复的目标。
    for (const FakeObservation::NodeKind kind : {
             FakeObservation::NodeKind::Missing,
             FakeObservation::NodeKind::File,
             FakeObservation::NodeKind::Other}) {
        FakeObservation observation;
        observation.root = QStringLiteral("/nonexistent/dest");
        observation.nodes.insert(QString(), {kind, {}});
        expectPlanRefused(built, &observation, invalid,
                          "a non-directory destination root was not refused");
    }
}

void testConflictsAndSymlinks()
{
    const QByteArray alpha = QByteArrayLiteral("alpha\n");
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("sub"), true, {}});
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("sub/b.txt"), false, alpha});
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 4);
    if (built.files.isEmpty()) return;
    const QString conflict =
        QStringLiteral("extension-staging-restore-destination-conflict");
    const QString symlink =
        QStringLiteral("extension-staging-restore-symlink-component");

    // 脏树：已有文件内容与计划不符，拒绝而不是静默覆盖。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub"), {FakeObservation::NodeKind::Directory, {}});
        observation.nodes.insert(
            QStringLiteral("sub/b.txt"),
            {FakeObservation::NodeKind::File, QByteArrayLiteral("omega\n")});
        expectPlanRefused(built, &observation, conflict,
                          "a dirty destination was planned over");
    }
    // 类型冲突：目录位置上已有文件、文件位置上已有目录。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub"), {FakeObservation::NodeKind::File, alpha});
        expectPlanRefused(built, &observation, conflict,
                          "a file blocking a planned directory was accepted");
    }
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub/b.txt"),
            {FakeObservation::NodeKind::Directory, {}});
        expectPlanRefused(built, &observation, conflict,
                          "a directory blocking a planned file was accepted");
    }
    // 符号链接：在计划路径本身与在中间组件上，各自都是独立拒绝。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub"), {FakeObservation::NodeKind::Directory, {}});
        observation.nodes.insert(
            QStringLiteral("sub/b.txt"),
            {FakeObservation::NodeKind::Symlink, {}});
        expectPlanRefused(built, &observation, symlink,
                          "a symlink at a planned file path was accepted");
    }
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub"), {FakeObservation::NodeKind::Symlink, {}});
        expectPlanRefused(built, &observation, symlink,
                          "a symlink at an intermediate component was accepted");
    }
    // 深路径且清单不含目录条目（手工构造的合法树）：符号链接藏在中间组件上，只有逐段
    // 祖先检查能发现它——路径本身与计划条目对观察接口都表现为缺失。
    {
        QVector<ExtensionTreeCaptureEntry> deepTree;
        deepTree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("sub/inner/b.txt"), false, alpha});
        const ConfigurationBackupSnapshot deep = buildOrDie(deepTree, 8);
        if (!deep.files.isEmpty()) {
            FakeObservation observation = emptyDestination(
                QStringLiteral("/nonexistent/dest"));
            observation.nodes.insert(
                QStringLiteral("sub"),
                {FakeObservation::NodeKind::Directory, {}});
            observation.nodes.insert(
                QStringLiteral("sub/inner"),
                {FakeObservation::NodeKind::Symlink, {}});
            expectPlanRefused(
                deep, &observation, symlink,
                "a symlinked intermediate directory on a deep path was "
                "accepted");
        }
    }
    // 中间组件被普通文件占用：目录无法成立，按冲突拒绝。
    {
        FakeObservation observation = emptyDestination(
            QStringLiteral("/nonexistent/dest"));
        observation.nodes.insert(
            QStringLiteral("sub"), {FakeObservation::NodeKind::File, alpha});
        observation.nodes.remove(QStringLiteral("sub/b.txt"));
        expectPlanRefused(built, &observation, conflict,
                          "a file at an intermediate component was accepted");
    }
}

// 手工构造一份聚合字节超过暂存域载荷上限、但逐条都通过验证的快照：验证层守住条目形状
// 与逐槽摘要，但不单独守聚合字节；计划层必须自己守住。
ConfigurationBackupSnapshot oversizedAggregateSnapshot()
{
    const ConfigurationBackupStoreDomain staging =
        ConfigurationBackupStore::extensionStagingDomain();
    const QByteArray chunk(static_cast<int>(staging.maxFileBytes), 'x');
    QVector<ExtensionTreeCaptureEntry> tree;
    qint64 aggregate = 0;
    int index = 0;
    while (aggregate <= staging.maxPayloadBytes) {
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("f%1.bin").arg(index), false, chunk});
        aggregate += chunk.size();
        ++index;
    }
    QJsonArray entries;
    for (int file = 0; file < tree.size(); ++file) {
        QJsonObject entry;
        entry.insert(QStringLiteral("byte_count"),
                     static_cast<int>(chunk.size()));
        entry.insert(QStringLiteral("kind"), QStringLiteral("file"));
        entry.insert(QStringLiteral("path"), tree.at(file).relativePath);
        entry.insert(QStringLiteral("sha256"), sha256Hex(chunk));
        entry.insert(QStringLiteral("slot"), file + 1);
        entries.append(entry);
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("entries"), entries);
    manifest.insert(QStringLiteral("file_count"),
                    static_cast<int>(tree.size()));
    manifest.insert(QStringLiteral("format"),
                    ExtensionStagingSnapshot::manifestFormat());
    manifest.insert(QStringLiteral("identity"),
                    ExtensionTreeCapture::contentIdentity(skillDomain(), tree));
    manifest.insert(QStringLiteral("subject"), kSubject);
    manifest.insert(QStringLiteral("version"), 1);
    ConfigurationBackupSnapshot snapshot;
    snapshot.backupId = fixtureBackupId(5);
    snapshot.tool = kSubject;
    snapshot.createdAt = fixtureCreatedAt();
    snapshot.files.append(ConfigurationBackupFile{
        0, true, QJsonDocument(manifest).toJson(QJsonDocument::Compact)});
    for (int file = 0; file < tree.size(); ++file) {
        snapshot.files.append(ConfigurationBackupFile{file + 1, true, chunk});
    }
    return snapshot;
}

void testBoundsDefenseInDepth()
{
    const ConfigurationBackupSnapshot snapshot = oversizedAggregateSnapshot();
    if (!expect(!snapshot.files.isEmpty(),
                "the oversize fixture could not be built")) {
        return;
    }
    // 前提：这份快照确实通过验证——否则测试证明的是验证而不是计划的纵深防御。
    QString error;
    if (!expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                 snapshot, &error),
                "the oversize fixture did not pass verification")) {
        QTextStream(stderr) << "  verify said: " << error << '\n';
        return;
    }
    FakeObservation observation = emptyDestination(
        QStringLiteral("/nonexistent/dest"));
    expectPlanRefused(
        snapshot, &observation,
        QStringLiteral("extension-staging-restore-bounds-exceeded"),
        "a plan exceeding the staging payload bound was not refused");
}

void testPlanIdentityBindsDestinationAndContent()
{
    QVector<ExtensionTreeCaptureEntry> tree;
    tree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("a.txt"), false, QByteArrayLiteral("x")});
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 6);
    if (built.files.isEmpty()) return;

    FakeObservation first = emptyDestination(QStringLiteral("/dest/one"));
    FakeObservation second = emptyDestination(QStringLiteral("/dest/two"));
    ExtensionStagingRestorePlan firstPlan;
    ExtensionStagingRestorePlan secondPlan;
    QString error;
    if (!expect(planOrCode(built, first.root, &first, &firstPlan, &error)
                    && planOrCode(built, second.root, &second, &secondPlan,
                                  &error),
                "the identity fixtures could not be planned")) {
        return;
    }
    expect(firstPlan.planIdentity != secondPlan.planIdentity,
           "two destinations share one plan identity");

    // 同一目标、不同内容：身份不同。
    QVector<ExtensionTreeCaptureEntry> otherTree;
    otherTree.append(ExtensionTreeCaptureEntry{
        QStringLiteral("a.txt"), false, QByteArrayLiteral("y")});
    const ConfigurationBackupSnapshot other = buildOrDie(otherTree, 7);
    FakeObservation third = emptyDestination(first.root);
    ExtensionStagingRestorePlan thirdPlan;
    expect(planOrCode(other, third.root, &third, &thirdPlan, &error)
               && thirdPlan.planIdentity != firstPlan.planIdentity,
           "two trees share one plan identity");

    // 篡改过的快照在计划开始前就被验证拒绝：诊断来自快照契约，计划为空。
    ConfigurationBackupSnapshot tampered = built;
    QByteArray content = tampered.files.at(1).content;
    content[0] = content.at(0) == 'A' ? 'B' : 'A';
    tampered.files[1].content = content;
    FakeObservation fourth = emptyDestination(first.root);
    ExtensionStagingRestorePlan refusedPlan;
    expect(!planOrCode(tampered, fourth.root, &fourth, &refusedPlan, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-content-digest-mismatch")
               && refusedPlan.operations.isEmpty(),
           "a tampered snapshot reached planning");

    // 逃逸形状的路径同样走不到计划层：验证先拒绝。
    {
        ConfigurationBackupSnapshot escaping = built;
        QJsonObject manifest =
            QJsonDocument::fromJson(escaping.files.at(0).content).object();
        QJsonArray entries =
            manifest.value(QStringLiteral("entries")).toArray();
        QJsonObject entry = entries.at(0).toObject();
        entry.insert(QStringLiteral("path"),
                     QStringLiteral("../escape.txt"));
        entries[0] = entry;
        manifest.insert(QStringLiteral("entries"), entries);
        escaping.files[0].content =
            QJsonDocument(manifest).toJson(QJsonDocument::Compact);
        FakeObservation fifth = emptyDestination(first.root);
        expect(!planOrCode(escaping, fifth.root, &fifth, &refusedPlan, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-path-invalid")
                   && refusedPlan.operations.isEmpty(),
               "an escaping manifest path reached planning");
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testRoundTrip();
    testAlreadyInPlace();
    testDestinationValidation();
    testConflictsAndSymlinks();
    testBoundsDefenseInDepth();
    testPlanIdentityBindsDestinationAndContent();
    if (failures == 0) {
        QTextStream(stdout) << "extension staging restore plan guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
