#ifndef COMPANION_ACTIVATION_JOURNAL_H
#define COMPANION_ACTIVATION_JOURNAL_H

#include "configuration_apply_receipt.h"

#include <QByteArray>
#include <QSettings>

// 激活日志的密钥授权存储。QSettings 只保存记录字节，真正的授权（MAC 密钥、
// 阶段、单调序号、期望 MAC）保存在平台安全存储中，因此删除或重算 QSettings
// 不能伪造一个"没有事务"的状态。
class CompanionActivationJournalSecureStore
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

    virtual ~CompanionActivationJournalSecureStore() = default;
    virtual ReadState readFresh(QByteArray *value, QString *errorCode) = 0;
    virtual WriteOutcome write(const QByteArray &value, QString *errorCode) = 0;
};

// `*CommitRequested` 在发出对应提交前持久化意图，因此恢复时可以区分
// "提交从未发出"（可确定性回滚）与"提交可能已经生效"（必须人工恢复）。
enum class CompanionActivationStage {
    Prepared,
    FilesApplied,
    GatewayCommitRequested,
    GatewayCommitted,
    ProfileCommitRequested,
    ProfileCommitted,
};

enum class CompanionActivationJournalState {
    Empty,
    Ready,
    Invalid,
    Unavailable,
    OutcomeUnknown,
    RecoveryRequired,
};

struct CompanionActivationRecord {
    QString transactionId;
    QString originalProfileId;
    QString candidateProfileId;
    QString candidateProfileIdentity;
    bool candidateTemporary = false;
    CompanionActivationStage stage = CompanionActivationStage::Prepared;
    ConfigurationApplyReceipt receipt;
    qint64 serial = 0;
    QString identity;
};

struct CompanionActivationJournalResult {
    CompanionActivationJournalState state = CompanionActivationJournalState::Invalid;
    CompanionActivationRecord record;
    QString errorCode;
};

class CompanionActivationJournal
{
public:
    CompanionActivationJournal(CompanionActivationJournalSecureStore *secureStore,
                               QSettings *settings);

    // 解析当前状态并完成 reserved 阶段的确定性恢复。因此它可能写入授权。
    CompanionActivationJournalResult load();
    bool create(const CompanionActivationRecord &record, QString *errorCode = nullptr);
    bool advance(const QString &expectedIdentity,
                 CompanionActivationStage nextStage,
                 const ConfigurationApplyReceipt &receipt,
                 CompanionActivationRecord *updated = nullptr,
                 QString *errorCode = nullptr);
    bool clear(const QString &expectedIdentity, QString *errorCode = nullptr);

    static QString identityFor(const CompanionActivationRecord &record);

private:
    CompanionActivationJournalSecureStore *m_secureStore = nullptr;
    QSettings *m_settings = nullptr;
};

#endif // COMPANION_ACTIVATION_JOURNAL_H
