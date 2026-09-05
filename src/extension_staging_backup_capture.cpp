#include "extension_staging_backup_capture.h"

#include "extension_staging_snapshot.h"
#include "skill_extension_inventory.h"

#include <QDateTime>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUuid>
#include <QVector>

namespace {

const QString kErrorPrefix = QStringLiteral("extension-staging-capture");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

// 暂存域备份 id：时间戳加随机后缀，形状由域的 backup-id 语法固定。同一秒的重复捕获
// 靠随机后缀分开；万一撞上，存储以 `extension-staging-backup-already-exists` 原样拒绝，
// 绝不覆盖既有备份。
QString newBackupId(const QDateTime &createdAt)
{
    return QStringLiteral("ext_%1_%2")
        .arg(createdAt.toString(QStringLiteral("yyyyMMdd_HHmmss")),
             QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
}

// 种类到捕获域的封闭映射。返回 false 时 error 携带独立诊断。语法层面不存在第四种
// 种类，但映射不依赖语法守住边界：语法将来放宽而这里未登记时，以 kind-unmapped
// 失败关闭，而不是落到某个默认域。
bool captureDomainForSubject(const QString &subject,
                             ExtensionTreeCaptureDomain *domain,
                             QString *error)
{
    if (subject.startsWith(QStringLiteral("skill:"))) {
        *domain = SkillExtensionInventory::treeCaptureDomain();
        return true;
    }
    // Codex 插件经捕获的 CLI 输出进入，MCP 经设置 JSON 进入：两者都不是这一层可以
    // 假装去捕获的树，各以独立诊断拒绝。
    if (subject.startsWith(QStringLiteral("codex-plugin:"))) {
        setError(error, code("codex-plugin-without-tree-source"));
        return false;
    }
    if (subject.startsWith(QStringLiteral("mcp:"))) {
        setError(error, code("mcp-without-tree-source"));
        return false;
    }
    setError(error, code("kind-unmapped"));
    return false;
}

// 与该主体最近一次既有备份的内容身份比对。只读：一次该主体的清点，加上对最近备份的
// 完整读回验证——身份只能由验证器重建的树重算，绝不自行解析清单。任何一步不可得都
// 是显式 Unknown 加降级诊断，绝不静默变成"没有既有备份"。
void applyPriorIdentity(ConfigurationBackupStore &store,
                        const QString &subject,
                        const ExtensionTreeCaptureDomain &captureDomain,
                        const QString &treeIdentity,
                        ExtensionStagingBackupCaptureResult *result)
{
    const ConfigurationBackupInventoryResult inventory =
        store.inventory(subject, 0, {});
    if (inventory.state == ConfigurationBackupInventoryState::Empty
            || (inventory.state == ConfigurationBackupInventoryState::Ready
                && inventory.entries.isEmpty())) {
        result->priorIdentity = ExtensionStagingPriorIdentity::NoPriorBackup;
        return;
    }
    if (inventory.state != ConfigurationBackupInventoryState::Ready) {
        result->priorIdentity = ExtensionStagingPriorIdentity::Unknown;
        result->priorIdentityDiagnostic = code("prior-identity-degraded");
        return;
    }
    ConfigurationBackupSnapshot prior;
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    QString ignored;
    if (!store.read(subject, inventory.entries.first().backupId, &prior, &ignored)
            || !ExtensionStagingSnapshot::verify(captureDomain, subject, prior,
                                                 &rebuilt, &ignored)) {
        result->priorIdentity = ExtensionStagingPriorIdentity::Unknown;
        result->priorIdentityDiagnostic = code("prior-identity-degraded");
        return;
    }
    const QString priorIdentity =
        ExtensionTreeCapture::contentIdentity(captureDomain, rebuilt);
    if (priorIdentity.isEmpty()) {
        result->priorIdentity = ExtensionStagingPriorIdentity::Unknown;
        result->priorIdentityDiagnostic = code("prior-identity-degraded");
        return;
    }
    result->priorIdentity = priorIdentity == treeIdentity
        ? ExtensionStagingPriorIdentity::Matched
        : ExtensionStagingPriorIdentity::Mismatched;
}

} // namespace

bool ExtensionStagingBackupCapture::capture(
        const QString &subject, const QString &sourceRoot,
        const QString &backupRoot, ConfigurationBackupKeyProvider *keyProvider,
        ExtensionStagingBackupCaptureResult *result, QString *error)
{
    if (error) error->clear();
    if (result) *result = ExtensionStagingBackupCaptureResult();
    if (!result) return false;

    // 主体语法先于一切文件系统工作：一个畸形主体连来源根都不该被触碰。
    const ConfigurationBackupStoreDomain staging =
        ConfigurationBackupStore::extensionStagingDomain();
    const QRegularExpression subjectPattern(staging.subjectPattern);
    if (!subjectPattern.match(subject).hasMatch()) {
        setError(error, code("subject-invalid"));
        return false;
    }
    ExtensionTreeCaptureDomain captureDomain;
    if (!captureDomainForSubject(subject, &captureDomain, error)) return false;
    if (!keyProvider || backupRoot.isEmpty()) {
        setError(error, code("request-invalid"));
        return false;
    }
    if (sourceRoot.isEmpty()) {
        setError(error, code("root-invalid"));
        return false;
    }

    // 来源根的符号链接必须在规范化之前拒绝：canonicalFilePath 会静默解析它，而解析
    // 之后捕获层看到的就不再是调用方指给它的那个位置。其余包含性与漂移纪律由捕获层
    // 在扫描内部完成。
    const QFileInfo sourceInfo(sourceRoot);
    if (sourceInfo.isSymLink()) {
        setError(error, code("root-symlink"));
        return false;
    }
    const QString canonicalRoot = sourceInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) {
        setError(error, code("root-unavailable"));
        return false;
    }

    QVector<ExtensionTreeCaptureEntry> tree;
    ExtensionTreeCaptureBudget budget;
    ExtensionTreeCaptureError captureError;
    if (!ExtensionTreeCapture::scanDirectory(
            captureDomain, canonicalRoot, canonicalRoot, QString(), 0, &budget,
            &tree, &captureError)) {
        // 捕获层的诊断逐字透传：调用方按那些代号理解失败，另造本地代号会让同一个
        // 失败在两条路径上有两个名字。
        setError(error, captureError.errorCode);
        return false;
    }
    const QString treeIdentity =
        ExtensionTreeCapture::contentIdentity(captureDomain, tree);
    if (treeIdentity.isEmpty()) {
        setError(error, code("identity-unavailable"));
        return false;
    }

    const QDateTime createdAt = QDateTime::currentDateTimeUtc();
    const QString backupId = newBackupId(createdAt);
    ConfigurationBackupSnapshot snapshot;
    if (!ExtensionStagingSnapshot::build(captureDomain, tree, subject, backupId,
                                         createdAt, &snapshot, error)) {
        return false;
    }

    ConfigurationBackupStore store(staging, backupRoot, keyProvider);
    ExtensionStagingBackupCaptureResult built;
    built.backupId = backupId;
    built.subject = subject;
    built.treeIdentity = treeIdentity;
    // 先查先备：写入之前完成该主体的既有身份比对。清点只是建议性输入，降级不阻断
    // 写入——存储清点坏了的时候恰恰最不该丢备份。
    applyPriorIdentity(store, subject, captureDomain, treeIdentity, &built);

    // 存储的 create 是备份目录级原子的：加锁、原子写、写后重读重解析复核，
    // 任何失败都回收整个备份目录。失败路径上没有本组件需要自己清理的残留。
    if (!store.create(snapshot, error)) return false;

    // 写入成功后取回清单身份：审计按它把备份绑到确切的树。这次清点同样可能退化；
    // 退化时身份留空并携带独立诊断，备份本身完整在盘上且可按 id 直接读回。
    const ConfigurationBackupInventoryResult after = store.inventory(subject, 0, {});
    if (after.state == ConfigurationBackupInventoryState::Ready) {
        for (const ConfigurationBackupInventoryEntry &entry : after.entries) {
            if (entry.backupId == backupId) {
                built.manifestIdentity = entry.identity;
                built.manifestIdentityKnown = true;
                break;
            }
        }
    }
    if (!built.manifestIdentityKnown) {
        built.manifestIdentityDiagnostic = code("manifest-identity-degraded");
    }
    *result = built;
    return true;
}
