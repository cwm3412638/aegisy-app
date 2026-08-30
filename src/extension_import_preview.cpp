#include "extension_import_preview.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

const QString &contentPrefix()
{
    static const QString value = QStringLiteral("extension-content:sha256:");
    return value;
}

const QString &sourcePrefix()
{
    static const QString value = QStringLiteral("extension-source:sha256:");
    return value;
}

ExtensionImportPreview unpresentable(const QString &code)
{
    ExtensionImportPreview preview;
    preview.state = ExtensionImportPreviewState::Unpresentable;
    preview.grantsInstallation = false;
    preview.errorCode = code;
    return preview;
}

} // namespace

bool ExtensionImportPreviewBuilder::executable(ExtensionComponentKind kind)
{
    switch (kind) {
    case ExtensionComponentKind::Skill:
    case ExtensionComponentKind::Hook:
    case ExtensionComponentKind::McpServer:
    case ExtensionComponentKind::Command:
        return true;
    case ExtensionComponentKind::Asset:
        return false;
    case ExtensionComponentKind::Unsupported:
        // 不认识的类型按可执行处理。把它当作资源会让一个未知的可执行组件被静默跳过,
        // 而人是按预览做决定的。
        return true;
    }
    // 未知取值同样按可执行处理:新增的组件类型不应默认被当成无害的资源。
    return true;
}

QString ExtensionImportPreviewBuilder::componentKindLabel(
    ExtensionComponentKind kind)
{
    switch (kind) {
    case ExtensionComponentKind::Skill:
        return QStringLiteral("Skill");
    case ExtensionComponentKind::Hook:
        return QStringLiteral("钩子");
    case ExtensionComponentKind::McpServer:
        return QStringLiteral("MCP 服务器");
    case ExtensionComponentKind::Command:
        return QStringLiteral("命令");
    case ExtensionComponentKind::Asset:
        return QStringLiteral("资源");
    case ExtensionComponentKind::Unsupported:
        return QStringLiteral("不支持的组件");
    }
    return QStringLiteral("不支持的组件");
}

ExtensionImportPreview ExtensionImportPreviewBuilder::build(
    const ExtensionBundleManifest &manifest)
{
    if (!Safety::validId(manifest.id)) {
        return unpresentable(QStringLiteral("extension-import-id-invalid"));
    }
    if (!Safety::hashIdentity(manifest.contentIdentity, contentPrefix())
            || !Safety::hashIdentity(manifest.sourceIdentity, sourcePrefix())) {
        return unpresentable(QStringLiteral("extension-import-identity-invalid"));
    }
    if (!Safety::safeDisplayText(manifest.name, 128)) {
        return unpresentable(QStringLiteral("extension-import-name-unsafe"));
    }
    if (!Safety::safeDisplayText(manifest.version, 64)) {
        return unpresentable(QStringLiteral("extension-import-version-unsafe"));
    }
    // 空包不构成一次可批准的导入:预览里没有任何组件时,人无从知道自己批准了什么。
    if (manifest.components.isEmpty()) {
        return unpresentable(QStringLiteral("extension-import-no-components"));
    }
    if (manifest.components.size() > MaxComponents) {
        return unpresentable(QStringLiteral("extension-import-component-limit"));
    }

    ExtensionImportPreview preview;
    preview.title = manifest.name;
    preview.identifier = manifest.id;
    preview.versionLabel = manifest.version;
    preview.sourceFingerprint = Safety::fingerprint(manifest.sourceIdentity);
    preview.contentFingerprint = Safety::fingerprint(manifest.contentIdentity);
    // 预览永远不授予安装:它只是把包里的内容如实列出来供人判断。
    preview.grantsInstallation = false;

    bool failedClosed = false;
    QStringList seen;
    for (const ExtensionBundleComponent &component : manifest.components) {
        if (!Safety::validId(component.id)) {
            return unpresentable(
                QStringLiteral("extension-import-component-id-invalid"));
        }
        // 同一个标识出现两次时无法判断哪一个的能力披露有效。
        if (seen.contains(component.id)) {
            return unpresentable(
                QStringLiteral("extension-import-component-duplicate"));
        }
        seen.append(component.id);
        if (!Safety::safeDisplayText(component.name, 128)) {
            return unpresentable(
                QStringLiteral("extension-import-component-name-unsafe"));
        }
        if (!Safety::hashIdentity(component.contentIdentity, contentPrefix())) {
            return unpresentable(
                QStringLiteral("extension-import-component-identity-invalid"));
        }
        if (component.requestedCapabilities.size() > MaxCapabilitiesPerComponent) {
            return unpresentable(
                QStringLiteral("extension-import-component-capability-limit"));
        }

        ExtensionComponentPreview item;
        item.kind = component.kind;
        item.identifier = component.id;
        // 名称不安全的情况已经在上面被拒绝,因此这里的名称可以直接展示;名称为空时回退到
        // 标识,而不是留下一行没有主体的披露。
        item.displayName = component.name.isEmpty() ? component.id : component.name;
        item.kindLabel = componentKindLabel(component.kind);
        item.contentFingerprint = Safety::fingerprint(component.contentIdentity);
        item.declaredType = component.declaredType;

        // 逐组件披露能力。整包汇总不能替代它:两个组件各自请求"读文件"与"连网"时,汇总
        // 看起来与一个组件同时请求两者完全一样,而后者才是真正危险的组合。
        for (const QString &capability : component.requestedCapabilities) {
            if (!Safety::safeDisplayText(capability, 64)) {
                return unpresentable(
                    QStringLiteral("extension-import-component-capability-unsafe"));
            }
            if (item.capabilities.contains(capability)) {
                return unpresentable(
                    QStringLiteral("extension-import-component-capability-duplicate"));
            }
            item.capabilities.append(capability);
            if (Safety::beyondReadOnly(capability)) item.beyondReadOnly = true;
        }
        if (item.beyondReadOnly) preview.anyBeyondReadOnly = true;

        // 不认识的可执行组件让导入失败关闭。它仍然被列进预览:失败关闭不等于把证据一起
        // 丢掉,否则没人能判断这个包到底想做什么。
        if (component.kind == ExtensionComponentKind::Unsupported) {
            item.unsupported = true;
            failedClosed = true;
        }
        preview.components.append(item);
    }

    preview.state = failedClosed
        ? ExtensionImportPreviewState::FailedClosed
        : ExtensionImportPreviewState::Ready;
    if (failedClosed) {
        preview.errorCode =
            QStringLiteral("extension-import-unsupported-component");
    }
    return preview;
}
