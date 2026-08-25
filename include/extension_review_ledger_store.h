#ifndef EXTENSION_REVIEW_LEDGER_STORE_H
#define EXTENSION_REVIEW_LEDGER_STORE_H

#include "extension_review_ledger.h"

class QSettings;

// 复核记录的持久化被拆成两半，和激活日志一致：授权（HMAC 密钥、已提交代号与身份）
// 放在平台安全存储里，体积较大的载荷字节放在 QSettings 里。两半必须互相印证，
// 因此删除任意一半都不能退化成"从未复核过"。
class ExtensionReviewLedgerSecureStore
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

    virtual ~ExtensionReviewLedgerSecureStore() = default;
    virtual ReadState readFresh(QByteArray *value, QString *errorCode) = 0;
    virtual WriteOutcome write(const QByteArray &value, QString *errorCode) = 0;
};

enum class ExtensionReviewLedgerStoreState {
    // 授权与载荷都确实不存在。只有这个状态代表"从未复核过"。
    Empty,
    Ready,
    // 两半互相矛盾、载荷无法认证，或授权本身不合法。
    Invalid,
    // 后端被锁定或不可读，当前内容未知。
    Unavailable,
    // 写入结果无法确定，必须由人工确认后再继续。
    OutcomeUnknown,
};

struct ExtensionReviewLedgerStoreResult {
    ExtensionReviewLedgerStoreState state =
        ExtensionReviewLedgerStoreState::Invalid;
    QList<ExtensionReviewPin> pins;
    qint64 generation = 0;
    QString identity;
    QString errorCode;
};

class ExtensionReviewLedgerStore
{
public:
    static constexpr qint64 MaxAuthorityBytes = 8 * 1024;

    ExtensionReviewLedgerStore(ExtensionReviewLedgerSecureStore *secureStore,
                               QSettings *settings);

    ExtensionReviewLedgerStoreResult load();

    // 复核集合只能整体替换，并且必须提交调用者读到的代号：并发的两次复核不允许
    // 静默覆盖彼此。`expectedGeneration` 为 0 表示调用者认为尚不存在任何载荷。
    bool replace(const QList<ExtensionReviewPin> &pins,
                 qint64 expectedGeneration,
                 ExtensionReviewLedgerStoreResult *updated,
                 QString *errorCode);

    static QString recordSettingsKey();

private:
    ExtensionReviewLedgerSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
};

#endif // EXTENSION_REVIEW_LEDGER_STORE_H
