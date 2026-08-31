#include "extension_import_presentation.h"

namespace {

// 每一条返回路径都必须把这两个恒假字段写出来。一次被拒绝的披露同样不导入、同样不写盘,
// 而把它们留给结构体默认值会让"披露不导入"这条规则在源码里没有任何一处可读的声明。
ExtensionImportDisclosure refuse(ExtensionImportDisclosureState state,
                                 const QString &code)
{
    ExtensionImportDisclosure disclosure;
    disclosure.state = state;
    disclosure.importsBundle = false;
    disclosure.writesToDisk = false;
    disclosure.errorCode = code;
    return disclosure;
}

} // namespace

QString ExtensionImportPresentation::stateLabel(
    ExtensionImportDisclosureState state)
{
    switch (state) {
    case ExtensionImportDisclosureState::Absent:
        return QStringLiteral("没有待披露的扩展包");
    case ExtensionImportDisclosureState::Ready:
        // 明确说清楚这是一次披露而不是一次导入。
        return QStringLiteral("已读出包内容，尚未导入任何内容");
    case ExtensionImportDisclosureState::FailedClosed:
        return QStringLiteral("包含不支持的可执行组件，导入失败关闭");
    case ExtensionImportDisclosureState::Unreadable:
        // 读不出来要去看权限,而不是去修包。
        return QStringLiteral("目录或文件读不出来，无法披露");
    case ExtensionImportDisclosureState::Unpresentable:
        return QStringLiteral("包无法安全展示，不能作为决定的依据");
    }
    return QStringLiteral("包无法安全展示，不能作为决定的依据");
}

ExtensionImportDisclosure ExtensionImportPresentation::build(
    const ExtensionBundleReadResult &read)
{
    // 读取失败时绝不构造预览。一次失败读取里的清单是垃圾,对它做预览有可能算出 Ready,
    // 于是一个读不出来的包在屏幕上变成一个可以批准的包。
    switch (read.state) {
    case ExtensionBundleReadState::Empty:
        return refuse(ExtensionImportDisclosureState::Absent, QString());
    case ExtensionBundleReadState::Unavailable:
        return refuse(ExtensionImportDisclosureState::Unreadable,
                      read.errorCode);
    case ExtensionBundleReadState::Invalid:
        return refuse(ExtensionImportDisclosureState::Unpresentable,
                      read.errorCode);
    case ExtensionBundleReadState::Ready:
        break;
    }

    const ExtensionImportPreview preview =
        ExtensionImportPreviewBuilder::build(read.manifest);
    if (preview.state == ExtensionImportPreviewState::Unpresentable) {
        // 判定层的诊断原样带出。这一层再编一个自己的诊断会让人拿着一个查不到出处的代号。
        return refuse(ExtensionImportDisclosureState::Unpresentable,
                      preview.errorCode);
    }

    ExtensionImportDisclosure disclosure;
    disclosure.state = preview.state == ExtensionImportPreviewState::FailedClosed
        ? ExtensionImportDisclosureState::FailedClosed
        : ExtensionImportDisclosureState::Ready;
    disclosure.title = preview.title;
    disclosure.identifier = preview.identifier;
    disclosure.versionLabel = preview.versionLabel;
    disclosure.sourceFingerprint = preview.sourceFingerprint;
    disclosure.contentFingerprint = preview.contentFingerprint;
    // 失败关闭保留全部组件证据,包括那个不支持的组件:隐藏它会让没人能判断这个包到底想
    // 做什么。能力仍然逐组件,这一层不做任何整包汇总。
    disclosure.components = preview.components;
    disclosure.anyBeyondReadOnly = preview.anyBeyondReadOnly;
    disclosure.importsBundle = false;
    disclosure.writesToDisk = false;
    disclosure.errorCode = preview.errorCode;
    return disclosure;
}
