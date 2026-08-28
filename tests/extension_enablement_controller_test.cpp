#include "extension_enablement_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

class FakeSecureStore final : public ExtensionEnablementLedgerSecureStore
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
    const QString skillDocument = skills + QStringLiteral("/fixture/SKILL.md");
    if (!writeFile(skills + QStringLiteral("/fixture/aegisy-skill.json"),
                   QJsonDocument(manifest).toJson(QJsonDocument::Compact))
            || !writeFile(skillDocument, QByteArrayLiteral("# Fixture\n"))) return 1;
    ExtensionInventoryInputs inputs;
    inputs.codexExecutable = root.filePath(QStringLiteral("missing-codex"));
    inputs.sourceEnvironment = QProcessEnvironment::systemEnvironment();
    inputs.skillsRoot = skills;
    inputs.mcpConfigurationPath = root.filePath(QStringLiteral("missing.json"));
    // 宿主能力证据必须存在，否则这条技能请求的 `filesystem-read` 落在授权集合之外，
    // 兼容性会确定为 Incompatible，授予也就永远走不到需要验证的那些门。
    inputs.host.grantedCapabilities =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();

    QSettings settings(root.filePath(QStringLiteral("grants.ini")),
                       QSettings::IniFormat);
    FakeSecureStore secure;
    ExtensionEnablementLedgerStore store(&secure, &settings);

    // 尚未复核时：授权集合为空，判定与记录一一对应，且每条判定都是"未授权"。
    const ExtensionEnablementSnapshot initial =
        ExtensionEnablementController::inspect(inputs, &store);
    if (!expect(initial.ledgerState == ExtensionEnablementLedgerStoreState::Empty
                    && initial.grants.isEmpty()
                    && initial.inventory.records.size() == 1
                    && initial.decisions.size() == initial.inventory.records.size()
                    && !initial.decisions.first().enabled
                    && initial.decisions.first().evidence
                        == ExtensionEnablementEvidence::NotGranted,
                "the initial enablement snapshot was not empty and ungranted")) {
        return 1;
    }
    if (!expect(!initial.inventory.records.first().effectiveEnabled,
                "inspection wrote effective enablement onto a record")) return 1;

    const ExtensionRegistryRecord discovered = initial.inventory.records.first();
    ExtensionEnablementRequest enable;
    enable.action = ExtensionEnablementAction::Enable;
    enable.kind = discovered.kind;
    enable.id = discovered.id;
    enable.reviewedSourceIdentity = discovered.sourceIdentity;
    enable.reviewedContentIdentity = discovered.contentIdentity;

    // 未复核时授予必须被拒绝，且不得留下任何已认证的授权等待复核出现。
    const ExtensionEnablementOperationResult unreviewed =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(!unreviewed.committed
                    && unreviewed.errorCode
                        == QStringLiteral("extension-enablement-trust-missing")
                    && !secure.present,
                "an unreviewed record was granted enablement")) return 1;

    // 加入人工复核记录，使记录成为 Verified。复核证据由复核侧提供，启用侧只消费它。
    ExtensionReviewPin pin;
    pin.kind = discovered.kind;
    pin.id = discovered.id;
    pin.sourceIdentity = discovered.sourceIdentity;
    pin.contentIdentity = discovered.contentIdentity;
    inputs.reviewPins = {pin};
    const ExtensionEnablementSnapshot reviewed =
        ExtensionEnablementController::inspect(inputs, &store);
    if (!expect(reviewed.inventory.records.first().trust
                    == ExtensionTrustState::Verified
                    && reviewed.inventory.records.first().compatibility
                        == ExtensionCompatibilityState::Compatible
                    && !reviewed.decisions.first().enabled,
                "a reviewed record was enabled without a grant")) return 1;

    // 渲染与授予之间内容变化：必须失败，且不得写入任何授权。
    if (!writeFile(skillDocument, QByteArrayLiteral("# Changed\n"))) return 1;
    const ExtensionEnablementOperationResult drifted =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(!drifted.committed
                    && drifted.errorCode
                        == QStringLiteral("extension-enablement-content-drift")
                    && !secure.present,
                "content drift after presentation wrote a grant")) return 1;
    if (!writeFile(skillDocument, QByteArrayLiteral("# Fixture\n"))) return 1;

    const ExtensionEnablementOperationResult granted =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(granted.committed && granted.changed
                    && granted.snapshot.ledgerState
                        == ExtensionEnablementLedgerStoreState::Ready
                    && granted.snapshot.grants.size() == 1
                    && granted.snapshot.decisions.size() == 1
                    && granted.snapshot.decisions.first().enabled
                    && granted.snapshot.decisions.first().evidence
                        == ExtensionEnablementEvidence::GrantMatched,
                "granting did not refresh the decision projection")) return 1;
    // 这是本层最关键的边界：授权已提交、判定为"应启用"，但记录本身仍未启用。在权限、
    // 审批、沙箱与恢复门禁完成之前，运行扩展内容的权限必须保持关闭。
    if (!expect(!granted.snapshot.inventory.records.first().effectiveEnabled,
                "a committed grant opened effective enablement on the record")) {
        return 1;
    }

    // 重复授予同一内容不推进代号。
    const ExtensionEnablementOperationResult repeated =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(repeated.committed && !repeated.changed
                    && repeated.snapshot.generation == granted.snapshot.generation,
                "re-granting identical content advanced the ledger")) return 1;

    // 复核被撤销后授权仍在账本里，但判定不再启用：两道门是独立的。
    inputs.reviewPins.clear();
    const ExtensionEnablementSnapshot unreviewedAgain =
        ExtensionEnablementController::inspect(inputs, &store);
    if (!expect(unreviewedAgain.grants.size() == 1
                    && !unreviewedAgain.decisions.first().enabled
                    && unreviewedAgain.decisions.first().evidence
                        == ExtensionEnablementEvidence::TrustMissing,
                "a grant survived review revocation as an enablement")) return 1;
    inputs.reviewPins = {pin};

    ExtensionEnablementRequest disable = enable;
    disable.action = ExtensionEnablementAction::Disable;
    disable.reviewedSourceIdentity.clear();
    disable.reviewedContentIdentity.clear();

    // 内容漂移之后仍然必须能停用，否则被篡改的扩展永远无法撤销其授权。
    if (!writeFile(skillDocument, QByteArrayLiteral("# Changed\n"))) return 1;
    const ExtensionEnablementOperationResult revokedAfterDrift =
        ExtensionEnablementController::apply(inputs, disable, &store);
    if (!expect(revokedAfterDrift.committed && revokedAfterDrift.changed
                    && revokedAfterDrift.snapshot.grants.isEmpty(),
                "content drift blocked revocation")) return 1;
    if (!writeFile(skillDocument, QByteArrayLiteral("# Fixture\n"))) return 1;

    // 本来就没有授权时停用不推进代号。
    const ExtensionEnablementOperationResult noOp =
        ExtensionEnablementController::apply(inputs, disable, &store);
    if (!expect(noOp.committed && !noOp.changed
                    && noOp.snapshot.generation
                        == revokedAfterDrift.snapshot.generation,
                "revoking an absent grant advanced the ledger")) return 1;

    // 来源整体消失之后同样必须能停用。
    const ExtensionEnablementOperationResult regranted =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(regranted.committed && regranted.snapshot.grants.size() == 1,
                "the grant was not re-created for the stale-grant test")) return 1;
    if (!QDir(skills + QStringLiteral("/fixture")).removeRecursively()) return 1;
    const ExtensionEnablementOperationResult staleRevoked =
        ExtensionEnablementController::apply(inputs, disable, &store);
    if (!expect(staleRevoked.committed && staleRevoked.changed
                    && staleRevoked.snapshot.grants.isEmpty(),
                "a stale grant was not revocable")) return 1;

    // 授权读不出来时不能规划，也不能显示成"没有被授权过"。
    secure.invalid = true;
    const ExtensionEnablementSnapshot invalid =
        ExtensionEnablementController::inspect(inputs, &store);
    if (!expect(invalid.ledgerState == ExtensionEnablementLedgerStoreState::Invalid
                    && invalid.grants.isEmpty(),
                "invalid grant authority was downgraded to empty")) return 1;
    // 控制器在非 Ready/Empty 时丢弃授权是纵深防御，因为存储本身也承诺这一点：这里
    // 直接钉住存储侧的约定，否则那道防御将无法被观察，将来放松存储契约也不会被发现。
    if (!expect(store.load().grants.isEmpty(),
                "the store returned grants alongside a non-ready state")) return 1;
    const ExtensionEnablementOperationResult blocked =
        ExtensionEnablementController::apply(inputs, enable, &store);
    if (!expect(!blocked.committed
                    && blocked.errorCode == QStringLiteral(
                        "extension-enablement-store-authority-backend-invalid"),
                "invalid grant authority still permitted a write")) return 1;

    // 后端不可用时也不能规划：当前授权未知，不是"没有授权"。
    secure.invalid = false;
    secure.available = false;
    const ExtensionEnablementSnapshot unavailable =
        ExtensionEnablementController::inspect(inputs, &store);
    if (!expect(unavailable.ledgerState
                    == ExtensionEnablementLedgerStoreState::Unavailable
                    && unavailable.grants.isEmpty(),
                "a locked backend was downgraded to empty")) return 1;
    if (!expect(!ExtensionEnablementController::apply(
                    inputs, disable, &store).committed,
                "a locked backend still permitted a revocation")) return 1;

    // 没有存储时既不崩溃也不授权。
    const ExtensionEnablementSnapshot storeless =
        ExtensionEnablementController::inspect(inputs, nullptr);
    if (!expect(storeless.ledgerState
                    == ExtensionEnablementLedgerStoreState::Unavailable
                    && storeless.grants.isEmpty(),
                "a missing store was not reported as unavailable")) return 1;
    return expect(!ExtensionEnablementController::apply(
                      inputs, enable, nullptr).committed,
                  "a missing store still permitted a grant") ? 0 : 1;
}
