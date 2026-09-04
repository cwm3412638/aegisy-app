#include "extension_bundle_reader.h"

#include "extension_display_safety.h"
#include "extension_tree_capture.h"
#include "strict_json_validator.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QVector>

namespace {

using Safety = ExtensionDisplaySafety;

// 扩展包读取的树捕获域。身份域与诊断代码前缀必须与历史摘要字节和诊断串完全一致；树机制
// 本身由共享层持有，这一层不再保留第二份副本。
const ExtensionTreeCaptureDomain &bundleTreeCaptureDomain()
{
    static const ExtensionTreeCaptureDomain domain{
        QByteArrayLiteral("aegisy-extension-bundle-content/0.1\0"),
        QStringLiteral("extension-content:sha256:"),
        QStringLiteral("extension-bundle")};
    return domain;
}

ExtensionBundleReadResult failure(ExtensionBundleReadState state,
                                  const QString &code)
{
    ExtensionBundleReadResult result;
    result.state = state;
    result.errorCode = code;
    return result;
}

ExtensionBundleReadResult captureFailure(const ExtensionTreeCaptureError &error)
{
    return failure(error.state == ExtensionTreeCaptureErrorState::Unavailable
                       ? ExtensionBundleReadState::Unavailable
                       : ExtensionBundleReadState::Invalid,
                   error.errorCode);
}

// 整包摘要覆盖包里的全部条目，包括目录结构与文件字节。声明的摘要从不参与：一个能自己声明
// 摘要的包可以描述它并未携带的内容。摘要的分帧与域分隔由共享树捕获层完成，这一层只提供
// 自己的域。
QString bundleContentIdentity(const QVector<ExtensionTreeCaptureEntry> &tree)
{
    return ExtensionTreeCapture::contentIdentity(bundleTreeCaptureDomain(), tree);
}

// 组件摘要覆盖该组件在包里指向的那些字节。组件声明一个路径前缀，摘要覆盖该前缀下的全部
// 条目；前缀不存在时组件的内容是空的，这仍然是一个确定的摘要而不是一个错误——人需要看到
// "这个组件没有携带内容"这件事，而不是让整个包因此读不出来。
QString componentContentIdentity(const QVector<ExtensionTreeCaptureEntry> &tree,
                                 const QString &componentId,
                                 const QString &pathPrefix)
{
    QList<QByteArray> parts;
    parts.append(componentId.toUtf8());
    parts.append(pathPrefix.toUtf8());
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (entry.relativePath != pathPrefix
                && !entry.relativePath.startsWith(pathPrefix + QLatin1Char('/'))) {
            continue;
        }
        parts.append(entry.directory ? QByteArrayLiteral("directory")
                                     : QByteArrayLiteral("file"));
        parts.append(entry.relativePath.toUtf8());
        if (!entry.directory) parts.append(entry.bytes);
    }
    return ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-extension-bundle-component/0.1\0"), parts,
        QStringLiteral("extension-content:sha256:"));
}

// 声明的类型串映射到组件类型。不认识的串保留为 Unsupported 并带上原始串：丢弃它会让包的
// 实际行为超出预览所描述的范围，而预览层正是依据 Unsupported 决定失败关闭。
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
        if (!ExtensionTreeCapture::safeEntryName(segment)) return false;
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
        // 能力串原样传递，不合并、不归一：在这一层做任何汇总都会毁掉逐组件披露存在的理由。
        // 重复的能力被拒绝而不是去重，因为无法判断哪一次声明是有意的。
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
        // 还没有包可以导入。这与一个畸形的包是两件事：把它报成 Invalid 会让界面显示
        // "这个包有问题"，而实际情况是根本没有包。
        return failure(ExtensionBundleReadState::Empty, QString());
    }
    // 只接受目录。归档文件必须先解压才能读，而解压就是写盘，那是这一层被禁止做的事。
    if (!suppliedRoot.isDir()) {
        return failure(ExtensionBundleReadState::Invalid,
                       QStringLiteral("extension-bundle-root-not-directory"));
    }
    const QString root = suppliedRoot.canonicalFilePath();
    if (root.isEmpty()) {
        return failure(ExtensionBundleReadState::Unavailable,
                       QStringLiteral("extension-bundle-root-unavailable"));
    }

    QVector<ExtensionTreeCaptureEntry> tree;
    ExtensionTreeCaptureBudget budget;
    ExtensionTreeCaptureError scanError;
    if (!ExtensionTreeCapture::scanDirectory(bundleTreeCaptureDomain(), root, root,
                                             QString(), 0, &budget, &tree,
                                             &scanError)) {
        return captureFailure(scanError);
    }

    const ExtensionTreeCaptureEntry *manifestEntry = ExtensionTreeCapture::findFile(
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
    // 严格校验拒绝重复键与其他歧义构造：一份两种读法的清单意味着屏幕上展示的可能不是
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
    // 未知字段被拒绝而不是忽略。摘要字段尤其如此：忽略一个声明的摘要会让写清单的人以为
    // 那个字段生效了，而实际生效的是磁盘上的字节。
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
    // 可展示性由共享呈现安全层判定，这一层不复制那套规则：两份副本会各自漂移，而漂移意味着
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
        // 摘要由磁盘上的字节算出，与清单里写了什么无关。
        component.contentIdentity = componentContentIdentity(tree, component.id, path);
        manifest.components.append(component);
    }

    manifest.contentIdentity = bundleContentIdentity(tree);
    manifest.sourceIdentity = ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-extension-bundle-source/0.1\0"),
        {root.toUtf8()}, QStringLiteral("extension-source:sha256:"));

    ExtensionBundleReadResult result;
    result.state = ExtensionBundleReadState::Ready;
    result.manifest = manifest;
    return result;
}
