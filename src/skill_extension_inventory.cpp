#include "skill_extension_inventory.h"
#include "extension_tree_capture.h"
#include "strict_json_validator.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>

namespace {

constexpr char kManifestName[] = "aegisy-skill.json";
constexpr char kSkillDocumentName[] = "SKILL.md";

} // namespace

// 技能清单的树捕获域。身份域与诊断代码前缀必须与历史摘要字节和诊断串完全一致；树机制
// 本身由共享层持有，这一层不再保留第二份副本。
const ExtensionTreeCaptureDomain &SkillExtensionInventory::treeCaptureDomain()
{
    static const ExtensionTreeCaptureDomain domain{
        QByteArrayLiteral("aegisy-skill-extension-content/0.1\0"),
        QStringLiteral("extension-content:sha256:"),
        QStringLiteral("skill")};
    return domain;
}

namespace {

SkillExtensionInventoryResult failure(SkillExtensionInventoryState state,
                                      const QString &code)
{
    SkillExtensionInventoryResult result;
    result.state = state;
    result.errorCode = code;
    return result;
}

SkillExtensionInventoryResult captureFailure(
    const ExtensionTreeCaptureError &error)
{
    return failure(error.state == ExtensionTreeCaptureErrorState::Unavailable
                       ? SkillExtensionInventoryState::Unavailable
                       : SkillExtensionInventoryState::Invalid,
                   error.errorCode);
}

bool safeScalar(const QString &value, int maximum, bool allowEmpty = false)
{
    if ((!allowEmpty && value.isEmpty()) || value.size() > maximum) return false;
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    return true;
}

bool registrySafeText(const QString &value, int maximum, bool allowEmpty = false)
{
    if (!safeScalar(value, maximum, allowEmpty)) return false;
    const QString lowered = value.toLower();
    return !lowered.contains(QStringLiteral("authorization"))
        && !lowered.contains(QStringLiteral("api_key"))
        && !lowered.contains(QStringLiteral("api-key"))
        && !lowered.contains(QStringLiteral("password"))
        && !lowered.contains(QStringLiteral("secret="))
        && !lowered.contains(QStringLiteral("token="))
        && !QRegularExpression(QStringLiteral("(^|[^a-z0-9])sk-[a-z0-9_-]{8,}"),
                               QRegularExpression::CaseInsensitiveOption)
                .match(value).hasMatch();
}

bool stringArray(const QJsonValue &value,
                 int maximumItems,
                 int maximumLength,
                 QStringList *output)
{
    if (!value.isArray()) return false;
    const QJsonArray array = value.toArray();
    if (array.size() > maximumItems) return false;
    QSet<QString> seen;
    for (const QJsonValue &item : array) {
        if (!item.isString() || !safeScalar(item.toString(), maximumLength)
                || seen.contains(item.toString())) {
            return false;
        }
        seen.insert(item.toString());
        output->append(item.toString());
    }
    return true;
}

bool parseManifest(const QByteArray &bytes,
                   QString *id,
                   QString *name,
                   QString *version,
                   QStringList *capabilities)
{
    if (bytes.isEmpty() || bytes.size() > SkillExtensionInventory::MaxManifestBytes
            || bytes.startsWith("\xef\xbb\xbf")) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !StrictJsonValidator::accepts(bytes)) {
        return false;
    }
    const QJsonObject manifest = document.object();
    const QSet<QString> required{
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"),
        QStringLiteral("executor"), QStringLiteral("enabled"),
        QStringLiteral("trusted"), QStringLiteral("builtin")};
    const QSet<QString> allowed{
        QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("version"),
        QStringLiteral("description"), QStringLiteral("executor"),
        QStringLiteral("enabled"), QStringLiteral("trusted"),
        QStringLiteral("builtin"), QStringLiteral("source"),
        QStringLiteral("required_group"), QStringLiteral("permissions"),
        QStringLiteral("triggers")};
    const QStringList manifestKeys = manifest.keys();
    const QSet<QString> actual(manifestKeys.cbegin(), manifestKeys.cend());
    QSet<QString> missing = required;
    missing.subtract(actual);
    QSet<QString> unknown = actual;
    unknown.subtract(allowed);
    if (!missing.isEmpty() || !unknown.isEmpty()) {
        return false;
    }
    if (!manifest.value(QStringLiteral("id")).isString()
            || !manifest.value(QStringLiteral("name")).isString()
            || !manifest.value(QStringLiteral("version")).isString()
            || !manifest.value(QStringLiteral("executor")).isString()
            || !manifest.value(QStringLiteral("enabled")).isBool()
            || !manifest.value(QStringLiteral("trusted")).isBool()
            || !manifest.value(QStringLiteral("builtin")).isBool()) {
        return false;
    }
    *id = manifest.value(QStringLiteral("id")).toString();
    *name = manifest.value(QStringLiteral("name")).toString();
    *version = manifest.value(QStringLiteral("version")).toString();
    if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
                .match(*id).hasMatch()
            || !registrySafeText(*name, 128)
            || !registrySafeText(*version, 64)
            || !QSet<QString>{QStringLiteral("instruction"), QStringLiteral("image"),
                              QStringLiteral("presentation")}
                    .contains(manifest.value(QStringLiteral("executor")).toString())) {
        return false;
    }
    for (const auto &field : {
             qMakePair(QStringLiteral("description"), 2048),
             qMakePair(QStringLiteral("source"), 2048),
             qMakePair(QStringLiteral("required_group"), 128)}) {
        if (manifest.contains(field.first)
                && (!manifest.value(field.first).isString()
                    || !safeScalar(manifest.value(field.first).toString(),
                                   field.second, true))) {
            return false;
        }
    }

    QStringList permissions;
    if (manifest.contains(QStringLiteral("permissions"))
            && !stringArray(manifest.value(QStringLiteral("permissions")),
                            16, 64, &permissions)) {
        return false;
    }
    QStringList triggers;
    if (manifest.contains(QStringLiteral("triggers"))
            && !stringArray(manifest.value(QStringLiteral("triggers")),
                            64, 256, &triggers)) {
        return false;
    }
    QSet<QString> requested{QStringLiteral("skill-content")};
    const QSet<QString> allowedPermissions{
        QStringLiteral("files-read"), QStringLiteral("files-write"),
        QStringLiteral("local-process"), QStringLiteral("aegisy-network"),
        QStringLiteral("model-request")};
    for (const QString &permission : permissions) {
        if (!allowedPermissions.contains(permission)) return false;
        if (permission == QStringLiteral("files-read")
                || permission == QStringLiteral("files-write")) {
            requested.insert(QStringLiteral("filesystem-read"));
        } else if (permission == QStringLiteral("local-process")) {
            requested.insert(QStringLiteral("process"));
        } else {
            requested.insert(QStringLiteral("network"));
        }
    }
    *capabilities = requested.values();
    std::sort(capabilities->begin(), capabilities->end());
    return true;
}

} // namespace

SkillExtensionInventoryResult SkillExtensionInventory::inspectRoot(
    const QString &rootPath)
{
    if (!safeScalar(rootPath, 4096) || rootPath.toUtf8().size() > 16384) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-path-invalid"));
    }
    const QFileInfo suppliedRoot(rootPath);
    if (suppliedRoot.isSymLink()) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-symlink-invalid"));
    }
    if (!suppliedRoot.exists()) {
        SkillExtensionInventoryResult result;
        result.state = SkillExtensionInventoryState::Empty;
        result.sourceIdentity = ExtensionTreeCapture::framedDigest(
            QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"),
            {QDir::cleanPath(suppliedRoot.absoluteFilePath()).toUtf8(),
             QByteArrayLiteral("missing")},
            QStringLiteral("skill-inventory-source:sha256:"));
        return result;
    }
    if (!suppliedRoot.isDir()) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-root-invalid"));
    }
    const QString root = suppliedRoot.canonicalFilePath();
    if (root.isEmpty()) {
        return failure(SkillExtensionInventoryState::Unavailable,
                       QStringLiteral("skill-root-unavailable"));
    }
    QDir rootDirectory(root);
    QFileInfoList skillDirectories = rootDirectory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::NoSort);
    std::sort(skillDirectories.begin(), skillDirectories.end(),
              [](const QFileInfo &left, const QFileInfo &right) {
        return left.fileName().toUtf8() < right.fileName().toUtf8();
    });
    if (skillDirectories.size() > MaxSkills) {
        return failure(SkillExtensionInventoryState::Invalid,
                       QStringLiteral("skill-count-limit"));
    }
    if (skillDirectories.isEmpty()) {
        SkillExtensionInventoryResult result;
        result.state = SkillExtensionInventoryState::Empty;
        result.sourceIdentity = ExtensionTreeCapture::framedDigest(
            QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"),
            {root.toUtf8(), QByteArrayLiteral("empty")},
            QStringLiteral("skill-inventory-source:sha256:"));
        return result;
    }

    SkillExtensionInventoryResult result;
    ExtensionTreeCaptureBudget budget;
    QSet<QString> foldedDirectories;
    QSet<QString> ids;
    for (const QFileInfo &skillDirectory : skillDirectories) {
        if (!ExtensionTreeCapture::safeEntryName(skillDirectory.fileName())
                || foldedDirectories.contains(skillDirectory.fileName().toCaseFolded())
                || skillDirectory.isSymLink() || !skillDirectory.isDir()) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-root-entry-invalid"));
        }
        foldedDirectories.insert(skillDirectory.fileName().toCaseFolded());
        ++budget.entries;
        QVector<ExtensionTreeCaptureEntry> tree;
        ExtensionTreeCaptureError scanError;
        if (!ExtensionTreeCapture::scanDirectory(
                treeCaptureDomain(), root, skillDirectory.absoluteFilePath(),
                QString(), 0, &budget, &tree, &scanError)) {
            return captureFailure(scanError);
        }
        const ExtensionTreeCaptureEntry *manifest = ExtensionTreeCapture::findFile(
            tree, QString::fromLatin1(kManifestName));
        const ExtensionTreeCaptureEntry *skillDocument =
            ExtensionTreeCapture::findFile(tree,
                                           QString::fromLatin1(kSkillDocumentName));
        if (!manifest || !skillDocument || skillDocument->bytes.isEmpty()
                || QString::fromUtf8(skillDocument->bytes).toUtf8()
                    != skillDocument->bytes
                || skillDocument->bytes.contains('\0')) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-required-content-invalid"));
        }
        QString id;
        QString name;
        QString version;
        QStringList capabilities;
        if (!parseManifest(manifest->bytes, &id, &name, &version, &capabilities)) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-manifest-invalid"));
        }
        if (ids.contains(id)) {
            return failure(SkillExtensionInventoryState::Invalid,
                           QStringLiteral("skill-id-duplicate"));
        }
        ids.insert(id);

        ExtensionRegistryRecord record;
        record.kind = ExtensionKind::Skill;
        record.id = id;
        record.name = name;
        record.version = version;
        record.sourceKind = ExtensionSourceKind::LocalDirectory;
        record.sourceIdentity = ExtensionTreeCapture::framedDigest(
            QByteArrayLiteral("aegisy-skill-extension-source/0.1\0"),
            {root.toUtf8(), skillDirectory.fileName().toUtf8()},
            QStringLiteral("extension-source:sha256:"));
        record.contentIdentity = ExtensionTreeCapture::contentIdentity(
            treeCaptureDomain(), tree);
        record.trust = ExtensionTrustState::Unverified;
        // 来源不自我声明兼容性；判定由 ExtensionCompatibilityPolicy 统一做出。
        record.compatibility = ExtensionCompatibilityState::Unknown;
        record.compatibilityReason = QStringLiteral("skill-compatibility-unevaluated");
        record.scope = QStringLiteral("user");
        record.requestedCapabilities = capabilities;
        record.installed = true;
        record.effectiveEnabled = false;
        record.updateAvailable = false;
        record.recoveryAvailable = false;
        result.records.append(record);
    }

    std::sort(result.records.begin(), result.records.end(),
              [](const ExtensionRegistryRecord &left,
                 const ExtensionRegistryRecord &right) {
        return left.id < right.id;
    });
    QList<QByteArray> rootParts{root.toUtf8()};
    for (const ExtensionRegistryRecord &record : result.records) {
        rootParts.append(record.sourceIdentity.toUtf8());
        rootParts.append(record.contentIdentity.toUtf8());
    }
    result.sourceIdentity = ExtensionTreeCapture::framedDigest(
        QByteArrayLiteral("aegisy-skill-inventory-source/0.1\0"), rootParts,
        QStringLiteral("skill-inventory-source:sha256:"));
    result.state = SkillExtensionInventoryState::Ready;
    return result;
}
