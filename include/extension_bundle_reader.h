#ifndef EXTENSION_BUNDLE_READER_H
#define EXTENSION_BUNDLE_READER_H

#include "extension_import_preview.h"
#include "extension_tree_capture.h"

#include <QString>

// 从磁盘上读出一个扩展包的清单。`ExtensionImportPreviewBuilder` 已经能把一份清单变成
// 逐组件的预览结论,但没有任何东西能产出那份清单,因此"导入这个包"这个决定到此为止无法
// 在产品里被提出。这一层产出它。
//
// **读取一个包不解包。** 这一层只扫描一个已经存在的目录,不解压归档、不写临时文件、不创建
// 任何路径。解压就是写盘,而在权限、审批、沙箱与恢复门禁完成之前写盘正是被禁止的那件事;
// 一个"只是为了看看里面有什么"而先解压到临时目录的读取器,已经把包里的内容落到了磁盘上。
//
// **每一个摘要都由磁盘上的字节算出,绝不采用清单里声明的摘要。** 如果清单可以自己声明
// 组件摘要,那么一个包就能描述它并未携带的内容,而人恰恰是按逐组件披露做决定的:屏幕上
// 写着这个组件的内容是 A,实际被引入的是 B。清单里出现摘要字段一律拒绝,而不是忽略——
// 忽略会让写清单的人以为那个字段生效了。
//
// **不认识的组件类型保留为 Unsupported 并带上原始类型串,不丢弃。** 丢弃会让包的实际
// 行为超出预览所描述的范围。这一层不决定导入是否失败关闭——那是预览层的结论——但它必须
// 把证据完整交上去。
//
// **能力逐组件原样传递,不做合并或归一。** 两个组件各自请求"读文件"与"连网"时,汇总看
// 起来与一个组件同时请求两者完全一样,而后者才是真正危险的组合。在这一层做任何汇总都会
// 毁掉预览存在的理由。
//
// 文本能否安全展示由 `ExtensionDisplaySafety` 判定,这一层不复制那套规则:两份副本会各自
// 漂移,而漂移意味着读取器放行了预览会拒绝的字符,或者反过来。这一层只负责长度与数量的
// 预算上限,以及路径包含与符号链接这类来源性质的检查。
enum class ExtensionBundleReadState {
    // 目录不存在。这不是错误:还没有包可以导入。
    Empty,
    Ready,
    // 清单存在但不可信:结构、预算、路径或摘要计算上有问题。
    Invalid,
    // 目录或文件读不出来。与 Invalid 区分开,因为一个读不出来的包不等于一个畸形的包。
    Unavailable,
};

struct ExtensionBundleReadResult {
    ExtensionBundleReadState state = ExtensionBundleReadState::Invalid;
    ExtensionBundleManifest manifest;
    QString errorCode;
};

class ExtensionBundleReader
{
public:
    static constexpr int MaxComponents = 128;
    static constexpr int MaxCapabilitiesPerComponent = 32;
    static constexpr int MaxEntries = ExtensionTreeCapture::MaxEntries;
    static constexpr int MaxDepth = ExtensionTreeCapture::MaxDepth;
    static constexpr qint64 MaxManifestBytes = 64 * 1024;
    static constexpr qint64 MaxFileBytes = ExtensionTreeCapture::MaxFileBytes;
    static constexpr qint64 MaxTotalBytes = ExtensionTreeCapture::MaxTotalBytes;

    // rootPath 指向一个已经存在的包目录。这一层不接受归档文件路径,因为读取归档就意味着
    // 解压,而解压是写盘。
    static ExtensionBundleReadResult read(const QString &rootPath);

    static constexpr const char *ManifestName = "aegisy-bundle.json";
};

#endif // EXTENSION_BUNDLE_READER_H
