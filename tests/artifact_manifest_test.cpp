#include "artifact_manifest.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>
#include <iostream>

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

QString runtimeFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("aegisy-agentd.exe");
#else
    return QStringLiteral("aegisy-agentd");
#endif
}

QString adapterFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("codex.exe");
#else
    return QStringLiteral("codex");
#endif
}

QJsonObject manifest(const QByteArray &runtimeHash, const QByteArray &adapterHash)
{
    return {
        {QStringLiteral("schema_version"), QStringLiteral("aegisy-artifact-manifest/0.1")},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("aegisy-agentd")},
            {QStringLiteral("version"), QStringLiteral("0.1.0")},
            {QStringLiteral("path"), runtimeFileName()},
            {QStringLiteral("sha256"), QString::fromLatin1(runtimeHash)},
        }},
        {QStringLiteral("adapter"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("path"), adapterFileName()},
            {QStringLiteral("sha256"), QString::fromLatin1(adapterHash)},
        }},
    };
}

int fakeAdapterMain(const QStringList &arguments)
{
    if (arguments.size() == 2 && arguments.at(1) == QStringLiteral("--version")) {
        std::fputs("codex-cli 0.144.5\n", stdout);
        std::fflush(stdout);
        return 0;
    }
    if (arguments.size() < 3
        || arguments.at(1) != QStringLiteral("app-server")
        || arguments.at(2) != QStringLiteral("--stdio")) {
        return -1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromStdString(line));
        if (!document.isObject()) continue;
        const QJsonObject message = document.object();
        if (message.value(QStringLiteral("method")).toString()
            != QStringLiteral("initialize")) {
            continue;
        }
        QJsonObject response{
            {QStringLiteral("id"), message.value(QStringLiteral("id"))},
            {QStringLiteral("result"), QJsonObject{}},
        };
        const QByteArray encoded = QJsonDocument(response).toJson(QJsonDocument::Compact)
            + QByteArrayLiteral("\n");
        if (std::fwrite(encoded.constData(), 1,
                        static_cast<std::size_t>(encoded.size()), stdout)
            != static_cast<std::size_t>(encoded.size())) {
            return 1;
        }
        std::fflush(stdout);
    }
    return 0;
}

bool copyExecutable(const QString &source, const QString &destination)
{
    if (!QFile::copy(source, destination)) return false;
    const QFileDevice::Permissions permissions = QFile::permissions(destination)
        | QFileDevice::ExeOwner | QFileDevice::ExeUser
        | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    return QFile::setPermissions(destination, permissions);
}

bool sendMessage(QProcess &process, const QJsonObject &message)
{
    const QByteArray encoded = QJsonDocument(message).toJson(QJsonDocument::Compact)
        + QByteArrayLiteral("\n");
    return process.write(encoded) == encoded.size() && process.waitForBytesWritten(5000);
}

QJsonObject waitForResponse(QProcess &process, const QString &requestId,
                            int timeoutMs = 30000)
{
    QByteArray pending;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        pending += process.readAllStandardOutput();
        qsizetype newline = -1;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(newline);
            pending.remove(0, newline + 1);
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (!document.isObject()) continue;
            const QJsonObject message = document.object();
            if (message.value(QStringLiteral("id")).toString() == requestId) {
                return message;
            }
        }
        if (process.state() == QProcess::NotRunning) break;
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        process.waitForReadyRead(qMin(remaining, 100));
    }
    return {};
}

QJsonObject runtimeInitializeRequest()
{
    QString os = QStringLiteral("unknown");
#if defined(Q_OS_MACOS)
    os = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    os = QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    os = QStringLiteral("linux");
#endif
    QString architecture = QStringLiteral("unknown");
#if defined(Q_PROCESSOR_ARM_64)
    architecture = QStringLiteral("arm64");
#elif defined(Q_PROCESSOR_X86_64)
    architecture = QStringLiteral("x86_64");
#endif
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("manifest-startup-initialize")},
        {QStringLiteral("method"), QStringLiteral("initialize")},
        {QStringLiteral("params"), QJsonObject{
            {QStringLiteral("protocol"), QJsonObject{
                {QStringLiteral("minimum"), QStringLiteral("0.1")},
                {QStringLiteral("maximum"), QStringLiteral("0.1")},
                {QStringLiteral("preferred"), QStringLiteral("0.1")},
            }},
            {QStringLiteral("client"), QJsonObject{
                {QStringLiteral("name"), QStringLiteral("artifact-manifest-runtime-test")},
                {QStringLiteral("version"), QStringLiteral("1")},
            }},
            {QStringLiteral("platform"), QJsonObject{
                {QStringLiteral("os"), os},
                {QStringLiteral("architecture"), architecture},
            }},
            {QStringLiteral("capabilities"), QJsonObject{
                {QStringLiteral("stable"), QJsonArray{
                    QStringLiteral("runtime.codex-app-server"),
                    QStringLiteral("runtime.health"),
                    QStringLiteral("permission.read-only"),
                }},
                {QStringLiteral("experimental"), QJsonArray{}},
            }},
            {QStringLiteral("limits"), QJsonObject{
                {QStringLiteral("max_frame_bytes"), 4 * 1024 * 1024},
            }},
            {QStringLiteral("transport_security"), QJsonObject{
                {QStringLiteral("transport"), QStringLiteral("stdio")},
                {QStringLiteral("local"), true},
                {QStringLiteral("authenticated"), false},
                {QStringLiteral("encrypted"), false},
                {QStringLiteral("peer_verified"), false},
            }},
        }},
    };
}

int runtimeStartupMain(const QStringList &arguments)
{
    if (arguments.size() != 5) {
        std::fprintf(stderr,
                     "usage: AegisyArtifactManifestTest --runtime-startup "
                     "<agentd> <cmake> <generator>\n");
        return 2;
    }

    QTemporaryDir directory(QDir::tempPath()
                            + QStringLiteral("/aegisy-manifest-runtime-\u9a8c\u8bc1-XXXXXX"));
    if (!expect(directory.isValid(), "runtime startup temporary directory unavailable")) {
        return 1;
    }
    if (!expect(directory.path().toUtf8() != directory.path().toLatin1(),
                "runtime startup fixture is not in a Unicode path")) {
        return 1;
    }

    const QString runtimePath = QDir(directory.path()).filePath(runtimeFileName());
    const QString adapterPath = QDir(directory.path()).filePath(adapterFileName());
    const QString manifestPath = QDir(directory.path()).filePath(
        QStringLiteral("aegisy-agentd.manifest.json"));
    if (!expect(copyExecutable(arguments.at(2), runtimePath),
                "real aegisy-agentd could not be copied into the manifest fixture")) {
        return 1;
    }
    if (!expect(copyExecutable(QCoreApplication::applicationFilePath(), adapterPath),
                "fake Codex adapter could not be copied into the manifest fixture")) {
        return 1;
    }

    QProcess generator;
    generator.setProgram(arguments.at(3));
    generator.setArguments({
        QStringLiteral("-DBASE_DIR=") + directory.path(),
        QStringLiteral("-DOUTPUT=") + manifestPath,
        QStringLiteral("-DRUNTIME_PATH=") + runtimePath,
        QStringLiteral("-DRUNTIME_ID=aegisy-agentd"),
        QStringLiteral("-DRUNTIME_VERSION=0.1.0"),
        QStringLiteral("-DADAPTER_PATH=") + adapterPath,
        QStringLiteral("-DADAPTER_ID=codex-app-server"),
        QStringLiteral("-DADAPTER_VERSION=codex-cli 0.144.5"),
        QStringLiteral("-P"),
        arguments.at(4),
    });
    generator.start();
    if (!expect(generator.waitForStarted(5000), "manifest generator did not start")
        || !expect(generator.waitForFinished(30000), "manifest generator timed out")
        || !expect(generator.exitStatus() == QProcess::NormalExit
                       && generator.exitCode() == 0,
                   "manifest generator rejected the runtime startup fixture")) {
        return 1;
    }

    QProcess runtime;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("AEGISY_AGENT_BACKEND"));
    environment.remove(QStringLiteral("AEGISY_AGENTD_NAMED_PIPE"));
    environment.remove(QStringLiteral("AEGISY_AGENTD_UNIX_SOCKET_DIR"));
    environment.remove(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"));
    environment.remove(QStringLiteral("AEGISY_WORKBENCH_EMERGENCY_DISABLED"));
    environment.insert(QStringLiteral("AEGISY_CODEX_PATH"),
                       QDir(directory.path()).filePath(QStringLiteral("ignored-codex")));
    runtime.setProcessEnvironment(environment);
    runtime.setWorkingDirectory(directory.path());
    runtime.setProgram(runtimePath);
    runtime.setProcessChannelMode(QProcess::SeparateChannels);
    runtime.start();
    if (!expect(runtime.waitForStarted(10000), "manifest-bound aegisy-agentd did not start")) {
        return 1;
    }
    if (!expect(sendMessage(runtime, runtimeInitializeRequest()),
                "AAP initialize could not be written to manifest-bound runtime")) {
        runtime.kill();
        runtime.waitForFinished();
        return 1;
    }
    const QJsonObject initialized = waitForResponse(
        runtime, QStringLiteral("manifest-startup-initialize"));
    const QJsonObject backend = initialized.value(QStringLiteral("result"))
        .toObject().value(QStringLiteral("backend")).toObject();
    const bool exactAdapterReady = !initialized.isEmpty()
        && !initialized.contains(QStringLiteral("error"))
        && backend.value(QStringLiteral("adapter")).toString()
            == QStringLiteral("codex-app-server")
        && backend.value(QStringLiteral("version")).toString()
            == QStringLiteral("codex-cli 0.144.5")
        && backend.value(QStringLiteral("status")).toString()
            == QStringLiteral("ready");
    if (!exactAdapterReady) {
        std::fprintf(stderr, "initialize response: %s\n",
                     QJsonDocument(initialized).toJson(QJsonDocument::Compact).constData());
        const QByteArray runtimeError = runtime.readAllStandardError();
        if (!runtimeError.isEmpty()) {
            std::fprintf(stderr, "runtime stderr: %s\n", runtimeError.constData());
        }
    }
    if (!expect(exactAdapterReady,
                "real daemon did not initialize the exact manifest-bound adapter")) {
        runtime.kill();
        runtime.waitForFinished();
        return 1;
    }

    if (!expect(sendMessage(runtime, QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("method"), QStringLiteral("initialized")},
                    {QStringLiteral("params"), QJsonObject{}},
                }), "AAP initialized notification could not be written")
        || !expect(sendMessage(runtime, QJsonObject{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), QStringLiteral("manifest-startup-shutdown")},
                    {QStringLiteral("method"), QStringLiteral("shutdown")},
                    {QStringLiteral("params"), QJsonObject{}},
                }), "AAP shutdown could not be written")) {
        runtime.kill();
        runtime.waitForFinished();
        return 1;
    }
    const QJsonObject shutdown = waitForResponse(
        runtime, QStringLiteral("manifest-startup-shutdown"), 10000);
    runtime.closeWriteChannel();
    const bool exited = runtime.waitForFinished(10000);
    if (!exited) {
        runtime.kill();
        runtime.waitForFinished();
    }
    return expect(!shutdown.isEmpty() && !shutdown.contains(QStringLiteral("error")),
                  "manifest-bound runtime did not acknowledge shutdown")
            && expect(exited && runtime.exitStatus() == QProcess::NormalExit
                          && runtime.exitCode() == 0,
                      "manifest-bound runtime did not exit cleanly")
        ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    const int fakeAdapterResult = fakeAdapterMain(arguments);
    if (fakeAdapterResult >= 0) return fakeAdapterResult;
    if (arguments.size() >= 2
        && arguments.at(1) == QStringLiteral("--runtime-startup")) {
        return runtimeStartupMain(arguments);
    }
    if (arguments.size() == 3) {
        const auto generatedResult = ArtifactManifest::verifyFile(
            arguments.at(1), arguments.at(2));
        return expect(generatedResult.ok,
                      "generated artifact manifest was rejected by the production verifier")
            ? 0 : 1;
    }
    if (arguments.size() != 1) {
        std::fprintf(stderr, "usage: AegisyArtifactManifestTest [manifest runtime]\n");
        return 2;
    }
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "temporary directory unavailable")) return 1;
    const QString runtimePath = QDir(directory.path()).filePath(runtimeFileName());
    const QString adapterPath = QDir(directory.path()).filePath(adapterFileName());
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

#ifdef Q_OS_WIN
    const QString extensionlessPath = QDir(directory.path()).filePath(QStringLiteral("codex"));
    const QByteArray extensionlessHash = writeArtifact(
        extensionlessPath, QByteArrayLiteral("manifested non-executable"));
    QJsonObject shadowed = manifest(runtimeHash, extensionlessHash);
    QJsonObject shadowedAdapter = shadowed.value(QStringLiteral("adapter")).toObject();
    shadowedAdapter.insert(QStringLiteral("path"), QStringLiteral("codex"));
    shadowed.insert(QStringLiteral("adapter"), shadowedAdapter);
    result = ArtifactManifest::verifyObject(shadowed, directory.path(), runtimePath);
    if (!expect(!result.ok && result.reason == QStringLiteral("invalid-artifact-entry"),
                "extensionless adapter with an executable shadow was accepted")) return 1;
#endif

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
