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
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
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
#include <sddl.h>
#include <stdlib.h>
#endif

struct AgentRuntimeClientSocketTestAccess {
    static QString endpointName(const AgentRuntimeClient &client)
    {
        return client.m_unixSocketPath;
    }

    static QProcess::ProcessState processState(const AgentRuntimeClient &client)
    {
        return client.m_process->state();
    }

    static bool automaticReconnectSuppressed(const AgentRuntimeClient &client)
    {
        return client.m_autoReconnectSuppressed;
    }

    static quint64 localSocketAttemptEpoch(const AgentRuntimeClient &client)
    {
        return client.m_localSocketAttemptEpoch;
    }

    static void shortenActiveStartupTimeout(AgentRuntimeClient &client, int timeoutMs)
    {
        if (client.m_startupTimer->isActive()) {
            client.m_startupTimer->start(timeoutMs);
        }
    }
};

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
    // The client authenticates the transport before any AAP frame: the first
    // line must be the one-time bootstrap prelude carrying the exact token
    // passed through the launch environment.
    std::string rawLine;
    if (!std::getline(std::cin, rawLine)) return 91;
    const QJsonDocument preludeDocument = QJsonDocument::fromJson(
        QByteArray::fromStdString(rawLine));
    if (!preludeDocument.isObject()) return 96;
    const QJsonObject prelude = preludeDocument.object();
    const QString expectedToken = qEnvironmentVariable("AEGISY_BOOTSTRAP_TOKEN");
    if (expectedToken.isEmpty()
        || prelude.value(QStringLiteral("schema")).toString()
            != QStringLiteral("aegisy-bootstrap-auth/0.1")
        || prelude.value(QStringLiteral("token")).toString() != expectedToken
        || prelude.size() != 2) {
        return 97;
    }
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
            {QStringLiteral("authenticated"), true},
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
bool setWideEnvironmentVariable(const wchar_t *name, const QString &value)
{
    // SetEnvironmentVariableW updates only the Win32 block while the client
    // reads through the CRT environment; _wputenv_s keeps both in sync.
    return _wputenv_s(name, reinterpret_cast<LPCWSTR>(value.utf16())) == 0;
}

void clearWideEnvironmentVariable(const wchar_t *name)
{
    _wputenv_s(name, L"");
}

HANDLE createCurrentUserControlPipe(const QString &path)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return INVALID_HANDLE_VALUE;
    }
    DWORD tokenBytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenBytes == 0) {
        CloseHandle(token);
        return INVALID_HANDLE_VALUE;
    }
    QByteArray tokenBuffer(static_cast<int>(tokenBytes), '\0');
    const bool tokenRead = GetTokenInformation(
        token, TokenUser, tokenBuffer.data(), tokenBytes, &tokenBytes);
    CloseHandle(token);
    if (!tokenRead) return INVALID_HANDLE_VALUE;

    const auto *tokenUser = reinterpret_cast<const TOKEN_USER *>(tokenBuffer.constData());
    LPWSTR sidText = nullptr;
    if (!IsValidSid(tokenUser->User.Sid)
        || !ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)
        || sidText == nullptr) {
        if (sidText != nullptr) LocalFree(sidText);
        return INVALID_HANDLE_VALUE;
    }
    const QString sddl = QStringLiteral("D:P(A;;GA;;;%1)")
        .arg(QString::fromWCharArray(sidText));
    LocalFree(sidText);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1,
            &descriptor, nullptr)
        || descriptor == nullptr) {
        if (descriptor != nullptr) LocalFree(descriptor);
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES attributes{
        static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE,
    };
    const HANDLE pipe = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024, 64 * 1024,
        0, &attributes);
    LocalFree(descriptor);
    return pipe;
}

QString localPipePath(const QString &pipeName)
{
    return QStringLiteral("\\\\.\\pipe\\%1").arg(pipeName);
}

QString remotePipePath(const QString &pipeName)
{
    WCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(computerName, &length) || length == 0) return {};
    return QStringLiteral("\\\\%1\\pipe\\%2")
        .arg(QString::fromWCharArray(computerName, int(length)), pipeName);
}

HANDLE openPipeOnce(const QString &path, DWORD *lastError = nullptr)
{
    const HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lastError != nullptr) {
        *lastError = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    }
    return handle;
}

HANDLE openPipeWithRetry(const QString &path, int timeoutMs, DWORD *lastError)
{
    QElapsedTimer timer;
    timer.start();
    HANDLE handle = INVALID_HANDLE_VALUE;
    do {
        handle = openPipeOnce(path, lastError);
        if (handle != INVALID_HANDLE_VALUE) return handle;
        if (lastError != nullptr && *lastError == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(reinterpret_cast<LPCWSTR>(path.utf16()), 50);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    } while (timer.elapsed() < timeoutMs);
    return INVALID_HANDLE_VALUE;
}

bool waitForPipeAvailable(const QString &path, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        if (WaitNamedPipeW(reinterpret_cast<LPCWSTR>(path.utf16()), 50)) return true;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY
            && error != ERROR_SEM_TIMEOUT) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    } while (timer.elapsed() < timeoutMs);
    return false;
}

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

bool pipeRejectsRemoteClients(QLocalSocket &socket)
{
    DWORD flags = 0;
    return GetNamedPipeInfo(
               reinterpret_cast<HANDLE>(socket.socketDescriptor()), &flags,
               nullptr, nullptr, nullptr) != FALSE
        && (flags & PIPE_REJECT_REMOTE_CLIENTS) == PIPE_REJECT_REMOTE_CLIENTS;
}

bool finishOverlappedConnect(HANDLE pipe, HANDLE event, OVERLAPPED *overlapped,
                             bool pending)
{
    if (!pending) return true;

    bool cancellationAccepted = true;
    if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) {
        if (!CancelIoEx(pipe, overlapped)) {
            cancellationAccepted = GetLastError() == ERROR_NOT_FOUND;
        }
    }

    DWORD transferred = 0;
    const bool completed = GetOverlappedResult(
        pipe, overlapped, &transferred, TRUE) != FALSE;
    const DWORD completionError = completed ? ERROR_SUCCESS : GetLastError();
    const bool terminalCompletion = completed
        || completionError == ERROR_OPERATION_ABORTED
        || completionError == ERROR_BROKEN_PIPE
        || completionError == ERROR_NO_DATA
        || completionError == ERROR_PIPE_NOT_CONNECTED;
    return cancellationAccepted && terminalCompletion;
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
        const QString probeEndpoint = AgentRuntimeClientSocketTestAccess::endpointName(client);
        std::cerr << "runtime path: " << client.runtimePath().toStdString()
                  << " exists: " << QFileInfo(client.runtimePath()).isFile()
                  << " process state: "
                  << AgentRuntimeClientSocketTestAccess::processState(client)
                  << " endpoint: " << probeEndpoint.toStdString()
                  << " attempt epoch: "
                  << AgentRuntimeClientSocketTestAccess::localSocketAttemptEpoch(client)
                  << std::endl;
        QLocalSocket probe;
        probe.connectToServer(probeEndpoint, QIODevice::ReadWrite);
        const bool probeConnected = probe.waitForConnected(2000);
        std::cerr << "endpoint probe connected: " << probeConnected
                  << " error: " << probe.errorString().toStdString() << std::endl;
#ifdef Q_OS_WIN
        // Distinguish a missing pipe (ERROR_FILE_NOT_FOUND) from a busy
        // single-instance pipe (ERROR_SEM_TIMEOUT) without occupying it.
        const QString widePipe = QStringLiteral("\\\\.\\pipe\\%1").arg(probeEndpoint);
        SetLastError(0);
        const BOOL pipeSeen =
            WaitNamedPipeW(reinterpret_cast<LPCWSTR>(widePipe.utf16()), 1);
        std::cerr << "waitNamedPipe: " << (pipeSeen != FALSE)
                  << " error: " << GetLastError() << std::endl;
#endif
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
        {QStringLiteral("authenticated"), true},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), true},
    }, "Runtime reported incorrect verified Windows named-pipe facts") && ok;

    const quint64 firstGeneration = client.processGeneration();
    const QString firstEndpoint = AgentRuntimeClientSocketTestAccess::endpointName(client);
    ok = expect(!firstEndpoint.isEmpty(),
                "first Windows named-pipe generation did not retain its endpoint") && ok;
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
    const quint64 secondGeneration = client.processGeneration();
    const QString secondEndpoint = AgentRuntimeClientSocketTestAccess::endpointName(client);
    ok = expect(!secondEndpoint.isEmpty() && secondEndpoint != firstEndpoint,
                "verified Windows named-pipe restart reused the old endpoint name") && ok;
    QLocalSocket retiredEndpointProbe;
    retiredEndpointProbe.connectToServer(firstEndpoint, QIODevice::ReadWrite);
    const bool retiredEndpointConnected = retiredEndpointProbe.waitForConnected(750);
    if (retiredEndpointConnected) retiredEndpointProbe.abort();
    ok = expect(!retiredEndpointConnected,
                "stopped Windows named-pipe endpoint remained connectable") && ok;
    ok = expect(client.isReady() && client.isControlAvailable()
                    && client.processGeneration() == secondGeneration,
                "old endpoint probe disturbed the ready replacement generation") && ok;
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
    // creates the selected pipe. The socket attempts remain inside one process
    // generation until the bounded startup deadline; they are not process-level
    // reconnect attempts. The transport must then fail closed without stdio.
    const QString fakeStdioRuntime = QCoreApplication::applicationFilePath();
    ok = expect(QFileInfo(fakeStdioRuntime).isFile(),
                "fake stdio sidecar executable is unavailable") && ok;
    if (QFileInfo(fakeStdioRuntime).isFile()) {
        const bool fakeRuntimePathSet = setWideEnvironmentVariable(
            L"AEGISY_AGENTD_PATH", fakeStdioRuntime);
        if (!expect(fakeRuntimePathSet,
                    "could not set the Unicode fake sidecar path")) {
            return 1;
        }
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
        const quint64 missingPipeGeneration = missingPipeClient.processGeneration();
        AgentRuntimeClientSocketTestAccess::shortenActiveStartupTimeout(
            missingPipeClient, 750);
        ok = expect(missingPipeClient.runtimePath()
                        == QFileInfo(fakeStdioRuntime).absoluteFilePath(),
                    "selected named-pipe failure did not use the requested "
                    "Unicode child executable")
            && ok;
        ok = expect(waitUntil([&]() {
            return missingPipeClient.reconnectState()
                == AgentRuntimeClient::ReconnectState::Exhausted;
        }, 3000),
                    "selected Windows named-pipe failure did not reach its "
                    "startup deadline")
            && ok;
        ok = expect(waitUntil([&]() {
            return AgentRuntimeClientSocketTestAccess::processState(missingPipeClient)
                == QProcess::NotRunning;
        }, 3000), "selected Windows named-pipe failure did not stop its fake child") && ok;
        ok = expect(!fallbackInitialized && !missingPipeClient.isReady(),
                    "selected named-pipe failure silently fell back to stdio") && ok;
        ok = expect(AgentRuntimeClientSocketTestAccess::localSocketAttemptEpoch(
                        missingPipeClient) > 1,
                    "selected named-pipe failure did not retry the local endpoint") && ok;
        ok = expect(AgentRuntimeClientSocketTestAccess::automaticReconnectSuppressed(
                        missingPipeClient)
                        && missingPipeClient.reconnectAttempt() == 0
                        && missingPipeClient.processGeneration() == missingPipeGeneration,
                    "selected named-pipe startup failure incorrectly entered process reconnect")
            && ok;
        missingPipeClient.stop();
        qunsetenv("AEGISY_WINDOWS_PIPE_FAKE_STDIO");
        clearWideEnvironmentVariable(L"AEGISY_AGENTD_PATH");
    }

    // Establish that this Windows host can reach a local pipe through its
    // remote-form path before treating a rejected remote-form connection as a
    // security result. The control deliberately omits PIPE_REJECT_REMOTE_CLIENTS.
    const QString remoteControlPipe = QStringLiteral("aegisy-agent-remote-control-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'))
                 .left(16).toLower());
    const QString remoteControlLocalPath = localPipePath(remoteControlPipe);
    const QString remoteControlPath = remotePipePath(remoteControlPipe);
    HANDLE remoteControlServer = createCurrentUserControlPipe(remoteControlLocalPath);
    HANDLE remoteControlEvent = remoteControlServer != INVALID_HANDLE_VALUE
        ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    OVERLAPPED remoteControlConnect{};
    remoteControlConnect.hEvent = remoteControlEvent;
    bool remoteControlWaiting = false;
    bool remoteControlPending = false;
    if (remoteControlServer != INVALID_HANDLE_VALUE && remoteControlEvent != nullptr) {
        const BOOL connected = ConnectNamedPipe(remoteControlServer, &remoteControlConnect);
        const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
        remoteControlPending = connectError == ERROR_IO_PENDING;
        remoteControlWaiting = connected || connectError == ERROR_IO_PENDING
            || connectError == ERROR_PIPE_CONNECTED;
        if (connectError == ERROR_PIPE_CONNECTED) SetEvent(remoteControlEvent);
    }
    DWORD remoteControlError = ERROR_SUCCESS;
    HANDLE remoteControlClient = remoteControlWaiting && !remoteControlPath.isEmpty()
        ? openPipeWithRetry(remoteControlPath, 3000, &remoteControlError)
        : INVALID_HANDLE_VALUE;
    const bool remoteControlConnected = remoteControlClient != INVALID_HANDLE_VALUE;
    if (!remoteControlConnected) {
        std::cerr << "remote-form named-pipe control failed with Windows error "
                  << remoteControlError << std::endl;
    }
    ok = expect(remoteControlWaiting && remoteControlConnected,
                "host cannot prove remote-form named-pipe connectivity with the control pipe")
        && ok;
    if (remoteControlClient != INVALID_HANDLE_VALUE) CloseHandle(remoteControlClient);
    bool remoteControlConnectFinished = true;
    if (remoteControlServer != INVALID_HANDLE_VALUE) {
        remoteControlConnectFinished = finishOverlappedConnect(
            remoteControlServer, remoteControlEvent, &remoteControlConnect,
            remoteControlPending);
        DisconnectNamedPipe(remoteControlServer);
        CloseHandle(remoteControlServer);
    }
    if (remoteControlEvent != nullptr) CloseHandle(remoteControlEvent);
    ok = expect(remoteControlConnectFinished,
                "remote-form control left overlapped pipe I/O pending") && ok;

    const QString remoteRejectPipe = QStringLiteral("aegisy-agent-remote-reject-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'))
                 .left(16).toLower());
    QProcess remoteRejectListener;
    remoteRejectListener.setProgram(runtimePath);
    remoteRejectListener.setProcessEnvironment(sidecarEnvironment(dataRoot, remoteRejectPipe));
    remoteRejectListener.start();
    ok = expect(remoteRejectListener.waitForStarted(2000),
                "could not start remote-rejection named-pipe listener") && ok;
    const bool remoteRejectReady = waitForPipeAvailable(localPipePath(remoteRejectPipe));
    ok = expect(remoteRejectReady,
                "remote-rejection named-pipe listener did not become available") && ok;
    DWORD remoteRejectError = ERROR_SUCCESS;
    HANDLE rejectedRemoteClient = remoteRejectReady
        ? openPipeWithRetry(remotePipePath(remoteRejectPipe), 1000,
                            &remoteRejectError)
        : INVALID_HANDLE_VALUE;
    const bool remoteFormRejected = remoteRejectReady
        && rejectedRemoteClient == INVALID_HANDLE_VALUE
        && remoteRejectError == ERROR_ACCESS_DENIED;
    if (rejectedRemoteClient != INVALID_HANDLE_VALUE) CloseHandle(rejectedRemoteClient);
    if (!remoteFormRejected) {
        std::cerr << "remote-form rejection returned Windows error "
                  << remoteRejectError << std::endl;
    }
    ok = expect(remoteControlConnected && remoteFormRejected,
                "PIPE_REJECT_REMOTE_CLIENTS did not reject the remote-form client") && ok;
    QLocalSocket remoteRejectLocalClient;
    const bool localStillConnected = waitForPipeConnection(
        remoteRejectLocalClient, remoteRejectPipe, 3000);
    ok = expect(localStillConnected,
                "remote-form rejection consumed or disabled the local named-pipe listener") && ok;
    ok = expect(localStillConnected
                    && pipeRejectsRemoteClients(remoteRejectLocalClient),
                "named-pipe listener did not expose PIPE_REJECT_REMOTE_CLIENTS") && ok;
    if (remoteRejectLocalClient.state() != QLocalSocket::UnconnectedState) {
        remoteRejectLocalClient.disconnectFromServer();
        if (remoteRejectLocalClient.state() != QLocalSocket::UnconnectedState) {
            remoteRejectLocalClient.waitForDisconnected(1000);
        }
    }
    ok = expect(stopChild(remoteRejectListener),
                "remote-rejection named-pipe listener did not clean up") && ok;

    // The selected endpoint must belong to the exact supervised QProcess. A
    // rogue local server in this parent process has the right user but the
    // wrong server PID, so no initialize frame may cross the connection.
    if (QFileInfo(fakeStdioRuntime).isFile()) {
        const bool fakeRuntimePathSet = setWideEnvironmentVariable(
            L"AEGISY_AGENTD_PATH", fakeStdioRuntime);
        if (!expect(fakeRuntimePathSet,
                    "could not set the wrong-PID Unicode fake sidecar path")) {
            return 1;
        }
        qputenv("AEGISY_WINDOWS_PIPE_FAKE_STDIO", QByteArrayLiteral("1"));
        bool rogueInitialized = false;
        bool roguePeerMismatchObserved = false;
        QByteArray rogueBytes;
        QLocalSocket *rogueConnection = nullptr;
        AgentRuntimeClient wrongServerClient(
            nullptr, 50, 250, {0, 20, 40},
            AgentRuntimeClient::TransportMode::VerifiedWindowsNamedPipe);
        QObject::connect(&wrongServerClient, &AgentRuntimeClient::runtimeInitialized,
                         [&rogueInitialized](const QJsonObject &) {
            rogueInitialized = true;
        });
        QObject::connect(&wrongServerClient, &AgentRuntimeClient::connectionStateChanged,
                         [&roguePeerMismatchObserved](bool, const QString &detail) {
            if (detail.contains(QStringLiteral("named-pipe-peer-mismatch"))) {
                roguePeerMismatchObserved = true;
            }
        });
        wrongServerClient.start();
        ok = expect(wrongServerClient.runtimePath()
                        == QFileInfo(fakeStdioRuntime).absoluteFilePath(),
                    "wrong-PID test did not use the requested Unicode fake sidecar")
            && ok;
        const QString rogueEndpoint =
            AgentRuntimeClientSocketTestAccess::endpointName(wrongServerClient);
        const quint64 rogueGeneration = wrongServerClient.processGeneration();
        ok = expect(!rogueEndpoint.isEmpty()
                        && AgentRuntimeClientSocketTestAccess::processState(
                               wrongServerClient) != QProcess::NotRunning,
                    "wrong-PID test did not start its supervised fake sidecar") && ok;
        QLocalServer rogueServer;
        // start() schedules the first socket attempt for a later timer turn, so
        // listen() claims the generated endpoint before this test pumps events.
        const bool rogueListening = !rogueEndpoint.isEmpty()
            && rogueServer.listen(rogueEndpoint);
        QObject::connect(&rogueServer, &QLocalServer::newConnection, [&]() {
            if (rogueConnection != nullptr || !rogueServer.hasPendingConnections()) return;
            rogueConnection = rogueServer.nextPendingConnection();
            QObject::connect(rogueConnection, &QLocalSocket::readyRead, [&]() {
                rogueBytes.append(rogueConnection->readAll());
            });
        });
        ok = expect(rogueListening,
                    "could not install wrong-PID named-pipe server") && ok;
        const bool rogueAccepted = rogueListening && waitUntil([&]() {
            return rogueConnection != nullptr;
        }, 3000);
        ok = expect(rogueAccepted,
                    "wrong-PID named-pipe server did not receive the Qt connection") && ok;
        const bool wrongServerRejected = waitUntil([&]() {
            return roguePeerMismatchObserved;
        }, 3000);
        ok = expect(wrongServerRejected,
                    "Qt accepted a named-pipe server from the wrong process PID") && ok;
        const bool fakeChildStopped = waitUntil([&]() {
            return AgentRuntimeClientSocketTestAccess::processState(wrongServerClient)
                == QProcess::NotRunning;
        }, 3000);
        if (rogueConnection != nullptr) rogueBytes.append(rogueConnection->readAll());
        ok = expect(fakeChildStopped,
                    "wrong-server rejection did not terminate the supervised process generation")
            && ok;
        ok = expect(!rogueInitialized && !wrongServerClient.isReady()
                        && rogueBytes.isEmpty(),
                    "wrong-server rejection sent AAP bytes or fell back to stdio") && ok;
        ok = expect(AgentRuntimeClientSocketTestAccess::automaticReconnectSuppressed(
                        wrongServerClient)
                        && wrongServerClient.reconnectState()
                            == AgentRuntimeClient::ReconnectState::Idle
                        && wrongServerClient.processGeneration() == rogueGeneration,
                    "wrong-server rejection left automatic reconnect active") && ok;
        rogueServer.close();
        wrongServerClient.stop();
        qunsetenv("AEGISY_WINDOWS_PIPE_FAKE_STDIO");
        clearWideEnvironmentVariable(L"AEGISY_AGENTD_PATH");
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
