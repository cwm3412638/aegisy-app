#include "agent_runtime_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonObject>
#include <QThread>

#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <functional>

namespace {
bool expect(bool condition, const char *message)
{
    if (!condition) std::cerr << message << std::endl;
    return condition;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    return condition();
}

QString createPrivateTempRoot()
{
    QByteArray pattern = QByteArrayLiteral("/tmp/aegisy-qt-unix-XXXXXX");
    char *created = ::mkdtemp(pattern.data());
    if (!created) return {};
    if (::chmod(created, 0700) != 0) {
        QDir().rmdir(QString::fromLocal8Bit(created));
        return {};
    }
    return QString::fromLocal8Bit(created);
}

QString shellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
}

int main(int argc, char *argv[])
{
    const QString tempRoot = createPrivateTempRoot();
    if (!expect(!tempRoot.isEmpty(), "could not create private socket test root")) return 1;
    qputenv("TMPDIR", QFile::encodeName(tempRoot + QLatin1Char('/')));

    QCoreApplication application(argc, argv);
    const QString dataRoot = QDir(tempRoot).filePath(QStringLiteral("workbench"));
    if (!expect(QDir().mkpath(dataRoot), "could not create workbench test root")) return 1;
    qputenv("AEGISY_AGENT_BACKEND", QByteArrayLiteral("preview"));
    qputenv("AEGISY_WORKBENCH_DATA_ROOT", QFile::encodeName(dataRoot));
    qunsetenv("AEGISY_AGENTD_PATH");

    QJsonObject initializeResult;
    QString failure;
    QString connectionDetail;
    QString diagnostic;
    AgentRuntimeClient client(
        nullptr, 50, 250, {0, 50, 100},
        AgentRuntimeClient::TransportMode::VerifiedUnixSocket);
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
                     [&connectionDetail](bool, const QString &detail) {
        connectionDetail = detail;
    });
    QObject::connect(&client, &AgentRuntimeClient::diagnosticMessage,
                     [&diagnostic](const QString &message) {
        diagnostic = message;
        std::cerr << "runtime diagnostic: " << message.toStdString() << std::endl;
    });

    client.start();
    bool ok = expect(waitUntil([&]() {
        return client.isReady() || !failure.isEmpty();
    }), "verified Unix socket Runtime did not finish initialization");
    if (!client.isReady()) {
        std::cerr << "connection detail: " << connectionDetail.toStdString()
                  << " diagnostic: " << diagnostic.toStdString()
                  << " initialize failure: " << failure.toStdString() << std::endl;
    }
    ok = expect(failure.isEmpty(), "verified Unix socket initialization failed") && ok;
    ok = expect(client.isReady(), "verified Unix socket Runtime is not ready") && ok;

    const QJsonObject security = initializeResult
        .value(QStringLiteral("transport_security")).toObject();
    ok = expect(security == QJsonObject{
        {QStringLiteral("transport"), QStringLiteral("unix-domain-socket")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), false},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), true},
    }, "Runtime reported incorrect verified Unix socket facts") && ok;

    client.stop();
    const bool endpointRemoved = waitUntil([&]() {
        return QDir(tempRoot).entryList(
            {QStringLiteral("aegisy-agent-*")},
            QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    }, 3000);
    ok = expect(endpointRemoved,
                "verified Unix socket endpoint was not cleaned") && ok;

    const QString wrapperPath = QDir(tempRoot).filePath(QStringLiteral("runtime-wrapper.sh"));
    QFile wrapper(wrapperPath);
    if (!expect(wrapper.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "could not create wrong-peer wrapper")) {
        ok = false;
    } else {
        const QString runtimePath = QStringLiteral(AEGISY_AGENTD_DEV_PATH);
        const QString script = QStringLiteral(
            "#!/bin/sh\n%1 &\nchild=$!\ntrap 'kill \"$child\" 2>/dev/null; wait \"$child\" 2>/dev/null' EXIT TERM INT\nwait \"$child\"\n")
            .arg(shellQuote(runtimePath));
        wrapper.write(script.toUtf8());
        wrapper.close();
        QFile::setPermissions(wrapperPath, QFileDevice::ReadOwner
            | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("AEGISY_AGENTD_PATH", QFile::encodeName(wrapperPath));

        bool sawPeerFailure = false;
        bool sawGenericDisconnectAfterPeerFailure = false;
        AgentRuntimeClient wrongPeerClient(
            nullptr, 50, 250, {0, 50, 100},
            AgentRuntimeClient::TransportMode::VerifiedUnixSocket);
        QObject::connect(&wrongPeerClient,
                         &AgentRuntimeClient::connectionStateChanged,
                         [&sawPeerFailure,
                          &sawGenericDisconnectAfterPeerFailure](
                             bool, const QString &detail) {
            if (detail.contains(QStringLiteral("unix-socket-peer-mismatch"))) {
                sawPeerFailure = true;
            } else if (sawPeerFailure
                       && detail == QStringLiteral("Unix 运行时连接已断开")) {
                sawGenericDisconnectAfterPeerFailure = true;
            }
        });
        QObject::connect(&wrongPeerClient,
                         &AgentRuntimeClient::diagnosticMessage,
                         [](const QString &message) {
            std::cerr << "wrong-peer runtime diagnostic: "
                      << message.toStdString() << std::endl;
        });
        wrongPeerClient.start();
        ok = expect(waitUntil([&]() { return sawPeerFailure; }, 5000),
                    "Qt accepted a Unix socket server with the wrong child PID") && ok;
        ok = expect(!wrongPeerClient.isReady(),
                    "wrong-peer Unix socket entered ready state") && ok;
        const bool wrongPeerEndpointRemoved = waitUntil([&]() {
            return QDir(tempRoot).entryList(
                {QStringLiteral("aegisy-agent-*")},
                QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
        }, 3000);
        wrongPeerClient.stop();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        ok = expect(!sawGenericDisconnectAfterPeerFailure,
                    "generic disconnect overwrote the peer verification failure") && ok;
        if (!wrongPeerEndpointRemoved) {
            const QStringList entries = QDir(tempRoot).entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System
                    | QDir::NoDotAndDotDot);
            std::cerr << "remaining temp entries: "
                      << entries.join(QLatin1Char(',')).toStdString() << std::endl;
            const QStringList socketDirectories = QDir(tempRoot).entryList(
                {QStringLiteral("aegisy-agent-*")},
                QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &directory : socketDirectories) {
                const QString path = QDir(tempRoot).filePath(directory);
                const QStringList children = QDir(path)
                    .entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                               | QDir::NoDotAndDotDot);
                std::cerr << "remaining socket directory "
                          << directory.toStdString() << ": "
                          << children.join(QLatin1Char(',')).toStdString()
                          << std::endl;
            }
        }
        ok = expect(wrongPeerEndpointRemoved,
                    "wrong-peer Unix socket endpoint was not cleaned") && ok;
        qunsetenv("AEGISY_AGENTD_PATH");
    }

    qunsetenv("AEGISY_AGENT_BACKEND");
    qunsetenv("AEGISY_WORKBENCH_DATA_ROOT");
    qunsetenv("TMPDIR");
    QDir(tempRoot).removeRecursively();
    return ok ? 0 : 1;
}
