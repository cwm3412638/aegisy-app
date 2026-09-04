#include "extension_tree_capture.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

struct EntrySpec {
    QByteArray relativePath;
    bool directory;
    QByteArray bytes;
};

// 独立重算期望身份：这里的长度前缀与拼接规则按分帧约定重写一遍，而不是调用被测实现，
// 因此被测实现里的分帧错误会让两边分叉。
void frameInto(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

QString expectedIdentity(const QByteArray &domain,
                         const QString &prefix,
                         const QList<EntrySpec> &entries)
{
    QByteArray preimage = domain;
    for (const EntrySpec &entry : entries) {
        frameInto(&preimage, entry.directory ? QByteArrayLiteral("directory")
                                             : QByteArrayLiteral("file"));
        frameInto(&preimage, entry.relativePath);
        if (!entry.directory) frameInto(&preimage, entry.bytes);
    }
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(preimage, QCryptographicHash::Sha256).toHex());
}

bool scan(const ExtensionTreeCaptureDomain &domain,
          const QString &root,
          QVector<ExtensionTreeCaptureEntry> *tree,
          ExtensionTreeCaptureError *error)
{
    ExtensionTreeCaptureBudget budget;
    // 与两个调用方一致，根先取规范化路径：临时目录可能位于符号链接之下。
    const QString canonical = QFileInfo(root).canonicalFilePath();
    return ExtensionTreeCapture::scanDirectory(domain, canonical, canonical,
                                               QString(), 0, &budget, tree, error);
}

// 固定的小树：一个文件、一个目录、目录里再一个文件。期望的条目序列与两个域的身份全部
// 由测试按分帧约定独立重算，而不是从被测实现里镜像出来。
void testByteCompatibleIdentities()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n"))
                    && writeFile(root + QStringLiteral("/sub/b.txt"),
                                 QByteArrayLiteral("beta\n")),
                "identity fixture could not be written")) {
        return;
    }
    const QList<EntrySpec> expectedEntries{
        {QByteArrayLiteral("a.txt"), false, QByteArrayLiteral("alpha\n")},
        {QByteArrayLiteral("sub"), true, {}},
        {QByteArrayLiteral("sub/b.txt"), false, QByteArrayLiteral("beta\n")},
    };

    QVector<ExtensionTreeCaptureEntry> tree;
    ExtensionTreeCaptureError error;
    if (!expect(scan(skillDomain(), root, &tree, &error),
                "a well-formed tree was not captured")) {
        return;
    }
    expect(tree.size() == expectedEntries.size(),
           "the capture lost or invented entries");
    const int limit = qMin(tree.size(), expectedEntries.size());
    for (int index = 0; index < limit; ++index) {
        expect(tree.at(index).relativePath.toUtf8()
                       == expectedEntries.at(index).relativePath
                   && tree.at(index).directory == expectedEntries.at(index).directory
                   && tree.at(index).bytes == expectedEntries.at(index).bytes,
               "the captured entry order or bytes diverge from the fixture");
    }

    const QString skillIdentity = ExtensionTreeCapture::contentIdentity(
        skillDomain(), tree);
    const QString bundleIdentity = ExtensionTreeCapture::contentIdentity(
        bundleDomain(), tree);
    expect(skillIdentity
               == expectedIdentity(
                   QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
                   QStringLiteral("extension-content:sha256:"), expectedEntries),
           "the skill-domain identity is not byte-compatible with the framing rules");
    expect(bundleIdentity
               == expectedIdentity(
                   QByteArrayLiteral("aegisy-extension-bundle-content/0.1\0"),
                   QStringLiteral("extension-content:sha256:"), expectedEntries),
           "the bundle-domain identity is not byte-compatible with the framing rules");
    expect(skillIdentity != bundleIdentity,
           "two domains produced the same identity for the same tree");
    expect(bundleIdentity
               != expectedIdentity(
                   QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
                   QStringLiteral("extension-content:sha256:"), expectedEntries),
           "one domain accepted the other domain's identity framing");

    // 分帧摘要助手的独立重算：域、长度前缀与前缀全部进入期望字节。
    QByteArray digestPreimage = QByteArrayLiteral("test-domain/0.1\0");
    frameInto(&digestPreimage, QByteArrayLiteral("ab"));
    frameInto(&digestPreimage, QByteArrayLiteral("c"));
    const QString expectedDigest = QStringLiteral("test:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            digestPreimage, QCryptographicHash::Sha256).toHex());
    expect(ExtensionTreeCapture::framedDigest(
               QByteArrayLiteral("test-domain/0.1\0"),
               {QByteArrayLiteral("ab"), QByteArrayLiteral("c")},
               QStringLiteral("test:sha256:"))
               == expectedDigest,
           "the framed digest diverges from an independent recomputation");
    expect(ExtensionTreeCapture::framedDigest(
               QByteArray(), {QByteArrayLiteral("ab")},
               QStringLiteral("test:sha256:"))
               .isEmpty(),
           "an empty digest domain was not refused");

    // 同一棵树扫描两次必须得到同一条目序列与同一身份：授权绑定的是这个值。
    QVector<ExtensionTreeCaptureEntry> again;
    ExtensionTreeCaptureError againError;
    expect(scan(skillDomain(), root, &again, &againError)
               && again.size() == tree.size(),
           "a repeated scan of an unchanged tree diverged");
    const int repeatLimit = qMin(tree.size(), again.size());
    for (int index = 0; index < repeatLimit; ++index) {
        expect(again.at(index).relativePath == tree.at(index).relativePath
                   && again.at(index).bytes == tree.at(index).bytes,
               "a repeated scan reordered or altered entries");
    }
    expect(ExtensionTreeCapture::contentIdentity(skillDomain(), again)
               == skillIdentity,
           "a repeated scan produced a different identity");

    const ExtensionTreeCaptureEntry *found =
        ExtensionTreeCapture::findFile(tree, QStringLiteral("sub/b.txt"));
    expect(found && found->bytes == QByteArrayLiteral("beta\n"),
           "findFile did not locate a nested file");
    expect(ExtensionTreeCapture::findFile(tree, QStringLiteral("sub")) == nullptr,
           "findFile returned a directory as a file");
}

void testUnconfiguredDomainIsRefused()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n")),
                "domain fixture could not be written")) {
        return;
    }
    QVector<ExtensionTreeCaptureEntry> tree;
    ExtensionTreeCaptureError error;
    expect(!scan(ExtensionTreeCaptureDomain{}, root, &tree, &error)
               && error.state == ExtensionTreeCaptureErrorState::Invalid
               && error.errorCode
                   == QStringLiteral("extension-tree-capture-domain-unconfigured"),
           "an unconfigured domain was scanned instead of refused");
    ExtensionTreeCaptureDomain partial = skillDomain();
    partial.errorPrefix.clear();
    expect(!scan(partial, root, &tree, &error)
               && error.errorCode
                   == QStringLiteral("extension-tree-capture-domain-unconfigured"),
           "a partially configured domain was scanned instead of refused");
    expect(ExtensionTreeCapture::contentIdentity(ExtensionTreeCaptureDomain{}, tree)
               .isEmpty(),
           "an unconfigured domain produced a content identity");
}

void testPerDomainErrorCodes()
{
    QTemporaryDir temporary;
    if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
    const QString root = temporary.path() + QStringLiteral("/tree");
    if (!expect(writeFile(root + QStringLiteral("/a.txt"),
                          QByteArrayLiteral("alpha\n")),
                "symlink fixture could not be written")) {
        return;
    }
    const QString link = root + QStringLiteral("/linked.txt");
    if (!QFile::link(root + QStringLiteral("/a.txt"), link)) {
        // 该平台不支持建立符号链接时跳过，而不是把这条约束报成通过。
        QTextStream(stdout) << "note: symlink creation unsupported, case skipped\n";
    } else {
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureError error;
        expect(!scan(skillDomain(), root, &tree, &error)
                   && error.state == ExtensionTreeCaptureErrorState::Invalid
                   && error.errorCode == QStringLiteral("skill-symlink-invalid"),
               "a symlink did not report the skill-domain diagnostic");
        tree.clear();
        expect(!scan(bundleDomain(), root, &tree, &error)
                   && error.errorCode
                       == QStringLiteral("extension-bundle-symlink-invalid"),
               "a symlink did not report the bundle-domain diagnostic");
    }

    // 名字里的控制字符同样逐域报告。
    const QString controlRoot = temporary.path() + QStringLiteral("/control");
    if (writeFile(controlRoot + QStringLiteral("/bad\nname.txt"),
                  QByteArrayLiteral("x"))) {
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureError error;
        expect(!scan(skillDomain(), controlRoot, &tree, &error)
                   && error.errorCode == QStringLiteral("skill-entry-invalid"),
               "a control-character name did not report the skill-domain diagnostic");
        tree.clear();
        expect(!scan(bundleDomain(), controlRoot, &tree, &error)
                   && error.errorCode
                       == QStringLiteral("extension-bundle-entry-invalid"),
               "a control-character name did not report the bundle-domain diagnostic");
    }

    // 规范化路径包含检查：扫描目录位于声明的根之外时拒绝，而不是跟随出去。
    const QString outside = temporary.path() + QStringLiteral("/outside");
    if (writeFile(outside + QStringLiteral("/a.txt"), QByteArrayLiteral("x"))) {
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureBudget budget;
        ExtensionTreeCaptureError error;
        const QString root = temporary.path() + QStringLiteral("/tree");
        expect(!ExtensionTreeCapture::scanDirectory(
                   skillDomain(), QFileInfo(root).canonicalFilePath(),
                   QFileInfo(outside).canonicalFilePath(), QString(), 0, &budget,
                   &tree, &error)
                   && error.errorCode
                       == QStringLiteral("skill-path-outside-root"),
               "a directory outside the declared root was captured");

        // 扫描目标不是目录时同样逐域拒绝。
        expect(!ExtensionTreeCapture::scanDirectory(
                   bundleDomain(), QFileInfo(root).canonicalFilePath(),
                   outside + QStringLiteral("/a.txt"), QString(), 0, &budget,
                   &tree, &error)
                   && error.errorCode
                       == QStringLiteral("extension-bundle-directory-invalid"),
               "a file was scanned as a directory without a diagnostic");
    }

    // 特殊文件（既不是常规文件也不是目录）被拒绝。仅在支持 FIFO 的平台上构造。
#ifdef Q_OS_UNIX
    const QString fifoRoot = temporary.path() + QStringLiteral("/fifo");
    if (QDir().mkpath(fifoRoot)
            && ::mkfifo(QFile::encodeName(
                            fifoRoot + QStringLiteral("/pipe")).constData(),
                        0600)
                   == 0) {
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureError error;
        expect(!scan(skillDomain(), fifoRoot, &tree, &error)
                   && error.errorCode == QStringLiteral("skill-entry-invalid"),
               "a special file did not report the skill-domain diagnostic");
        tree.clear();
        expect(!scan(bundleDomain(), fifoRoot, &tree, &error)
                   && error.errorCode
                       == QStringLiteral("extension-bundle-entry-invalid"),
               "a special file did not report the bundle-domain diagnostic");
    } else {
        QTextStream(stdout) << "note: fifo creation unsupported, case skipped\n";
    }

    // 读不出来的文件报 Unavailable 而不是 Invalid：一个读不出来的内容不等于一份畸形的
    // 内容。
    const QString unreadableRoot = temporary.path() + QStringLiteral("/unreadable");
    const QString unreadable = unreadableRoot + QStringLiteral("/a.txt");
    if (writeFile(unreadable, QByteArrayLiteral("x"))
            && ::chmod(QFile::encodeName(unreadable).constData(), 0) == 0) {
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureError error;
        expect(!scan(skillDomain(), unreadableRoot, &tree, &error)
                   && error.state == ExtensionTreeCaptureErrorState::Unavailable
                   && error.errorCode == QStringLiteral("skill-file-unavailable"),
               "an unreadable file did not report an unavailable diagnostic");
        ::chmod(QFile::encodeName(unreadable).constData(),
                S_IRUSR | S_IWUSR);
    } else {
        QTextStream(stdout)
            << "note: permission restriction unsupported, case skipped\n";
    }
#endif
}

void testBoundsAreEnforced()
{
    // 单文件上限。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
        const QString root = temporary.path() + QStringLiteral("/tree");
        QFile oversized(root + QStringLiteral("/large.bin"));
        QDir().mkpath(root);
        if (expect(oversized.open(QIODevice::WriteOnly)
                       && oversized.resize(ExtensionTreeCapture::MaxFileBytes + 1),
                   "oversized fixture could not be created")) {
            oversized.close();
            QVector<ExtensionTreeCaptureEntry> tree;
            ExtensionTreeCaptureError error;
            expect(!scan(skillDomain(), root, &tree, &error)
                       && error.errorCode
                           == QStringLiteral("skill-file-oversized"),
                   "an oversized file was captured");
        }
    }

    // 总字节上限：每个文件都在单文件上限之内，总和越界。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
        const QString root = temporary.path() + QStringLiteral("/tree");
        bool written = true;
        for (int index = 0; index < 9; ++index) {
            written = writeFile(root + QStringLiteral("/f%1.bin").arg(index),
                                QByteArray(static_cast<int>(
                                               ExtensionTreeCapture::MaxFileBytes),
                                           'x'))
                && written;
        }
        if (expect(written, "total-bytes fixture could not be written")) {
            QVector<ExtensionTreeCaptureEntry> tree;
            ExtensionTreeCaptureError error;
            expect(!scan(skillDomain(), root, &tree, &error)
                       && error.errorCode
                           == QStringLiteral("skill-total-bytes-limit"),
                   "a tree beyond the total byte budget was captured");
        }
    }

    // 条目上限。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
        const QString root = temporary.path() + QStringLiteral("/tree");
        bool written = true;
        for (int index = 0; index <= ExtensionTreeCapture::MaxEntries; ++index) {
            written = writeFile(root + QStringLiteral("/f%1").arg(index),
                                QByteArrayLiteral("x"))
                && written;
        }
        if (expect(written, "entry-limit fixture could not be written")) {
            QVector<ExtensionTreeCaptureEntry> tree;
            ExtensionTreeCaptureError error;
            expect(!scan(skillDomain(), root, &tree, &error)
                       && error.errorCode == QStringLiteral("skill-entry-limit"),
                   "a tree beyond the entry budget was captured");
        }
    }

    // 深度上限：第 MaxDepth + 1 层目录被拒绝。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
        QString deep = temporary.path() + QStringLiteral("/tree");
        for (int index = 0; index <= ExtensionTreeCapture::MaxDepth + 1; ++index) {
            deep += QStringLiteral("/d%1").arg(index);
        }
        if (expect(QDir().mkpath(deep), "depth fixture could not be created")) {
            QVector<ExtensionTreeCaptureEntry> tree;
            ExtensionTreeCaptureError error;
            expect(!scan(skillDomain(), temporary.path() + QStringLiteral("/tree"),
                         &tree, &error)
                       && error.errorCode == QStringLiteral("skill-depth-limit"),
                   "a tree beyond the depth budget was captured");
        }
    }

    // 相对路径长度上限：这一层把调用方给的相对前缀算进长度，因此用一个深前缀触发，
    // 而不依赖宿主机能否真的创建超过 PATH_MAX 的目录。
    {
        QTemporaryDir temporary;
        if (!expect(temporary.isValid(), "temporary directory unavailable")) return;
        const QString root = temporary.path() + QStringLiteral("/tree");
        if (expect(writeFile(root + QStringLiteral("/a.txt"),
                             QByteArrayLiteral("x")),
                   "path-limit fixture could not be written")) {
            QVector<ExtensionTreeCaptureEntry> tree;
            ExtensionTreeCaptureBudget budget;
            ExtensionTreeCaptureError error;
            const QString canonical = QFileInfo(root).canonicalFilePath();
            expect(!ExtensionTreeCapture::scanDirectory(
                       skillDomain(), canonical, canonical,
                       QString(4096, QLatin1Char('p')), 0, &budget, &tree, &error)
                       && error.errorCode == QStringLiteral("skill-path-limit"),
                   "a tree beyond the relative-path budget was captured");
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testByteCompatibleIdentities();
    testUnconfiguredDomainIsRefused();
    testPerDomainErrorCodes();
    testBoundsAreEnforced();
    if (failures == 0) {
        QTextStream(stdout) << "extension tree capture guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
