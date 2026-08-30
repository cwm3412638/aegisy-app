#include "extension_import_preview.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTextStream>

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

QString identity(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

QString sourceOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-source:sha256:"), seed + "-source");
}

QString contentOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-content:sha256:"), seed + "-content");
}

ExtensionBundleComponent component(ExtensionComponentKind kind, const QString &id,
                                   const QStringList &capabilities = {})
{
    ExtensionBundleComponent value;
    value.kind = kind;
    value.id = id;
    value.name = id;
    value.requestedCapabilities = capabilities;
    value.contentIdentity = contentOf(id.toUtf8());
    return value;
}

// 一个携带多个组件的包:这正是"导入这个包"从来不是一个决定的情形。
ExtensionBundleManifest manifest()
{
    ExtensionBundleManifest value;
    value.id = QStringLiteral("acme.bundle");
    value.name = QStringLiteral("Acme Bundle");
    value.version = QStringLiteral("1.4.0");
    value.sourceIdentity = sourceOf("acme");
    value.contentIdentity = contentOf("acme");
    value.components = {
        component(ExtensionComponentKind::Skill, QStringLiteral("acme.format"),
                  {QStringLiteral("skill-content")}),
        component(ExtensionComponentKind::McpServer, QStringLiteral("acme.tools"),
                  {QStringLiteral("mcp-tools"), QStringLiteral("network")}),
        component(ExtensionComponentKind::Asset, QStringLiteral("acme.icons")),
    };
    return value;
}

// 每个组件都必须被逐一列出,并带上它自己请求的能力。
void readyTests()
{
    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(manifest());
    expect(preview.state == ExtensionImportPreviewState::Ready,
           "a recognizable bundle was not previewable");
    expect(preview.errorCode.isEmpty(), "a ready preview carried an error code");
    // 关键:组件数量与清单一致。少列一个组件会让人批准一个自己没有看到的可执行内容。
    expect(preview.components.size() == manifest().components.size(),
           "the preview did not list every component in the bundle");
    // 每个组件的能力披露必须是它自己的,而不是整包汇总。
    expect(preview.components.at(0).capabilities
               == QStringList{QStringLiteral("skill-content")},
           "a component's capability disclosure is not its own");
    expect(preview.components.at(1).capabilities
               == QStringList{QStringLiteral("mcp-tools"), QStringLiteral("network")},
           "a component's capability disclosure is not its own");
    expect(preview.components.at(2).capabilities.isEmpty(),
           "an asset was attributed capabilities it never requested");
    // 每个组件都带自己的类型标签与内容摘要,否则无法分辨谁请求了什么。
    for (const ExtensionComponentPreview &item : preview.components) {
        expect(!item.kindLabel.isEmpty() && !item.identifier.isEmpty()
                   && !item.contentFingerprint.isEmpty(),
               "a previewed component is not identifiable");
        expect(!item.unsupported, "a recognizable component was marked unsupported");
    }
    // 预览不导入任何东西。
    expect(!preview.grantsInstallation,
           "building a preview granted installation");
    expect(!preview.anyBeyondReadOnly,
           "a read-only bundle was reported as beyond read-only");
}

// 不认识的可执行组件必须让导入失败关闭,同时保留可检视的元数据。
void failClosedTests()
{
    ExtensionBundleManifest value = manifest();
    ExtensionBundleComponent unknown =
        component(ExtensionComponentKind::Unsupported, QStringLiteral("acme.daemon"));
    unknown.declaredType = QStringLiteral("native-daemon");
    value.components.append(unknown);

    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(value);
    expect(preview.state == ExtensionImportPreviewState::FailedClosed,
           "an unsupported executable component did not fail the import closed");
    expect(preview.errorCode
               == QStringLiteral("extension-import-unsupported-component"),
           "a failed-closed import did not report why");
    // 关键:失败关闭不等于把证据丢掉。所有组件仍然被列出,包括不支持的那个。
    expect(preview.components.size() == value.components.size(),
           "failing closed discarded the component list");
    const ExtensionComponentPreview &item = preview.components.last();
    expect(item.unsupported, "the unsupported component was not marked as such");
    // 声明的原始类型串必须保留:否则没人能判断这个包到底想做什么。
    expect(item.declaredType == QStringLiteral("native-daemon"),
           "the declared component type was not preserved for inspection");
    expect(!item.contentFingerprint.isEmpty(),
           "the unsupported component's content is not inspectable");
    // 失败关闭时同样不授予安装。
    expect(!preview.grantsInstallation,
           "a failed-closed import still granted installation");

    // 不认识的类型按可执行处理:把它当作资源会让它被静默跳过。
    expect(ExtensionImportPreviewBuilder::executable(
               ExtensionComponentKind::Unsupported),
           "an unrecognized component type was treated as a harmless asset");
    // 未知取值同样按可执行处理。
    expect(ExtensionImportPreviewBuilder::executable(
               static_cast<ExtensionComponentKind>(9999)),
           "an unclassified component type defaulted to non-executable");
    // 已知的可执行类型必须被识别为可执行。
    for (const ExtensionComponentKind kind : {
             ExtensionComponentKind::Skill, ExtensionComponentKind::Hook,
             ExtensionComponentKind::McpServer, ExtensionComponentKind::Command}) {
        expect(ExtensionImportPreviewBuilder::executable(kind),
               "an executable component type was classified as non-executable");
    }
    expect(!ExtensionImportPreviewBuilder::executable(
               ExtensionComponentKind::Asset),
           "an asset was classified as executable");
}

// 越出只读边界的能力必须被逐组件标记,并在整包层面可见。
void beyondReadOnlyTests()
{
    ExtensionBundleManifest value = manifest();
    value.components.append(component(
        ExtensionComponentKind::Hook, QStringLiteral("acme.hook"),
        {QStringLiteral("filesystem-write")}));
    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(value);
    expect(preview.state == ExtensionImportPreviewState::Ready,
           "a recognizable bundle with a write request was not previewable");
    expect(preview.components.last().beyondReadOnly,
           "a beyond-read-only capability was not marked on its component");
    expect(preview.anyBeyondReadOnly,
           "a bundle containing a write request did not disclose it");
    // 关键:标记是逐组件的。请求只读能力的组件不得被邻居的越界请求染色。
    expect(!preview.components.at(0).beyondReadOnly,
           "a read-only component inherited its neighbour's write request");
}

// 清单本身无法安全展示时不能作为决定的依据。
void unpresentableTests()
{
    ExtensionBundleManifest badId = manifest();
    badId.id = QStringLiteral("Bad Id");
    expect(ExtensionImportPreviewBuilder::build(badId).errorCode
               == QStringLiteral("extension-import-id-invalid"),
           "a malformed bundle identifier was previewed");

    ExtensionBundleManifest spoofed = manifest();
    spoofed.name = QStringLiteral("Acme‮Bundle");
    expect(ExtensionImportPreviewBuilder::build(spoofed).state
               == ExtensionImportPreviewState::Unpresentable,
           "a bidirectional override in the bundle name was previewed");

    ExtensionBundleManifest badIdentity = manifest();
    badIdentity.contentIdentity = QStringLiteral("extension-content:sha256:abc");
    expect(ExtensionImportPreviewBuilder::build(badIdentity).errorCode
               == QStringLiteral("extension-import-identity-invalid"),
           "a truncated bundle content identity was previewed");

    // 空包不构成一次可批准的导入:人无从知道自己批准了什么。
    ExtensionBundleManifest empty = manifest();
    empty.components.clear();
    expect(ExtensionImportPreviewBuilder::build(empty).errorCode
               == QStringLiteral("extension-import-no-components"),
           "an empty bundle was offered as an approvable import");

    // 组件级别的展示安全同样必须成立。
    ExtensionBundleManifest spoofedComponent = manifest();
    spoofedComponent.components[1].name = QStringLiteral("Tools​Server");
    expect(ExtensionImportPreviewBuilder::build(spoofedComponent).errorCode
               == QStringLiteral("extension-import-component-name-unsafe"),
           "an invisible character in a component name was previewed");

    ExtensionBundleManifest badComponentId = manifest();
    badComponentId.components[0].id = QStringLiteral("Bad Component");
    expect(ExtensionImportPreviewBuilder::build(badComponentId).errorCode
               == QStringLiteral("extension-import-component-id-invalid"),
           "a malformed component identifier was previewed");

    // 同一个标识出现两次时无法判断哪一份能力披露有效。
    ExtensionBundleManifest duplicated = manifest();
    duplicated.components.append(
        component(ExtensionComponentKind::Skill, QStringLiteral("acme.format")));
    expect(ExtensionImportPreviewBuilder::build(duplicated).errorCode
               == QStringLiteral("extension-import-component-duplicate"),
           "a bundle with duplicate component identifiers was previewed");

    ExtensionBundleManifest duplicateCapability = manifest();
    duplicateCapability.components[1].requestedCapabilities.append(
        QStringLiteral("network"));
    expect(ExtensionImportPreviewBuilder::build(duplicateCapability).errorCode
               == QStringLiteral("extension-import-component-capability-duplicate"),
           "a duplicated capability disclosure was previewed");

    ExtensionBundleManifest overLimit = manifest();
    for (int index = 0; index <= ExtensionImportPreviewBuilder::MaxComponents;
         ++index) {
        overLimit.components.append(component(
            ExtensionComponentKind::Asset,
            QStringLiteral("acme.asset%1").arg(index)));
    }
    expect(ExtensionImportPreviewBuilder::build(overLimit).errorCode
               == QStringLiteral("extension-import-component-limit"),
           "an unbounded component list was previewed");

    // 无法展示时不得泄露任何组件:一个不可信的清单不应决定屏幕上出现什么。
    const ExtensionImportPreview rejected =
        ExtensionImportPreviewBuilder::build(badId);
    expect(rejected.components.isEmpty() && !rejected.grantsInstallation,
           "an unpresentable manifest still produced component rows");
}

// 预览不安装、不启用、不执行任何东西。
void authorityTests()
{
    for (const ExtensionImportPreview &preview : {
             ExtensionImportPreviewBuilder::build(manifest()),
             ExtensionImportPreviewBuilder::build(ExtensionBundleManifest())}) {
        expect(!preview.grantsInstallation,
               "some preview path granted installation");
    }
    // 组件类型标签对每一个已定义取值都必须存在,否则界面会出现一行没有类型的披露。
    for (const ExtensionComponentKind kind : {
             ExtensionComponentKind::Skill, ExtensionComponentKind::Hook,
             ExtensionComponentKind::McpServer, ExtensionComponentKind::Command,
             ExtensionComponentKind::Asset, ExtensionComponentKind::Unsupported}) {
        expect(!ExtensionImportPreviewBuilder::componentKindLabel(kind).isEmpty(),
               "a component kind has no display label");
    }
    expect(!ExtensionImportPreviewBuilder::componentKindLabel(
               static_cast<ExtensionComponentKind>(9999)).isEmpty(),
           "an unclassified component kind has no display label");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    readyTests();
    failClosedTests();
    beyondReadOnlyTests();
    unpresentableTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension import preview tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
