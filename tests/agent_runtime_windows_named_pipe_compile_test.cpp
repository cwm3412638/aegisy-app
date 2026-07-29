#include "agent_runtime_client.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <functional>
#include <iostream>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Aclapi.h>
#endif

namespace {
bool expect(bool condition, const char *message)
{
    if (!condition) std::cerr << message << std::endl;
    return condition;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 20000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    return condition();
}

bool stopChild(QProcess &process, int timeoutMs = 2000)
{
    if (process.state() == QProcess::NotRunning) return true;
    process.terminate();
    if (process.waitForFinished(timeoutMs)) return true;
    process.kill();
    return process.waitForFinished(timeoutMs);
}

QProcessEnvironment sidecarEnvironment(const QString &dataRoot,
                                       const QString &pipeName);

int runFakeStdioRuntime()
{
    std::string rawLine;
    if (!std::getline(std::cin, rawLine)) return 91;
    const QJsonDocument requestDocument = QJsonDocument::fromJson(
        QByteArray::fromStdString(rawLine));
    if (!requestDocument.isObject()) return 92;
    const QJsonObject request = requestDocument.object();
    const QJsonObject result{
        {QStringLiteral("protocol"), QJsonObject{
            {QStringLiteral("minimum"), QStringLiteral("0.1")},
            {QStringLiteral("maximum"), QStringLiteral("0.1")},
            {QStringLiteral("selected"), QStringLiteral("0.1")},
            {QStringLiteral("upgrade_direction"), QStringLiteral("none")},
        }},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("aegisy-agentd")},
            {QStringLiteral("version"), QStringLiteral("0.1.0")},
        }},
        {QStringLiteral("platform"), QJsonObject{
            {QStringLiteral("os"), QStringLiteral("windows")},
            {QStringLiteral("architecture"), QStringLiteral("x86_64")},
        }},
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("preview")},
            {QStringLiteral("version"), QStringLiteral("0.1.0")},
            {QStringLiteral("status"), QStringLiteral("ready")},
        }},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("stable"), QJsonArray{
                QStringLiteral("runtime.preview"),
                QStringLiteral("runtime.health"),
                QStringLiteral("runtime.degradations"),
                QStringLiteral("permission.read-only"),
            }},
            {QStringLiteral("experimental"), QJsonArray{}},
        }},
        {QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("max_frame_bytes"), 4 * 1024 * 1024},
        }},
        {QStringLiteral("transport_security"), QJsonObject{
            {QStringLiteral("transport"), QStringLiteral("windows-named-pipe")},
            {QStringLiteral("local"), true},
            {QStringLiteral("authenticated"), false},
            {QStringLiteral("encrypted"), false},
            {QStringLiteral("peer_verified"), true},
        }},
    };
    const QJsonObject response{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), request.value(QStringLiteral("id"))},
        {QStringLiteral("result"), result},
    };
    std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData()
              << std::endl;
    while (std::getline(std::cin, rawLine)) {
    }
    return 0;
}

int launchSidecarAndExit(const QStringList &arguments)
{
    if (arguments.size() != 7) return 93;
    QProcess sidecar;
    sidecar.setProgram(arguments.at(2));
    sidecar.setProcessEnvironment(sidecarEnvironment(arguments.at(4), arguments.at(3)));
    sidecar.setStandardOutputFile(QProcess::nullDevice());
    sidecar.setStandardErrorFile(arguments.at(6));
    qint64 pid = 0;
    if (!sidecar.startDetached(&pid) || pid <= 0) return 94;
    QFile pidFile(arguments.at(5));
    if (!pidFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || pidFile.write(QByteArray::number(pid)) <= 0) {
        return 95;
    }
    return 0;
}

QProcessEnvironment sidecarEnvironment(const QString &dataRoot,
                                       const QString &pipeName)
{
    QProcessEnvironment environment = AgentRuntimeClient::sanitizedSidecarEnvironment(
        QProcessEnvironment::systemEnvironment());
    environment.remove(QStringLiteral("AEGISY_AGENTD_PATH"));
    environment.insert(QStringLiteral("AEGISY_AGENT_BACKEND"),
                       QStringLiteral("preview"));
    environment.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"), dataRoot);
    environment.insert(QStringLiteral("AEGISY_AGENTD_NAMED_PIPE"), pipeName);
    return environment;
}

bool waitForPipeConnection(QLocalSocket &socket, const QString &pipeName,
                           int timeoutMs = 5000)
{
    return waitUntil([&]() {
        if (socket.state() == QLocalSocket::UnconnectedState) {
            socket.connectToServer(pipeName, QIODevice::ReadWrite);
        }
        return socket.state() == QLocalSocket::ConnectedState;
    }, timeoutMs);
}

#ifdef Q_OS_WIN
bool pipeHasProtectedCurrentUserDacl(QLocalSocket &socket)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD tokenBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenBytes == 0) {
        CloseHandle(token);
        return false;
    }
    QByteArray tokenBuffer(static_cast<int>(tokenBytes), '\0');
    const bool tokenRead = GetTokenInformation(
        token, TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes);
    CloseHandle(token);
    if (!tokenRead) return false;
    const auto *tokenUser = reinterpret_cast<const TOKEN_USER *>(tokenBuffer.constData());

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    const DWORD status = GetSecurityInfo(
        reinterpret_cast<HANDLE>(socket.socketDescriptor()), SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || descriptor == nullptr || dacl == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return false;
    }

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION information{};
    const bool protectedDacl = GetSecurityDescriptorControl(
        descriptor, &control, &revision)
        && (control & SE_DACL_PROTECTED) != 0;
    const bool aclRead = GetAclInformation(
        dacl, &information, sizeof(information), AclSizeInformation);
    if (!protectedDacl || !aclRead || information.AceCount != 1) {
        LocalFree(descriptor);
        return false;
    }

    void *rawAce = nullptr;
    const bool aceRead = GetAce(dacl, 0, &rawAce);
    const auto *header = static_cast<const ACE_HEADER *>(rawAce);
    if (!aceRead || header == nullptr || header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
        LocalFree(descriptor);
        return false;
    }
    const auto *ace = reinterpret_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
    const PSID aceSid = const_cast<DWORD *>(&ace->SidStart);
    // Object creation may retain GENERIC_ALL or map it to file-object rights.
    const bool exactAccess = header->AceFlags == 0
        && (ace->Mask == GENERIC_ALL || ace->Mask == FILE_ALL_ACCESS);
    const bool matchesCurrentUser = exactAccess && IsValidSid(aceSid)
        && EqualSid(aceSid, tokenUser->User.Sid);
    LocalFree(descriptor);
    return matchesCurrentUser;
}
#endif
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
#ifdef Q_OS_WIN
    if (application.arguments().value(1)
        == QStringLiteral("--launch-sidecar-and-exit")) {
        return launchSidecarAndExit(application.arguments());
    }
    if (qEnvironmentVariableIsSet("AEGISY_WINDOWS_PIPE_FAKE_STDIO")) {
        return runFakeStdioRuntime();
    }
    if (application.arguments().size() == 3
        && application.arguments().at(1) == QStringLiteral("--connect-pipe")) {
        QLocalSocket socket;
        if (!waitForPipeConnection(socket, application.arguments().at(2), 5000)) return 1;
        return waitUntil([&]() {
            return socket.state() == QLocalSocket::UnconnectedState;
        }, 5000) ? 0 : 2;
    }
#endif
    QTemporaryDir temporaryRoot;
    if (!expect(temporaryRoot.isValid(), "could not create named-pipe test root")) return 1;
    const QString dataRoot = QDir(temporaryRoot.path()).filePath(QStringLiteral("workbench"));
    if (!expect(QDir().mkpath(dataRoot), "could not create workbench test root")) return 1;

    qputenv("AEGISY_AGENT_BACKEND", QByteArrayLiteral("preview"));
    qputenv("AEGISY_WORKBENCH_DATA_ROOT", dataRoot.toUtf8());
    qunsetenv("AEGISY_AGENTD_PATH");

    QJsonObject initializeResult;
    QString failure;
    QString connectionDetail;
    QString diagnostic;
    bool stopRequested = false;
    bool disconnectedAfterStop = false;
    AgentRuntimeClient client(
        nullptr, 50, 250, {0, 50, 100},
        AgentRuntimeClient::TransportMode::VerifiedWindowsNamedPipe);
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initializeResult](const QJsonObject &result) {
        initializeResult = result;
    });
    QObject::connect(&client, &AgentRuntimeClient::requestFailedExact,
                     [&failure](const QString &, const QString &method,
                                const QString &message, const QString &) {
        if (method == QStringLiteral("initialize")) failure = message;
    });
    QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                     [&connectionDetail, &stopRequested,
                      &disconnectedAfterStop](bool connected,
                                              const QString &detail) {
        connectionDetail = detail;
        if (stopRequested && !connected) disconnectedAfterStop = true;
    });
    QObject::connect(&client, &AgentRuntimeClient::diagnosticMessage,
                     [&diagnostic](const QString &message) {
        diagnostic = message;
        std::cerr << "runtime diagnostic: " << message.toStdString() << std::endl;
    });

    client.start();
    bool ok = expect(waitUntil([&]() {
        return client.isReady() || !failure.isEmpty();
    }), "verified Windows named-pipe Runtime did not finish initialization");
    if (!client.isReady()) {
        std::cerr << "connection detail: " << connectionDetail.toStdString()
                  << " diagnostic: " << diagnostic.toStdString()
                  << " initialize failure: " << failure.toStdString() << std::endl;
    }
    ok = expect(failure.isEmpty(), "verified Windows named-pipe initialization failed") && ok;
    ok = expect(client.isReady(), "verified Windows named-pipe Runtime is not ready") && ok;

    const QString runtimePath = client.runtimePath();
    ok = expect(!runtimePath.isEmpty() && QFileInfo(runtimePath).isFile(),
                "verified Windows named-pipe test did not resolve aegisy-agentd") && ok;

    const QJsonObject security = initializeResult
        .value(QStringLiteral("transport_security")).toObject();
    ok = expect(security == QJsonObject{
        {QStringLiteral("transport"), QStringLiteral("windows-named-pipe")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), false},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), true},
    }, "Runtime reported incorrect verified Windows named-pipe facts") && ok;

    const quint64 firstGeneration = client.processGeneration();
    stopRequested = true;
    client.stop();
    ok = expect(waitUntil([&]() {
        return disconnectedAfterStop && !client.isReady()
            && !client.isControlAvailable()
            && client.reconnectState() == AgentRuntimeClient::ReconnectState::Idle;
    }, 3000), "verified Windows named-pipe Runtime did not stop cleanly") && ok;

    // A stopped generation must be replaceable without reusing the old pipe or
    // allowing stale disconnect/handshake callbacks to make the new generation
    // look ready. The endpoint name is generated independently per launch.
    stopRequested = false;
    disconnectedAfterStop = false;
    initializeResult = {};
    failure.clear();
    connectionDetail.clear();
    client.start();
    ok = expect(waitUntil([&]() {
        return client.isReady() || !failure.isEmpty();
    }), "verified Windows named-pipe Runtime did not restart") && ok;
    ok = expect(failure.isEmpty() && client.isReady(),
                "verified Windows named-pipe restart did not initialize") && ok;
    ok = expect(client.processGeneration() > firstGeneration,
                "verified Windows named-pipe restart reused the old process generation") && ok;
    ok = expect(initializeResult.value(QStringLiteral("transport_security")).toObject()
                    == security,
                "verified Windows named-pipe restart changed transport security facts") && ok;
    stopRequested = true;
    client.stop();
    ok = expect(waitUntil([&]() {
        return disconnectedAfterStop && !client.isReady()
            && !client.isControlAvailable()
            && client.reconnectState() == AgentRuntimeClient::ReconnectState::Idle;
    }, 3000), "verified Windows named-pipe restart did not stop cleanly") && ok;

    // Point the supervised process at a real Windows executable that never
    // creates the selected pipe. A verified transport must fail closed after
    // bounded retries; it must not silently fall back to stdio and report a
    // successful initialize response.
    const QString fakeStdioRuntime = QCoreApplication::applicationFilePath();
    ok = expect(QFileInfo(fakeStdioRuntime).isFile(),
                "fake stdio sidecar executable is unavailable") && ok;
    if (QFileInfo(fakeStdioRuntime).isFile()) {
        qputenv("AEGISY_AGENTD_PATH", QFile::encodeName(fakeStdioRuntime));
        qputenv("AEGISY_WINDOWS_PIPE_FAKE_STDIO", QByteArrayLiteral("1"));
        bool fallbackInitialized = false;
        AgentRuntimeClient missingPipeClient(
            nullptr, 50, 250, {0, 20, 40},
            AgentRuntimeClient::TransportMode::VerifiedWindowsNamedPipe);
        QObject::connect(&missingPipeClient, &AgentRuntimeClient::runtimeInitialized,
                         [&fallbackInitialized](const QJsonObject &) {
            fallbackInitialized = true;
        });
        missingPipeClient.start();
        ok = expect(waitUntil([&]() {
            return missingPipeClient.reconnectState()
                == AgentRuntimeClient::ReconnectState::Exhausted;
        }, 6000), "selected Windows named-pipe failure did not exhaust retries") && ok;
        ok = expect(!fallbackInitialized && !missingPipeClient.isReady(),
                    "selected named-pipe failure silently fell back to stdio") && ok;
        ok = expect(missingPipeClient.runtimePath()
                        == QFileInfo(fakeStdioRuntime).absoluteFilePath(),
                    "selected named-pipe failure did not use the requested child executable") && ok;
        ok = expect(missingPipeClient.reconnectAttempt()
                        == missingPipeClient.maximumReconnectAttempts(),
                    "selected named-pipe failure exceeded its bounded retry budget") && ok;
        missingPipeClient.stop();
        qunsetenv("AEGISY_WINDOWS_PIPE_FAKE_STDIO");
        qunsetenv("AEGISY_AGENTD_PATH");
    }

    // Exercise the Rust listener's malformed-name guard directly. This keeps
    // invalid endpoint input on the selected transport and proves that the
    // sidecar exits before constructing Runtime or silently serving stdio.
    QProcess malformedName;
    malformedName.setProgram(runtimePath);
    malformedName.setProcessEnvironment(sidecarEnvironment(
        dataRoot, QStringLiteral("malformed-pipe-name")));
    malformedName.start();
    ok = expect(malformedName.waitForStarted(2000),
                "could not start sidecar for malformed named-pipe test") && ok;
    const bool malformedFinished = waitUntil([&]() {
        return malformedName.state() == QProcess::NotRunning;
    }, 5000);
    if (!malformedFinished) stopChild(malformedName);
    const QString malformedDiagnostic = QString::fromUtf8(
        malformedName.readAllStandardError()).trimmed();
    ok = expect(malformedFinished,
                "malformed named-pipe sidecar did not terminate") && ok;
    ok = expect(malformedDiagnostic.contains(
                    QStringLiteral("windows-named-pipe-invalid-name")),
                "malformed named-pipe input was not rejected by the sidecar") && ok;

    // The listener retains the exact supervising process generation and checks
    // liveness throughout accept. Launch through a short-lived helper and
    // require the sidecar to fail closed when that parent exits.
    const QString parentDeathPipe = QStringLiteral("aegisy-agent-parent-death-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'))
                 .left(16).toLower());
    const QString parentDeathPidPath = QDir(temporaryRoot.path()).filePath(
        QStringLiteral("parent-death.pid"));
    const QString parentDeathDiagnosticPath = QDir(temporaryRoot.path()).filePath(
        QStringLiteral("parent-death.stderr"));
    QProcess parentDeathLauncher;
    parentDeathLauncher.setProgram(QCoreApplication::applicationFilePath());
    parentDeathLauncher.setArguments({
        QStringLiteral("--launch-sidecar-and-exit"), runtimePath,
        parentDeathPipe, dataRoot, parentDeathPidPath,
        parentDeathDiagnosticPath,
    });
    parentDeathLauncher.start();
    ok = expect(parentDeathLauncher.waitForStarted(2000),
                "could not start parent-death launcher") && ok;
    ok = expect(parentDeathLauncher.waitForFinished(5000)
                    && parentDeathLauncher.exitStatus() == QProcess::NormalExit
                    && parentDeathLauncher.exitCode() == 0,
                "parent-death launcher did not start the sidecar") && ok;
    QFile parentDeathPidFile(parentDeathPidPath);
    ok = expect(parentDeathPidFile.open(QIODevice::ReadOnly),
                "parent-death launcher did not record the sidecar PID") && ok;
    bool parentDeathPidOk = false;
    const DWORD parentDeathPid = parentDeathPidFile.readAll().trimmed().toULong(
        &parentDeathPidOk);
    HANDLE parentDeathProcess = parentDeathPidOk
        ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, parentDeathPid)
        : nullptr;
    ok = expect(parentDeathProcess != nullptr,
                "could not retain the parent-death sidecar process") && ok;
    DWORD parentDeathWait = WAIT_FAILED;
    if (parentDeathProcess != nullptr) {
        parentDeathWait = WaitForSingleObject(parentDeathProcess, 5000);
        if (parentDeathWait == WAIT_TIMEOUT) {
            TerminateProcess(parentDeathProcess, 96);
            WaitForSingleObject(parentDeathProcess, 2000);
        }
        CloseHandle(parentDeathProcess);
    }
    const QString parentDeathDiagnostic = QString::fromUtf8([&]() {
        QFile file(parentDeathDiagnosticPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    }()).trimmed();
    ok = expect(parentDeathWait == WAIT_OBJECT_0,
                "named-pipe sidecar survived its supervising parent") && ok;
    ok = expect(parentDeathDiagnostic.contains(
                    QStringLiteral("windows-named-pipe-parent-changed"))
                    || parentDeathDiagnostic.contains(
                        QStringLiteral("windows-named-pipe-parent-query-failed")),
                "parent loss did not fail closed before named-pipe initialization") && ok;

    // Two listeners cannot own the same name because the Rust side requests
    // FILE_FLAG_FIRST_PIPE_INSTANCE. Connect a local client to the first
    // listener so the second process is tested after the first has bound.
    const QString collisionPipe = QStringLiteral("aegisy-agent-collision-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'))
                 .left(16).toLower());
    QProcess firstListener;
    firstListener.setProgram(runtimePath);
    firstListener.setProcessEnvironment(sidecarEnvironment(dataRoot, collisionPipe));
    firstListener.start();
    ok = expect(firstListener.waitForStarted(2000),
                "could not start first named-pipe collision listener") && ok;
    QLocalSocket firstClient;
    const bool firstConnected = waitForPipeConnection(firstClient, collisionPipe);
    ok = expect(firstConnected,
                "first named-pipe collision listener did not accept a local client") && ok;
#ifdef Q_OS_WIN
    ok = expect(firstConnected && pipeHasProtectedCurrentUserDacl(firstClient),
                "named pipe did not expose one protected current-token-user GENERIC_ALL ACE") && ok;
#endif

    QProcess secondListener;
    secondListener.setProgram(runtimePath);
    secondListener.setProcessEnvironment(sidecarEnvironment(dataRoot, collisionPipe));
    secondListener.start();
    ok = expect(secondListener.waitForStarted(2000),
                "could not start second named-pipe collision listener") && ok;
    const bool secondFinished = waitUntil([&]() {
        return secondListener.state() == QProcess::NotRunning;
    }, 5000);
    if (!secondFinished) stopChild(secondListener);
    const QString collisionDiagnostic = QString::fromUtf8(
        secondListener.readAllStandardError()).trimmed();
    ok = expect(secondFinished,
                "second named-pipe collision listener did not terminate") && ok;
    ok = expect(collisionDiagnostic.contains(
                    QStringLiteral("windows-named-pipe-create-failed")),
                "first-instance named-pipe collision was not rejected") && ok;
    if (firstClient.state() != QLocalSocket::UnconnectedState) {
        firstClient.disconnectFromServer();
        if (firstClient.state() != QLocalSocket::UnconnectedState) {
            firstClient.waitForDisconnected(1000);
        }
    }
    ok = expect(stopChild(firstListener),
                "first named-pipe collision listener did not clean up") && ok;

    // A same-user local process is still not the supervised Qt parent. Run this
    // executable as a connector child and require the Rust listener to reject
    // that distinct PID before constructing Runtime or serving any AAP frame.
    const QString wrongPeerPipe = QStringLiteral("aegisy-agent-wrong-peer-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'))
                 .left(16).toLower());
    QProcess wrongPeerListener;
    wrongPeerListener.setProgram(runtimePath);
    wrongPeerListener.setProcessEnvironment(sidecarEnvironment(dataRoot, wrongPeerPipe));
    wrongPeerListener.start();
    ok = expect(wrongPeerListener.waitForStarted(2000),
                "could not start wrong-peer named-pipe listener") && ok;
    QProcess wrongPeerConnector;
    wrongPeerConnector.setProgram(QCoreApplication::applicationFilePath());
    wrongPeerConnector.setArguments({QStringLiteral("--connect-pipe"), wrongPeerPipe});
    wrongPeerConnector.start();
    ok = expect(wrongPeerConnector.waitForStarted(2000),
                "could not start wrong-peer connector child") && ok;
    const bool wrongPeerFinished = waitUntil([&]() {
        return wrongPeerListener.state() == QProcess::NotRunning;
    }, 5000);
    if (!wrongPeerFinished) stopChild(wrongPeerListener);
    const QString wrongPeerDiagnostic = QString::fromUtf8(
        wrongPeerListener.readAllStandardError()).trimmed();
    ok = expect(wrongPeerFinished,
                "wrong-peer named-pipe listener did not terminate") && ok;
    ok = expect(wrongPeerDiagnostic.contains(
                    QStringLiteral("windows-named-pipe-peer-mismatch")),
                "named-pipe listener accepted a client from the wrong PID") && ok;
    const bool wrongPeerConnectorFinished = wrongPeerConnector.waitForFinished(5000);
    if (!wrongPeerConnectorFinished) stopChild(wrongPeerConnector);
    ok = expect(wrongPeerConnectorFinished
                    && wrongPeerConnector.exitStatus() == QProcess::NormalExit
                    && wrongPeerConnector.exitCode() == 0,
                "wrong-peer connector did not observe bounded pipe rejection") && ok;

    qunsetenv("AEGISY_AGENT_BACKEND");
    qunsetenv("AEGISY_WORKBENCH_DATA_ROOT");
    return ok ? 0 : 1;
}
