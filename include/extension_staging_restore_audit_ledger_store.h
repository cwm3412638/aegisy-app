#ifndef EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_STORE_H
#define EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_STORE_H

#include "extension_staging_restore_audit_ledger.h"

class QSettings;

// 暂存恢复审批审计链的持久化。持久化被拆成两半，与复核记录、启用授权及激活日志一致：
// 授权（HMAC 密钥、已提交代号与身份）放在平台安全存储里，体积较大的载荷字节放在
// QSettings 里。两半必须互相印证，因此删除任意一半都不能退化成"从未记录过任何决定"。
//
// 分半持久化的关键性质是反降级：删除任意一半都不是"从未审计过"，而是各自独立的失败。
// 退化成"从未记录过"会把"审计记录被删除了"表述成"这个问题从未被问过"，从而掩盖一次
// 篡改。
//
// 追加语义：审计链天然是追加式的，但本层沿用既有账本的"完整集合比较并交换"机器——
// 追加即"读出当前集合、在末尾加上新条目、连同读到的代号整体提交"，因此只有一个篡改
// 模型、一套三阶段发布与一套恢复逻辑。集合有界（MaxEntries）：写满后以独立代号拒绝
// 新条目，绝不静默驱逐历史。
//
// 域分隔在这一层同样成立：授权模式串、QSettings 键与载荷域全部独立于复核记录与启用
// 授权，因此那两个子系统的授权信封或载荷都无法被当作恢复审计记录采用。
//
// 这一层只搬字节。它不判定批准是否有效、不执行恢复、不授予任何权限：读出的条目今天
// 没有任何消费方。
class ExtensionStagingRestoreAuditSecureStore
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

    virtual ~ExtensionStagingRestoreAuditSecureStore() = default;
    virtual ReadState readFresh(QByteArray *value, QString *errorCode) = 0;
    virtual WriteOutcome write(const QByteArray &value, QString *errorCode) = 0;
};

enum class ExtensionStagingRestoreAuditStoreState {
    // 授权与载荷都确实不存在。只有这个状态代表"从未记录过任何决定"。
    Empty,
    // 载荷已认证。零条目的 Ready 是合法的"已审计、尚无记录"状态。
    Ready,
    // 两半互相矛盾、载荷无法认证，或授权本身不合法。
    Invalid,
    // 后端被锁定或不可读，当前内容未知。
    Unavailable,
    // 写入结果无法确定，必须由人工确认后再继续。
    OutcomeUnknown,
};

struct ExtensionStagingRestoreAuditStoreResult {
    ExtensionStagingRestoreAuditStoreState state =
        ExtensionStagingRestoreAuditStoreState::Invalid;
    QList<ExtensionStagingRestoreAuditEntry> entries;
    qint64 generation = 0;
    QString identity;
    QString errorCode;
};

class ExtensionStagingRestoreAuditLedgerStore
{
public:
    static constexpr qint64 MaxAuthorityBytes = 8 * 1024;
    static constexpr int MaxEntries =
        ExtensionStagingRestoreAuditLedger::MaxEntries;

    ExtensionStagingRestoreAuditLedgerStore(
        ExtensionStagingRestoreAuditSecureStore *secureStore,
        QSettings *settings);

    ExtensionStagingRestoreAuditStoreResult load();

    // 集合只能整体替换，并且必须提交调用者读到的代号：并发的两次记录不允许静默覆盖
    // 彼此。`expectedGeneration` 为 0 表示调用者认为尚不存在任何载荷。追加一条审计
    // 记录即"load()、在末尾追加、连同读到的代号 replace()"——完整集合语义保持单一
    // 篡改模型。集合已满（MaxEntries 条）时以独立代号拒绝，绝不驱逐历史。
    // 丢弃一份自相矛盾的审计链，把它重建为"从未记录过"。这不是 `replace` 的特例：
    // `replace` 拒绝在 `Invalid` 之上写入，而那正是恢复要处理的状态。这条路径**只
    // 清空**，永远不接受任何条目：一份自相矛盾的审计链无法被"修复"成它大概曾经持有
    // 的历史，那是伪造审计，而唯一诚实的重建是空集合。
    //
    // 它只在 `load()` 确实返回 `Invalid` 时执行。可读的审计链不得被它触碰：如果它能
    // 作用在健康账本上，它就是一条不经授权就清空审计历史的路径。
    //
    // **顺序是安全性的一部分：先销毁授权密钥，再删除载荷字节。** 两次写入不可能原子
    // 完成，因此必须选一个安全的中间态。先销毁密钥意味着任何残留的载荷字节从此无法
    // 被任何人认证，于是这次清空是不可逆的；反过来先删载荷、密钥仍在，则任何能把
    // 那些字节放回去的人都能让被清空的审计历史复活。两种中间态都仍然是 `Invalid`，
    // 因此都不会被误认为成功。
    bool discard(ExtensionStagingRestoreAuditStoreResult *updated,
                 QString *errorCode);

    bool replace(const QList<ExtensionStagingRestoreAuditEntry> &entries,
                 qint64 expectedGeneration,
                 ExtensionStagingRestoreAuditStoreResult *updated,
                 QString *errorCode);

    static QString recordSettingsKey();

private:
    ExtensionStagingRestoreAuditSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
};

#endif // EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_STORE_H
