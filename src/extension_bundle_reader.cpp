#include "extension_bundle_reader.h"

#include "extension_display_safety.h"
#include "strict_json_validator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace {

using Safety = ExtensionDisplaySafety;

struct TreeEntry {
    QString relativePath;
    bool directory = false;
    QByteArray bytes;
};

struct ScanBudget {
    int entries = 0;
    qint64 bytes = 0;
};

ExtensionBundleReadResult failure(ExtensionBundleReadState state,
                                  const QString &code)
{
    ExtensionBundleReadResult result;
    result.state = state;
    result.errorCode = code;
    return result;
}

void appendLength(QCryptographicHash *hash, quint64 size)
{
    char encoded[8];
    for (int index = 0; index < 8; ++index) {
        encoded[index] = static_cast<char>((size >> (56 - index * 8)) & 0xff);
    }
    hash->addData(QByteArray(encoded, 8));
}

// 每一段输入都带长度前缀。不带长度的拼接可以让两组不同的输入产生同一个摘要,而摘要正是
// 把"人看到的那份内容"与"实际被引入的内容"绑在一起的唯一手段。
void appendFramed(QCryptographicHash *hash, const QByteArray &value)
{
    appendLength(hash, static_cast<quint64>(value.size()));
    hash->addData(value);
}

bool safeEntryName(const QString &name)
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
    return normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'),
                                          sensitivity);
}

// 读完之后重新检查一遍文件属性。读取期间被换掉的文件会让算出的摘要对应一份已经不在那里
// 的内容,而人是按那个摘要做决定的。
bool readStableFile(const QFileInfo &initial,
                    const QString &root,
                    QByteArray *bytes,
                    ExtensionBundleReadResult *error)
{
    if (initial.isSymLink() || !initial.isFile() || initial.size() < 0) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-file-invalid"));
        return false;
    }
    if (initial.size() > ExtensionBundleReader::MaxFileBytes) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-file-oversized"));
        return false;
    }
    const QString canonical = initial.canonicalFilePath();
    if (canonical.isEmpty() || !containedBy(root, canonical)) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-path-outside-root"));
        return false;
    }
    QFile file(initial.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        *error = failure(ExtensionBundleReadState::Unavailable,
                         QStringLiteral("extension-bundle-file-unavailable"));
        return false;
    }
    const QByteArray content = file.read(ExtensionBundleReader::MaxFileBytes + 1);
    const bool readFailed = file.error() != QFileDevice::NoError;
    file.close();
    if (readFailed) {
        *error = failure(ExtensionBundleReadState::Unavailable,
                         QStringLiteral("extension-bundle-file-unavailable"));
        return false;
    }
    if (content.size() > ExtensionBundleReader::MaxFileBytes) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-file-oversized"));
        return false;
    }
    const QFileInfo final(initial.absoluteFilePath());
    if (final.isSymLink() || !final.isFile() || final.size() != content.size()
            || final.canonicalFilePath() != canonical
            || initial.size() != content.size()) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-file-drift"));
        return false;
    }
    *bytes = content;
    return true;
}

bool scanDirectory(const QString &root,
                   const QString &directory,
                   const QString &relativeDirectory,
                   int depth,
                   ScanBudget *budget,
                   QVector<TreeEntry> *tree,
                   ExtensionBundleReadResult *error)
{
    if (depth > ExtensionBundleReader::MaxDepth) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-depth-limit"));
        return false;
    }
    const QFileInfo directoryInfo(directory);
    if (directoryInfo.isSymLink() || !directoryInfo.isDir()) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-directory-invalid"));
        return false;
    }
    const QString canonical = directoryInfo.canonicalFilePath();
    if (canonical.isEmpty()
            || (canonical != root && !containedBy(root, canonical))) {
        *error = failure(ExtensionBundleReadState::Invalid,
                         QStringLiteral("extension-bundle-path-outside-root"));
        return false;
    }

    QDir dir(canonical);
    QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    // 遍历顺序必须固定,否则同一个包在两台机器上算出两个不同的摘要,而那个摘要是授权
    // 绑定的对象。
    std::sort(entries.begin(), entries.end(), [](const QFileInfo &left,
                                                 const QFileInfo &right) {
        return left.fileName().toUtf8() < right.fileName().toUtf8();
    });
    QSet<QString> foldedNames;
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        // 大小写折叠后重名的条目被拒绝:在不区分大小写的文件系统上,两个这样的名字指向
        // 同一份内容,而摘要会把它们算成两份。
        if (!safeEntryName(name) || foldedNames.contains(name.toCaseFolded())) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-entry-invalid"));
            return false;
        }
        foldedNames.insert(name.toCaseFolded());
        ++budget->entries;
        if (budget->entries > ExtensionBundleReader::MaxEntries) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-entry-limit"));
            return false;
        }
        // 符号链接被拒绝而不是跟随:跟随会让摘要覆盖包外的字节,而包外的内容不在人批准
        // 的范围里。
        if (entry.isSymLink()) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-symlink-invalid"));
            return false;
        }
        const QString relative = relativeDirectory.isEmpty()
            ? name : relativeDirectory + QLatin1Char('/') + name;
        if (relative.toUtf8().size() > 4096) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-path-limit"));
            return false;
        }
        if (entry.isDir()) {
            tree->append(TreeEntry{relative, true, {}});
            if (!scanDirectory(root, entry.absoluteFilePath(), relative, depth + 1,
                               budget, tree, error)) {
                return false;
            }
            continue;
        }
        if (!entry.isFile()) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-entry-invalid"));
            return false;
        }
        QByteArray bytes;
        if (!readStableFile(entry, root, &bytes, error)) return false;
        if (budget->bytes > ExtensionBundleReader::MaxTotalBytes - bytes.size()) {
            *error = failure(ExtensionBundleReadState::Invalid,
                             QStringLiteral("extension-bundle-total-bytes-limit"));
            return false;
        }
        budget->bytes += bytes.size();
        tree->append(TreeEntry{relative, false, bytes});
    }
    return true;
}

const TreeEntry *findFile(const QVector<TreeEntry> &tree, const QString &path)
{
    const auto found = std::find_if(tree.cbegin(), tree.cend(),
                                    [&](const TreeEntry &entry) {
        return !entry.directory && entry.relativePath == path;
    });
    return found == tree.cend() ? nullptr : &*found;
}

QString digestOf(const QByteArray &domain,
                 const QList<QByteArray> &parts,
                 const QString &prefix)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(domain);
    for (const QByteArray &part : parts) appendFramed(&hash, part);
    return prefix + QString::fromLatin1(hash.result().toHex());
}

// 整包摘要覆盖包里的全部条目,包括目录结构与文件字节。声明的摘要从不参与:一个能自己声明
// 摘要的包可以描述它并未携带的内容。
QString bundleContentIdentity(const QVector<TreeEntry> &tree)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("aegisy-extension-bundle-content/0.1\0"));
    for (const TreeEntry &entry : tree) {
        appendFramed(&hash, entry.directory ? QByteArrayLiteral("directory")
                                            : QByteArrayLiteral("file"));
        appendFramed(&hash, entry.relativePath.toUtf8());
        if (!entry.directory) appendFramed(&hash, entry.bytes);
    }
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(hash.result().toHex());
}

// 组件摘要覆盖该组件在包里指向的那些字节。组件声明一个路径前缀,摘要覆盖该前缀下的全部
// 条目;前缀不存在时组件的内容是空的,这仍然是一个确定的摘要而不是一个错误——人需要看到
// "这个组件没有携带内容"这件事,而不是让整个包因此读不出来。
QString componentContentIdentity(const QVector<TreeEntry> &tree,
                                 const QString &componentId,
                                 const QString &pathPrefix)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("aegisy-extension-bundle-component/0.1\0"));
    appendFramed(&hash, componentId.toUtf8());
    appendFramed(&hash, pathPrefix.toUtf8());
    for (const TreeEntry &entry : tree) {
        if (entry.relativePath != pathPrefix
                && !entry.relativePath.startsWith(pathPrefix + QLatin1Char('/'))) {
            continue;
        }
        appendFramed(&hash, entry.directory ? QByteArrayLiteral("directory")
                                            : QByteArrayLiteral("file"));
        appendFramed(&hash, entry.relativePath.toUtf8());
        if (!entry.directory) appendFramed(&hash, entry.bytes);
    }
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(hash.result().toHex());
}

// 声明的类型串映射到组件类型。不认识的串保留为 Unsupported 并带上原始串:丢弃它会让包的
// 实际行为超出预览所描述的范围,而预览层正是依据 Unsupported 决定失败关闭。
ExtensionComponentKind componentKindOf(const QString &declaredType)
{
    if (declaredType == QStringLiteral("skill")) {
        return ExtensionComponentKind::Skill;
    }
    if (declaredType == QStringLiteral("hook")) {
        return ExtensionComponentKind::Hook;
    }
    if (declaredType == QStringLiteral("mcp-server")) {
        return ExtensionComponentKind::McpServer;
    }
    if (declaredType == QStringLiteral("command")) {
        return ExtensionComponentKind::Command;
    }
    if (declaredType == QStringLiteral("asset")) {
        return ExtensionComponentKind::Asset;
    }
    return ExtensionComponentKind::Unsupported;
}

bool safePathPrefix(const QString &value)
{
    if (value.isEmpty() || value.size() > 1024) return false;
    if (value.startsWith(QLatin1Char('/')) || value.contains(QStringLiteral(".."))
            || value.contains(QLatin1Char('\\'))
            || value.contains(QLatin1Char(':'))
            || value.contains(QStringLiteral("//"))
            || value.endsWith(QLatin1Char('/'))) {
        return false;
    }
    for (const QString &segment : value.split(QLatin1Char('/'))) {
        if (!safeEntryName(segment)) return false;
    }
    return true;
}

bool stringArray(const QJsonValue &value,
                 int maximumItems,
                 int maximumLength,
                 QStringList *output)
{
    if (!value.isArray()) return false;
    const QJsonArray array = value.toArray();
    if (array.size() > maximumItems) return false;
    for (const QJsonValue &item : array) {
        if (!item.isString()) return false;
        const QString text = item.toString();
        // 能力串原样传递,不合并、不归一:在这一层做任何汇总都会毁掉逐组件披露存在的理由。
        // 重复的能力被拒绝而不是去重,因为无法判断哪一次声明是有意的。
        if (!Safety::safeDisplayText(text, maximumLength)
                || output->contains(text)) {
            return false;
        }
        output->append(text);
    }
    return true;
}

} // namespace

ExtensionBundleReadResult ExtensionBundleReader::read(const QString &rootPath)
{
    if (rootPath.isEmpty() || rootPath.size() > 4096
            || rootPath.toUtf8().size() > 16384) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-root-path-invalid"));
    }
    const QFileInfo suppliedRoot(rootPath);
    if (suppliedRoot.isSymLink()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-root-symlink-invalid"));
    }
    if (!suppliedRoot.exists()) {
        // 还没有包可以导入。这与一个畸形的包是两件事:把它报成 Invalid 会让界面显示
        // "这个包有问题",而实际情况是根本没有包。
        return failure(ExtensionBundleReadState::Empty, QString());
    }
    // 只接受目录。归档文件必须先解压才能读,而解压就是写盘,那是这一层被禁止做的事。

    // 只接受目录。归档文件必须先解压才能读,而解压就是写盘,那是这一层被禁止做的事。
    if (!suppliedRoot.isDir()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-root-not-directory"));
    }
    const QString root = suppliedRoot.canonicalFilePath();
    if (root.isEmpty()) {
        return failure(ExtensionBundleReadState::Unavailable,
                       QStringLiteral("extension-bundle-root-unavailable"));
    }

    QVector<TreeEntry> tree;
    ScanBudget budget;
    ExtensionBundleReadResult error;
    if (!scanDirectory(root, root, QString(), 0, &budget, &tree, &error)) {
        return error;
    }

    const TreeEntry *manifestEntry = findFile(
        tree, QString::fromLatin1(ManifestName));
    if (!manifestEntry) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-absent"));
    }
    const QByteArray bytes = manifestEntry->bytes;
    if (bytes.isEmpty() || bytes.size() > MaxManifestBytes
            || bytes.startsWith("\xef\xbb\xbf")) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-invalid"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    // 严格校验拒绝重复键与其他歧义构造:一份两种读法的清单意味着屏幕上展示的可能不是
    // 生效的那一份。
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !StrictJsonValidator::accepts(bytes)) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-invalid"));
    }
    const QJsonObject object = document.object();
    const QSet<QString> required{
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"),
        QStringLiteral("components")};
    const QSet<QString> allowed = required;
    const QStringList keys = object.keys();
    const QSet<QString> actual(keys.cbegin(), keys.cend());
    QSet<QString> missing = required;
    missing.subtract(actual);
    QSet<QString> unknown = actual;
    unknown.subtract(allowed);
    // 未知字段被拒绝而不是忽略。摘要字段尤其如此:忽略一个声明的摘要会让写清单的人以为
    // 那个字段生效了,而实际生效的是磁盘上的字节。
    if (!missing.isEmpty() || !unknown.isEmpty()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-fields-invalid"));
    }
    if (!object.value(QStringLiteral("id")).isString()
            || !object.value(QStringLiteral("name")).isString()
            || !object.value(QStringLiteral("version")).isString()
            || !object.value(QStringLiteral("components")).isArray()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-fields-invalid"));
    }

    ExtensionBundleManifest manifest;
    manifest.id = object.value(QStringLiteral("id")).toString();
    manifest.name = object.value(QStringLiteral("name")).toString();
    manifest.version = object.value(QStringLiteral("version")).toString();
    // 可展示性由共享呈现安全层判定,这一层不复制那套规则:两份副本会各自漂移,而漂移意味着
    // 读取器放行了预览会拒绝的字符。
    if (!Safety::validId(manifest.id)
            || !Safety::safeDisplayText(manifest.name, 128)
            || !Safety::safeDisplayText(manifest.version, 64)) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-manifest-unsafe"));
    }

    const QJsonArray components = object.value(QStringLiteral("components")).toArray();
    if (components.isEmpty()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-no-components"));
    }
    if (components.size() > MaxComponents) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-component-limit"));
    }
    QSet<QString> seenIds;
    for (const QJsonValue &value : components) {
        if (!value.isObject()) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-invalid"));
        }
        const QJsonObject entry = value.toObject();
        const QSet<QString> componentRequired{
            QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("type"),
            QStringLiteral("path"), QStringLiteral("capabilities")};
        const QStringList componentKeys = entry.keys();
        const QSet<QString> componentActual(componentKeys.cbegin(),
                                            componentKeys.cend());
        if (componentActual != componentRequired) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-fields-invalid"));
        }
        if (!entry.value(QStringLiteral("id")).isString()
                || !entry.value(QStringLiteral("name")).isString()
                || !entry.value(QStringLiteral("type")).isString()
                || !entry.value(QStringLiteral("path")).isString()) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-fields-invalid"));
        }

        ExtensionBundleComponent component;
        component.id = entry.value(QStringLiteral("id")).toString();
        component.name = entry.value(QStringLiteral("name")).toString();
        component.declaredType = entry.value(QStringLiteral("type")).toString();
        if (!Safety::validId(component.id)
                || !Safety::safeDisplayText(component.name, 128)
                || !Safety::safeDisplayText(component.declaredType, 64)) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-unsafe"));
        }
        // 同一个标识出现两次时无法判断哪一个的能力披露有效。
        if (seenIds.contains(component.id)) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-duplicate"));
        }
        seenIds.insert(component.id);
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (!safePathPrefix(path)) {
            return failure(ExtensionBundleReadState::Invalid,
                           QStringLiteral("extension-bundle-component-path-invalid"));
        }
        if (!stringArray(entry.value(QStringLiteral("capabilities")),
                         MaxCapabilitiesPerComponent, 64,
                         &component.requestedCapabilities)) {
            return failure(
                ExtensionBundleReadState::Invalid,
                QStringLiteral("extension-bundle-component-capabilities-invalid"));
        }
        component.kind = componentKindOf(component.declaredType);
        // 摘要由磁盘上的字节算出,与清单里写了什么无关。
        component.contentIdentity = componentContentIdentity(tree, component.id, path);
        manifest.components.append(component);
    }

    manifest.contentIdentity = bundleContentIdentity(tree);
    manifest.sourceIdentity = digestOf(
        QByteArrayLiteral("aegisy-extension-bundle-source/0.1\0"),
        {root.toUtf8()}, QStringLiteral("extension-source:sha256:"));

    ExtensionBundleReadResult result;
    result.state = ExtensionBundleReadState::Ready;
    result.manifest = manifest;
    return result;
}
