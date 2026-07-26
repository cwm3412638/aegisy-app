#include "artifact_manifest.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QByteArray writeArtifact(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(bytes);
    file.close();
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QJsonObject manifest(const QByteArray &runtimeHash, const QByteArray &adapterHash)
{
    return {
        {QStringLiteral("schema_version"), QStringLiteral("aegisy-artifact-manifest/0.1")},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("aegisy-agentd")},
            {QStringLiteral("version"), QStringLiteral("0.1.0")},
            {QStringLiteral("path"), QStringLiteral("aegisy-agentd")},
            {QStringLiteral("sha256"), QString::fromLatin1(runtimeHash)},
        }},
        {QStringLiteral("adapter"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("path"), QStringLiteral("codex")},
            {QStringLiteral("sha256"), QString::fromLatin1(adapterHash)},
        }},
    };
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 3) {
        const auto generatedResult = ArtifactManifest::verifyFile(
            QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]));
        return expect(generatedResult.ok,
                      "generated artifact manifest was rejected by the production verifier")
            ? 0 : 1;
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: AegisyArtifactManifestTest [manifest runtime]\n");
        return 2;
    }
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "temporary directory unavailable")) return 1;
    const QString runtimePath = QDir(directory.path()).filePath(QStringLiteral("aegisy-agentd"));
    const QString adapterPath = QDir(directory.path()).filePath(QStringLiteral("codex"));
    const QByteArray runtimeHash = writeArtifact(runtimePath, QByteArrayLiteral("runtime"));
    const QByteArray adapterHash = writeArtifact(adapterPath, QByteArrayLiteral("adapter"));
    const QString manifestPath = QDir(directory.path()).filePath(QStringLiteral("manifest.json"));
    QFile manifestFile(manifestPath);
    if (!expect(manifestFile.open(QIODevice::WriteOnly), "manifest cannot be written")) return 1;
    manifestFile.write(QJsonDocument(manifest(runtimeHash, adapterHash)).toJson(QJsonDocument::Compact));
    manifestFile.close();

    auto result = ArtifactManifest::verifyFile(manifestPath, runtimePath);
    if (!expect(result.ok && result.version == QStringLiteral("0.1.0/codex-cli 0.144.5"),
                "valid artifact manifest was rejected")) return 1;

    QFile tampered(adapterPath);
    if (!expect(tampered.open(QIODevice::Append), "tampered artifact cannot be opened")) return 1;
    tampered.write("tamper");
    tampered.close();
    result = ArtifactManifest::verifyFile(manifestPath, runtimePath);
    if (!expect(!result.ok && result.reason == QStringLiteral("artifact-hash-mismatch"),
                "adapter hash tampering was accepted")) return 1;

    QJsonObject invalid = manifest(runtimeHash, adapterHash);
    invalid[QStringLiteral("runtime")] = QJsonObject{
        {QStringLiteral("id"), QStringLiteral("aegisy-agentd")},
        {QStringLiteral("version"), QStringLiteral("0.1.0")},
        {QStringLiteral("path"), QStringLiteral("../outside")},
        {QStringLiteral("sha256"), QString::fromLatin1(runtimeHash)},
    };
    result = ArtifactManifest::verifyObject(invalid, directory.path(), runtimePath);
    if (!expect(!result.ok && result.reason == QStringLiteral("invalid-artifact-entry"),
                "path traversal was accepted")) return 1;

    QJsonObject unknown = manifest(runtimeHash, adapterHash);
    unknown.insert(QStringLiteral("unexpected"), true);
    result = ArtifactManifest::verifyObject(unknown, directory.path(), runtimePath);
    if (!expect(!result.ok && result.reason == QStringLiteral("manifest-fields-invalid"),
                "unknown manifest fields were accepted")) return 1;
    return 0;
}
