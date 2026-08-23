#ifndef COMPANION_ACTIVATION_JOURNAL_H
#define COMPANION_ACTIVATION_JOURNAL_H

#include "configuration_apply_receipt.h"

#include <QSettings>

enum class CompanionActivationStage {
    Prepared,
    FilesApplied,
    GatewayCommitted,
    ProfileCommitted,
};

enum class CompanionActivationJournalState {
    Empty,
    Ready,
    Invalid,
    Unavailable,
};

struct CompanionActivationRecord {
    QString transactionId;
    QString originalProfileId;
    QString candidateProfileId;
    QString candidateProfileIdentity;
    CompanionActivationStage stage = CompanionActivationStage::Prepared;
    ConfigurationApplyReceipt receipt;
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
    explicit CompanionActivationJournal(QSettings *settings);

    CompanionActivationJournalResult load() const;
    bool create(const CompanionActivationRecord &record, QString *errorCode = nullptr);
    bool advance(const QString &expectedIdentity,
                 CompanionActivationStage nextStage,
                 const ConfigurationApplyReceipt &receipt,
                 CompanionActivationRecord *updated = nullptr,
                 QString *errorCode = nullptr);
    bool clear(const QString &expectedIdentity, QString *errorCode = nullptr);

    static QString identityFor(const CompanionActivationRecord &record);

private:
    static bool validate(CompanionActivationRecord *record, QString *errorCode);
    static QByteArray serialize(const CompanionActivationRecord &record);
    static bool deserialize(const QByteArray &bytes,
                            CompanionActivationRecord *record,
                            QString *errorCode);

    QSettings *m_settings = nullptr;
};

#endif // COMPANION_ACTIVATION_JOURNAL_H
