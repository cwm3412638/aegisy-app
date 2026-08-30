#ifndef EXTENSION_IMPORT_PREVIEW_H
#define EXTENSION_IMPORT_PREVIEW_H

#include "extension_registry.h"

#include <QList>
#include <QString>
#include <QStringList>

// 导入预览。一个插件包可以同时携带 Skills、hooks、MCP 配置、命令与资源,因此"导入这个
// 包"从来不是一个决定,而是一组决定。如果预览只展示包本身的名字与来源,那么人批准的是
// 一个标题,而实际被引入的是标题背后的全部可执行组件——其中任何一个都可能请求超出预期
// 的能力。
//
// 因此预览必须逐组件列出,并且每个组件都带上它自己请求的能力。整包级别的能力汇总不能
// 替代逐组件披露:两个组件各自请求"读文件"与"连网"时,汇总看起来与一个组件同时请求两者
// 完全一样,而后者才是真正危险的组合。
//
// 未知的可执行组件类型必须让导入**失败关闭**,而不是被跳过。跳过一个不认识的可执行组件
// 会让包的实际行为超出预览所描述的范围,而人是按预览做决定的。同时元数据必须保留下来
// 可供检视——失败关闭不等于把证据一起丢掉,否则没人能判断这个包到底想做什么。
//
// 这一层不解压、不写盘、不安装、不执行任何东西。它只把一份已解析的清单变成一个可判定的
// 预览结论。
enum class ExtensionComponentKind {
    Skill,
    Hook,
    McpServer,
    Command,
    // 非可执行的资源:它仍然必须被列出,但不请求能力。
    Asset,
    // 清单声明了当前不支持的可执行组件类型。
    Unsupported,
};

struct ExtensionBundleComponent {
    ExtensionComponentKind kind = ExtensionComponentKind::Unsupported;
    QString id;
    QString name;
    // 该组件自己请求的能力。整包汇总不能替代它。
    QStringList requestedCapabilities;
    // 清单里声明的原始类型串,用于在失败关闭时保留可检视的证据。
    QString declaredType;
    // 该组件内容的确切摘要。
    QString contentIdentity;
};

struct ExtensionBundleManifest {
    QString id;
    QString name;
    QString version;
    QString sourceIdentity;
    QString contentIdentity;
    QList<ExtensionBundleComponent> components;
};

enum class ExtensionImportPreviewState {
    // 每个组件都可被识别与安全展示,可以呈现给人做决定。
    Ready,
    // 存在不支持的可执行组件:导入失败关闭,但元数据仍然保留可供检视。
    FailedClosed,
    // 清单本身无法被安全展示,因此不能作为决定的依据。
    Unpresentable,
};

// 单个组件在预览中的呈现结果。
struct ExtensionComponentPreview {
    ExtensionComponentKind kind = ExtensionComponentKind::Unsupported;
    QString identifier;
    QString displayName;
    QString kindLabel;
    // 逐组件的能力披露,按固定顺序输出。
    QStringList capabilities;
    // 该组件请求了越出当前只读边界的能力。
    bool beyondReadOnly = false;
    // 该组件是不支持的可执行类型。它仍然被列出:失败关闭不等于隐藏证据。
    bool unsupported = false;
    QString declaredType;
    QString contentFingerprint;
};

struct ExtensionImportPreview {
    ExtensionImportPreviewState state =
        ExtensionImportPreviewState::Unpresentable;
    QString title;
    QString identifier;
    QString versionLabel;
    QString sourceFingerprint;
    QString contentFingerprint;
    // 每一个组件都在这里,包括不支持的那些。
    QList<ExtensionComponentPreview> components;
    // 至少一个组件请求了越出只读边界的能力。
    bool anyBeyondReadOnly = false;
    // 恒为假:预览不导入、不安装、不启用任何东西。
    bool grantsInstallation = false;
    QString errorCode;
};

class ExtensionImportPreviewBuilder
{
public:
    static constexpr int MaxComponents = 128;
    static constexpr int MaxCapabilitiesPerComponent = 32;

    static ExtensionImportPreview build(const ExtensionBundleManifest &manifest);

    static QString componentKindLabel(ExtensionComponentKind kind);

    // 该组件类型是否是可执行的。资源不可执行,因此一个不认识的**资源**类型不必让导入
    // 失败;而一个不认识的可执行类型必须。
    static bool executable(ExtensionComponentKind kind);
};

#endif // EXTENSION_IMPORT_PREVIEW_H
