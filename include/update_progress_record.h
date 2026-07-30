#pragma once

#include <QString>

namespace UpdateProgressRecord {

enum class Phase {
    CandidateEvaluated,
    DownloadStarted,
    DownloadVerified,
    InstallStarted,
    InstallationObserved,
};

struct Record
{
    quint64 releaseSequence = 0;
    QString artifactSetIdentity;
    Phase phase = Phase::CandidateEvaluated;
    quint64 revision = 0;
    quint64 updatedAtMs = 0;
    QString previousRecordIdentity;
    QString recordIdentity;
};

struct Observation
{
    // A progress record is continuity evidence only and never update authority.
    bool ok = false;
    bool missing = false;
    bool continuityVerified = false;
    bool downloadAuthorized = false;
    bool installAuthorized = false;
    bool rollbackAuthorized = false;
    QString errorCode;
    Record record;
};

struct AdvanceResult
{
    // These flags stay false even after a verified atomic commit.
    bool ok = false;
    bool idempotent = false;
    bool postCommitVerified = false;
    bool downloadAuthorized = false;
    bool installAuthorized = false;
    bool rollbackAuthorized = false;
    QString errorCode;
    Record record;
};

class Store
{
public:
    explicit Store(const QString &stateRoot);

    Observation load(quint64 minimumReleaseSequence = 0,
                     quint64 minimumRevision = 0,
                     const QString &expectedRecordIdentity = QString()) const;

    AdvanceResult advance(quint64 releaseSequence,
                          const QString &artifactSetIdentity,
                          Phase phase,
                          qint64 nowMs,
                          const QString &expectedCurrentIdentity = QString()) const;

private:
    QString m_stateRoot;
};

QString phaseName(Phase phase);

} // namespace UpdateProgressRecord
