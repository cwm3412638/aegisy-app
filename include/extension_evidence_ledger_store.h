#ifndef EXTENSION_EVIDENCE_LEDGER_STORE_H
#define EXTENSION_EVIDENCE_LEDGER_STORE_H

#include "extension_evidence_ledger.h"

class QSettings;

// 扩展证据的持久化被拆成两半，和激活日志一致：授权（HMAC 密钥、已提交代号与身份）
// 放在平台安全存储里，体积较大的载荷字节放在 QSettings 里。两半必须互相印证，因此
// 删除任意一半都不能退化成"从未记录过"。
//
// 复核证据与启用授权的持久化需求完全一致——同样的三阶段发布、同样的反降级规则、同样
// 的比较并交换。把这套恢复逻辑复制两份会产生两个可以各自漂移的副本，因此它被抽取到
// 这一层，由调用方提供自己的域：授权模式串、QSettings 键、错误代码前缀与载荷层的域
// 全部由调用方指定，因此一类证据的授权信封在另一类里无法解析。
//
// 这一层只搬字节。它不判定信任、不授予启用、不安装、不执行任何东西。
class ExtensionEvidenceLedgerSecureStore
{
public:
    enum class ReadState {
        Missing,
        Found,
        Unavailable,
        Invalid,
    };
    enum class WriteOutcome {
        Committed,
        DefiniteFailure,
        OutcomeUnknown,
    };

    virtual ~ExtensionEvidenceLedgerSecureStore() = default;
    virtual ReadState readFresh(QByteArray *value, QString *errorCode) = 0;
    virtual WriteOutcome write(const QByteArray &value, QString *errorCode) = 0;
};

struct ExtensionEvidenceLedgerStoreDomain {
    // 载荷编解码的域，决定被持久化的载荷字节。
    ExtensionEvidenceLedgerDomain ledger;
    // 授权信封的模式串。它进入被持久化的授权字节，因此一类证据的授权在另一类里
    // 无法解析，重贴标签也不行。
    QString authoritySchema;
    // 载荷字节在 QSettings 里的键。
    QString recordKey;
    // 固定诊断代码的前缀，例如 `extension-review-store`。
    QString errorPrefix;
    // 条目相关诊断代码里的名词，例如 `pins`。
    QString entriesCodeNoun;

    bool configured() const
    {
        return ledger.configured() && !authoritySchema.isEmpty()
            && !recordKey.isEmpty() && !errorPrefix.isEmpty()
            && !entriesCodeNoun.isEmpty();
    }
};

enum class ExtensionEvidenceLedgerStoreState {
    // 授权与载荷都确实不存在。只有这个状态代表"从未记录过"。
    Empty,
    Ready,
    // 两半互相矛盾、载荷无法认证，或授权本身不合法。
    Invalid,
    // 后端被锁定或不可读，当前内容未知。
    Unavailable,
    // 写入结果无法确定，必须由人工确认后再继续。
    OutcomeUnknown,
};

struct ExtensionEvidenceLedgerStoreResult {
    ExtensionEvidenceLedgerStoreState state =
        ExtensionEvidenceLedgerStoreState::Invalid;
    QList<ExtensionEvidenceEntry> entries;
    qint64 generation = 0;
    QString identity;
    QString errorCode;
};

class ExtensionEvidenceLedgerStore
{
public:
    static constexpr qint64 MaxAuthorityBytes = 8 * 1024;

    ExtensionEvidenceLedgerStore(const ExtensionEvidenceLedgerStoreDomain &domain,
                                 ExtensionEvidenceLedgerSecureStore *secureStore,
                                 QSettings *settings);

    ExtensionEvidenceLedgerStoreResult load();

    // 集合只能整体替换，并且必须提交调用者读到的代号：并发的两次修改不允许静默覆盖
    // 彼此。`expectedGeneration` 为 0 表示调用者认为尚不存在任何载荷。
    // 丢弃一份自相矛盾的账本，把它重建为"从未记录过"。这不是 `replace` 的特例：`replace`
    // 拒绝在 `Invalid` 之上写入，而那正是恢复要处理的那个状态，因此恢复没有它就完全无法
    // 执行。这条路径**只清空**，永远不接受任何条目：一份自相矛盾的账本无法被"修复"成它
    // 大概曾经持有的集合，那是伪造证据，而唯一诚实的重建是空集合。
    //
    // 它只在 `load()` 确实返回 `Invalid` 时执行。可读的账本不得被它触碰：如果它能作用在
    // 健康账本上，它就是一条不经审批就清空一切的路径。
    //
    // **顺序是安全性的一部分：先销毁授权密钥，再删除载荷字节。** 两次写入不可能原子完成，
    // 因此必须选一个安全的中间态。先销毁密钥意味着任何残留的载荷字节从此无法被任何人认证，
    // 于是这次清空是不可逆的；反过来先删载荷、密钥仍在，则任何能把那些字节放回去的人都能
    // 让被收回的授权复活，而恢复的全部意义就是收回授权。两种中间态都仍然是 `Invalid`，
    // 因此都不会被误认为成功。
    bool discard(ExtensionEvidenceLedgerStoreResult *updated, QString *errorCode);

    bool replace(const QList<ExtensionEvidenceEntry> &entries,
                 qint64 expectedGeneration,
                 ExtensionEvidenceLedgerStoreResult *updated,
                 QString *errorCode);

private:
    ExtensionEvidenceLedgerStoreDomain m_domain;
    ExtensionEvidenceLedgerSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
};

#endif // EXTENSION_EVIDENCE_LEDGER_STORE_H
