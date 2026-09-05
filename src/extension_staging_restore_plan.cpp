#include "extension_staging_restore_plan.h"

#include <QCryptographicHash>
#include <QFileInfo>

namespace {

const QString kErrorPrefix = QStringLiteral("extension-staging-restore");
const QString kIdentityPrefix =
    QStringLiteral("extension-staging-restore-plan:sha256:");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

// 纵深防御：清单已被验证，但计划层在把任何路径与目标根拼接之前仍按同一套逐段规则重查
// 一次。任何 traversal 形状的路径（`..`、绝对路径、空段）都在这里失败关闭，而不是靠
// "上游已经查过"。
bool containedRelativePath(const QString &path)
{
    if (path.isEmpty() || path.toUtf8().size() > 4096) return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!ExtensionTreeCapture::safeEntryName(segment)) return false;
    }
    return true;
}

using Observation = ExtensionStagingRestoreObservation;

// 观察一条计划路径的每一个前缀组件（不含路径本身）。符号链接在任何组件上都是独立拒绝：
// 跟随它会让写入落到目标根之外，而根外的内容不在任何人批准的恢复范围里。
bool checkAncestors(Observation *observation, const QString &path,
                    QString *error)
{
    const QStringList segments = path.split(QLatin1Char('/'));
    QString prefix;
    for (int index = 0; index + 1 < segments.size(); ++index) {
        prefix += segments.at(index);
        switch (observation->nodeKind(prefix)) {
        case Observation::NodeKind::Unavailable:
            setError(error, code("destination-unavailable"));
            return false;
        case Observation::NodeKind::Symlink:
            setError(error, code("symlink-component"));
            return false;
        case Observation::NodeKind::File:
        case Observation::NodeKind::Other:
            // 中间组件被非目录占用：计划中的祖先目录无法成立，这是与计划内容不符的
            // 已有状态，按冲突拒绝而不是让执行侧走到一半才发现。
            setError(error, code("destination-conflict"));
            return false;
        case Observation::NodeKind::Missing:
        case Observation::NodeKind::Directory:
            break;
        }
        prefix += QLatin1Char('/');
    }
    return true;
}

} // namespace

bool ExtensionStagingRestorePlanBuilder::plan(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QString &expectedSubject,
        const ConfigurationBackupSnapshot &snapshot,
        const QString &destinationRoot,
        ExtensionStagingRestoreObservation *observation,
        ExtensionStagingRestorePlan *plan,
        QString *error)
{
    if (error) error->clear();
    if (plan) *plan = ExtensionStagingRestorePlan();
    if (!plan || !observation) {
        setError(error, code("destination-unavailable"));
        return false;
    }

    // 第一道门禁：快照验证。计划层只消费验证侧重建出的树，绝不自行解析未验证字节；
    // 验证诊断原样透传，篡改过的快照在计划开始前就失败关闭。
    QVector<ExtensionTreeCaptureEntry> tree;
    if (!ExtensionStagingSnapshot::verify(captureDomain, expectedSubject,
                                          snapshot, &tree, error)) {
        return false;
    }

    // 目标根语法：空、相对、或与平台规范化形式不一致的目标一律拒绝。规范化形式只能来自
    // 注入的观察接口；观察给不出时目标状态未知，拒绝而不是盲计划。
    if (destinationRoot.isEmpty()
            || QFileInfo(destinationRoot).isRelative()) {
        setError(error, code("destination-invalid"));
        return false;
    }
    const QString canonical = observation->canonicalRoot();
    if (canonical.isEmpty()) {
        setError(error, code("destination-unavailable"));
        return false;
    }
    if (canonical != destinationRoot) {
        setError(error, code("destination-invalid"));
        return false;
    }

    // 目标根本身必须是一个真实目录：符号链接目标根单独诊断，其余形状都不是可恢复的
    // 目标。
    switch (observation->nodeKind(QString())) {
    case Observation::NodeKind::Unavailable:
        setError(error, code("destination-unavailable"));
        return false;
    case Observation::NodeKind::Symlink:
        setError(error, code("root-symlink"));
        return false;
    case Observation::NodeKind::Directory:
        break;
    case Observation::NodeKind::Missing:
    case Observation::NodeKind::File:
    case Observation::NodeKind::Other:
        setError(error, code("destination-invalid"));
        return false;
    }

    // 上限重查（纵深防御）：文件数、单文件字节与聚合字节都按暂存域上限再守一次。验证层
    // 守住条目形状与逐槽摘要，但并不单独守聚合字节——存储层在写入时才守它，而一份计划
    // 必须在产出任何决定之前就知道自己落在域内。
    const ConfigurationBackupStoreDomain staging =
        ConfigurationBackupStore::extensionStagingDomain();
    int fileCount = 0;
    qint64 aggregate = 0;
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (!containedRelativePath(entry.relativePath)) {
            setError(error, code("path-escapes-destination"));
            return false;
        }
        if (entry.directory) continue;
        ++fileCount;
        if (fileCount > staging.maxFiles - 1
                || entry.bytes.size() > staging.maxFileBytes
                || aggregate > staging.maxPayloadBytes - entry.bytes.size()) {
            setError(error, code("bounds-exceeded"));
            return false;
        }
        aggregate += entry.bytes.size();
    }

    const QString treeIdentity =
        ExtensionTreeCapture::contentIdentity(captureDomain, tree);
    if (treeIdentity.isEmpty()) {
        setError(error, code("identity-unavailable"));
        return false;
    }

    // 逐条对照目标现状。目录创建排前、文件写入排后，各自保持清单顺序。
    ExtensionStagingRestorePlan result;
    result.destinationRoot = canonical;
    result.subject = expectedSubject;
    result.treeIdentity = treeIdentity;
    QVector<ExtensionStagingRestoreOperation> directoryOps;
    QVector<ExtensionStagingRestoreOperation> fileOps;
    int slot = 1;
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (!checkAncestors(observation, entry.relativePath, error)) {
            return false;
        }
        ExtensionStagingRestoreOperation operation;
        operation.directory = entry.directory;
        operation.relativePath = entry.relativePath;
        const Observation::NodeKind kind =
            observation->nodeKind(entry.relativePath);
        if (entry.directory) {
            switch (kind) {
            case Observation::NodeKind::Unavailable:
                setError(error, code("destination-unavailable"));
                return false;
            case Observation::NodeKind::Symlink:
                setError(error, code("symlink-component"));
                return false;
            case Observation::NodeKind::File:
            case Observation::NodeKind::Other:
                setError(error, code("destination-conflict"));
                return false;
            case Observation::NodeKind::Missing:
                break;
            case Observation::NodeKind::Directory:
                operation.alreadyInPlace = true;
                break;
            }
            directoryOps.append(operation);
            continue;
        }
        operation.byteCount = entry.bytes.size();
        operation.sha256 = sha256Hex(entry.bytes);
        operation.sourceSlot = slot;
        ++slot;
        switch (kind) {
        case Observation::NodeKind::Unavailable:
            setError(error, code("destination-unavailable"));
            return false;
        case Observation::NodeKind::Symlink:
            setError(error, code("symlink-component"));
            return false;
        case Observation::NodeKind::Directory:
        case Observation::NodeKind::Other:
            setError(error, code("destination-conflict"));
            return false;
        case Observation::NodeKind::Missing:
            break;
        case Observation::NodeKind::File: {
            // 已有文件：读出现状并与计划内容逐字节比对。读不出来即目标未知，拒绝而不是
            // 盲计划；内容不符是冲突，绝不静默覆盖；逐字节一致才是 already-in-place。
            QByteArray existing;
            if (!observation->fileContent(entry.relativePath, &existing)) {
                setError(error, code("destination-unavailable"));
                return false;
            }
            if (existing.size() != entry.bytes.size()
                    || sha256Hex(existing) != operation.sha256) {
                setError(error, code("destination-conflict"));
                return false;
            }
            operation.alreadyInPlace = true;
            break;
        }
        }
        fileOps.append(operation);
    }
    result.operations = directoryOps + fileOps;

    // 计划身份：目标根、主体、树身份与每一条操作经长度分帧摘要绑定。少绑任何一项，同
    // 一份身份就能对两棵不同的树或两个不同的目标成立。
    QList<QByteArray> parts;
    parts.append(canonical.toUtf8());
    parts.append(expectedSubject.toUtf8());
    parts.append(treeIdentity.toUtf8());
    for (const ExtensionStagingRestoreOperation &operation : result.operations) {
        parts.append(operation.directory ? QByteArrayLiteral("directory")
                                         : QByteArrayLiteral("file"));
        parts.append(operation.relativePath.toUtf8());
        if (operation.directory) {
            parts.append(operation.alreadyInPlace
                             ? QByteArrayLiteral("present")
                             : QByteArrayLiteral("create"));
            continue;
        }
        parts.append(QByteArray::number(operation.byteCount));
        parts.append(operation.sha256.toUtf8());
        parts.append(QByteArray::number(operation.sourceSlot));
        parts.append(operation.alreadyInPlace ? QByteArrayLiteral("in-place")
                                              : QByteArrayLiteral("write"));
    }
    result.planIdentity = ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-extension-staging-restore-plan/0.1\0"),
        parts, kIdentityPrefix);
    if (result.planIdentity.isEmpty()) {
        setError(error, code("identity-unavailable"));
        return false;
    }
    *plan = result;
    return true;
}
