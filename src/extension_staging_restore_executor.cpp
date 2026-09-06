#include "extension_staging_restore_executor.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace {

const QString kErrorPrefix = QStringLiteral("extension-restore-execution");
const QString kPlanIdentityPrefix =
    QStringLiteral("extension-staging-restore-plan:sha256:");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

// 与计划层完全相同的逐段包含性规则。执行器是最后一道防线：验证过的快照携带不了逃逸路径，
// 但一份手工构造的计划可能携带——每一条操作在触碰磁盘之前都必须重新过这一关。
bool containedRelativePath(const QString &path)
{
    if (path.isEmpty() || path.toUtf8().size() > 4096) return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!ExtensionTreeCapture::safeEntryName(segment)) return false;
    }
    return true;
}

bool sha256Shape(const QString &digest)
{
    if (digest.size() != 64) return false;
    for (const QChar character : digest) {
        const ushort value = character.unicode();
        const bool digit = value >= '0' && value <= '9';
        const bool lower = value >= 'a' && value <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

using Observation = ExtensionStagingRestoreObservation;
using Operation = ExtensionStagingRestoreOperation;

// 重观察一条计划路径的每一个前缀组件（不含路径本身）。计划时刻这些祖先被证明是目录或缺
// 失；现在出现符号链接仍是符号链接拒绝，出现文件/特殊文件则是漂移——计划不是针对这个
// 状态决定的。
bool reobserveAncestors(Observation *observation, const QString &path,
                        QString *error)
{
    const QStringList segments = path.split(QLatin1Char('/'));
    QString prefix;
    for (int index = 0; index + 1 < segments.size(); ++index) {
        prefix += segments.at(index);
        switch (observation->nodeKind(prefix)) {
        case Observation::NodeKind::Unavailable:
            *error = code("destination-unavailable");
            return false;
        case Observation::NodeKind::Symlink:
            *error = code("symlink-component");
            return false;
        case Observation::NodeKind::File:
        case Observation::NodeKind::Other:
            *error = code("destination-drift");
            return false;
        case Observation::NodeKind::Missing:
        case Observation::NodeKind::Directory:
            break;
        }
        prefix += QLatin1Char('/');
    }
    return true;
}

// 对照计划期望重观察一条操作的当前目标状态（pre-flight 与逐条执行前共用）。directory
// 操作期望 already-in-place 为 Directory、否则 Missing；文件操作期望 already-in-place
// 为逐字节一致的 File、否则 Missing。任何不符都是漂移而不是覆盖理由。
bool reobserveOperation(Observation *observation, const Operation &operation,
                        QString *error)
{
    if (!reobserveAncestors(observation, operation.relativePath, error)) {
        return false;
    }
    const Observation::NodeKind kind =
        observation->nodeKind(operation.relativePath);
    switch (kind) {
    case Observation::NodeKind::Unavailable:
        *error = code("destination-unavailable");
        return false;
    case Observation::NodeKind::Symlink:
        *error = code("symlink-component");
        return false;
    default:
        break;
    }
    if (!operation.alreadyInPlace) {
        if (kind != Observation::NodeKind::Missing) {
            *error = code("destination-drift");
            return false;
        }
        return true;
    }
    if (operation.directory) {
        if (kind != Observation::NodeKind::Directory) {
            *error = code("destination-drift");
            return false;
        }
        return true;
    }
    if (kind != Observation::NodeKind::File) {
        *error = code("destination-drift");
        return false;
    }
    // already-in-place 的重复核：重读既有字节并对期望摘要重哈希。读不出来即目标未知；
    // 摘要不符即该条目自计划以来已漂移——那是冲突，不是跳过。
    QByteArray existing;
    if (!observation->fileContent(operation.relativePath, &existing)) {
        *error = code("destination-unavailable");
        return false;
    }
    if (existing.size() != operation.byteCount
            || sha256Hex(existing) != operation.sha256) {
        *error = code("destination-drift");
        return false;
    }
    return true;
}

ExtensionStagingRestoreExecutionResult refuse(
        const ExtensionStagingRestorePlan &plan, const QString &reason)
{
    ExtensionStagingRestoreExecutionResult result;
    result.state = ExtensionStagingRestoreExecutionState::Refused;
    result.errorCode = reason;
    result.planIdentity = plan.planIdentity;
    result.treeIdentity = plan.treeIdentity;
    // 拒绝意味着零写入：逐条如实报告为未触达。
    for (const Operation &operation : plan.operations) {
        ExtensionStagingRestoreOperationResult entry;
        entry.directory = operation.directory;
        entry.relativePath = operation.relativePath;
        result.operations.append(entry);
    }
    return result;
}

} // namespace

ExtensionStagingRestoreExecutionResult ExtensionStagingRestoreExecutor::execute(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QString &expectedSubject,
        const ConfigurationBackupSnapshot &snapshot,
        const ExtensionStagingRestorePlan &plan,
        const ExtensionStagingRestoreApprovalVerdict &credential,
        ExtensionStagingRestoreObservation *observation)
{
    // 观察不可用：执行器绝不盲写。
    if (!observation) {
        return refuse(plan, code("destination-unavailable"));
    }

    // 计划形状：身份前缀、目标根与主体必须先成立，后续比较才有意义。
    if (plan.planIdentity.isEmpty()
            || !plan.planIdentity.startsWith(kPlanIdentityPrefix)
            || plan.destinationRoot.isEmpty()
            || QFileInfo(plan.destinationRoot).isRelative()
            || plan.subject.isEmpty()) {
        return refuse(plan, code("plan-invalid"));
    }

    // 凭据绑定（全部意义所在）：批准必须存在，且两个身份都与被执行的计划逐字节相等。
    // 给计划 A 签发的凭据永远执行不了计划 B。
    if (credential.state != ExtensionStagingRestoreApprovalState::Authorized) {
        return refuse(plan, code("credential-not-authorized"));
    }
    if (credential.authorizedPlanIdentity != plan.planIdentity) {
        return refuse(plan, code("credential-plan-mismatch"));
    }
    if (credential.authorizedTreeIdentity != plan.treeIdentity) {
        return refuse(plan, code("credential-tree-mismatch"));
    }

    // 每条操作的包含性重查先于一切：逃逸路径连快照验证都走不到。
    bool directoryPhase = true;
    for (const Operation &operation : plan.operations) {
        if (!containedRelativePath(operation.relativePath)) {
            return refuse(plan, code("path-escapes-destination"));
        }
        if (operation.directory) {
            if (!directoryPhase) return refuse(plan, code("plan-invalid"));
            continue;
        }
        directoryPhase = false;
        if (operation.byteCount < 0 || !sha256Shape(operation.sha256)
                || operation.sourceSlot <= 0) {
            return refuse(plan, code("plan-invalid"));
        }
    }

    // 快照在执行开始时重新验证：绝不相信调用方"已验证"的声称。验证诊断原样透传，
    // 篡改过的快照在任何写入之前失败关闭。
    QVector<ExtensionTreeCaptureEntry> tree;
    QString error;
    if (!ExtensionStagingSnapshot::verify(captureDomain, expectedSubject,
                                          snapshot, &tree, &error)) {
        return refuse(plan, error);
    }
    const QString treeIdentity =
        ExtensionTreeCapture::contentIdentity(captureDomain, tree);
    if (treeIdentity.isEmpty() || plan.treeIdentity != treeIdentity
            || plan.subject != expectedSubject) {
        return refuse(plan, code("plan-snapshot-mismatch"));
    }

    // 计划必须就是从这份快照构建的那一份：按同一规则（目录在前、文件在后，各保持清单
    // 顺序）从验证侧重建的树重新推导期望操作序列，逐字段对齐。already-in-place 标志依
    // 赖于目标状态、无法从树推导——它由计划身份绑定，凭据已把它绑死。
    QVector<const ExtensionTreeCaptureEntry *> fileEntries;
    int planIndex = 0;
    int slot = 1;
    bool aligned = true;
    for (int pass = 0; pass < 2 && aligned; ++pass) {
        const bool wantDirectory = pass == 0;
        for (const ExtensionTreeCaptureEntry &entry : tree) {
            if (entry.directory != wantDirectory) continue;
            if (planIndex >= plan.operations.size()) {
                aligned = false;
                break;
            }
            const Operation &operation = plan.operations.at(planIndex);
            if (operation.directory != entry.directory
                    || operation.relativePath != entry.relativePath) {
                aligned = false;
                break;
            }
            if (!entry.directory) {
                if (operation.byteCount != entry.bytes.size()
                        || operation.sha256 != sha256Hex(entry.bytes)
                        || operation.sourceSlot != slot) {
                    aligned = false;
                    break;
                }
                fileEntries.append(&entry);
                ++slot;
            }
            ++planIndex;
        }
    }
    if (!aligned || planIndex != plan.operations.size()) {
        return refuse(plan, code("plan-snapshot-mismatch"));
    }

    // 执行前重观察（pre-flight）：计划是针对某一个被观察到的目标状态决定的。在第一个
    // 字节写入之前，用与计划层相同的纪律重观察目标；任何漂移都拒绝，零写入。
    const QString canonical = observation->canonicalRoot();
    if (canonical.isEmpty()) {
        return refuse(plan, code("destination-unavailable"));
    }
    if (canonical != plan.destinationRoot) {
        return refuse(plan, code("destination-invalid"));
    }
    switch (observation->nodeKind(QString())) {
    case Observation::NodeKind::Unavailable:
        return refuse(plan, code("destination-unavailable"));
    case Observation::NodeKind::Symlink:
        return refuse(plan, code("root-symlink"));
    case Observation::NodeKind::Directory:
        break;
    case Observation::NodeKind::Missing:
    case Observation::NodeKind::File:
    case Observation::NodeKind::Other:
        return refuse(plan, code("destination-invalid"));
    }
    for (const Operation &operation : plan.operations) {
        if (!reobserveOperation(observation, operation, &error)) {
            return refuse(plan, error);
        }
    }

    // 执行：目录创建在前、文件写入在后，保持计划顺序。任何失败立即停下，绝不越过失败
    // 点继续；不做自动回滚——恢复前的目标状态没有在这里被捕获，发明它就是第二次未经
    // 复核的变更。已完成的操作逐条如实报告，反向恢复因此可能。
    ExtensionStagingRestoreExecutionResult result;
    result.planIdentity = plan.planIdentity;
    result.treeIdentity = plan.treeIdentity;
    int fileIndex = 0;
    for (int index = 0; index < plan.operations.size(); ++index) {
        const Operation &operation = plan.operations.at(index);
        ExtensionStagingRestoreOperationResult entry;
        entry.directory = operation.directory;
        entry.relativePath = operation.relativePath;

        // 纵深防御：逐条操作在执行前重查包含性，写入只落在计划的精确路径上。
        if (!containedRelativePath(operation.relativePath)) {
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
            entry.errorCode = code("path-escapes-destination");
            result.operations.append(entry);
            break;
        }

        if (operation.directory) {
            if (operation.alreadyInPlace) {
                // 跳过并复核：目录必须仍是目录。
                if (observation->nodeKind(operation.relativePath)
                        != Observation::NodeKind::Directory) {
                    entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
                    entry.errorCode = code("destination-drift");
                    result.operations.append(entry);
                    break;
                }
                entry.outcome =
                    ExtensionStagingRestoreOperationOutcome::SkippedVerified;
                ++result.skippedVerifiedCount;
                result.operations.append(entry);
                continue;
            }
            if (!reobserveOperation(observation, operation, &error)) {
                entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
                entry.errorCode = error;
                result.operations.append(entry);
                break;
            }
            if (!QDir(plan.destinationRoot).mkpath(operation.relativePath)
                    || observation->nodeKind(operation.relativePath)
                        != Observation::NodeKind::Directory) {
                entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
                entry.errorCode = code("directory-create-failed");
                result.operations.append(entry);
                break;
            }
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Done;
            ++result.doneCount;
            result.operations.append(entry);
            continue;
        }

        const ExtensionTreeCaptureEntry &source = *fileEntries.at(fileIndex);
        ++fileIndex;
        if (operation.alreadyInPlace) {
            // 跳过并复核：验证永不被静默跳过；漂移是冲突而不是跳过。
            if (!reobserveOperation(observation, operation, &error)) {
                entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
                entry.errorCode = error;
                result.operations.append(entry);
                break;
            }
            entry.outcome =
                ExtensionStagingRestoreOperationOutcome::SkippedVerified;
            ++result.skippedVerifiedCount;
            result.operations.append(entry);
            continue;
        }

        if (!reobserveOperation(observation, operation, &error)) {
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
            entry.errorCode = error;
            result.operations.append(entry);
            break;
        }
        const QString absolute = plan.destinationRoot + QLatin1Char('/')
            + operation.relativePath;
        const QString parent = QFileInfo(absolute).absolutePath();
        if (parent != plan.destinationRoot && !QDir().mkpath(parent)) {
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
            entry.errorCode = code("write-failed");
            result.operations.append(entry);
            break;
        }
        // 原子写：QSaveFile 是 ToolManager 配置写入的既有惯例——临时文件加提交重命名，
        // 失败时目标路径不留半截字节。
        QSaveFile output(absolute);
        bool written = output.open(QIODevice::WriteOnly)
            && output.write(source.bytes) == source.bytes.size();
        if (written) written = output.commit();
        if (!written) {
            output.cancelWriting();
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
            entry.errorCode = code("write-failed");
            result.operations.append(entry);
            break;
        }
        // 写后验证：重读重哈希，摘要不符即执行失败。
        QByteArray reread;
        if (!observation->fileContent(operation.relativePath, &reread)
                || reread.size() != operation.byteCount
                || sha256Hex(reread) != operation.sha256) {
            entry.outcome = ExtensionStagingRestoreOperationOutcome::Failed;
            entry.errorCode = code("post-write-mismatch");
            result.operations.append(entry);
            break;
        }
        entry.outcome = ExtensionStagingRestoreOperationOutcome::Done;
        ++result.doneCount;
        result.operations.append(entry);
    }

    // 失败点之后的操作如实报告为未触达。
    for (int index = result.operations.size(); index < plan.operations.size();
         ++index) {
        ExtensionStagingRestoreOperationResult entry;
        entry.directory = plan.operations.at(index).directory;
        entry.relativePath = plan.operations.at(index).relativePath;
        result.operations.append(entry);
    }
    const int lastIndex = result.operations.size() - 1;
    const bool failed = lastIndex >= 0
        && result.operations.at(lastIndex).outcome
            == ExtensionStagingRestoreOperationOutcome::Failed;
    if (!failed) {
        result.state = ExtensionStagingRestoreExecutionState::Complete;
        result.errorCode.clear();
        result.failureIndex = -1;
        return result;
    }
    result.failedCount = 1;
    result.failureIndex = lastIndex;
    result.errorCode = result.operations.at(lastIndex).errorCode;
    // 混合状态必须可与完整恢复区分：有已完成操作即 Partial，否则 NotStarted。
    result.state = (result.doneCount + result.skippedVerifiedCount) > 0
        ? ExtensionStagingRestoreExecutionState::Partial
        : ExtensionStagingRestoreExecutionState::NotStarted;
    return result;
}
