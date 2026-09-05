#include "extension_staging_snapshot.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace {

const QString kManifestFormat =
    QStringLiteral("aegisy-extension-staging-snapshot-manifest/0.1");
constexpr int kManifestVersion = 1;
const QString kErrorPrefix = QStringLiteral("extension-staging-snapshot");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

bool exactKeys(const QJsonObject &object, const QStringList &expected)
{
    QStringList actual = object.keys();
    QStringList wanted = expected;
    actual.sort();
    wanted.sort();
    return actual == wanted;
}

bool exactInteger(const QJsonValue &value, int minimum, int maximum, int *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < minimum || number > maximum) {
        return false;
    }
    if (result) *result = static_cast<int>(number);
    return true;
}

bool isHexDigest(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        const ushort unicode = character.unicode();
        const bool digit = unicode >= '0' && unicode <= '9';
        const bool lower = unicode >= 'a' && unicode <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

// 清单路径必须与捕获层接受过的形状一致：非空、长度受限、逐段安全。逐段校验同时拒绝
// `..`、空段（绝对路径与连续分隔符的产物）、控制字符、反斜杠与冒号。
bool validRelativePath(const QString &path)
{
    if (path.isEmpty() || path.toUtf8().size() > 4096) return false;
    const QStringList segments = path.split(QLatin1Char('/'));
    for (const QString &segment : segments) {
        if (!ExtensionTreeCapture::safeEntryName(segment)) return false;
    }
    return true;
}

// 清单文档占用槽 0，而槽 0 在存储层是一份普通文件，因此它同时受域的单槽上限约束；取两者
// 中更紧的那个。一个超过单槽上限的清单永远无法从存储里读回来，构建侧必须提前拒绝。
qint64 effectiveManifestBytesBound(const ConfigurationBackupStoreDomain &domain)
{
    return qMin(domain.maxFileBytes, domain.maxManifestBytes);
}

QString sha256Hex(const QByteArray &content)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
}

// 暂存域的主体语法直接取自域定义，而不是在这里复制一份正则：两份语法会各自漂移，而漂移
// 的方向是这一层放行存储会拒绝（或反过来）的主体。
bool validSubject(const ConfigurationBackupStoreDomain &domain,
                  const QString &subject)
{
    const QRegularExpression pattern(domain.subjectPattern);
    return pattern.match(subject).hasMatch();
}

// 存储会把整个载荷序列化成一份 JSON 再加密，并要求它不超过域的清单上限。这里按序列化
// 规则保守上界估算（base64 膨胀 4/3，每槽固定开销按 192 字节计），超出即在产出任何字节
// 之前拒绝，而不是让存储在写入时才失败。
qint64 serializedPayloadBound(const QList<qint64> &slotBytes)
{
    qint64 bound = 96;
    for (const qint64 size : slotBytes) {
        bound += 192 + 4 * ((size + 2) / 3);
    }
    return bound;
}

QJsonObject fileEntryJson(const QString &path, int slot, const QByteArray &bytes)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("byte_count"),
                 static_cast<int>(bytes.size()));
    entry.insert(QStringLiteral("kind"), QStringLiteral("file"));
    entry.insert(QStringLiteral("path"), path);
    entry.insert(QStringLiteral("sha256"), sha256Hex(bytes));
    entry.insert(QStringLiteral("slot"), slot);
    return entry;
}

QJsonObject directoryEntryJson(const QString &path)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("kind"), QStringLiteral("directory"));
    entry.insert(QStringLiteral("path"), path);
    return entry;
}

// 清单里的一条文件条目。仅用于验证路径上的中间结果。
struct ManifestFileEntry {
    QString path;
    int slot = 0;
    int byteCount = 0;
    QString sha256;
};

} // namespace

QString ExtensionStagingSnapshot::manifestFormat()
{
    return kManifestFormat;
}

bool ExtensionStagingSnapshot::build(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QVector<ExtensionTreeCaptureEntry> &tree,
        const QString &subject,
        const QString &backupId,
        const QDateTime &createdAt,
        ConfigurationBackupSnapshot *snapshot,
        QString *error)
{
    if (error) error->clear();
    if (snapshot) *snapshot = ConfigurationBackupSnapshot();
    if (!snapshot) return false;
    // 未配置的捕获域会让树身份退化成空串；在任何工作之前拒绝。
    if (!captureDomain.configured()) {
        setError(error, code("domain-unconfigured"));
        return false;
    }
    const ConfigurationBackupStoreDomain staging =
        ConfigurationBackupStore::extensionStagingDomain();
    // 主体在任何捕获结果被触碰之前校验：一个畸形主体的快照即使造出来也无法通过存储的
    // 主体校验，提前拒绝让它连构建这一步都走不到。
    if (!validSubject(staging, subject)) {
        setError(error, code("subject-invalid"));
        return false;
    }
    const QRegularExpression backupIdPattern(staging.backupIdPattern);
    if (!backupIdPattern.match(backupId).hasMatch() || !createdAt.isValid()) {
        setError(error, code("metadata-invalid"));
        return false;
    }

    // 条目数沿用捕获层上限：清单文档允许表达的最大树与捕获层允许捕获的最大树是同一棵。
    if (tree.size() > ExtensionTreeCapture::MaxEntries) {
        setError(error, code("entry-count-limit"));
        return false;
    }
    // 上限对账：槽 0 已被清单占用，文件至多 maxFiles - 1 个。捕获层放行 4096 条目，因此
    // 一棵捕获层完全接受的树仍可能在这里被拒绝——拒绝而不是截断，因为截断后的快照不再
    // 是那棵树，而树身份是之后一切决定绑定的对象。
    int fileCount = 0;
    qint64 aggregate = 0;
    QSet<QString> seenPaths;
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (!validRelativePath(entry.relativePath)
                || (entry.directory && !entry.bytes.isEmpty())) {
            setError(error, code("entry-invalid"));
            return false;
        }
        if (seenPaths.contains(entry.relativePath)) {
            setError(error, code("path-duplicate"));
            return false;
        }
        seenPaths.insert(entry.relativePath);
        if (entry.directory) continue;
        ++fileCount;
        if (fileCount > staging.maxFiles - 1) {
            setError(error, code("file-count-limit"));
            return false;
        }
        // 捕获层的单文件上限（2 MiB）已经更紧，但输入可能不是捕获层产出的；域的单槽上限
        // 在这里再守一次。
        if (entry.bytes.size() > staging.maxFileBytes) {
            setError(error, code("file-oversized"));
            return false;
        }
        if (aggregate > staging.maxPayloadBytes - entry.bytes.size()) {
            setError(error, code("payload-oversized"));
            return false;
        }
        aggregate += entry.bytes.size();
    }

    // 树身份用调用方的捕获域计算并原样写进清单；验证侧用同一个域重算，用错域即完整性
    // 失败。
    const QString identity =
        ExtensionTreeCapture::contentIdentity(captureDomain, tree);
    if (identity.isEmpty()) {
        setError(error, code("identity-unavailable"));
        return false;
    }

    QJsonArray entries;
    int slot = 1;
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (entry.directory) {
            entries.append(directoryEntryJson(entry.relativePath));
        } else {
            entries.append(fileEntryJson(entry.relativePath, slot, entry.bytes));
            ++slot;
        }
    }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("entries"), entries);
    manifest.insert(QStringLiteral("file_count"), fileCount);
    manifest.insert(QStringLiteral("format"), kManifestFormat);
    manifest.insert(QStringLiteral("identity"), identity);
    manifest.insert(QStringLiteral("subject"), subject);
    manifest.insert(QStringLiteral("version"), kManifestVersion);
    const QByteArray manifestBytes =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (manifestBytes.isEmpty()
            || manifestBytes.size() > effectiveManifestBytesBound(staging)) {
        setError(error, code("manifest-oversized"));
        return false;
    }

    // 序列化载荷的保守上界必须在产出快照之前就成立。
    QList<qint64> slotBytes;
    slotBytes.append(manifestBytes.size());
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (!entry.directory) slotBytes.append(entry.bytes.size());
    }
    if (serializedPayloadBound(slotBytes) > staging.maxManifestBytes) {
        setError(error, code("payload-oversized"));
        return false;
    }

    ConfigurationBackupSnapshot result;
    result.backupId = backupId;
    result.tool = subject;
    result.createdAt = createdAt;
    result.files.append(ConfigurationBackupFile{0, true, manifestBytes});
    slot = 1;
    for (const ExtensionTreeCaptureEntry &entry : tree) {
        if (entry.directory) continue;
        result.files.append(ConfigurationBackupFile{slot, true, entry.bytes});
        ++slot;
    }
    *snapshot = result;
    return true;
}

bool ExtensionStagingSnapshot::verify(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QString &expectedSubject,
        const ConfigurationBackupSnapshot &snapshot,
        QString *error)
{
    return verify(captureDomain, expectedSubject, snapshot, nullptr, error);
}

bool ExtensionStagingSnapshot::verify(
        const ExtensionTreeCaptureDomain &captureDomain,
        const QString &expectedSubject,
        const ConfigurationBackupSnapshot &snapshot,
        QVector<ExtensionTreeCaptureEntry> *rebuiltTree,
        QString *error)
{
    if (error) error->clear();
    if (rebuiltTree) rebuiltTree->clear();
    if (!captureDomain.configured()) {
        setError(error, code("domain-unconfigured"));
        return false;
    }
    const ConfigurationBackupStoreDomain staging =
        ConfigurationBackupStore::extensionStagingDomain();
    if (!validSubject(staging, expectedSubject)) {
        setError(error, code("subject-invalid"));
        return false;
    }
    // 槽 0 必须存在且承载清单；一份没有清单槽的快照连"它声称是哪棵树"都无法回答。
    if (snapshot.files.isEmpty() || snapshot.files.at(0).slot != 0
            || !snapshot.files.at(0).existed
            || snapshot.files.at(0).content.isEmpty()) {
        setError(error, code("slot-mismatch"));
        return false;
    }
    const QByteArray manifestBytes = snapshot.files.at(0).content;
    if (manifestBytes.size() > effectiveManifestBytesBound(staging)) {
        setError(error, code("manifest-oversized"));
        return false;
    }
    // NUL 字节单独诊断：它几乎总意味着清单被二进制内容替换或截断，而不是 JSON 写坏了。
    if (manifestBytes.contains('\0')) {
        setError(error, code("manifest-encoding"));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, code("manifest-parse"));
        return false;
    }
    // 非规范化 JSON（多余空白、键序不同、重复键被解析层吞掉）一律拒绝：清单的规范字节
    // 就是构建侧写出的字节，两种读法的文档没有资格进入完整性校验。
    if (document.toJson(QJsonDocument::Compact) != manifestBytes) {
        setError(error, code("manifest-canonical"));
        return false;
    }
    const QJsonObject manifest = document.object();
    if (!exactKeys(manifest, {
            QStringLiteral("entries"), QStringLiteral("file_count"),
            QStringLiteral("format"), QStringLiteral("identity"),
            QStringLiteral("subject"), QStringLiteral("version") })
            || manifest.value(QStringLiteral("format")).toString() != kManifestFormat
            || !exactInteger(manifest.value(QStringLiteral("version")),
                             kManifestVersion, kManifestVersion, nullptr)
            || !manifest.value(QStringLiteral("identity")).isString()
            || !manifest.value(QStringLiteral("subject")).isString()
            || !manifest.value(QStringLiteral("entries")).isArray()) {
        setError(error, code("manifest-shape"));
        return false;
    }
    // 清单主体必须自身合法且与期望主体、快照主体逐字节一致；三者任何一对不符都意味着
    // 这份快照说的不是它声称的那个扩展。
    const QString subject = manifest.value(QStringLiteral("subject")).toString();
    if (!validSubject(staging, subject)) {
        setError(error, code("subject-invalid"));
        return false;
    }
    if (subject != expectedSubject || subject != snapshot.tool) {
        setError(error, code("subject-mismatch"));
        return false;
    }
    const QString identity = manifest.value(QStringLiteral("identity")).toString();
    if (!identity.startsWith(captureDomain.identityPrefix)
            || !isHexDigest(identity.mid(captureDomain.identityPrefix.size()))) {
        setError(error, code("manifest-shape"));
        return false;
    }
    const QJsonArray entries = manifest.value(QStringLiteral("entries")).toArray();
    if (entries.size() > ExtensionTreeCapture::MaxEntries) {
        setError(error, code("entry-count-limit"));
        return false;
    }
    int fileCount = 0;
    if (!exactInteger(manifest.value(QStringLiteral("file_count")), 0,
                      staging.maxFiles - 1, &fileCount)) {
        setError(error, code("manifest-shape"));
        return false;
    }

    QList<ManifestFileEntry> fileEntries;
    // 重建计划按清单顺序保留全部条目（文件先记空字节），第二个循环再把槽内容按文件序号
    // 填回去。分两个循环做校验，但树身份的输入必须与构建侧是同一种顺序。
    QVector<ExtensionTreeCaptureEntry> rebuilt;
    QList<int> rebuiltFilePositions;
    QSet<QString> seenPaths;
    for (const QJsonValue &value : entries) {
        if (!value.isObject()) {
            setError(error, code("manifest-shape"));
            return false;
        }
        const QJsonObject entry = value.toObject();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const bool isFile = kind == QStringLiteral("file");
        const bool isDirectory = kind == QStringLiteral("directory");
        if ((!isFile && !isDirectory)
                || (isFile && !exactKeys(entry, {
                        QStringLiteral("byte_count"), QStringLiteral("kind"),
                        QStringLiteral("path"), QStringLiteral("sha256"),
                        QStringLiteral("slot") }))
                || (isDirectory && !exactKeys(entry, {
                        QStringLiteral("kind"), QStringLiteral("path") }))
                || !entry.value(QStringLiteral("path")).isString()) {
            setError(error, code("manifest-shape"));
            return false;
        }
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (!validRelativePath(path)) {
            setError(error, code("path-invalid"));
            return false;
        }
        if (seenPaths.contains(path)) {
            setError(error, code("path-duplicate"));
            return false;
        }
        seenPaths.insert(path);
        if (isDirectory) {
            rebuilt.append(ExtensionTreeCaptureEntry{path, true, {}});
            continue;
        }
        ManifestFileEntry fileEntry;
        fileEntry.path = path;
        // 槽位号必须就是它在文件序列里的位置：任何重排或缺槽都在这里失败，而不是等到
        // 摘要不符。
        if (!exactInteger(entry.value(QStringLiteral("byte_count")), 0,
                          static_cast<int>(staging.maxFileBytes),
                          &fileEntry.byteCount)
                || !entry.value(QStringLiteral("sha256")).isString()
                || !isHexDigest(entry.value(QStringLiteral("sha256")).toString())) {
            setError(error, code("manifest-shape"));
            return false;
        }
        fileEntry.sha256 = entry.value(QStringLiteral("sha256")).toString();
        const int expectedSlot = static_cast<int>(fileEntries.size()) + 1;
        if (!exactInteger(entry.value(QStringLiteral("slot")),
                          expectedSlot, expectedSlot, &fileEntry.slot)) {
            setError(error, code("slot-mismatch"));
            return false;
        }
        rebuiltFilePositions.append(static_cast<int>(rebuilt.size()));
        rebuilt.append(ExtensionTreeCaptureEntry{path, false, {}});
        fileEntries.append(fileEntry);
    }
    // 清单声明的文件数必须与实际列出的文件条目一致。
    if (fileEntries.size() != fileCount
            || snapshot.files.size() != fileCount + 1) {
        setError(error, code("slot-mismatch"));
        return false;
    }
    for (int index = 0; index < fileEntries.size(); ++index) {
        const ManifestFileEntry &fileEntry = fileEntries.at(index);
        const ConfigurationBackupFile &file = snapshot.files.at(index + 1);
        if (file.slot != index + 1 || !file.existed) {
            setError(error, code("slot-mismatch"));
            return false;
        }
        if (file.content.size() != fileEntry.byteCount) {
            setError(error, code("byte-count-mismatch"));
            return false;
        }
        // 每个文件槽的字节都必须散列回清单声明的值；少一次比较，被改动的槽就能带着
        // 一份正确的树身份通过验证。
        if (sha256Hex(file.content) != fileEntry.sha256) {
            setError(error, code("content-digest-mismatch"));
            return false;
        }
        rebuilt[rebuiltFilePositions.at(index)].bytes = file.content;
    }
    // 最后重算整树身份。它把条目顺序、目录结构、每条路径与每份字节绑在一起：前面任何
    // 一项被以彼此一致的方式篡改过，身份也不会碰巧对上。
    if (ExtensionTreeCapture::contentIdentity(captureDomain, rebuilt) != identity) {
        setError(error, code("identity-mismatch"));
        return false;
    }
    if (rebuiltTree) *rebuiltTree = rebuilt;
    return true;
}
