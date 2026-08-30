#include "extension_lifecycle_controller.h"

#include "extension_enablement_workflow.h"
#include "extension_review_workflow.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

template <typename Base>
class FakeSecureStore final : public Base
{
public:
    typename Base::ReadState readFresh(QByteArray *value, QString *) override
    {
        if (!value) return Base::ReadState::Invalid;
        if (!available) return Base::ReadState::Unavailable;
        if (!present) {
            value->clear();
            return Base::ReadState::Missing;
        }
        *value = bytes;
        return invalid ? Base::ReadState::Invalid : Base::ReadState::Found;
    }

    typename Base::WriteOutcome write(const QByteArray &value, QString *) override
    {
        if (refuseWrites) return Base::WriteOutcome::DefiniteFailure;
        if (!available) return Base::WriteOutcome::OutcomeUnknown;
        // 谎报提交:后端说写成功了,但字节没有变。这让"重新读到的字节才是依据"这条
        // 规则成为可观察的——否则只要 replace 返回真就总能推出授权已收回。
        if (acknowledgeWithoutPersisting) return Base::WriteOutcome::Committed;
        bytes = value;
        present = true;
        invalid = false;
        return Base::WriteOutcome::Committed;
    }

    bool available = true;
    bool present = false;
    bool invalid = false;
    // 只拒绝写入而仍然可读:用于制造"授权已收回、复核记录写不回去"的部分完成。
    bool refuseWrites = false;
    // 报告提交但不真的持久化。
    bool acknowledgeWithoutPersisting = false;
    QByteArray bytes;
};

using FakeReviewSecureStore = FakeSecureStore<ExtensionReviewLedgerSecureStore>;
using FakeGrantSecureStore =
    FakeSecureStore<ExtensionEnablementLedgerSecureStore>;

QString contentIdentityOf(const QByteArray &seed)
{
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            seed, QCryptographicHash::Sha256).toHex());
}

struct Fixture {
    QTemporaryDir root;
    QString skills;
    QString skillDocument;
    ExtensionInventoryInputs inputs;

    bool valid() const { return root.isValid(); }
};

bool buildFixture(Fixture *fixture)
{
    if (!fixture->root.isValid()) return false;
    fixture->skills = fixture->root.filePath(QStringLiteral("skills"));
    fixture->skillDocument =
        fixture->skills + QStringLiteral("/fixture/SKILL.md");
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
    if (!writeFile(fixture->skills + QStringLiteral("/fixture/aegisy-skill.json"),
                   QJsonDocument(manifest).toJson(QJsonDocument::Compact))) {
        return false;
    }
    if (!writeFile(fixture->skillDocument, QByteArrayLiteral("# Fixture\n"))) {
        return false;
    }
    fixture->inputs.codexExecutable =
        fixture->root.filePath(QStringLiteral("missing-codex"));
    fixture->inputs.sourceEnvironment = QProcessEnvironment::systemEnvironment();
    fixture->inputs.skillsRoot = fixture->skills;
    fixture->inputs.mcpConfigurationPath =
        fixture->root.filePath(QStringLiteral("missing.json"));
    // 宿主能力证据必须存在,否则这条技能请求的 `filesystem-read` 落在授权集合之外,
    // 兼容性会确定为 Incompatible,授予也就永远走不到需要验证的那些门。
    fixture->inputs.host.grantedCapabilities =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();
    return true;
}

ExtensionUpdateEvidence passingEvidence()
{
    ExtensionUpdateEvidence evidence;
    evidence.signatureValid = true;
    evidence.manifestValid = true;
    evidence.compatible = true;
    evidence.dependenciesSatisfied = true;
    evidence.healthy = true;
    return evidence;
}

// 让固定件同时持有一份复核记录与一份启用授权,这样"移除必须收回两者"才是可观察的:
// 一个本来就没有授权的目标,移除后没有授权说明不了任何事情。
bool grantAndReview(const Fixture &fixture,
                    ExtensionReviewLedgerStore *reviewStore,
                    ExtensionEnablementLedgerStore *grantStore,
                    ExtensionRegistryRecord *record)
{
    const ExtensionLifecycleSnapshot snapshot =
        ExtensionLifecycleController::inspect(fixture.inputs, reviewStore,
                                             grantStore);
    if (snapshot.inventory.records.size() != 1) return false;
    *record = snapshot.inventory.records.first();

    ExtensionReviewRequest approve;
    approve.action = ExtensionReviewAction::Approve;
    approve.kind = record->kind;
    approve.id = record->id;
    approve.reviewedSourceIdentity = record->sourceIdentity;
    approve.reviewedContentIdentity = record->contentIdentity;
    const ExtensionReviewPlan reviewPlan = ExtensionReviewWorkflow::plan(
        approve, snapshot.inventory.records, reviewStore->load());
    if (reviewPlan.state != ExtensionReviewPlanState::Ready) return false;
    ExtensionReviewLedgerStoreResult reviewUpdated;
    QString errorCode;
    if (!reviewStore->replace(reviewPlan.pins, reviewPlan.expectedGeneration,
                              &reviewUpdated, &errorCode)) {
        return false;
    }

    // 授权规划要求记录已经是 Verified + Compatible,因此必须在复核提交之后用带上复核
    // 记录的清单重新规划。
    const ExtensionLifecycleSnapshot reviewed =
        ExtensionLifecycleController::inspect(fixture.inputs, reviewStore,
                                             grantStore);
    if (reviewed.inventory.records.size() != 1) return false;
    ExtensionEnablementRequest enable;
    enable.action = ExtensionEnablementAction::Enable;
    enable.kind = record->kind;
    enable.id = record->id;
    enable.reviewedSourceIdentity = record->sourceIdentity;
    enable.reviewedContentIdentity = record->contentIdentity;
    const ExtensionEnablementPlan grantPlan = ExtensionEnablementWorkflow::plan(
        enable, reviewed.inventory.records, grantStore->load());
    if (grantPlan.state != ExtensionEnablementPlanState::Ready) return false;
    ExtensionEnablementLedgerStoreResult grantUpdated;
    return grantStore->replace(grantPlan.grants, grantPlan.expectedGeneration,
                               &grantUpdated, &errorCode);
}

// 更新校验失败时当前版本保持不变,候选不执行,两份账本一个字节都不动。
void updateRejectionTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the update fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }
    const QByteArray reviewBefore = reviewSecure.bytes;
    const QByteArray grantBefore = grantSecure.bytes;

    ExtensionUpdateCandidate candidate;
    candidate.kind = record.kind;
    candidate.id = record.id;
    candidate.version = QStringLiteral("2.0.0");
    candidate.sourceIdentity = record.sourceIdentity;
    candidate.contentIdentity = contentIdentityOf("candidate-content");

    const QList<QPair<QString, ExtensionUpdateEvidence>> failing{
        {QStringLiteral("extension-update-signature-invalid"), [] {
             ExtensionUpdateEvidence value = passingEvidence();
             value.signatureValid = false;
             return value;
         }()},
        {QStringLiteral("extension-update-manifest-invalid"), [] {
             ExtensionUpdateEvidence value = passingEvidence();
             value.manifestValid = false;
             return value;
         }()},
        {QStringLiteral("extension-update-incompatible"), [] {
             ExtensionUpdateEvidence value = passingEvidence();
             value.compatible = false;
             return value;
         }()},
        {QStringLiteral("extension-update-dependency-unsatisfied"), [] {
             ExtensionUpdateEvidence value = passingEvidence();
             value.dependenciesSatisfied = false;
             return value;
         }()},
        {QStringLiteral("extension-update-health-failed"), [] {
             ExtensionUpdateEvidence value = passingEvidence();
             value.healthy = false;
             return value;
         }()},
    };
    for (const auto &entry : failing) {
        const ExtensionLifecycleResult result =
            ExtensionLifecycleController::stageUpdate(
                fixture.inputs, candidate, entry.second, &reviewStore,
                &grantStore);
        expect(result.outcome == ExtensionLifecycleOutcome::Refused,
               "a failed upgrade validation was not refused");
        expect(result.errorCode == entry.first,
               "a failed upgrade validation did not report which check failed");
        // 关键:当前生效的版本保持不变,候选不执行。
        expect(result.activePreserved,
               "a failed upgrade validation disturbed the active version");
        expect(!result.candidateExecutable,
               "a candidate that failed validation was executable");
        // 两份账本一个字节都没动:失败的校验不应留下任何权威痕迹。
        expect(reviewSecure.bytes == reviewBefore
                   && grantSecure.bytes == grantBefore,
               "a failed upgrade validation wrote to a ledger");
    }

    // 目标不存在时不得暂存:更新一个不存在的目标等于预先授权将来出现的内容。
    ExtensionUpdateCandidate absent = candidate;
    absent.id = QStringLiteral("nobody.here");
    expect(ExtensionLifecycleController::stageUpdate(
               fixture.inputs, absent, passingEvidence(), &reviewStore,
               &grantStore).errorCode
               == QStringLiteral("extension-update-target-absent"),
           "an update to an absent target was staged");

    // 内容摘要不变的"更新"不是更新。
    ExtensionUpdateCandidate unchanged = candidate;
    unchanged.contentIdentity = record.contentIdentity;
    expect(ExtensionLifecycleController::stageUpdate(
               fixture.inputs, unchanged, passingEvidence(), &reviewStore,
               &grantStore).errorCode
               == QStringLiteral("extension-update-content-unchanged"),
           "an unchanged content digest was accepted as an update");
}

// 校验通过时候选可被暂存,但仍然未复核、未授权,因此不可执行。
void updateStagingTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the staging fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }
    // 关键:固定件确实持有复核记录与授权。否则"候选不继承它们"这条不变量无从观察——
    // 一个本来就没有权威的目标,更新后没有权威说明不了任何事情。
    const ExtensionLifecycleSnapshot before =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    if (!expect(before.pins.size() == 1 && before.grants.size() == 1,
                "the fixture did not hold the authority the candidate must not inherit")) {
        return;
    }
    const QByteArray reviewBefore = reviewSecure.bytes;
    const QByteArray grantBefore = grantSecure.bytes;

    ExtensionUpdateCandidate candidate;
    candidate.kind = record.kind;
    candidate.id = record.id;
    candidate.version = QStringLiteral("2.0.0");
    candidate.sourceIdentity = record.sourceIdentity;
    candidate.contentIdentity = contentIdentityOf("candidate-content");

    const ExtensionLifecycleResult staged =
        ExtensionLifecycleController::stageUpdate(
            fixture.inputs, candidate, passingEvidence(), &reviewStore,
            &grantStore);
    expect(staged.outcome == ExtensionLifecycleOutcome::StagedUnreviewed,
           "a validated candidate was not staged");
    expect(staged.errorCode.isEmpty(), "a staged candidate carried an error code");
    // 关键:暂存不等于可运行。候选按定义是另一份内容,必须重新复核并重新授权。
    expect(!staged.candidateExecutable,
           "a staged candidate was executable before review");
    expect(!staged.inheritsTrust,
           "a candidate inherited the previous version's review");
    expect(!staged.inheritsGrant,
           "a candidate inherited the previous version's grant");
    expect(staged.activePreserved,
           "staging a candidate disturbed the active version");
    // 关键:暂存不为候选写入任何权威。账本字节必须完全不变。
    expect(reviewSecure.bytes == reviewBefore,
           "staging a candidate wrote a review pin");
    expect(grantSecure.bytes == grantBefore,
           "staging a candidate wrote an enablement grant");
    // 旧记录仍然在账本里,但它绑定旧内容摘要,因此对候选而言是漂移。
    if (expect(staged.snapshot.pins.size() == 1,
               "the previous review pin disappeared")) {
        expect(staged.snapshot.pins.first().contentIdentity
                   != candidate.contentIdentity,
               "the retained review pin described the candidate content");
    }
    if (expect(staged.snapshot.grants.size() == 1,
               "the previous grant disappeared")) {
        expect(staged.snapshot.grants.first().contentIdentity
                   != candidate.contentIdentity,
               "the retained grant described the candidate content");
    }

    // 降级不被禁止,但必须可见。
    ExtensionUpdateCandidate older = candidate;
    older.version = QStringLiteral("0.9.0");
    const ExtensionLifecycleResult downgrade =
        ExtensionLifecycleController::stageUpdate(
            fixture.inputs, older, passingEvidence(), &reviewStore, &grantStore);
    expect(downgrade.outcome == ExtensionLifecycleOutcome::StagedUnreviewed
               && downgrade.downgrade,
           "a downgrade was staged without disclosing that it is a downgrade");
    expect(!staged.downgrade, "an upgrade was reported as a downgrade");
}

// 移除:收回启用授权与复核记录,保留不可变身份。
void removalTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the removal fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }
    // 关键:移除之前确实存在授权与复核记录。
    const ExtensionLifecycleSnapshot before =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    if (!expect(before.grants.size() == 1 && before.pins.size() == 1,
                "the fixture did not hold the authority removal must withdraw")) {
        return;
    }

    const ExtensionLifecycleResult removed = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    expect(removed.outcome == ExtensionLifecycleOutcome::Withdrawn,
           "a complete removal was not reported as withdrawn");
    expect(removed.errorCode.isEmpty(),
           "a complete removal carried an error code");
    // 关键:可执行内容被停用或删除。
    expect(removed.executableContentRemoved,
           "removal did not disable or remove the executable content");
    // 关键:授权被收回。留着授权会让同名内容重新出现时直接继承它。
    expect(removed.grantRevoked && removed.snapshot.grants.isEmpty(),
           "removal left the enablement grant in the ledger");
    expect(removed.reviewRevoked && removed.snapshot.pins.isEmpty(),
           "removal left the review pin in the ledger");
    // 关键:不可变身份被保留,否则"这份内容曾被授权运行过"的历史一并消失。
    expect(!removed.retainedIdentity.isEmpty(),
           "removal discarded the immutable identity metadata");
    expect(removed.retainedIdentity.contains(record.id)
               && removed.retainedIdentity.contains(record.contentIdentity),
           "the retained identity did not name the content that was removed");

    // 目标已经不存在时仍然必须能够收回授权并留下身份:一个消失的目录不应把授权留在账本里。
    ExtensionRegistryRecord second;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &second),
                "the fixture could not be re-granted for the stale case")) {
        return;
    }
    if (!expect(QDir(fixture.skills + QStringLiteral("/fixture"))
                    .removeRecursively(),
                "the fixture directory could not be removed")) {
        return;
    }
    const ExtensionLifecycleResult stale = ExtensionLifecycleController::remove(
        fixture.inputs, second.kind, second.id, &reviewStore, &grantStore);
    expect(stale.outcome == ExtensionLifecycleOutcome::Withdrawn,
           "a stale target could not be removed");
    expect(stale.grantRevoked && stale.reviewRevoked,
           "a stale target's authority was not withdrawn");
    expect(!stale.retainedIdentity.isEmpty(),
           "a stale removal left no identity metadata");

    // 移除一个从未被授权的目标不是错误,但也不改变任何东西。
    const ExtensionLifecycleResult again = ExtensionLifecycleController::remove(
        fixture.inputs, second.kind, second.id, &reviewStore, &grantStore);
    expect(again.outcome == ExtensionLifecycleOutcome::Withdrawn,
           "removing an already-withdrawn target reported a failure");
}

// 部分完成必须可分辨,不能报成成功。
void partialRemovalTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the partial fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }

    // 复核账本拒绝写入,授权账本正常。顺序保证失败停在"没有授权、复核记录尚存"上,
    // 那是注册表双重门禁下的未启用,也就是安全的一侧。
    reviewSecure.refuseWrites = true;
    const ExtensionLifecycleResult partial = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    // 关键:不报成功。一个只收回了授权的移除必须让人看到还有一条复核记录留着。
    expect(partial.outcome == ExtensionLifecycleOutcome::PartiallyWithdrawn,
           "an incomplete removal was reported as complete");
    expect(!partial.errorCode.isEmpty(),
           "an incomplete removal carried no diagnostic");
    // 诊断必须说出是哪一步失败的。一个泛化的"未完成"会让人不知道该重试还是该去查后端,
    // 而这里确切知道原因:复核账本的写入被拒绝了。
    // 诊断必须说出失败发生在哪里。存储自己的后端原因被原样传上来,而不是被一个泛化的
    // "未完成"盖掉:人需要知道是复核账本写不进去,才知道该重试还是该去查后端。
    expect(partial.errorCode.contains(QStringLiteral("review")),
           "an incomplete removal did not report which half failed");
    // 关键:先收回的是授权,因此授权确实不在了。
    expect(partial.grantRevoked,
           "the enablement grant was not withdrawn first");
    expect(!partial.reviewRevoked,
           "a review pin that could not be written was reported as withdrawn");
    // 身份元数据在部分完成时同样保留。
    expect(!partial.retainedIdentity.isEmpty(),
           "an incomplete removal discarded the identity metadata");
    // 重新读取账本确认顺序:授权空了,复核记录还在。
    reviewSecure.refuseWrites = false;
    const ExtensionLifecycleSnapshot after =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    expect(after.grants.isEmpty(),
           "the grant survived an incomplete removal");
    expect(after.pins.size() == 1,
           "the review pin was lost by an incomplete removal");
    // 注册表双重门禁下这条记录仍然不是启用的:安全的一侧。
    if (expect(after.inventory.records.size() == 1,
               "the record disappeared after an incomplete removal")) {
        expect(!after.inventory.records.first().effectiveEnabled,
               "an incompletely removed extension remained effectively enabled");
    }
    // 重试完成剩下的一半。
    const ExtensionLifecycleResult retried = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    expect(retried.outcome == ExtensionLifecycleOutcome::Withdrawn
               && retried.reviewRevoked,
           "retrying an incomplete removal did not finish it");
}

// 顺序本身必须可观察:授权写入失败时,复核记录必须一个字节都没动。
void removalOrderTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the order fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }
    const QByteArray reviewBefore = reviewSecure.bytes;
    if (!expect(!reviewBefore.isEmpty(),
                "the fixture holds no review evidence to protect")) {
        return;
    }

    // 授权账本拒绝写入。正确顺序下这次移除在第一步就停下,复核记录还没被碰过。反过来
    // 先删复核记录的实现会在这里已经抹掉审计证据,而授权仍然留着——正是最坏的中间态。
    grantSecure.refuseWrites = true;
    const ExtensionLifecycleResult blocked = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    grantSecure.refuseWrites = false;
    expect(blocked.outcome != ExtensionLifecycleOutcome::Withdrawn,
           "a removal whose grant write failed was reported as complete");
    expect(!blocked.grantRevoked,
           "a refused grant write was reported as a withdrawal");
    expect(!blocked.reviewRevoked,
           "the review pin was withdrawn before the grant was");
    // 关键:复核账本的字节完全没有变化。
    expect(reviewSecure.bytes == reviewBefore,
           "a failed grant write still cost the review evidence");
    const ExtensionLifecycleSnapshot after =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    expect(after.pins.size() == 1,
           "the review pin was deleted before the grant was withdrawn");
    expect(after.grants.size() == 1,
           "the grant vanished despite the write being refused");
}

// 一次被确认的写入不等于授权真的不在了。结论必须来自重新读到的字节。
void acknowledgedButUnpersistedTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the acknowledgement fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }

    // 后端确认了写入,但字节没有变。载荷落了盘而授权没有跟上,于是账本重新读出来是
    // 不可用的:当前授权集合未知。相信 `replace` 的返回值就会把这次移除报成完成,而那
    // 是最坏的一种谎报——人会以为授权已经收回了,实际上没人知道它在哪个状态。
    grantSecure.acknowledgeWithoutPersisting = true;
    const ExtensionLifecycleResult result = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    grantSecure.acknowledgeWithoutPersisting = false;
    expect(!result.grantRevoked,
           "an acknowledged write was taken as proof the grant was withdrawn");
    expect(result.outcome == ExtensionLifecycleOutcome::PartiallyWithdrawn,
           "a removal whose grant state is unknown was reported as complete");
    expect(!result.errorCode.isEmpty(),
           "an incomplete removal carried no diagnostic");
    // 复核记录不得被继续删除:授权状态未知时继续往下走会留下更坏的中间态。
    expect(!result.reviewRevoked,
           "the review pin was withdrawn while the grant state was unknown");
    // 关键:账本状态确实不可用,证明上面的断言不是空的。声称集合为空会让界面显示
    // "没有授权过",而实际情况是未知。
    const ExtensionLifecycleSnapshot after =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    expect(after.grantState != ExtensionEnablementLedgerStoreState::Ready
               && after.grantState != ExtensionEnablementLedgerStoreState::Empty,
           "an unpersisted authority write left the ledger reporting a clean state");
    expect(after.pins.size() == 1,
           "the review pin was lost while the grant state was unknown");

    // 对称的一侧:授权确实收回了,但复核账本的写入被确认而没有持久化,于是它重新读出来
    // 是不可用的。此时不能声称复核记录已经收回——状态未知不是空集合。
    Fixture second;
    if (!expect(buildFixture(&second),
                "the review-side acknowledgement fixture could not be built")) {
        return;
    }
    QSettings secondReviewSettings(
        second.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings secondGrantSettings(
        second.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore secondReviewSecure;
    FakeGrantSecureStore secondGrantSecure;
    ExtensionReviewLedgerStore secondReviewStore(&secondReviewSecure,
                                                &secondReviewSettings);
    ExtensionEnablementLedgerStore secondGrantStore(&secondGrantSecure,
                                                   &secondGrantSettings);
    ExtensionRegistryRecord secondRecord;
    if (!expect(grantAndReview(second, &secondReviewStore, &secondGrantStore,
                               &secondRecord),
                "the review-side fixture could not be reviewed and granted")) {
        return;
    }
    secondReviewSecure.acknowledgeWithoutPersisting = true;
    const ExtensionLifecycleResult reviewUnknown =
        ExtensionLifecycleController::remove(second.inputs, secondRecord.kind,
                                            secondRecord.id, &secondReviewStore,
                                            &secondGrantStore);
    secondReviewSecure.acknowledgeWithoutPersisting = false;
    // 授权那一半确实完成了,所以这条断言隔离出的正是复核那一半。
    expect(reviewUnknown.grantRevoked,
           "the grant was not withdrawn in the review-side case");
    expect(!reviewUnknown.reviewRevoked,
           "an unusable review ledger was reported as a withdrawal");
    expect(reviewUnknown.outcome == ExtensionLifecycleOutcome::PartiallyWithdrawn,
           "a removal whose review state is unknown was reported as complete");
    expect(!reviewUnknown.errorCode.isEmpty(),
           "a removal with an unknown review state carried no diagnostic");
    const ExtensionLifecycleSnapshot reviewAfter =
        ExtensionLifecycleController::inspect(second.inputs, &secondReviewStore,
                                             &secondGrantStore);
    expect(reviewAfter.reviewState != ExtensionReviewLedgerStoreState::Ready
               && reviewAfter.reviewState
                   != ExtensionReviewLedgerStoreState::Empty,
           "an unpersisted review write left the ledger reporting a clean state");
}

// 账本读不出来时不得声称收回了任何东西。
void unusableLedgerTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the unusable fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }

    // 授权账本不可读:在授权未知的情况下声称已经收回授权是最不该做的事。
    grantSecure.available = false;
    const ExtensionLifecycleResult blocked = ExtensionLifecycleController::remove(
        fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    expect(blocked.outcome == ExtensionLifecycleOutcome::Refused,
           "removal proceeded with an unreadable grant ledger");
    expect(!blocked.grantRevoked && !blocked.reviewRevoked,
           "an unreadable ledger still reported authority as withdrawn");
    expect(!blocked.errorCode.isEmpty(),
           "a refused removal carried no diagnostic");
    grantSecure.available = true;

    // 复核账本不可读同样拦下,而且必须在写入授权之前拦下。
    reviewSecure.invalid = true;
    const QByteArray grantBefore = grantSecure.bytes;
    const ExtensionLifecycleResult reviewBlocked =
        ExtensionLifecycleController::remove(
            fixture.inputs, record.kind, record.id, &reviewStore, &grantStore);
    expect(reviewBlocked.outcome == ExtensionLifecycleOutcome::Refused,
           "removal proceeded with an unusable review ledger");
    expect(grantSecure.bytes == grantBefore,
           "an unusable review ledger still cost the grant");
    reviewSecure.invalid = false;

    // 缺少任一存储时拒绝,而不是当作"没有授权"。
    expect(ExtensionLifecycleController::remove(
               fixture.inputs, record.kind, record.id, nullptr, &grantStore)
               .errorCode
               == QStringLiteral("extension-removal-store-unavailable"),
           "a missing review store was treated as an absent ledger");
    expect(ExtensionLifecycleController::remove(
               fixture.inputs, record.kind, record.id, &reviewStore, nullptr)
               .errorCode
               == QStringLiteral("extension-removal-store-unavailable"),
           "a missing grant store was treated as an absent ledger");

    // 非法 ID 不得进入任何写入路径。
    expect(ExtensionLifecycleController::remove(
               fixture.inputs, record.kind, QStringLiteral("Bad Id"),
               &reviewStore, &grantStore).errorCode
               == QStringLiteral("extension-removal-id-invalid"),
           "a malformed identifier reached the removal path");
}

// 这一层不安装、不下载、不执行,也从不让任何东西变得可执行。
void authorityTests()
{
    Fixture fixture;
    if (!expect(buildFixture(&fixture), "the authority fixture could not be built")) {
        return;
    }
    QSettings reviewSettings(
        fixture.root.filePath(QStringLiteral("reviews.ini")), QSettings::IniFormat);
    QSettings grantSettings(
        fixture.root.filePath(QStringLiteral("grants.ini")), QSettings::IniFormat);
    FakeReviewSecureStore reviewSecure;
    FakeGrantSecureStore grantSecure;
    ExtensionReviewLedgerStore reviewStore(&reviewSecure, &reviewSettings);
    ExtensionEnablementLedgerStore grantStore(&grantSecure, &grantSettings);

    ExtensionRegistryRecord record;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &record),
                "the fixture could not be reviewed and granted")) {
        return;
    }

    ExtensionUpdateCandidate candidate;
    candidate.kind = record.kind;
    candidate.id = record.id;
    candidate.version = QStringLiteral("2.0.0");
    candidate.sourceIdentity = record.sourceIdentity;
    candidate.contentIdentity = contentIdentityOf("another-candidate");

    // 每一条返回路径都不让候选可执行,也不传递权威。
    const QList<ExtensionLifecycleResult> results{
        ExtensionLifecycleController::stageUpdate(
            fixture.inputs, candidate, passingEvidence(), &reviewStore,
            &grantStore),
        ExtensionLifecycleController::stageUpdate(
            fixture.inputs, candidate, ExtensionUpdateEvidence(), &reviewStore,
            &grantStore),
        ExtensionLifecycleController::stageUpdate(
            fixture.inputs, ExtensionUpdateCandidate(), passingEvidence(),
            &reviewStore, &grantStore),
        ExtensionLifecycleController::remove(
            fixture.inputs, record.kind, record.id, &reviewStore, &grantStore),
    };
    for (const ExtensionLifecycleResult &result : results) {
        expect(!result.candidateExecutable,
               "some lifecycle path made a candidate executable");
        expect(!result.inheritsTrust,
               "some lifecycle path transferred trust");
        expect(!result.inheritsGrant,
               "some lifecycle path transferred a grant");
        expect(result.activePreserved,
               "some lifecycle path disturbed the active version");
    }

    // 清单永远不因为这一层而变成启用:那道门在四道门禁完成前保持关闭。
    const ExtensionLifecycleSnapshot snapshot =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    for (const ExtensionRegistryRecord &item : snapshot.inventory.records) {
        expect(!item.effectiveEnabled,
               "the lifecycle controller shipped an effectively enabled record");
    }

    // 两个存储都缺失时快照仍然可用,且不声称任何账本内容。
    const ExtensionLifecycleSnapshot none =
        ExtensionLifecycleController::inspect(fixture.inputs, nullptr, nullptr);
    expect(none.pins.isEmpty() && none.grants.isEmpty(),
           "a missing store produced ledger content");
    expect(none.reviewState == ExtensionReviewLedgerStoreState::Unavailable
               && none.grantState
                   == ExtensionEnablementLedgerStoreState::Unavailable,
           "a missing store was reported as an empty ledger");

    // 读不出来的账本不返回内容。一份不完整的集合会让界面显示"这些扩展没有被复核/授权过",
    // 而实际情况是当前状态未知——那正是最容易让人放心地按下启用的一种错误显示。
    // 上面的 remove 已经把两份记录都收回了,因此先重新建立它们:一个本来就空的账本无法
    // 证明"读不出来时返回空"这条规则。
    ExtensionRegistryRecord regranted;
    if (!expect(grantAndReview(fixture, &reviewStore, &grantStore, &regranted),
                "the fixture could not be re-granted for the unreadable case")) {
        return;
    }
    const ExtensionLifecycleSnapshot readable =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    if (!expect(readable.pins.size() == 1 && readable.grants.size() == 1,
                "the fixture held no ledger content for the unreadable case to hide")) {
        return;
    }
    reviewSecure.available = false;
    grantSecure.available = false;
    const ExtensionLifecycleSnapshot unreadable =
        ExtensionLifecycleController::inspect(fixture.inputs, &reviewStore,
                                             &grantStore);
    reviewSecure.available = true;
    grantSecure.available = true;
    expect(unreadable.reviewState != ExtensionReviewLedgerStoreState::Ready
               && unreadable.reviewState
                   != ExtensionReviewLedgerStoreState::Empty,
           "an unreadable review ledger reported a clean state");
    expect(unreadable.grantState != ExtensionEnablementLedgerStoreState::Ready
               && unreadable.grantState
                   != ExtensionEnablementLedgerStoreState::Empty,
           "an unreadable grant ledger reported a clean state");
    expect(unreadable.pins.isEmpty() && unreadable.grants.isEmpty(),
           "an unreadable ledger returned partial content as if it were complete");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    updateRejectionTests();
    updateStagingTests();
    removalTests();
    partialRemovalTests();
    removalOrderTests();
    acknowledgedButUnpersistedTests();
    unusableLedgerTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension lifecycle controller tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
