#include "extension_removal_sequence.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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

ExtensionTreeCaptureDomain skillDomain()
{
    return {QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
            QStringLiteral("extension-content:sha256:"),
            QStringLiteral("skill")};
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

QString sourceIdentity()
{
    return QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('a'));
}

QString otherContentIdentity()
{
    return QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('b'));
}

// 一棵纯数据的捕获树：计划层不接触文件系统，测试因此也不需要真实目录。
QVector<ExtensionTreeCaptureEntry> fixtureTree()
{
    QVector<ExtensionTreeCaptureEntry> tree;
    ExtensionTreeCaptureEntry manifest;
    manifest.relativePath = QStringLiteral("SKILL.md");
    manifest.bytes = QByteArrayLiteral("# Notes\n");
    tree.append(manifest);
    ExtensionTreeCaptureEntry docs;
    docs.relativePath = QStringLiteral("docs");
    docs.directory = true;
    tree.append(docs);
    ExtensionTreeCaptureEntry guide;
    guide.relativePath = QStringLiteral("docs/guide.md");
    guide.bytes = QByteArrayLiteral("guide\n");
    tree.append(guide);
    return tree;
}

QString treeIdentity(const QVector<ExtensionTreeCaptureEntry> &tree)
{
    return ExtensionTreeCapture::contentIdentity(skillDomain(), tree);
}

ExtensionRegistryRecord skillRecord(const QString &id,
                                    const QString &contentIdentity)
{
    ExtensionRegistryRecord record;
    record.kind = ExtensionKind::Skill;
    record.id = id;
    record.name = id;
    record.version = QStringLiteral("1.0.0");
    record.sourceKind = ExtensionSourceKind::LocalDirectory;
    record.sourceIdentity = sourceIdentity();
    record.contentIdentity = contentIdentity;
    record.trust = ExtensionTrustState::Verified;
    record.compatibility = ExtensionCompatibilityState::Compatible;
    record.installed = true;
    return record;
}

ExtensionEnablementGrant grantFor(const QString &id,
                                  const QString &contentIdentity)
{
    ExtensionEnablementGrant grant;
    grant.kind = ExtensionKind::Skill;
    grant.id = id;
    grant.sourceIdentity = sourceIdentity();
    grant.contentIdentity = contentIdentity;
    return grant;
}

ExtensionReviewPin pinFor(const QString &id, const QString &contentIdentity)
{
    ExtensionReviewPin pin;
    pin.kind = ExtensionKind::Skill;
    pin.id = id;
    pin.sourceIdentity = sourceIdentity();
    pin.contentIdentity = contentIdentity;
    return pin;
}

// 标准请求：一个已安装的技能记录，带一份授权与一份复核，新鲜捕获与记录一致。
ExtensionRemovalSequenceRequest standardRequest()
{
    const QVector<ExtensionTreeCaptureEntry> tree = fixtureTree();
    ExtensionRemovalSequenceRequest request;
    request.kind = ExtensionKind::Skill;
    request.id = QStringLiteral("notes");
    request.records = {skillRecord(QStringLiteral("notes"), treeIdentity(tree)),
                       skillRecord(QStringLiteral("other"),
                                   otherContentIdentity())};
    request.reviewLedger.state = ExtensionReviewLedgerStoreState::Ready;
    request.reviewLedger.generation = 7;
    request.reviewLedger.pins = {pinFor(QStringLiteral("notes"),
                                        treeIdentity(tree)),
                                 pinFor(QStringLiteral("other"),
                                        otherContentIdentity())};
    request.grantLedger.state = ExtensionEnablementLedgerStoreState::Ready;
    request.grantLedger.generation = 11;
    request.grantLedger.grants = {grantFor(QStringLiteral("notes"),
                                           treeIdentity(tree)),
                                  grantFor(QStringLiteral("other"),
                                           otherContentIdentity())};
    request.sourceRoot = QStringLiteral("/tmp/aegisy-removal-test/skills/notes");
    request.backupRoot = QStringLiteral("/tmp/aegisy-removal-test/backups");
    request.captureDomain = skillDomain();
    request.freshTree = tree;
    request.freshTreeCaptured = true;
    return request;
}

// 独立重算计划身份：与实现同一套域、分帧与字段顺序。swapAuthority 为真时把复核
// 分帧放在授权之前——一份"先收回复核"的重排计划必须产出不同身份。
QString recomputeIdentity(const ExtensionRemovalSequenceRequest &request,
                          const ExtensionRemovalSequence &plan,
                          bool swapAuthority)
{
    QList<QByteArray> parts;
    parts.append(QByteArrayLiteral("skill:") + request.id.toUtf8());
    parts.append(request.sourceRoot.toUtf8());
    parts.append(request.backupRoot.toUtf8());
    parts.append(plan.backup.possible ? QByteArrayLiteral("backup-possible")
                                      : QByteArrayLiteral("backup-impossible"));
    parts.append(plan.backup.impossibleCode.toUtf8());
    parts.append(plan.backup.contentIdentity.toUtf8());
    QList<QByteArray> grantParts;
    grantParts.append(QByteArrayLiteral("grant-withdrawal"));
    grantParts.append(
        QByteArray::number(plan.grantWithdrawal.expectedGeneration));
    grantParts.append(plan.grantWithdrawal.changed
                          ? QByteArrayLiteral("changed")
                          : QByteArrayLiteral("unchanged"));
    for (const ExtensionEnablementGrant &grant :
             plan.grantWithdrawal.resultingGrants) {
        grantParts.append(QByteArray::number(static_cast<int>(grant.kind)));
        grantParts.append(grant.id.toUtf8());
        grantParts.append(grant.sourceIdentity.toUtf8());
        grantParts.append(grant.contentIdentity.toUtf8());
    }
    QList<QByteArray> reviewParts;
    reviewParts.append(QByteArrayLiteral("review-withdrawal"));
    reviewParts.append(
        QByteArray::number(plan.reviewWithdrawal.expectedGeneration));
    reviewParts.append(plan.reviewWithdrawal.changed
                           ? QByteArrayLiteral("changed")
                           : QByteArrayLiteral("unchanged"));
    for (const ExtensionReviewPin &pin : plan.reviewWithdrawal.resultingPins) {
        reviewParts.append(QByteArray::number(static_cast<int>(pin.kind)));
        reviewParts.append(pin.id.toUtf8());
        reviewParts.append(pin.sourceIdentity.toUtf8());
        reviewParts.append(pin.contentIdentity.toUtf8());
    }
    if (swapAuthority) {
        parts.append(reviewParts);
        parts.append(grantParts);
    } else {
        parts.append(grantParts);
        parts.append(reviewParts);
    }
    parts.append(QByteArrayLiteral("content-removal"));
    parts.append(plan.contentRemoval.possible
                     ? QByteArrayLiteral("possible")
                     : QByteArrayLiteral("impossible"));
    for (const ExtensionRemovalContentFile &file : plan.contentRemoval.files) {
        parts.append(file.relativePath.toUtf8());
        parts.append(QByteArray::number(file.byteCount));
        parts.append(file.sha256.toUtf8());
    }
    parts.append(QByteArrayLiteral("directories"));
    for (const QString &directory : plan.contentRemoval.directories) {
        parts.append(directory.toUtf8());
    }
    parts.append(QByteArrayLiteral("retention"));
    parts.append(QByteArray::number(static_cast<int>(plan.retention.kind)));
    parts.append(plan.retention.id.toUtf8());
    parts.append(plan.retention.sourceIdentity.toUtf8());
    parts.append(plan.retention.contentIdentityKnown
                     ? plan.retention.contentIdentity.toUtf8()
                     : QByteArrayLiteral("absent"));
    parts.append(plan.retention.backupIdDeferred
                     ? QByteArrayLiteral("deferred")
                     : QByteArrayLiteral("recorded"));
    return ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-extension-removal-plan/0.1\0"), parts,
        QStringLiteral("extension-removal-plan:sha256:"));
}

bool refusedWith(const ExtensionRemovalSequence &plan, const QString &code)
{
    return plan.state == ExtensionRemovalSequenceState::Refused
        && plan.errorCode == code;
}

void testHappyPath()
{
    const ExtensionRemovalSequenceRequest request = standardRequest();
    const ExtensionRemovalSequence plan = ExtensionRemovalSequenceBuilder::plan(request);
    expect(plan.state == ExtensionRemovalSequenceState::Ready,
           "a removable installed skill was refused");
    expect(plan.errorCode.isEmpty(), "a ready plan carries an error code");
    expect(!plan.degradedAuthorityOnly,
           "a present installed target planned as degraded");

    // 步骤顺序常量：备份 → 授权收回 → 复核收回 → 内容删除 → 保留声明。
    expect(plan.backup.order == 0 && plan.grantWithdrawal.order == 1
               && plan.reviewWithdrawal.order == 2
               && plan.contentRemoval.order == 3 && plan.retention.order == 4,
           "the step order constants drifted");

    // 备份步：绑定记录的确切当前内容身份与调用方给出的两个权威路径。
    expect(plan.backup.possible, "the backup step is not possible");
    expect(plan.backup.subject == QStringLiteral("skill:notes"),
           "the backup subject drifted");
    expect(plan.backup.sourceRoot == request.sourceRoot
               && plan.backup.backupRoot == request.backupRoot,
           "the backup step does not echo the caller-supplied authority paths");
    expect(plan.backup.contentIdentity
               == request.records.first().contentIdentity,
           "the backup step is not bound to the record's content identity");

    // 授权收回：完整提交后集合,目标被移除、无关授权保留、代号透传。
    expect(plan.grantWithdrawal.changed,
           "the grant withdrawal reports no change");
    expect(plan.grantWithdrawal.expectedGeneration == 11,
           "the grant withdrawal lost the CAS generation");
    expect(plan.grantWithdrawal.resultingGrants.size() == 1
               && plan.grantWithdrawal.resultingGrants.first().id
                   == QStringLiteral("other"),
           "the post-commit grant set is wrong");

    // 复核收回：同构,且在授权之后。
    expect(plan.reviewWithdrawal.changed,
           "the review withdrawal reports no change");
    expect(plan.reviewWithdrawal.expectedGeneration == 7,
           "the review withdrawal lost the CAS generation");
    expect(plan.reviewWithdrawal.resultingPins.size() == 1
               && plan.reviewWithdrawal.resultingPins.first().id
                   == QStringLiteral("other"),
           "the post-commit pin set is wrong");

    // 内容删除：有界显式文件列表,逐条携带摘要;目录深度逆序在文件之后。
    expect(plan.contentRemoval.possible, "the content step is not possible");
    expect(plan.contentRemoval.files.size() == 2,
           "the deletion list is not the captured file set");
    if (plan.contentRemoval.files.size() == 2) {
        const ExtensionRemovalContentFile &first =
            plan.contentRemoval.files.at(0);
        expect(first.relativePath == QStringLiteral("SKILL.md")
                   && first.byteCount == 8
                   && first.sha256 == sha256Hex(QByteArrayLiteral("# Notes\n")),
               "the first deletion entry drifted from the capture");
        expect(plan.contentRemoval.files.at(1).relativePath
                   == QStringLiteral("docs/guide.md"),
               "the second deletion entry drifted from the capture");
    }
    expect(plan.contentRemoval.directories
               == QStringList{QStringLiteral("docs")},
           "the directory deletion list drifted");

    // 保留声明：身份元数据必须保留,备份 id 延后到捕获完成后由执行侧记入。
    expect(plan.retention.mustRetain && plan.retention.backupIdDeferred,
           "the retention statement lost its mandates");
    expect(plan.retention.contentIdentityKnown
               && plan.retention.contentIdentity
                   == request.records.first().contentIdentity
               && plan.retention.sourceIdentity == sourceIdentity(),
           "the retention statement does not carry the record identity");

    expect(plan.planIdentity.startsWith(
               QStringLiteral("extension-removal-plan:sha256:"))
               && plan.planIdentity.size()
                   == QStringLiteral("extension-removal-plan:sha256:").size()
                       + 64,
           "the plan identity lost its framing");
}

void testIdentityStabilityAndBinding()
{
    const ExtensionRemovalSequenceRequest request = standardRequest();
    const ExtensionRemovalSequence first = ExtensionRemovalSequenceBuilder::plan(request);
    const ExtensionRemovalSequence second = ExtensionRemovalSequenceBuilder::plan(request);
    expect(first.planIdentity == second.planIdentity,
           "the plan identity is not stable for identical inputs");

    ExtensionRemovalSequenceRequest moved = request;
    moved.backupRoot = QStringLiteral("/tmp/aegisy-removal-test/other-backups");
    expect(ExtensionRemovalSequenceBuilder::plan(moved).planIdentity
               != first.planIdentity,
           "the plan identity does not bind the backup destination");

    // 身份绑定内容:漂移的捕获与记录不一致即拒绝,而不是产出另一份计划。
    ExtensionRemovalSequenceRequest drifted = request;
    drifted.freshTree = fixtureTree();
    drifted.freshTree[0].bytes = QByteArrayLiteral("# Tampered\n");
    const ExtensionRemovalSequence driftedPlan =
        ExtensionRemovalSequenceBuilder::plan(drifted);
    expect(refusedWith(driftedPlan,
                       QStringLiteral("extension-removal-plan-content-drift")),
           "a drifted capture still planned a removal");
    expect(driftedPlan.planIdentity.isEmpty(),
           "a refused plan carries an identity");

    // 记录身份与捕获一致但记录被换成另一份身份:同样漂移拒绝。
    ExtensionRemovalSequenceRequest mismatched = request;
    mismatched.records[0].contentIdentity = otherContentIdentity();
    expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(mismatched),
                       QStringLiteral("extension-removal-plan-content-drift")),
           "a record pointing at other content still planned a removal");
}

void testOrderingProof()
{
    const ExtensionRemovalSequenceRequest request = standardRequest();
    const ExtensionRemovalSequence plan = ExtensionRemovalSequenceBuilder::plan(request);
    expect(plan.state == ExtensionRemovalSequenceState::Ready,
           "the ordering fixture was refused");

    // 顺序由类型形状固定:授权收回与复核收回是两个独立字段,不存在可重排的
    // 步骤列表;顺序常量由构建器写入且授权严格在复核之前。
    expect(plan.grantWithdrawal.order < plan.reviewWithdrawal.order,
           "the grant withdrawal no longer precedes the review withdrawal");

    // 独立重算:按实现的字段顺序重算必须与计划身份逐字节相等;把复核分帧放到
    // 授权之前的重排版本必须产出不同身份——交换顺序的"计划"无法通过身份校验。
    expect(recomputeIdentity(request, plan, false) == plan.planIdentity,
           "the independent identity recomputation mismatched");
    expect(!recomputeIdentity(request, plan, true).isEmpty()
               && recomputeIdentity(request, plan, true) != plan.planIdentity,
           "a review-before-grant reordering validates against the plan identity");
}

void testRefusals()
{
    const ExtensionRemovalSequenceRequest base = standardRequest();

    {
        ExtensionRemovalSequenceRequest request = base;
        request.grantLedger.state = ExtensionEnablementLedgerStoreState::Invalid;
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-grant-ledger-unusable")),
               "an unreadable grant ledger still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.reviewLedger.state =
            ExtensionReviewLedgerStoreState::Unavailable;
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-review-ledger-unusable")),
               "an unreadable review ledger still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.id = QStringLiteral("Bad Id");
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-target-id-invalid")),
               "a malformed target id still planned");
    }
    {
        // codex-plugin 只有观察:即便清单里存在记录,应用也从未安装它,
        // 没有任何处于自身权威内可删除的字节。
        ExtensionRemovalSequenceRequest request = base;
        request.kind = ExtensionKind::CodexPlugin;
        ExtensionRegistryRecord plugin;
        plugin.kind = ExtensionKind::CodexPlugin;
        plugin.id = request.id;
        plugin.sourceKind = ExtensionSourceKind::CodexCli;
        plugin.installed = true;
        request.records.append(plugin);
        expect(refusedWith(
                   ExtensionRemovalSequenceBuilder::plan(request),
                   QStringLiteral(
                       "extension-removal-plan-codex-plugin-observation-only")),
               "an observation-only codex plugin planned a removal");
    }
    {
        // mcp 移除是对共享设置文件的文档编辑,不是文件删除;本切片拒绝。
        ExtensionRemovalSequenceRequest request = base;
        request.kind = ExtensionKind::Mcp;
        expect(refusedWith(
                   ExtensionRemovalSequenceBuilder::plan(request),
                   QStringLiteral(
                       "extension-removal-plan-mcp-document-edit-unsupported")),
               "an mcp document edit planned as a file removal");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.records.clear();
        request.reviewLedger.pins.clear();
        request.grantLedger.grants.clear();
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-target-absent")),
               "a target with no record and no authority still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.records.append(request.records.first());
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-target-ambiguous")),
               "a duplicated (kind, id) record still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.records[0].installed = false;
        request.reviewLedger.pins.clear();
        request.grantLedger.grants.clear();
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-target-not-installed")),
               "a not-installed target with no authority still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.records[0].contentIdentity = QStringLiteral("junk");
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-target-record-invalid")),
               "a malformed record identity still planned");
    }
    {
        // Managed 强制启用:组织策略强制的扩展不可被用户请求移除,拒绝携带
        // 阻挡层级的标签。
        ExtensionRemovalSequenceRequest request = base;
        ExtensionScopeRule mandate;
        mandate.level = ExtensionScopeLevel::Managed;
        mandate.kind = ExtensionKind::Skill;
        mandate.id = request.id;
        mandate.sourceIdentity = sourceIdentity();
        mandate.contentIdentity = request.records.first().contentIdentity;
        mandate.disposition = ExtensionScopeDisposition::Enabled;
        mandate.mandatory = true;
        request.scopeRules = {mandate};
        const ExtensionRemovalSequence plan =
            ExtensionRemovalSequenceBuilder::plan(request);
        expect(refusedWith(plan, QStringLiteral(
                                     "extension-removal-plan-managed-mandated")),
               "a Managed-mandated extension planned a removal");
        expect(plan.errorDetail == ExtensionScopePolicy::levelLabel(
                                       ExtensionScopeLevel::Managed),
               "the mandate refusal does not name the blocking level");
        // 非强制或绑定其他内容的 Managed 规则不阻挡。
        ExtensionRemovalSequenceRequest unrelated = base;
        ExtensionScopeRule drifted = mandate;
        drifted.contentIdentity = otherContentIdentity();
        unrelated.scopeRules = {drifted};
        expect(ExtensionRemovalSequenceBuilder::plan(unrelated).state
                   == ExtensionRemovalSequenceState::Ready,
               "a mandate bound to other content blocked the removal");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.freshTreeCaptured = false;
        request.freshTree.clear();
        expect(refusedWith(
                   ExtensionRemovalSequenceBuilder::plan(request),
                   QStringLiteral(
                       "extension-removal-plan-fresh-capture-unavailable")),
               "a removal planned without a fresh capture");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.freshTree[0].relativePath = QStringLiteral("../escape.md");
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-capture-path-unsafe")),
               "an escaping capture path reached the deletion list");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.captureDomain = ExtensionTreeCaptureDomain();
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-capture-domain-invalid")),
               "an unconfigured capture domain still planned");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        request.sourceRoot = QStringLiteral("relative/skills/notes");
        expect(refusedWith(ExtensionRemovalSequenceBuilder::plan(request),
                           QStringLiteral(
                               "extension-removal-plan-authority-path-invalid")),
               "a relative source root still planned");
    }
    {
        // 授权集合内部冲突:工作流拒绝,本层以独立代号透传并保留下游代号。
        ExtensionRemovalSequenceRequest request = base;
        request.grantLedger.grants.append(request.grantLedger.grants.first());
        const ExtensionRemovalSequence plan =
            ExtensionRemovalSequenceBuilder::plan(request);
        expect(refusedWith(plan,
                           QStringLiteral(
                               "extension-removal-plan-grant-withdrawal-unplannable")),
               "a conflicting grant set still planned");
        expect(plan.errorDetail
                   == QStringLiteral("extension-enablement-ledger-conflict"),
               "the grant withdrawal refusal lost the workflow code");
    }
    {
        ExtensionRemovalSequenceRequest request = base;
        ExtensionReviewPin malformed = request.reviewLedger.pins.first();
        malformed.contentIdentity = QStringLiteral("junk");
        request.reviewLedger.pins.append(malformed);
        const ExtensionRemovalSequence plan =
            ExtensionRemovalSequenceBuilder::plan(request);
        expect(refusedWith(plan,
                           QStringLiteral(
                               "extension-removal-plan-review-withdrawal-unplannable")),
               "a malformed pin set still planned");
        expect(plan.errorDetail
                   == QStringLiteral("extension-review-ledger-pin-invalid"),
               "the review withdrawal refusal lost the workflow code");
    }
}

void testStrandedAuthority()
{
    const ExtensionRemovalSequenceRequest base = standardRequest();

    // 记录整体缺席但授权与复核尚存:退化计划照常收回权威,备份步显式声明
    // 不可能,保留声明从幸存的授权取回最后已知身份。
    ExtensionRemovalSequenceRequest request = base;
    request.records.clear();
    request.sourceRoot.clear();
    request.backupRoot.clear();
    request.freshTree.clear();
    request.freshTreeCaptured = false;
    const ExtensionRemovalSequence plan = ExtensionRemovalSequenceBuilder::plan(request);
    expect(plan.state == ExtensionRemovalSequenceState::Ready,
           "stranded authority could not be planned for withdrawal");
    expect(plan.degradedAuthorityOnly,
           "a content-gone plan is not marked degraded");
    expect(!plan.backup.possible
               && plan.backup.impossibleCode == QStringLiteral(
                   "extension-removal-plan-backup-impossible-content-gone"),
           "the degraded plan does not say the backup was impossible");
    expect(!plan.contentRemoval.possible
               && plan.contentRemoval.files.isEmpty()
               && plan.contentRemoval.directories.isEmpty(),
           "a degraded plan still carries a deletion list");
    expect(plan.grantWithdrawal.changed
               && plan.grantWithdrawal.resultingGrants.size() == 1,
           "the degraded plan does not withdraw the stranded grant");
    expect(plan.reviewWithdrawal.changed
               && plan.reviewWithdrawal.resultingPins.size() == 1,
           "the degraded plan does not withdraw the stranded pin");
    expect(plan.retention.contentIdentityKnown
               && plan.retention.contentIdentity == treeIdentity(fixtureTree()),
           "the degraded retention lost the last known content identity");
    expect(!plan.planIdentity.isEmpty(),
           "a degraded plan carries no identity");
    expect(recomputeIdentity(request, plan, false) == plan.planIdentity,
           "the degraded plan identity does not recompute");

    // 记录存在但未安装、只有复核记录:同样退化,保留声明取自记录身份。
    ExtensionRemovalSequenceRequest uninstalled = base;
    uninstalled.records[0].installed = false;
    uninstalled.grantLedger.grants.clear();
    const ExtensionRemovalSequence uninstalledPlan =
        ExtensionRemovalSequenceBuilder::plan(uninstalled);
    expect(uninstalledPlan.state == ExtensionRemovalSequenceState::Ready
               && uninstalledPlan.degradedAuthorityOnly,
           "a not-installed record with a stranded pin refused to plan");
    expect(!uninstalledPlan.grantWithdrawal.changed
               && uninstalledPlan.reviewWithdrawal.changed,
           "the degraded plan misstates which authority is stranded");
    expect(uninstalledPlan.retention.contentIdentity
               == treeIdentity(fixtureTree()),
           "the not-installed retention does not carry the record identity");
}

void testInstalledWithoutAuthority()
{
    // 已安装但从未被复核/授权的内容:移除照常可规划,两步权威收回是显式
    // 无操作(changed=false)而不是被省略,备份与删除步骤照常。
    ExtensionRemovalSequenceRequest request = standardRequest();
    request.reviewLedger.state = ExtensionReviewLedgerStoreState::Empty;
    request.reviewLedger.generation = 0;
    request.reviewLedger.pins.clear();
    request.grantLedger.state = ExtensionEnablementLedgerStoreState::Empty;
    request.grantLedger.generation = 0;
    request.grantLedger.grants.clear();
    const ExtensionRemovalSequence plan = ExtensionRemovalSequenceBuilder::plan(request);
    expect(plan.state == ExtensionRemovalSequenceState::Ready,
           "an installed extension without any authority refused to plan");
    expect(!plan.grantWithdrawal.changed && !plan.reviewWithdrawal.changed,
           "an empty ledger reported a changed withdrawal");
    expect(plan.backup.possible && plan.contentRemoval.possible
               && plan.retention.mustRetain,
           "an authority-free removal lost its backup, deletion, or retention");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);
    testHappyPath();
    testIdentityStabilityAndBinding();
    testOrderingProof();
    testRefusals();
    testStrandedAuthority();
    testInstalledWithoutAuthority();
    if (failures == 0) {
        QTextStream(stdout) << "extension removal plan guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
