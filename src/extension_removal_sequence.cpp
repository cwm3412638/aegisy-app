#include "extension_removal_sequence.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace {

const QString kErrorPrefix = QStringLiteral("extension-removal-plan");
const QString kIdentityPrefix = QStringLiteral("extension-removal-plan:sha256:");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

// 与注册表、信任判定、启用判定保持一致：形式不合法的摘要不能进入计划。
bool hashIdentity(const QString &value, const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool wellFormed(const QString &sourceIdentity, const QString &contentIdentity)
{
    return hashIdentity(sourceIdentity, QStringLiteral("extension-source:sha256:"))
        && hashIdentity(contentIdentity, QStringLiteral("extension-content:sha256:"));
}

bool validId(const QString &value)
{
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
        .match(value).hasMatch();
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

QString subjectFor(ExtensionKind kind, const QString &id)
{
    const char *prefix = "skill:";
    if (kind == ExtensionKind::Mcp) prefix = "mcp:";
    if (kind == ExtensionKind::CodexPlugin) prefix = "codex-plugin:";
    return QLatin1String(prefix) + id;
}

ExtensionRemovalSequence refuse(const QString &errorCode, const QString &detail = QString())
{
    ExtensionRemovalSequence plan;
    plan.state = ExtensionRemovalSequenceState::Refused;
    plan.errorCode = errorCode;
    plan.errorDetail = detail;
    return plan;
}

// 纵深防御：捕获层已按同一套规则校验过条目名，但计划层在把任何路径放进删除列表之前
// 仍逐段重查一次。任何 traversal 形状（`..`、绝对路径、空段）都在这里失败关闭，而不是
// 靠"上游已经查过"——删除列表里的一个越界路径毁掉的是目标根之外的字节。
bool containedRelativePath(const QString &path)
{
    if (path.isEmpty() || path.toUtf8().size() > 4096) return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!ExtensionTreeCapture::safeEntryName(segment)) return false;
    }
    return true;
}

bool absolutePath(const QString &path)
{
    return !path.isEmpty() && !QFileInfo(path).isRelative();
}

} // namespace

ExtensionRemovalSequence ExtensionRemovalSequenceBuilder::plan(
    const ExtensionRemovalSequenceRequest &request)
{
    // 两份权威集合都必须先可读。对着读不出的授权或复核集合规划，等于规划一份可能
    // 静默搁浅权威的收回——计划会声称授权将被收回，而没有人知道它当前的状态。
    if (request.grantLedger.state != ExtensionEnablementLedgerStoreState::Ready
            && request.grantLedger.state != ExtensionEnablementLedgerStoreState::Empty) {
        return refuse(code("grant-ledger-unusable"), request.grantLedger.errorCode);
    }
    if (request.reviewLedger.state != ExtensionReviewLedgerStoreState::Ready
            && request.reviewLedger.state != ExtensionReviewLedgerStoreState::Empty) {
        return refuse(code("review-ledger-unusable"), request.reviewLedger.errorCode);
    }
    if (!validId(request.id)) {
        return refuse(code("target-id-invalid"));
    }

    // 种类边界是封闭的。codex-plugin 只有观察：应用从未安装它，没有任何处于自身
    // 权威内的字节可删。`mcp:` 移除是对共享设置文件的文档编辑而非文件删除，有界
    // 显式步骤列表不重新实现合并语义就无法诚实表达它——拒绝而不是产出 dishonest
    // 计划。语法之外的种类失败关闭，绝不落到某个默认处理。
    switch (request.kind) {
    case ExtensionKind::CodexPlugin:
        return refuse(code("codex-plugin-observation-only"));
    case ExtensionKind::Mcp:
        return refuse(code("mcp-document-edit-unsupported"));
    case ExtensionKind::Skill:
        break;
    }
    if (request.kind != ExtensionKind::Skill) {
        return refuse(code("kind-unmapped"));
    }

    // 清单解析目标。同一 (kind, id) 出现多条记录说明来源已不可信，不能任选一条
    // 规划删除——"删哪一条"没有正确答案。
    const ExtensionRegistryRecord *target = nullptr;
    int matches = 0;
    for (const ExtensionRegistryRecord &record : request.records) {
        if (record.kind != request.kind || record.id != request.id) continue;
        ++matches;
        target = &record;
    }
    if (matches > 1) {
        return refuse(code("target-ambiguous"));
    }
    if (target && (!validId(target->id)
            || !wellFormed(target->sourceIdentity, target->contentIdentity))) {
        return refuse(code("target-record-invalid"));
    }

    // 搁浅授权判定：内容是否已经消失（记录缺席或未安装）而权威记录仍然存在。
    // 撤销只按 (kind, id) 键合，因此漂移或消失的目标依然可撤销。
    bool strandedGrant = false;
    for (const ExtensionEnablementGrant &grant : request.grantLedger.grants) {
        if (grant.kind == request.kind && grant.id == request.id) strandedGrant = true;
    }
    bool strandedPin = false;
    for (const ExtensionReviewPin &pin : request.reviewLedger.pins) {
        if (pin.kind == request.kind && pin.id == request.id) strandedPin = true;
    }
    const bool strandedAuthority = strandedGrant || strandedPin;
    const bool contentGone = !target || !target->installed;
    if (contentGone && !strandedAuthority) {
        return refuse(target ? code("target-not-installed")
                             : code("target-absent"));
    }

    ExtensionRemovalSequence result;
    result.state = ExtensionRemovalSequenceState::Ready;
    result.degradedAuthorityOnly = contentGone;

    if (!contentGone) {
        // Managed 强制启用的扩展不可被用户请求移除：组织策略的强制结论不可被用户
        // 覆盖，而删除可执行内容正是最强形式的覆盖。拒绝携带阻挡层级的标签。
        for (const ExtensionScopeRule &rule : request.scopeRules) {
            if (rule.level != ExtensionScopeLevel::Managed || !rule.mandatory
                    || rule.disposition != ExtensionScopeDisposition::Enabled) {
                continue;
            }
            if (ExtensionScopePolicy::appliesTo(rule, *target)) {
                return refuse(code("managed-mandated"),
                              ExtensionScopePolicy::levelLabel(rule.level));
            }
        }

        // 权威路径：这一层从不发明位置。调用方给出的位置必须非空且绝对。
        if (!absolutePath(request.sourceRoot)
                || !absolutePath(request.backupRoot)) {
            return refuse(code("authority-path-invalid"));
        }
        if (!request.captureDomain.configured()) {
            return refuse(code("capture-domain-invalid"));
        }
        // 备份前置条件：计划只针对一份内容身份构建。没有新鲜捕获就没有"当前内容"
        // 的诚实图景，而对着陈旧记录规划删除会把漂移内容当成已备份内容。
        if (!request.freshTreeCaptured) {
            return refuse(code("fresh-capture-unavailable"));
        }
        if (request.freshTree.size() > ExtensionTreeCapture::MaxEntries) {
            return refuse(code("content-step-unbounded"));
        }
        for (const ExtensionTreeCaptureEntry &entry : request.freshTree) {
            if (!containedRelativePath(entry.relativePath)) {
                return refuse(code("capture-path-unsafe"));
            }
        }
        const QString freshIdentity = ExtensionTreeCapture::contentIdentity(
            request.captureDomain, request.freshTree);
        if (freshIdentity.isEmpty()) {
            return refuse(code("identity-unavailable"));
        }
        if (freshIdentity != target->contentIdentity) {
            return refuse(code("content-drift"));
        }

        // 第一步：备份。绑定记录的确切当前内容身份——执行侧的捕获必须与之逐字节
        // 相等，内容漂移的备份不是备份，捕获失败的移除拒绝删除。
        result.backup.possible = true;
        result.backup.subject = subjectFor(request.kind, request.id);
        result.backup.sourceRoot = request.sourceRoot;
        result.backup.backupRoot = request.backupRoot;
        result.backup.contentIdentity = target->contentIdentity;

        // 第四步：内容删除。有界显式文件列表，逐条携带字节数与期望摘要；目录按
        // 深度逆序排在文件之后。
        result.contentRemoval.possible = true;
        result.contentRemoval.sourceRoot = request.sourceRoot;
        QStringList directories;
        for (const ExtensionTreeCaptureEntry &entry : request.freshTree) {
            if (entry.directory) {
                directories.append(entry.relativePath);
                continue;
            }
            ExtensionRemovalContentFile file;
            file.relativePath = entry.relativePath;
            file.byteCount = entry.bytes.size();
            file.sha256 = sha256Hex(entry.bytes);
            result.contentRemoval.files.append(file);
        }
        std::sort(directories.begin(), directories.end(),
                  [](const QString &lhs, const QString &rhs) {
                      const int lhsDepth = lhs.count(QLatin1Char('/'));
                      const int rhsDepth = rhs.count(QLatin1Char('/'));
                      if (lhsDepth != rhsDepth) return lhsDepth > rhsDepth;
                      return lhs < rhs;
                  });
        result.contentRemoval.directories = directories;
    } else {
        // 退化计划：内容已消失，备份无从谈起，但计划必须**说出**备份不可能，
        // 而不是静默省略这一步。
        result.backup.possible = false;
        result.backup.impossibleCode = code("backup-impossible-content-gone");
        result.backup.subject = subjectFor(request.kind, request.id);
        result.contentRemoval.possible = false;
    }

    // 第二、三步：权威收回。完整提交后集合由既有工作流产出（撤销只按 (kind, id)
    // 键合），这一层绝不重新实现撤销语义——两份副本会各自漂移。工作流的拒绝以
    // 独立代号透传，细节代号放入 errorDetail。授权先于复核：任何中间失败都停在
    // "没有授权、复核尚存"的安全一侧。
    ExtensionEnablementRequest grantRequest;
    grantRequest.action = ExtensionEnablementAction::Disable;
    grantRequest.kind = request.kind;
    grantRequest.id = request.id;
    const ExtensionEnablementPlan grantPlan = ExtensionEnablementWorkflow::plan(
        grantRequest, request.records, request.grantLedger);
    if (grantPlan.state != ExtensionEnablementPlanState::Ready) {
        return refuse(code("grant-withdrawal-unplannable"), grantPlan.errorCode);
    }
    result.grantWithdrawal.kind = request.kind;
    result.grantWithdrawal.id = request.id;
    result.grantWithdrawal.resultingGrants = grantPlan.grants;
    result.grantWithdrawal.expectedGeneration = grantPlan.expectedGeneration;
    result.grantWithdrawal.changed = grantPlan.changed;

    ExtensionReviewRequest reviewRequest;
    reviewRequest.action = ExtensionReviewAction::Revoke;
    reviewRequest.kind = request.kind;
    reviewRequest.id = request.id;
    const ExtensionReviewPlan reviewPlan = ExtensionReviewWorkflow::plan(
        reviewRequest, request.records, request.reviewLedger);
    if (reviewPlan.state != ExtensionReviewPlanState::Ready) {
        return refuse(code("review-withdrawal-unplannable"), reviewPlan.errorCode);
    }
    result.reviewWithdrawal.kind = request.kind;
    result.reviewWithdrawal.id = request.id;
    result.reviewWithdrawal.resultingPins = reviewPlan.pins;
    result.reviewWithdrawal.expectedGeneration = reviewPlan.expectedGeneration;
    result.reviewWithdrawal.changed = reviewPlan.changed;

    // 第五步：元数据保留声明。记录身份、最后内容身份与备份 id 必须保留；备份 id
    // 在捕获完成后才存在，执行侧必须先记入它再删除。记录缺席时从幸存的授权或
    // 复核记录取最后已知身份——撤销的对象记得这份内容曾经被授权过。
    result.retention.mustRetain = true;
    result.retention.backupIdDeferred = true;
    result.retention.kind = request.kind;
    result.retention.id = request.id;
    if (target) {
        result.retention.sourceIdentity = target->sourceIdentity;
        result.retention.contentIdentity = target->contentIdentity;
        result.retention.contentIdentityKnown = true;
    } else {
        for (const ExtensionEnablementGrant &grant : request.grantLedger.grants) {
            if (grant.kind != request.kind || grant.id != request.id) continue;
            result.retention.sourceIdentity = grant.sourceIdentity;
            result.retention.contentIdentity = grant.contentIdentity;
            result.retention.contentIdentityKnown = true;
            break;
        }
        if (!result.retention.contentIdentityKnown) {
            for (const ExtensionReviewPin &pin : request.reviewLedger.pins) {
                if (pin.kind != request.kind || pin.id != request.id) continue;
                result.retention.sourceIdentity = pin.sourceIdentity;
                result.retention.contentIdentity = pin.contentIdentity;
                result.retention.contentIdentityKnown = true;
                break;
            }
        }
    }

    // 计划身份：主体、权威路径与每一步的每个字段按固定顺序长度分帧。少绑任何
    // 一项，同一份身份就能对两份不同的计划成立；交换授权与复核的分帧顺序会
    // 产出不同身份——重排过的"计划"因此无法通过身份校验。
    QList<QByteArray> parts;
    parts.append(subjectFor(request.kind, request.id).toUtf8());
    parts.append(request.sourceRoot.toUtf8());
    parts.append(request.backupRoot.toUtf8());
    parts.append(result.backup.possible ? QByteArrayLiteral("backup-possible")
                                        : QByteArrayLiteral("backup-impossible"));
    parts.append(result.backup.impossibleCode.toUtf8());
    parts.append(result.backup.contentIdentity.toUtf8());
    parts.append(QByteArrayLiteral("grant-withdrawal"));
    parts.append(QByteArray::number(result.grantWithdrawal.expectedGeneration));
    parts.append(result.grantWithdrawal.changed ? QByteArrayLiteral("changed")
                                                : QByteArrayLiteral("unchanged"));
    for (const ExtensionEnablementGrant &grant :
             result.grantWithdrawal.resultingGrants) {
        parts.append(QByteArray::number(static_cast<int>(grant.kind)));
        parts.append(grant.id.toUtf8());
        parts.append(grant.sourceIdentity.toUtf8());
        parts.append(grant.contentIdentity.toUtf8());
    }
    parts.append(QByteArrayLiteral("review-withdrawal"));
    parts.append(QByteArray::number(result.reviewWithdrawal.expectedGeneration));
    parts.append(result.reviewWithdrawal.changed ? QByteArrayLiteral("changed")
                                                 : QByteArrayLiteral("unchanged"));
    for (const ExtensionReviewPin &pin : result.reviewWithdrawal.resultingPins) {
        parts.append(QByteArray::number(static_cast<int>(pin.kind)));
        parts.append(pin.id.toUtf8());
        parts.append(pin.sourceIdentity.toUtf8());
        parts.append(pin.contentIdentity.toUtf8());
    }
    parts.append(QByteArrayLiteral("content-removal"));
    parts.append(result.contentRemoval.possible
                     ? QByteArrayLiteral("possible")
                     : QByteArrayLiteral("impossible"));
    for (const ExtensionRemovalContentFile &file : result.contentRemoval.files) {
        parts.append(file.relativePath.toUtf8());
        parts.append(QByteArray::number(file.byteCount));
        parts.append(file.sha256.toUtf8());
    }
    parts.append(QByteArrayLiteral("directories"));
    for (const QString &directory : result.contentRemoval.directories) {
        parts.append(directory.toUtf8());
    }
    parts.append(QByteArrayLiteral("retention"));
    parts.append(QByteArray::number(static_cast<int>(result.retention.kind)));
    parts.append(result.retention.id.toUtf8());
    parts.append(result.retention.sourceIdentity.toUtf8());
    parts.append(result.retention.contentIdentityKnown
                     ? result.retention.contentIdentity.toUtf8()
                     : QByteArrayLiteral("absent"));
    parts.append(result.retention.backupIdDeferred
                     ? QByteArrayLiteral("deferred")
                     : QByteArrayLiteral("recorded"));
    result.planIdentity = ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-extension-removal-plan/0.1\0"),
        parts, kIdentityPrefix);
    if (result.planIdentity.isEmpty()) {
        return refuse(code("identity-unavailable"));
    }
    return result;
}
