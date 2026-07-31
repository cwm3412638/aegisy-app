#pragma once

#include "update_signing_key_ring.h"

#include <QByteArray>
#include <QString>

namespace UpdateSigningKeyRingCache {

enum class State {
    Empty,
    Authoritative,
    CachedButNotAuthoritative,
    Invalid,
    Unavailable,
};

struct Observation
{
    State state = State::Unavailable;
    bool present = false;
    bool integrityVerified = false;
    QString errorCode;
    QString verificationErrorCode;
    QString trustAnchorIdentity;
    quint64 generation = 0;
    QString latestEntryIdentity;
    QString chainIdentity;
    QString cacheIdentity;
    QString previousCacheIdentity;
    QString ringIdentity;
    QString ringAuthorityIdentity;
    UpdateSigningKeyRing::Authority authority;

    // This cache is local integrity evidence, never update authority.
    bool updateAuthorized = false;
    bool networkAuthorized = false;
    bool downloadAuthorized = false;
    bool installAuthorized = false;
    bool rollbackAuthorized = false;
    bool resumeAuthorized = false;
    bool executionAuthorized = false;
    bool antiRollbackProtected = false;
    bool antiDeletionProtected = false;
    bool trustedTimeAvailable = false;
    bool expiredSignerRecoveryAvailable = false;
};

struct CommitResult
{
    bool committed = false;
    bool idempotent = false;
    bool postCommitVerified = false;
    QString errorCode;
    Observation observation;

    bool updateAuthorized = false;
    bool networkAuthorized = false;
    bool downloadAuthorized = false;
    bool installAuthorized = false;
    bool rollbackAuthorized = false;
    bool resumeAuthorized = false;
    bool executionAuthorized = false;
    bool antiRollbackProtected = false;
    bool antiDeletionProtected = false;
    bool trustedTimeAvailable = false;
    bool expiredSignerRecoveryAvailable = false;
};

class Store
{
public:
    explicit Store(const QString &stateRoot);

    Observation
    load(const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
         qint64 nowMs) const;

    CommitResult
    bootstrap(const QByteArray &generationOneEnvelope,
              const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
              qint64 nowMs) const;

    CommitResult
    append(const QByteArray &nextEnvelope,
           const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
           qint64 nowMs, const QString &expectedCurrentCacheIdentity) const;

private:
    QString m_stateRoot;
    QByteArray m_stateRootIdentity;
};

QString stateName(State state);

} // namespace UpdateSigningKeyRingCache
