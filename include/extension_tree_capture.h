#ifndef EXTENSION_TREE_CAPTURE_H
#define EXTENSION_TREE_CAPTURE_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QVector>

// 扩展树的受控捕获层。`SkillExtensionInventory` 与 `ExtensionBundleReader` 曾各自携带一份
// 私有副本：同一套有界递归遍历、同一套确定性排序、同一套分帧摘要。两份副本会各自漂移，而
// 漂移意味着同一个包在两条路径上算出两个身份，而那个身份正是授权绑定的对象。因此这套机制
// 只有一份，由调用方提供自己的域。
//
// 域分隔是这一层的安全性质，不是格式细节。身份域进入被摘要的字节，因此一类调用方算出的
// 内容身份在另一类里不成立；诊断代码前缀保留各调用方原有的代码，因此抽取不会改变已经被
// 测试与文档固定的诊断串。未配置的域被直接拒绝，而不是退回某个默认域。
//
// 这一层只做树机制：符号链接拒绝、特殊文件拒绝、深度/条目/字节/路径长度上限、规范化路径
// 包含检查、读取后漂移复查、确定性排序与分帧摘要。清单解析、必需文件判定、组件分类等
// 调用方各自的校验不属于这一层，仍由各调用方持有。这一层不安装、不启用、不执行任何东西。
struct ExtensionTreeCaptureEntry {
    QString relativePath;
    bool directory = false;
    QByteArray bytes;
};

// 跨多次 scanDirectory 调用共享的预算。技能清单在整棵根目录的多棵子树间共享一份预算，
// 因此预算由调用方持有而不是每次扫描重置。
struct ExtensionTreeCaptureBudget {
    int entries = 0;
    qint64 bytes = 0;
};

// 调用方的域。任何一项为空都会让扫描与摘要整体失败，因此不存在"缺省域"。
struct ExtensionTreeCaptureDomain {
    // 内容身份的摘要域，包含域末尾的分隔字节，必须与历史摘要字节一致。
    QByteArray identityDomain;
    // 内容身份的展示前缀，例如 `extension-content:sha256:`。
    QString identityPrefix;
    // 固定诊断代码的前缀，例如 `skill` 或 `extension-bundle`。它进入诊断代码而不是
    // 摘要字节，因此既保持各调用方原有的代码不变，也不影响身份兼容性。
    QString errorPrefix;

    bool configured() const
    {
        return !identityDomain.isEmpty() && !identityPrefix.isEmpty()
            && !errorPrefix.isEmpty();
    }
};

enum class ExtensionTreeCaptureErrorState {
    // 树不可信：结构、预算、路径或漂移检查上有问题。
    Invalid,
    // 树读不出来，因此当前内容未知。
    Unavailable,
};

struct ExtensionTreeCaptureError {
    ExtensionTreeCaptureErrorState state = ExtensionTreeCaptureErrorState::Invalid;
    QString errorCode;
};

class ExtensionTreeCapture
{
public:
    static constexpr int MaxEntries = 4096;
    static constexpr int MaxDepth = 16;
    static constexpr qint64 MaxFileBytes = 2 * 1024 * 1024;
    static constexpr qint64 MaxTotalBytes = 16 * 1024 * 1024;

    // 单个条目名的安全判定：非空、长度受限、无控制字符、不含分隔符与冒号，且不是
    // `.` 或 `..`。大小写折叠后重名的条目在扫描中被拒绝：在不区分大小写的文件系统上，
    // 两个这样的名字指向同一份内容，而摘要会把它们算成两份。
    static bool safeEntryName(const QString &name);

    // 有界递归遍历。遍历顺序必须固定，否则同一棵树在两台机器上算出两个不同的摘要，而
    // 那个摘要是授权绑定的对象。符号链接被拒绝而不是跟随：跟随会让摘要覆盖树外的字节，
    // 而树外的内容不在人批准的范围里。域未配置时整体失败。
    static bool scanDirectory(const ExtensionTreeCaptureDomain &domain,
                              const QString &root,
                              const QString &directory,
                              const QString &relativeDirectory,
                              int depth,
                              ExtensionTreeCaptureBudget *budget,
                              QVector<ExtensionTreeCaptureEntry> *tree,
                              ExtensionTreeCaptureError *error);

    static const ExtensionTreeCaptureEntry *findFile(
        const QVector<ExtensionTreeCaptureEntry> &tree, const QString &path);

    // 整棵树的域分隔摘要，覆盖目录结构与文件字节。域未配置时返回空串。
    static QString contentIdentity(const ExtensionTreeCaptureDomain &domain,
                                   const QVector<ExtensionTreeCaptureEntry> &tree);

    // 长度前缀分帧摘要。每一段输入都带长度前缀：不带长度的拼接可以让两组不同的输入
    // 产生同一个摘要。域或前缀为空时返回空串，而不是退回某个默认域。
    static QString framedDigest(const QByteArray &domain,
                                const QList<QByteArray> &parts,
                                const QString &prefix);
};

#endif // EXTENSION_TREE_CAPTURE_H
