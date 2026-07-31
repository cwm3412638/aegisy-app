#ifndef UPDATE_SIGNING_KEY_RING_H
#define UPDATE_SIGNING_KEY_RING_H

#include <QByteArray>
#include <QSharedPointer>
#include <QString>

namespace UpdateSigningKeyRing {

struct AuthorityResult;
struct ArtifactSignatureResult;
class Verifier;

class TrustAnchorAuthority
{
public:
    struct Data;

    bool isValid() const;
    QString keyId() const;
    QString anchorIdentity() const;

private:
    QSharedPointer<const Data> m_data;

    friend class Verifier;
    friend TrustAnchorAuthority embeddedTrustAnchor();
#ifdef AEGISY_UPDATE_SIGNING_KEY_RING_TESTING
    friend TrustAnchorAuthority testingTrustAnchor(
        const QString &, const QByteArray &, QString *);
#endif
};

class Authority
{
public:
    struct Data;

    bool isValid() const;
    quint64 generation() const;
    QString ringIdentity() const;
    QString trustAnchorIdentity() const;
    QString authorityIdentity() const;

private:
    QSharedPointer<const Data> m_data;

    friend class Verifier;
    friend AuthorityResult verifyBootstrap(
        const QByteArray &, const TrustAnchorAuthority &, qint64);
    friend AuthorityResult verifyRotation(
        const QByteArray &, const Authority &, qint64);
    friend ArtifactSignatureResult verifyArtifactSetSignature(
        const Authority &, const QString &, quint64, quint64, bool,
        const QByteArray &, const QString &);
};

struct AuthorityResult
{
    bool ok = false;
    bool idempotent = false;
    QString errorCode;
    Authority authority;
};

struct ArtifactSignatureResult
{
    bool ok = false;
    QString errorCode;
    QString signerKeyId;
    QString signerKeyIdentity;
    QString ringIdentity;
    quint64 ringGeneration = 0;
    QString trustAnchorIdentity;
    QString ringAuthorityIdentity;
};

TrustAnchorAuthority embeddedTrustAnchor();
AuthorityResult verifyBootstrap(const QByteArray &signedRingJson,
                                const TrustAnchorAuthority &trustAnchor,
                                qint64 nowMs);
AuthorityResult verifyRotation(const QByteArray &signedRingJson,
                               const Authority &previous,
                               qint64 nowMs);
ArtifactSignatureResult verifyArtifactSetSignature(
    const Authority &authority,
    const QString &signerKeyId,
    quint64 signedAtMs,
    quint64 nowMs,
    bool requireCurrentlyActive,
    const QByteArray &payload,
    const QString &signatureBase64);

#ifdef AEGISY_UPDATE_SIGNING_KEY_RING_TESTING
TrustAnchorAuthority testingTrustAnchor(const QString &keyId,
                                        const QByteArray &publicKeyBase64,
                                        QString *errorCode = nullptr);
#endif

} // namespace UpdateSigningKeyRing

#endif // UPDATE_SIGNING_KEY_RING_H
