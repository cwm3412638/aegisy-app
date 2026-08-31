#ifndef EXTENSION_IMPORT_PRESENTATION_H
#define EXTENSION_IMPORT_PRESENTATION_H

#include "extension_bundle_reader.h"
#include "extension_import_preview.h"

#include <QList>
#include <QString>

// 把一次包读取变成一份可以给人看的披露。`ExtensionBundleReader` 产出清单,
// `ExtensionImportPreviewBuilder` 判定清单,但两者各自会以不同的方式拒绝,而那两类拒绝
// 要求人做不同的事:一个读不出来的目录要去看权限,一个畸形的包要去修包。把它们并成一个
// "无效"会把人送去重写一个本来没问题的包。这一层把两层的状态合成一个结论,并且把是哪一
// 层拒绝的原样带出来,绝不自己重新编一个诊断。
//
// **披露不导入。** 这一层与它的界面都不解包、不写盘、不安装、不启用任何东西:导入意味着
// 把内容落到磁盘上,而在权限、审批、沙箱与恢复门禁完成之前写盘正是被禁止的那件事。
// `importsBundle` 与 `writesToDisk` 是显式暴露的恒假字段而不是省略,因为界面若把"已经
// 看过这个包的内容"说成"已经导入这个包",人会以为磁盘上已经多了一份东西并据此往下走。
//
// **读取失败时不构造预览。** 一次失败的读取里的清单是垃圾:对它做预览有可能算出一个
// Ready 结论,于是一个读不出来的包在屏幕上变成一个可以批准的包。只有读取成功才预览。
//
// **失败关闭保留全部组件证据,包括那个不支持的组件。** 隐藏证据会让没人能判断这个包到底
// 想做什么。而其余的失败状态里组件列表必须是空的:那些情况下我们并没有读到任何组件,
// 列出任何一行都是在展示我们没有读到的东西。
//
// 能力仍然逐组件披露,这一层不做任何整包汇总:两个组件各自请求"读文件"与"连网"时,汇总
// 看起来与一个组件同时请求两者完全一样,而后者才是真正危险的组合。
enum class ExtensionImportDisclosureState {
    // 目录不存在。这不是错误:还没有包可以披露。
    Absent,
    // 可以完整披露给人做决定。
    Ready,
    // 存在不支持的可执行组件:导入失败关闭,证据完整保留。
    FailedClosed,
    // 目录或文件读不出来。与畸形区分开:要去看的是权限,不是包。
    Unreadable,
    // 包畸形,或清单无法安全展示,因此不能作为决定的依据。
    Unpresentable,
};

struct ExtensionImportDisclosure {
    ExtensionImportDisclosureState state =
        ExtensionImportDisclosureState::Unpresentable;
    QString title;
    QString identifier;
    QString versionLabel;
    QString sourceFingerprint;
    QString contentFingerprint;
    // 每一个组件都在这里,包括不支持的那些。非 Ready/FailedClosed 状态下恒为空。
    QList<ExtensionComponentPreview> components;
    // 至少一个组件请求了越出只读边界的能力。
    bool anyBeyondReadOnly = false;
    // 恒为假:披露不导入任何东西。
    bool importsBundle = false;
    // 恒为假:这一层不写盘。
    bool writesToDisk = false;
    // 拒绝这次披露的那一层给出的诊断,原样带出。
    QString errorCode;
};

class ExtensionImportPresentation
{
public:
    static ExtensionImportDisclosure build(const ExtensionBundleReadResult &read);

    static QString stateLabel(ExtensionImportDisclosureState state);
};

#endif // EXTENSION_IMPORT_PRESENTATION_H
