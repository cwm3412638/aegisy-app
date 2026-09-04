#include "extension_staging_snapshot.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

#include <functional>

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

ExtensionTreeCaptureDomain bundleDomain()
{
    return {QByteArrayLiteral("aegisy-extension-bundle-content/0.1\0"),
            QStringLiteral("extension-content:sha256:"),
            QStringLiteral("extension-bundle")};
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
    return QDateTime::fromString(QStringLiteral("2026-09-04T12:00:00.123Z"),
                                 Qt::ISODateWithMs);
}

QString fixtureBackupId(int variant)
{
    return QStringLiteral("ext_20260904_120000_%1")
        .arg(variant, 8, 16, QLatin1Char('0'));
}

ConfigurationBackupSnapshot buildOrDie(
        const QVector<ExtensionTreeCaptureEntry> &tree, int variant)
{
    ConfigurationBackupSnapshot snapshot;
    QString error;
    if (!ExtensionStagingSnapshot::build(skillDomain(), tree, kSubject,
                                         fixtureBackupId(variant),
                                         fixtureCreatedAt(), &snapshot, &error)) {
        QTextStream(stderr) << "FAIL: fixture snapshot refused: " << error << '\n';
        ++failures;
    }
    return snapshot;
}

// 按规范化 JSON 约定修改清单：解析、变更、重新序列化，因此产出的仍是规范化字节，
// 失败必须来自语义校验而不是编码校验。
ConfigurationBackupSnapshot mutatedManifestSnapshot(
        const ConfigurationBackupSnapshot &base,
        const std::function<void(QJsonObject *)> &mutate)
{
    QJsonObject manifest =
        QJsonDocument::fromJson(base.files.at(0).content).object();
    mutate(&manifest);
    ConfigurationBackupSnapshot copy = base;
    copy.files[0].content =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    return copy;
}

ConfigurationBackupSnapshot mutatedEntrySnapshot(
        const ConfigurationBackupSnapshot &base, int entryIndex,
        const std::function<void(QJsonObject *)> &mutate)
{
    return mutatedManifestSnapshot(base, [&](QJsonObject *manifest) {
        QJsonArray entries =
            manifest->value(QStringLiteral("entries")).toArray();
        QJsonObject entry = entries.at(entryIndex).toObject();
        mutate(&entry);
        entries[entryIndex] = entry;
        manifest->insert(QStringLiteral("entries"), entries);
    });
}

void expectVerifyFailure(const ConfigurationBackupSnapshot &snapshot,
                         const QString &expectedCode, const char *message)
{
    QString error;
    if (!expect(!ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                  snapshot, &error),
                message)) {
        return;
    }
    if (!expect(error == expectedCode, message)) {
        QTextStream(stderr) << "  expected " << expectedCode << " but got "
                            << error << '\n';
    }
}

void testSubjectAndMetadataGrammar()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n")),
                "grammar fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "grammar fixture could not be captured")) {
        return;
    }

    for (const QString &subject : {
             QStringLiteral("skill:example"),
             QStringLiteral("codex-plugin:a.b-c_1"),
             QStringLiteral("mcp:server0"),
         }) {
        ConfigurationBackupSnapshot snapshot;
        QString error;
        expect(ExtensionStagingSnapshot::build(skillDomain(), tree, subject,
                                               fixtureBackupId(1),
                                               fixtureCreatedAt(), &snapshot,
                                               &error),
               "a grammar-valid subject was refused");
    }
    for (const QString &subject : {
             QStringLiteral("codex"),
             QStringLiteral("tool:a"),
             QStringLiteral("SKILL:a"),
             QStringLiteral("skill:"),
             QStringLiteral("skill:A"),
             QStringLiteral("skill:-a"),
             QStringLiteral("skill:") + QString(129, QLatin1Char('a')),
             QStringLiteral("claude:code"),
         }) {
        ConfigurationBackupSnapshot snapshot;
        QString error;
        if (!expect(!ExtensionStagingSnapshot::build(
                        skillDomain(), tree, subject, fixtureBackupId(1),
                        fixtureCreatedAt(), &snapshot, &error),
                    "a grammar-invalid subject was built")) {
            continue;
        }
        expect(error == QStringLiteral("extension-staging-snapshot-subject-invalid"),
               "a grammar-invalid subject reported the wrong diagnostic");
        expect(snapshot.files.isEmpty(),
               "a refused subject still produced snapshot files");
    }

    ConfigurationBackupSnapshot snapshot;
    QString error;
    expect(!ExtensionStagingSnapshot::build(
               skillDomain(), tree, kSubject,
               QStringLiteral("20260904_120000_000_abcd1234"),
               fixtureCreatedAt(), &snapshot, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-metadata-invalid"),
           "a tool-domain backup id was accepted for the staging domain");
    expect(!ExtensionStagingSnapshot::build(
               skillDomain(), tree, kSubject, fixtureBackupId(1),
               QDateTime(), &snapshot, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-metadata-invalid"),
           "an invalid creation time was accepted");
    expect(!ExtensionStagingSnapshot::build(
               ExtensionTreeCaptureDomain{}, tree, kSubject, fixtureBackupId(1),
               fixtureCreatedAt(), &snapshot, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-domain-unconfigured"),
           "an unconfigured capture domain was built instead of refused");

    // 验证侧同样先拒绝畸形期望主体，再触碰快照内容。
    const ConfigurationBackupSnapshot valid = buildOrDie(tree, 2);
    expect(!ExtensionStagingSnapshot::verify(
               skillDomain(), QStringLiteral("not a subject"), valid, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-subject-invalid"),
           "a grammar-invalid expected subject reached the verifier");
    expect(!ExtensionStagingSnapshot::verify(
               ExtensionTreeCaptureDomain{}, kSubject, valid, &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-domain-unconfigured"),
           "an unconfigured capture domain reached the verifier");
}

void testRoundTripThroughEncryptedDomain()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/empty.bin"),
                                 QByteArray())
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
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 3);
    if (built.files.isEmpty()) return;
    // 槽位布局：槽 0 清单，槽 1..3 三个文件，目录不占槽。
    expect(built.files.size() == 4,
           "the snapshot does not hold one manifest slot plus three file slots");
    expect(built.tool == kSubject
               && built.backupId == fixtureBackupId(3)
               && built.createdAt == fixtureCreatedAt(),
           "the snapshot metadata does not match the request");

    FixedKeyProvider provider;
    const QString storeRoot = temporary.path() + QStringLiteral("/backups");
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), storeRoot,
        &provider);
    QString error;
    if (!expect(store.create(built, &error),
                "the staging domain store refused a contract-valid snapshot")) {
        QTextStream(stderr) << "  create said: " << error << '\n';
        return;
    }
    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSubject, fixtureBackupId(3), &readBack, &error),
                "the staging domain store could not read the snapshot back")) {
        QTextStream(stderr) << "  read said: " << error << '\n';
        return;
    }
    if (!expect(readBack.files.size() == built.files.size(),
                "the encrypted round trip changed the slot count")) {
        return;
    }
    for (int index = 0; index < built.files.size(); ++index) {
        expect(readBack.files.at(index).slot == built.files.at(index).slot
                   && readBack.files.at(index).existed
                   && readBack.files.at(index).content
                       == built.files.at(index).content,
               "the encrypted round trip altered snapshot bytes");
    }
    QString verifyError;
    expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject, readBack,
                                            &verifyError),
           "a freshly read snapshot failed verification");
}

void testDeterminism()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("beta\n")),
                "determinism fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> first;
    QVector<ExtensionTreeCaptureEntry> second;
    if (!expect(scan(root, &first) && scan(root, &second),
                "determinism fixture could not be captured twice")) {
        return;
    }
    // 不同的备份号与时间不得影响清单字节与树身份：清单是树与主体的纯函数。
    const ConfigurationBackupSnapshot one = buildOrDie(first, 4);
    const ConfigurationBackupSnapshot two = buildOrDie(second, 5);
    if (one.files.isEmpty() || two.files.isEmpty()) return;
    expect(one.files.at(0).content == two.files.at(0).content,
           "two builds of the same tree produced different manifest bytes");
    QString error;
    expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject, two,
                                            &error),
           "a determinism rebuild failed verification");
}

void testEmptyAndDirectoryOnlyTrees()
{
    // 空树：清单条目为空、文件数为零，快照只剩槽 0。这仍是一份可存储、可验证的快照。
    QVector<ExtensionTreeCaptureEntry> empty;
    const ConfigurationBackupSnapshot emptySnapshot = buildOrDie(empty, 6);
    if (expect(emptySnapshot.files.size() == 1,
               "an empty tree did not produce a manifest-only snapshot")) {
        QString error;
        expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                emptySnapshot, &error),
               "an empty-tree snapshot failed verification");
    }

    QVector<ExtensionTreeCaptureEntry> directoriesOnly;
    directoriesOnly.append(ExtensionTreeCaptureEntry{
        QStringLiteral("docs"), true, {}});
    directoriesOnly.append(ExtensionTreeCaptureEntry{
        QStringLiteral("docs/inner"), true, {}});
    const ConfigurationBackupSnapshot directorySnapshot =
        buildOrDie(directoriesOnly, 7);
    if (expect(directorySnapshot.files.size() == 1,
               "a directory-only tree produced file slots")) {
        QString error;
        expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                directorySnapshot, &error),
               "a directory-only snapshot failed verification");
    }

    // 两类树都必须能穿过加密暂存域往返。
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(), temporary.path(),
        &provider);
    QString error;
    if (!expect(store.create(directorySnapshot, &error),
                "a directory-only snapshot was refused by the store")) {
        QTextStream(stderr) << "  create said: " << error << '\n';
        return;
    }
    ConfigurationBackupSnapshot readBack;
    expect(store.read(kSubject, fixtureBackupId(7), &readBack, &error)
               && ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                   readBack, &error),
           "a directory-only snapshot did not survive the encrypted domain");
}

void testManifestStrictness()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("beta\n")),
                "strictness fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "strictness fixture could not be captured")) {
        return;
    }
    const ConfigurationBackupSnapshot valid = buildOrDie(tree, 8);
    if (valid.files.size() != 3) {
        expect(false, "strictness fixture does not have the expected slots");
        return;
    }
    QString error;
    if (!expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                 valid, &error),
                "the unmodified fixture failed verification")) {
        QTextStream(stderr) << "  verify said: " << error << '\n';
        return;
    }

    const QString shape =
        QStringLiteral("extension-staging-snapshot-manifest-shape");
    const QString canonical =
        QStringLiteral("extension-staging-snapshot-manifest-canonical");
    const QString parse =
        QStringLiteral("extension-staging-snapshot-manifest-parse");

    // 未知字段与缺失字段。
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("surprise"), 1);
        }),
        shape, "an unknown manifest field was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->remove(QStringLiteral("subject"));
        }),
        shape, "a manifest without a subject was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("format"),
                             QStringLiteral("aegisy-extension-staging-snapshot-manifest/0.2"));
        }),
        shape, "a foreign manifest format was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("version"), 2);
        }),
        shape, "a foreign manifest version was not refused");
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            entry->insert(QStringLiteral("mode"), QStringLiteral("0755"));
        }),
        shape, "an unknown entry field was not refused");
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            entry->remove(QStringLiteral("sha256"));
        }),
        shape, "a file entry without a digest was not refused");
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            entry->insert(QStringLiteral("kind"),
                          QStringLiteral("symlink"));
        }),
        shape, "an unknown entry kind was not refused");

    // 非规范化编码：多余空白与重复键都必须失败关闭。
    {
        ConfigurationBackupSnapshot copy = valid;
        QByteArray bytes = copy.files.at(0).content;
        bytes.insert(1, ' ');
        copy.files[0].content = bytes;
        expectVerifyFailure(copy, canonical,
                            "non-canonical manifest whitespace was accepted");
    }
    {
        ConfigurationBackupSnapshot copy = valid;
        QByteArray bytes = copy.files.at(0).content;
        bytes.replace("\"version\":1}",
                      QByteArrayLiteral("\"version\":1,\"version\":1}"));
        copy.files[0].content = bytes;
        expectVerifyFailure(copy, canonical,
                            "a duplicate manifest key was accepted");
    }
    {
        ConfigurationBackupSnapshot copy = valid;
        QByteArray bytes = copy.files.at(0).content;
        bytes[2] = '\0';
        copy.files[0].content = bytes;
        expectVerifyFailure(
            copy, QStringLiteral("extension-staging-snapshot-manifest-encoding"),
            "a NUL-bearing manifest was not refused with the encoding code");
    }
    {
        ConfigurationBackupSnapshot copy = valid;
        copy.files[0].content = valid.files.at(0).content + QByteArray(1, '\xff');
        expectVerifyFailure(copy, parse,
                            "an invalid-UTF-8 manifest was not refused");
    }
    {
        ConfigurationBackupSnapshot copy = valid;
        copy.files[0].content = valid.files.at(0).content;
        copy.files[0].content.chop(1);
        expectVerifyFailure(copy, parse,
                            "a truncated manifest was not refused");
    }

    // 遍历形状的路径。
    for (const QString &path : {
             QStringLiteral("../escape.txt"),
             QStringLiteral("sub/../../escape.txt"),
             QStringLiteral("/absolute.txt"),
             QStringLiteral("sub//b.txt"),
             QStringLiteral("."),
         }) {
        expectVerifyFailure(
            mutatedEntrySnapshot(valid, 0, [&](QJsonObject *entry) {
                entry->insert(QStringLiteral("path"), path);
            }),
            QStringLiteral("extension-staging-snapshot-path-invalid"),
            "a traversal-shaped manifest path was not refused");
    }
    // 重复路径。
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            QJsonArray entries =
                manifest->value(QStringLiteral("entries")).toArray();
            entries.append(entries.at(1)); // 目录条目 sub 的副本
            manifest->insert(QStringLiteral("entries"), entries);
        }),
        QStringLiteral("extension-staging-snapshot-path-duplicate"),
        "a duplicate manifest path was not refused");

    // 槽位、计数、字节、摘要与身份的不符各自有独立诊断。
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            entry->insert(QStringLiteral("slot"), 2);
        }),
        QStringLiteral("extension-staging-snapshot-slot-mismatch"),
        "a renumbered manifest slot was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("file_count"), 1);
        }),
        QStringLiteral("extension-staging-snapshot-slot-mismatch"),
        "a wrong manifest file count was not refused");
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            entry->insert(QStringLiteral("byte_count"), 99);
        }),
        QStringLiteral("extension-staging-snapshot-byte-count-mismatch"),
        "a wrong manifest byte count was not refused");
    expectVerifyFailure(
        mutatedEntrySnapshot(valid, 0, [](QJsonObject *entry) {
            QString digest = entry->value(QStringLiteral("sha256")).toString();
            digest[0] = digest.at(0) == QLatin1Char('0') ? QLatin1Char('1')
                                                         : QLatin1Char('0');
            entry->insert(QStringLiteral("sha256"), digest);
        }),
        QStringLiteral("extension-staging-snapshot-content-digest-mismatch"),
        "a wrong manifest digest was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            QString identity =
                manifest->value(QStringLiteral("identity")).toString();
            identity[identity.size() - 1] =
                identity.at(identity.size() - 1) == QLatin1Char('0')
                    ? QLatin1Char('1') : QLatin1Char('0');
            manifest->insert(QStringLiteral("identity"), identity);
        }),
        QStringLiteral("extension-staging-snapshot-identity-mismatch"),
        "a wrong tree identity was not refused");
    expectVerifyFailure(
        mutatedManifestSnapshot(valid, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("subject"),
                             QStringLiteral("skill:other"));
        }),
        QStringLiteral("extension-staging-snapshot-subject-mismatch"),
        "a manifest naming another subject was not refused");

    // 用错捕获域重算身份必须失败关闭，而不是退回某个默认域。
    expect(!ExtensionStagingSnapshot::verify(bundleDomain(), kSubject, valid,
                                             &error)
               && error == QStringLiteral(
                   "extension-staging-snapshot-identity-mismatch"),
           "the verifier accepted a foreign capture domain");

    // 快照结构本身的不符。
    {
        ConfigurationBackupSnapshot copy = valid;
        copy.files.removeLast();
        expectVerifyFailure(
            copy, QStringLiteral("extension-staging-snapshot-slot-mismatch"),
            "a snapshot missing a file slot was not refused");
    }
    {
        ConfigurationBackupSnapshot copy = valid;
        copy.files[1].existed = false;
        copy.files[1].content.clear();
        expectVerifyFailure(
            copy, QStringLiteral("extension-staging-snapshot-slot-mismatch"),
            "a snapshot slot marked absent was not refused");
    }
    {
        ConfigurationBackupSnapshot copy;
        expectVerifyFailure(
            copy, QStringLiteral("extension-staging-snapshot-slot-mismatch"),
            "an empty snapshot was not refused");
    }
}

void testBoundsReconciliation()
{
    // 对账的核心：捕获层放行 300 个文件（4096 条目之内、总量之内），暂存域只有 256 个
    // 槽且槽 0 归清单，因此构建必须拒绝而不是截断。用真实磁盘扫描证明这棵树确实过了
    // 捕获层。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) {
            return;
        }
        const QString root = temporary.path() + QStringLiteral("/tree");
        bool written = true;
        for (int index = 0; index < 300; ++index) {
            written = writeFile(root + QStringLiteral("/f%1.bin").arg(index),
                                QByteArrayLiteral("x"))
                && written;
        }
        if (expect(written, "file-count fixture could not be written")) {
            QVector<ExtensionTreeCaptureEntry> tree;
            if (expect(scan(root, &tree) && tree.size() == 300,
                       "the capture layer did not accept 300 files")) {
                ConfigurationBackupSnapshot snapshot;
                QString error;
                expect(!ExtensionStagingSnapshot::build(
                           skillDomain(), tree, kSubject, fixtureBackupId(9),
                           fixtureCreatedAt(), &snapshot, &error)
                           && error == QStringLiteral(
                               "extension-staging-snapshot-file-count-limit")
                           && snapshot.files.isEmpty(),
                       "a tree the capture allowed but the staging domain "
                       "cannot hold was built instead of refused");
            }
        }
    }

    // 边界：255 个文件恰好填满 256 个槽，构建必须成功；第 256 个文件必须被拒绝。
    {
        QVector<ExtensionTreeCaptureEntry> tree;
        for (int index = 0; index < 255; ++index) {
            tree.append(ExtensionTreeCaptureEntry{
                QStringLiteral("f%1.bin").arg(index), false,
                QByteArrayLiteral("x")});
        }
        ConfigurationBackupSnapshot snapshot;
        QString error;
        if (expect(ExtensionStagingSnapshot::build(
                       skillDomain(), tree, kSubject, fixtureBackupId(10),
                       fixtureCreatedAt(), &snapshot, &error),
                   "a 255-file tree did not fill the staging domain exactly")) {
            expect(snapshot.files.size() == 256,
                   "a 255-file snapshot does not occupy exactly 256 slots");
            expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                    snapshot, &error),
                   "a boundary snapshot failed verification");
        }
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("f255.bin"), false, QByteArrayLiteral("x")});
        expect(!ExtensionStagingSnapshot::build(
                   skillDomain(), tree, kSubject, fixtureBackupId(10),
                   fixtureCreatedAt(), &snapshot, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-file-count-limit"),
               "the 256th file was not refused");
    }

    // 单槽上限：暂存域 4 MiB。捕获层的 2 MiB 更紧，因此这一支只能由手工构造的输入
    // 触发；构建层必须独立守住。
    {
        QVector<ExtensionTreeCaptureEntry> tree;
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("large.bin"), false,
            QByteArray(4 * 1024 * 1024 + 1, 'x')});
        ConfigurationBackupSnapshot snapshot;
        QString error;
        expect(!ExtensionStagingSnapshot::build(
                   skillDomain(), tree, kSubject, fixtureBackupId(11),
                   fixtureCreatedAt(), &snapshot, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-file-oversized"),
               "a file beyond the staging per-slot bound was not refused");
    }

    // 清单上限：槽 0 是存储层的一份普通文件，因此清单的实际天花板是单槽上限
    // （4 MiB）而不是域清单上限（32 MiB）。用许多条长路径的目录条目构造，不写
    // 任何真实内容。
    {
        const QString segment(135, QLatin1Char('a'));
        QVector<ExtensionTreeCaptureEntry> tree;
        for (int index = 0; index < 2000; ++index) {
            QString path;
            for (int level = 0; level < 16; ++level) {
                path += segment + QLatin1Char('/');
            }
            path += QStringLiteral("d%1").arg(index);
            tree.append(ExtensionTreeCaptureEntry{path, true, {}});
        }
        ConfigurationBackupSnapshot snapshot;
        QString error;
        expect(!ExtensionStagingSnapshot::build(
                   skillDomain(), tree, kSubject, fixtureBackupId(12),
                   fixtureCreatedAt(), &snapshot, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-manifest-oversized"),
               "a manifest beyond the effective slot bound was not refused");
    }

    // 手工构造的树里的非法路径与重复路径同样失败关闭。
    {
        QVector<ExtensionTreeCaptureEntry> tree;
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("../escape"), false, QByteArrayLiteral("x")});
        ConfigurationBackupSnapshot snapshot;
        QString error;
        expect(!ExtensionStagingSnapshot::build(
                   skillDomain(), tree, kSubject, fixtureBackupId(13),
                   fixtureCreatedAt(), &snapshot, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-entry-invalid"),
               "a traversal-shaped capture entry was built");
        tree.clear();
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("dup.txt"), false, QByteArrayLiteral("x")});
        tree.append(ExtensionTreeCaptureEntry{
            QStringLiteral("dup.txt"), false, QByteArrayLiteral("y")});
        expect(!ExtensionStagingSnapshot::build(
                   skillDomain(), tree, kSubject, fixtureBackupId(13),
                   fixtureCreatedAt(), &snapshot, &error)
                   && error == QStringLiteral(
                       "extension-staging-snapshot-path-duplicate"),
               "a tree with duplicate paths was built");
    }
}

void testTamperDetection()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    // 两个文件内容长度一致，交换后只有逐槽摘要能发现。
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("omega\n")),
                "tamper fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!expect(scan(root, &tree), "tamper fixture could not be captured")) {
        return;
    }
    const ConfigurationBackupSnapshot built = buildOrDie(tree, 14);
    if (built.files.size() != 3) {
        expect(false, "tamper fixture does not have the expected slots");
        return;
    }

    FixedKeyProvider provider;
    ConfigurationBackupStore store(
        ConfigurationBackupStore::extensionStagingDomain(),
        temporary.path() + QStringLiteral("/backups"), &provider);
    QString error;
    if (!expect(store.create(built, &error),
                "the tamper fixture could not be stored")) {
        QTextStream(stderr) << "  create said: " << error << '\n';
        return;
    }
    ConfigurationBackupSnapshot readBack;
    if (!expect(store.read(kSubject, fixtureBackupId(14), &readBack, &error),
                "the tamper fixture could not be read back")) {
        return;
    }
    if (!expect(ExtensionStagingSnapshot::verify(skillDomain(), kSubject,
                                                 readBack, &error),
                "the untampered fixture failed verification")) {
        QTextStream(stderr) << "  verify said: " << error << '\n';
        return;
    }

    // 翻转一个文件槽里的一字节：验证必须落在逐槽摘要诊断上。
    {
        ConfigurationBackupSnapshot tampered = readBack;
        QByteArray content = tampered.files.at(1).content;
        content[0] = content.at(0) == 'A' ? 'B' : 'A';
        tampered.files[1].content = content;
        expectVerifyFailure(
            tampered,
            QStringLiteral("extension-staging-snapshot-content-digest-mismatch"),
            "a one-byte slot tamper was not detected");
    }
    // 同字节数、同清单但两个槽的内容互换：逐槽摘要抓住第一个。
    {
        ConfigurationBackupSnapshot tampered = readBack;
        const QByteArray first = tampered.files.at(1).content;
        tampered.files[1].content = tampered.files.at(2).content;
        tampered.files[2].content = first;
        expectVerifyFailure(
            tampered,
            QStringLiteral("extension-staging-snapshot-content-digest-mismatch"),
            "swapped slot contents were not detected");
    }
    // 清单主体换成另一个语法合法的主体。
    expectVerifyFailure(
        mutatedManifestSnapshot(readBack, [](QJsonObject *manifest) {
            manifest->insert(QStringLiteral("subject"),
                             QStringLiteral("mcp:other"));
        }),
        QStringLiteral("extension-staging-snapshot-subject-mismatch"),
        "a subject swap inside the manifest was not detected");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testSubjectAndMetadataGrammar();
    testRoundTripThroughEncryptedDomain();
    testDeterminism();
    testEmptyAndDirectoryOnlyTrees();
    testManifestStrictness();
    testBoundsReconciliation();
    testTamperDetection();
    if (failures == 0) {
        QTextStream(stdout) << "extension staging snapshot guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
