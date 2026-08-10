#include "agent_runtime_client.h"

#include <QCoreApplication>

#ifdef Q_OS_WIN
#include <stdlib.h>
// qputenv on Windows routes values through the ANSI code page, corrupting
// non-ASCII paths such as the Unicode validation checkout; _wputenv_s keeps
// both the CRT and Win32 environment blocks in sync without conversion.
void putenvUnicode(const char *name, const QString &value)
{
    _wputenv_s(reinterpret_cast<const wchar_t *>(QString::fromLatin1(name).utf16()),
               reinterpret_cast<const wchar_t *>(value.utf16()));
}
#else
void putenvUnicode(const char *name, const QString &value)
{
    qputenv(name, value.toUtf8());
}
#endif
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <iostream>
#include <iterator>

struct AgentRuntimeClientSocketTestAccess {
    static void configure(AgentRuntimeClient &client, QLocalSocket *socket,
                          quint64 generation, quint64 attemptEpoch)
    {
        client.configureLocalSocket(socket, generation, attemptEpoch);
    }

    static void installCurrent(AgentRuntimeClient &client, QLocalSocket *socket,
                               quint64 generation, quint64 attemptEpoch)
    {
        client.m_localSocket = socket;
        client.m_processGeneration = generation;
        client.m_unixSocketConnectGeneration = generation;
        client.m_localSocketAttemptEpoch = attemptEpoch;
        client.m_unixSocketPeerVerifiedGeneration = generation;
        client.m_localSocketPeerVerifiedAttemptEpoch = attemptEpoch;
        client.m_unixSocketDisconnectGeneration = 0;
        client.m_ownedTerminationGeneration = 0;
        client.m_handshakeComplete = true;
    }

    static bool retainedNewConnectionState(const AgentRuntimeClient &client,
                                           QLocalSocket *socket,
                                           quint64 generation,
                                           quint64 attemptEpoch)
    {
        return client.m_localSocket == socket
            && client.m_processGeneration == generation
            && client.m_unixSocketConnectGeneration == generation
            && client.m_localSocketAttemptEpoch == attemptEpoch
            && client.m_unixSocketPeerVerifiedGeneration == generation
            && client.m_localSocketPeerVerifiedAttemptEpoch == attemptEpoch
            && client.m_unixSocketDisconnectGeneration == 0
            && client.m_ownedTerminationGeneration == 0
            && client.m_handshakeComplete
            && !client.m_autoReconnectSuppressed;
    }

    static void retire(AgentRuntimeClient &client)
    {
        client.retireLocalSocket();
    }

    static bool retiredRequestWasCleared(const AgentRuntimeClient &client,
                                         const QString &requestId)
    {
        return !client.m_pendingRequests.contains(requestId)
            && client.m_retiredResponseIds.contains(requestId);
    }

    static bool capabilityWasCleared(const AgentRuntimeClient &client,
                                     const QString &capability)
    {
        return !client.m_negotiatedStableCapabilities.contains(capability);
    }

    static void holdReconnectTimer(AgentRuntimeClient &client)
    {
        client.m_reconnectTimer->blockSignals(true);
    }

    static bool beginWaitingReconnectNow(AgentRuntimeClient &client)
    {
        if (client.m_reconnectState != AgentRuntimeClient::ReconnectState::Waiting
            || client.m_reconnectScheduledGeneration != client.m_processGeneration) {
            return false;
        }
        client.m_reconnectTimer->stop();
        client.m_reconnectTimer->blockSignals(false);
        client.beginReconnectAttempt();
        return true;
    }
};

namespace {

constexpr qsizetype kTestMaximumFrameBytes = 4 * 1024 * 1024;

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

bool staleLocalSocketCallbacksAreInert()
{
    constexpr quint64 currentGeneration = 41;
    constexpr quint64 currentAttemptEpoch = 6;
    const QString serverName = QStringLiteral("as-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    if (!expect(server.listen(serverName),
                "could not create stale-callback local socket server")) {
        return false;
    }
    AgentRuntimeClient client(
        nullptr, 5000, 15000, {0},
        AgentRuntimeClient::TransportMode::VerifiedUnixSocket);
    auto *retiredSocket = new QLocalSocket(&client);
    const QPointer<QLocalSocket> retiredSocketGuard(retiredSocket);
    AgentRuntimeClientSocketTestAccess::configure(
        client, retiredSocket, currentGeneration - 2,
        currentAttemptEpoch - 2);
    AgentRuntimeClientSocketTestAccess::installCurrent(
        client, retiredSocket, currentGeneration - 2,
        currentAttemptEpoch - 2);
    bool queuedCallbacks = QMetaObject::invokeMethod(
        retiredSocket, "readyRead", Qt::QueuedConnection);
    queuedCallbacks = QMetaObject::invokeMethod(
        retiredSocket, "connected", Qt::QueuedConnection) && queuedCallbacks;
    queuedCallbacks = QMetaObject::invokeMethod(
        retiredSocket, "disconnected", Qt::QueuedConnection) && queuedCallbacks;
    AgentRuntimeClientSocketTestAccess::retire(client);

    auto *oldGenerationSocket = new QLocalSocket(&client);
    oldGenerationSocket->connectToServer(serverName, QIODevice::ReadWrite);
    if (!expect(oldGenerationSocket->waitForConnected(1000)
                    && server.waitForNewConnection(1000),
                "could not create old-generation local socket fixture")) {
        return false;
    }
    QLocalSocket *fixturePeer = server.nextPendingConnection();
    if (!expect(fixturePeer != nullptr,
                "stale-callback local socket fixture has no peer")) {
        return false;
    }
    const QByteArray staleBytes = QByteArrayLiteral("stale-aap-frame\n");
    const qint64 queuedBytes = fixturePeer->write(staleBytes);
    const bool bytesFlushed = fixturePeer->bytesToWrite() == 0
        || fixturePeer->waitForBytesWritten(1000);
    if (!expect(queuedBytes == staleBytes.size() && bytesFlushed
                    && oldGenerationSocket->waitForReadyRead(1000),
                "could not queue bytes on the old-generation local socket")) {
        return false;
    }
    auto *oldAttemptSocket = new QLocalSocket(&client);
    AgentRuntimeClientSocketTestAccess::configure(
        client, oldGenerationSocket, currentGeneration - 1,
        currentAttemptEpoch - 1);
    AgentRuntimeClientSocketTestAccess::configure(
        client, oldAttemptSocket, currentGeneration,
        currentAttemptEpoch - 1);
    AgentRuntimeClientSocketTestAccess::installCurrent(
        client, oldAttemptSocket, currentGeneration, currentAttemptEpoch);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    const auto emitStaleCallbacks = [](QLocalSocket *socket) {
        bool invoked = QMetaObject::invokeMethod(
            socket, "readyRead", Qt::DirectConnection);
        invoked = QMetaObject::invokeMethod(
            socket, "connected", Qt::DirectConnection) && invoked;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        invoked = QMetaObject::invokeMethod(
            socket, "errorOccurred", Qt::DirectConnection,
            Q_ARG(QLocalSocket::LocalSocketError,
                  QLocalSocket::UnknownSocketError)) && invoked;
#else
        invoked = QMetaObject::invokeMethod(
            socket, "error", Qt::DirectConnection,
            Q_ARG(QLocalSocket::LocalSocketError,
                  QLocalSocket::UnknownSocketError)) && invoked;
#endif
        invoked = QMetaObject::invokeMethod(
            socket, "disconnected", Qt::DirectConnection) && invoked;
        return invoked;
    };

    bool ok = expect(queuedCallbacks,
                     "could not queue callbacks for a retired local socket");
    ok = expect(!retiredSocketGuard,
                "retired local socket was not deleted after queued callbacks") && ok;
    ok = expect(AgentRuntimeClientSocketTestAccess::retainedNewConnectionState(
                    client, oldAttemptSocket, currentGeneration,
                    currentAttemptEpoch),
                "queued retired-socket callbacks changed the current connection")
        && ok;
    ok = expect(emitStaleCallbacks(oldGenerationSocket),
                "could not emit old-generation local socket callbacks") && ok;
    ok = expect(oldGenerationSocket->bytesAvailable() == staleBytes.size(),
                "old-generation readyRead consumed stale transport bytes") && ok;
    ok = expect(AgentRuntimeClientSocketTestAccess::retainedNewConnectionState(
                    client, oldAttemptSocket, currentGeneration,
                    currentAttemptEpoch),
                "old-generation local socket callbacks changed the current connection")
        && ok;
    ok = expect(emitStaleCallbacks(oldAttemptSocket),
                "could not emit old-attempt local socket callbacks") && ok;
    ok = expect(AgentRuntimeClientSocketTestAccess::retainedNewConnectionState(
                    client, oldAttemptSocket, currentGeneration,
                    currentAttemptEpoch),
                "old-attempt local socket callbacks changed the current connection")
        && ok;
    AgentRuntimeClientSocketTestAccess::retire(client);
    fixturePeer->disconnectFromServer();
    oldGenerationSocket->disconnectFromServer();
    server.close();
    QLocalServer::removeServer(serverName);
    return ok;
}

void appendLogLine(QFile *log, const QByteArray &line)
{
    log->write(line);
    log->write("\n");
    log->flush();
}

QJsonObject testPlatform()
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
        {QStringLiteral("os"), os},
        {QStringLiteral("architecture"), architecture},
    };
}

QJsonObject testTransportSecurity()
{
    return {
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), true},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), false},
    };
}

QJsonObject validInitializeResult()
{
    return {
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
        {QStringLiteral("platform"), testPlatform()},
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
        {QStringLiteral("transport_security"), testTransportSecurity()},
    };
}

QString testTerminalPlatformCapability()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("terminal.pty.macos.user-initiated");
#elif defined(Q_OS_WIN)
    return QStringLiteral("terminal.conpty.windows.user-initiated");
#else
    return QStringLiteral("terminal.pty.unsupported");
#endif
}

QString timelineEventId(QLatin1Char fill)
{
    return QStringLiteral("event:sha256:") + QString(64, fill);
}

QJsonObject timelineAnchor(int sequence, const QJsonValue &eventId)
{
    return {
        {QStringLiteral("sequence"), sequence},
        {QStringLiteral("event_id"), eventId},
    };
}

QJsonObject timelineSyncPage(const QJsonObject &requestParams)
{
    const QJsonObject after = requestParams.value(QStringLiteral("after")).toObject();
    QJsonValue watermark = requestParams.value(QStringLiteral("watermark"));
    if (watermark.isNull()) {
        watermark = timelineAnchor(0, QJsonValue(QJsonValue::Null));
    }
    return {
        {QStringLiteral("schema_version"), QStringLiteral("timeline-sync-page/0.1")},
        {QStringLiteral("session_id"),
         requestParams.value(QStringLiteral("session_id"))},
        {QStringLiteral("after"), after},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("events"), QJsonArray{}},
        {QStringLiteral("next_after"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("complete"), true},
    };
}

QJsonObject timelineSnapshotPage(const QJsonObject &requestParams)
{
    const QJsonValue after = requestParams.value(QStringLiteral("after"));
    const bool continuation = !after.isNull();
    const QString snapshotIdentity =
        QStringLiteral("timeline-session-snapshot:sha256:")
        + QString(64, QLatin1Char('a'));
    const QJsonObject watermark = timelineAnchor(
        3, timelineEventId(QLatin1Char('b')));
    const QJsonObject item{
        {QStringLiteral("ordinal"), 1},
        {QStringLiteral("item_identity"),
         QStringLiteral("timeline-session-snapshot-item:sha256:")
             + QString(64, QLatin1Char('c'))},
        {QStringLiteral("turn_id"), QStringLiteral("turn-1")},
        {QStringLiteral("correlation_id"), QStringLiteral("turn-1")},
        {QStringLiteral("turn_state"), QStringLiteral("completed")},
        {QStringLiteral("first_event"), timelineAnchor(
            1, timelineEventId(QLatin1Char('1')))},
        {QStringLiteral("latest_event"), timelineAnchor(
            3, timelineEventId(QLatin1Char('b')))},
        {QStringLiteral("item"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("item-1")},
            {QStringLiteral("kind"), QStringLiteral("message")},
            {QStringLiteral("role"), QStringLiteral("agent")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("content"), QStringLiteral("snapshot")},
        }},
        {QStringLiteral("item_update"), QJsonObject{
            {QStringLiteral("revision"), 1},
            {QStringLiteral("content_mode"), QStringLiteral("snapshot-replacement")},
        }},
    };
    const QJsonArray items = continuation ? QJsonArray{} : QJsonArray{item};
    const QJsonValue nextAfter = continuation
        ? QJsonValue(QJsonValue::Null)
        : QJsonValue(QJsonObject{
            {QStringLiteral("ordinal"), 1},
            {QStringLiteral("item_id"), QStringLiteral("item-1")},
            {QStringLiteral("item_identity"), item.value(QStringLiteral("item_identity"))},
        });
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-session-snapshot-page/0.1")},
        {QStringLiteral("session_id"), requestParams.value(QStringLiteral("session_id"))},
        {QStringLiteral("snapshot_identity"), snapshotIdentity},
        {QStringLiteral("floor"), timelineAnchor(
            0, QJsonValue(QJsonValue::Null))},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("active_turn"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("total_items"), 1},
        {QStringLiteral("total_canonical_bytes"), 1},
        {QStringLiteral("after"), after},
        {QStringLiteral("items"), items},
        {QStringLiteral("next_after"), nextAfter},
        {QStringLiteral("complete"), continuation},
        {QStringLiteral("page_identity"),
         QStringLiteral("timeline-session-snapshot-page:sha256:")
             + QString(64, QLatin1Char('d'))},
    };
}

QJsonObject validTimelineEnvelope()
{
    const QString sessionId(128, QLatin1Char('s'));
    const QString turnId(128, QLatin1Char('t'));
    QJsonObject event{
        {QStringLiteral("schema_version"), QStringLiteral("timeline-event/0.1")},
        {QStringLiteral("event_id"), QString()},
        {QStringLiteral("sequence"), 1},
        {QStringLiteral("timestamp_ms"), 1'000},
        {QStringLiteral("correlation_id"), turnId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("turn_id"), turnId},
        {QStringLiteral("turn_state"), QStringLiteral("running")},
        {QStringLiteral("event"), QStringLiteral("turn.started")},
        {QStringLiteral("item"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("item_update"), QJsonValue(QJsonValue::Null)},
    };
    event.insert(QStringLiteral("event_id"),
                 AgentRuntimeClient::timelineEventIdentity(event));
    return event;
}

QJsonObject timelineDataEnvelope(const QJsonValue &data)
{
    QJsonObject event = validTimelineEnvelope();
    event.insert(QStringLiteral("event"), QStringLiteral("item.completed"));
    event.insert(QStringLiteral("item"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("item-1")},
        {QStringLiteral("kind"), QStringLiteral("message")},
        {QStringLiteral("role"), QStringLiteral("agent")},
        {QStringLiteral("state"), QStringLiteral("completed")},
        {QStringLiteral("content"), QStringLiteral("bounded")},
        {QStringLiteral("data"), data},
    });
    event.insert(QStringLiteral("item_update"), QJsonObject{
        {QStringLiteral("revision"), 1},
        {QStringLiteral("content_mode"), QStringLiteral("snapshot-replacement")},
    });
    event.insert(QStringLiteral("event_id"),
                 AgentRuntimeClient::timelineEventIdentity(event));
    return event;
}

QJsonObject validLargeGenericTimelineEnvelope()
{
    QJsonObject event = timelineDataEnvelope(QJsonObject{
        {QStringLiteral("payload"), QString(300 * 1024, QLatin1Char('d'))},
    });
    QJsonObject item = event.value(QStringLiteral("item")).toObject();
    item.insert(QStringLiteral("kind"),
                QStringLiteral("a") + QString(63, QLatin1Char('g')));
    event.insert(QStringLiteral("item"), item);
    event.insert(QStringLiteral("event_id"),
                 AgentRuntimeClient::timelineEventIdentity(event));
    return event;
}

QJsonObject validMathematicalIntegerTimelineEnvelope()
{
    return timelineDataEnvelope(QJsonObject{
        {QStringLiteral("maximum"), 9'007'199'254'740'991.0},
        {QStringLiteral("minimum"), -9'007'199'254'740'991.0},
        {QStringLiteral("negative_zero"), 0},
        {QStringLiteral("one"), 1},
        {QStringLiteral("thousand"), 1'000},
    });
}

QByteArray rawMathematicalIntegerTimelineNotification()
{
    const QJsonObject event = validMathematicalIntegerTimelineEnvelope();
    QByteArray frame = QJsonDocument(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), QStringLiteral("event")},
        {QStringLiteral("params"), event},
    }).toJson(QJsonDocument::Compact);
    if (!frame.contains("\"sequence\":1")
            || !frame.contains("\"timestamp_ms\":1000")
            || !frame.contains("\"negative_zero\":0")
            || !frame.contains("\"one\":1")
            || !frame.contains("\"thousand\":1000")) {
        return {};
    }
    frame.replace("\"sequence\":1", "\"sequence\":1.0");
    frame.replace("\"timestamp_ms\":1000", "\"timestamp_ms\":1e3");
    frame.replace("\"negative_zero\":0", "\"negative_zero\":-0.0");
    frame.replace("\"one\":1", "\"one\":1.0");
    frame.replace("\"thousand\":1000", "\"thousand\":1e3");
    return frame;
}

QByteArray exactBoundaryTimelineNotification()
{
    QJsonObject event = timelineDataEnvelope(QJsonObject{
        {QStringLiteral("payload"), QString()},
    });
    const auto encode = [&event]() {
        event.insert(QStringLiteral("event_id"),
                     AgentRuntimeClient::timelineEventIdentity(event));
        return QJsonDocument(QJsonObject{
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("method"), QStringLiteral("event")},
            {QStringLiteral("params"), event},
        }).toJson(QJsonDocument::Compact);
    };
    const QByteArray emptyFrame = encode();
    const qsizetype payloadBytes = kTestMaximumFrameBytes - emptyFrame.size();
    if (payloadBytes <= 0) return {};
    QJsonObject item = event.value(QStringLiteral("item")).toObject();
    item.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("payload"), QString(payloadBytes, QLatin1Char('d'))},
    });
    event.insert(QStringLiteral("item"), item);
    const QByteArray frame = encode();
    return frame.size() == kTestMaximumFrameBytes ? frame : QByteArray{};
}

void writeRawFakeRuntimeFrame(const QByteArray &frame)
{
    std::cout.write(frame.constData(), std::streamsize(frame.size()));
    std::cout.put('\n');
    std::cout.flush();
}

QByteArray rawTransportParserViolation(const QString &testCase)
{
    if (testCase == QStringLiteral("transport-raw-leading-bom")) {
        QByteArray frame("\xef\xbb\xbf", 3);
        frame.append(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{}})");
        return frame;
    }
    if (testCase == QStringLiteral("transport-raw-invalid-utf8")) {
        QByteArray frame(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{"value":")");
        frame.append(char(0xff));
        frame.append(R"("}})");
        return frame;
    }
    if (testCase == QStringLiteral("transport-raw-duplicate-decoded-key")) {
        return QByteArrayLiteral(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{"a":1,"\u0061":2}})");
    }
    if (testCase == QStringLiteral("transport-raw-lone-surrogate")) {
        return QByteArrayLiteral(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{"value":"\ud800"}})");
    }
    if (testCase == QStringLiteral("transport-raw-depth-129")) {
        QByteArray frame = QByteArrayLiteral(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{"value":)");
        frame.append(QByteArray(129, '['));
        frame.append("null");
        frame.append(QByteArray(129, ']'));
        frame.append("}}");
        return frame;
    }
    if (testCase == QStringLiteral("transport-raw-node-65537")) {
        QByteArray frame = QByteArrayLiteral(
            R"({"jsonrpc":"2.0","method":"future/notice","params":{"value":[)");
        for (int index = 0; index < 65'537; ++index) {
            if (index > 0) frame.append(',');
            frame.append("null");
        }
        frame.append("]}}");
        return frame;
    }
    return {};
}

QString subscriptionIdentityStage(const QString &method)
{
    if (method == QStringLiteral("timeline/subscribe")) {
        return QStringLiteral("subscribe");
    }
    if (method == QStringLiteral("timeline/subscription-sync")) {
        return QStringLiteral("subscription-sync");
    }
    if (method == QStringLiteral("timeline/subscription-snapshot")) {
        return QStringLiteral("subscription-snapshot");
    }
    if (method == QStringLiteral("timeline/subscription-activate")) {
        return QStringLiteral("activate");
    }
    return {};
}

QString subscriptionFailureStage(const QString &method)
{
    if (method == QStringLiteral("timeline/subscribe")) {
        return QStringLiteral("subscribe");
    }
    if (method == QStringLiteral("timeline/subscription-sync")) {
        return QStringLiteral("sync");
    }
    if (method == QStringLiteral("timeline/subscription-snapshot")) {
        return QStringLiteral("snapshot");
    }
    if (method == QStringLiteral("timeline/subscription-activate")) {
        return QStringLiteral("activate");
    }
    return {};
}

QJsonObject subscriptionFailureForRequest(const QString &method,
                                          const QJsonObject &request,
                                          const QString &drift)
{
    QJsonValue cursor;
    QJsonValue watermark;
    if (method == QStringLiteral("timeline/subscribe")
        || method == QStringLiteral("timeline/subscription-activate")) {
        cursor = request.value(QStringLiteral("cursor"));
        watermark = request.value(QStringLiteral("watermark"));
    } else if (method == QStringLiteral("timeline/subscription-sync")) {
        const QJsonObject nested = request.value(QStringLiteral("request")).toObject();
        cursor = nested.value(QStringLiteral("after"));
        watermark = nested.value(QStringLiteral("watermark"));
    } else {
        const QJsonObject nested = request.value(QStringLiteral("request")).toObject();
        cursor = timelineAnchor(0, QJsonValue(QJsonValue::Null));
        watermark = nested.value(QStringLiteral("watermark"));
    }
    QString stage = subscriptionFailureStage(method);
    if (drift == QStringLiteral("stage")) {
        stage = stage == QStringLiteral("subscribe")
            ? QStringLiteral("sync") : QStringLiteral("subscribe");
    }
    QString requestIdentity = AgentRuntimeClient::timelineSubscriptionRequestIdentity(
        subscriptionIdentityStage(method), request);
    if (drift == QStringLiteral("identity")) {
        requestIdentity = QStringLiteral("timeline-subscription-request:sha256:")
            + QString(64, QLatin1Char('f'));
    }
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-failure/0.1")},
        {QStringLiteral("connection_generation"),
         request.value(QStringLiteral("connection_generation"))},
        {QStringLiteral("session_id"), request.value(QStringLiteral("session_id"))},
        {QStringLiteral("subscription_id"),
         request.value(QStringLiteral("subscription_id"))},
        {QStringLiteral("state"), QStringLiteral("failed")},
        {QStringLiteral("stage"), stage},
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("request_identity"), requestIdentity},
        {QStringLiteral("reason"), QStringLiteral("transport")},
        {QStringLiteral("retryable"), true},
        {QStringLiteral("cleanup_required"), true},
    };
}

QString testMutationFingerprint()
{
    return QString(64, QLatin1Char('a'));
}

QString testMutationIdempotencyKey()
{
    return QStringLiteral("gesture-transport-test");
}

QString testMutationOperationIdentity(const QString &sessionId)
{
    return AgentRuntimeClient::durableMutationOperationIdentity(
        sessionId, QStringLiteral("turn-start"), testMutationIdempotencyKey(),
        testMutationFingerprint());
}

QJsonObject testMutationOperation(const QString &sessionId, int revision,
                                  bool acceptedConsumed)
{
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("mutation-acknowledgement-operation/0.1")},
        {QStringLiteral("operation_identity"),
         testMutationOperationIdentity(sessionId)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("mutation_kind"), QStringLiteral("turn-start")},
        {QStringLiteral("idempotency_key"), testMutationIdempotencyKey()},
        {QStringLiteral("request_fingerprint"), testMutationFingerprint()},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("state"), QStringLiteral("accepted")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-transport-test")},
        {QStringLiteral("accepted_anchor"),
         timelineAnchor(1, timelineEventId(QLatin1Char('a')))},
        {QStringLiteral("terminal_anchor"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("accepted_consumed"), acceptedConsumed},
        {QStringLiteral("terminal_consumed"), false},
    };
}

QJsonObject mutationListResult(const QJsonObject &request, bool drift)
{
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("mutation-acknowledgement-page/0.1")},
        {QStringLiteral("session_id"), drift
             ? QJsonValue(QStringLiteral("session-drift"))
             : request.value(QStringLiteral("session_id"))},
        {QStringLiteral("after"), request.value(QStringLiteral("after"))},
        {QStringLiteral("operations"), QJsonArray{}},
        {QStringLiteral("next_after"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("complete"), true},
    };
}

QJsonObject mutationConsumeResult(const QJsonObject &request, bool drift)
{
    QJsonObject confirmedAnchor = request.value(
        QStringLiteral("confirmed_anchor")).toObject();
    if (drift) {
        confirmedAnchor = timelineAnchor(2, timelineEventId(QLatin1Char('b')));
    }
    QJsonObject operation = testMutationOperation(
        request.value(QStringLiteral("session_id")).toString(),
        request.value(QStringLiteral("expected_revision")).toInt() + 1, true);
    operation.insert(QStringLiteral("accepted_anchor"), confirmedAnchor);
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("mutation-acknowledgement-consume-result/0.1")},
        {QStringLiteral("session_id"), request.value(QStringLiteral("session_id"))},
        {QStringLiteral("operation_identity"),
         request.value(QStringLiteral("operation_identity"))},
        {QStringLiteral("expected_revision"),
         request.value(QStringLiteral("expected_revision"))},
        {QStringLiteral("target"), request.value(QStringLiteral("target"))},
        {QStringLiteral("confirmed_anchor"), confirmedAnchor},
        {QStringLiteral("operation"), operation},
    };
}

bool verifyRustTimelineIdentityFixture()
{
    const QList<QByteArray> fixtureLines{
        QByteArrayLiteral(R"({"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:a3ea83f9fd0b70adafafe203fc62f1a82a78b42a652e82615725a858d411236a","sequence":1,"timestamp_ms":1784851200001,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"turn.started","item":null,"item_update":null}})"),
        QByteArrayLiteral(R"({"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:6cf6e2279d2c733558d9ad3b6c7f35431a850328a14f8686a4a53ba0c1f00798","sequence":2,"timestamp_ms":1784851200002,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"item.completed","item":{"id":"item-user-1","kind":"message","role":"user","state":"completed","content":"Inspect the project."},"item_update":{"revision":1,"content_mode":"snapshot-replacement"}}})"),
        QByteArrayLiteral(R"({"jsonrpc":"2.0","method":"event","params":{"schema_version":"timeline-event/0.1","event_id":"event:sha256:a8e4ae9fca35aebb4b095f0276dfa1955736bd8e632cbfdb58bb49dd20c4cf63","sequence":3,"timestamp_ms":1784851200003,"correlation_id":"turn-1","session_id":"session-1","turn_id":"turn-1","turn_state":"running","event":"item.started","item":{"id":"item-command-1","kind":"command","role":"tool","state":"started","content":"Command started","data":{"risk":"read-only","canonical":{"array":[0,-1,9007199254740991,"quote\" slash\\ newline\n 界"],"ordered":{"z":2,"a":1}}}},"item_update":{"revision":1,"content_mode":"snapshot-replacement"}}})"),
    };
    for (const QByteArray &line : fixtureLines) {
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) return false;
        const QJsonObject event = document.object().value(
            QStringLiteral("params")).toObject();
        if (event.value(QStringLiteral("event_id")).toString()
            != AgentRuntimeClient::timelineEventIdentity(event)) {
            return false;
        }
    }
    const QByteArray mathematicalFrame = rawMathematicalIntegerTimelineNotification();
    const QJsonDocument mathematicalDocument = QJsonDocument::fromJson(
        mathematicalFrame);
    if (mathematicalFrame.isEmpty() || !mathematicalDocument.isObject()) return false;
    const QJsonObject mathematicalEvent = mathematicalDocument.object().value(
        QStringLiteral("params")).toObject();
    return mathematicalEvent.value(QStringLiteral("event_id")).toString()
        == AgentRuntimeClient::timelineEventIdentity(mathematicalEvent);
}

bool verifyRustTimelineSnapshotIdentityFixture()
{
    const QString itemIdentity =
        QStringLiteral("timeline-session-snapshot-item:sha256:")
        + QStringLiteral("c64cd9d8d5683b2b3365cad4e18647b5b96fb97ce3b05a16cf69dccbfec24b0b");
    const QJsonObject itemPage{
        {QStringLiteral("ordinal"), 1},
        {QStringLiteral("item_identity"), itemIdentity},
        {QStringLiteral("turn_id"), QStringLiteral("turn-1")},
        {QStringLiteral("correlation_id"), QStringLiteral("turn-1")},
        {QStringLiteral("turn_state"), QStringLiteral("running")},
        {QStringLiteral("first_event"), timelineAnchor(
            2, timelineEventId(QLatin1Char('2')))},
        {QStringLiteral("latest_event"), timelineAnchor(
            2, timelineEventId(QLatin1Char('2')))},
        {QStringLiteral("item"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("item-user-1")},
            {QStringLiteral("kind"), QStringLiteral("message")},
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("content"), QStringLiteral("Inspect the project.")},
        }},
        {QStringLiteral("item_update"), QJsonObject{
            {QStringLiteral("revision"), 1},
            {QStringLiteral("content_mode"), QStringLiteral("snapshot-replacement")},
        }},
    };
    const QString secondIdentity =
        QStringLiteral("timeline-session-snapshot-item:sha256:")
        + QStringLiteral("cb04df50b58cf6e5e94d2e1b6df5ce967ca3dbe8e971579f5dec57ef94dd5d90");
    const QJsonObject floor = timelineAnchor(0, QJsonValue(QJsonValue::Null));
    const QJsonObject watermark = timelineAnchor(
        3, timelineEventId(QLatin1Char('b')));
    const QJsonObject activeTurn{
        {QStringLiteral("turn_id"), QStringLiteral("turn-1")},
        {QStringLiteral("correlation_id"), QStringLiteral("turn-1")},
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("started_event"), timelineAnchor(
            1, timelineEventId(QLatin1Char('1')))},
        {QStringLiteral("latest_event"), watermark},
        {QStringLiteral("open_item_ids"), QJsonArray{}},
    };
    const QString snapshotIdentity = AgentRuntimeClient::timelineSnapshotIdentity(
        QStringLiteral("session-1"), floor, watermark, activeTurn,
        2, 1170, {itemIdentity, secondIdentity});
    const QJsonObject page{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-session-snapshot-page/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-1")},
        {QStringLiteral("snapshot_identity"), snapshotIdentity},
        {QStringLiteral("floor"), floor},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("active_turn"), activeTurn},
        {QStringLiteral("total_items"), 2},
        {QStringLiteral("total_canonical_bytes"), 1170},
        {QStringLiteral("after"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("items"), QJsonArray{itemPage}},
        {QStringLiteral("next_after"), QJsonObject{
            {QStringLiteral("ordinal"), 1},
            {QStringLiteral("item_id"), QStringLiteral("item-user-1")},
            {QStringLiteral("item_identity"), itemIdentity},
        }},
        {QStringLiteral("complete"), false},
        {QStringLiteral("page_identity"), QString()},
    };
    const QString pageIdentity = AgentRuntimeClient::timelineSnapshotPageIdentity(page);
    return expect(AgentRuntimeClient::timelineSnapshotItemIdentity(
                      QStringLiteral("session-1"), itemPage) == itemIdentity,
                  "Qt Timeline snapshot Item identity diverged from Rust fixture")
        && expect(AgentRuntimeClient::timelineSnapshotItemCanonicalBytes(
                       QStringLiteral("session-1"), itemPage) > 0,
                   "Qt Timeline snapshot Item canonical bytes were not bounded")
        && expect(snapshotIdentity
                      == QStringLiteral("timeline-session-snapshot:sha256:")
                             + QStringLiteral("93c2795bd0ed77feabfe115eaa270f732f42b076fef6a91b4ed1f29263e2bdfb"),
                   "Qt Timeline snapshot identity diverged from Rust fixture")
        && expect(pageIdentity
                      == QStringLiteral("timeline-session-snapshot-page:sha256:")
                             + QStringLiteral("93c541f5fea427a36f71da915789dd766944be0d31a226799b8573f40d9fc5f9"),
                   "Qt Timeline snapshot page identity diverged from Rust fixture");
}

QJsonObject validSinglePageCommandArtifactResult(const QJsonObject &request)
{
    const QByteArray source("abcdef");
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(
        source, QCryptographicHash::Sha256).toHex());
    const QString reference = QStringLiteral("command-output:sha256:") + sha256;
    const QString mediaType = QStringLiteral("text/plain; charset=utf-8");
    const int pageSize = request.value(QStringLiteral("limit")).toInt();
    QJsonObject limits{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-inline-limits/0.1")},
        {QStringLiteral("max_item_bytes"), pageSize},
        {QStringLiteral("max_total_bytes"),
         request.value(QStringLiteral("max_total_inline_bytes"))},
    };
    limits.insert(QStringLiteral("identity"),
                  AgentRuntimeClient::contentInlineLimitsIdentity(limits));
    QJsonObject preview{
        {QStringLiteral("schema_version"), QStringLiteral("content-preview/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("content_bytes"), source.size()},
        {QStringLiteral("preview_bytes"), source.size()},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("line_count"), 1},
        {QStringLiteral("width"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("height"), QJsonValue(QJsonValue::Null)},
    };
    preview.insert(QStringLiteral("identity"),
                   AgentRuntimeClient::contentPreviewIdentity(preview));
    const QJsonObject content{
        {QStringLiteral("schema_version"), QStringLiteral("content-reference/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), source.size()},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("preview"), preview},
    };
    QJsonObject result{
        {QStringLiteral("schema_version"),
         QStringLiteral("command-output-artifact-page/0.1")},
        {QStringLiteral("session_id"), request.value(QStringLiteral("session_id"))},
        {QStringLiteral("item_id"), request.value(QStringLiteral("item_id"))},
        {QStringLiteral("created_at_ms"), 1'700'000'000'123.0},
        {QStringLiteral("content_reference"), content},
        {QStringLiteral("source_bytes"), source.size()},
        {QStringLiteral("redacted_count"), 0},
        {QStringLiteral("redacted"), false},
        {QStringLiteral("total_bytes"), source.size()},
        {QStringLiteral("retained_bytes"), source.size()},
        {QStringLiteral("omitted_bytes"), 0},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("read_only"), true},
    };
    result.insert(QStringLiteral("binding_identity"),
                  AgentRuntimeClient::commandArtifactPageBindingIdentity(result));
    QJsonObject page{
        {QStringLiteral("schema_version"), QStringLiteral("content-reference-page/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), source.size()},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("page_size"), pageSize},
        {QStringLiteral("page_bytes"), source.size()},
        {QStringLiteral("inline"), QString::fromUtf8(source)},
        {QStringLiteral("inline_truncated"), false},
        {QStringLiteral("limits"), limits},
        {QStringLiteral("binding_identity"),
         result.value(QStringLiteral("binding_identity"))},
        {QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null)},
    };
    page.insert(QStringLiteral("identity"),
                AgentRuntimeClient::contentReferencePageIdentity(page));
    result.insert(QStringLiteral("page"), page);
    return result;
}

int runFakeRuntime(const QString &testCase)
{
    const QString logPath = qEnvironmentVariable("AEGISY_FAKE_RUNTIME_LOG");
    int priorInitializeCount = 0;
    QString priorRuntimeHealthId;
    QFile history(logPath);
    if (history.open(QIODevice::ReadOnly)) {
        while (!history.atEnd()) {
            const QJsonDocument prior = QJsonDocument::fromJson(history.readLine());
            if (!prior.isObject()) continue;
            const QJsonObject priorMessage = prior.object();
            const QString priorMethod = priorMessage.value(
                QStringLiteral("method")).toString();
            if (priorMethod == QStringLiteral("initialize")) {
                ++priorInitializeCount;
            } else if (priorMethod == QStringLiteral("runtime/health")
                       && priorMessage.value(QStringLiteral("id")).isString()) {
                priorRuntimeHealthId = priorMessage.value(
                    QStringLiteral("id")).toString();
            }
        }
    }
    const int invocation = priorInitializeCount + 1;
    QFile log(logPath);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 90;

    // The client authenticates the transport before any AAP frame: the first
    // line must be the one-time bootstrap prelude carrying the exact token
    // passed through the launch environment. It is intentionally not logged.
    std::string rawLine;
    if (!std::getline(std::cin, rawLine)) return 91;
    const QByteArray preludeLine = QByteArray::fromStdString(rawLine);
    const QJsonDocument preludeDocument = QJsonDocument::fromJson(preludeLine);
    if (!preludeDocument.isObject()) return 93;
    const QJsonObject prelude = preludeDocument.object();
    const QString expectedToken = qEnvironmentVariable("AEGISY_BOOTSTRAP_TOKEN");
    if (expectedToken.isEmpty()
        || prelude.value(QStringLiteral("schema")).toString()
            != QStringLiteral("aegisy-bootstrap-auth/0.1")
        || prelude.value(QStringLiteral("token")).toString() != expectedToken
        || prelude.size() != 2) {
        return 94;
    }
    if (!std::getline(std::cin, rawLine)) return 91;
    const QByteArray firstLine = QByteArray::fromStdString(rawLine);
    appendLogLine(&log, firstLine);
    const QJsonDocument requestDocument = QJsonDocument::fromJson(firstLine);
    if (!requestDocument.isObject()) return 92;
    const QJsonObject request = requestDocument.object();
    if (testCase == QStringLiteral("emergency-policy-switch")) {
        appendLogLine(&log, QJsonDocument(QJsonObject{
            {QStringLiteral("method"), QStringLiteral("test/emergency-environment")},
            {QStringLiteral("enabled"),
             qEnvironmentVariable("AEGISY_WORKBENCH_EMERGENCY_DISABLED")
                 == QStringLiteral("1")},
        }).toJson(QJsonDocument::Compact));
    }

    if (testCase == QStringLiteral("reconnect-exhaust") && invocation > 1) {
        return 42;
    }

    QJsonObject response{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), request.value(QStringLiteral("id"))},
    };
    QJsonObject result = validInitializeResult();
    const auto setStableCapabilities = [&result](const QJsonArray &stable) {
        QJsonObject capabilities = result.value(QStringLiteral("capabilities")).toObject();
        capabilities.insert(QStringLiteral("stable"), stable);
        result.insert(QStringLiteral("capabilities"), capabilities);
    };
    if (testCase.startsWith(QStringLiteral("heartbeat-"))) {
        result.insert(QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("status"), QStringLiteral("ready")},
        });
        QJsonArray stableCapabilities{
            QStringLiteral("runtime.codex-app-server"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("runtime.heartbeat.out-of-band"),
            QStringLiteral("permission.read-only"),
            QStringLiteral("session.list"),
            QStringLiteral("turn.cancel.interrupt"),
            QStringLiteral("terminal.lifecycle.named"),
            QStringLiteral("terminal.stop.out-of-band"),
            testTerminalPlatformCapability(),
        };
        if (testCase.startsWith(QStringLiteral("heartbeat-subscription-"))) {
            stableCapabilities.append(
                QStringLiteral("timeline.subscription.fixed-watermark"));
        }
        setStableCapabilities(stableCapabilities);
    } else if (testCase == QStringLiteral("turn-start-ack")) {
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.preview"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("timeline.streaming"),
            QStringLiteral("session.mutation-acknowledgements"),
            QStringLiteral("permission.read-only"),
        });
    } else if (testCase.startsWith(
                   QStringLiteral("transport-subscription-error-"))) {
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.preview"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("timeline.subscription.fixed-watermark"),
            QStringLiteral("permission.read-only"),
        });
    } else if (testCase.startsWith(QStringLiteral("transport-mutation-"))) {
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.preview"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("session.mutation-acknowledgements"),
            QStringLiteral("permission.read-only"),
        });
    } else if (testCase == QStringLiteral("valid-list-only")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("session.list"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("command-artifact-request-shape")
               || testCase == QStringLiteral("command-artifact-valid-response")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("artifact.command-output.bounded"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("valid-outbound-frame")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("session.portable.import"));
        setStableCapabilities(capabilities);
    } else if (testCase.startsWith(QStringLiteral("timeline-sync-"))) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("timeline.replay.fixed-watermark"));
        if (testCase.startsWith(QStringLiteral("timeline-sync-retention-gap"))
            && testCase != QStringLiteral("timeline-sync-retention-gap-unnegotiated")
            && testCase != QStringLiteral("timeline-sync-retention-gap-unavailable")) {
            capabilities.append(QStringLiteral("timeline.snapshot.current"));
        }
        setStableCapabilities(capabilities);
    } else if (testCase.startsWith(QStringLiteral("timeline-snapshot-"))) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("timeline.snapshot.current"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("notification-valid-event-envelope")
               || testCase == QStringLiteral("notification-valid-large-generic-event")
               || testCase == QStringLiteral("notification-valid-mathematical-integers")
               || testCase == QStringLiteral("notification-valid-boundary-event")
               || testCase == QStringLiteral("notification-invalid-event-envelope")
               || testCase == QStringLiteral("notification-invalid-event-item")
               || testCase == QStringLiteral("notification-unsafe-event-data-integer")
               || testCase == QStringLiteral("notification-float-event-data")
               || testCase == QStringLiteral("notification-invalid-event-data-key")
               || testCase == QStringLiteral("notification-unknown-event-item")
               || testCase == QStringLiteral("notification-event-identity-tamper")
               || testCase == QStringLiteral("notification-overlong-event-identity")
               || testCase == QStringLiteral("notification-nonascii-event-identity")
               || testCase == QStringLiteral("notification-removed-persistence-terminal")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("timeline.streaming"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("valid-codex")) {
        result.insert(QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("status"), QStringLiteral("ready")},
        });
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.codex-app-server"),
            QStringLiteral("runtime.restart"),
            QStringLiteral("timeline.command.structured.read-only"),
            QStringLiteral("turn.cancel.interrupt"),
            QStringLiteral("turn.steer.same-turn"),
            QStringLiteral("session.provider.lifecycle.archive"),
            QStringLiteral("session.provider.lifecycle.unarchive"),
            QStringLiteral("session.provider.lifecycle.list-read"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("permission.read-only"),
        });
    } else if (testCase == QStringLiteral("valid-recovery")) {
        result.insert(QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("aegisy-workbench-store")},
            {QStringLiteral("version"),
             QStringLiteral("workbench-recovery-diagnostic/0.1")},
            {QStringLiteral("status"), QStringLiteral("read-only-recovery")},
        });
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.recovery.read-only"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("model.catalog.read-only"),
            QStringLiteral("model.catalog.refresh.status.read-only"),
            QStringLiteral("model.capability-check.read-only"),
            QStringLiteral("runtime.recovery.status"),
            QStringLiteral("runtime.recovery.diagnostic-export"),
            QStringLiteral("permission.read-only"),
        });
    } else if (testCase == QStringLiteral("valid-unavailable")) {
        result.insert(QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("status"), QStringLiteral("unavailable")},
        });
        setStableCapabilities(QJsonArray{
            QStringLiteral("runtime.unavailable"),
            QStringLiteral("runtime.restart"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.degradations"),
        });
    } else if (testCase == QStringLiteral("protocol-mismatch")) {
        QJsonObject protocol = result.value(QStringLiteral("protocol")).toObject();
        protocol.insert(QStringLiteral("minimum"), QStringLiteral("0.2"));
        protocol.insert(QStringLiteral("maximum"), QStringLiteral("0.2"));
        protocol.insert(QStringLiteral("selected"), QStringLiteral("0.2"));
        protocol.insert(QStringLiteral("upgrade_direction"), QStringLiteral("client"));
        result.insert(QStringLiteral("protocol"), protocol);
    } else if (testCase == QStringLiteral("protocol-leading-zero")) {
        QJsonObject protocol = result.value(QStringLiteral("protocol")).toObject();
        protocol.insert(QStringLiteral("minimum"), QStringLiteral("00.1"));
        result.insert(QStringLiteral("protocol"), protocol);
    } else if (testCase == QStringLiteral("upgrade-direction")) {
        QJsonObject protocol = result.value(QStringLiteral("protocol")).toObject();
        protocol.insert(QStringLiteral("upgrade_direction"), QStringLiteral("runtime"));
        result.insert(QStringLiteral("protocol"), protocol);
    } else if (testCase == QStringLiteral("runtime-mismatch")) {
        QJsonObject runtime = result.value(QStringLiteral("runtime")).toObject();
        runtime.insert(QStringLiteral("version"), QStringLiteral("9.9.9-secret-sentinel"));
        result.insert(QStringLiteral("runtime"), runtime);
    } else if (testCase == QStringLiteral("duplicate-capability")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("runtime.health"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("unknown-capability")) {
        QJsonArray capabilities = result.value(QStringLiteral("capabilities")).toObject()
                                      .value(QStringLiteral("stable")).toArray();
        capabilities.append(QStringLiteral("runtime.unknown.secret-sentinel"));
        setStableCapabilities(capabilities);
    } else if (testCase == QStringLiteral("empty-capability")) {
        setStableCapabilities(QJsonArray{});
    } else if (testCase == QStringLiteral("missing-capability")) {
        result.remove(QStringLiteral("capabilities"));
    } else if (testCase == QStringLiteral("experimental-capability")) {
        QJsonObject capabilities = result.value(QStringLiteral("capabilities")).toObject();
        capabilities.insert(QStringLiteral("experimental"),
                            QJsonArray{QStringLiteral("future.feature")});
        result.insert(QStringLiteral("capabilities"), capabilities);
    } else if (testCase == QStringLiteral("wrong-platform")) {
        QJsonObject platform = result.value(QStringLiteral("platform")).toObject();
        platform.insert(QStringLiteral("os"),
                        platform.value(QStringLiteral("os")).toString()
                                == QStringLiteral("windows")
                            ? QStringLiteral("macos") : QStringLiteral("windows"));
        result.insert(QStringLiteral("platform"), platform);
    } else if (testCase == QStringLiteral("unsafe-transport")) {
        QJsonObject security = result.value(QStringLiteral("transport_security")).toObject();
        security.insert(QStringLiteral("authenticated"), false);
        result.insert(QStringLiteral("transport_security"), security);
    } else if (testCase == QStringLiteral("invalid-limit")) {
        result.insert(QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("max_frame_bytes"), 4 * 1024 * 1024 + 1},
        });
    } else if (testCase == QStringLiteral("invalid-low-limit")) {
        result.insert(QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("max_frame_bytes"), 1024 * 1024},
        });
    } else if (testCase == QStringLiteral("backend-contradiction")) {
        QJsonObject backend = result.value(QStringLiteral("backend")).toObject();
        backend.insert(QStringLiteral("status"), QStringLiteral("unavailable"));
        result.insert(QStringLiteral("backend"), backend);
    } else if (testCase == QStringLiteral("non-jsonrpc-2")) {
        response.insert(QStringLiteral("jsonrpc"), QStringLiteral("1.0"));
    } else if (testCase == QStringLiteral("missing-jsonrpc")) {
        response.remove(QStringLiteral("jsonrpc"));
    } else if (testCase == QStringLiteral("wrong-id-type")) {
        response.insert(QStringLiteral("id"), 1);
    }
    if (testCase == QStringLiteral("error-response")
        || testCase == QStringLiteral("generic-error-response")
        || testCase == QStringLiteral("upgrade-client-error")
        || testCase == QStringLiteral("upgrade-runtime-error")
        || testCase == QStringLiteral("malformed-upgrade-error")) {
        QJsonObject error{
            {QStringLiteral("code"),
             testCase == QStringLiteral("generic-error-response") ? -32602 : -32003},
            {QStringLiteral("message"), QStringLiteral("secret-sentinel")},
        };
        if (testCase == QStringLiteral("upgrade-client-error")
            || testCase == QStringLiteral("upgrade-runtime-error")
            || testCase == QStringLiteral("malformed-upgrade-error")) {
            const bool clientUpgrade = testCase != QStringLiteral("upgrade-runtime-error");
            error.insert(QStringLiteral("data"), QJsonObject{
                {QStringLiteral("schema_version"), QStringLiteral("initialize-error/0.1")},
                {QStringLiteral("reason"), QStringLiteral("protocol-range-not-overlapping")},
                {QStringLiteral("client"), QJsonObject{
                    {QStringLiteral("minimum"), QStringLiteral("0.1")},
                    {QStringLiteral("maximum"), QStringLiteral("0.1")},
                }},
                {QStringLiteral("runtime"), QJsonObject{
                    {QStringLiteral("minimum"),
                     clientUpgrade ? QStringLiteral("0.2") : QStringLiteral("0.0")},
                    {QStringLiteral("maximum"),
                     clientUpgrade ? QStringLiteral("0.2") : QStringLiteral("0.0")},
                }},
                {QStringLiteral("upgrade_direction"),
                 testCase == QStringLiteral("malformed-upgrade-error")
                     ? QStringLiteral("secret-sentinel")
                     : (clientUpgrade ? QStringLiteral("client")
                                      : QStringLiteral("runtime"))},
            });
        }
        response.insert(QStringLiteral("error"), error);
    } else if (testCase == QStringLiteral("wrong-result-type")) {
        response.insert(QStringLiteral("result"), QJsonArray{});
    } else {
        response.insert(QStringLiteral("result"), result);
    }
    if (testCase == QStringLiteral("result-and-error")) {
        response.insert(QStringLiteral("error"), QJsonObject{
            {QStringLiteral("code"), -32003},
            {QStringLiteral("message"), QStringLiteral("invalid")},
        });
    } else if (testCase == QStringLiteral("method-and-result")) {
        response.insert(QStringLiteral("method"), QStringLiteral("event"));
    } else if (testCase == QStringLiteral("params-and-result")) {
        response.insert(QStringLiteral("params"), QJsonObject{});
    }
    std::cout << QJsonDocument(response).toJson(QJsonDocument::Compact).constData()
              << std::endl;

    bool ordinaryViolationSent = false;
    QJsonValue combinedFirstId;
    bool hasCombinedFirstId = false;
    int timelineSyncRequests = 0;
    int timelineSnapshotRequests = 0;
    int transportHealthRequests = 0;
    QJsonObject firstDelayedHeartbeat;
    QJsonObject pendingStartupHealth;
    QJsonObject pendingStartupDegradations;
    QJsonObject pendingFirstSameMethodHealth;
    QJsonObject pendingMutationList;
    QJsonObject pendingMutationConsume;
    while (std::getline(std::cin, rawLine)) {
        const QByteArray line = QByteArray::fromStdString(rawLine);
        appendLogLine(&log, line);
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) continue;
        const QJsonObject message = document.object();
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("runtime/heartbeat")
            && testCase.startsWith(QStringLiteral("heartbeat-"))) {
            if (firstDelayedHeartbeat.isEmpty()) firstDelayedHeartbeat = message;
            if (testCase == QStringLiteral("heartbeat-normal")
                || (testCase.startsWith(
                        QStringLiteral("heartbeat-subscription-"))
                    && invocation > 1)) {
                const QJsonObject params = message.value(QStringLiteral("params")).toObject();
                const QJsonObject heartbeatResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                    {QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("schema_version"),
                         QStringLiteral("runtime-heartbeat/0.1")},
                        {QStringLiteral("nonce"), params.value(QStringLiteral("nonce"))},
                        {QStringLiteral("state"), QStringLiteral("alive")},
                    }},
                };
                std::cout
                    << QJsonDocument(heartbeatResponse).toJson(QJsonDocument::Compact)
                           .constData()
                    << std::endl;
            }
            continue;
        }
        if (testCase.startsWith(QStringLiteral("heartbeat-subscription-"))
            && invocation == 1) {
            const QString stage = testCase.mid(
                QStringLiteral("heartbeat-subscription-").size());
            const QHash<QString, QString> pendingMethods{
                {QStringLiteral("subscribe"), QStringLiteral("timeline/subscribe")},
                {QStringLiteral("sync"),
                 QStringLiteral("timeline/subscription-sync")},
                {QStringLiteral("snapshot"),
                 QStringLiteral("timeline/subscription-snapshot")},
                {QStringLiteral("activate"),
                 QStringLiteral("timeline/subscription-activate")},
            };
            if (method == pendingMethods.value(stage)) continue;
        }
        if (method == QStringLiteral("session/list")
            && testCase.startsWith(QStringLiteral("heartbeat-"))) {
            continue;
        }
        if (method == QStringLiteral("terminal/stop-user")
            && testCase == QStringLiteral("heartbeat-late")
            && !firstDelayedHeartbeat.isEmpty()) {
            const QJsonObject params = firstDelayedHeartbeat.value(
                QStringLiteral("params")).toObject();
            const QJsonObject lateResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), firstDelayedHeartbeat.value(QStringLiteral("id"))},
                {QStringLiteral("result"), QJsonObject{
                    {QStringLiteral("schema_version"),
                     QStringLiteral("runtime-heartbeat/0.1")},
                    {QStringLiteral("nonce"), params.value(QStringLiteral("nonce"))},
                    {QStringLiteral("state"), QStringLiteral("alive")},
                }},
            };
            std::cout << QJsonDocument(lateResponse).toJson(QJsonDocument::Compact)
                             .constData()
                      << std::endl;
            firstDelayedHeartbeat = {};
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("initialized")
            && testCase == QStringLiteral("initialize-late")) {
            std::cout
                << QJsonDocument(response).toJson(QJsonDocument::Compact).constData()
                << std::endl;
            continue;
        }
        if (method == QStringLiteral("initialized")
            && testCase.startsWith(QStringLiteral("transport-raw-"))) {
            const QByteArray violation = rawTransportParserViolation(testCase);
            if (violation.isEmpty()) return 94;
            writeRawFakeRuntimeFrame(violation);
            continue;
        }
        if (method == QStringLiteral("initialized")
            && testCase == QStringLiteral(
                "transport-unmatched-malformed-typed-error")) {
            writeRawFakeRuntimeFrame(QByteArrayLiteral(
                R"({"jsonrpc":"2.0","id":"unmatched-malformed","error":{"code":-32150,"message":"subscription failed","data":{"schema_version":"timeline-subscription-failure/0.1"}}})"));
            continue;
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("initialized")
            && testCase.startsWith(QStringLiteral("notification-"))) {
            if (testCase == QStringLiteral("notification-valid-mathematical-integers")
                    || testCase == QStringLiteral("notification-valid-boundary-event")) {
                QByteArray frame = testCase
                        == QStringLiteral("notification-valid-mathematical-integers")
                    ? rawMathematicalIntegerTimelineNotification()
                    : exactBoundaryTimelineNotification();
                if (frame.isEmpty()) return 93;
                frame.append('\n');
                std::cout.write(frame.constData(), std::streamsize(frame.size()));
                std::cout.flush();
                continue;
            }
            QJsonObject notification{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("method"), QStringLiteral("event")},
                {QStringLiteral("params"), QJsonObject{}},
            };
            if (testCase == QStringLiteral("notification-method-result")) {
                notification.insert(QStringLiteral("result"), QJsonObject{});
            } else if (testCase == QStringLiteral("notification-wrong-params")) {
                notification.insert(QStringLiteral("params"), QJsonArray{});
            } else if (testCase == QStringLiteral("notification-missing-params")) {
                notification.remove(QStringLiteral("params"));
            } else if (testCase == QStringLiteral("notification-valid-event-envelope")) {
                notification.insert(QStringLiteral("params"), validTimelineEnvelope());
            } else if (testCase
                       == QStringLiteral("notification-valid-large-generic-event")) {
                notification.insert(QStringLiteral("params"),
                                    validLargeGenericTimelineEnvelope());
            } else if (testCase == QStringLiteral("notification-invalid-event-envelope")) {
                QJsonObject event = validTimelineEnvelope();
                event.remove(QStringLiteral("correlation_id"));
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase == QStringLiteral("notification-invalid-event-item")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("event"), QStringLiteral("item.completed"));
                event.insert(QStringLiteral("item"), QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("item-1")},
                    {QStringLiteral("kind"), QStringLiteral("message")},
                    {QStringLiteral("role"), QStringLiteral("agent")},
                    {QStringLiteral("state"), QStringLiteral("completed")},
                });
                event.insert(QStringLiteral("item_update"), QJsonObject{
                    {QStringLiteral("revision"), 1},
                    {QStringLiteral("content_mode"),
                     QStringLiteral("snapshot-replacement")},
                });
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase
                       == QStringLiteral("notification-unsafe-event-data-integer")) {
                notification.insert(QStringLiteral("params"), timelineDataEnvelope(
                    QJsonObject{{QStringLiteral("unsafe"),
                                 9'007'199'254'740'992.0}}));
            } else if (testCase == QStringLiteral("notification-float-event-data")) {
                notification.insert(QStringLiteral("params"), timelineDataEnvelope(
                    QJsonObject{{QStringLiteral("unsafe"), 1.5}}));
            } else if (testCase
                       == QStringLiteral("notification-invalid-event-data-key")) {
                notification.insert(QStringLiteral("params"), timelineDataEnvelope(
                    QJsonObject{{QStringLiteral("项目"), true}}));
            } else if (testCase == QStringLiteral("notification-unknown-event-item")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("event"), QStringLiteral("future.item"));
                event.insert(QStringLiteral("item"), QJsonObject{
                    {QStringLiteral("id"), QStringLiteral("item-1")},
                    {QStringLiteral("kind"), QStringLiteral("message")},
                    {QStringLiteral("role"), QStringLiteral("agent")},
                    {QStringLiteral("state"), QStringLiteral("completed")},
                    {QStringLiteral("content"), QStringLiteral("invalid")},
                });
                event.insert(QStringLiteral("item_update"), QJsonObject{
                    {QStringLiteral("revision"), 1},
                    {QStringLiteral("content_mode"),
                     QStringLiteral("snapshot-replacement")},
                });
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase
                       == QStringLiteral("notification-event-identity-tamper")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("timestamp_ms"), 1'001);
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase
                       == QStringLiteral("notification-overlong-event-identity")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("session_id"),
                             QString(129, QLatin1Char('s')));
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase
                       == QStringLiteral("notification-nonascii-event-identity")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("session_id"), QStringLiteral("项目"));
                notification.insert(QStringLiteral("params"), event);
            } else if (testCase
                       == QStringLiteral("notification-removed-persistence-terminal")) {
                QJsonObject event = validTimelineEnvelope();
                event.insert(QStringLiteral("event"),
                             QStringLiteral("turn.persistence-failed"));
                notification.insert(QStringLiteral("params"), event);
            }
            std::cout
                << QJsonDocument(notification).toJson(QJsonDocument::Compact).constData()
                << std::endl;
            continue;
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("initialized")
            && (testCase == QStringLiteral("reconnect-exit-success")
                || testCase == QStringLiteral("reconnect-exhaust"))
            && invocation == 1) {
            return 41;
        }
        if (message.value(QStringLiteral("method")).toString()
            == QStringLiteral("shutdown")) {
            const QJsonObject shutdownResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), QJsonObject{}},
            };
            std::cout
                << QJsonDocument(shutdownResponse).toJson(QJsonDocument::Compact).constData()
                << std::endl;
            return 0;
        }
        if (testCase == QStringLiteral("command-artifact-valid-response")
            && method == QStringLiteral("artifact/read-command-output-page")) {
            const QJsonObject pageResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), validSinglePageCommandArtifactResult(
                    message.value(QStringLiteral("params")).toObject())},
            };
            writeRawFakeRuntimeFrame(QJsonDocument(pageResponse).toJson(
                QJsonDocument::Compact));
            continue;
        }
        if (testCase == QStringLiteral("transport-correlation-ids")
            && method == QStringLiteral("runtime/health")) {
            const QJsonObject retiredResponse = response;
            const QJsonObject unmatchedResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), QStringLiteral("unmatched-response-id")},
                {QStringLiteral("result"), QJsonObject{}},
            };
            const QJsonObject nullResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), QJsonValue(QJsonValue::Null)},
                {QStringLiteral("error"), QJsonObject{
                    {QStringLiteral("code"), -32000},
                    {QStringLiteral("message"), QStringLiteral("unmatched")},
                }},
            };
            const QJsonObject correctResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), QJsonObject{}},
            };
            writeRawFakeRuntimeFrame(
                QJsonDocument(retiredResponse).toJson(QJsonDocument::Compact));
            writeRawFakeRuntimeFrame(
                QJsonDocument(unmatchedResponse).toJson(QJsonDocument::Compact));
            writeRawFakeRuntimeFrame(
                QJsonDocument(nullResponse).toJson(QJsonDocument::Compact));
            writeRawFakeRuntimeFrame(
                QJsonDocument(correctResponse).toJson(QJsonDocument::Compact));
            continue;
        }
        if (testCase == QStringLiteral("transport-huge-error-code")
            && method == QStringLiteral("runtime/health")) {
            QByteArray hugeError = QByteArrayLiteral(
                R"({"jsonrpc":"2.0","id":")");
            hugeError.append(message.value(QStringLiteral("id")).toString().toUtf8());
            hugeError.append(QByteArrayLiteral(
                R"(","error":{"code":123456789012345678901234567890,"message":"huge code"}})"));
            writeRawFakeRuntimeFrame(hugeError);
            continue;
        }
        if (testCase == QStringLiteral("transport-concurrent-reverse")
            && (method == QStringLiteral("runtime/health")
                || method == QStringLiteral("runtime/degradations"))) {
            if (method == QStringLiteral("runtime/health")) {
                ++transportHealthRequests;
                if (transportHealthRequests == 1) {
                    pendingStartupHealth = message;
                } else if (transportHealthRequests == 2) {
                    pendingFirstSameMethodHealth = message;
                } else {
                    const QJsonObject secondError{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                        {QStringLiteral("error"), QJsonObject{
                            {QStringLiteral("code"), -32060},
                            {QStringLiteral("message"),
                             QStringLiteral("second request failed")},
                        }},
                    };
                    const QJsonObject firstSuccess{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), pendingFirstSameMethodHealth.value(
                             QStringLiteral("id"))},
                        {QStringLiteral("result"), QJsonObject{}},
                    };
                    writeRawFakeRuntimeFrame(QJsonDocument(secondError).toJson(
                        QJsonDocument::Compact));
                    writeRawFakeRuntimeFrame(QJsonDocument(firstSuccess).toJson(
                        QJsonDocument::Compact));
                    pendingFirstSameMethodHealth = {};
                }
            } else {
                pendingStartupDegradations = message;
            }
            if (!pendingStartupHealth.isEmpty()
                && !pendingStartupDegradations.isEmpty()) {
                const QJsonObject degradationsResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), pendingStartupDegradations.value(
                         QStringLiteral("id"))},
                    {QStringLiteral("result"), QJsonObject{}},
                };
                const QJsonObject healthResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), pendingStartupHealth.value(
                         QStringLiteral("id"))},
                    {QStringLiteral("result"), QJsonObject{}},
                };
                writeRawFakeRuntimeFrame(QJsonDocument(degradationsResponse).toJson(
                    QJsonDocument::Compact));
                writeRawFakeRuntimeFrame(QJsonDocument(healthResponse).toJson(
                    QJsonDocument::Compact));
                pendingStartupHealth = {};
                pendingStartupDegradations = {};
            }
            continue;
        }
        if (testCase == QStringLiteral("transport-old-generation")
            && method == QStringLiteral("runtime/health")) {
            if (invocation == 1) return 44;
            if (!priorRuntimeHealthId.isEmpty()) {
                const QJsonObject oldResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), priorRuntimeHealthId},
                    {QStringLiteral("result"), QJsonObject{}},
                };
                writeRawFakeRuntimeFrame(QJsonDocument(oldResponse).toJson(
                    QJsonDocument::Compact));
            }
            const QJsonObject currentResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), QJsonObject{}},
            };
            writeRawFakeRuntimeFrame(QJsonDocument(currentResponse).toJson(
                QJsonDocument::Compact));
            continue;
        }
        if (testCase.startsWith(QStringLiteral("transport-subscription-error-"))) {
            const QString suffix = testCase.mid(
                QStringLiteral("transport-subscription-error-").size());
            QString targetStage;
            for (const QString &candidate : {
                     QStringLiteral("subscribe"), QStringLiteral("sync"),
                     QStringLiteral("snapshot"), QStringLiteral("activate")}) {
                if (suffix.startsWith(candidate + QLatin1Char('-'))) {
                    targetStage = candidate;
                    break;
                }
            }
            const QHash<QString, QString> methods{
                {QStringLiteral("subscribe"), QStringLiteral("timeline/subscribe")},
                {QStringLiteral("sync"),
                 QStringLiteral("timeline/subscription-sync")},
                {QStringLiteral("snapshot"),
                 QStringLiteral("timeline/subscription-snapshot")},
                {QStringLiteral("activate"),
                 QStringLiteral("timeline/subscription-activate")},
            };
            if (!targetStage.isEmpty() && method == methods.value(targetStage)) {
                const QString drift = suffix.mid(targetStage.size() + 1);
                const QJsonObject failure = subscriptionFailureForRequest(
                    method, message.value(QStringLiteral("params")).toObject(),
                    drift == QStringLiteral("valid") ? QString() : drift);
                const QJsonObject errorResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                    {QStringLiteral("error"), QJsonObject{
                        {QStringLiteral("code"), -32150},
                        {QStringLiteral("message"),
                         QStringLiteral("subscription failed")},
                        {QStringLiteral("data"), failure},
                    }},
                };
                writeRawFakeRuntimeFrame(QJsonDocument(errorResponse).toJson(
                    QJsonDocument::Compact));
                continue;
            }
        }
        if (testCase.startsWith(QStringLiteral("transport-mutation-"))) {
            if (method == QStringLiteral("session/mutation-acknowledgements")) {
                if (testCase == QStringLiteral("transport-mutation-reverse")) {
                    pendingMutationList = message;
                } else if (testCase == QStringLiteral(
                               "transport-mutation-list-drift")) {
                    const QJsonObject mutationResponse{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                        {QStringLiteral("result"), mutationListResult(
                             message.value(QStringLiteral("params")).toObject(), true)},
                    };
                    writeRawFakeRuntimeFrame(QJsonDocument(mutationResponse).toJson(
                        QJsonDocument::Compact));
                    continue;
                }
            } else if (method
                       == QStringLiteral("mutation/acknowledgement/consume")) {
                if (testCase == QStringLiteral("transport-mutation-reverse")) {
                    pendingMutationConsume = message;
                } else if (testCase == QStringLiteral(
                               "transport-mutation-consume-drift")) {
                    const QJsonObject mutationResponse{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                        {QStringLiteral("result"), mutationConsumeResult(
                             message.value(QStringLiteral("params")).toObject(), true)},
                    };
                    writeRawFakeRuntimeFrame(QJsonDocument(mutationResponse).toJson(
                        QJsonDocument::Compact));
                    continue;
                }
            }
            if (!pendingMutationList.isEmpty() && !pendingMutationConsume.isEmpty()) {
                const QJsonObject consumeResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), pendingMutationConsume.value(
                         QStringLiteral("id"))},
                    {QStringLiteral("result"), mutationConsumeResult(
                         pendingMutationConsume.value(QStringLiteral("params")).toObject(),
                         false)},
                };
                const QJsonObject listResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), pendingMutationList.value(
                         QStringLiteral("id"))},
                    {QStringLiteral("result"), mutationListResult(
                         pendingMutationList.value(QStringLiteral("params")).toObject(),
                         false)},
                };
                writeRawFakeRuntimeFrame(QJsonDocument(consumeResponse).toJson(
                    QJsonDocument::Compact));
                writeRawFakeRuntimeFrame(QJsonDocument(listResponse).toJson(
                    QJsonDocument::Compact));
                pendingMutationList = {};
                pendingMutationConsume = {};
                continue;
            }
            if (method == QStringLiteral("session/mutation-acknowledgements")
                || method == QStringLiteral(
                    "mutation/acknowledgement/consume")) {
                continue;
            }
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("timeline/sync")
            && testCase.startsWith(QStringLiteral("timeline-sync-"))) {
            ++timelineSyncRequests;
            if (testCase == QStringLiteral("timeline-sync-disconnect")
                && invocation == 1) {
                return 0;
            }
            QJsonObject timelineResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
            };
            if (testCase == QStringLiteral("timeline-sync-error-cleanup")
                && timelineSyncRequests == 1) {
                timelineResponse.insert(QStringLiteral("error"), QJsonObject{
                    {QStringLiteral("code"), -32160},
                    {QStringLiteral("message"), QStringLiteral("timeline sync rejected")},
                });
            } else if (testCase.startsWith(
                           QStringLiteral("timeline-sync-retention-gap"))) {
                const QJsonObject params = message.value(QStringLiteral("params")).toObject();
                QJsonObject data{
                    {QStringLiteral("schema_version"),
                     QStringLiteral("timeline-retention-gap/0.1")},
                    {QStringLiteral("reason"),
                     QStringLiteral("requested-anchor-not-retained")},
                    {QStringLiteral("session_id"), params.value(QStringLiteral("session_id"))},
                    {QStringLiteral("requested_after"), params.value(QStringLiteral("after"))},
                    {QStringLiteral("requested_watermark"),
                     params.value(QStringLiteral("watermark"))},
                    {QStringLiteral("retained_floor"),
                     timelineAnchor(2, timelineEventId(QLatin1Char('a')))},
                    {QStringLiteral("head"),
                     timelineAnchor(3, timelineEventId(QLatin1Char('b')))},
                    {QStringLiteral("snapshot_required"), true},
                    {QStringLiteral("snapshot_available"), true},
                    {QStringLiteral("snapshot_capability"),
                     QStringLiteral("timeline.snapshot.current")},
                    {QStringLiteral("snapshot_method"), QStringLiteral("timeline/snapshot")},
                    {QStringLiteral("event_history_complete"), false},
                    {QStringLiteral("replay_from_floor_allowed"), false},
                };
                if (testCase == QStringLiteral("timeline-sync-retention-gap-drift")) {
                    data.insert(QStringLiteral("session_id"), QStringLiteral("session-2"));
                } else if (testCase
                           == QStringLiteral("timeline-sync-retention-gap-unavailable")) {
                    data.insert(QStringLiteral("snapshot_available"), false);
                }
                timelineResponse.insert(QStringLiteral("error"), QJsonObject{
                    {QStringLiteral("code"), -32148},
                    {QStringLiteral("message"),
                     QStringLiteral("requested Timeline history is no longer retained")},
                    {QStringLiteral("data"), data},
                });
            } else {
                timelineResponse.insert(
                    QStringLiteral("result"),
                    timelineSyncPage(message.value(QStringLiteral("params")).toObject()));
            }
            QByteArray encoded = QJsonDocument(timelineResponse).toJson(
                QJsonDocument::Compact);
            if (testCase == QStringLiteral(
                    "timeline-sync-retention-gap-code-decimal")) {
                encoded.replace("\"code\":-32148", "\"code\":-32148.0");
            } else if (testCase == QStringLiteral(
                           "timeline-sync-retention-gap-code-exponent")) {
                encoded.replace("\"code\":-32148", "\"code\":-3.2148e4");
            }
            writeRawFakeRuntimeFrame(encoded);
            continue;
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("timeline/snapshot")
            && testCase.startsWith(QStringLiteral("timeline-snapshot-"))) {
            ++timelineSnapshotRequests;
            QJsonObject timelineResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
            };
            if (testCase == QStringLiteral("timeline-snapshot-error-cleanup")
                && timelineSnapshotRequests == 1) {
                timelineResponse.insert(QStringLiteral("error"), QJsonObject{
                    {QStringLiteral("code"), -32160},
                    {QStringLiteral("message"), QStringLiteral("timeline snapshot rejected")},
                });
            } else {
                timelineResponse.insert(
                    QStringLiteral("result"),
                    timelineSnapshotPage(message.value(QStringLiteral("params")).toObject()));
            }
            std::cout
                << QJsonDocument(timelineResponse).toJson(QJsonDocument::Compact).constData()
                << std::endl;
            continue;
        }
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("turn/start")
            && testCase == QStringLiteral("turn-start-ack")) {
            const QJsonObject params = message.value(QStringLiteral("params")).toObject();
            const QString fingerprint = QStringLiteral(
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
            const QString sessionId = params.value(QStringLiteral("session_id")).toString();
            const QString key = params.value(QStringLiteral("idempotency_key")).toString();
            const QString turnId = QStringLiteral("turn-1");
            const QString eventId = QStringLiteral("event:sha256:")
                + QString(64, QLatin1Char('a'));
            const QString operationIdentity = AgentRuntimeClient::
                durableMutationOperationIdentity(sessionId, QStringLiteral("turn-start"),
                                                 key, fingerprint);
            const QJsonObject operation{
                {QStringLiteral("schema_version"),
                 QStringLiteral("mutation-acknowledgement-operation/0.1")},
                {QStringLiteral("operation_identity"), operationIdentity},
                {QStringLiteral("session_id"), sessionId},
                {QStringLiteral("mutation_kind"), QStringLiteral("turn-start")},
                {QStringLiteral("idempotency_key"), key},
                {QStringLiteral("request_fingerprint"), fingerprint},
                {QStringLiteral("revision"), 2},
                {QStringLiteral("state"), QStringLiteral("accepted")},
                {QStringLiteral("turn_id"), turnId},
                {QStringLiteral("accepted_anchor"), QJsonObject{
                    {QStringLiteral("sequence"), 1},
                    {QStringLiteral("event_id"), eventId},
                }},
                {QStringLiteral("terminal_anchor"), QJsonValue(QJsonValue::Null)},
                {QStringLiteral("accepted_consumed"), false},
                {QStringLiteral("terminal_consumed"), false},
            };
            const QJsonObject result{
                {QStringLiteral("turn"), QJsonObject{
                    {QStringLiteral("id"), turnId},
                    {QStringLiteral("state"), QStringLiteral("started")},
                }},
                {QStringLiteral("context"), QJsonObject{
                    {QStringLiteral("item_count"), 0},
                    {QStringLiteral("bytes"), 0},
                    {QStringLiteral("truncated"), false},
                    {QStringLiteral("manifest"), QJsonObject{}},
                    {QStringLiteral("budget"), QJsonObject{}},
                }},
                {QStringLiteral("request_fingerprint"), fingerprint},
                {QStringLiteral("mutation_acknowledgement"), QJsonObject{
                    {QStringLiteral("schema_version"),
                     QStringLiteral("mutation-acknowledgement/0.1")},
                    {QStringLiteral("request_id"), message.value(QStringLiteral("id"))},
                    {QStringLiteral("idempotency_key"), key},
                    {QStringLiteral("session_id"), sessionId},
                    {QStringLiteral("generation"), params.value(QStringLiteral("generation"))},
                    {QStringLiteral("state"), QStringLiteral("accepted")},
                }},
                {QStringLiteral("mutation_operation"), operation},
            };
            const QJsonObject turnResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), result},
            };
            std::cout << QJsonDocument(turnResponse).toJson(QJsonDocument::Compact).constData()
                      << std::endl;
            continue;
        }
        if (message.value(QStringLiteral("id")).isString()) {
            if (testCase == QStringLiteral("combined-legal-frames")) {
                if (!hasCombinedFirstId) {
                    combinedFirstId = message.value(QStringLiteral("id"));
                    hasCombinedFirstId = true;
                    continue;
                }
                const QString padding(2200000, QLatin1Char('a'));
                const QJsonObject firstResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), combinedFirstId},
                    {QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("padding"), padding},
                    }},
                };
                const QJsonObject secondResponse{
                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                    {QStringLiteral("result"), QJsonObject{
                        {QStringLiteral("padding"), padding},
                    }},
                };
                QByteArray combined = QJsonDocument(firstResponse)
                                          .toJson(QJsonDocument::Compact);
                combined.append('\n');
                combined.append(QJsonDocument(secondResponse)
                                    .toJson(QJsonDocument::Compact));
                combined.append("\r\n");
                std::cout.write(combined.constData(), combined.size());
                std::cout.flush();
                combinedFirstId = QJsonValue();
                hasCombinedFirstId = false;
                continue;
            }
            if (testCase == QStringLiteral("ordinary-oversized-tail")
                && !ordinaryViolationSent) {
                ordinaryViolationSent = true;
                const std::string oversized(4 * 1024 * 1024 + 1, 'x');
                std::cout.write(oversized.data(), std::streamsize(oversized.size()));
                std::cout.flush();
                continue;
            }
            QJsonObject ordinaryResponse{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), message.value(QStringLiteral("id"))},
                {QStringLiteral("result"), QJsonObject{}},
            };
            if (testCase.startsWith(QStringLiteral("ordinary-"))
                && !ordinaryViolationSent) {
                ordinaryViolationSent = true;
                if (testCase == QStringLiteral("ordinary-wrong-jsonrpc")) {
                    ordinaryResponse.insert(QStringLiteral("jsonrpc"),
                                            QStringLiteral("1.0"));
                } else if (testCase == QStringLiteral("ordinary-missing-jsonrpc")) {
                    ordinaryResponse.remove(QStringLiteral("jsonrpc"));
                } else if (testCase == QStringLiteral("ordinary-wrong-id-type")) {
                    ordinaryResponse.insert(QStringLiteral("id"), 2);
                } else if (testCase == QStringLiteral("ordinary-unknown-id")) {
                    ordinaryResponse.insert(QStringLiteral("id"), QStringLiteral("999999"));
                } else if (testCase == QStringLiteral("ordinary-result-and-error")) {
                    ordinaryResponse.insert(QStringLiteral("error"), QJsonObject{
                        {QStringLiteral("code"), -32000},
                        {QStringLiteral("message"), QStringLiteral("invalid")},
                    });
                } else if (testCase == QStringLiteral("ordinary-method-and-result")) {
                    ordinaryResponse.insert(QStringLiteral("method"),
                                            QStringLiteral("event"));
                } else if (testCase == QStringLiteral("ordinary-nonobject-result")) {
                    ordinaryResponse.insert(QStringLiteral("result"), QJsonArray{});
                } else if (testCase == QStringLiteral("ordinary-invalid-error")) {
                    ordinaryResponse.remove(QStringLiteral("result"));
                    ordinaryResponse.insert(QStringLiteral("error"), QJsonObject{
                        {QStringLiteral("code"), QStringLiteral("-32000")},
                        {QStringLiteral("message"), QStringLiteral("invalid")},
                    });
                }
            }
            std::cout
                << QJsonDocument(ordinaryResponse).toJson(QJsonDocument::Compact).constData()
                << std::endl;
        }
    }
    return 0;
}

QList<QJsonObject> readLogMessages(const QString &path)
{
    QFile file(path);
    QList<QJsonObject> messages;
    if (!file.open(QIODevice::ReadOnly)) return messages;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (document.isObject()) messages.append(document.object());
    }
    return messages;
}

bool logContainsMethod(const QString &path, const QString &method)
{
    const QList<QJsonObject> messages = readLogMessages(path);
    return std::any_of(messages.cbegin(), messages.cend(), [&method](const QJsonObject &message) {
        return message.value(QStringLiteral("method")).toString() == method;
    });
}

bool runEmergencyPolicySwitchCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create emergency switch directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("emergency-policy-switch"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());
    qputenv("AEGISY_WORKBENCH_DATA_ROOT", directory.path().toUtf8());

    int initializedCount = 0;
    int emergencyFailureCode = 0;
    AgentRuntimeClient client;
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initializedCount](const QJsonObject &) { ++initializedCount; });
    QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                     [&emergencyFailureCode](const QString &, const QString &method,
                                              const QString &, int code) {
        if (method == QStringLiteral("session/start")) emergencyFailureCode = code;
    });
    client.start();
    if (!expect(waitUntil([&]() { return initializedCount == 1 && client.isReady(); }),
                "initial runtime did not become ready for emergency switching")) {
        return false;
    }
    const quint64 normalGeneration = client.processGeneration();
    client.setEmergencyDisabled(true);
    if (!expect(client.startSession(QStringLiteral("chat")).isEmpty()
                    && emergencyFailureCode == -32153,
                "new session escaped the immediate emergency request gate")) {
        return false;
    }
    if (!expect(waitUntil([&]() {
            return initializedCount == 2 && client.isReady()
                && client.processGeneration() > normalGeneration;
        }), "runtime did not restart into emergency mode")) {
        return false;
    }
    const quint64 emergencyGeneration = client.processGeneration();
    if (!expect(client.emergencyDisabled(), "client lost the emergency policy state")
        || !expect(client.startSession(QStringLiteral("chat")).isEmpty()
                       && emergencyFailureCode == -32153,
                   "new session escaped the emergency request gate")) {
        return false;
    }
    client.setEmergencyDisabled(false);
    if (!expect(waitUntil([&]() {
            return initializedCount == 3 && client.isReady()
                && client.processGeneration() > emergencyGeneration;
        }), "runtime did not restart after a signed policy recovery")) {
        return false;
    }
    client.stop();
    waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });

    QList<bool> observedModes;
    for (const QJsonObject &message : readLogMessages(logPath)) {
        if (message.value(QStringLiteral("method")).toString()
            == QStringLiteral("test/emergency-environment")) {
            observedModes.append(message.value(QStringLiteral("enabled")).toBool());
        }
    }
    return expect(observedModes.size() == 3 && !observedModes.at(0)
                      && observedModes.at(1) && !observedModes.at(2),
                  "sidecar generations did not receive the exact emergency environment");
}

QList<QJsonObject> logMessagesForMethod(const QString &path, const QString &method)
{
    QList<QJsonObject> matching;
    const QList<QJsonObject> messages = readLogMessages(path);
    std::copy_if(messages.cbegin(), messages.cend(), std::back_inserter(matching),
                 [&method](const QJsonObject &message) {
        return message.value(QStringLiteral("method")).toString() == method;
    });
    return matching;
}

bool runHandshakeCase(const QString &testCase, bool expectAccepted,
                      bool expectReady = true, bool expectRecovery = false,
                      const QString &expectedFailureCode = QString())
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create handshake test directory")) return false;
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());
    {
        const QString fakePath = qEnvironmentVariable("AEGISY_AGENTD_PATH");
        const QFileInfo fakeInfo(fakePath);
        std::cerr << "fake candidate: " << fakePath.toStdString()
                  << " isFile: " << fakeInfo.isFile()
                  << " isExecutable: " << fakeInfo.isExecutable() << std::endl;
    }

    bool initialized = false;
    bool initializeFailed = false;
    bool turnStarted = false;
    QString turnStartRequestId;
    quint64 turnStartGeneration = 0;
    QString failureMessage;
    QStringList diagnostics;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::diagnosticMessage,
                         [&diagnostics](const QString &message) {
            diagnostics.append(message);
        });
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&initializeFailed, &failureMessage](const QString &,
                                                              const QString &method,
                                                              const QString &message,
                                                              int) {
            if (method == QStringLiteral("initialize")) {
                initializeFailed = true;
                failureMessage = message;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::turnStarted,
                         [&turnStarted, &turnStartRequestId, &turnStartGeneration](
                             const QString &requestId, quint64 generation,
                             const QJsonObject &) {
            turnStarted = true;
            turnStartRequestId = requestId;
            turnStartGeneration = generation;
        });
        client.start();
        const QString prematureRequest = client.listProjects();
        if (!expect(prematureRequest.isEmpty(),
                    "business request escaped before initialize completed")) {
            return false;
        }
        const bool observed = waitUntil([&]() { return initialized || initializeFailed; });
        if (!expect(observed, "handshake case timed out")) return false;
        if (expectAccepted) {
            if (!expect(initialized && !initializeFailed
                            && client.isReady() == expectReady
                            && client.isRecoveryMode() == expectRecovery,
                        "valid initialize response was not accepted")) {
                std::cerr << "case: " << testCase.toStdString()
                          << " initialize failure: " << failureMessage.toStdString()
                          << " runtime path: " << client.runtimePath().toStdString()
                          << " log exists: " << QFileInfo::exists(logPath)
                          << " diagnostics: "
                          << diagnostics.join(QStringLiteral(" | ")).toStdString()
                          << std::endl;
                QFile log(logPath);
                if (log.open(QIODevice::ReadOnly)) {
                    std::cerr << "fake runtime log: "
                              << log.readAll().left(2048).constData() << std::endl;
                }
                return false;
            }
            if (!expect(waitUntil([&]() {
                    return logContainsMethod(logPath, QStringLiteral("runtime/health"))
                        && logContainsMethod(logPath, QStringLiteral("runtime/degradations"));
                }), "negotiated startup requests were not sent")) {
                return false;
            }
            if (testCase == QStringLiteral("turn-start-ack")) {
                const QString requestId = client.startTurn(
                    QStringLiteral("session-1"), QStringLiteral("hello"), {}, {}, {},
                    QStringLiteral("gesture-550e8400"));
                if (!expect(!requestId.isEmpty(),
                            "turn/start request was not sent with durable idempotency")) {
                    return false;
                }
                if (!expect(waitUntil([&]() { return turnStarted; }),
                            "validated turn/start response signal was not emitted")) {
                    return false;
                }
                if (!expect(turnStartRequestId == requestId && turnStartGeneration > 0,
                            "turn/start response lost exact request generation binding")) {
                    return false;
                }
            }
            client.stop();
            if (expectReady) {
                waitUntil([&]() {
                    return logContainsMethod(logPath, QStringLiteral("shutdown"));
                });
            }
        } else {
            if (!expect(!initialized && initializeFailed && !client.isReady(),
                        "invalid initialize response did not fail closed")) {
                return false;
            }
            if (!expect(!failureMessage.contains(QStringLiteral("secret-sentinel")),
                        "handshake diagnostic leaked response content")) {
                return false;
            }
            if (!expectedFailureCode.isEmpty()
                && !expect(failureMessage.contains(expectedFailureCode),
                           "handshake diagnostic omitted fixed upgrade direction")) {
                return false;
            }
            waitUntil([&]() { return true; }, 50);
        }
    }

    const QList<QJsonObject> messages = readLogMessages(logPath);
    if (!expect(!messages.isEmpty(), "fake runtime did not receive initialize")) return false;
    const QJsonObject initialize = messages.first();
    const QJsonObject params = initialize.value(QStringLiteral("params")).toObject();
    const QJsonObject protocol = params.value(QStringLiteral("protocol")).toObject();
    const QJsonObject capabilities = params.value(QStringLiteral("capabilities")).toObject();
    const QJsonObject limits = params.value(QStringLiteral("limits")).toObject();
    if (!expect(initialize.value(QStringLiteral("jsonrpc")).toString()
                    == QStringLiteral("2.0")
                && initialize.value(QStringLiteral("method")).toString()
                    == QStringLiteral("initialize")
                && initialize.value(QStringLiteral("id")).isString()
                && protocol.value(QStringLiteral("minimum")).toString()
                    == QStringLiteral("0.1")
                && protocol.value(QStringLiteral("maximum")).toString()
                    == QStringLiteral("0.1")
                && protocol.value(QStringLiteral("preferred")).toString()
                    == QStringLiteral("0.1")
                && params.value(QStringLiteral("client")).toObject()
                       .value(QStringLiteral("name")).toString()
                    == QStringLiteral("aegisy-client")
                && params.value(QStringLiteral("platform")).toObject() == testPlatform()
                && !capabilities.value(QStringLiteral("stable")).toArray().isEmpty()
                && capabilities.value(QStringLiteral("stable")).toArray().contains(
                    QStringLiteral("workspace.edit.proposal.read-only"))
                && capabilities.value(QStringLiteral("experimental")).toArray().isEmpty()
                && limits.value(QStringLiteral("max_frame_bytes")).toInt()
                    == 4 * 1024 * 1024
                && params.value(QStringLiteral("transport_security")).toObject()
                    == testTransportSecurity(),
                "initialize request did not declare the bounded protocol/capability contract")) {
        return false;
    }
    if (!expect(!logContainsMethod(logPath, QStringLiteral("project/list")),
                "pre-handshake business request reached the runtime")) {
        return false;
    }
    if (expectAccepted) {
        const bool catalogExpected = testCase == QStringLiteral("valid-recovery");
        const auto initialized = std::find_if(
            messages.cbegin(), messages.cend(), [](const QJsonObject &message) {
                return message.value(QStringLiteral("method")).toString()
                    == QStringLiteral("initialized");
            });
        return expect(initialized != messages.cend()
                          && initialized->value(QStringLiteral("params")).isObject()
                          && initialized->value(QStringLiteral("params")).toObject().isEmpty()
                          && !initialized->contains(QStringLiteral("id")),
                      "valid response did not produce exact initialized notification")
            && expect(logContainsMethod(logPath, QStringLiteral("model/catalog"))
                          == catalogExpected,
                      "client did not honor the negotiated model-catalog capability");
    }
    return expect(messages.size() == 1,
                  "invalid response produced initialized or business traffic");
}

bool runLateInitializeResponseCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create late initialize directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("initialize-late"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "late initialize handshake did not complete")) {
            return false;
        }
        waitUntil([&]() { return disconnected; }, 100);
        if (!expect(!disconnected && client.isReady(),
                    "late duplicate initialize response closed a healthy connection")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return !client.isControlAvailable(); });
    }
    return true;
}

bool runHeartbeatNormalCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create heartbeat normal directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("heartbeat-normal"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool livenessUnknown = false;
    {
        AgentRuntimeClient client(nullptr, 20, 80);
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeLivenessChanged,
                         [&livenessUnknown](bool healthy, const QString &) {
            if (!healthy) livenessUnknown = true;
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return initialized
                    && !logMessagesForMethod(logPath,
                            QStringLiteral("runtime/heartbeat")).isEmpty();
            }), "normal heartbeat was not requested")) {
            return false;
        }
        if (!expect(client.isReady() && client.isHeartbeatHealthy()
                        && client.isControlAvailable(),
                    "normal heartbeat did not preserve operational state")) {
            return false;
        }
        waitUntil([]() { return false; }, 140);
        if (!expect(!livenessUnknown && client.isReady(),
                    "healthy heartbeat entered Unknown state")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return !client.isControlAvailable(); });
    }

    const QList<QJsonObject> heartbeats = logMessagesForMethod(
        logPath, QStringLiteral("runtime/heartbeat"));
    if (!expect(heartbeats.size() >= 2,
                "heartbeat interval did not issue repeated single-flight probes")) {
        return false;
    }
    QSet<QString> nonces;
    for (const QJsonObject &heartbeat : heartbeats) {
        const QJsonObject params = heartbeat.value(QStringLiteral("params")).toObject();
        const QString nonce = params.value(QStringLiteral("nonce")).toString();
        if (!expect(params.size() == 2
                        && params.value(QStringLiteral("schema_version")).toString()
                            == QStringLiteral("runtime-heartbeat-request/0.1")
                        && !nonce.isEmpty() && nonce.toUtf8().size() <= 64
                        && !nonces.contains(nonce),
                    "heartbeat request contract or nonce uniqueness was invalid")) {
            return false;
        }
        nonces.insert(nonce);
    }
    return true;
}

bool runHeartbeatTimeoutCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create heartbeat timeout directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("heartbeat-timeout"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool unknown = false;
    bool disconnectedAfterInitialize = false;
    bool ordinaryFailed = false;
    bool cancellationRequested = false;
    bool terminalStopped = false;
    {
        AgentRuntimeClient client(nullptr, 20, 80);
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeLivenessChanged,
                         [&unknown](bool healthy, const QString &) {
            if (!healthy) unknown = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnectedAfterInitialize](
                             bool ready, const QString &) {
            if (initialized && !ready) disconnectedAfterInitialize = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&ordinaryFailed](const QString &, const QString &method,
                                           const QString &, int) {
            if (method == QStringLiteral("session/list")) ordinaryFailed = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::terminalStopped,
                         [&terminalStopped](const QString &, const QJsonObject &) {
            terminalStopped = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::turnCancellationRequested,
                         [&cancellationRequested](const QString &, const QJsonObject &) {
            cancellationRequested = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "heartbeat timeout handshake did not complete")) {
            return false;
        }
        const QString pending = client.listSessions();
        if (!expect(!pending.isEmpty(), "ordinary request was not pending before timeout")) {
            return false;
        }
        if (!expect(waitUntil([&]() { return unknown && ordinaryFailed; }),
                    "heartbeat timeout did not enter Unknown and clear ordinary pending")) {
            return false;
        }
        if (!expect(!client.isReady() && !client.isHeartbeatHealthy()
                        && client.isControlAvailable()
                        && !disconnectedAfterInitialize,
                    "heartbeat timeout changed connection or control availability")) {
            return false;
        }
        const int sentSessionLists = logMessagesForMethod(
            logPath, QStringLiteral("session/list")).size();
        if (!expect(client.listSessions().isEmpty()
                        && logMessagesForMethod(logPath,
                               QStringLiteral("session/list")).size() == sentSessionLists,
                    "ordinary request escaped while liveness was Unknown")) {
            return false;
        }
        if (!expect(!client.cancelTurn(QStringLiteral("session-1"),
                                       QStringLiteral("turn-1")).isEmpty()
                        && waitUntil([&]() { return cancellationRequested; }),
                    "turn cancellation was unavailable while liveness was Unknown")) {
            return false;
        }
        if (!expect(!client.stopUserTerminal(QStringLiteral("session-1"),
                                             QStringLiteral("terminal-1")).isEmpty()
                        && waitUntil([&]() { return terminalStopped; }),
                    "terminal stop control was unavailable while liveness was Unknown")) {
            return false;
        }
        if (!expect(!disconnectedAfterInitialize && !client.isReady()
                        && client.isControlAvailable(),
                    "control response incorrectly restored or disconnected liveness")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return !client.isControlAvailable(); });
    }
    return true;
}

bool runHeartbeatLateAndRehandshakeCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create heartbeat rehandshake directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("heartbeat-late"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    int initializedCount = 0;
    bool unknown = false;
    bool healthyAfterUnknown = false;
    bool terminalStopped = false;
    AgentRuntimeClient client(nullptr, 20, 80);
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initializedCount](const QJsonObject &) { ++initializedCount; });
    QObject::connect(&client, &AgentRuntimeClient::runtimeLivenessChanged,
                     [&unknown, &healthyAfterUnknown](bool healthy, const QString &) {
        if (!healthy) unknown = true;
        else if (unknown) healthyAfterUnknown = true;
    });
    QObject::connect(&client, &AgentRuntimeClient::terminalStopped,
                     [&terminalStopped](const QString &, const QJsonObject &) {
        terminalStopped = true;
    });
    client.start();
    if (!expect(waitUntil([&]() { return initializedCount == 1 && unknown; }),
                "late heartbeat case did not reach Unknown")) {
        return false;
    }
    if (!expect(!client.stopUserTerminal(QStringLiteral("session-1"),
                                         QStringLiteral("terminal-1")).isEmpty()
                    && waitUntil([&]() { return terminalStopped; }),
                "late heartbeat control response was not processed")) {
        return false;
    }
    if (!expect(!healthyAfterUnknown && !client.isReady()
                    && client.isControlAvailable(),
                "late heartbeat response restored liveness")) {
        return false;
    }

    client.stop();
    if (!expect(waitUntil([&]() { return !client.isControlAvailable(); }),
                "Unknown runtime did not complete shutdown cleanup")) {
        return false;
    }
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("heartbeat-normal"));
    client.start();
    if (!expect(waitUntil([&]() {
            return initializedCount == 2 && client.isReady()
                && logMessagesForMethod(logPath,
                       QStringLiteral("runtime/heartbeat")).size() >= 2;
        }), "fresh handshake did not restore heartbeat liveness")) {
        return false;
    }
    const QList<QJsonObject> heartbeats = logMessagesForMethod(
        logPath, QStringLiteral("runtime/heartbeat"));
    const QString firstNonce = heartbeats.first().value(QStringLiteral("params"))
        .toObject().value(QStringLiteral("nonce")).toString();
    const QString lastNonce = heartbeats.last().value(QStringLiteral("params"))
        .toObject().value(QStringLiteral("nonce")).toString();
    if (!expect(!firstNonce.isEmpty() && !lastNonce.isEmpty()
                    && firstNonce != lastNonce,
                "fresh QProcess generation reused the stale heartbeat nonce")) {
        return false;
    }
    client.stop();
    waitUntil([&]() { return !client.isControlAvailable(); });
    return true;
}

bool runHeartbeatSubscriptionOwnershipCase(const QString &stage,
                                           const QString &method)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create heartbeat subscription ownership directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE",
            QStringLiteral("heartbeat-subscription-%1").arg(stage).toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool requestFailed = false;
    bool connectionAbandoned = false;
    bool livenessUnknown = false;
    int reconnectHandshakes = 0;
    QString pendingRequestId;
    AgentRuntimeClient client(nullptr, 20, 100, {0, 10, 20});
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initialized](const QJsonObject &) { initialized = true; });
    QObject::connect(&client, &AgentRuntimeClient::runtimeLivenessChanged,
                     [&livenessUnknown](bool healthy, const QString &) {
        if (!healthy) livenessUnknown = true;
    });
    QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                     [&initialized, &connectionAbandoned](bool ready, const QString &) {
        if (initialized && !ready) connectionAbandoned = true;
    });
    QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                     [&pendingRequestId, &requestFailed, &method](
                         const QString &requestId, const QString &failedMethod,
                         const QString &, int) {
        if (!pendingRequestId.isEmpty() && requestId == pendingRequestId
            && failedMethod == method) {
            requestFailed = true;
        }
    });
    QObject::connect(&client, &AgentRuntimeClient::reconnectHandshakeReady,
                     [&client, &reconnectHandshakes](quint64 generation,
                                                     const QJsonObject &) {
        ++reconnectHandshakes;
        client.completeReconnectRecovery(
            generation, true, QStringLiteral("subscription ownership recovered"));
    });

    client.start();
    if (!expect(waitUntil([&]() { return initialized; }),
                "heartbeat subscription ownership handshake did not complete")) {
        return false;
    }
    const quint64 firstGeneration = client.processGeneration();
    const QJsonObject zeroAnchor{
        {QStringLiteral("sequence"), 0},
        {QStringLiteral("event_id"), QJsonValue(QJsonValue::Null)},
    };
    const QJsonObject oneAnchor{
        {QStringLiteral("sequence"), 1},
        {QStringLiteral("event_id"),
         QStringLiteral("event:sha256:") + QString(64, QLatin1Char('a'))},
    };
    if (stage == QStringLiteral("subscribe")) {
        pendingRequestId = client.subscribeTimeline(
            QStringLiteral("session-1"), firstGeneration, 0);
    } else if (stage == QStringLiteral("sync")) {
        pendingRequestId = client.syncTimelineSubscription(
            firstGeneration, QStringLiteral("session-1"),
            QStringLiteral("subscription-1"), 0, QString(), oneAnchor, 100);
    } else if (stage == QStringLiteral("snapshot")) {
        pendingRequestId = client.snapshotTimelineSubscription(
            firstGeneration, QStringLiteral("session-1"),
            QStringLiteral("subscription-1"), zeroAnchor);
    } else if (stage == QStringLiteral("activate")) {
        pendingRequestId = client.activateTimelineSubscription(
            firstGeneration, QStringLiteral("session-1"),
            QStringLiteral("subscription-1"), QStringLiteral("sync"),
            zeroAnchor, zeroAnchor);
    }
    if (!expect(!pendingRequestId.isEmpty()
                    && waitUntil([&]() {
                        return logContainsMethod(logPath, method);
                    }),
                "Timeline subscription ownership request was not pending")) {
        return false;
    }
    const bool recovered = waitUntil([&]() {
            return requestFailed && connectionAbandoned && livenessUnknown
                && reconnectHandshakes == 1 && client.isReady()
                && client.processGeneration() > firstGeneration;
        }, 5000);
    if (!recovered) {
        std::fprintf(stderr,
                     "subscription ownership stage=%s failed=%d abandoned=%d unknown=%d handshakes=%d ready=%d generation=%llu first=%llu reconnect=%d\n",
                     stage.toUtf8().constData(), requestFailed, connectionAbandoned,
                     livenessUnknown, reconnectHandshakes, client.isReady(),
                     static_cast<unsigned long long>(client.processGeneration()),
                     static_cast<unsigned long long>(firstGeneration),
                     client.reconnectAttempt());
    }
    if (!expect(recovered,
                "ambiguous Timeline subscription ownership did not recover on a new generation")) {
        return false;
    }

    const QList<QJsonObject> initializes = logMessagesForMethod(
        logPath, QStringLiteral("initialize"));
    const QList<QJsonObject> stageRequests = logMessagesForMethod(logPath, method);
    const bool valid = expect(initializes.size() == 2,
                              "subscription ownership recovery did not use exactly one fresh process")
        && expect(stageRequests.size() == 1,
                  "subscription ownership request was retried on the ambiguous connection")
        && expect(client.reconnectAttempt() == 1,
                  "subscription ownership recovery escaped the bounded reconnect counter");
    client.stop();
    waitUntil([&]() { return !client.isControlAvailable(); });
    return valid;
}

bool runBoundedProcessReconnectCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create process reconnect directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("reconnect-exit-success"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    int handshakeReadyCount = 0;
    int recoverySignals = 0;
    bool sawWaiting = false;
    bool sawRestarting = false;
    {
        AgentRuntimeClient client(nullptr, 5000, 15000, {0, 10, 20});
        QObject::connect(&client, &AgentRuntimeClient::runtimeReconnectStateChanged,
                         [&sawWaiting, &sawRestarting](AgentRuntimeClient::ReconnectState state,
                                                       int, int maximum, int, const QString &) {
            if (state == AgentRuntimeClient::ReconnectState::Waiting) sawWaiting = maximum == 3;
            if (state == AgentRuntimeClient::ReconnectState::Restarting) sawRestarting = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::reconnectHandshakeReady,
                         [&client, &handshakeReadyCount, &recoverySignals](quint64 generation,
                                                                            const QJsonObject &) {
            ++handshakeReadyCount;
            ++recoverySignals;
            if (!client.completeReconnectRecovery(generation, true,
                                                  QStringLiteral("test recovery"))) {
                ++recoverySignals;
            }
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return handshakeReadyCount == 1 && client.isReady();
            }, 3000), "bounded process reconnect did not recover")) {
            return false;
        }
        if (!expect(sawWaiting && sawRestarting && recoverySignals == 1
                        && client.reconnectAttempt() == 1
                        && client.processGeneration() > 1,
                    "process reconnect state or generation contract was invalid")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return !client.isControlAvailable(); });
    }
    const QList<QJsonObject> initializes = logMessagesForMethod(
        logPath, QStringLiteral("initialize"));
    return expect(initializes.size() == 2,
                  "successful process reconnect did not use exactly one new generation");
}

bool runControlledTimelineSubscriptionAbandonCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create controlled subscription abandon directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("heartbeat-normal"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    int reconnectHandshakes = 0;
    int disconnectedSignals = 0;
    AgentRuntimeClient client(nullptr, 5000, 15000, {0, 10, 20});
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initialized](const QJsonObject &) { initialized = true; });
    QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                     [&initialized, &disconnectedSignals](bool ready, const QString &) {
        if (initialized && !ready) ++disconnectedSignals;
    });
    QObject::connect(&client, &AgentRuntimeClient::reconnectHandshakeReady,
                     [&client, &reconnectHandshakes](quint64 generation,
                                                     const QJsonObject &) {
        ++reconnectHandshakes;
        client.completeReconnectRecovery(
            generation, true, QStringLiteral("controlled subscription recovery"));
    });

    client.start();
    if (!expect(waitUntil([&]() { return initialized && client.isReady(); }),
                "controlled subscription abandon handshake did not complete")) {
        return false;
    }
    const quint64 firstGeneration = client.processGeneration();
    client.abandonTimelineSubscriptionConnection(
        QStringLiteral("test requested subscription generation replacement"));
    client.abandonTimelineSubscriptionConnection(
        QStringLiteral("duplicate replacement request must be inert"));
    const bool recovered = waitUntil([&]() {
        return reconnectHandshakes == 1 && disconnectedSignals >= 1
            && client.isReady() && client.processGeneration() > firstGeneration;
    }, 5000);
    const QList<QJsonObject> initializes = logMessagesForMethod(
        logPath, QStringLiteral("initialize"));
    const bool valid = expect(recovered,
                              "controlled subscription abandon did not start a fresh generation")
        && expect(initializes.size() == 2,
                  "controlled subscription abandon launched more than one replacement generation")
        && expect(client.reconnectAttempt() == 1,
                  "controlled subscription abandon escaped the bounded reconnect counter");
    client.stop();
    waitUntil([&]() { return !client.isControlAvailable(); });
    return valid;
}

bool runProcessReconnectExhaustionCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create reconnect exhaustion directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("reconnect-exhaust"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    int waitingCount = 0;
    QList<int> delays;
    bool exhausted = false;
    {
        AgentRuntimeClient client(nullptr, 5000, 15000, {0, 10, 20});
        QObject::connect(&client, &AgentRuntimeClient::runtimeReconnectStateChanged,
                         [&waitingCount, &delays, &exhausted](
                             AgentRuntimeClient::ReconnectState state,
                             int attempt, int, int nextDelay, const QString &) {
            if (state == AgentRuntimeClient::ReconnectState::Waiting) {
                ++waitingCount;
                if (attempt > 0) delays.append(nextDelay);
            } else if (state == AgentRuntimeClient::ReconnectState::Exhausted) {
                exhausted = true;
            }
        });
        client.start();
        // Four bounded relaunches of a Qt process on a loaded CI runner can
        // take far longer than the interactive budget.
        if (!expect(waitUntil([&]() { return exhausted; }, 15000),
                    "bounded process reconnect did not exhaust")) {
            return false;
        }
        if (!expect(!client.isReady() && client.reconnectAttempt() == 3,
                    "reconnect exhaustion exposed an incorrect ready or attempt state")) {
            return false;
        }
        client.stop();
    }
    const QList<QJsonObject> initializes = logMessagesForMethod(
        logPath, QStringLiteral("initialize"));
    return expect(waitingCount == 3 && delays == QList<int>{0, 10, 20}
                      && initializes.size() == 4,
                  "reconnect backoff or attempt cap was not exact");
}

bool runTransportCorrelationIdCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create Transport correlation directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("transport-correlation-ids"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool healthRead = false;
    bool degradationsRead = false;
    bool disconnected = false;
    int failedRequests = 0;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeHealthRead,
                         [&healthRead](const QJsonObject &) { healthRead = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeDegradationsRead,
                         [&degradationsRead](const QString &, const QJsonObject &) {
            degradationsRead = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&failedRequests](const QString &, const QString &,
                                           const QString &, int) {
            ++failedRequests;
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return initialized && healthRead && degradationsRead;
            }), "wrong/null/retired IDs consumed the live pending response")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected && failedRequests == 0,
                    "unmatched response IDs changed negotiated client state")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runTransportHugeErrorCodeCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create huge error-code directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("transport-huge-error-code"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    QString exactRequestId;
    QString exactCode;
    QString compatibilityRequestId;
    QString compatibilityMessage;
    int compatibilityCode = -1;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::requestFailedExact,
                         [&exactRequestId, &exactCode](
                             const QString &requestId, const QString &method,
                             const QString &, const QString &canonicalCode) {
            if (method == QStringLiteral("runtime/health")) {
                exactRequestId = requestId;
                exactCode = canonicalCode;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&compatibilityRequestId, &compatibilityMessage,
                          &compatibilityCode](const QString &requestId,
                                              const QString &method,
                                              const QString &message, int code) {
            if (method == QStringLiteral("runtime/health")) {
                compatibilityRequestId = requestId;
                compatibilityMessage = message;
                compatibilityCode = code;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return initialized && !exactRequestId.isEmpty();
            }), "huge mathematical error code was not surfaced")) {
            return false;
        }
        const bool hugeCodeValid = compatibilityRequestId.isEmpty()
            && compatibilityMessage.isEmpty()
            && compatibilityCode == -1
            && exactCode == QStringLiteral("12345678901234567890123456789e1")
            && client.isReady() && !disconnected;
        if (!expect(hugeCodeValid,
                    "huge error code was narrowed, mapped to a compatibility int, or disconnected")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runTransportConcurrentReverseCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create Transport reverse-order directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("transport-concurrent-reverse"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    int healthReads = 0;
    QString degradationCreatedId;
    QString degradationReadId;
    QString failedId;
    QString failedMethod;
    int failedCode = 0;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeHealthRead,
                         [&healthReads](const QJsonObject &) { ++healthReads; });
        QObject::connect(&client,
                         &AgentRuntimeClient::runtimeDegradationRequestCreated,
                         [&degradationCreatedId](const QString &requestId) {
            degradationCreatedId = requestId;
        });
        QObject::connect(&client, &AgentRuntimeClient::runtimeDegradationsRead,
                         [&degradationReadId](const QString &requestId,
                                              const QJsonObject &) {
            degradationReadId = requestId;
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&failedId, &failedMethod, &failedCode](
                             const QString &requestId, const QString &method,
                             const QString &, int code) {
            if (method == QStringLiteral("runtime/health")) {
                failedId = requestId;
                failedMethod = method;
                failedCode = code;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return initialized && healthReads == 1
                    && !degradationReadId.isEmpty();
            }), "reverse-order different-method startup responses were not correlated")) {
            return false;
        }
        if (!expect(degradationReadId == degradationCreatedId,
                    "reverse-order degradation response lost its exact request ID")) {
            return false;
        }
        const QString firstHealthId = client.runtimeHealth();
        const QString secondHealthId = client.runtimeHealth();
        if (!expect(!firstHealthId.isEmpty() && !secondHealthId.isEmpty()
                        && firstHealthId != secondHealthId,
                    "same-method concurrent requests were not both admitted")) {
            return false;
        }
        if (!expect(waitUntil([&]() {
                return healthReads == 2 && failedId == secondHealthId;
            }), "reverse-order same-method success/error responses crossed pending contexts")) {
            return false;
        }
        if (!expect(failedMethod == QStringLiteral("runtime/health")
                        && failedCode == -32060 && client.isReady() && !disconnected,
                    "same-method reverse-order completion changed transport state")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runTransportOldGenerationCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create old-generation Transport directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("transport-old-generation"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    int initializedCount = 0;
    int healthReads = 0;
    int reconnectHandshakes = 0;
    int processFailures = 0;
    quint64 firstGeneration = 0;
    AgentRuntimeClient client(nullptr, 5000, 15000, {0, 10, 20});
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initializedCount, &firstGeneration, &client](
                         const QJsonObject &) {
        ++initializedCount;
        if (firstGeneration == 0) firstGeneration = client.processGeneration();
    });
    QObject::connect(&client, &AgentRuntimeClient::runtimeHealthRead,
                     [&healthReads](const QJsonObject &) { ++healthReads; });
    QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                     [&processFailures](const QString &, const QString &method,
                                        const QString &, int) {
        if (method == QStringLiteral("runtime/health")) ++processFailures;
    });
    QObject::connect(&client, &AgentRuntimeClient::reconnectHandshakeReady,
                     [&client, &reconnectHandshakes](quint64 generation,
                                                     const QJsonObject &) {
        ++reconnectHandshakes;
        client.completeReconnectRecovery(
            generation, true, QStringLiteral("old-generation response test"));
    });
    client.start();
    if (!expect(waitUntil([&]() {
            return initializedCount == 2 && reconnectHandshakes == 1
                && healthReads == 1 && client.isReady()
                && client.processGeneration() > firstGeneration;
        }, 5000), "old-generation response prevented current request completion")) {
        return false;
    }
    const QList<QJsonObject> healthRequests = logMessagesForMethod(
        logPath, QStringLiteral("runtime/health"));
    const bool valid = expect(processFailures == 1,
                              "old generation did not fail exactly its owned pending request")
        && expect(healthReads == 1,
                  "old-generation response was projected as a current health result")
        && expect(healthRequests.size() == 2,
                  "old-generation test did not issue one request per process generation");
    client.stop();
    waitUntil([&]() { return !client.isControlAvailable(); });
    return valid;
}

QString subscriptionMethodForStage(const QString &stage)
{
    if (stage == QStringLiteral("subscribe")) return QStringLiteral("timeline/subscribe");
    if (stage == QStringLiteral("sync")) {
        return QStringLiteral("timeline/subscription-sync");
    }
    if (stage == QStringLiteral("snapshot")) {
        return QStringLiteral("timeline/subscription-snapshot");
    }
    if (stage == QStringLiteral("activate")) {
        return QStringLiteral("timeline/subscription-activate");
    }
    return {};
}

bool runTransportSubscriptionErrorCase(const QString &stage,
                                       const QString &variant)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create subscription typed-error directory")) {
        return false;
    }
    const QString testCase = QStringLiteral("transport-subscription-error-%1-%2")
        .arg(stage, variant);
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    int typedFailures = 0;
    QString typedFailureId;
    QJsonObject typedFailure;
    QString requestId;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::timelineSubscriptionFailed,
                         [&typedFailures, &typedFailureId, &typedFailure](
                             const QString &failedId, const QJsonObject &failure) {
            ++typedFailures;
            typedFailureId = failedId;
            typedFailure = failure;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "subscription typed-error handshake timed out")) {
            return false;
        }
        const quint64 generation = client.processGeneration();
        const QJsonObject zeroAnchor = timelineAnchor(
            0, QJsonValue(QJsonValue::Null));
        if (stage == QStringLiteral("subscribe")) {
            requestId = client.subscribeTimeline(
                QStringLiteral("session-transport"), generation, 0);
        } else if (stage == QStringLiteral("sync")) {
            requestId = client.syncTimelineSubscription(
                generation, QStringLiteral("session-transport"),
                QStringLiteral("subscription-transport"), 0, QString(),
                zeroAnchor, 100);
        } else if (stage == QStringLiteral("snapshot")) {
            requestId = client.snapshotTimelineSubscription(
                generation, QStringLiteral("session-transport"),
                QStringLiteral("subscription-transport"), zeroAnchor);
        } else if (stage == QStringLiteral("activate")) {
            requestId = client.activateTimelineSubscription(
                generation, QStringLiteral("session-transport"),
                QStringLiteral("subscription-transport"), QStringLiteral("sync"),
                zeroAnchor, zeroAnchor);
        }
        if (!expect(!requestId.isEmpty(),
                    "subscription typed-error request was not admitted")) {
            return false;
        }
        if (variant == QStringLiteral("valid")) {
            if (!expect(waitUntil([&]() { return typedFailures == 1; })
                            && typedFailureId == requestId
                            && typedFailure.value(QStringLiteral("stage")).toString()
                                == subscriptionFailureStage(
                                    subscriptionMethodForStage(stage))
                            && client.isReady() && !disconnected,
                        "valid subscription typed error lost request binding")) {
                return false;
            }
            client.stop();
            waitUntil([&]() {
                return logContainsMethod(logPath, QStringLiteral("shutdown"));
            });
        } else if (!expect(waitUntil([&]() { return disconnected; })
                               && typedFailures == 0 && !client.isReady(),
                           "subscription stage/identity drift did not fail closed")) {
            return false;
        }
    }
    return true;
}

bool runTransportMutationReverseCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create mutation reverse-order directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("transport-mutation-reverse"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    QString listedId;
    QString consumedId;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client,
                         &AgentRuntimeClient::mutationAcknowledgementsListed,
                         [&listedId](const QString &requestId, const QJsonObject &) {
            listedId = requestId;
        });
        QObject::connect(&client,
                         &AgentRuntimeClient::mutationAcknowledgementConsumed,
                         [&consumedId](const QString &requestId, const QJsonObject &) {
            consumedId = requestId;
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "mutation reverse-order handshake timed out")) {
            return false;
        }
        const QString sessionId = QStringLiteral("session-mutation-transport");
        const QString listId = client.listMutationAcknowledgements(sessionId);
        const QString consumeId = client.consumeMutationAcknowledgement(
            sessionId, testMutationOperationIdentity(sessionId), 2,
            QStringLiteral("accepted"),
            timelineAnchor(1, timelineEventId(QLatin1Char('a'))));
        if (!expect(!listId.isEmpty() && !consumeId.isEmpty() && listId != consumeId,
                    "mutation list/consume requests were not concurrently admitted")) {
            return false;
        }
        if (!expect(waitUntil([&]() {
                return listedId == listId && consumedId == consumeId;
            }), "reverse-order mutation responses crossed request contexts")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "valid reverse-order mutation responses closed the transport")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runTransportMutationDriftCase(bool consume)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create mutation echo-drift directory")) {
        return false;
    }
    const QString testCase = consume
        ? QStringLiteral("transport-mutation-consume-drift")
        : QStringLiteral("transport-mutation-list-drift");
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    int acceptedSignals = 0;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        QObject::connect(&client,
                         &AgentRuntimeClient::mutationAcknowledgementsListed,
                         [&acceptedSignals](const QString &, const QJsonObject &) {
            ++acceptedSignals;
        });
        QObject::connect(&client,
                         &AgentRuntimeClient::mutationAcknowledgementConsumed,
                         [&acceptedSignals](const QString &, const QJsonObject &) {
            ++acceptedSignals;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "mutation echo-drift handshake timed out")) {
            return false;
        }
        const QString sessionId = QStringLiteral("session-mutation-transport");
        const QString requestId = consume
            ? client.consumeMutationAcknowledgement(
                  sessionId, testMutationOperationIdentity(sessionId), 2,
                  QStringLiteral("accepted"),
                  timelineAnchor(1, timelineEventId(QLatin1Char('a'))))
            : client.listMutationAcknowledgements(sessionId);
        if (!expect(!requestId.isEmpty(),
                    "mutation echo-drift request was not admitted")) {
            return false;
        }
        if (!expect(waitUntil([&]() { return disconnected; })
                        && acceptedSignals == 0 && !client.isReady(),
                    "mutation response echo drift did not fail closed")) {
            return false;
        }
    }
    return true;
}

bool runRawTransportParserRejectionCase(const QString &testCase)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create raw Transport parser directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    AgentRuntimeClient client;
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initialized](const QJsonObject &) { initialized = true; });
    QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                     [&initialized, &disconnected](bool ready, const QString &) {
        if (initialized && !ready) disconnected = true;
    });
    client.start();
    return expect(waitUntil([&]() { return initialized && disconnected; }, 5000)
                      && !client.isReady(),
                  "lossless Transport parser accepted a forbidden raw frame");
}

bool runOrdinaryEnvelopeCase(const QString &testCase)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create ordinary-envelope directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    bool pendingFailed = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&pendingFailed](const QString &, const QString &method,
                                          const QString &, int) {
            if (method == QStringLiteral("runtime/health")) pendingFailed = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized && disconnected; }),
                    "malformed post-handshake envelope did not close the connection")) {
            return false;
        }
        if (!expect(!client.isReady(),
                    "malformed post-handshake envelope retained ready state")) {
            return false;
        }
        if (!expect(pendingFailed,
                    "malformed response consumed or abandoned the pending request")) {
            return false;
        }
        const QList<QJsonObject> before = readLogMessages(logPath);
        const int healthRequestsBefore = int(std::count_if(
            before.cbegin(), before.cend(),
            [](const QJsonObject &message) {
                return message.value(QStringLiteral("method")).toString()
                    == QStringLiteral("runtime/health");
            }));
        if (!expect(client.runtimeHealth().isEmpty(),
                    "request escaped after protocol violation cleared negotiation")) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        const QList<QJsonObject> after = readLogMessages(logPath);
        const int healthRequestsAfter = int(std::count_if(
            after.cbegin(), after.cend(), [](const QJsonObject &message) {
                return message.value(QStringLiteral("method")).toString()
                    == QStringLiteral("runtime/health");
            }));
        if (!expect(healthRequestsAfter == healthRequestsBefore,
                    "cleared capability was reused after protocol disconnect")) {
            return false;
        }
    }
    return expect(!logContainsMethod(logPath, QStringLiteral("project/list")),
                  "protocol violation allowed unrelated business traffic");
}

bool runCombinedFramesCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create combined-frame directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("combined-legal-frames"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool healthRead = false;
    bool degradationsRead = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeHealthRead,
                         [&healthRead](const QJsonObject &) { healthRead = true; });
        QObject::connect(&client, &AgentRuntimeClient::runtimeDegradationsRead,
                         [&degradationsRead](const QString &, const QJsonObject &) {
            degradationsRead = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() {
                return initialized && healthRead && degradationsRead;
            }, 8000), "combined legal frames were not independently accepted")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "combined legal frames were mistaken for one oversized frame")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runCapabilityGateCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create capability-gate directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("valid-list-only"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool capabilityRejected = false;
    bool searchCapabilityRejected = false;
    bool timelineSyncCapabilityRejected = false;
    bool proposalCapabilityRejected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&capabilityRejected, &searchCapabilityRejected,
                          &timelineSyncCapabilityRejected, &proposalCapabilityRejected](
                             const QString &, const QString &method,
                             const QString &, int code) {
            if (method == QStringLiteral("project/list") && code == -32601) {
                capabilityRejected = true;
            } else if (method == QStringLiteral("session/search") && code == -32601) {
                searchCapabilityRejected = true;
            } else if (method == QStringLiteral("timeline/sync") && code == -32601) {
                timelineSyncCapabilityRejected = true;
            } else if (method == QStringLiteral("workspace/edit/proposal/latest")
                       && code == -32601) {
                proposalCapabilityRejected = true;
            }
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "capability-gate handshake timed out")) {
            return false;
        }
        if (!expect(client.listProjects().isEmpty() && capabilityRejected,
                    "missing optional capability did not reject the dependent method")) {
            return false;
        }
        if (!expect(!client.listSessions().isEmpty(),
                    "negotiated session.list capability did not allow session/list")) {
            return false;
        }
        if (!expect(client.searchSessions(QStringLiteral("query")).isEmpty()
                        && searchCapabilityRejected,
                    "session.list incorrectly authorized session/search")) {
            return false;
        }
        if (!expect(client.syncTimeline(QStringLiteral("session-1"), 0).isEmpty()
                        && timelineSyncCapabilityRejected,
                    "missing Timeline replay capability did not reject timeline/sync")) {
            return false;
        }
        if (!expect(client.latestWorkspaceEditProposal(QStringLiteral("session-1")).isEmpty()
                        && proposalCapabilityRejected,
                    "missing Proposal capability did not reject latest read")) {
            return false;
        }
        if (!expect(!client.runtimeHealth().isEmpty(),
                    "negotiated capability did not allow its method")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return expect(!logContainsMethod(logPath, QStringLiteral("project/list")),
                  "capability-rejected method reached the runtime")
        && expect(logContainsMethod(logPath, QStringLiteral("session/list")),
                  "negotiated session/list request did not reach the runtime")
        && expect(!logContainsMethod(logPath, QStringLiteral("session/search")),
                  "session/search escaped without session.search.branch")
        && expect(!logContainsMethod(logPath, QStringLiteral("timeline/sync")),
                  "timeline/sync escaped without its negotiated capability")
        && expect(!logContainsMethod(logPath, QStringLiteral("workspace/edit/proposal/latest")),
                  "Proposal latest escaped without its negotiated capability");
}

bool runTimelineSyncContractCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create Timeline sync directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("timeline-sync-contract"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    const QString eventA = timelineEventId(QLatin1Char('a'));
    const QString eventB = timelineEventId(QLatin1Char('b'));
    const QJsonObject fixedWatermark = timelineAnchor(9, eventB);
    bool initialized = false;
    int invalidRequests = 0;
    QHash<QString, QJsonObject> pages;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineSynced,
                         [&pages](const QString &requestId, const QJsonObject &page) {
            pages.insert(requestId, page);
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&invalidRequests](const QString &requestId,
                                            const QString &method,
                                            const QString &, int code) {
            if (requestId.isEmpty() && method == QStringLiteral("timeline/sync")
                && code == -32602) {
                ++invalidRequests;
            }
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "Timeline sync handshake timed out")) {
            return false;
        }
        const QJsonObject invalidWatermark = timelineAnchor(0, eventA);
        const QJsonObject earlierWatermark = timelineAnchor(8, eventA);
        if (!expect(client.syncTimeline(QStringLiteral("session-1"), 0, eventA).isEmpty()
                        && client.syncTimeline(QStringLiteral("session-1"), 1).isEmpty()
                        && client.syncTimeline(QStringLiteral("session-1"), 0, {},
                                               invalidWatermark).isEmpty()
                        && client.syncTimeline(QStringLiteral("session-1"), 9, eventB,
                                               earlierWatermark).isEmpty()
                        && invalidRequests == 4,
                    "invalid Timeline anchors were not rejected before transport")) {
            return false;
        }

        const QString firstRequest = client.syncTimeline(
            QStringLiteral("session-1"), 0, {}, {}, 999);
        if (!expect(!firstRequest.isEmpty()
                        && waitUntil([&]() { return pages.contains(firstRequest); }),
                    "initial Timeline sync page was not signalled")) {
            return false;
        }
        const QString secondRequest = client.syncTimeline(
            QStringLiteral("session-1"), 9, eventB, fixedWatermark, 25);
        if (!expect(!secondRequest.isEmpty() && secondRequest != firstRequest
                        && waitUntil([&]() { return pages.contains(secondRequest); }),
                    "fixed-watermark Timeline sync page was not signalled")) {
            return false;
        }
        if (!expect(pages.value(firstRequest).value(QStringLiteral("schema_version"))
                            == QStringLiteral("timeline-sync-page/0.1")
                        && pages.value(firstRequest).value(QStringLiteral("session_id"))
                            == QStringLiteral("session-1")
                        && pages.value(secondRequest).value(QStringLiteral("watermark"))
                            == fixedWatermark,
                    "Timeline sync response signal did not preserve the page contract")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }

    QList<QJsonObject> syncRequests;
    QJsonObject initializeRequest;
    for (const QJsonObject &message : readLogMessages(logPath)) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("initialize")) initializeRequest = message;
        if (method == QStringLiteral("timeline/sync")) syncRequests.append(message);
    }
    const QJsonArray declared = initializeRequest.value(QStringLiteral("params")).toObject()
                                    .value(QStringLiteral("capabilities")).toObject()
                                    .value(QStringLiteral("stable")).toArray();
    if (!expect(declared.contains(QStringLiteral("timeline.replay.fixed-watermark")),
                "initialize did not declare the Timeline replay capability")
        || !expect(syncRequests.size() == 2,
                   "invalid Timeline requests reached the runtime")) {
        return false;
    }
    const QJsonObject firstParams = syncRequests.at(0).value(QStringLiteral("params")).toObject();
    const QJsonObject secondParams = syncRequests.at(1).value(QStringLiteral("params")).toObject();
    return expect(firstParams == QJsonObject{
                      {QStringLiteral("session_id"), QStringLiteral("session-1")},
                      {QStringLiteral("after"),
                       timelineAnchor(0, QJsonValue(QJsonValue::Null))},
                      {QStringLiteral("watermark"), QJsonValue(QJsonValue::Null)},
                      {QStringLiteral("limit"), 200},
                  }, "initial Timeline sync request did not normalize its anchor")
        && expect(secondParams == QJsonObject{
                      {QStringLiteral("session_id"), QStringLiteral("session-1")},
                      {QStringLiteral("after"), timelineAnchor(9, eventB)},
                      {QStringLiteral("watermark"), fixedWatermark},
                      {QStringLiteral("limit"), 25},
                  }, "fixed-watermark Timeline sync request changed its contract");
}

bool runTimelineSnapshotContractCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create Timeline snapshot directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("timeline-snapshot-contract"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    int invalidRequests = 0;
    QHash<QString, QJsonObject> pages;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineSnapshotReceived,
                         [&pages](const QString &requestId, const QJsonObject &page) {
            pages.insert(requestId, page);
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&invalidRequests](const QString &requestId,
                                            const QString &method,
                                            const QString &, int code) {
            if (requestId.isEmpty() && method == QStringLiteral("timeline/snapshot")
                && code == -32602) {
                ++invalidRequests;
            }
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "Timeline snapshot handshake timed out")) {
            return false;
        }
        const QString snapshotIdentity =
            QStringLiteral("timeline-session-snapshot:sha256:")
            + QString(64, QLatin1Char('a'));
        const QJsonObject watermark = timelineAnchor(
            3, timelineEventId(QLatin1Char('b')));
        const QJsonObject cursor{
            {QStringLiteral("ordinal"), 1},
            {QStringLiteral("item_id"), QStringLiteral("item-1")},
            {QStringLiteral("item_identity"),
             QStringLiteral("timeline-session-snapshot-item:sha256:")
                 + QString(64, QLatin1Char('c'))},
        };
        if (!expect(client.timelineSnapshot(QString(), snapshotIdentity, watermark, cursor)
                        .isEmpty()
                    && client.timelineSnapshot(QStringLiteral("session-1"), snapshotIdentity,
                                               {}, cursor).isEmpty()
                    && client.timelineSnapshot(QStringLiteral("session-1"),
                                               QStringLiteral("snapshot:bad"), watermark,
                                               cursor).isEmpty()
                    && client.timelineSnapshot(QStringLiteral("session-1"), snapshotIdentity,
                                               timelineAnchor(0, QJsonValue(QJsonValue::Null)),
                                               cursor).isEmpty()
                    && client.timelineSnapshot(QStringLiteral("session-1"), snapshotIdentity,
                                               watermark, QJsonObject{{QStringLiteral("ordinal"), 1}})
                           .isEmpty()
                    && invalidRequests == 5,
                    "invalid Timeline snapshot requests were not rejected before transport")) {
            return false;
        }
        const QString firstRequest = client.timelineSnapshot(
            QStringLiteral("session-1"), {}, {}, {}, 200);
        if (!expect(!firstRequest.isEmpty()
                        && waitUntil([&]() { return pages.contains(firstRequest); }),
                    "initial Timeline snapshot page was not signalled")) {
            return false;
        }
        const QJsonObject firstPage = pages.value(firstRequest);
        const QString secondRequest = client.timelineSnapshot(
            QStringLiteral("session-1"),
            firstPage.value(QStringLiteral("snapshot_identity")).toString(),
            firstPage.value(QStringLiteral("watermark")).toObject(),
            firstPage.value(QStringLiteral("next_after")).toObject(), 25);
        if (!expect(!secondRequest.isEmpty() && secondRequest != firstRequest
                        && waitUntil([&]() { return pages.contains(secondRequest); }),
                    "Timeline snapshot continuation page was not signalled")) {
            return false;
        }
        if (!expect(firstPage.value(QStringLiteral("schema_version"))
                            == QStringLiteral("timeline-session-snapshot-page/0.1")
                        && pages.value(secondRequest).value(QStringLiteral("after"))
                            == firstPage.value(QStringLiteral("next_after")),
                    "Timeline snapshot response signal did not preserve the page contract")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }

    QList<QJsonObject> snapshotRequests;
    QJsonObject initializeRequest;
    for (const QJsonObject &message : readLogMessages(logPath)) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("initialize")) initializeRequest = message;
        if (method == QStringLiteral("timeline/snapshot")) snapshotRequests.append(message);
    }
    const QJsonArray declared = initializeRequest.value(QStringLiteral("params")).toObject()
                                    .value(QStringLiteral("capabilities")).toObject()
                                    .value(QStringLiteral("stable")).toArray();
    if (!expect(declared.contains(QStringLiteral("timeline.snapshot.current")),
                "initialize did not declare the Timeline snapshot capability")
        || !expect(snapshotRequests.size() == 2,
                   "invalid Timeline snapshot requests reached the runtime")) {
        return false;
    }
    const QJsonObject firstParams = snapshotRequests.at(0).value(QStringLiteral("params"))
                                        .toObject();
    const QJsonObject secondParams = snapshotRequests.at(1).value(QStringLiteral("params"))
                                         .toObject();
    return expect(firstParams == QJsonObject{
                      {QStringLiteral("session_id"), QStringLiteral("session-1")},
                      {QStringLiteral("snapshot_identity"), QJsonValue(QJsonValue::Null)},
                      {QStringLiteral("watermark"), QJsonValue(QJsonValue::Null)},
                      {QStringLiteral("after"), QJsonValue(QJsonValue::Null)},
                      {QStringLiteral("limit"), 200},
                  }, "initial Timeline snapshot request did not use null capture fields")
        && expect(secondParams == QJsonObject{
                      {QStringLiteral("session_id"), QStringLiteral("session-1")},
                      {QStringLiteral("snapshot_identity"),
                       QStringLiteral("timeline-session-snapshot:sha256:")
                           + QString(64, QLatin1Char('a'))},
                      {QStringLiteral("watermark"),
                       timelineAnchor(3, timelineEventId(QLatin1Char('b')))},
                      {QStringLiteral("after"), QJsonObject{
                          {QStringLiteral("ordinal"), 1},
                          {QStringLiteral("item_id"), QStringLiteral("item-1")},
                          {QStringLiteral("item_identity"),
                           QStringLiteral("timeline-session-snapshot-item:sha256:")
                               + QString(64, QLatin1Char('c'))},
                      }},
                      {QStringLiteral("limit"), 25},
                  }, "Timeline snapshot continuation changed its contract");
}

bool runTimelineSyncErrorCleanupCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create Timeline error directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("timeline-sync-error-cleanup"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    QString failedRequest;
    QString completedRequest;
    int failedCount = 0;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&failedRequest, &failedCount](const QString &requestId,
                                                       const QString &method,
                                                       const QString &, int code) {
            if (method == QStringLiteral("timeline/sync") && code == -32160) {
                failedRequest = requestId;
                ++failedCount;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::timelineSynced,
                         [&completedRequest](const QString &requestId,
                                             const QJsonObject &) {
            completedRequest = requestId;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "Timeline error cleanup handshake timed out")) {
            return false;
        }
        const QString firstRequest = client.syncTimeline(QStringLiteral("session-1"), 0);
        if (!expect(!firstRequest.isEmpty()
                        && waitUntil([&]() { return failedRequest == firstRequest; })
                        && failedCount == 1,
                    "Timeline error did not clear the exact pending request")) {
            return false;
        }
        const QString secondRequest = client.syncTimeline(QStringLiteral("session-1"), 0);
        if (!expect(!secondRequest.isEmpty() && secondRequest != firstRequest
                        && waitUntil([&]() { return completedRequest == secondRequest; })
                        && client.isReady(),
                    "request tracking did not recover after a Timeline error")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runTimelineRetentionGapContractCase(const QString &testCase,
                                         bool expectAcceptedGap,
                                         bool expectedSnapshotAvailable = true)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create retention-gap directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", testCase.toUtf8());
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool disconnected = false;
    int gapCount = 0;
    QString gapRequestId;
    QJsonObject gapData;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::timelineRetentionGap,
                         [&gapCount, &gapRequestId, &gapData](
                             const QString &requestId, const QJsonObject &data) {
            ++gapCount;
            gapRequestId = requestId;
            gapData = data;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "retention-gap handshake timed out")) {
            return false;
        }
        const QString requestId = client.syncTimeline(QStringLiteral("session-1"), 0);
        if (requestId.isEmpty()) return false;
        if (expectAcceptedGap) {
            if (!expect(waitUntil([&]() { return gapCount == 1; })
                            && gapRequestId == requestId
                            && gapData.value(QStringLiteral("session_id")).toString()
                                == QStringLiteral("session-1")
                            && gapData.value(QStringLiteral("snapshot_available")).toBool()
                                == expectedSnapshotAvailable
                            && client.isReady() && !disconnected,
                        "valid retention-gap response was not request-bound and signalled")) {
                return false;
            }
            client.stop();
            waitUntil([&]() {
                return logContainsMethod(logPath, QStringLiteral("shutdown"));
            });
        } else if (!expect(waitUntil([&]() { return disconnected; })
                               && gapCount == 0 && !client.isReady(),
                           "request-drifted retention-gap response was not rejected")) {
            return false;
        }
    }
    return true;
}

bool runTimelineSyncDisconnectCleanupCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create Timeline disconnect directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("timeline-sync-disconnect"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool firstGenerationDisconnected = false;
    QString firstRequestId;
    QString secondRequestId;
    quint64 firstGeneration = 0;
    quint64 reconnectGeneration = 0;
    int exactFailures = 0;
    QHash<QString, QJsonObject> pages;
    {
        // Hold automatic reconnect behind a test-controlled signal barrier so
        // cleanup can be inspected without depending on CI wall-clock scheduling.
        AgentRuntimeClient client(nullptr, 5000, 15000, {0});
        AgentRuntimeClientSocketTestAccess::holdReconnectTimer(client);
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&client, &initialized, &firstGeneration,
                          &firstGenerationDisconnected](bool ready, const QString &) {
            if (initialized && firstGeneration != 0
                && client.processGeneration() == firstGeneration && !ready) {
                firstGenerationDisconnected = true;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&firstRequestId, &exactFailures](const QString &failedId,
                                                          const QString &method,
                                                          const QString &, int) {
            if (!firstRequestId.isEmpty() && failedId == firstRequestId
                && method == QStringLiteral("timeline/sync")) {
                ++exactFailures;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::reconnectHandshakeReady,
                         [&reconnectGeneration](quint64 generation,
                                                const QJsonObject &) {
            reconnectGeneration = generation;
        });
        QObject::connect(&client, &AgentRuntimeClient::timelineSynced,
                         [&pages](const QString &requestId, const QJsonObject &page) {
            pages.insert(requestId, page);
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "Timeline disconnect handshake timed out")) {
            return false;
        }
        firstGeneration = client.processGeneration();
        firstRequestId = client.syncTimeline(QStringLiteral("session-1"), 0);
        if (!expect(!firstRequestId.isEmpty()
                        && waitUntil([&]() {
                               return firstGenerationDisconnected && exactFailures == 1;
                           })
                        && client.processGeneration() == firstGeneration
                        && client.reconnectState()
                            == AgentRuntimeClient::ReconnectState::Waiting
                        && AgentRuntimeClientSocketTestAccess::retiredRequestWasCleared(
                            client, firstRequestId)
                        && AgentRuntimeClientSocketTestAccess::capabilityWasCleared(
                            client, QStringLiteral("timeline.replay.fixed-watermark"))
                        && !client.isReady(),
                    "disconnect did not fail and clear the exact Timeline request")) {
            return false;
        }
        if (!expect(AgentRuntimeClientSocketTestAccess::beginWaitingReconnectNow(client),
                    "Timeline cleanup fixture could not release its reconnect barrier")) {
            return false;
        }

        if (!expect(waitUntil([&]() { return reconnectGeneration != 0; })
                        && reconnectGeneration > firstGeneration
                        && client.processGeneration() == reconnectGeneration
                        && client.isReconnectRecoveryAvailable()
                        && client.completeReconnectRecovery(
                            reconnectGeneration, true,
                            QStringLiteral("Timeline cleanup fixture recovered"))
                        && client.isReady(),
                    "Timeline cleanup fixture did not negotiate a fresh generation")) {
            return false;
        }
        secondRequestId = client.syncTimeline(QStringLiteral("session-1"), 0);
        if (!expect(!secondRequestId.isEmpty()
                        && secondRequestId != firstRequestId
                        && waitUntil([&]() { return pages.contains(secondRequestId); }),
                    "fresh Timeline generation could not issue a legal sync request")) {
            return false;
        }
        client.stop();
        if (!expect(waitUntil([&]() {
                        return logContainsMethod(logPath, QStringLiteral("shutdown"));
                    }), "Timeline cleanup fixture did not shut down")) {
            return false;
        }
    }

    int initializeCount = 0;
    QHash<int, QList<QJsonObject>> syncRequestsByGeneration;
    for (const QJsonObject &message : readLogMessages(logPath)) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("initialize")) {
            ++initializeCount;
        } else if (method == QStringLiteral("timeline/sync")) {
            syncRequestsByGeneration[initializeCount].append(message);
        }
    }
    return expect(initializeCount == 2,
                  "Timeline cleanup fixture used an unexpected process generation count")
        && expect(syncRequestsByGeneration.value(1).size() == 1
                      && syncRequestsByGeneration.value(1).constFirst()
                             .value(QStringLiteral("id")).toString() == firstRequestId,
                  "retired Timeline generation retained or duplicated its request")
        && expect(syncRequestsByGeneration.value(2).size() == 1
                      && syncRequestsByGeneration.value(2).constFirst()
                             .value(QStringLiteral("id")).toString() == secondRequestId,
                  "fresh Timeline generation did not own exactly its legal request");
}

bool runValidTimelineNotificationCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create Timeline-envelope directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("notification-valid-event-envelope"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool eventReceived = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineEvent,
                         [&eventReceived](const QJsonObject &event) {
            eventReceived = event == validTimelineEnvelope();
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized && eventReceived; }),
                    "valid Timeline envelope was not emitted")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "valid Timeline envelope closed the negotiated transport")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runLargeGenericTimelineNotificationCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create large Timeline directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE",
            QByteArray("notification-valid-large-generic-event"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool eventReceived = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineEvent,
                         [&eventReceived](const QJsonObject &event) {
            eventReceived = event == validLargeGenericTimelineEnvelope();
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized && eventReceived; }),
                    "large generic Timeline envelope was not emitted")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "legal large Timeline envelope closed the transport")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runMathematicalIntegerTimelineNotificationCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create integer Timeline directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE",
            QByteArray("notification-valid-mathematical-integers"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool eventReceived = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineEvent,
                         [&eventReceived](const QJsonObject &event) {
            eventReceived = event == validMathematicalIntegerTimelineEnvelope();
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized && eventReceived; }),
                    "mathematical integer Timeline envelope was not emitted")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "mathematical integer normalization closed the transport")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runBoundaryTimelineNotificationCase()
{
    const QByteArray expectedFrame = exactBoundaryTimelineNotification();
    if (!expect(expectedFrame.size() == kTestMaximumFrameBytes,
                "could not construct an exact 4 MiB Timeline frame")) {
        return false;
    }
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create boundary Timeline directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE",
            QByteArray("notification-valid-boundary-event"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool eventReceived = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::timelineEvent,
                         [&eventReceived](const QJsonObject &event) {
            const QJsonObject data = event.value(QStringLiteral("item")).toObject()
                .value(QStringLiteral("data")).toObject();
            eventReceived = data.value(QStringLiteral("payload")).toString()
                                .toUtf8().size() > 3 * 1024 * 1024
                && event.value(QStringLiteral("event_id")).toString()
                    == AgentRuntimeClient::timelineEventIdentity(event);
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized && eventReceived; }, 10000),
                    "exact 4 MiB Timeline envelope was not emitted")) {
            return false;
        }
        if (!expect(client.isReady() && !disconnected,
                    "exact 4 MiB Timeline envelope closed the transport")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return true;
}

bool runOutboundFrameLimitCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create outbound-frame directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("valid-outbound-frame"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());

    bool initialized = false;
    bool frameRejected = false;
    bool healthReadAfterRejection = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::requestFailed,
                         [&frameRejected](const QString &, const QString &method,
                                          const QString &, int code) {
            if (method == QStringLiteral("session/import/preview") && code == -32005) {
                frameRejected = true;
            }
        });
        QObject::connect(&client, &AgentRuntimeClient::runtimeHealthRead,
                         [&frameRejected, &healthReadAfterRejection](const QJsonObject &) {
            if (frameRejected) healthReadAfterRejection = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "outbound-frame handshake timed out")) {
            return false;
        }
        const QJsonObject package{
            {QStringLiteral("padding"), QString(4 * 1024 * 1024, QLatin1Char('x'))},
        };
        if (!expect(client.previewPortableSessionImport(
                        package, QString(), QStringLiteral("reject")).isEmpty()
                        && frameRejected,
                    "oversized outbound frame was not rejected locally")) {
            return false;
        }
        if (!expect(client.isReady(),
                    "local outbound frame rejection disconnected the runtime")) {
            return false;
        }
        if (!expect(!client.runtimeHealth().isEmpty()
                        && waitUntil([&]() { return healthReadAfterRejection; }),
                    "legal request did not complete after outbound frame rejection")) {
            return false;
        }
        client.stop();
        waitUntil([&]() { return logContainsMethod(logPath, QStringLiteral("shutdown")); });
    }
    return expect(!logContainsMethod(logPath, QStringLiteral("session/import/preview")),
                  "oversized outbound frame reached the runtime");
}

bool runCommandArtifactRequestShapeCase(bool continuation)
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(), "could not create command-page request directory")) {
        return false;
    }
    const QString logPath = directory.filePath(QStringLiteral("runtime-input.jsonl"));
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("command-artifact-request-shape"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG", logPath.toUtf8());
    bool initialized = false;
    const QJsonObject cursor{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-reference-cursor/0.1")},
        {QStringLiteral("reference"), QStringLiteral("command-output:sha256:")
            + QString(64, QLatin1Char('a'))},
        {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("bytes"), 7},
        {QStringLiteral("media_type"),
         QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("offset"), 4},
        {QStringLiteral("page_size"), 4},
        {QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("content-inline-limits/0.1")},
            {QStringLiteral("max_item_bytes"), 4},
            {QStringLiteral("max_total_bytes"), 8},
            {QStringLiteral("identity"), QStringLiteral(
                "content-inline-limits:sha256:") + QString(64, QLatin1Char('b'))},
        }},
        {QStringLiteral("binding_identity"), QStringLiteral(
            "content-reference-binding:sha256:") + QString(64, QLatin1Char('c'))},
        {QStringLiteral("identity"), QStringLiteral(
            "content-reference-cursor:sha256:") + QString(64, QLatin1Char('d'))},
    };
    bool pageRead = false;
    bool disconnected = false;
    {
        AgentRuntimeClient client;
        QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                         [&initialized](const QJsonObject &) { initialized = true; });
        QObject::connect(&client, &AgentRuntimeClient::commandArtifactPageRead,
                         [&pageRead](const QString &, const QJsonObject &) {
            pageRead = true;
        });
        QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                         [&initialized, &disconnected](bool ready, const QString &) {
            if (initialized && !ready) disconnected = true;
        });
        client.start();
        if (!expect(waitUntil([&]() { return initialized; }),
                    "command-page request handshake timed out")) {
            return false;
        }
        const QString requestId = client.readCommandArtifactPage(
            QStringLiteral("session-shape"), QStringLiteral("command.\"slash/\\:1"),
            QStringLiteral("command-output:sha256:")
                + QString(64, QLatin1Char('a')),
            continuation ? cursor : QJsonObject{});
        if (!expect(!requestId.isEmpty()
                        && waitUntil([&]() {
                            return logContainsMethod(
                                logPath,
                                QStringLiteral("artifact/read-command-output-page"));
                        }),
                    "command-page request did not reach the fake Runtime")) {
            return false;
        }
        if (!expect(waitUntil([&]() { return disconnected; }) && !pageRead,
                    "malformed command-page response did not close the protocol")) {
            return false;
        }
        client.stop();
    }
    QFile log(logPath);
    if (!expect(log.open(QIODevice::ReadOnly),
                "command-page request log could not be opened")) {
        return false;
    }
    QJsonObject params;
    while (!log.atEnd()) {
        const QJsonDocument document = QJsonDocument::fromJson(log.readLine());
        if (!document.isObject()) continue;
        const QJsonObject message = document.object();
        if (message.value(QStringLiteral("method")).toString()
                == QStringLiteral("artifact/read-command-output-page")) {
            params = message.value(QStringLiteral("params")).toObject();
        }
    }
    const QStringList parameterKeys = params.keys();
    const QSet<QString> keys(parameterKeys.cbegin(), parameterKeys.cend());
    const QSet<QString> expected = continuation
        ? QSet<QString>{QStringLiteral("session_id"), QStringLiteral("item_id"),
                        QStringLiteral("reference"), QStringLiteral("cursor")}
        : QSet<QString>{QStringLiteral("session_id"), QStringLiteral("item_id"),
                        QStringLiteral("reference"), QStringLiteral("limit"),
                        QStringLiteral("max_total_inline_bytes")};
    return expect(keys == expected,
                  "command-page request fields did not match first/continuation contract")
        && expect(params.value(QStringLiteral("item_id")).toString()
                      == QStringLiteral("command.\"slash/\\:1"),
                  "command-page request did not preserve the exact wire Item ID")
        && expect(continuation
                ? params.value(QStringLiteral("cursor")) == QJsonValue(cursor)
                : params.value(QStringLiteral("limit")).toInt() == 64 * 1024
                    && params.value(QStringLiteral("max_total_inline_bytes")).toInt()
                        == 64 * 1024,
                  "command-page request renegotiated or omitted its limits/cursor");
}

bool runCommandArtifactSuccessRoutingCase()
{
    QTemporaryDir directory;
    if (!expect(directory.isValid(),
                "could not create valid command-page runtime directory")) {
        return false;
    }
    putenvUnicode("AEGISY_AGENTD_PATH", QCoreApplication::applicationFilePath());
    qputenv("AEGISY_FAKE_RUNTIME_CASE", QByteArray("command-artifact-valid-response"));
    qputenv("AEGISY_FAKE_RUNTIME_LOG",
            directory.filePath(QStringLiteral("runtime-input.jsonl")).toUtf8());
    bool initialized = false;
    bool disconnected = false;
    QString receivedRequestId;
    QJsonObject received;
    AgentRuntimeClient client;
    QObject::connect(&client, &AgentRuntimeClient::runtimeInitialized,
                     [&initialized](const QJsonObject &) { initialized = true; });
    QObject::connect(&client, &AgentRuntimeClient::commandArtifactPageRead,
                     [&receivedRequestId, &received](const QString &requestId,
                                                     const QJsonObject &result) {
        receivedRequestId = requestId;
        received = result;
    });
    QObject::connect(&client, &AgentRuntimeClient::connectionStateChanged,
                     [&initialized, &disconnected](bool ready, const QString &) {
        if (initialized && !ready) disconnected = true;
    });
    client.start();
    if (!expect(waitUntil([&]() { return initialized; }),
                "valid command-page handshake timed out")) {
        return false;
    }
    const QString requestId = client.readCommandArtifactPage(
        QStringLiteral("session-page"), QStringLiteral("command.route:1"),
        QStringLiteral("command-output:sha256:")
            + QStringLiteral(
                "bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721"));
    const bool routed = expect(!requestId.isEmpty()
                    && waitUntil([&]() { return !receivedRequestId.isEmpty(); }),
                "valid command-page response was not routed")
        && expect(receivedRequestId == requestId,
                  "valid command-page response lost pending request correlation")
        && expect(received.value(QStringLiteral("page")).toObject()
                      .value(QStringLiteral("inline")).toString()
                      == QStringLiteral("abcdef"),
                  "valid command-page response changed its inline bytes")
        && expect(!disconnected,
                  "valid command-page response closed the protocol");
    client.stop();
    return routed;
}

bool workspaceEditProposalPageIdentityMatchesRustFixture()
{
    const QString digest = QStringLiteral(
        "7c4604d03f399eac32a48edbb7be1710838b70c83ad0e94b60137920945d6c40");
    const QJsonObject page{
        {QStringLiteral("schema_version"),
         QStringLiteral("workspace-edit-proposal-artifact-page/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-1")},
        {QStringLiteral("proposal_id"), QStringLiteral("workspace-edit-proposal:sha256:")
            + QString(64, QLatin1Char('a'))},
        {QStringLiteral("project_id"), QStringLiteral("project-1")},
        {QStringLiteral("edit_id"), QStringLiteral("edit-1")},
        {QStringLiteral("artifact"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("diff")},
            {QStringLiteral("reference"),
             QStringLiteral("workspace-edit-diff:sha256:") + digest},
            {QStringLiteral("sha256"), digest},
            {QStringLiteral("bytes"), 5},
            {QStringLiteral("media_type"),
             QStringLiteral("text/x-diff; charset=utf-8")},
        }},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("next_offset"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("total_bytes"), 5},
        {QStringLiteral("data_base64"), QStringLiteral("ZGlmZgo=")},
        {QStringLiteral("chunk_sha256"), digest},
        {QStringLiteral("page_identity"),
         QStringLiteral("workspace-edit-proposal-artifact-page:sha256:d6669806e19a8648ee1bfa52ed6a987ff1cfdd372a777b0050d80d631341125b")},
        {QStringLiteral("file_mutation_authority"), false},
        {QStringLiteral("approval_recorded"), false},
        {QStringLiteral("apply_available"), false},
    };
    QJsonObject drifted = page;
    drifted.insert(QStringLiteral("offset"), 1);
    return expect(AgentRuntimeClient::workspaceEditProposalArtifactPageIdentity(page)
                      == page.value(QStringLiteral("page_identity")).toString(),
                  "Qt Proposal artifact page identity diverged from Rust fixture")
        && expect(AgentRuntimeClient::workspaceEditProposalArtifactPageIdentity(drifted)
                      != page.value(QStringLiteral("page_identity")).toString(),
                  "Proposal page identity omitted offset binding");
}

bool workspaceEditProposalPreviewIdentityMatchesRustFixture()
{
    const QString diffReference = QStringLiteral("workspace-edit-diff:sha256:")
        + QStringLiteral("2ca8ab2ff3700e34b5f1e23baa2c7da4027fe934fdd6a3d2d802ffc745e6ba61");
    const QJsonObject proposal{
        {QStringLiteral("internal_schema_version"),
         QStringLiteral("workspace-edit-proposal/0.1")},
        {QStringLiteral("summary"), QJsonObject{
            {QStringLiteral("file_count"), 1},
            {QStringLiteral("additions"), 1},
            {QStringLiteral("deletions"), 1},
            {QStringLiteral("warning_count"), 0},
            {QStringLiteral("applicable"), true},
            {QStringLiteral("aggregate_diff"), QJsonObject{
                {QStringLiteral("reference"), diffReference},
            }},
            {QStringLiteral("files"), QJsonArray{QJsonObject{
                {QStringLiteral("diff"), QJsonObject{
                    {QStringLiteral("reference"), diffReference},
                }},
            }}},
        }},
    };
    QJsonObject drifted = proposal;
    QJsonObject summary = drifted.value(QStringLiteral("summary")).toObject();
    summary.insert(QStringLiteral("additions"), 2);
    drifted.insert(QStringLiteral("summary"), summary);
    const QString expected = QStringLiteral(
        "workspace-edit-preview:sha256:56faa6585dcdf09eef6cfc2ff54cd2383c3c031ae7f62be47281d3a02a769a5b");
    return expect(AgentRuntimeClient::workspaceEditProposalPreviewIdentity(proposal)
                      == expected,
                  "Qt Proposal preview identity diverged from Rust legacy fixture")
        && expect(AgentRuntimeClient::workspaceEditProposalPreviewIdentity(drifted)
                      != expected,
                  "Proposal preview identity omitted aggregate count binding");
}

QJsonObject commandArtifactBindingFixture(const QString &itemId)
{
    const QString sha256 = QStringLiteral(
        "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03");
    return {
        {QStringLiteral("session_id"), QStringLiteral("session-vector-1")},
        {QStringLiteral("item_id"), itemId},
        {QStringLiteral("created_at_ms"), 1'700'000'000'123.0},
        {QStringLiteral("content_reference"), QJsonObject{
            {QStringLiteral("reference"),
             QStringLiteral("command-output:sha256:") + sha256},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("media_type"),
             QStringLiteral("text/plain; charset=utf-8")},
        }},
        {QStringLiteral("source_bytes"), 6},
        {QStringLiteral("redacted_count"), 0},
        {QStringLiteral("redacted"), false},
        {QStringLiteral("total_bytes"), 6},
        {QStringLiteral("retained_bytes"), 6},
        {QStringLiteral("omitted_bytes"), 0},
        {QStringLiteral("truncated"), false},
    };
}

bool commandArtifactIdentitiesMatchRustFixtures()
{
    const QJsonObject simple = commandArtifactBindingFixture(
        QStringLiteral("command.item:1"));
    const QJsonObject punctuation = commandArtifactBindingFixture(
        QStringLiteral("command.\"slash/\\:1"));
    const QString simpleExpected = QStringLiteral(
        "content-reference-binding:sha256:"
        "d6a8d1c07d2e2a8d0817b50a191e02a0ac677eddacd17bceaeb6c5f67f24216f");
    const QString punctuationExpected = QStringLiteral(
        "content-reference-binding:sha256:"
        "a0b5b2ca29247112b19796e03fafed08972eef39e3f4cd8bb769a7906f8b78a3");

    const QString sha256 = QStringLiteral(
        "bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721");
    QJsonObject limits{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-inline-limits/0.1")},
        {QStringLiteral("max_item_bytes"), 4},
        {QStringLiteral("max_total_bytes"), 8},
        {QStringLiteral("identity"), QStringLiteral(
            "content-inline-limits:sha256:"
            "2093b1974c59acb69989ef3cab60a4b77098bb5d801a891ac90b89b1de97f8fc")},
    };
    QJsonObject cursor{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-reference-cursor/0.1")},
        {QStringLiteral("reference"), QStringLiteral("content:sha256:") + sha256},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), 6},
        {QStringLiteral("media_type"),
         QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("offset"), 4},
        {QStringLiteral("page_size"), 4},
        {QStringLiteral("limits"), limits},
        {QStringLiteral("identity"), QStringLiteral(
            "content-reference-cursor:sha256:"
            "87116127b2d7a8dfdb11b79f20a80cec89360c2ed1d0ede2c810e39cd634f029")},
    };
    QJsonObject page{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-reference-page/0.1")},
        {QStringLiteral("reference"), QStringLiteral("content:sha256:") + sha256},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), 6},
        {QStringLiteral("media_type"),
         QStringLiteral("text/plain; charset=utf-8")},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("page_size"), 4},
        {QStringLiteral("page_bytes"), 4},
        {QStringLiteral("inline"), QStringLiteral("abcd")},
        {QStringLiteral("inline_truncated"), false},
        {QStringLiteral("limits"), limits},
        {QStringLiteral("next_cursor"), cursor},
        {QStringLiteral("identity"), QStringLiteral(
            "content-reference-page:sha256:"
            "5085fcde4966cde1054d75976ff16a37335ed5333e374c1bbfdb4cae9e0e48e1")},
    };
    return expect(AgentRuntimeClient::commandArtifactPageBindingIdentity(simple)
                      == simpleExpected,
                  "Qt command Artifact binding diverged from the Rust fixture")
        && expect(AgentRuntimeClient::commandArtifactPageBindingIdentity(punctuation)
                      == punctuationExpected,
                  "Qt command Artifact binding escaped a legal Item ID differently")
        && expect(AgentRuntimeClient::contentInlineLimitsIdentity(limits)
                      == limits.value(QStringLiteral("identity")).toString(),
                  "Qt inline limits identity diverged from the Rust fixture")
        && expect(AgentRuntimeClient::contentReferenceCursorIdentity(cursor)
                      == cursor.value(QStringLiteral("identity")).toString(),
                  "Qt content cursor identity diverged from the Rust fixture")
        && expect(AgentRuntimeClient::contentReferencePageIdentity(page)
                      == page.value(QStringLiteral("identity")).toString(),
                  "Qt content page identity diverged from the Rust fixture");
}

QJsonObject validCommandArtifactFirstPage(QJsonObject *request,
                                          QJsonObject *continuationRequest = nullptr)
{
    const QByteArray source("abcdef");
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(
        source, QCryptographicHash::Sha256).toHex());
    const QString reference = QStringLiteral("command-output:sha256:") + sha256;
    const QString mediaType = QStringLiteral("text/plain; charset=utf-8");
    QJsonObject limits{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-inline-limits/0.1")},
        {QStringLiteral("max_item_bytes"), 4},
        {QStringLiteral("max_total_bytes"), 8},
    };
    limits.insert(QStringLiteral("identity"),
                  AgentRuntimeClient::contentInlineLimitsIdentity(limits));
    QJsonObject preview{
        {QStringLiteral("schema_version"), QStringLiteral("content-preview/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("content_bytes"), 6},
        {QStringLiteral("preview_bytes"), 6},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("line_count"), 1},
        {QStringLiteral("width"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("height"), QJsonValue(QJsonValue::Null)},
    };
    preview.insert(QStringLiteral("identity"),
                   AgentRuntimeClient::contentPreviewIdentity(preview));
    const QJsonObject content{
        {QStringLiteral("schema_version"), QStringLiteral("content-reference/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), 6},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("preview"), preview},
    };
    QJsonObject result{
        {QStringLiteral("schema_version"),
         QStringLiteral("command-output-artifact-page/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-page")},
        {QStringLiteral("item_id"), QStringLiteral("command.\"slash/\\:1")},
        {QStringLiteral("created_at_ms"), 1'700'000'000'123.0},
        {QStringLiteral("content_reference"), content},
        {QStringLiteral("source_bytes"), 6},
        {QStringLiteral("redacted_count"), 0},
        {QStringLiteral("redacted"), false},
        {QStringLiteral("total_bytes"), 6},
        {QStringLiteral("retained_bytes"), 6},
        {QStringLiteral("omitted_bytes"), 0},
        {QStringLiteral("truncated"), false},
        {QStringLiteral("read_only"), true},
    };
    result.insert(QStringLiteral("binding_identity"),
                  AgentRuntimeClient::commandArtifactPageBindingIdentity(result));
    const QString binding = result.value(QStringLiteral("binding_identity")).toString();
    QJsonObject cursor{
        {QStringLiteral("schema_version"),
         QStringLiteral("content-reference-cursor/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), 6},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("offset"), 4},
        {QStringLiteral("page_size"), 4},
        {QStringLiteral("limits"), limits},
        {QStringLiteral("binding_identity"), binding},
    };
    cursor.insert(QStringLiteral("identity"),
                  AgentRuntimeClient::contentReferenceCursorIdentity(cursor));
    QJsonObject page{
        {QStringLiteral("schema_version"), QStringLiteral("content-reference-page/0.1")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("bytes"), 6},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("offset"), 0},
        {QStringLiteral("page_size"), 4},
        {QStringLiteral("page_bytes"), 4},
        {QStringLiteral("inline"), QStringLiteral("abcd")},
        {QStringLiteral("inline_truncated"), false},
        {QStringLiteral("limits"), limits},
        {QStringLiteral("binding_identity"), binding},
        {QStringLiteral("next_cursor"), cursor},
    };
    page.insert(QStringLiteral("identity"),
                AgentRuntimeClient::contentReferencePageIdentity(page));
    result.insert(QStringLiteral("page"), page);
    if (request) {
        *request = {
            {QStringLiteral("session_id"), QStringLiteral("session-page")},
            {QStringLiteral("item_id"), QStringLiteral("command.\"slash/\\:1")},
            {QStringLiteral("reference"), reference},
            {QStringLiteral("limit"), 4},
            {QStringLiteral("max_total_inline_bytes"), 8},
        };
    }
    if (continuationRequest) {
        *continuationRequest = {
            {QStringLiteral("session_id"), QStringLiteral("session-page")},
            {QStringLiteral("item_id"), QStringLiteral("command.\"slash/\\:1")},
            {QStringLiteral("reference"), reference},
            {QStringLiteral("cursor"), cursor},
        };
    }
    return result;
}

bool commandArtifactPageValidationIsStrict()
{
    QJsonObject firstRequest;
    QJsonObject continuationRequest;
    const QJsonObject first = validCommandArtifactFirstPage(
        &firstRequest, &continuationRequest);
    if (!expect(AgentRuntimeClient::isValidCommandArtifactPage(first, firstRequest),
                "Qt rejected a valid command Artifact first page")) {
        return false;
    }

    QJsonObject second = first;
    QJsonObject secondPage = second.value(QStringLiteral("page")).toObject();
    secondPage.insert(QStringLiteral("offset"), 4);
    secondPage.insert(QStringLiteral("page_bytes"), 2);
    secondPage.insert(QStringLiteral("inline"), QStringLiteral("ef"));
    secondPage.insert(QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null));
    secondPage.insert(QStringLiteral("identity"),
                      AgentRuntimeClient::contentReferencePageIdentity(secondPage));
    second.insert(QStringLiteral("page"), secondPage);
    if (!expect(AgentRuntimeClient::isValidCommandArtifactPage(
                    second, continuationRequest),
                "Qt rejected a valid command Artifact continuation")) {
        return false;
    }

    const QString emptySha = QStringLiteral(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const QString emptyReference = QStringLiteral("command-output:sha256:") + emptySha;
    QJsonObject empty = first;
    QJsonObject emptyContent = empty.value(QStringLiteral("content_reference")).toObject();
    QJsonObject emptyPreview = emptyContent.value(QStringLiteral("preview")).toObject();
    emptyPreview.insert(QStringLiteral("reference"), emptyReference);
    emptyPreview.insert(QStringLiteral("sha256"), emptySha);
    emptyPreview.insert(QStringLiteral("content_bytes"), 0);
    emptyPreview.insert(QStringLiteral("preview_bytes"), 0);
    emptyPreview.insert(QStringLiteral("line_count"), 0);
    emptyPreview.insert(QStringLiteral("identity"),
                        AgentRuntimeClient::contentPreviewIdentity(emptyPreview));
    emptyContent.insert(QStringLiteral("reference"), emptyReference);
    emptyContent.insert(QStringLiteral("sha256"), emptySha);
    emptyContent.insert(QStringLiteral("bytes"), 0);
    emptyContent.insert(QStringLiteral("preview"), emptyPreview);
    empty.insert(QStringLiteral("content_reference"), emptyContent);
    empty.insert(QStringLiteral("source_bytes"), 0);
    empty.insert(QStringLiteral("total_bytes"), 0);
    empty.insert(QStringLiteral("retained_bytes"), 0);
    empty.insert(QStringLiteral("binding_identity"),
                 AgentRuntimeClient::commandArtifactPageBindingIdentity(empty));
    QJsonObject emptyPage = empty.value(QStringLiteral("page")).toObject();
    emptyPage.insert(QStringLiteral("reference"), emptyReference);
    emptyPage.insert(QStringLiteral("sha256"), emptySha);
    emptyPage.insert(QStringLiteral("bytes"), 0);
    emptyPage.insert(QStringLiteral("page_bytes"), 0);
    emptyPage.insert(QStringLiteral("inline"), QString());
    emptyPage.insert(QStringLiteral("binding_identity"),
                     empty.value(QStringLiteral("binding_identity")));
    emptyPage.insert(QStringLiteral("next_cursor"), QJsonValue(QJsonValue::Null));
    emptyPage.insert(QStringLiteral("identity"),
                     AgentRuntimeClient::contentReferencePageIdentity(emptyPage));
    empty.insert(QStringLiteral("page"), emptyPage);
    QJsonObject emptyRequest = firstRequest;
    emptyRequest.insert(QStringLiteral("reference"), emptyReference);
    if (!expect(AgentRuntimeClient::isValidCommandArtifactPage(empty, emptyRequest),
                "Qt rejected a valid empty command Artifact page")) {
        return false;
    }

    QJsonObject unknown = first;
    unknown.insert(QStringLiteral("future_authority"), true);
    QJsonObject wrongSession = first;
    wrongSession.insert(QStringLiteral("session_id"), QStringLiteral("session-other"));
    QJsonObject wrongItem = first;
    wrongItem.insert(QStringLiteral("item_id"), QStringLiteral("command-other"));
    QJsonObject wrongReference = first;
    QJsonObject wrongReferenceContent = wrongReference.value(
        QStringLiteral("content_reference")).toObject();
    wrongReferenceContent.insert(QStringLiteral("reference"),
        QStringLiteral("command-output:sha256:") + QString(64, QLatin1Char('0')));
    wrongReference.insert(QStringLiteral("content_reference"), wrongReferenceContent);
    QJsonObject wrongBinding = first;
    wrongBinding.insert(QStringLiteral("binding_identity"),
        QStringLiteral("content-reference-binding:sha256:")
            + QString(64, QLatin1Char('0')));
    QJsonObject wrongPreview = first;
    QJsonObject wrongPreviewContent = wrongPreview.value(
        QStringLiteral("content_reference")).toObject();
    QJsonObject wrongPreviewValue = wrongPreviewContent.value(
        QStringLiteral("preview")).toObject();
    wrongPreviewValue.insert(QStringLiteral("identity"),
        QStringLiteral("content-preview:sha256:") + QString(64, QLatin1Char('0')));
    wrongPreviewContent.insert(QStringLiteral("preview"), wrongPreviewValue);
    wrongPreview.insert(QStringLiteral("content_reference"), wrongPreviewContent);
    QJsonObject unknownLimit = first;
    QJsonObject unknownLimitPage = unknownLimit.value(QStringLiteral("page")).toObject();
    QJsonObject unknownLimits = unknownLimitPage.value(QStringLiteral("limits")).toObject();
    unknownLimits.insert(QStringLiteral("future_limit"), 1);
    unknownLimitPage.insert(QStringLiteral("limits"), unknownLimits);
    unknownLimit.insert(QStringLiteral("page"), unknownLimitPage);
    QJsonObject wrongPageIdentity = first;
    QJsonObject wrongIdentityPage = wrongPageIdentity.value(
        QStringLiteral("page")).toObject();
    wrongIdentityPage.insert(QStringLiteral("identity"),
        QStringLiteral("content-reference-page:sha256:")
            + QString(64, QLatin1Char('0')));
    wrongPageIdentity.insert(QStringLiteral("page"), wrongIdentityPage);
    QJsonObject unsafeInteger = first;
    unsafeInteger.insert(QStringLiteral("total_bytes"),
                         9'007'199'254'740'992.0);
    QJsonObject fractionalOffset = first;
    QJsonObject fractionalPage = fractionalOffset.value(
        QStringLiteral("page")).toObject();
    fractionalPage.insert(QStringLiteral("offset"), 0.5);
    fractionalOffset.insert(QStringLiteral("page"), fractionalPage);
    QJsonObject writable = first;
    writable.insert(QStringLiteral("read_only"), false);
    QJsonObject badTerminal = second;
    QJsonObject badTerminalPage = secondPage;
    badTerminalPage.insert(QStringLiteral("page_bytes"), 1);
    badTerminalPage.insert(QStringLiteral("inline"), QStringLiteral("e"));
    badTerminalPage.insert(QStringLiteral("identity"),
        AgentRuntimeClient::contentReferencePageIdentity(badTerminalPage));
    badTerminal.insert(QStringLiteral("page"), badTerminalPage);
    QJsonObject renegotiatedRequest = continuationRequest;
    renegotiatedRequest.insert(QStringLiteral("limit"), 4);
    QJsonObject forgedCursorRequest = continuationRequest;
    QJsonObject forgedCursor = forgedCursorRequest.value(
        QStringLiteral("cursor")).toObject();
    forgedCursor.insert(QStringLiteral("identity"),
        QStringLiteral("content-reference-cursor:sha256:")
            + QString(64, QLatin1Char('0')));
    forgedCursorRequest.insert(QStringLiteral("cursor"), forgedCursor);
    return expect(!AgentRuntimeClient::isValidCommandArtifactPage(unknown, firstRequest),
                  "Qt accepted an unknown command Artifact page field")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      wrongSession, firstRequest),
                  "Qt accepted a cross-Session command Artifact page")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(wrongItem, firstRequest),
                  "Qt accepted a cross-Item command Artifact page")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      wrongReference, firstRequest),
                  "Qt accepted a substituted command Artifact reference")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(wrongBinding, firstRequest),
                  "Qt accepted command Artifact binding drift")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      wrongPreview, firstRequest),
                  "Qt accepted command Artifact preview identity drift")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      unknownLimit, firstRequest),
                  "Qt accepted an unknown inline-limit field")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      wrongPageIdentity, firstRequest),
                  "Qt accepted command Artifact page identity drift")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      unsafeInteger, firstRequest),
                  "Qt accepted an unsafe command Artifact integer")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      fractionalOffset, firstRequest),
                  "Qt accepted a fractional command Artifact offset")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      writable, firstRequest),
                  "Qt accepted writable command Artifact authority")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      badTerminal, continuationRequest),
                  "Qt accepted a non-terminal page without a cursor")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      second, renegotiatedRequest),
                  "Qt accepted continuation limit renegotiation")
        && expect(!AgentRuntimeClient::isValidCommandArtifactPage(
                      second, forgedCursorRequest),
                  "Qt accepted a substituted continuation cursor identity");
}

QJsonObject completeCommandArtifactMetadata(const QByteArray &content,
                                            quint64 retainedBytes,
                                            quint64 omittedBytes)
{
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(
        content, QCryptographicHash::Sha256).toHex());
    return {
        {QStringLiteral("content_reference"), QJsonObject{
            {QStringLiteral("reference"),
             QStringLiteral("command-output:sha256:") + sha256},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("bytes"), static_cast<double>(content.size())},
        }},
        {QStringLiteral("total_bytes"),
         static_cast<double>(retainedBytes + omittedBytes)},
        {QStringLiteral("retained_bytes"), static_cast<double>(retainedBytes)},
        {QStringLiteral("omitted_bytes"), static_cast<double>(omittedBytes)},
        {QStringLiteral("truncated"), omittedBytes > 0},
    };
}

bool commandArtifactCompleteContentValidationIsStrict()
{
    constexpr qsizetype headBytes = 1024 * 1024;
    constexpr qsizetype tailBytes = 1024 * 1024;
    constexpr quint64 omittedBytes = 137;
    const QByteArray marker = QStringLiteral(
        "\n[Aegisy omitted %1 command output bytes]\n")
                                  .arg(omittedBytes).toUtf8();
    const QByteArray valid = QByteArray(headBytes, 'h') + marker
        + QByteArray(tailBytes, 't');
    const quint64 retainedBytes = headBytes + tailBytes;
    if (!expect(AgentRuntimeClient::isValidCompleteCommandArtifact(
                    completeCommandArtifactMetadata(
                        valid, retainedBytes, omittedBytes), valid),
                "Qt rejected a canonical truncated command Artifact")) {
        return false;
    }

    QByteArray missing = valid;
    missing.replace(headBytes, marker.size(), QByteArray(marker.size(), 'x'));
    QByteArray duplicate = valid;
    duplicate.replace(0, marker.size(), marker);
    const QByteArray moved = marker + QByteArray(headBytes, 'h')
        + QByteArray(tailBytes, 't');
    return expect(!AgentRuntimeClient::isValidCompleteCommandArtifact(
                      completeCommandArtifactMetadata(
                          missing, retainedBytes, omittedBytes), missing),
                  "Qt accepted a truncated Artifact without its omission marker")
        && expect(!AgentRuntimeClient::isValidCompleteCommandArtifact(
                      completeCommandArtifactMetadata(
                          duplicate, retainedBytes, omittedBytes), duplicate),
                  "Qt accepted duplicate command Artifact omission markers")
        && expect(!AgentRuntimeClient::isValidCompleteCommandArtifact(
                      completeCommandArtifactMetadata(
                          moved, retainedBytes, omittedBytes), moved),
                  "Qt accepted a command Artifact omission marker outside its boundary");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    Q_UNUSED(application);

    const QString fakeRuntimeCase = qEnvironmentVariable("AEGISY_FAKE_RUNTIME_CASE");
    if (!fakeRuntimeCase.isEmpty()) return runFakeRuntime(fakeRuntimeCase);

    QProcessEnvironment input;
    input.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    input.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"),
                QStringLiteral("/tmp/aegisy-workbench"));
    input.insert(QStringLiteral("OPENAI_API_KEY"),
                QStringLiteral("sentinel-openai-api-key"));
    input.insert(QStringLiteral("Aegisy_Auth_Token"),
                QStringLiteral("sentinel-login-jwt"));
    input.insert(QStringLiteral("MY_REFRESH_TOKEN"),
                QStringLiteral("sentinel-refresh-token"));
    input.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"),
                QStringLiteral("sentinel-cloud-secret"));
    input.insert(QStringLiteral("HTTPS_PROXY"),
                QStringLiteral("https://user:password@example.invalid"));
    input.insert(QStringLiteral("MODEL_NAME"), QStringLiteral("aegisy-coding"));

    const QProcessEnvironment sanitized =
        AgentRuntimeClient::sanitizedSidecarEnvironment(input);
    bool ok = expect(sanitized.value(QStringLiteral("PATH")) == QStringLiteral("/usr/bin"),
                           "safe environment values were not preserved")
        && expect(sanitized.value(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"))
                      == QStringLiteral("/tmp/aegisy-workbench"),
                  "workbench data root was removed")
        && expect(!sanitized.contains(QStringLiteral("OPENAI_API_KEY")),
                  "OpenAI API key reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("Aegisy_Auth_Token")),
                  "Aegisy login token reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("MY_REFRESH_TOKEN")),
                  "refresh token reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("AWS_SECRET_ACCESS_KEY")),
                  "cloud secret reached the sidecar environment")
        && expect(!sanitized.contains(QStringLiteral("HTTPS_PROXY")),
                  "authenticated proxy reached the sidecar environment")
        && expect(sanitized.value(QStringLiteral("MODEL_NAME")) == QStringLiteral("aegisy-coding"),
                  "ordinary model setting was removed");
    ok = staleLocalSocketCallbacksAreInert() && ok;
    ok = expect(verifyRustTimelineIdentityFixture(),
                "Qt Timeline Event identity diverged from the Rust fixture") && ok;
    ok = verifyRustTimelineSnapshotIdentityFixture() && ok;
    ok = expect(AgentRuntimeClient::durableMutationOperationIdentity(
                    QStringLiteral("session-1"), QStringLiteral("turn-start"),
                    QStringLiteral("gesture-550e8400"),
                    QStringLiteral(
                        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"))
                    == QStringLiteral(
                        "mutation-operation:sha256:cc814865240ac3d6c8c514b9c0426f1fe5b4b40c00fa918d087e4cce044143a3"),
                "Qt durable mutation operation identity diverged from the AAP contract") && ok;
    ok = expect(AgentRuntimeClient::durableMutationOperationIdentity(
                    QStringLiteral("session-1"), QStringLiteral("turn-start"),
                    QStringLiteral("gesture-550e8400"), QStringLiteral("not-a-fingerprint"))
                    .isEmpty(),
                "Qt accepted an invalid durable mutation fingerprint") && ok;
    ok = workspaceEditProposalPreviewIdentityMatchesRustFixture() && ok;
    ok = workspaceEditProposalPageIdentityMatchesRustFixture() && ok;
    ok = commandArtifactIdentitiesMatchRustFixtures() && ok;
    ok = commandArtifactPageValidationIsStrict() && ok;
    ok = commandArtifactCompleteContentValidationIsStrict() && ok;
    ok = runCommandArtifactRequestShapeCase(false) && ok;
    ok = runCommandArtifactRequestShapeCase(true) && ok;
    ok = runCommandArtifactSuccessRoutingCase() && ok;
    ok = runHandshakeCase(QStringLiteral("valid-preview"), true) && ok;
    ok = runHandshakeCase(QStringLiteral("turn-start-ack"), true) && ok;
    ok = runHandshakeCase(QStringLiteral("valid-codex"), true) && ok;
    ok = runHandshakeCase(QStringLiteral("valid-recovery"), true, true, true) && ok;
    ok = runHandshakeCase(QStringLiteral("valid-unavailable"), true, false, false) && ok;
    ok = runHandshakeCase(QStringLiteral("protocol-mismatch"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("protocol-leading-zero"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("upgrade-direction"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("runtime-mismatch"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("duplicate-capability"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("unknown-capability"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("empty-capability"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("missing-capability"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("experimental-capability"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("wrong-platform"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("unsafe-transport"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("invalid-limit"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("invalid-low-limit"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("backend-contradiction"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("non-jsonrpc-2"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("missing-jsonrpc"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("wrong-id-type"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("wrong-result-type"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("result-and-error"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("method-and-result"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("params-and-result"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("error-response"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("generic-error-response"), false) && ok;
    ok = runHandshakeCase(QStringLiteral("upgrade-client-error"), false, true, false,
                          QStringLiteral("upgrade-client")) && ok;
    ok = runHandshakeCase(QStringLiteral("upgrade-runtime-error"), false, true, false,
                          QStringLiteral("upgrade-runtime")) && ok;
    ok = runHandshakeCase(QStringLiteral("malformed-upgrade-error"), false) && ok;
    ok = runLateInitializeResponseCase() && ok;
    ok = runHeartbeatNormalCase() && ok;
    ok = runHeartbeatTimeoutCase() && ok;
    ok = runHeartbeatLateAndRehandshakeCase() && ok;
    ok = runHeartbeatSubscriptionOwnershipCase(
             QStringLiteral("subscribe"), QStringLiteral("timeline/subscribe")) && ok;
    ok = runHeartbeatSubscriptionOwnershipCase(
             QStringLiteral("sync"),
             QStringLiteral("timeline/subscription-sync")) && ok;
    ok = runHeartbeatSubscriptionOwnershipCase(
             QStringLiteral("snapshot"),
             QStringLiteral("timeline/subscription-snapshot")) && ok;
    ok = runHeartbeatSubscriptionOwnershipCase(
             QStringLiteral("activate"),
             QStringLiteral("timeline/subscription-activate")) && ok;
    ok = runControlledTimelineSubscriptionAbandonCase() && ok;
    ok = runBoundedProcessReconnectCase() && ok;
    ok = runProcessReconnectExhaustionCase() && ok;
    ok = runTransportCorrelationIdCase() && ok;
    ok = runTransportHugeErrorCodeCase() && ok;
    ok = runTransportConcurrentReverseCase() && ok;
    ok = runTransportOldGenerationCase() && ok;
    for (const QString &stage : {
             QStringLiteral("subscribe"), QStringLiteral("sync"),
             QStringLiteral("snapshot"), QStringLiteral("activate")}) {
        for (const QString &variant : {
                 QStringLiteral("valid"), QStringLiteral("stage"),
                 QStringLiteral("identity")}) {
            ok = runTransportSubscriptionErrorCase(stage, variant) && ok;
        }
    }
    ok = runTransportMutationReverseCase() && ok;
    ok = runTransportMutationDriftCase(false) && ok;
    ok = runTransportMutationDriftCase(true) && ok;
    ok = runEmergencyPolicySwitchCase() && ok;
    ok = runCapabilityGateCase() && ok;
    ok = runTimelineSyncContractCase() && ok;
    ok = runTimelineSnapshotContractCase() && ok;
    ok = runTimelineSyncErrorCleanupCase() && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap"), true) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-drift"), false) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-unnegotiated"), false) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-unavailable"), true, false) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-code-integer"), true) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-code-decimal"), true) && ok;
    ok = runTimelineRetentionGapContractCase(
             QStringLiteral("timeline-sync-retention-gap-code-exponent"), true) && ok;
    ok = runTimelineSyncDisconnectCleanupCase() && ok;
    ok = runValidTimelineNotificationCase() && ok;
    ok = runLargeGenericTimelineNotificationCase() && ok;
    ok = runMathematicalIntegerTimelineNotificationCase() && ok;
    ok = runBoundaryTimelineNotificationCase() && ok;
    ok = runCombinedFramesCase() && ok;
    ok = runOutboundFrameLimitCase() && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-wrong-jsonrpc")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-missing-jsonrpc")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-wrong-id-type")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-result-and-error")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-method-and-result")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-nonobject-result")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-invalid-error")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("ordinary-oversized-tail")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("notification-method-result")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("notification-wrong-params")) && ok;
    ok = runOrdinaryEnvelopeCase(QStringLiteral("notification-missing-params")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-event-without-capability")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-invalid-event-envelope")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-invalid-event-item")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-unsafe-event-data-integer")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-float-event-data")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-invalid-event-data-key")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-unknown-event-item")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-event-identity-tamper")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-overlong-event-identity")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-nonascii-event-identity")) && ok;
    ok = runOrdinaryEnvelopeCase(
             QStringLiteral("notification-removed-persistence-terminal")) && ok;
    for (const QString &testCase : {
             QStringLiteral("transport-raw-leading-bom"),
             QStringLiteral("transport-raw-invalid-utf8"),
             QStringLiteral("transport-raw-duplicate-decoded-key"),
             QStringLiteral("transport-raw-lone-surrogate"),
             QStringLiteral("transport-raw-depth-129"),
             QStringLiteral("transport-raw-node-65537"),
             QStringLiteral("transport-unmatched-malformed-typed-error")}) {
        ok = runRawTransportParserRejectionCase(testCase) && ok;
    }
    qunsetenv("AEGISY_AGENTD_PATH");
    qunsetenv("AEGISY_FAKE_RUNTIME_CASE");
    qunsetenv("AEGISY_FAKE_RUNTIME_LOG");
    return ok ? 0 : 1;
}
