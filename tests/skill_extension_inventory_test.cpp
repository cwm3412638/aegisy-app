#include "skill_extension_inventory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QJsonObject manifest(const QString &id)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), id + QStringLiteral(" display")},
        {QStringLiteral("version"), QStringLiteral("1.0.0")},
        {QStringLiteral("description"), QStringLiteral("Inventory fixture")},
        {QStringLiteral("executor"), QStringLiteral("instruction")},
        {QStringLiteral("enabled"), true},
        {QStringLiteral("trusted"), true},
        {QStringLiteral("builtin"), true},
        {QStringLiteral("permissions"), QJsonArray{
             QStringLiteral("files-read"), QStringLiteral("files-write"),
             QStringLiteral("local-process"), QStringLiteral("model-request")}},
        {QStringLiteral("triggers"), QJsonArray{QStringLiteral("fixture")}},
    };
}

bool makeSkill(const QString &root,
               const QString &directory,
               const QJsonObject &value,
               const QByteArray &script = QByteArray())
{
    const QString skillRoot = root + QLatin1Char('/') + directory;
    return writeFile(skillRoot + QStringLiteral("/aegisy-skill.json"),
                     QJsonDocument(value).toJson(QJsonDocument::Compact))
        && writeFile(skillRoot + QStringLiteral("/SKILL.md"),
                     QByteArrayLiteral("---\nname: fixture\n---\n\n# Fixture\n"))
        && (script.isNull()
            || writeFile(skillRoot + QStringLiteral("/scripts/run.sh"), script));
}

const ExtensionRegistryRecord *record(const SkillExtensionInventoryResult &result,
                                      const QString &id)
{
    for (const ExtensionRegistryRecord &candidate : result.records) {
        if (candidate.id == id) return &candidate;
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    bool ok = true;

    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    const QString missingPath = temporary.filePath(QStringLiteral("missing"));
    const SkillExtensionInventoryResult missing =
        SkillExtensionInventory::inspectRoot(missingPath);
    ok = expect(missing.state == SkillExtensionInventoryState::Empty
                    && missing.records.isEmpty()
                    && missing.sourceIdentity.startsWith(
                        QStringLiteral("skill-inventory-source:sha256:")),
                "missing root was not a bounded empty inventory") && ok;

    const QString root = temporary.filePath(QStringLiteral("skills"));
    const QString marker = temporary.filePath(QStringLiteral("executed"));
    ok = expect(makeSkill(root, QStringLiteral("zeta-directory"),
                          manifest(QStringLiteral("zeta.skill")),
                          QByteArray("#!/bin/sh\ntouch '") + marker.toUtf8() + "'\n")
                    && makeSkill(root, QStringLiteral("alpha-directory"),
                                 manifest(QStringLiteral("alpha.skill"))),
                "valid skill fixture could not be created") && ok;
    const SkillExtensionInventoryResult ready =
        SkillExtensionInventory::inspectRoot(root);
    ok = expect(ready.state == SkillExtensionInventoryState::Ready
                    && ready.records.size() == 2
                    && ready.records.at(0).id == QStringLiteral("alpha.skill")
                    && ready.records.at(1).id == QStringLiteral("zeta.skill")
                    && !QFileInfo::exists(marker),
                "valid inventory failed or executed skill content") && ok;
    const ExtensionRegistryRecord *zeta = record(ready, QStringLiteral("zeta.skill"));
    ok = expect(zeta
                    && zeta->sourceKind == ExtensionSourceKind::LocalDirectory
                    && zeta->trust == ExtensionTrustState::Unverified
                    && zeta->compatibility == ExtensionCompatibilityState::Unknown
                    && zeta->scope == QStringLiteral("user")
                    && zeta->installed && !zeta->effectiveEnabled
                    && !zeta->updateAvailable && !zeta->recoveryAvailable
                    && zeta->sourceIdentity.startsWith(
                        QStringLiteral("extension-source:sha256:"))
                    && zeta->contentIdentity.startsWith(
                        QStringLiteral("extension-content:sha256:"))
                    && zeta->requestedCapabilities
                        == QStringList{QStringLiteral("filesystem-read"),
                                       QStringLiteral("network"),
                                       QStringLiteral("process"),
                                       QStringLiteral("skill-content")},
                "skill record metadata or authority boundary is incorrect") && ok;
    ExtensionRegistryProjection projection;
    QString registryError;
    ok = expect(ExtensionRegistry::build(ready.records, &projection, &registryError),
                "skill records did not enter the strict extension registry") && ok;
    const QByteArray projectionBytes = QJsonDocument(projection.object)
        .toJson(QJsonDocument::Compact);
    ok = expect(!projectionBytes.contains(marker.toUtf8())
                    && !projection.object.value(QStringLiteral("install_authority"))
                        .toBool(true)
                    && !projection.object.value(QStringLiteral("enable_authority"))
                        .toBool(true)
                    && !projection.object.value(QStringLiteral("update_authority"))
                        .toBool(true)
                    && !projection.object.value(QStringLiteral("remove_authority"))
                        .toBool(true)
                    && !projection.object.value(QStringLiteral("execution_authority"))
                        .toBool(true),
                "skill inventory leaked content or granted registry authority") && ok;

    const SkillExtensionInventoryResult repeated =
        SkillExtensionInventory::inspectRoot(root);
    const ExtensionRegistryRecord *repeatedZeta = record(
        repeated, QStringLiteral("zeta.skill"));
    ok = expect(repeated.state == SkillExtensionInventoryState::Ready
                    && repeated.sourceIdentity == ready.sourceIdentity
                    && repeatedZeta
                    && repeatedZeta->sourceIdentity == zeta->sourceIdentity
                    && repeatedZeta->contentIdentity == zeta->contentIdentity,
                "unchanged skill inventory identities were not deterministic") && ok;
    ok = expect(writeFile(root + QStringLiteral("/zeta-directory/scripts/run.sh"),
                          QByteArrayLiteral("#!/bin/sh\nexit 7\n")),
                "skill content mutation failed") && ok;
    const SkillExtensionInventoryResult changed =
        SkillExtensionInventory::inspectRoot(root);
    const ExtensionRegistryRecord *changedZeta = record(changed, QStringLiteral("zeta.skill"));
    ok = expect(changed.state == SkillExtensionInventoryState::Ready && changedZeta
                    && changedZeta->sourceIdentity == zeta->sourceIdentity
                    && changedZeta->contentIdentity != zeta->contentIdentity
                    && changed.sourceIdentity != ready.sourceIdentity,
                "source and content identities did not separate location from bytes") && ok;

    QTemporaryDir unknownRoot;
    ok = expect(unknownRoot.isValid()
                    && writeFile(unknownRoot.filePath(QStringLiteral("unexpected.txt")),
                                 QByteArrayLiteral("unexpected"))
                    && SkillExtensionInventory::inspectRoot(unknownRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "unknown root item was accepted") && ok;

    QTemporaryDir malformedRoot;
    ok = expect(malformedRoot.isValid()
                    && writeFile(malformedRoot.filePath(
                                     QStringLiteral("broken/aegisy-skill.json")),
                                 QByteArrayLiteral("{\"id\":}"))
                    && writeFile(malformedRoot.filePath(QStringLiteral("broken/SKILL.md")),
                                 QByteArrayLiteral("# Broken\n"))
                    && SkillExtensionInventory::inspectRoot(malformedRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "malformed manifest was accepted") && ok;

    QTemporaryDir duplicateManifestKeyRoot;
    ok = expect(duplicateManifestKeyRoot.isValid()
                    && writeFile(duplicateManifestKeyRoot.filePath(
                                     QStringLiteral("skill/aegisy-skill.json")),
                                 QByteArrayLiteral(
                                     "{\"id\":\"first.skill\",\"\\u0069d\":\"second.skill\","
                                     "\"name\":\"Duplicate\",\"version\":\"1\","
                                     "\"executor\":\"instruction\",\"enabled\":false,"
                                     "\"trusted\":false,\"builtin\":false}"))
                    && writeFile(duplicateManifestKeyRoot.filePath(
                                     QStringLiteral("skill/SKILL.md")),
                                 QByteArrayLiteral("# Duplicate\n"))
                    && SkillExtensionInventory::inspectRoot(
                           duplicateManifestKeyRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "duplicate decoded manifest key was accepted") && ok;

    QTemporaryDir unknownFieldRoot;
    QJsonObject unknownField = manifest(QStringLiteral("unknown.field"));
    unknownField.insert(QStringLiteral("command"), QStringLiteral("run-me"));
    ok = expect(unknownFieldRoot.isValid()
                    && makeSkill(unknownFieldRoot.path(), QStringLiteral("skill"),
                                 unknownField)
                    && SkillExtensionInventory::inspectRoot(unknownFieldRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "unknown manifest field was accepted") && ok;

    QTemporaryDir duplicateRoot;
    ok = expect(duplicateRoot.isValid()
                    && makeSkill(duplicateRoot.path(), QStringLiteral("first"),
                                 manifest(QStringLiteral("duplicate.skill")))
                    && makeSkill(duplicateRoot.path(), QStringLiteral("second"),
                                 manifest(QStringLiteral("duplicate.skill")))
                    && SkillExtensionInventory::inspectRoot(duplicateRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "duplicate skill ID was accepted") && ok;

    QTemporaryDir duplicatePermissionRoot;
    QJsonObject duplicatePermission = manifest(QStringLiteral("duplicate.permission"));
    duplicatePermission.insert(QStringLiteral("permissions"),
                               QJsonArray{QStringLiteral("files-read"),
                                          QStringLiteral("files-read")});
    ok = expect(duplicatePermissionRoot.isValid()
                    && makeSkill(duplicatePermissionRoot.path(), QStringLiteral("skill"),
                                 duplicatePermission)
                    && SkillExtensionInventory::inspectRoot(
                           duplicatePermissionRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "duplicate permission was accepted") && ok;

    QTemporaryDir unknownPermissionRoot;
    QJsonObject unknownPermission = manifest(QStringLiteral("unknown.permission"));
    unknownPermission.insert(QStringLiteral("permissions"),
                             QJsonArray{QStringLiteral("shell-execute")});
    ok = expect(unknownPermissionRoot.isValid()
                    && makeSkill(unknownPermissionRoot.path(), QStringLiteral("skill"),
                                 unknownPermission)
                    && SkillExtensionInventory::inspectRoot(
                           unknownPermissionRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "unknown permission was accepted") && ok;

    QTemporaryDir oversizedRoot;
    ok = expect(oversizedRoot.isValid()
                    && makeSkill(oversizedRoot.path(), QStringLiteral("skill"),
                                 manifest(QStringLiteral("oversized.skill"))),
                "oversized fixture setup failed") && ok;
    QFile oversized(oversizedRoot.filePath(QStringLiteral("skill/large.bin")));
    ok = expect(oversized.open(QIODevice::WriteOnly)
                    && oversized.resize(SkillExtensionInventory::MaxFileBytes + 1),
                "oversized fixture could not be created") && ok;
    oversized.close();
    ok = expect(SkillExtensionInventory::inspectRoot(oversizedRoot.path()).state
                    == SkillExtensionInventoryState::Invalid,
                "oversized skill file was accepted") && ok;

    QTemporaryDir depthRoot;
    ok = expect(depthRoot.isValid()
                    && makeSkill(depthRoot.path(), QStringLiteral("skill"),
                                 manifest(QStringLiteral("deep.skill"))),
                "depth fixture setup failed") && ok;
    QString deep = depthRoot.filePath(QStringLiteral("skill"));
    for (int index = 0; index <= SkillExtensionInventory::MaxDepth; ++index) {
        deep += QStringLiteral("/d%1").arg(index);
    }
    ok = expect(QDir().mkpath(deep)
                    && SkillExtensionInventory::inspectRoot(depthRoot.path()).state
                        == SkillExtensionInventoryState::Invalid,
                "skill depth limit was not enforced") && ok;

    QTemporaryDir countRoot;
    ok = expect(countRoot.isValid(), "skill count fixture root unavailable") && ok;
    for (int index = 0; index <= SkillExtensionInventory::MaxSkills; ++index) {
        ok = expect(QDir().mkpath(countRoot.filePath(
                        QStringLiteral("skill-%1").arg(index))),
                    "skill count fixture directory could not be created") && ok;
    }
    ok = expect(SkillExtensionInventory::inspectRoot(countRoot.path()).state
                    == SkillExtensionInventoryState::Invalid,
                "skill count limit was not enforced") && ok;

    QTemporaryDir symlinkRoot;
    if (symlinkRoot.isValid()
            && makeSkill(symlinkRoot.path(), QStringLiteral("skill"),
                         manifest(QStringLiteral("linked.skill")))) {
        const QString link = symlinkRoot.filePath(QStringLiteral("skill/linked.md"));
        if (QFile::link(symlinkRoot.filePath(QStringLiteral("skill/SKILL.md")), link)) {
            ok = expect(SkillExtensionInventory::inspectRoot(symlinkRoot.path()).state
                            == SkillExtensionInventoryState::Invalid,
                        "symlinked skill item was accepted") && ok;
        }
    }

    QTemporaryDir rootLinkParent;
    if (rootLinkParent.isValid()) {
        const QString rootLink = rootLinkParent.filePath(QStringLiteral("skills-link"));
        if (QFile::link(root, rootLink)) {
            ok = expect(SkillExtensionInventory::inspectRoot(rootLink).state
                            == SkillExtensionInventoryState::Invalid,
                        "symlinked skill root was accepted") && ok;
        }
    }

    return ok ? 0 : 1;
}
