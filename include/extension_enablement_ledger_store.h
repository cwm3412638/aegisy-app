#ifndef EXTENSION_ENABLEMENT_LEDGER_STORE_H
#define EXTENSION_ENABLEMENT_LEDGER_STORE_H

#include "extension_enablement_policy.h"

class QSettings;

// 启用授权的持久化。它与复核证据的持久化共用 `ExtensionEvidenceLedgerStore` 的三阶段
// 发布与恢复逻辑，但持有独立的授权模式串、QSettings 键与载荷域，因此一类证据的授权
// 信封与载荷在另一类里都无法被采用。
//
// 分半持久化的关键性质是反降级：删除任意一半都不是"从未授权过"，而是各自独立的失败。
// 退化成"从未授权过"在这里恰好是安全的方向（没有授权就不启用），但它会把"授权记录被
// 删除了"表述成"用户从未要求启用"，从而掩盖一次篡改。
//
// 这一层只搬字节。它不判定信任、不授予启用、不安装、不执行任何东西：读出的授权仍然
// 要经过 ExtensionEnablementPolicy，而判定还要求已复核、兼容且已安装。
class ExtensionEnablementLedgerSecureStore
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

    virtual ~ExtensionEnablementLedgerSecureStore() = default;
    virtual ReadState readFresh(QByteArray *value, QString *errorCode) = 0;
    virtual WriteOutcome write(const QByteArray &value, QString *errorCode) = 0;
};

enum class ExtensionEnablementLedgerStoreState {
    // 授权与载荷都确实不存在。只有这个状态代表"从未授权过"。
    Empty,
    Ready,
    // 两半互相矛盾、载荷无法认证，或授权本身不合法。
    Invalid,
    // 后端被锁定或不可读，当前内容未知。
    Unavailable,
    // 写入结果无法确定，必须由人工确认后再继续。
    OutcomeUnknown,
};

struct ExtensionEnablementLedgerStoreResult {
    ExtensionEnablementLedgerStoreState state =
        ExtensionEnablementLedgerStoreState::Invalid;
    QList<ExtensionEnablementGrant> grants;
    qint64 generation = 0;
    QString identity;
    QString errorCode;
};

class ExtensionEnablementLedgerStore
{
public:
    ExtensionEnablementLedgerStore(
        ExtensionEnablementLedgerSecureStore *secureStore,
        QSettings *settings);

    ExtensionEnablementLedgerStoreResult load();

    // 授权集合只能整体替换，并且必须提交调用者读到的代号：并发的两次修改不允许静默
    // 覆盖彼此。`expectedGeneration` 为 0 表示调用者认为尚不存在任何载荷。
    bool replace(const QList<ExtensionEnablementGrant> &grants,
                 qint64 expectedGeneration,
                 ExtensionEnablementLedgerStoreResult *updated,
                 QString *errorCode);

    static QString recordSettingsKey();

private:
    ExtensionEnablementLedgerSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
};

#endif // EXTENSION_ENABLEMENT_LEDGER_STORE_H
