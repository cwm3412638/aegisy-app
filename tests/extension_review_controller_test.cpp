#include "extension_review_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

class FakeSecureStore final : public ExtensionReviewLedgerSecureStore
{
public:
    ReadState readFresh(QByteArray *value, QString *) override
    {
        if (!value) return ReadState::Invalid;
        if (!available) return ReadState::Unavailable;
        if (!present) {
            value->clear();
            return ReadState::Missing;
        }
        *value = bytes;
        return invalid ? ReadState::Invalid : ReadState::Found;
    }

    WriteOutcome write(const QByteArray &value, QString *) override
    {
        if (!available) return WriteOutcome::OutcomeUnknown;
        bytes = value;
        present = true;
        invalid = false;
        return WriteOutcome::Committed;
    }

    bool available = true;
    bool present = false;
    bool invalid = false;
    QByteArray bytes;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const QString skills = root.filePath(QStringLiteral("skills"));
    const QJsonObject manifest{
        {QStringLiteral("id"), QStringLiteral("fixture.skill")},
        {QStringLiteral("name"), QStringLiteral("fixture skill")},
        {QStringLiteral("version"), QStringLiteral("1.0.0")},
        {QStringLiteral("executor"), QStringLiteral("instruction")},
        {QStringLiteral("enabled"), false},
        {QStringLiteral("trusted"), false},
        {QStringLiteral("builtin"), false},
        {QStringLiteral("permissions"), QJsonArray{
             QStringLiteral("files-read")}},
    };
    if (!writeFile(skills + QStringLiteral("/fixture/aegisy-skill.json"),
                   QJsonDocument(manifest).toJson(QJsonDocument::Compact))
            || !writeFile(skills + QStringLiteral("/fixture/SKILL.md"),
                          QByteArrayLiteral("# Fixture\n"))) return 1;
    const QString codex = root.filePath(QStringLiteral("missing-codex"));
    ExtensionInventoryInputs inputs;
    inputs.codexExecutable = codex;
    inputs.sourceEnvironment = QProcessEnvironment::systemEnvironment();
    inputs.skillsRoot = skills;
    inputs.mcpConfigurationPath = root.filePath(QStringLiteral("missing.json"));

    QSettings settings(root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    FakeSecureStore secure;
    ExtensionReviewLedgerStore store(&secure, &settings);
    ExtensionReviewSnapshot initial = ExtensionReviewController::inspect(inputs, &store);
    if (!expect(initial.ledgerState == ExtensionReviewLedgerStoreState::Empty
                    && initial.inventory.records.size() == 1
                    && initial.inventory.records.first().trust
                        == ExtensionTrustState::Unverified,
                "initial review snapshot was not empty and untrusted")) return 1;

    const ExtensionRegistryRecord record = initial.inventory.records.first();
    ExtensionReviewRequest approve;
    approve.action = ExtensionReviewAction::Approve;
    approve.kind = record.kind;
    approve.id = record.id;
    approve.reviewedSourceIdentity = record.sourceIdentity;
    approve.reviewedContentIdentity = record.contentIdentity;
    const QString skillDocument = skills + QStringLiteral("/fixture/SKILL.md");
    if (!writeFile(skillDocument, QByteArrayLiteral("# Changed\n"))) return 1;
    const ExtensionReviewOperationResult drifted =
        ExtensionReviewController::apply(inputs, approve, &store);
    if (!expect(!drifted.committed
                    && drifted.errorCode
                        == QStringLiteral("extension-review-content-drift")
                    && !secure.present,
                "content drift after presentation wrote review evidence")) return 1;
    if (!writeFile(skillDocument, QByteArrayLiteral("# Fixture\n"))) return 1;
    const ExtensionReviewOperationResult approved =
        ExtensionReviewController::apply(inputs, approve, &store);
    if (!expect(approved.committed && approved.changed
                    && approved.snapshot.ledgerState == ExtensionReviewLedgerStoreState::Ready
                    && approved.snapshot.inventory.records.first().trust
                        == ExtensionTrustState::Verified
                    && !approved.snapshot.inventory.records.first().effectiveEnabled,
                "approved review did not refresh trust without enablement")) return 1;

    ExtensionReviewRequest revoke = approve;
    revoke.action = ExtensionReviewAction::Revoke;
    revoke.reviewedSourceIdentity.clear();
    revoke.reviewedContentIdentity.clear();
    const ExtensionReviewOperationResult revoked =
        ExtensionReviewController::apply(inputs, revoke, &store);
    if (!expect(revoked.committed && revoked.changed
                    && revoked.snapshot.pins.isEmpty()
                    && revoked.snapshot.inventory.records.first().trust
                        == ExtensionTrustState::Unverified,
                "revocation did not remove trust evidence")) return 1;

    const ExtensionReviewOperationResult noOp =
        ExtensionReviewController::apply(inputs, revoke, &store);
    if (!expect(noOp.committed && !noOp.changed
                    && noOp.snapshot.generation == revoked.snapshot.generation,
                "revoking an absent pin advanced the ledger")) return 1;

    // A stale pin remains revocable even after the source disappears.
    const ExtensionReviewOperationResult reapproved =
        ExtensionReviewController::apply(inputs, approve, &store);
    if (!expect(reapproved.committed && reapproved.snapshot.pins.size() == 1,
                "review was not re-created for stale-pin test")) return 1;
    if (!QDir(skills + QStringLiteral("/fixture")).removeRecursively()) return 1;
    const ExtensionReviewOperationResult staleRevoked =
        ExtensionReviewController::apply(inputs, revoke, &store);
    if (!expect(staleRevoked.committed && staleRevoked.changed
                    && staleRevoked.snapshot.pins.isEmpty(),
                "stale review pin was not revocable")) return 1;

    secure.invalid = true;
    const ExtensionReviewSnapshot invalid =
        ExtensionReviewController::inspect(inputs, &store);
    if (!expect(invalid.ledgerState == ExtensionReviewLedgerStoreState::Invalid,
                "invalid review authority was downgraded to empty")) return 1;
    const ExtensionReviewOperationResult blocked =
        ExtensionReviewController::apply(inputs, approve, &store);
    return expect(!blocked.committed
                      && blocked.errorCode == QStringLiteral(
                          "extension-review-store-authority-backend-invalid"),
                  "invalid review authority still permitted a write") ? 0 : 1;
}
