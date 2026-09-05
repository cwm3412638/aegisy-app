#include "extension_staging_backup_capture.h"

#include "extension_staging_snapshot.h"
#include "mcp_configuration_inventory.h"
#include "skill_extension_inventory.h"

#include <QDateTime>
#include <QFile>
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
    if (subject.startsWith(QStringLiteral("mcp:"))) {
        *domain = McpConfigurationInventory::backupCaptureDomain();
        return true;
    }
    // Codex 插件只有观察没有字节：应用只消费捕获到的 CLI 列表输出，从未打开过
    // 插件的任何文件，也没有任何变更面——这一层没有任何处于自身权威内的东西可以
    // 假装去备份，因此以原诊断拒绝。
    if (subject.startsWith(QStringLiteral("codex-plugin:"))) {
        setError(error, code("codex-plugin-without-tree-source"));
        return false;
    }
    setError(error, code("kind-unmapped"));
    return false;
}

// 读取 MCP 设置文件的原始字节，纪律与 `McpConfigurationInventory::inspectFile` 相同：
// 符号链接在打开之前拒绝，大小先按 stat 上限（1 MiB——比捕获层的 2 MiB 与暂存域的
// 4 MiB 都紧，更紧的一侧获胜）拒绝，打开后仍有界读取，读完重新 stat 比对——读取期间
// 被换掉或改写的文件会让存下的字节对应一份已不在那里的内容。备份的是原始字节本身，
// 刻意不解析 JSON：有效性判定属于清单与恢复路径。文件不存在是独立诊断而不是空备份。
bool readMcpSourceFile(const QString &path, QByteArray *bytes, QString *error)
{
    const QFileInfo initial(path);
    if (initial.isSymLink()) {
        setError(error, code("mcp-source-symlink"));
        return false;
    }
    if (!initial.exists()) {
        setError(error, code("mcp-source-missing"));
        return false;
    }
    if (!initial.isFile() || initial.size() < 0) {
        setError(error, code("mcp-source-invalid"));
        return false;
    }
    if (initial.size() > McpConfigurationInventory::MaxFileBytes) {
        setError(error, code("mcp-source-oversized"));
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, code("mcp-source-unavailable"));
        return false;
    }
    const QByteArray content =
        file.read(McpConfigurationInventory::MaxFileBytes + 1);
    const bool readFailed = file.error() != QFileDevice::NoError;
    file.close();
    if (readFailed) {
        setError(error, code("mcp-source-unavailable"));
        return false;
    }
    if (content.size() > McpConfigurationInventory::MaxFileBytes) {
        setError(error, code("mcp-source-oversized"));
        return false;
    }
    // 漂移复查：被哈希的字节必须就是被存下的字节。读取期间出现的符号链接、类型变化
    // 与大小变化都以独立诊断失败关闭。
    const QFileInfo finalInfo(path);
    if (finalInfo.isSymLink() || !finalInfo.isFile()
            || finalInfo.size() != content.size()
            || initial.size() != content.size()) {
        setError(error, code("mcp-source-drift"));
        return false;
    }
    *bytes = content;
    return true;
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

    QVector<ExtensionTreeCaptureEntry> tree;
    if (subject.startsWith(QStringLiteral("mcp:"))) {
        // MCP 的诚实备份单元是整个设置文件的原始字节：文件同时是来源身份与变更的单位，
        // 且被所有 `mcp:` 主体共享。读出字节后合成一棵固定单条目树——相对路径恒为字面量
        // `settings.json`，绝不从调用方的文件名推导，因此清单形状与文件实际住处无关。
        QByteArray bytes;
        if (!readMcpSourceFile(sourceRoot, &bytes, error)) return false;
        ExtensionTreeCaptureEntry entry;
        entry.relativePath = QStringLiteral("settings.json");
        entry.directory = false;
        entry.bytes = bytes;
        tree.append(entry);
    } else {
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
    // MCP 备份覆盖的是整个共享设置文件而不只是该主体的服务器条目——这一点必须让
    // 调用方看得见，因为恢复语义同样是整文件。
    built.coversSharedSettingsFile = subject.startsWith(QStringLiteral("mcp:"));
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
