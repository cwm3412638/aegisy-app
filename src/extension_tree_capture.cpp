#include "extension_tree_capture.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace {

void appendLength(QCryptographicHash *hash, quint64 size)
{
    char encoded[8];
    for (int index = 0; index < 8; ++index) {
        encoded[index] = static_cast<char>((size >> (56 - index * 8)) & 0xff);
    }
    hash->addData(QByteArray(encoded, 8));
}

void appendFramed(QCryptographicHash *hash, const QByteArray &value)
{
    appendLength(hash, static_cast<quint64>(value.size()));
    hash->addData(value);
}

ExtensionTreeCaptureError failure(ExtensionTreeCaptureErrorState state,
                                  const QString &code)
{
    ExtensionTreeCaptureError error;
    error.state = state;
    error.errorCode = code;
    return error;
}

// 诊断代码保留各调用方原有的前缀，因此抽取不会改变已经被测试与文档固定的诊断串。
QString code(const ExtensionTreeCaptureDomain &domain, const char *suffix)
{
    return domain.errorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

bool containedBy(const QString &root, const QString &candidate)
{
    const QString normalizedRoot = QDir::cleanPath(QDir::fromNativeSeparators(root));
    const QString normalizedCandidate = QDir::cleanPath(
        QDir::fromNativeSeparators(candidate));
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'), sensitivity);
}

// 读完之后重新检查一遍文件属性。读取期间被换掉的文件会让算出的摘要对应一份已经不在那里
// 的内容，而人是按那个摘要做决定的。
bool readStableFile(const ExtensionTreeCaptureDomain &domain,
                    const QFileInfo &initial,
                    const QString &root,
                    QByteArray *bytes,
                    ExtensionTreeCaptureError *error)
{
    if (initial.isSymLink() || !initial.isFile() || initial.size() < 0) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "file-invalid"));
        return false;
    }
    if (initial.size() > ExtensionTreeCapture::MaxFileBytes) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "file-oversized"));
        return false;
    }
    const QString canonical = initial.canonicalFilePath();
    if (canonical.isEmpty() || !containedBy(root, canonical)) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "path-outside-root"));
        return false;
    }
    QFile file(initial.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = failure(ExtensionTreeCaptureErrorState::Unavailable,
                         code(domain, "file-unavailable"));
        return false;
    }
    const QByteArray content = file.read(ExtensionTreeCapture::MaxFileBytes + 1);
    const bool readFailed = file.error() != QFileDevice::NoError;
    file.close();
    if (readFailed) {
        *error = failure(ExtensionTreeCaptureErrorState::Unavailable,
                         code(domain, "file-unavailable"));
        return false;
    }
    if (content.size() > ExtensionTreeCapture::MaxFileBytes) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "file-oversized"));
        return false;
    }
    const QFileInfo final(initial.absoluteFilePath());
    if (final.isSymLink() || !final.isFile() || final.size() != content.size()
            || final.canonicalFilePath() != canonical
            || initial.size() != content.size()) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "file-drift"));
        return false;
    }
    *bytes = content;
    return true;
}

} // namespace

bool ExtensionTreeCapture::safeEntryName(const QString &name)
{
    if (name.isEmpty() || name.size() > 255 || name.toUtf8().size() > 1024
            || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return false;
    }
    for (const QChar character : name) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    return !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'))
        && !name.contains(QLatin1Char(':'));
}

bool ExtensionTreeCapture::scanDirectory(const ExtensionTreeCaptureDomain &domain,
                                         const QString &root,
                                         const QString &directory,
                                         const QString &relativeDirectory,
                                         int depth,
                                         ExtensionTreeCaptureBudget *budget,
                                         QVector<ExtensionTreeCaptureEntry> *tree,
                                         ExtensionTreeCaptureError *error)
{
    // 未配置的域被拒绝，而不是退回某个默认域：默认域会让两类调用方共用同一套摘要字节。
    if (!domain.configured()) {
        *error = failure(
            ExtensionTreeCaptureErrorState::Invalid,
            QStringLiteral("extension-tree-capture-domain-unconfigured"));
        return false;
    }
    if (depth > MaxDepth) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "depth-limit"));
        return false;
    }
    const QFileInfo directoryInfo(directory);
    if (directoryInfo.isSymLink() || !directoryInfo.isDir()) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "directory-invalid"));
        return false;
    }
    const QString canonical = directoryInfo.canonicalFilePath();
    if (canonical.isEmpty() || (canonical != root && !containedBy(root, canonical))) {
        *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                         code(domain, "path-outside-root"));
        return false;
    }

    QDir dir(canonical);
    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &left,
                                                 const QFileInfo &right) {
        return left.fileName().toUtf8() < right.fileName().toUtf8();
    });
    QSet<QString> foldedNames;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        if (!safeEntryName(name) || foldedNames.contains(name.toCaseFolded())) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "entry-invalid"));
            return false;
        }
        foldedNames.insert(name.toCaseFolded());
        ++budget->entries;
        if (budget->entries > MaxEntries) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "entry-limit"));
            return false;
        }
        if (entry.isSymLink()) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "symlink-invalid"));
            return false;
        }
        const QString relative = relativeDirectory.isEmpty()
            ? name : relativeDirectory + QLatin1Char('/') + name;
        if (relative.toUtf8().size() > 4096) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "path-limit"));
            return false;
        }
        if (entry.isDir()) {
            tree->append(ExtensionTreeCaptureEntry{relative, true, {}});
            if (!scanDirectory(domain, root, entry.absoluteFilePath(), relative,
                               depth + 1, budget, tree, error)) {
                return false;
            }
            continue;
        }
        if (!entry.isFile()) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "entry-invalid"));
            return false;
        }
        QByteArray bytes;
        if (!readStableFile(domain, entry, root, &bytes, error)) return false;
        if (budget->bytes > MaxTotalBytes - bytes.size()) {
            *error = failure(ExtensionTreeCaptureErrorState::Invalid,
                             code(domain, "total-bytes-limit"));
            return false;
        }
        budget->bytes += bytes.size();
        tree->append(ExtensionTreeCaptureEntry{relative, false, bytes});
    }
    return true;
}

const ExtensionTreeCaptureEntry *ExtensionTreeCapture::findFile(
    const QVector<ExtensionTreeCaptureEntry> &tree, const QString &path)
{
    const auto found = std::find_if(tree.cbegin(), tree.cend(),
                                    [&](const ExtensionTreeCaptureEntry &entry) {
        return !entry.directory && entry.relativePath == path;
    });
    return found == tree.cend() ? nullptr : &*found;
}

QString ExtensionTreeCapture::contentIdentity(
    const ExtensionTreeCaptureDomain &domain,
    const QVector<ExtensionTreeCaptureEntry> &tree)
{
    if (!domain.configured()) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(domain.identityDomain);
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        appendFramed(&hash, entry.directory ? QByteArrayLiteral("directory")
                                            : QByteArrayLiteral("file"));
        appendFramed(&hash, entry.relativePath.toUtf8());
        if (!entry.directory) appendFramed(&hash, entry.bytes);
    }
    return domain.identityPrefix
        + QString::fromLatin1(hash.result().toHex());
}

QString ExtensionTreeCapture::framedDigest(const QByteArray &domain,
                                           const QList<QByteArray> &parts,
                                           const QString &prefix)
{
    if (domain.isEmpty() || prefix.isEmpty()) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(domain);
    for (const QByteArray &part : parts) appendFramed(&hash, part);
    return prefix + QString::fromLatin1(hash.result().toHex());
}
