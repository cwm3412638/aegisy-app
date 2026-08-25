#include "extension_inventory_coordinator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QJsonDocument>
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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) return 1;

#ifdef Q_OS_WIN
    const QString codex = root.filePath(QStringLiteral("codex.cmd"));
    const QByteArray launcher = QByteArrayLiteral(
        "@echo off\r\n"
        "if not \"%PRIVATE_EXTENSION_TOKEN%\"==\"\" exit /b 9\r\n"
        "echo {\"installed\":[],\"available\":[{\"pluginId\":\"fixture.plugin\","
        "\"name\":\"Fixture Plugin\",\"marketplaceName\":\"Fixture\","
        "\"version\":\"1.0.0\",\"installed\":false,\"enabled\":false,"
        "\"source\":{\"path\":\"C:/private/plugin\"}}]}\r\n");
#else
    const QString codex = root.filePath(QStringLiteral("codex"));
    const QByteArray launcher = QByteArrayLiteral(
        "#!/bin/sh\n"
        "test -z \"$PRIVATE_EXTENSION_TOKEN\" || exit 9\n"
        "printf '%s\\n' '{\"installed\":[],\"available\":[{\"pluginId\":\"fixture.plugin\","
        "\"name\":\"Fixture Plugin\",\"marketplaceName\":\"Fixture\","
        "\"version\":\"1.0.0\",\"installed\":false,\"enabled\":false,"
        "\"source\":{\"path\":\"/private/plugin\"}}]}'\n");
#endif
    if (!writeFile(codex, launcher)) return 1;
    QFile::setPermissions(codex, QFile::permissions(codex)
        | QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther);

    const QString skillsRoot = root.filePath(QStringLiteral("skills"));
    const QByteArray manifest = QByteArrayLiteral(
        "{\"id\":\"fixture.skill\",\"name\":\"Fixture Skill\","
        "\"version\":\"1.0.0\",\"executor\":\"instruction\","
        "\"enabled\":true,\"trusted\":true,\"builtin\":false}");
    if (!writeFile(skillsRoot + QStringLiteral("/fixture/aegisy-skill.json"),
                   manifest)
            || !writeFile(skillsRoot + QStringLiteral("/fixture/SKILL.md"),
                          QByteArrayLiteral("# Fixture\n"))) {
        return 1;
    }
    const QString mcpPath = root.filePath(QStringLiteral("settings.json"));
    if (!writeFile(mcpPath, QByteArrayLiteral(
            "{\"mcpServers\":{\"fixture.mcp\":{\"command\":\"npx\","
            "\"env\":{\"ACCESS_TOKEN\":\"private-value\"}}}}"))) {
        return 1;
    }

    ExtensionInventoryInputs inputs;
    inputs.codexExecutable = codex;
    inputs.sourceEnvironment = QProcessEnvironment::systemEnvironment();
    inputs.sourceEnvironment.insert(
        QStringLiteral("PRIVATE_EXTENSION_TOKEN"), QStringLiteral("private-value"));
    inputs.skillsRoot = skillsRoot;
    inputs.mcpConfigurationPath = mcpPath;
    inputs.codexTimeoutMs = 3000;
    const ExtensionInventorySnapshot snapshot =
        ExtensionInventoryCoordinator::collect(inputs);
    if (!expect(snapshot.registryValid && snapshot.records.size() == 3
                    && snapshot.sourceIssueCodes.isEmpty()
                    && snapshot.registryIdentity.startsWith(
                        QStringLiteral("extension-registry:sha256:")),
                "three strict extension sources did not produce one registry")) {
        return 1;
    }
    ExtensionRegistryProjection projection;
    QString error;
    if (!expect(ExtensionRegistry::build(snapshot.records, &projection, &error),
                "coordinator records failed registry revalidation")) return 1;
    const QByteArray bytes = QJsonDocument(projection.object).toJson();
    if (!expect(!bytes.contains("private-value") && !bytes.contains("/private/plugin")
                    && !bytes.contains("C:/private/plugin") && !bytes.contains("npx")
                    && !bytes.contains("ACCESS_TOKEN"),
                "coordinator projection leaked source or environment details")) {
        return 1;
    }
    for (const ExtensionRegistryRecord &record : snapshot.records) {
        if (!expect(!record.effectiveEnabled && !record.updateAvailable
                        && !record.recoveryAvailable,
                    "coordinator source granted unsupported authority")) return 1;
    }
    // 默认宿主档案不授予任何能力，因此每条请求能力的记录都必须确定不兼容，
    // 而不是降级成"未知"。
    for (const ExtensionRegistryRecord &record : snapshot.records) {
        const bool judged = record.requestedCapabilities.isEmpty()
            ? record.compatibility != ExtensionCompatibilityState::Compatible
            : record.compatibility == ExtensionCompatibilityState::Incompatible
                && record.compatibilityReason
                    == QStringLiteral("extension-capability-not-granted");
        if (!expect(judged, "coordinator left compatibility unevaluated")) return 1;
    }

    // 授予当前只读能力集合后，判定改变但授权不改变。stdio MCP 服务器请求进程执行，
    // 因此仍然确定不兼容；Codex 插件缺少宿主版本证据，只能得出"未知"。
    inputs.host.grantedCapabilities =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();
    const ExtensionInventorySnapshot granted =
        ExtensionInventoryCoordinator::collect(inputs);
    if (!expect(granted.registryValid && granted.records.size() == 3,
                "granted host profile did not produce one registry")) return 1;
    for (const ExtensionRegistryRecord &record : granted.records) {
        bool judged = false;
        switch (record.kind) {
        case ExtensionKind::CodexPlugin:
            judged = record.compatibility == ExtensionCompatibilityState::Unknown
                && record.compatibilityReason
                    == QStringLiteral("codex-plugin-host-version-unknown");
            break;
        case ExtensionKind::Skill:
            judged = record.compatibility == ExtensionCompatibilityState::Compatible
                && record.compatibilityReason.isEmpty();
            break;
        case ExtensionKind::Mcp:
            judged = record.compatibility == ExtensionCompatibilityState::Incompatible
                && record.compatibilityReason
                    == QStringLiteral("extension-capability-not-granted");
            break;
        }
        if (!expect(judged, "granted host profile produced the wrong verdict")) return 1;
        if (!expect(record.trust == ExtensionTrustState::Unverified
                        && !record.effectiveEnabled,
                    "a compatible verdict granted enablement authority")) return 1;
    }

    // 没有复核记录时每条记录都保持 Unverified，因此启用门禁无法被满足。
    ExtensionReviewPin pin;
    for (const ExtensionRegistryRecord &record : granted.records) {
        if (record.kind != ExtensionKind::Skill) continue;
        pin.kind = record.kind;
        pin.id = record.id;
        pin.sourceIdentity = record.sourceIdentity;
        pin.contentIdentity = record.contentIdentity;
    }
    if (!expect(!pin.id.isEmpty(), "the fixture skill record was not found")) return 1;

    // 针对确切内容的复核记录只授予信任，不授予启用，也不改变兼容性判定。
    inputs.reviewPins = {pin};
    const ExtensionInventorySnapshot reviewed =
        ExtensionInventoryCoordinator::collect(inputs);
    if (!expect(reviewed.registryValid && reviewed.records.size() == 3,
                "reviewed inventory did not produce one registry")) return 1;
    int verifiedCount = 0;
    for (const ExtensionRegistryRecord &record : reviewed.records) {
        if (record.trust == ExtensionTrustState::Verified) ++verifiedCount;
        if (!expect(!record.effectiveEnabled,
                    "a review pin enabled an extension")) return 1;
    }
    if (!expect(verifiedCount == 1,
                "an exact review pin did not verify exactly one record")) return 1;

    // 内容漂移让复核结论失效，信任必须收回。
    ExtensionReviewPin drifted = pin;
    drifted.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('a'));
    inputs.reviewPins = {drifted};
    const ExtensionInventorySnapshot stale =
        ExtensionInventoryCoordinator::collect(inputs);
    for (const ExtensionRegistryRecord &record : stale.records) {
        if (!expect(record.trust == ExtensionTrustState::Unverified,
                    "content drift did not revoke trust in the inventory")) return 1;
    }
    inputs.reviewPins.clear();

    inputs.codexExecutable = root.filePath(QStringLiteral("missing-codex"));
    const ExtensionInventorySnapshot degraded =
        ExtensionInventoryCoordinator::collect(inputs);
    return expect(degraded.registryValid && degraded.records.size() == 2
                      && degraded.sourceIssueCodes
                          == QStringList{QStringLiteral("codex-plugin-source-unavailable")},
                  "unavailable Codex source did not degrade to safe partial inventory")
        ? 0 : 1;
}
