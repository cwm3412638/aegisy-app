#include "agent_runtime_client.h"
#include "artifact_manifest.h"
#include "aap_transport_runtime.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#if defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
constexpr int kStartupTimeoutMs = 60000;
constexpr int kMaximumFrameBytes = 4 * 1024 * 1024;
constexpr int kMaximumIdentityBytes = 64;
constexpr int kMaximumCapabilities = 128;
constexpr int kMaximumCapabilityBytes = 128;
constexpr int kMaximumRetiredResponseIds = 1024;
constexpr int kMaximumReconnectAttempts = 3;
constexpr int kMaximumReconnectDelayMs = 30000;
constexpr int kReconnectTerminationGraceMs = 250;
constexpr int kReconnectStabilityWindowMs = 60000;
constexpr int kMaximumTimelineIdentityBytes = 128;
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr int kMaximumTimelineDataDepth = 16;
constexpr qsizetype kMaximumTimelineDataNodes = 4096;
constexpr qsizetype kMaximumTimelineDataObjectProperties = 128;
constexpr qsizetype kMaximumTimelineDataArrayItems = 4096;

namespace transport_generated = aegisy::aap::transport_generated;
namespace transport_runtime = aegisy::aap::transport_runtime;
using TransportJsonObject = transport_generated::TransportJsonValue::Object;

const TransportJsonObject *transportObject(
    const transport_generated::TransportJsonValue &value)
{
    return std::get_if<TransportJsonObject>(&value.value);
}

const transport_generated::TransportJsonValue *transportField(
    const TransportJsonObject &object, const QString &name)
{
    const auto iterator = object.constFind(name);
    return iterator == object.constEnd() ? nullptr : &iterator.value();
}

const QString *transportStringField(const TransportJsonObject &object,
                                    const QString &name)
{
    const auto *value = transportField(object, name);
    return value ? std::get_if<QString>(&value->value) : nullptr;
}

const transport_generated::TransportJsonNumber *transportNumberField(
    const TransportJsonObject &object, const QString &name)
{
    const auto *value = transportField(object, name);
    return value
        ? std::get_if<transport_generated::TransportJsonNumber>(&value->value)
        : nullptr;
}

bool projectTransportValue(const transport_generated::TransportJsonValue &value,
                           QJsonValue *output)
{
    transport_runtime::TransportProjectionError ignored;
    return transport_runtime::projectJsonSafeTransportValue(value, output, &ignored);
}

// Keep the range explicit so a later compatible AAP revision changes one reviewed
// boundary instead of weakening response validation throughout the client.
const QString kMinimumProtocolVersion = QStringLiteral("0.1");
const QString kMaximumProtocolVersion = QStringLiteral("0.1");
const QString kPreferredProtocolVersion = QStringLiteral("0.1");
const QString kRuntimeName = QStringLiteral("aegisy-agentd");
const QString kMinimumRuntimeVersion = QStringLiteral("0.1.0");
const QString kMaximumRuntimeVersion = QStringLiteral("0.1.0");
const QString kPreviewAdapter = QStringLiteral("preview");
const QString kPreviewVersion = QStringLiteral("0.1.0");
const QString kCodexAdapter = QStringLiteral("codex-app-server");
const QString kCodexVersion = QStringLiteral("codex-cli 0.144.5");
const QString kRecoveryAdapter = QStringLiteral("aegisy-workbench-store");
const QString kRecoveryVersion = QStringLiteral("workbench-recovery-diagnostic/0.1");

struct ProtocolVersion
{
    quint64 major = 0;
    quint64 minor = 0;
};

bool parseProtocolVersion(const QJsonValue &value, ProtocolVersion *version)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"));
    const QString text = value.toString();
    if (text.toUtf8().size() > 16) return false;
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return false;
    bool majorOk = false;
    bool minorOk = false;
    const quint64 major = match.captured(1).toULongLong(&majorOk);
    const quint64 minor = match.captured(2).toULongLong(&minorOk);
    if (!majorOk || !minorOk) return false;
    if (version) *version = {major, minor};
    return true;
}

int compareProtocolVersions(const ProtocolVersion &left, const ProtocolVersion &right)
{
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    return 0;
}

QString platformOs()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}

QString platformArchitecture()
{
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("arm64");
#elif defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("x86_64");
#else
    return QStringLiteral("unknown");
#endif
}

QString terminalPlatformCapability()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("terminal.pty.macos.user-initiated");
#elif defined(Q_OS_WIN)
    return QStringLiteral("terminal.conpty.windows.user-initiated");
#else
    return QStringLiteral("__unsupported_terminal_platform__");
#endif
}

QJsonObject localPlatform()
{
    return {
        {QStringLiteral("os"), platformOs()},
        {QStringLiteral("architecture"), platformArchitecture()},
    };
}

QJsonObject stdioTransportSecurity()
{
    return {
        {QStringLiteral("transport"), QStringLiteral("stdio")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), false},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), false},
    };
}

QJsonObject verifiedUnixSocketTransportSecurity()
{
    return {
        {QStringLiteral("transport"), QStringLiteral("unix-domain-socket")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), false},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), true},
    };
}

QJsonObject verifiedWindowsNamedPipeTransportSecurity()
{
    return {
        {QStringLiteral("transport"), QStringLiteral("windows-named-pipe")},
        {QStringLiteral("local"), true},
        {QStringLiteral("authenticated"), false},
        {QStringLiteral("encrypted"), false},
        {QStringLiteral("peer_verified"), true},
    };
}

#if defined(Q_OS_MACOS)
enum class UnixEndpointState {
    NotReady,
    Ready,
    Invalid,
};

struct UnixEndpointIdentity {
    quint64 directoryDevice = 0;
    quint64 directoryInode = 0;
    quint64 socketDevice = 0;
    quint64 socketInode = 0;
};

UnixEndpointState inspectUnixEndpoint(const QString &directoryPath,
                                      const QString &socketPath,
                                      UnixEndpointIdentity *identity = nullptr)
{
    struct stat directoryStatus {};
    const QByteArray directory = QFile::encodeName(directoryPath);
    if (::lstat(directory.constData(), &directoryStatus) != 0) {
        return errno == ENOENT ? UnixEndpointState::NotReady
                              : UnixEndpointState::Invalid;
    }
    if (!S_ISDIR(directoryStatus.st_mode)
        || directoryStatus.st_uid != ::geteuid()
        || (directoryStatus.st_mode & 0777) != 0700) {
        return UnixEndpointState::Invalid;
    }

    struct stat socketStatus {};
    const QByteArray socket = QFile::encodeName(socketPath);
    if (::lstat(socket.constData(), &socketStatus) != 0) {
        return errno == ENOENT ? UnixEndpointState::NotReady
                              : UnixEndpointState::Invalid;
    }
    if (!S_ISSOCK(socketStatus.st_mode)
        || socketStatus.st_uid != ::geteuid()
        || (socketStatus.st_mode & 0777) != 0600) {
        return UnixEndpointState::Invalid;
    }
    if (identity) {
        identity->directoryDevice = quint64(directoryStatus.st_dev);
        identity->directoryInode = quint64(directoryStatus.st_ino);
        identity->socketDevice = quint64(socketStatus.st_dev);
        identity->socketInode = quint64(socketStatus.st_ino);
    }
    return UnixEndpointState::Ready;
}
#endif

bool hasExactKeys(const QJsonObject &object, const QStringList &keys)
{
    QSet<QString> expected(keys.cbegin(), keys.cend());
    QSet<QString> actual;
    const QStringList actualKeys = object.keys();
    for (const QString &key : actualKeys) actual.insert(key);
    return actual == expected;
}

QByteArray unsignedBigEndian(quint64 value)
{
    QByteArray bytes;
    bytes.reserve(8);
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.append(static_cast<char>((value >> shift) & 0xff));
    }
    return bytes;
}

void appendLengthFramed(QByteArray *target, const QByteArray &component)
{
    if (!target) return;
    target->append(unsignedBigEndian(static_cast<quint64>(component.size())));
    target->append(component);
}

QString lengthFramedIdentity(const QByteArray &prefix,
                             const QList<QByteArray> &components)
{
    QByteArray material = prefix;
    material.append('\0');
    for (const QByteArray &component : components) {
        appendLengthFramed(&material, component);
    }
    return QString::fromLatin1(prefix)
        + QString::fromLatin1(QCryptographicHash::hash(
            material, QCryptographicHash::Sha256).toHex());
}

bool readUnsignedSafeJsonInteger(const QJsonValue &value, quint64 *number = nullptr)
{
    if (!value.isDouble()) return false;
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || raw < 0.0 || raw > kMaximumSafeJsonInteger
            || std::floor(raw) != raw) {
        return false;
    }
    if (number) *number = static_cast<quint64>(raw);
    return true;
}

bool isLowerHexSha256(const QString &value)
{
    return value.size() == 64
        && std::all_of(value.cbegin(), value.cend(), [](QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
}

bool isSha256Identity(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix)
        && isLowerHexSha256(value.mid(prefix.size()));
}

bool isUnicodeScalarString(const QString &value)
{
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character.isHighSurrogate()) {
            if (index + 1 >= value.size() || !value.at(index + 1).isLowSurrogate()) {
                return false;
            }
            ++index;
        } else if (character.isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

bool isAsciiGraphicalId(const QJsonValue &value, qsizetype maximumBytes)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > maximumBytes) return false;
    return std::all_of(text.cbegin(), text.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return code >= 0x21 && code <= 0x7e;
    });
}

bool isCommandOutputReference(const QString &reference, const QString &sha256)
{
    return isLowerHexSha256(sha256)
        && reference == QStringLiteral("command-output:sha256:") + sha256;
}

const QStringList &declaredCapabilities()
{
    static const QStringList capabilities = {
        QStringLiteral("artifact.command-output.bounded"),
        QStringLiteral("background-job.recovery.inspect"),
        QStringLiteral("background-notification.outbox.read-only"),
        QStringLiteral("model.capability-check.read-only"),
        QStringLiteral("model.catalog.cache.read-only"),
        QStringLiteral("model.catalog.read-only"),
        QStringLiteral("model.catalog.refresh.status.read-only"),
        QStringLiteral("model.profile.read-only"),
        QStringLiteral("operation.reconciliation"),
        QStringLiteral("operation.reconciliation.probe"),
        QStringLiteral("operation.reconciliation.status"),
        QStringLiteral("permission.read-only"),
        QStringLiteral("project.list"),
        QStringLiteral("project.navigation.persistent"),
        QStringLiteral("project.open"),
        QStringLiteral("project.relink.explicit"),
        QStringLiteral("project.roots.scoped"),
        QStringLiteral("project.trust-acknowledge"),
        QStringLiteral("project.trust-review"),
        QStringLiteral("retention.maintenance.host-triggered"),
        QStringLiteral("retention.policy.manage"),
        QStringLiteral("runtime.codex-app-server"),
        QStringLiteral("runtime.degradations"),
        QStringLiteral("runtime.health"),
        QStringLiteral("runtime.heartbeat.out-of-band"),
        QStringLiteral("runtime.preview"),
        QStringLiteral("runtime.projection-recovery.status"),
        QStringLiteral("runtime.recovery.diagnostic-export"),
        QStringLiteral("runtime.recovery.read-only"),
        QStringLiteral("runtime.recovery.status"),
        QStringLiteral("runtime.restart"),
        QStringLiteral("runtime.unavailable"),
        QStringLiteral("session.chat"),
        QStringLiteral("session.compaction.checkpoint-review"),
        QStringLiteral("session.deletion.two-phase"),
        QStringLiteral("session.deletion.undo"),
        QStringLiteral("session.fork"),
        QStringLiteral("session.history.paginated"),
        QStringLiteral("session.list"),
        QStringLiteral("session.mutation-acknowledgements"),
        QStringLiteral("session.metadata.manage"),
        QStringLiteral("session.portable.export"),
        QStringLiteral("session.portable.import"),
        QStringLiteral("session.provider.lifecycle.archive"),
        QStringLiteral("session.provider.lifecycle.list-read"),
        QStringLiteral("session.provider.lifecycle.unarchive"),
        QStringLiteral("session.recovery.status"),
        QStringLiteral("session.resume"),
        QStringLiteral("session.search.branch"),
        QStringLiteral("session.work.preview"),
        QStringLiteral("session.workspace-binding.read-only"),
        QStringLiteral("terminal.conpty.windows.user-initiated"),
        QStringLiteral("terminal.environment.session-scoped"),
        QStringLiteral("terminal.excerpt.read"),
        QStringLiteral("terminal.lifecycle.named"),
        QStringLiteral("terminal.pty.macos.user-initiated"),
        QStringLiteral("terminal.pty.unsupported"),
        QStringLiteral("terminal.stop.out-of-band"),
        QStringLiteral("timeline.command.structured.read-only"),
        QStringLiteral("timeline.replay.fixed-watermark"),
        QStringLiteral("timeline.snapshot.current"),
        QStringLiteral("timeline.subscription.fixed-watermark"),
        QStringLiteral("timeline.streaming"),
        QStringLiteral("turn.cancel.interrupt"),
        QStringLiteral("turn.context.inspect"),
        QStringLiteral("turn.context.manifest"),
        QStringLiteral("turn.context.pinned-selected"),
        QStringLiteral("turn.context.structured"),
        QStringLiteral("turn.steer.same-turn"),
        QStringLiteral("workspace.definition"),
        QStringLiteral("workspace.diagnostics.command-output"),
        QStringLiteral("workspace.diagnostics.language-server"),
        QStringLiteral("workspace.diagnostics.observed"),
        QStringLiteral("workspace.diagnostics.raw-reference"),
        QStringLiteral("workspace.edit.preview.read-only"),
        QStringLiteral("workspace.edit.proposal.read-only"),
        QStringLiteral("workspace.git-context.read-only"),
        QStringLiteral("workspace.git-query.read-only"),
        QStringLiteral("workspace.git-status"),
        QStringLiteral("workspace.image.import-user"),
        QStringLiteral("workspace.image.preview"),
        QStringLiteral("workspace.index.cancel"),
        QStringLiteral("workspace.index.tree-sitter"),
        QStringLiteral("workspace.instructions.discovery"),
        QStringLiteral("workspace.language-servers"),
        QStringLiteral("workspace.list"),
        QStringLiteral("workspace.metadata"),
        QStringLiteral("workspace.pinned-context.manage"),
        QStringLiteral("workspace.pinned-context.store"),
        QStringLiteral("workspace.read-text"),
        QStringLiteral("workspace.references"),
        QStringLiteral("workspace.repository-map.budgeted"),
        QStringLiteral("workspace.save-user-text"),
        QStringLiteral("workspace.search.bounded"),
        QStringLiteral("workspace.search.cancel"),
        QStringLiteral("workspace.watch.poll"),
    };
    return capabilities;
}

bool isBoundedString(const QJsonValue &value, const QString &expected)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    return !text.isEmpty() && text.toUtf8().size() <= kMaximumIdentityBytes
        && text == expected;
}

bool isPositiveSafeJsonInteger(const QJsonValue &value)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= 1.0
        && number <= kMaximumSafeJsonInteger && number == std::floor(number);
}

bool isSafeJsonInteger(const QJsonValue &value)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && number >= -kMaximumSafeJsonInteger
        && number <= kMaximumSafeJsonInteger && number == std::floor(number);
}

bool isValidTimelineEventId(const QString &eventId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^event:sha256:[0-9a-f]{64}$"));
    return pattern.match(eventId).hasMatch();
}

bool isValidTimelineAnchor(const QJsonObject &anchor)
{
    if (!hasExactKeys(anchor, {
            QStringLiteral("sequence"), QStringLiteral("event_id"),
        })) {
        return false;
    }
    const QJsonValue sequenceValue = anchor.value(QStringLiteral("sequence"));
    if (!isSafeJsonInteger(sequenceValue) || sequenceValue.toDouble() < 0.0) return false;
    const bool isZero = sequenceValue.toDouble() == 0.0;
    const QJsonValue eventIdValue = anchor.value(QStringLiteral("event_id"));
    return isZero ? eventIdValue.isNull()
                  : eventIdValue.isString()
                        && isValidTimelineEventId(eventIdValue.toString());
}

bool isValidTimelineDataKey(const QString &key)
{
    if (key.isEmpty() || key.toUtf8().size() > kMaximumTimelineIdentityBytes) {
        return false;
    }
    return std::all_of(key.cbegin(), key.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return code >= 0x21 && code <= 0x7e;
    });
}

bool isValidTimelineDataValue(const QJsonValue &value, int depth,
                              qsizetype *nodes)
{
    if (!nodes || ++(*nodes) > kMaximumTimelineDataNodes
            || depth > kMaximumTimelineDataDepth) {
        return false;
    }
    if (value.isNull() || value.isBool() || value.isString()) return true;
    if (value.isDouble()) return isSafeJsonInteger(value);
    if (value.isArray()) {
        const QJsonArray values = value.toArray();
        if (values.size() > kMaximumTimelineDataArrayItems) return false;
        return std::all_of(values.cbegin(), values.cend(),
                           [depth, nodes](const QJsonValue &child) {
            return isValidTimelineDataValue(child, depth + 1, nodes);
        });
    }
    if (value.isObject()) {
        const QJsonObject values = value.toObject();
        if (values.size() > kMaximumTimelineDataObjectProperties) return false;
        for (auto valueIt = values.constBegin(); valueIt != values.constEnd(); ++valueIt) {
            if (!isValidTimelineDataKey(valueIt.key())
                    || !isValidTimelineDataValue(valueIt.value(), depth + 1, nodes)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool isBoundedTimelineIdentity(const QJsonValue &value)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.toUtf8().size() > kMaximumTimelineIdentityBytes) {
        return false;
    }
    return std::all_of(text.cbegin(), text.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return code >= 0x21 && code <= 0x7e;
    });
}

bool isValidTimelineWindow(const QJsonObject &after, const QJsonObject &watermark)
{
    if (!isValidTimelineAnchor(after) || !isValidTimelineAnchor(watermark)) return false;
    const double afterSequence = after.value(QStringLiteral("sequence")).toDouble();
    const double watermarkSequence = watermark.value(QStringLiteral("sequence")).toDouble();
    return watermarkSequence > afterSequence
        || (watermarkSequence == afterSequence
            && watermark.value(QStringLiteral("event_id"))
                == after.value(QStringLiteral("event_id")));
}

bool isValidTimelineRetentionGapData(const QJsonObject &data,
                                     const QJsonObject &request)
{
    if (!hasExactKeys(data, {
            QStringLiteral("schema_version"), QStringLiteral("reason"),
            QStringLiteral("session_id"), QStringLiteral("requested_after"),
            QStringLiteral("requested_watermark"), QStringLiteral("retained_floor"),
            QStringLiteral("head"), QStringLiteral("snapshot_required"),
            QStringLiteral("snapshot_available"), QStringLiteral("snapshot_capability"),
            QStringLiteral("snapshot_method"), QStringLiteral("event_history_complete"),
            QStringLiteral("replay_from_floor_allowed"),
        })
        || data.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-retention-gap/0.1")
        || data.value(QStringLiteral("reason")).toString()
            != QStringLiteral("requested-anchor-not-retained")
        || !isBoundedTimelineIdentity(data.value(QStringLiteral("session_id")))
        || data.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || !data.value(QStringLiteral("requested_after")).isObject()
        || data.value(QStringLiteral("requested_after"))
            != request.value(QStringLiteral("after"))
        || data.value(QStringLiteral("requested_watermark"))
            != request.value(QStringLiteral("watermark"))
        || !data.value(QStringLiteral("retained_floor")).isObject()
        || !data.value(QStringLiteral("head")).isObject()
        || !data.value(QStringLiteral("snapshot_required")).isBool()
        || !data.value(QStringLiteral("snapshot_required")).toBool()
        || !data.value(QStringLiteral("snapshot_available")).isBool()
        || data.value(QStringLiteral("snapshot_capability")).toString()
            != QStringLiteral("timeline.snapshot.current")
        || data.value(QStringLiteral("snapshot_method")).toString()
            != QStringLiteral("timeline/snapshot")
        || !data.value(QStringLiteral("event_history_complete")).isBool()
        || data.value(QStringLiteral("event_history_complete")).toBool()
        || !data.value(QStringLiteral("replay_from_floor_allowed")).isBool()
        || data.value(QStringLiteral("replay_from_floor_allowed")).toBool()) {
        return false;
    }
    const QJsonObject requestedAfter = data.value(QStringLiteral("requested_after")).toObject();
    const QJsonValue requestedWatermark = data.value(QStringLiteral("requested_watermark"));
    const QJsonObject retainedFloor = data.value(QStringLiteral("retained_floor")).toObject();
    const QJsonObject head = data.value(QStringLiteral("head")).toObject();
    return isValidTimelineAnchor(requestedAfter)
        && (requestedWatermark.isNull()
            || (requestedWatermark.isObject()
                && isValidTimelineWindow(requestedAfter, requestedWatermark.toObject())))
        && isValidTimelineWindow(retainedFloor, head)
        && requestedAfter.value(QStringLiteral("sequence")).toDouble()
            < retainedFloor.value(QStringLiteral("sequence")).toDouble();
}

bool isValidTimelineSnapshotIdentity(const QString &identity)
{
    static const QRegularExpression pattern(
        QStringLiteral("^timeline-session-snapshot:sha256:[0-9a-f]{64}$"));
    return identity.toUtf8().size() <= kMaximumTimelineIdentityBytes
        && pattern.match(identity).hasMatch();
}

bool isValidTimelineSnapshotItemIdentity(const QString &identity)
{
    static const QRegularExpression pattern(
        QStringLiteral("^timeline-session-snapshot-item:sha256:[0-9a-f]{64}$"));
    return identity.toUtf8().size() <= kMaximumTimelineIdentityBytes
        && pattern.match(identity).hasMatch();
}

bool isValidTimelineSnapshotCursor(const QJsonObject &cursor)
{
    if (!hasExactKeys(cursor, {
            QStringLiteral("ordinal"), QStringLiteral("item_id"),
            QStringLiteral("item_identity"),
        })
        || !isPositiveSafeJsonInteger(cursor.value(QStringLiteral("ordinal")))
        || !isBoundedTimelineIdentity(cursor.value(QStringLiteral("item_id")))
        || !isValidTimelineSnapshotItemIdentity(
            cursor.value(QStringLiteral("item_identity")).toString())) {
        return false;
    }
    return cursor.value(QStringLiteral("ordinal")).toDouble()
        <= kMaximumSafeJsonInteger;
}

bool isValidTimelineName(const QJsonValue &value, qsizetype maximumBytes)
{
    if (!value.isString()) return false;
    const QString text = value.toString();
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$"));
    return !text.isEmpty() && text.toUtf8().size() <= maximumBytes
        && pattern.match(text).hasMatch();
}

bool isValidTimelineItem(const QJsonObject &item)
{
    const QSet<QString> requiredKeys{
        QStringLiteral("id"), QStringLiteral("kind"), QStringLiteral("role"),
        QStringLiteral("state"), QStringLiteral("content"),
    };
    QSet<QString> allowedKeys = requiredKeys;
    allowedKeys.insert(QStringLiteral("data"));
    const QStringList keys = item.keys();
    if (keys.size() < requiredKeys.size() || keys.size() > allowedKeys.size()
        || std::any_of(requiredKeys.cbegin(), requiredKeys.cend(),
                       [&item](const QString &key) { return !item.contains(key); })
        || std::any_of(keys.cbegin(), keys.cend(), [&allowedKeys](const QString &key) {
               return !allowedKeys.contains(key);
           })
        || !isBoundedTimelineIdentity(item.value(QStringLiteral("id")))
        || !isValidTimelineName(item.value(QStringLiteral("kind")), 64)
        || !item.value(QStringLiteral("content")).isString()
        || item.value(QStringLiteral("content")).toString().toUcs4().size() > 65536) {
        return false;
    }

    const QString role = item.value(QStringLiteral("role")).toString();
    const QString state = item.value(QStringLiteral("state")).toString();
    if ((role != QStringLiteral("user") && role != QStringLiteral("agent")
         && role != QStringLiteral("system") && role != QStringLiteral("tool"))
        || (state != QStringLiteral("started") && state != QStringLiteral("running")
            && state != QStringLiteral("delta") && state != QStringLiteral("updated")
            && state != QStringLiteral("completed") && state != QStringLiteral("failed")
            && state != QStringLiteral("interrupted")
            && state != QStringLiteral("truncated")
            && state != QStringLiteral("unavailable"))) {
        return false;
    }
    if (item.contains(QStringLiteral("data"))) {
        const QJsonValue data = item.value(QStringLiteral("data"));
        qsizetype nodes = 0;
        if (!data.isObject() || !isValidTimelineDataValue(data, 1, &nodes)) {
            return false;
        }
    }
    return true;
}

QByteArray compactJsonValue(const QJsonValue &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value})
                                 .toJson(QJsonDocument::Compact);
    return array.size() >= 2 ? array.mid(1, array.size() - 2) : QByteArray{};
}

QByteArray canonicalTimelineDataValue(const QJsonValue &value)
{
    if (value.isArray()) {
        QByteArray encoded(1, '[');
        const QJsonArray values = value.toArray();
        for (qsizetype index = 0; index < values.size(); ++index) {
            if (index > 0) encoded += ',';
            const QByteArray child = canonicalTimelineDataValue(values.at(index));
            if (child.isEmpty()) return {};
            encoded += child;
        }
        encoded += ']';
        return encoded;
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end(), [](const QString &left, const QString &right) {
            return left.toUtf8() < right.toUtf8();
        });
        QByteArray encoded(1, '{');
        for (qsizetype index = 0; index < keys.size(); ++index) {
            if (index > 0) encoded += ',';
            encoded += compactJsonValue(QJsonValue(keys.at(index)));
            encoded += ':';
            const QByteArray child = canonicalTimelineDataValue(
                object.value(keys.at(index)));
            if (child.isEmpty()) return {};
            encoded += child;
        }
        encoded += '}';
        return encoded;
    }
    return compactJsonValue(value);
}

QByteArray canonicalTimelineItem(const QJsonValue &value)
{
    if (value.isNull()) return QByteArrayLiteral("null");
    if (!value.isObject()) return {};
    const QJsonObject item = value.toObject();
    QByteArray encoded = QByteArrayLiteral("{\"id\":");
    encoded += compactJsonValue(item.value(QStringLiteral("id")));
    encoded += QByteArrayLiteral(",\"kind\":");
    encoded += compactJsonValue(item.value(QStringLiteral("kind")));
    encoded += QByteArrayLiteral(",\"role\":");
    encoded += compactJsonValue(item.value(QStringLiteral("role")));
    encoded += QByteArrayLiteral(",\"state\":");
    encoded += compactJsonValue(item.value(QStringLiteral("state")));
    encoded += QByteArrayLiteral(",\"content\":");
    encoded += compactJsonValue(item.value(QStringLiteral("content")));
    if (item.contains(QStringLiteral("data"))) {
        encoded += QByteArrayLiteral(",\"data\":");
        const QByteArray data = canonicalTimelineDataValue(
            item.value(QStringLiteral("data")));
        if (data.isEmpty()) return {};
        encoded += data;
    }
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineItemUpdate(const QJsonValue &value)
{
    if (value.isNull()) return QByteArrayLiteral("null");
    if (!value.isObject()) return {};
    const QJsonObject update = value.toObject();
    QByteArray encoded = QByteArrayLiteral("{\"revision\":");
    encoded += compactJsonValue(update.value(QStringLiteral("revision")));
    encoded += QByteArrayLiteral(",\"content_mode\":");
    encoded += compactJsonValue(update.value(QStringLiteral("content_mode")));
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSnapshotAnchor(const QJsonValue &value)
{
    if (!value.isObject() || !isValidTimelineAnchor(value.toObject())) return {};
    const QJsonObject anchor = value.toObject();
    QByteArray encoded = QByteArrayLiteral("{\"sequence\":");
    encoded += compactJsonValue(anchor.value(QStringLiteral("sequence")));
    encoded += QByteArrayLiteral(",\"event_id\":");
    encoded += compactJsonValue(anchor.value(QStringLiteral("event_id")));
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSnapshotCursor(const QJsonValue &value)
{
    if (!value.isObject() || !isValidTimelineSnapshotCursor(value.toObject())) return {};
    const QJsonObject cursor = value.toObject();
    QByteArray encoded = QByteArrayLiteral("{\"ordinal\":");
    encoded += compactJsonValue(cursor.value(QStringLiteral("ordinal")));
    encoded += QByteArrayLiteral(",\"item_id\":");
    encoded += compactJsonValue(cursor.value(QStringLiteral("item_id")));
    encoded += QByteArrayLiteral(",\"item_identity\":");
    encoded += compactJsonValue(cursor.value(QStringLiteral("item_identity")));
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSyncRequest(const QJsonValue &value)
{
    if (!value.isObject()) return {};
    const QJsonObject request = value.toObject();
    if (!hasExactKeys(request, {
            QStringLiteral("session_id"), QStringLiteral("after"),
            QStringLiteral("watermark"), QStringLiteral("limit"),
        })
        || !isBoundedTimelineIdentity(request.value(QStringLiteral("session_id")))
        || !isValidTimelineAnchor(request.value(QStringLiteral("after")).toObject())
        || !isValidTimelineAnchor(request.value(QStringLiteral("watermark")).toObject())
        || !isPositiveSafeJsonInteger(request.value(QStringLiteral("limit")))) {
        return {};
    }
    QByteArray encoded = QByteArrayLiteral("{\"session_id\":");
    encoded += compactJsonValue(request.value(QStringLiteral("session_id")));
    encoded += QByteArrayLiteral(",\"after\":");
    encoded += canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("after")));
    encoded += QByteArrayLiteral(",\"watermark\":");
    encoded += canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("watermark")));
    encoded += QByteArrayLiteral(",\"limit\":");
    encoded += compactJsonValue(request.value(QStringLiteral("limit")));
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSnapshotRequest(const QJsonValue &value)
{
    if (!value.isObject()) return {};
    const QJsonObject request = value.toObject();
    if (!hasExactKeys(request, {
            QStringLiteral("session_id"), QStringLiteral("snapshot_identity"),
            QStringLiteral("watermark"), QStringLiteral("after"),
            QStringLiteral("limit"),
        })
        || !isBoundedTimelineIdentity(request.value(QStringLiteral("session_id")))
        || !isPositiveSafeJsonInteger(request.value(QStringLiteral("limit")))) {
        return {};
    }
    const QJsonValue snapshotIdentity = request.value(QStringLiteral("snapshot_identity"));
    const QJsonValue watermark = request.value(QStringLiteral("watermark"));
    const QJsonValue after = request.value(QStringLiteral("after"));
    const bool firstPage = snapshotIdentity.isNull() && watermark.isNull() && after.isNull();
    const bool continuation = snapshotIdentity.isString()
        && isValidTimelineSnapshotIdentity(snapshotIdentity.toString())
        && watermark.isObject() && isValidTimelineAnchor(watermark.toObject())
        && after.isObject() && isValidTimelineSnapshotCursor(after.toObject());
    if (!firstPage && !continuation) return {};

    QByteArray encoded = QByteArrayLiteral("{\"session_id\":");
    encoded += compactJsonValue(request.value(QStringLiteral("session_id")));
    encoded += QByteArrayLiteral(",\"snapshot_identity\":");
    encoded += compactJsonValue(snapshotIdentity);
    encoded += QByteArrayLiteral(",\"watermark\":");
    encoded += watermark.isNull()
        ? QByteArrayLiteral("null") : canonicalTimelineSnapshotAnchor(watermark);
    encoded += QByteArrayLiteral(",\"after\":");
    encoded += after.isNull()
        ? QByteArrayLiteral("null") : canonicalTimelineSnapshotCursor(after);
    encoded += QByteArrayLiteral(",\"limit\":");
    encoded += compactJsonValue(request.value(QStringLiteral("limit")));
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSubscriptionRequest(const QString &stage,
                                                const QJsonObject &request)
{
    const bool bindingValid = isPositiveSafeJsonInteger(
            request.value(QStringLiteral("connection_generation")))
        && isBoundedTimelineIdentity(request.value(QStringLiteral("session_id")))
        && isBoundedTimelineIdentity(request.value(QStringLiteral("subscription_id")));
    if (!bindingValid) return {};
    QByteArray encoded;
    if (stage == QStringLiteral("subscribe")) {
        if (!hasExactKeys(request, {
                QStringLiteral("schema_version"),
                QStringLiteral("connection_generation"),
                QStringLiteral("session_id"), QStringLiteral("subscription_id"),
                QStringLiteral("cursor"), QStringLiteral("watermark"),
            })
            || request.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("timeline-subscribe-request/0.1")
            || !isValidTimelineAnchor(request.value(QStringLiteral("cursor")).toObject())
            || !(request.value(QStringLiteral("watermark")).isNull()
                 || (request.value(QStringLiteral("watermark")).isObject()
                     && isValidTimelineAnchor(
                         request.value(QStringLiteral("watermark")).toObject())))) {
            return {};
        }
        encoded = QByteArrayLiteral("{\"schema_version\":\"timeline-subscribe-request/0.1\",\"connection_generation\":");
        encoded += compactJsonValue(request.value(QStringLiteral("connection_generation")));
        encoded += QByteArrayLiteral(",\"session_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("session_id")));
        encoded += QByteArrayLiteral(",\"subscription_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("subscription_id")));
        encoded += QByteArrayLiteral(",\"cursor\":");
        encoded += canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("cursor")));
        encoded += QByteArrayLiteral(",\"watermark\":");
        encoded += request.value(QStringLiteral("watermark")).isNull()
            ? QByteArrayLiteral("null")
            : canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("watermark")));
        encoded += '}';
    } else if (stage == QStringLiteral("subscription-sync")
               || stage == QStringLiteral("subscription-snapshot")) {
        const QString expectedSchema = stage == QStringLiteral("subscription-sync")
            ? QStringLiteral("timeline-subscription-sync-request/0.1")
            : QStringLiteral("timeline-subscription-snapshot-request/0.1");
        if (!hasExactKeys(request, {
                QStringLiteral("schema_version"),
                QStringLiteral("connection_generation"),
                QStringLiteral("session_id"), QStringLiteral("subscription_id"),
                QStringLiteral("request"),
            })
            || request.value(QStringLiteral("schema_version")).toString() != expectedSchema) {
            return {};
        }
        const QByteArray nested = stage == QStringLiteral("subscription-sync")
            ? canonicalTimelineSyncRequest(request.value(QStringLiteral("request")))
            : canonicalTimelineSnapshotRequest(request.value(QStringLiteral("request")));
        if (nested.isEmpty()
            || request.value(QStringLiteral("request")).toObject()
                    .value(QStringLiteral("session_id"))
                != request.value(QStringLiteral("session_id"))) {
            return {};
        }
        encoded = QByteArrayLiteral("{\"schema_version\":");
        encoded += compactJsonValue(QJsonValue(expectedSchema));
        encoded += QByteArrayLiteral(",\"connection_generation\":");
        encoded += compactJsonValue(request.value(QStringLiteral("connection_generation")));
        encoded += QByteArrayLiteral(",\"session_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("session_id")));
        encoded += QByteArrayLiteral(",\"subscription_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("subscription_id")));
        encoded += QByteArrayLiteral(",\"request\":");
        encoded += nested;
        encoded += '}';
    } else if (stage == QStringLiteral("activate")) {
        if (!hasExactKeys(request, {
                QStringLiteral("schema_version"),
                QStringLiteral("connection_generation"),
                QStringLiteral("session_id"), QStringLiteral("subscription_id"),
                QStringLiteral("source"), QStringLiteral("cursor"),
                QStringLiteral("watermark"), QStringLiteral("snapshot_identity"),
            })
            || request.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("timeline-subscription-activate-request/0.1")
            || !isValidTimelineAnchor(request.value(QStringLiteral("cursor")).toObject())
            || request.value(QStringLiteral("cursor"))
                != request.value(QStringLiteral("watermark"))) {
            return {};
        }
        const QString source = request.value(QStringLiteral("source")).toString();
        const QJsonValue snapshotIdentity = request.value(QStringLiteral("snapshot_identity"));
        if (!((source == QStringLiteral("sync") && snapshotIdentity.isNull())
              || (source == QStringLiteral("snapshot") && snapshotIdentity.isString()
                  && isValidTimelineSnapshotIdentity(snapshotIdentity.toString())))) {
            return {};
        }
        encoded = QByteArrayLiteral("{\"schema_version\":\"timeline-subscription-activate-request/0.1\",\"connection_generation\":");
        encoded += compactJsonValue(request.value(QStringLiteral("connection_generation")));
        encoded += QByteArrayLiteral(",\"session_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("session_id")));
        encoded += QByteArrayLiteral(",\"subscription_id\":");
        encoded += compactJsonValue(request.value(QStringLiteral("subscription_id")));
        encoded += QByteArrayLiteral(",\"source\":");
        encoded += compactJsonValue(request.value(QStringLiteral("source")));
        encoded += QByteArrayLiteral(",\"cursor\":");
        encoded += canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("cursor")));
        encoded += QByteArrayLiteral(",\"watermark\":");
        encoded += canonicalTimelineSnapshotAnchor(request.value(QStringLiteral("watermark")));
        encoded += QByteArrayLiteral(",\"snapshot_identity\":");
        encoded += compactJsonValue(snapshotIdentity);
        encoded += '}';
    } else {
        return {};
    }
    return encoded;
}

QString domainSeparatedTimelineSubscriptionRequestIdentity(
    const QString &stage, const QJsonObject &request)
{
    const QByteArray material = canonicalTimelineSubscriptionRequest(stage, request);
    const QByteArray stageBytes = stage.toUtf8();
    if (material.isEmpty() || stageBytes.isEmpty()) return {};
    static constexpr char domain[] = "aegisy-timeline-subscription-request/0.1";
    QByteArray input(domain, static_cast<int>(std::strlen(domain)) + 1);
    const auto appendLength = [&input](quint64 value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            input.append(static_cast<char>((value >> shift) & 0xff));
        }
    };
    appendLength(static_cast<quint64>(stageBytes.size()));
    input += stageBytes;
    appendLength(static_cast<quint64>(material.size()));
    input += material;
    return QStringLiteral("timeline-subscription-request:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

QByteArray canonicalTimelineSnapshotActiveTurn(const QJsonValue &value)
{
    if (value.isNull()) return QByteArrayLiteral("null");
    if (!value.isObject()) return {};
    const QJsonObject activeTurn = value.toObject();
    if (!hasExactKeys(activeTurn, {
            QStringLiteral("turn_id"), QStringLiteral("correlation_id"),
            QStringLiteral("state"), QStringLiteral("started_event"),
            QStringLiteral("latest_event"), QStringLiteral("open_item_ids"),
        })
        || !isBoundedTimelineIdentity(activeTurn.value(QStringLiteral("turn_id")))
        || activeTurn.value(QStringLiteral("correlation_id"))
            != activeTurn.value(QStringLiteral("turn_id"))
        || activeTurn.value(QStringLiteral("state")).toString()
            != QStringLiteral("running")) {
        return {};
    }
    const QByteArray started = canonicalTimelineSnapshotAnchor(
        activeTurn.value(QStringLiteral("started_event")));
    const QByteArray latest = canonicalTimelineSnapshotAnchor(
        activeTurn.value(QStringLiteral("latest_event")));
    const QJsonValue openItems = activeTurn.value(QStringLiteral("open_item_ids"));
    if (started.isEmpty() || latest.isEmpty()
        || activeTurn.value(QStringLiteral("started_event")).toObject()
                .value(QStringLiteral("sequence")).toDouble() == 0.0
        || activeTurn.value(QStringLiteral("latest_event")).toObject()
                .value(QStringLiteral("sequence")).toDouble()
            < activeTurn.value(QStringLiteral("started_event")).toObject()
                  .value(QStringLiteral("sequence")).toDouble()
        || !openItems.isArray() || openItems.toArray().size() > 10000) {
        return {};
    }
    QSet<QString> unique;
    for (const QJsonValue &value : openItems.toArray()) {
        if (!isBoundedTimelineIdentity(value) || unique.contains(value.toString())) {
            return {};
        }
        unique.insert(value.toString());
    }
    QByteArray encoded = QByteArrayLiteral("{\"turn_id\":");
    encoded += compactJsonValue(activeTurn.value(QStringLiteral("turn_id")));
    encoded += QByteArrayLiteral(",\"correlation_id\":");
    encoded += compactJsonValue(activeTurn.value(QStringLiteral("correlation_id")));
    encoded += QByteArrayLiteral(",\"state\":");
    encoded += compactJsonValue(activeTurn.value(QStringLiteral("state")));
    encoded += QByteArrayLiteral(",\"started_event\":");
    encoded += started;
    encoded += QByteArrayLiteral(",\"latest_event\":");
    encoded += latest;
    encoded += QByteArrayLiteral(",\"open_item_ids\":");
    encoded += compactJsonValue(openItems);
    encoded += '}';
    return encoded;
}

QByteArray canonicalTimelineSnapshotItemMaterial(const QString &sessionId,
                                                 const QJsonObject &itemPage)
{
    if (!isBoundedTimelineIdentity(sessionId)
        || !hasExactKeys(itemPage, {
            QStringLiteral("ordinal"), QStringLiteral("item_identity"),
            QStringLiteral("turn_id"), QStringLiteral("correlation_id"),
            QStringLiteral("turn_state"), QStringLiteral("first_event"),
            QStringLiteral("latest_event"), QStringLiteral("item"),
            QStringLiteral("item_update"),
        })
        || !isPositiveSafeJsonInteger(itemPage.value(QStringLiteral("ordinal")))
        || !isValidTimelineSnapshotItemIdentity(
            itemPage.value(QStringLiteral("item_identity")).toString())
        || !isBoundedTimelineIdentity(itemPage.value(QStringLiteral("turn_id")))
        || itemPage.value(QStringLiteral("correlation_id"))
            != itemPage.value(QStringLiteral("turn_id"))) {
        return {};
    }
    const QString turnState = itemPage.value(QStringLiteral("turn_state")).toString();
    if (turnState != QStringLiteral("running")
        && turnState != QStringLiteral("completed")
        && turnState != QStringLiteral("failed")
        && turnState != QStringLiteral("interrupted")) {
        return {};
    }
    const QByteArray first = canonicalTimelineSnapshotAnchor(
        itemPage.value(QStringLiteral("first_event")));
    const QByteArray latest = canonicalTimelineSnapshotAnchor(
        itemPage.value(QStringLiteral("latest_event")));
    const QByteArray item = canonicalTimelineItem(itemPage.value(QStringLiteral("item")));
    const QByteArray update = canonicalTimelineItemUpdate(
        itemPage.value(QStringLiteral("item_update")));
    if (first.isEmpty() || latest.isEmpty() || item.isEmpty() || update.isEmpty()
        || itemPage.value(QStringLiteral("first_event")).toObject()
                .value(QStringLiteral("sequence")).toDouble() == 0.0
        || itemPage.value(QStringLiteral("latest_event")).toObject()
                .value(QStringLiteral("sequence")).toDouble()
            < itemPage.value(QStringLiteral("first_event")).toObject()
                  .value(QStringLiteral("sequence")).toDouble()
        || !isPositiveSafeJsonInteger(itemPage.value(QStringLiteral("item_update"))
                                          .toObject()
                                          .value(QStringLiteral("revision")))
        || itemPage.value(QStringLiteral("item_update")).toObject()
                .value(QStringLiteral("content_mode")).toString()
            != QStringLiteral("snapshot-replacement")) {
        return {};
    }

    QByteArray encoded = QByteArrayLiteral(
        "{\"schema_version\":\"timeline-session-snapshot-item/0.1\",\"session_id\":");
    encoded += compactJsonValue(QJsonValue(sessionId));
    encoded += QByteArrayLiteral(",\"ordinal\":");
    encoded += compactJsonValue(itemPage.value(QStringLiteral("ordinal")));
    encoded += QByteArrayLiteral(",\"turn_id\":");
    encoded += compactJsonValue(itemPage.value(QStringLiteral("turn_id")));
    encoded += QByteArrayLiteral(",\"correlation_id\":");
    encoded += compactJsonValue(itemPage.value(QStringLiteral("correlation_id")));
    encoded += QByteArrayLiteral(",\"turn_state\":");
    encoded += compactJsonValue(itemPage.value(QStringLiteral("turn_state")));
    encoded += QByteArrayLiteral(",\"first_event\":");
    encoded += first;
    encoded += QByteArrayLiteral(",\"latest_event\":");
    encoded += latest;
    encoded += QByteArrayLiteral(",\"item\":");
    encoded += item;
    encoded += QByteArrayLiteral(",\"item_update\":");
    encoded += update;
    encoded += '}';
    return encoded;
}

QString domainSeparatedSnapshotIdentity(const char *domain, const char *prefix,
                                        const QByteArray &material)
{
    QByteArray input(domain, static_cast<int>(std::strlen(domain)) + 1);
    const quint64 size = static_cast<quint64>(material.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.append(static_cast<char>((size >> shift) & 0xff));
    }
    input += material;
    return QStringLiteral("%1%2").arg(
        QString::fromLatin1(prefix),
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

bool isValidTimelineEventEnvelope(const QJsonObject &event)
{
    if (!hasExactKeys(event, {
            QStringLiteral("schema_version"), QStringLiteral("event_id"),
            QStringLiteral("sequence"), QStringLiteral("timestamp_ms"),
            QStringLiteral("correlation_id"), QStringLiteral("session_id"),
            QStringLiteral("turn_id"), QStringLiteral("turn_state"),
            QStringLiteral("event"), QStringLiteral("item"),
            QStringLiteral("item_update"),
        })
        || event.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-event/0.1")
        || !isPositiveSafeJsonInteger(event.value(QStringLiteral("sequence")))
        || !isPositiveSafeJsonInteger(event.value(QStringLiteral("timestamp_ms")))
        || !isBoundedTimelineIdentity(event.value(QStringLiteral("session_id")))
        || !isBoundedTimelineIdentity(event.value(QStringLiteral("turn_id")))
        || !isBoundedTimelineIdentity(event.value(QStringLiteral("correlation_id")))) {
        return false;
    }

    if (!event.value(QStringLiteral("event_id")).isString()
        || !isValidTimelineEventId(
                event.value(QStringLiteral("event_id")).toString())
        || !isValidTimelineName(event.value(QStringLiteral("event")), 128)
        || event.value(QStringLiteral("event")).toString()
            == QStringLiteral("turn.persistence-failed")
        || event.value(QStringLiteral("correlation_id"))
            != event.value(QStringLiteral("turn_id"))
        || event.value(QStringLiteral("event_id")).toString()
            != AgentRuntimeClient::timelineEventIdentity(event)) {
        return false;
    }

    const QString turnState = event.value(QStringLiteral("turn_state")).toString();
    if (turnState != QStringLiteral("running")
        && turnState != QStringLiteral("completed")
        && turnState != QStringLiteral("failed")
        && turnState != QStringLiteral("interrupted")) {
        return false;
    }

    const QJsonValue item = event.value(QStringLiteral("item"));
    const QJsonValue update = event.value(QStringLiteral("item_update"));
    if (item.isNull() != update.isNull()) return false;
    const QString eventName = event.value(QStringLiteral("event")).toString();
    const auto matchesItem = [&item](const QString &kind, const QString &role,
                                     const QString &state) {
        if (!item.isObject()) return false;
        const QJsonObject object = item.toObject();
        return (kind.isEmpty() || object.value(QStringLiteral("kind")).toString() == kind)
            && (role.isEmpty() || object.value(QStringLiteral("role")).toString() == role)
            && object.value(QStringLiteral("state")).toString() == state;
    };
    const auto isRunning = [&turnState]() {
        return turnState == QStringLiteral("running");
    };

    if (item.isNull()) {
        if (eventName == QStringLiteral("turn.completed")) {
            return turnState == QStringLiteral("completed");
        }
        if (eventName == QStringLiteral("turn.interrupted")) {
            return turnState == QStringLiteral("interrupted");
        }
        if (eventName == QStringLiteral("turn.failed")) return false;
        static const QSet<QString> itemBearingEvents{
            QStringLiteral("item.started"), QStringLiteral("item.delta"),
            QStringLiteral("item.completed"), QStringLiteral("diagnostics.observed"),
            QStringLiteral("usage.updated"), QStringLiteral("usage.truncated"),
            QStringLiteral("turn.diff.updated"),
            QStringLiteral("turn.diff.truncated"),
            QStringLiteral("turn.plan.updated"),
            QStringLiteral("turn.plan.truncated"),
            QStringLiteral("turn.error-observed"),
            QStringLiteral("turn.steering-requested"),
            QStringLiteral("turn.steering-failed"),
            QStringLiteral("turn.cancellation-failed"),
        };
        return isRunning() && !itemBearingEvents.contains(eventName);
    }
    if (!item.isObject() || !update.isObject()) return false;
    if (!isValidTimelineItem(item.toObject())) return false;
    const QJsonObject itemUpdate = update.toObject();
    if (!hasExactKeys(itemUpdate, {
            QStringLiteral("revision"), QStringLiteral("content_mode"),
        })
        || !isPositiveSafeJsonInteger(itemUpdate.value(QStringLiteral("revision")))
        || itemUpdate.value(QStringLiteral("content_mode")).toString()
            != QStringLiteral("snapshot-replacement")) {
        return false;
    }

    if (!isRunning()) {
        return eventName == QStringLiteral("turn.failed")
            && turnState == QStringLiteral("failed")
            && matchesItem(QStringLiteral("error"), QStringLiteral("system"),
                           QStringLiteral("completed"));
    }
    if (eventName == QStringLiteral("item.started")) {
        return matchesItem(QString(), QString(), QStringLiteral("started"));
    }
    if (eventName == QStringLiteral("item.delta")) {
        return matchesItem(QString(), QString(), QStringLiteral("delta"));
    }
    if (eventName == QStringLiteral("item.completed")) {
        return matchesItem(QString(), QString(), QStringLiteral("completed"));
    }
    if (eventName == QStringLiteral("diagnostics.observed")) {
        return matchesItem(QStringLiteral("diagnostic"), QStringLiteral("tool"),
                           QStringLiteral("completed"));
    }
    if (eventName == QStringLiteral("usage.updated")) {
        return matchesItem(QStringLiteral("usage"), QStringLiteral("system"),
                           QStringLiteral("updated"));
    }
    if (eventName == QStringLiteral("usage.truncated")) {
        return matchesItem(QStringLiteral("usage"), QStringLiteral("system"),
                           QStringLiteral("truncated"));
    }
    if (eventName == QStringLiteral("turn.diff.updated")) {
        return matchesItem(QStringLiteral("diff"), QStringLiteral("tool"),
                           QStringLiteral("updated"));
    }
    if (eventName == QStringLiteral("turn.diff.truncated")) {
        return matchesItem(QStringLiteral("diff"), QStringLiteral("tool"),
                           QStringLiteral("truncated"));
    }
    if (eventName == QStringLiteral("turn.plan.updated")) {
        return matchesItem(QStringLiteral("plan"), QStringLiteral("agent"),
                           QStringLiteral("updated"));
    }
    if (eventName == QStringLiteral("turn.plan.truncated")) {
        return matchesItem(QStringLiteral("plan"), QStringLiteral("agent"),
                           QStringLiteral("truncated"));
    }
    if (eventName == QStringLiteral("turn.error-observed")) {
        return matchesItem(QStringLiteral("error"), QStringLiteral("system"),
                           QStringLiteral("updated"));
    }
    if (eventName == QStringLiteral("turn.steering-requested")) {
        return matchesItem(QStringLiteral("message"), QStringLiteral("user"),
                           QStringLiteral("completed"));
    }
    if (eventName == QStringLiteral("turn.steering-failed")
        || eventName == QStringLiteral("turn.cancellation-failed")) {
        return matchesItem(QStringLiteral("error"), QStringLiteral("system"),
                           QStringLiteral("completed"));
    }
    return false;
}

bool containsCapability(const QSet<QString> &capabilities, const char *capability)
{
    return capabilities.contains(QString::fromLatin1(capability));
}

bool validateCapabilityArray(const QJsonValue &value,
                             const QSet<QString> &declared,
                             bool allowEmpty,
                             QSet<QString> *validated,
                             QString *reasonCode)
{
    if (!value.isArray()) {
        if (reasonCode) *reasonCode = QStringLiteral("capabilities-type");
        return false;
    }
    const QJsonArray array = value.toArray();
    if ((!allowEmpty && array.isEmpty()) || array.size() > kMaximumCapabilities) {
        if (reasonCode) *reasonCode = QStringLiteral("capabilities-count");
        return false;
    }
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9]+)*$"));
    QSet<QString> result;
    for (const QJsonValue &entry : array) {
        if (!entry.isString()) {
            if (reasonCode) *reasonCode = QStringLiteral("capability-type");
            return false;
        }
        const QString capability = entry.toString();
        if (capability.toUtf8().size() > kMaximumCapabilityBytes
            || !pattern.match(capability).hasMatch()) {
            if (reasonCode) *reasonCode = QStringLiteral("capability-format");
            return false;
        }
        if (!declared.contains(capability)) {
            if (reasonCode) *reasonCode = QStringLiteral("capability-not-declared");
            return false;
        }
        if (result.contains(capability)) {
            if (reasonCode) *reasonCode = QStringLiteral("capability-duplicate");
            return false;
        }
        result.insert(capability);
    }
    if (validated) *validated = result;
    return true;
}

bool isValidTimelineSubscribeResult(const QJsonObject &result,
                                    const QJsonObject &request)
{
    if (!hasExactKeys(result, {
            QStringLiteral("schema_version"),
            QStringLiteral("connection_generation"),
            QStringLiteral("session_id"), QStringLiteral("subscription_id"),
            QStringLiteral("state"), QStringLiteral("cursor"),
            QStringLiteral("watermark"), QStringLiteral("next_method"),
        })
        || result.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-subscribe-result/0.1")
        || !isPositiveSafeJsonInteger(
            result.value(QStringLiteral("connection_generation")))
        || result.value(QStringLiteral("connection_generation"))
            != request.value(QStringLiteral("connection_generation"))
        || result.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || result.value(QStringLiteral("subscription_id"))
            != request.value(QStringLiteral("subscription_id"))
        || result.value(QStringLiteral("cursor"))
            != request.value(QStringLiteral("cursor"))
        || !isValidTimelineAnchor(result.value(QStringLiteral("cursor")).toObject())) {
        return false;
    }
    const QJsonValue requestedWatermark = request.value(QStringLiteral("watermark"));
    if (!requestedWatermark.isNull()
        && result.value(QStringLiteral("watermark")) != requestedWatermark) {
        return false;
    }
    const QString state = result.value(QStringLiteral("state")).toString();
    const QJsonValue watermark = result.value(QStringLiteral("watermark"));
    const QString nextMethod = result.value(QStringLiteral("next_method")).toString();
    if (state == QStringLiteral("sync-required")) {
        return watermark.isObject() && isValidTimelineAnchor(watermark.toObject())
            && nextMethod == QStringLiteral("timeline/subscription-sync");
    }
    return state == QStringLiteral("snapshot-required") && watermark.isNull()
        && nextMethod == QStringLiteral("timeline/subscription-snapshot");
}

bool isValidTimelineActivateResult(const QJsonObject &result,
                                   const QJsonObject &request)
{
    return hasExactKeys(result, {
            QStringLiteral("schema_version"),
            QStringLiteral("connection_generation"),
            QStringLiteral("session_id"), QStringLiteral("subscription_id"),
            QStringLiteral("state"), QStringLiteral("cursor"),
            QStringLiteral("watermark"),
        })
        && result.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("timeline-subscription-active/0.1")
        && result.value(QStringLiteral("connection_generation"))
            == request.value(QStringLiteral("connection_generation"))
        && result.value(QStringLiteral("session_id"))
            == request.value(QStringLiteral("session_id"))
        && result.value(QStringLiteral("subscription_id"))
            == request.value(QStringLiteral("subscription_id"))
        && result.value(QStringLiteral("state")).toString() == QStringLiteral("active")
        && result.value(QStringLiteral("cursor"))
            == request.value(QStringLiteral("cursor"))
        && result.value(QStringLiteral("watermark"))
            == request.value(QStringLiteral("watermark"))
        && result.value(QStringLiteral("cursor"))
            == result.value(QStringLiteral("watermark"))
        && isValidTimelineAnchor(result.value(QStringLiteral("cursor")).toObject());
}

bool isValidTimelineSubscriptionEvent(const QJsonObject &wrapper)
{
    if (!hasExactKeys(wrapper, {
            QStringLiteral("schema_version"),
            QStringLiteral("connection_generation"),
            QStringLiteral("session_id"), QStringLiteral("subscription_id"),
            QStringLiteral("state"), QStringLiteral("cursor"),
            QStringLiteral("watermark"), QStringLiteral("event"),
        })
        || wrapper.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-subscription-event/0.1")
        || !isPositiveSafeJsonInteger(
            wrapper.value(QStringLiteral("connection_generation")))
        || !isBoundedTimelineIdentity(wrapper.value(QStringLiteral("session_id")))
        || !isBoundedTimelineIdentity(wrapper.value(QStringLiteral("subscription_id")))
        || wrapper.value(QStringLiteral("state")).toString() != QStringLiteral("active")
        || !isValidTimelineAnchor(wrapper.value(QStringLiteral("cursor")).toObject())
        || !isValidTimelineAnchor(wrapper.value(QStringLiteral("watermark")).toObject())
        || !wrapper.value(QStringLiteral("event")).isObject()) {
        return false;
    }
    const QJsonObject cursor = wrapper.value(QStringLiteral("cursor")).toObject();
    const QJsonObject watermark = wrapper.value(QStringLiteral("watermark")).toObject();
    const QJsonObject event = wrapper.value(QStringLiteral("event")).toObject();
    const double cursorSequence = cursor.value(QStringLiteral("sequence")).toDouble();
    return watermark.value(QStringLiteral("sequence")).toDouble() <= cursorSequence
        && isValidTimelineEventEnvelope(event)
        && event.value(QStringLiteral("session_id"))
            == wrapper.value(QStringLiteral("session_id"))
        && event.value(QStringLiteral("sequence")).toDouble() == cursorSequence + 1.0;
}

bool isValidTimelineSubscriptionFailure(const QJsonObject &failure)
{
    if (!hasExactKeys(failure, {
            QStringLiteral("schema_version"),
            QStringLiteral("connection_generation"),
            QStringLiteral("session_id"), QStringLiteral("subscription_id"),
            QStringLiteral("state"), QStringLiteral("stage"),
            QStringLiteral("cursor"), QStringLiteral("watermark"),
            QStringLiteral("request_identity"), QStringLiteral("reason"),
            QStringLiteral("retryable"), QStringLiteral("cleanup_required"),
        })
        || failure.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-subscription-failure/0.1")
        || !isPositiveSafeJsonInteger(
            failure.value(QStringLiteral("connection_generation")))
        || !isBoundedTimelineIdentity(failure.value(QStringLiteral("session_id")))
        || !isBoundedTimelineIdentity(failure.value(QStringLiteral("subscription_id")))
        || failure.value(QStringLiteral("state")).toString() != QStringLiteral("failed")
        || !isValidTimelineAnchor(failure.value(QStringLiteral("cursor")).toObject())
        || !(failure.value(QStringLiteral("watermark")).isNull()
             || (failure.value(QStringLiteral("watermark")).isObject()
                 && isValidTimelineAnchor(
                    failure.value(QStringLiteral("watermark")).toObject())))
        || !isValidTimelineName(failure.value(QStringLiteral("reason")), 64)
        || !failure.value(QStringLiteral("retryable")).isBool()
        || failure.value(QStringLiteral("cleanup_required")) != QJsonValue(true)) {
        return false;
    }
    static const QSet<QString> requestStages{
        QStringLiteral("subscribe"), QStringLiteral("sync"),
        QStringLiteral("snapshot"), QStringLiteral("activate"),
    };
    const QString stage = failure.value(QStringLiteral("stage")).toString();
    if (stage == QStringLiteral("live")) {
        return failure.value(QStringLiteral("request_identity")).isNull();
    }
    static const QRegularExpression requestIdentityPattern(QStringLiteral(
        "^timeline-subscription-request:sha256:[0-9a-f]{64}$"));
    return requestStages.contains(stage)
        && failure.value(QStringLiteral("request_identity")).isString()
        && requestIdentityPattern.match(
            failure.value(QStringLiteral("request_identity")).toString()).hasMatch();
}

QString timelineSubscriptionIdentityStageForMethod(const QString &method)
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

QString timelineSubscriptionFailureStageForMethod(const QString &method)
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

bool isValidTimelineSubscriptionFailureForRequest(
    const QJsonObject &failure, const QString &method, const QJsonObject &metadata)
{
    const QJsonObject request = metadata.value(QStringLiteral("request")).toObject();
    const QString identityStage = timelineSubscriptionIdentityStageForMethod(method);
    const QString failureStage = timelineSubscriptionFailureStageForMethod(method);
    if (!isValidTimelineSubscriptionFailure(failure)
        || request.isEmpty() || identityStage.isEmpty() || failureStage.isEmpty()
        || failure.value(QStringLiteral("connection_generation"))
            != request.value(QStringLiteral("connection_generation"))
        || failure.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || failure.value(QStringLiteral("subscription_id"))
            != request.value(QStringLiteral("subscription_id"))
        || failure.value(QStringLiteral("stage")).toString() != failureStage
        || failure.value(QStringLiteral("request_identity")).toString()
            != domainSeparatedTimelineSubscriptionRequestIdentity(identityStage, request)) {
        return false;
    }
    if (method == QStringLiteral("timeline/subscribe")) {
        return failure.value(QStringLiteral("cursor"))
                == request.value(QStringLiteral("cursor"))
            && failure.value(QStringLiteral("watermark"))
                == request.value(QStringLiteral("watermark"));
    }
    if (method == QStringLiteral("timeline/subscription-sync")) {
        const QJsonObject nested = request.value(QStringLiteral("request")).toObject();
        return failure.value(QStringLiteral("cursor"))
                == nested.value(QStringLiteral("after"))
            && failure.value(QStringLiteral("watermark"))
                == nested.value(QStringLiteral("watermark"));
    }
    if (method == QStringLiteral("timeline/subscription-snapshot")) {
        const QJsonObject nested = request.value(QStringLiteral("request")).toObject();
        return failure.value(QStringLiteral("cursor"))
                == metadata.value(QStringLiteral("subscription_cursor"))
            && failure.value(QStringLiteral("watermark"))
                == nested.value(QStringLiteral("watermark"));
    }
    return failure.value(QStringLiteral("cursor"))
            == request.value(QStringLiteral("cursor"))
        && failure.value(QStringLiteral("watermark"))
            == request.value(QStringLiteral("watermark"));
}

bool isValidMutationFingerprint(const QJsonValue &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}

bool isValidDurableMutationOperationIdentity(const QJsonValue &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("^mutation-operation:sha256:[0-9a-f]{64}$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}

bool isValidPositiveTimelineAnchor(const QJsonValue &value)
{
    return value.isObject() && isValidTimelineAnchor(value.toObject())
        && value.toObject().value(QStringLiteral("sequence")).toDouble() > 0.0;
}

bool isValidDurableMutationCursor(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject cursor = value.toObject();
    return hasExactKeys(cursor, {
            QStringLiteral("operation_identity"), QStringLiteral("revision"),
        })
        && isValidDurableMutationOperationIdentity(
            cursor.value(QStringLiteral("operation_identity")))
        && isPositiveSafeJsonInteger(cursor.value(QStringLiteral("revision")));
}

bool isValidDurableMutationOperation(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject operation = value.toObject();
    if (!hasExactKeys(operation, {
            QStringLiteral("schema_version"),
            QStringLiteral("operation_identity"),
            QStringLiteral("session_id"), QStringLiteral("mutation_kind"),
            QStringLiteral("idempotency_key"),
            QStringLiteral("request_fingerprint"), QStringLiteral("revision"),
            QStringLiteral("state"), QStringLiteral("turn_id"),
            QStringLiteral("accepted_anchor"), QStringLiteral("terminal_anchor"),
            QStringLiteral("accepted_consumed"),
            QStringLiteral("terminal_consumed"),
        })
        || operation.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("mutation-acknowledgement-operation/0.1")
        || !isValidDurableMutationOperationIdentity(
            operation.value(QStringLiteral("operation_identity")))
        || !isBoundedTimelineIdentity(operation.value(QStringLiteral("session_id")))
        || operation.value(QStringLiteral("mutation_kind")).toString()
            != QStringLiteral("turn-start")
        || !isBoundedTimelineIdentity(
            operation.value(QStringLiteral("idempotency_key")))
        || !isValidMutationFingerprint(
            operation.value(QStringLiteral("request_fingerprint")))
        || !isPositiveSafeJsonInteger(operation.value(QStringLiteral("revision")))
        || !operation.value(QStringLiteral("accepted_consumed")).isBool()
        || !operation.value(QStringLiteral("terminal_consumed")).isBool()) {
        return false;
    }
    const QString expectedIdentity =
        AgentRuntimeClient::durableMutationOperationIdentity(
            operation.value(QStringLiteral("session_id")).toString(),
            operation.value(QStringLiteral("mutation_kind")).toString(),
            operation.value(QStringLiteral("idempotency_key")).toString(),
            operation.value(QStringLiteral("request_fingerprint")).toString());
    if (expectedIdentity.isEmpty()
            || operation.value(QStringLiteral("operation_identity")).toString()
                != expectedIdentity) {
        return false;
    }
    const QJsonValue turnId = operation.value(QStringLiteral("turn_id"));
    const QJsonValue acceptedAnchor = operation.value(QStringLiteral("accepted_anchor"));
    const QJsonValue terminalAnchor = operation.value(QStringLiteral("terminal_anchor"));
    const bool acceptedConsumed = operation.value(
        QStringLiteral("accepted_consumed")).toBool();
    const bool terminalConsumed = operation.value(
        QStringLiteral("terminal_consumed")).toBool();
    if (!(turnId.isNull() || isBoundedTimelineIdentity(turnId))
            || !(acceptedAnchor.isNull()
                 || isValidPositiveTimelineAnchor(acceptedAnchor))
            || !(terminalAnchor.isNull()
                 || isValidPositiveTimelineAnchor(terminalAnchor))
            || (acceptedAnchor.isObject() && turnId.isNull())
            || (acceptedConsumed && acceptedAnchor.isNull())
            || (terminalConsumed && !acceptedConsumed)) {
        return false;
    }
    const QString state = operation.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("accepted")
            || state == QStringLiteral("reconciliation-required")) {
        return terminalAnchor.isNull() && !terminalConsumed;
    }
    if (state != QStringLiteral("terminal") || turnId.isNull()
            || !acceptedAnchor.isObject() || !terminalAnchor.isObject()) {
        return false;
    }
    return terminalAnchor.toObject().value(QStringLiteral("sequence")).toDouble()
        > acceptedAnchor.toObject().value(QStringLiteral("sequence")).toDouble();
}

bool isValidDurableMutationPage(const QJsonObject &page,
                                const QJsonObject &request)
{
    if (!hasExactKeys(page, {
            QStringLiteral("schema_version"), QStringLiteral("session_id"),
            QStringLiteral("after"), QStringLiteral("operations"),
            QStringLiteral("next_after"), QStringLiteral("complete"),
        })
        || page.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("mutation-acknowledgement-page/0.1")
        || page.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || page.value(QStringLiteral("after")) != request.value(QStringLiteral("after"))
        || !page.value(QStringLiteral("operations")).isArray()
        || !page.value(QStringLiteral("complete")).isBool()) {
        return false;
    }
    const QJsonArray operations = page.value(QStringLiteral("operations")).toArray();
    const int requestedLimit = request.value(QStringLiteral("limit")).toInt();
    if (operations.size() > requestedLimit || operations.size() > 100) return false;
    QString previousIdentity;
    const QJsonValue after = page.value(QStringLiteral("after"));
    if (after.isObject()) {
        if (!isValidDurableMutationCursor(after)) return false;
        previousIdentity = after.toObject().value(
            QStringLiteral("operation_identity")).toString();
    } else if (!after.isNull()) {
        return false;
    }
    QJsonObject lastOperation;
    for (const QJsonValue &operationValue : operations) {
        if (!isValidDurableMutationOperation(operationValue)) return false;
        const QJsonObject operation = operationValue.toObject();
        const QString identity = operation.value(
            QStringLiteral("operation_identity")).toString();
        if (operation.value(QStringLiteral("session_id"))
                != request.value(QStringLiteral("session_id"))
                || (!previousIdentity.isEmpty() && identity <= previousIdentity)) {
            return false;
        }
        previousIdentity = identity;
        lastOperation = operation;
    }
    const bool complete = page.value(QStringLiteral("complete")).toBool();
    const QJsonValue nextAfter = page.value(QStringLiteral("next_after"));
    if (complete) return nextAfter.isNull();
    if (lastOperation.isEmpty() || !isValidDurableMutationCursor(nextAfter)) return false;
    const QJsonObject cursor = nextAfter.toObject();
    return cursor.value(QStringLiteral("operation_identity"))
            == lastOperation.value(QStringLiteral("operation_identity"))
        && cursor.value(QStringLiteral("revision"))
            == lastOperation.value(QStringLiteral("revision"));
}

bool isValidDurableMutationConsumeResult(const QJsonObject &result,
                                         const QJsonObject &request)
{
    if (!hasExactKeys(result, {
            QStringLiteral("schema_version"), QStringLiteral("session_id"),
            QStringLiteral("operation_identity"),
            QStringLiteral("expected_revision"), QStringLiteral("target"),
            QStringLiteral("confirmed_anchor"), QStringLiteral("operation"),
        })
        || result.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("mutation-acknowledgement-consume-result/0.1")
        || result.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || result.value(QStringLiteral("operation_identity"))
            != request.value(QStringLiteral("operation_identity"))
        || result.value(QStringLiteral("expected_revision"))
            != request.value(QStringLiteral("expected_revision"))
        || result.value(QStringLiteral("target"))
            != request.value(QStringLiteral("target"))
        || result.value(QStringLiteral("confirmed_anchor"))
            != request.value(QStringLiteral("confirmed_anchor"))
        || !isValidPositiveTimelineAnchor(
            result.value(QStringLiteral("confirmed_anchor")))
        || !isValidDurableMutationOperation(
            result.value(QStringLiteral("operation")))) {
        return false;
    }
    const QJsonObject operation = result.value(QStringLiteral("operation")).toObject();
    const double expectedRevision = result.value(
        QStringLiteral("expected_revision")).toDouble();
    if (operation.value(QStringLiteral("session_id"))
            != result.value(QStringLiteral("session_id"))
            || operation.value(QStringLiteral("operation_identity"))
                != result.value(QStringLiteral("operation_identity"))
            || operation.value(QStringLiteral("revision")).toDouble()
                != expectedRevision + 1.0) {
        return false;
    }
    const QString target = result.value(QStringLiteral("target")).toString();
    if (target == QStringLiteral("accepted")) {
        return operation.value(QStringLiteral("accepted_consumed")).toBool()
            && operation.value(QStringLiteral("accepted_anchor"))
                == result.value(QStringLiteral("confirmed_anchor"));
    }
    return target == QStringLiteral("terminal")
        && operation.value(QStringLiteral("state")).toString()
            == QStringLiteral("terminal")
        && operation.value(QStringLiteral("accepted_consumed")).toBool()
        && operation.value(QStringLiteral("terminal_consumed")).toBool()
        && operation.value(QStringLiteral("terminal_anchor"))
            == result.value(QStringLiteral("confirmed_anchor"));
}

bool isValidTurnStartResult(const QJsonObject &result,
                            const QJsonObject &request)
{
    if (!hasExactKeys(result, {
            QStringLiteral("turn"), QStringLiteral("context"),
            QStringLiteral("request_fingerprint"),
            QStringLiteral("mutation_acknowledgement"),
            QStringLiteral("mutation_operation"),
        })
        || !isValidMutationFingerprint(
            result.value(QStringLiteral("request_fingerprint")))
        || !result.value(QStringLiteral("turn")).isObject()
        || !result.value(QStringLiteral("context")).isObject()
        || !result.value(QStringLiteral("mutation_acknowledgement")).isObject()
        || !isValidDurableMutationOperation(
            result.value(QStringLiteral("mutation_operation")))) {
        return false;
    }
    const QJsonObject turn = result.value(QStringLiteral("turn")).toObject();
    const QJsonObject context = result.value(QStringLiteral("context")).toObject();
    const QJsonObject acknowledgement = result.value(
        QStringLiteral("mutation_acknowledgement")).toObject();
    const QJsonObject operation = result.value(
        QStringLiteral("mutation_operation")).toObject();
    if (!hasExactKeys(turn, {QStringLiteral("id"), QStringLiteral("state")})
        || !isBoundedTimelineIdentity(turn.value(QStringLiteral("id")))
        || turn.value(QStringLiteral("state")).toString() != QStringLiteral("started")
        || !hasExactKeys(context, {
            QStringLiteral("item_count"), QStringLiteral("bytes"),
            QStringLiteral("truncated"), QStringLiteral("manifest"),
            QStringLiteral("budget"),
        })
        || !isSafeJsonInteger(context.value(QStringLiteral("item_count")))
        || context.value(QStringLiteral("item_count")).toDouble() < 0.0
        || !isSafeJsonInteger(context.value(QStringLiteral("bytes")))
        || context.value(QStringLiteral("bytes")).toDouble() < 0.0
        || !context.value(QStringLiteral("truncated")).isBool()
        || !context.value(QStringLiteral("manifest")).isObject()
        || !context.value(QStringLiteral("budget")).isObject()
        || !hasExactKeys(acknowledgement, {
            QStringLiteral("schema_version"), QStringLiteral("request_id"),
            QStringLiteral("idempotency_key"), QStringLiteral("session_id"),
            QStringLiteral("generation"), QStringLiteral("state"),
        })
        || acknowledgement.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("mutation-acknowledgement/0.1")
        || acknowledgement.value(QStringLiteral("request_id"))
            != request.value(QStringLiteral("request_id"))
        || acknowledgement.value(QStringLiteral("idempotency_key"))
            != request.value(QStringLiteral("idempotency_key"))
        || acknowledgement.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || acknowledgement.value(QStringLiteral("generation"))
            != request.value(QStringLiteral("generation"))
        || acknowledgement.value(QStringLiteral("state")).toString()
            != QStringLiteral("accepted")) {
        return false;
    }
    const QString operationState = operation.value(QStringLiteral("state")).toString();
    const bool acceptedState = operationState == QStringLiteral("accepted")
        && operation.value(QStringLiteral("terminal_anchor")).isNull();
    const bool terminalState = operationState == QStringLiteral("terminal")
        && isValidPositiveTimelineAnchor(
            operation.value(QStringLiteral("terminal_anchor")));
    return (acceptedState || terminalState)
        && operation.value(QStringLiteral("session_id"))
            == request.value(QStringLiteral("session_id"))
        && operation.value(QStringLiteral("idempotency_key"))
            == request.value(QStringLiteral("idempotency_key"))
        && operation.value(QStringLiteral("request_fingerprint"))
            == result.value(QStringLiteral("request_fingerprint"))
        && operation.value(QStringLiteral("turn_id"))
            == turn.value(QStringLiteral("id"))
        && isValidPositiveTimelineAnchor(
            operation.value(QStringLiteral("accepted_anchor")))
        && !operation.value(QStringLiteral("accepted_consumed")).toBool()
        && !operation.value(QStringLiteral("terminal_consumed")).toBool();
}

bool validateInitializeResult(const QJsonObject &result,
                              const QJsonObject &expectedSecurity,
                              QSet<QString> *stableCapabilities,
                              int *maximumFrameBytes,
                              QString *reasonCode)
{
    const auto fail = [reasonCode](const char *code) {
        if (reasonCode) *reasonCode = QString::fromLatin1(code);
        return false;
    };
    if (!hasExactKeys(result, {
            QStringLiteral("protocol"),
            QStringLiteral("runtime"),
            QStringLiteral("platform"),
            QStringLiteral("backend"),
            QStringLiteral("capabilities"),
            QStringLiteral("limits"),
            QStringLiteral("transport_security"),
        })) return fail("result-fields");

    const QJsonValue protocolValue = result.value(QStringLiteral("protocol"));
    if (!protocolValue.isObject()) return fail("protocol-type");
    const QJsonObject protocol = protocolValue.toObject();
    if (!hasExactKeys(protocol, {
            QStringLiteral("minimum"), QStringLiteral("maximum"),
            QStringLiteral("selected"), QStringLiteral("upgrade_direction"),
        })) return fail("protocol-fields");
    ProtocolVersion runtimeMinimum;
    ProtocolVersion runtimeMaximum;
    ProtocolVersion selected;
    ProtocolVersion clientMinimum;
    ProtocolVersion clientMaximum;
    if (!parseProtocolVersion(protocol.value(QStringLiteral("minimum")), &runtimeMinimum)
        || !parseProtocolVersion(protocol.value(QStringLiteral("maximum")), &runtimeMaximum)
        || !parseProtocolVersion(protocol.value(QStringLiteral("selected")), &selected)
        || !parseProtocolVersion(QJsonValue(kMinimumProtocolVersion), &clientMinimum)
        || !parseProtocolVersion(QJsonValue(kMaximumProtocolVersion), &clientMaximum)
        || compareProtocolVersions(runtimeMinimum, runtimeMaximum) > 0
        || compareProtocolVersions(selected, runtimeMinimum) < 0
        || compareProtocolVersions(selected, runtimeMaximum) > 0
        || compareProtocolVersions(selected, clientMinimum) < 0
        || compareProtocolVersions(selected, clientMaximum) > 0
        || protocol.value(QStringLiteral("selected")).toString()
            != kPreferredProtocolVersion
        || protocol.value(QStringLiteral("upgrade_direction")).toString()
            != QStringLiteral("none")) {
        return fail("protocol-version");
    }

    const QJsonValue runtimeValue = result.value(QStringLiteral("runtime"));
    if (!runtimeValue.isObject()) return fail("runtime-type");
    const QJsonObject runtime = runtimeValue.toObject();
    if (!hasExactKeys(runtime, {
            QStringLiteral("name"), QStringLiteral("version"),
        })) return fail("runtime-fields");
    if (!isBoundedString(runtime.value(QStringLiteral("name")), kRuntimeName)
        || !isBoundedString(runtime.value(QStringLiteral("version")),
                            kMinimumRuntimeVersion)
        || kMinimumRuntimeVersion != kMaximumRuntimeVersion) {
        return fail("runtime-identity");
    }

    const QJsonValue platformValue = result.value(QStringLiteral("platform"));
    if (!platformValue.isObject()) return fail("platform-type");
    const QJsonObject platform = platformValue.toObject();
    if (!hasExactKeys(platform, {
            QStringLiteral("os"), QStringLiteral("architecture"),
        })
        || !isBoundedString(platform.value(QStringLiteral("os")), platformOs())
        || !isBoundedString(platform.value(QStringLiteral("architecture")),
                            platformArchitecture())) {
        return fail("platform-identity");
    }

    const QJsonValue backendValue = result.value(QStringLiteral("backend"));
    if (!backendValue.isObject()) return fail("backend-type");
    const QJsonObject backend = backendValue.toObject();
    const QJsonValue adapterValue = backend.value(QStringLiteral("adapter"));
    const QJsonValue versionValue = backend.value(QStringLiteral("version"));
    const QJsonValue statusValue = backend.value(QStringLiteral("status"));
    if (!adapterValue.isString() || !versionValue.isString() || !statusValue.isString()
        || !hasExactKeys(backend, {
            QStringLiteral("adapter"), QStringLiteral("version"),
            QStringLiteral("status"),
        })
        || adapterValue.toString().toUtf8().size() > kMaximumIdentityBytes
        || versionValue.toString().toUtf8().size() > kMaximumIdentityBytes
        || statusValue.toString().toUtf8().size() > kMaximumIdentityBytes) {
        return fail("backend-fields");
    }

    const QJsonValue capabilitiesValue = result.value(QStringLiteral("capabilities"));
    if (!capabilitiesValue.isObject()) return fail("capabilities-type");
    const QJsonObject capabilityObject = capabilitiesValue.toObject();
    if (!hasExactKeys(capabilityObject, {
            QStringLiteral("stable"), QStringLiteral("experimental"),
        })) return fail("capabilities-fields");
    QSet<QString> declared;
    for (const QString &capability : declaredCapabilities()) declared.insert(capability);
    QSet<QString> negotiated;
    if (!validateCapabilityArray(capabilityObject.value(QStringLiteral("stable")),
                                 declared, false, &negotiated, reasonCode)) return false;
    const QSet<QString> noExperimentalCapabilities;
    QSet<QString> negotiatedExperimental;
    if (!validateCapabilityArray(
            capabilityObject.value(QStringLiteral("experimental")),
            noExperimentalCapabilities, true, &negotiatedExperimental, reasonCode)
        || !negotiatedExperimental.isEmpty()) return fail("experimental-capabilities");

    const QJsonValue limitsValue = result.value(QStringLiteral("limits"));
    if (!limitsValue.isObject()) return fail("limits-type");
    const QJsonObject limits = limitsValue.toObject();
    const QJsonValue frameLimit = limits.value(QStringLiteral("max_frame_bytes"));
    if (!hasExactKeys(limits, {QStringLiteral("max_frame_bytes")})
        || !frameLimit.isDouble()
        || frameLimit.toDouble() != std::floor(frameLimit.toDouble())
        || frameLimit.toDouble() != kMaximumFrameBytes) {
        return fail("limits-frame");
    }

    const QJsonValue securityValue = result.value(QStringLiteral("transport_security"));
    if (!securityValue.isObject()
        || securityValue.toObject() != expectedSecurity) {
        return fail("transport-security");
    }

    const QString adapter = adapterValue.toString();
    const QString version = versionValue.toString();
    const QString status = statusValue.toString();
    const int backendMarkers = int(containsCapability(negotiated, "runtime.preview"))
        + int(containsCapability(negotiated, "runtime.codex-app-server"))
        + int(containsCapability(negotiated, "runtime.recovery.read-only"))
        + int(containsCapability(negotiated, "runtime.unavailable"));
    if (backendMarkers != 1) return fail("backend-capability-marker");

    if (adapter == kPreviewAdapter) {
        if (version != kPreviewVersion || status != QStringLiteral("ready")
            || !containsCapability(negotiated, "runtime.preview")
            || containsCapability(negotiated, "runtime.restart")
            || containsCapability(negotiated,
                                  "timeline.command.structured.read-only")
            || containsCapability(negotiated, "turn.cancel.interrupt")
            || containsCapability(negotiated, "turn.steer.same-turn")
            || containsCapability(negotiated,
                                  "session.provider.lifecycle.archive")
            || containsCapability(negotiated,
                                  "session.provider.lifecycle.unarchive")
            || containsCapability(negotiated,
                                  "session.provider.lifecycle.list-read")) {
            return fail("backend-preview-combination");
        }
    } else if (adapter == kCodexAdapter) {
        if (version != kCodexVersion) return fail("backend-codex-version");
        if (status == QStringLiteral("ready")) {
            if (!containsCapability(negotiated, "runtime.codex-app-server")) {
                return fail("backend-codex-combination");
            }
        } else if (status == QStringLiteral("unavailable")) {
            const QSet<QString> allowed = {
                QStringLiteral("runtime.unavailable"),
                QStringLiteral("runtime.restart"),
                QStringLiteral("runtime.health"),
                QStringLiteral("runtime.heartbeat.out-of-band"),
                QStringLiteral("runtime.degradations"),
            };
            QSet<QString> unexpected = negotiated;
            unexpected.subtract(allowed);
            if (!containsCapability(negotiated, "runtime.unavailable")
                || !unexpected.isEmpty()) {
                return fail("backend-unavailable-combination");
            }
        } else {
            return fail("backend-codex-status");
        }
    } else if (adapter == kRecoveryAdapter) {
        const QSet<QString> allowed = {
            QStringLiteral("runtime.recovery.read-only"),
            QStringLiteral("runtime.health"),
            QStringLiteral("runtime.heartbeat.out-of-band"),
            QStringLiteral("runtime.degradations"),
            QStringLiteral("model.catalog.read-only"),
            QStringLiteral("model.catalog.refresh.status.read-only"),
            QStringLiteral("model.capability-check.read-only"),
            QStringLiteral("runtime.recovery.status"),
            QStringLiteral("runtime.recovery.diagnostic-export"),
            QStringLiteral("permission.read-only"),
        };
        QSet<QString> unexpected = negotiated;
        unexpected.subtract(allowed);
        if (version != kRecoveryVersion || status != QStringLiteral("read-only-recovery")
            || !containsCapability(negotiated, "runtime.recovery.read-only")
            || !containsCapability(negotiated, "permission.read-only")
            || !unexpected.isEmpty()) {
            return fail("backend-recovery-combination");
        }
    } else {
        return fail("backend-adapter");
    }

    if (status == QStringLiteral("ready")
        && !containsCapability(negotiated, "permission.read-only")) {
        return fail("ready-security-capabilities");
    }
    if (stableCapabilities) *stableCapabilities = negotiated;
    if (maximumFrameBytes) *maximumFrameBytes = int(frameLimit.toDouble());
    return true;
}

bool validateInitializeError(const QJsonObject &error, QString *reasonCode)
{
    const auto fail = [reasonCode](const char *code) {
        if (reasonCode) *reasonCode = QString::fromLatin1(code);
        return false;
    };
    if (error.value(QStringLiteral("code")).toInt() != -32003) {
        if (reasonCode) *reasonCode = QStringLiteral("runtime-rejected");
        return true;
    }
    const QJsonValue dataValue = error.value(QStringLiteral("data"));
    if (!dataValue.isObject()) return fail("initialize-error-data");
    const QJsonObject data = dataValue.toObject();
    if (!hasExactKeys(data, {
            QStringLiteral("schema_version"), QStringLiteral("reason"),
            QStringLiteral("client"), QStringLiteral("runtime"),
            QStringLiteral("upgrade_direction"),
        })
        || data.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("initialize-error/0.1")
        || data.value(QStringLiteral("reason")).toString()
            != QStringLiteral("protocol-range-not-overlapping")) {
        return fail("initialize-error-data");
    }
    const QJsonValue clientValue = data.value(QStringLiteral("client"));
    const QJsonValue runtimeValue = data.value(QStringLiteral("runtime"));
    if (!clientValue.isObject() || !runtimeValue.isObject()) {
        return fail("initialize-error-ranges");
    }
    const QJsonObject client = clientValue.toObject();
    const QJsonObject runtime = runtimeValue.toObject();
    if (!hasExactKeys(client, {
            QStringLiteral("minimum"), QStringLiteral("maximum"),
        })
        || !hasExactKeys(runtime, {
            QStringLiteral("minimum"), QStringLiteral("maximum"),
        })
        || client.value(QStringLiteral("minimum")).toString()
            != kMinimumProtocolVersion
        || client.value(QStringLiteral("maximum")).toString()
            != kMaximumProtocolVersion) {
        return fail("initialize-error-ranges");
    }
    ProtocolVersion clientMinimum;
    ProtocolVersion clientMaximum;
    ProtocolVersion runtimeMinimum;
    ProtocolVersion runtimeMaximum;
    if (!parseProtocolVersion(client.value(QStringLiteral("minimum")), &clientMinimum)
        || !parseProtocolVersion(client.value(QStringLiteral("maximum")), &clientMaximum)
        || !parseProtocolVersion(runtime.value(QStringLiteral("minimum")), &runtimeMinimum)
        || !parseProtocolVersion(runtime.value(QStringLiteral("maximum")), &runtimeMaximum)
        || compareProtocolVersions(clientMinimum, clientMaximum) > 0
        || compareProtocolVersions(runtimeMinimum, runtimeMaximum) > 0) {
        return fail("initialize-error-ranges");
    }
    const QString direction = data.value(QStringLiteral("upgrade_direction")).toString();
    if (direction == QStringLiteral("client")
        && compareProtocolVersions(clientMaximum, runtimeMinimum) < 0) {
        if (reasonCode) *reasonCode = QStringLiteral("upgrade-client");
        return true;
    }
    if (direction == QStringLiteral("runtime")
        && compareProtocolVersions(clientMinimum, runtimeMaximum) > 0) {
        if (reasonCode) *reasonCode = QStringLiteral("upgrade-runtime");
        return true;
    }
    return fail("initialize-error-direction");
}

QStringList requiredCapabilitiesForMethod(const QString &method,
                                          const QJsonObject &params)
{
    static const QHash<QString, QString> capabilities = {
        {QStringLiteral("runtime/health"), QStringLiteral("runtime.health")},
        {QStringLiteral("runtime/heartbeat"),
         QStringLiteral("runtime.heartbeat.out-of-band")},
        {QStringLiteral("runtime/degradations"), QStringLiteral("runtime.degradations")},
        {QStringLiteral("model/catalog"), QStringLiteral("model.catalog.read-only")},
        {QStringLiteral("model/catalog-cache"), QStringLiteral("model.catalog.cache.read-only")},
        {QStringLiteral("model/catalog-refresh-status"), QStringLiteral("model.catalog.refresh.status.read-only")},
        {QStringLiteral("model/capability-check"), QStringLiteral("model.capability-check.read-only")},
        {QStringLiteral("model/profile/list"), QStringLiteral("model.profile.read-only")},
        {QStringLiteral("model/profile/read"), QStringLiteral("model.profile.read-only")},
        {QStringLiteral("runtime/restart"), QStringLiteral("runtime.restart")},
        {QStringLiteral("project/list"), QStringLiteral("project.list")},
        {QStringLiteral("project/navigation"), QStringLiteral("project.navigation.persistent")},
        {QStringLiteral("project/open"), QStringLiteral("project.open")},
        {QStringLiteral("project/relink"), QStringLiteral("project.relink.explicit")},
        {QStringLiteral("project/trust-review"), QStringLiteral("project.trust-review")},
        {QStringLiteral("project/trust-acknowledge"), QStringLiteral("project.trust-acknowledge")},
        {QStringLiteral("project/root-list"), QStringLiteral("project.roots.scoped")},
        {QStringLiteral("project/root-add"), QStringLiteral("project.roots.scoped")},
        {QStringLiteral("project/root-remove"), QStringLiteral("project.roots.scoped")},
        {QStringLiteral("session/resume"), QStringLiteral("session.resume")},
        {QStringLiteral("session/fork"), QStringLiteral("session.fork")},
        {QStringLiteral("session/list"), QStringLiteral("session.list")},
        {QStringLiteral("session/search"), QStringLiteral("session.search.branch")},
        {QStringLiteral("session/mutation-acknowledgements"),
         QStringLiteral("session.mutation-acknowledgements")},
        {QStringLiteral("mutation/acknowledgement/consume"),
         QStringLiteral("session.mutation-acknowledgements")},
        {QStringLiteral("session/title"), QStringLiteral("session.metadata.manage")},
        {QStringLiteral("session/archive"), QStringLiteral("session.metadata.manage")},
        {QStringLiteral("session/unarchive"), QStringLiteral("session.metadata.manage")},
        {QStringLiteral("session/delete/preview"), QStringLiteral("session.deletion.two-phase")},
        {QStringLiteral("session/delete/schedule"), QStringLiteral("session.deletion.two-phase")},
        {QStringLiteral("session/delete/undo"), QStringLiteral("session.deletion.undo")},
        {QStringLiteral("session/deletion/status"), QStringLiteral("session.deletion.two-phase")},
        {QStringLiteral("session/export/preview"), QStringLiteral("session.portable.export")},
        {QStringLiteral("session/export"), QStringLiteral("session.portable.export")},
        {QStringLiteral("session/import/preview"), QStringLiteral("session.portable.import")},
        {QStringLiteral("session/import"), QStringLiteral("session.portable.import")},
        {QStringLiteral("retention/policy/read"), QStringLiteral("retention.policy.manage")},
        {QStringLiteral("retention/policy/set"), QStringLiteral("retention.policy.manage")},
        {QStringLiteral("retention/policy/remove"), QStringLiteral("retention.policy.manage")},
        {QStringLiteral("retention/maintenance/run"), QStringLiteral("retention.maintenance.host-triggered")},
        {QStringLiteral("session/read"), QStringLiteral("session.history.paginated")},
        {QStringLiteral("timeline/sync"), QStringLiteral("timeline.replay.fixed-watermark")},
        {QStringLiteral("timeline/snapshot"), QStringLiteral("timeline.snapshot.current")},
        {QStringLiteral("timeline/subscribe"),
         QStringLiteral("timeline.subscription.fixed-watermark")},
        {QStringLiteral("timeline/subscription-sync"),
         QStringLiteral("timeline.subscription.fixed-watermark")},
        {QStringLiteral("timeline/subscription-snapshot"),
         QStringLiteral("timeline.subscription.fixed-watermark")},
        {QStringLiteral("timeline/subscription-activate"),
         QStringLiteral("timeline.subscription.fixed-watermark")},
        {QStringLiteral("session/background-notifications"), QStringLiteral("background-notification.outbox.read-only")},
        {QStringLiteral("session/background-recovery"), QStringLiteral("background-job.recovery.inspect")},
        {QStringLiteral("runtime/projection-recovery/status"), QStringLiteral("runtime.projection-recovery.status")},
        {QStringLiteral("session/recovery/status"), QStringLiteral("session.recovery.status")},
        {QStringLiteral("operation/status"), QStringLiteral("operation.reconciliation.status")},
        {QStringLiteral("operation/probe"), QStringLiteral("operation.reconciliation.probe")},
        {QStringLiteral("operation/reconcile"), QStringLiteral("operation.reconciliation")},
        {QStringLiteral("session/compaction/checkpoint/create"), QStringLiteral("session.compaction.checkpoint-review")},
        {QStringLiteral("session/compaction/checkpoint/read"), QStringLiteral("session.compaction.checkpoint-review")},
        {QStringLiteral("session/compaction/checkpoint/revise"), QStringLiteral("session.compaction.checkpoint-review")},
        {QStringLiteral("runtime/recovery/status"), QStringLiteral("runtime.recovery.status")},
        {QStringLiteral("turn/cancel"), QStringLiteral("turn.cancel.interrupt")},
        {QStringLiteral("turn/steer"), QStringLiteral("turn.steer.same-turn")},
        {QStringLiteral("turn/context/inspect"), QStringLiteral("turn.context.inspect")},
        {QStringLiteral("workspace/pinned-context/list"), QStringLiteral("workspace.pinned-context.store")},
        {QStringLiteral("workspace/pinned-context/save"), QStringLiteral("workspace.pinned-context.manage")},
        {QStringLiteral("workspace/pinned-context/remove"), QStringLiteral("workspace.pinned-context.manage")},
        {QStringLiteral("workspace/image/import-user"), QStringLiteral("workspace.image.import-user")},
        {QStringLiteral("workspace/image/read"), QStringLiteral("workspace.image.preview")},
        {QStringLiteral("workspace/list"), QStringLiteral("workspace.list")},
        {QStringLiteral("workspace/read"), QStringLiteral("workspace.read-text")},
        {QStringLiteral("workspace/save-user-text"), QStringLiteral("workspace.save-user-text")},
        {QStringLiteral("workspace/metadata"), QStringLiteral("workspace.metadata")},
        {QStringLiteral("workspace/git-status"), QStringLiteral("workspace.git-status")},
        {QStringLiteral("workspace/git/overview"), QStringLiteral("workspace.git-query.read-only")},
        {QStringLiteral("workspace/git/log"), QStringLiteral("workspace.git-query.read-only")},
        {QStringLiteral("workspace/git/commit"), QStringLiteral("workspace.git-query.read-only")},
        {QStringLiteral("workspace/git/diff"), QStringLiteral("workspace.git-query.read-only")},
        {QStringLiteral("workspace/git/context/read"), QStringLiteral("workspace.git-context.read-only")},
        {QStringLiteral("workspace/search"), QStringLiteral("workspace.search.bounded")},
        {QStringLiteral("workspace/search/cancel"), QStringLiteral("workspace.search.cancel")},
        {QStringLiteral("workspace/index"), QStringLiteral("workspace.index.tree-sitter")},
        {QStringLiteral("workspace/index/cancel"), QStringLiteral("workspace.index.cancel")},
        {QStringLiteral("workspace/repository-map"), QStringLiteral("workspace.repository-map.budgeted")},
        {QStringLiteral("workspace/language-servers"), QStringLiteral("workspace.language-servers")},
        {QStringLiteral("workspace/language-server/start"), QStringLiteral("workspace.language-servers")},
        {QStringLiteral("workspace/language-server/stop"), QStringLiteral("workspace.language-servers")},
        {QStringLiteral("workspace/definition"), QStringLiteral("workspace.definition")},
        {QStringLiteral("workspace/references"), QStringLiteral("workspace.references")},
        {QStringLiteral("workspace/diagnostics"), QStringLiteral("workspace.diagnostics.language-server")},
        {QStringLiteral("workspace/observed-diagnostics"), QStringLiteral("workspace.diagnostics.observed")},
        {QStringLiteral("workspace/diagnostics/raw"), QStringLiteral("workspace.diagnostics.raw-reference")},
        {QStringLiteral("workspace/edit/preview"), QStringLiteral("workspace.edit.preview.read-only")},
        {QStringLiteral("workspace/edit/artifact/read"), QStringLiteral("workspace.edit.preview.read-only")},
        {QStringLiteral("workspace/edit/proposal/latest"), QStringLiteral("workspace.edit.proposal.read-only")},
        {QStringLiteral("workspace/edit/proposal/read"), QStringLiteral("workspace.edit.proposal.read-only")},
        {QStringLiteral("workspace/edit/proposal/artifact/read"), QStringLiteral("workspace.edit.proposal.read-only")},
        {QStringLiteral("workspace/watch"), QStringLiteral("workspace.watch.poll")},
        {QStringLiteral("workspace/watch/poll"), QStringLiteral("workspace.watch.poll")},
        {QStringLiteral("terminal/open-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/list"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/attach"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/read"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/input-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/resize"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/signal-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/stop-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/restart-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/remove-user"), QStringLiteral("terminal.lifecycle.named")},
        {QStringLiteral("terminal/excerpt/read"), QStringLiteral("terminal.excerpt.read")},
        {QStringLiteral("artifact/read-command-output"), QStringLiteral("artifact.command-output.bounded")},
        {QStringLiteral("artifact/read-command-output-page"), QStringLiteral("artifact.command-output.bounded")},
    };

    if (method == QStringLiteral("shutdown")) return {};
    if (method == QStringLiteral("session/start")) {
        return {params.value(QStringLiteral("mode")).toString() == QStringLiteral("chat")
                    ? QStringLiteral("session.chat")
                    : QStringLiteral("session.work.preview")};
    }
    QStringList required;
    if (method == QStringLiteral("turn/start")) {
        required.append(QStringLiteral("timeline.streaming"));
        required.append(QStringLiteral("session.mutation-acknowledgements"));
    } else {
        const auto capability = capabilities.constFind(method);
        if (capability == capabilities.cend()) return {QStringLiteral("__unknown_method__")};
        required.append(*capability);
    }
    if ((method == QStringLiteral("turn/start")
         || method == QStringLiteral("turn/context/inspect"))
        && !params.value(QStringLiteral("context")).toArray().isEmpty()) {
        required.append(QStringLiteral("turn.context.structured"));
    }
    if (method.startsWith(QStringLiteral("workspace/edit/proposal/"))) {
        required.append(QStringLiteral("permission.read-only"));
    }
    if ((method == QStringLiteral("turn/start")
         || method == QStringLiteral("turn/context/inspect"))
        && (!params.value(QStringLiteral("pinned_context_ids")).toArray().isEmpty()
            || params.contains(QStringLiteral("pinned_context_set_identity")))) {
        required.append(QStringLiteral("turn.context.pinned-selected"));
    }
    static const QSet<QString> terminalOperations = {
        QStringLiteral("terminal/open-user"),
        QStringLiteral("terminal/input-user"),
        QStringLiteral("terminal/resize"),
        QStringLiteral("terminal/signal-user"),
        QStringLiteral("terminal/stop-user"),
        QStringLiteral("terminal/restart-user"),
        QStringLiteral("terminal/excerpt/read"),
    };
    if (terminalOperations.contains(method)) {
        required.append(terminalPlatformCapability());
    }
    if (method == QStringLiteral("terminal/stop-user")) {
        required.append(QStringLiteral("terminal.stop.out-of-band"));
    }
    return required;
}

bool isLivenessControlMethod(const QString &method)
{
    return method == QStringLiteral("initialize")
        || method == QStringLiteral("shutdown")
        || method == QStringLiteral("runtime/heartbeat")
        || method == QStringLiteral("turn/cancel")
        || method == QStringLiteral("turn/steer")
        || method == QStringLiteral("terminal/stop-user");
}

bool isReconnectRecoveryMethod(const QString &method)
{
    static const QSet<QString> methods{
        QStringLiteral("runtime/heartbeat"),
        QStringLiteral("session/read"),
        QStringLiteral("shutdown"),
        QStringLiteral("terminal/attach"),
        QStringLiteral("terminal/list"),
        QStringLiteral("terminal/read"),
        QStringLiteral("timeline/snapshot"),
        QStringLiteral("timeline/sync"),
        QStringLiteral("timeline/subscribe"),
        QStringLiteral("timeline/subscription-sync"),
        QStringLiteral("timeline/subscription-snapshot"),
        QStringLiteral("timeline/subscription-activate"),
        QStringLiteral("session/mutation-acknowledgements"),
        QStringLiteral("mutation/acknowledgement/consume"),
        QStringLiteral("workspace/edit/proposal/latest"),
        QStringLiteral("workspace/edit/proposal/read"),
    };
    return methods.contains(method);
}

bool isValidHeartbeatResult(const QJsonObject &result, const QString &nonce)
{
    return hasExactKeys(result, {
            QStringLiteral("schema_version"), QStringLiteral("nonce"),
            QStringLiteral("state"),
        })
        && result.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("runtime-heartbeat/0.1")
        && result.value(QStringLiteral("nonce")).toString() == nonce
        && result.value(QStringLiteral("state")).toString()
            == QStringLiteral("alive");
}

bool sensitiveSidecarEnvironmentName(const QString &name)
{
    const QString upper = name.toUpper();
    if (upper == QStringLiteral("HTTP_PROXY")
        || upper == QStringLiteral("HTTPS_PROXY")
        || upper == QStringLiteral("ALL_PROXY")
        || upper == QStringLiteral("SSH_AUTH_SOCK")
        || upper == QStringLiteral("GPG_AGENT_INFO")
        || upper == QStringLiteral("GOOGLE_APPLICATION_CREDENTIALS")) {
        return true;
    }
    QString normalized;
    normalized.reserve(upper.size());
    for (const QChar character : upper) {
        if (character.isLetterOrNumber()) normalized.append(character);
    }
    if (normalized.contains(QStringLiteral("APIKEY"))
        || normalized.contains(QStringLiteral("PRIVATEKEY"))
        || normalized.contains(QStringLiteral("ACCESSKEY"))
        || normalized.contains(QStringLiteral("SECRETKEY"))) {
        return true;
    }
    const QStringList parts = upper.split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")),
                                          Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral("TOKEN")
            || part == QStringLiteral("SECRET")
            || part == QStringLiteral("PASSWORD")
            || part == QStringLiteral("PASSWD")
            || part == QStringLiteral("CREDENTIAL")
            || part == QStringLiteral("CREDENTIALS")
            || part == QStringLiteral("COOKIE")
            || part == QStringLiteral("AUTHORIZATION")
            || part == QStringLiteral("JWT")) {
            return true;
        }
    }
    return false;
}
}

AgentRuntimeClient::AgentRuntimeClient(QObject *parent,
                                       int heartbeatIntervalMs,
                                       int heartbeatDeadlineMs,
                                       QList<int> reconnectBackoffMs,
                                       TransportMode transportMode)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_transportMode(transportMode)
    , m_startupTimer(new QTimer(this))
    , m_heartbeatIntervalTimer(new QTimer(this))
    , m_heartbeatDeadlineTimer(new QTimer(this))
    , m_reconnectTimer(new QTimer(this))
    , m_reconnectStabilityTimer(new QTimer(this))
{
    if (reconnectBackoffMs.isEmpty()) {
        reconnectBackoffMs = {0, 500, 2000};
    }
    reconnectBackoffMs = reconnectBackoffMs.mid(0, kMaximumReconnectAttempts);
    for (int &delayMs : reconnectBackoffMs) {
        delayMs = qBound(0, delayMs, kMaximumReconnectDelayMs);
    }
    m_reconnectBackoffMs = reconnectBackoffMs;
    m_startupTimer->setSingleShot(true);
    m_heartbeatIntervalTimer->setInterval(qMax(1, heartbeatIntervalMs));
    m_heartbeatDeadlineTimer->setInterval(
        qMax(m_heartbeatIntervalTimer->interval(), heartbeatDeadlineMs));
    m_heartbeatDeadlineTimer->setSingleShot(true);
    m_reconnectTimer->setSingleShot(true);
    m_reconnectStabilityTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this]() {
        if (!m_ready && m_startupGeneration == m_processGeneration
            && m_process->state() != QProcess::NotRunning
            && !m_stopping && !m_autoReconnectSuppressed) {
            const QString detail = QStringLiteral("运行时握手超时，需显式停止后重试");
            suppressAutomaticReconnect();
            m_discardProcessOutput = true;
            clearNegotiationState();
            failPending(detail);
            setReconnectState(ReconnectState::Exhausted, 0, detail);
            emit connectionStateChanged(false, detail);
            m_processTerminationPending = true;
            const quint64 generation = m_processGeneration;
            closeTransportWrite();
            m_process->terminate();
            QTimer::singleShot(kReconnectTerminationGraceMs, this,
                               [this, generation]() {
                if (generation == m_processGeneration
                    && m_processTerminationPending
                    && m_process->state() != QProcess::NotRunning) {
                    m_process->kill();
                }
            });
        }
    });
    connect(m_heartbeatIntervalTimer, &QTimer::timeout, this, [this]() {
        sendHeartbeat(false);
    });
    connect(m_heartbeatDeadlineTimer, &QTimer::timeout,
            this, &AgentRuntimeClient::handleHeartbeatTimeout);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &AgentRuntimeClient::beginReconnectAttempt);
    connect(m_reconnectStabilityTimer, &QTimer::timeout, this, [this]() {
        if (m_reconnectStabilityGeneration != 0
            && m_reconnectStabilityGeneration == m_processGeneration
            && m_reconnectState == ReconnectState::Idle && isReady()
            && m_reconnectAttempt > 0) {
            m_reconnectStabilityGeneration = 0;
            m_reconnectAttempt = 0;
            setReconnectState(ReconnectState::Idle, 0,
                              QStringLiteral("运行时连接已稳定"));
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AgentRuntimeClient::processStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString output = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!output.isEmpty()) emit diagnosticMessage(output);
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (m_stopping || m_autoReconnectSuppressed
            || m_reconnectTerminationPending) {
            return;
        }
        if (error == QProcess::Crashed) {
            return;
        }
        const QString detail = QStringLiteral("运行时启动失败：%1").arg(m_process->errorString());
        if (m_process->state() == QProcess::NotRunning) {
            handleRetryableProcessFailure(detail);
            return;
        }
        suppressAutomaticReconnect();
        clearNegotiationState();
        failPending(detail);
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        m_startupTimer->stop();
        m_startupGeneration = 0;
        m_unixSocketConnectGeneration = 0;
        m_unixSocketPeerVerifiedGeneration = 0;
        m_localSocketPeerVerifiedAttemptEpoch = 0;
        const bool ownedTermination =
            m_ownedTerminationGeneration == m_processGeneration;
        m_ownedTerminationGeneration = 0;
        retireLocalSocket();
        cleanupUnixSocketEndpoint();
        const bool expected = m_stopping;
        if (expected) m_stopping = false;
        if (m_policyRestartPending) {
            m_policyRestartPending = false;
            clearNegotiationState();
            m_autoReconnectSuppressed = false;
            setReconnectState(ReconnectState::Idle, 0,
                              QStringLiteral("正在应用工作台应急策略"));
            QTimer::singleShot(0, this, &AgentRuntimeClient::start);
            return;
        }
        if (m_processTerminationPending) {
            m_processTerminationPending = false;
            clearNegotiationState();
            return;
        }
        if (m_reconnectTerminationPending
            && m_reconnectTerminationGeneration == m_processGeneration) {
            m_reconnectTerminationPending = false;
            m_reconnectTerminationGeneration = 0;
            if (!expected && !m_autoReconnectSuppressed
                && m_reconnectState == ReconnectState::Restarting) {
                launchProcess(true);
            }
            return;
        }
        clearNegotiationState();
        const QString detail = expected
            ? QStringLiteral("运行时已停止")
            : QStringLiteral("运行时已退出（%1，代码 %2）")
                  .arg(status == QProcess::CrashExit ? QStringLiteral("异常")
                                                      : QStringLiteral("正常"))
                  .arg(exitCode);
        failPending(detail);
        if (expected || m_autoReconnectSuppressed) {
            if (m_reconnectState != ReconnectState::Exhausted) {
                setReconnectState(ReconnectState::Idle, 0, detail);
            }
            if (!ownedTermination) emit connectionStateChanged(false, detail);
            return;
        }
        if (m_reconnectState != ReconnectState::Waiting) {
            scheduleReconnect(detail);
        }
        if (!ownedTermination) emit connectionStateChanged(false, detail);
    });
}

AgentRuntimeClient::~AgentRuntimeClient()
{
    stop();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->waitForFinished(500);
    }
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
    retireLocalSocket();
    if (m_process->state() == QProcess::NotRunning) {
        cleanupUnixSocketEndpoint();
    }
}

QString AgentRuntimeClient::timelineEventIdentity(const QJsonObject &event)
{
    const QByteArray item = canonicalTimelineItem(event.value(QStringLiteral("item")));
    const QByteArray itemUpdate = canonicalTimelineItemUpdate(
        event.value(QStringLiteral("item_update")));
    if (item.isEmpty() || itemUpdate.isEmpty()) return {};

    QByteArray material = QByteArrayLiteral("{\"schema_version\":");
    material += compactJsonValue(event.value(QStringLiteral("schema_version")));
    material += QByteArrayLiteral(",\"sequence\":");
    material += compactJsonValue(event.value(QStringLiteral("sequence")));
    material += QByteArrayLiteral(",\"timestamp_ms\":");
    material += compactJsonValue(event.value(QStringLiteral("timestamp_ms")));
    material += QByteArrayLiteral(",\"correlation_id\":");
    material += compactJsonValue(event.value(QStringLiteral("correlation_id")));
    material += QByteArrayLiteral(",\"session_id\":");
    material += compactJsonValue(event.value(QStringLiteral("session_id")));
    material += QByteArrayLiteral(",\"turn_id\":");
    material += compactJsonValue(event.value(QStringLiteral("turn_id")));
    material += QByteArrayLiteral(",\"turn_state\":");
    material += compactJsonValue(event.value(QStringLiteral("turn_state")));
    material += QByteArrayLiteral(",\"event\":");
    material += compactJsonValue(event.value(QStringLiteral("event")));
    material += QByteArrayLiteral(",\"item\":");
    material += item;
    material += QByteArrayLiteral(",\"item_update\":");
    material += itemUpdate;
    material += '}';

    static constexpr char domain[] = "aegisy-timeline-event/0.1\0";
    QByteArray input(domain, sizeof(domain) - 1);
    const quint64 size = static_cast<quint64>(material.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.append(static_cast<char>((size >> shift) & 0xff));
    }
    input += material;
    return QStringLiteral("event:sha256:%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()));
}

QString AgentRuntimeClient::timelineSnapshotItemIdentity(const QString &sessionId,
                                                          const QJsonObject &itemPage)
{
    const QByteArray material = canonicalTimelineSnapshotItemMaterial(sessionId, itemPage);
    if (material.isEmpty()) return {};
    return domainSeparatedSnapshotIdentity(
        "aegisy-timeline-session-snapshot-item/0.1\0",
        "timeline-session-snapshot-item:sha256:", material);
}

quint64 AgentRuntimeClient::timelineSnapshotItemCanonicalBytes(
    const QString &sessionId, const QJsonObject &itemPage)
{
    const QByteArray material = canonicalTimelineSnapshotItemMaterial(sessionId, itemPage);
    return material.isEmpty() ? 0 : static_cast<quint64>(material.size());
}

QString AgentRuntimeClient::timelineSnapshotIdentity(
    const QString &sessionId, const QJsonObject &floor,
    const QJsonObject &watermark, const QJsonObject &activeTurn,
    quint64 totalItems, quint64 totalCanonicalBytes,
    const QStringList &orderedItemIdentities)
{
    const double floorSequence = floor.value(QStringLiteral("sequence")).toDouble();
    const double watermarkSequence = watermark.value(QStringLiteral("sequence")).toDouble();
    if (!isBoundedTimelineIdentity(sessionId)
        || !isValidTimelineAnchor(floor) || !isValidTimelineAnchor(watermark)
        || watermarkSequence < floorSequence
        || (watermarkSequence == floorSequence
            && floor.value(QStringLiteral("event_id"))
                != watermark.value(QStringLiteral("event_id")))
        || totalItems > 10000 || totalCanonicalBytes > 64ULL * 1024ULL * 1024ULL
        || totalItems != static_cast<quint64>(orderedItemIdentities.size())
        || (totalItems == 0) != (totalCanonicalBytes == 0)) {
        return {};
    }
    const QByteArray active = activeTurn.isEmpty()
        ? QByteArrayLiteral("null")
        : canonicalTimelineSnapshotActiveTurn(activeTurn);
    if (active.isEmpty()) return {};
    if (!activeTurn.isEmpty()) {
        const QJsonObject latest = activeTurn.value(QStringLiteral("latest_event")).toObject();
        if (!isValidTimelineAnchor(latest)
            || latest.value(QStringLiteral("sequence")).toDouble()
                > watermark.value(QStringLiteral("sequence")).toDouble()) {
            return {};
        }
    }
    QByteArray identities(1, '[');
    for (qsizetype index = 0; index < orderedItemIdentities.size(); ++index) {
        if (index > 0) identities += ',';
        const QString &identity = orderedItemIdentities.at(index);
        if (!isValidTimelineSnapshotItemIdentity(identity)) return {};
        identities += compactJsonValue(QJsonValue(identity));
    }
    identities += ']';

    QByteArray material = QByteArrayLiteral(
        "{\"schema_version\":\"timeline-session-snapshot-page/0.1\",\"session_id\":");
    material += compactJsonValue(QJsonValue(sessionId));
    material += QByteArrayLiteral(",\"floor\":");
    material += canonicalTimelineSnapshotAnchor(floor);
    material += QByteArrayLiteral(",\"watermark\":");
    material += canonicalTimelineSnapshotAnchor(watermark);
    material += QByteArrayLiteral(",\"active_turn\":");
    material += active;
    material += QByteArrayLiteral(",\"total_items\":");
    material += QByteArray::number(totalItems);
    material += QByteArrayLiteral(",\"total_canonical_bytes\":");
    material += QByteArray::number(totalCanonicalBytes);
    material += QByteArrayLiteral(",\"ordered_item_identities\":");
    material += identities;
    material += '}';
    return domainSeparatedSnapshotIdentity(
        "aegisy-timeline-session-snapshot/0.1\0",
        "timeline-session-snapshot:sha256:", material);
}

QString AgentRuntimeClient::timelineSnapshotPageIdentity(const QJsonObject &page)
{
    if (!hasExactKeys(page, {
            QStringLiteral("schema_version"), QStringLiteral("session_id"),
            QStringLiteral("snapshot_identity"), QStringLiteral("floor"),
            QStringLiteral("watermark"), QStringLiteral("active_turn"),
            QStringLiteral("total_items"), QStringLiteral("total_canonical_bytes"),
            QStringLiteral("after"), QStringLiteral("items"),
            QStringLiteral("next_after"), QStringLiteral("complete"),
            QStringLiteral("page_identity"),
        })
        || page.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("timeline-session-snapshot-page/0.1")
        || !isValidTimelineSnapshotIdentity(
            page.value(QStringLiteral("snapshot_identity")).toString())
        || !page.value(QStringLiteral("session_id")).isString()
        || !isValidTimelineAnchor(page.value(QStringLiteral("floor")).toObject())
        || !isValidTimelineAnchor(page.value(QStringLiteral("watermark")).toObject())
        || !page.value(QStringLiteral("items")).isArray()
        || !page.value(QStringLiteral("complete")).isBool()) {
        return {};
    }
    const QByteArray after = page.value(QStringLiteral("after")).isNull()
        ? QByteArrayLiteral("null")
        : canonicalTimelineSnapshotCursor(page.value(QStringLiteral("after")));
    const QByteArray nextAfter = page.value(QStringLiteral("next_after")).isNull()
        ? QByteArrayLiteral("null")
        : canonicalTimelineSnapshotCursor(page.value(QStringLiteral("next_after")));
    if (after.isEmpty() || nextAfter.isEmpty()) return {};
    const QJsonArray items = page.value(QStringLiteral("items")).toArray();
    QByteArray identities(1, '[');
    QSet<QString> seen;
    for (qsizetype index = 0; index < items.size(); ++index) {
        if (index > 0) identities += ',';
        if (!items.at(index).isObject()) return {};
        const QJsonObject item = items.at(index).toObject();
        const QString identity = item.value(QStringLiteral("item_identity")).toString();
        if (!isValidTimelineSnapshotItemIdentity(identity) || seen.contains(identity)) {
            return {};
        }
        seen.insert(identity);
        identities += compactJsonValue(QJsonValue(identity));
    }
    identities += ']';
    QByteArray material = QByteArrayLiteral(
        "{\"schema_version\":\"timeline-session-snapshot-page/0.1\",\"snapshot_identity\":");
    material += compactJsonValue(page.value(QStringLiteral("snapshot_identity")));
    material += QByteArrayLiteral(",\"after\":");
    material += after;
    material += QByteArrayLiteral(",\"ordered_item_identities\":");
    material += identities;
    material += QByteArrayLiteral(",\"next_after\":");
    material += nextAfter;
    material += QByteArrayLiteral(",\"complete\":");
    material += compactJsonValue(page.value(QStringLiteral("complete")));
    material += '}';
    return domainSeparatedSnapshotIdentity(
        "aegisy-timeline-session-snapshot-page/0.1\0",
        "timeline-session-snapshot-page:sha256:", material);
}

QString AgentRuntimeClient::timelineSubscriptionRequestIdentity(
    const QString &stage, const QJsonObject &request)
{
    return domainSeparatedTimelineSubscriptionRequestIdentity(stage, request);
}

QString AgentRuntimeClient::durableMutationOperationIdentity(
    const QString &sessionId, const QString &mutationKind,
    const QString &idempotencyKey, const QString &requestFingerprint)
{
    if (!isBoundedTimelineIdentity(QJsonValue(sessionId))
            || mutationKind != QStringLiteral("turn-start")
            || !isBoundedTimelineIdentity(QJsonValue(idempotencyKey))
            || !isValidMutationFingerprint(QJsonValue(requestFingerprint))) {
        return {};
    }
    QByteArray material = QByteArrayLiteral(
        "aegisy-durable-mutation-operation/0.1");
    material.append('\0');
    const auto appendField = [&material](const QString &value) {
        const QByteArray bytes = value.toUtf8();
        const quint64 size = static_cast<quint64>(bytes.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            material.append(static_cast<char>((size >> shift) & 0xff));
        }
        material.append(bytes);
    };
    appendField(sessionId);
    appendField(mutationKind);
    appendField(idempotencyKey);
    appendField(requestFingerprint);
    return QStringLiteral("mutation-operation:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            material, QCryptographicHash::Sha256).toHex());
}

QString AgentRuntimeClient::workspaceEditProposalPreviewIdentity(
    const QJsonObject &proposal)
{
    const QString schema = proposal.value(QStringLiteral("internal_schema_version"))
        .toString();
    const QJsonObject summary = proposal.value(QStringLiteral("summary")).toObject();
    if ((schema != QStringLiteral("workspace-edit-proposal/0.1")
         && schema != QStringLiteral("workspace-edit-proposal/0.2"))
        || summary.isEmpty()) {
        return {};
    }

    const auto encodeDiff = [](const QJsonObject &diff) {
        QByteArray encoded = QByteArrayLiteral("{\"reference\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("reference")));
        encoded += QByteArrayLiteral(",\"sha256\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("sha256")));
        encoded += QByteArrayLiteral(",\"bytes\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("bytes")));
        encoded += QByteArrayLiteral(",\"media_type\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("media_type")));
        encoded += QByteArrayLiteral(",\"inline_truncated\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("inline_truncated")));
        encoded += QByteArrayLiteral(",\"source_truncated\":");
        encoded += compactJsonValue(diff.value(QStringLiteral("source_truncated")));
        encoded += '}';
        return encoded;
    };
    const auto encodeFormat = [](const QJsonObject &format) {
        QByteArray encoded = QByteArrayLiteral("{\"encoding\":");
        encoded += compactJsonValue(format.value(QStringLiteral("encoding")));
        encoded += QByteArrayLiteral(",\"newline\":");
        encoded += compactJsonValue(format.value(QStringLiteral("newline")));
        encoded += QByteArrayLiteral(",\"mode\":");
        encoded += compactJsonValue(format.value(QStringLiteral("mode")));
        encoded += '}';
        return encoded;
    };
    const auto encodeWarnings = [](const QJsonArray &warnings) {
        QByteArray encoded(1, '[');
        for (qsizetype index = 0; index < warnings.size(); ++index) {
            if (index > 0) encoded += ',';
            const QJsonObject warning = warnings.at(index).toObject();
            encoded += QByteArrayLiteral("{\"code\":");
            encoded += compactJsonValue(warning.value(QStringLiteral("code")));
            encoded += QByteArrayLiteral(",\"severity\":");
            encoded += compactJsonValue(warning.value(QStringLiteral("severity")));
            encoded += QByteArrayLiteral(",\"path\":");
            encoded += compactJsonValue(warning.value(QStringLiteral("path")));
            encoded += '}';
        }
        encoded += ']';
        return encoded;
    };

    const QJsonObject aggregate = summary.value(QStringLiteral("aggregate_diff")).toObject();
    const QJsonArray files = summary.value(QStringLiteral("files")).toArray();
    QByteArray references(1, '[');
    QByteArray encodedFiles(1, '[');
    for (qsizetype index = 0; index < files.size(); ++index) {
        const QJsonObject file = files.at(index).toObject();
        const QJsonObject diff = file.value(QStringLiteral("diff")).toObject();
        if (index > 0) {
            references += ',';
            encodedFiles += ',';
        }
        references += compactJsonValue(diff.value(QStringLiteral("reference")));
        encodedFiles += QByteArrayLiteral("{\"ordinal\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("ordinal")));
        encodedFiles += QByteArrayLiteral(",\"kind\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("kind")));
        encodedFiles += QByteArrayLiteral(",\"path\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("path")));
        if (file.contains(QStringLiteral("from_path"))
            && !file.value(QStringLiteral("from_path")).isNull()) {
            encodedFiles += QByteArrayLiteral(",\"from_path\":");
            encodedFiles += compactJsonValue(file.value(QStringLiteral("from_path")));
        }
        encodedFiles += QByteArrayLiteral(",\"additions\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("additions")));
        encodedFiles += QByteArrayLiteral(",\"deletions\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("deletions")));
        encodedFiles += QByteArrayLiteral(",\"base_matches\":");
        encodedFiles += compactJsonValue(file.value(QStringLiteral("base_matches")));
        if (file.contains(QStringLiteral("proposed_format"))) {
            encodedFiles += QByteArrayLiteral(",\"proposed_format\":");
            encodedFiles += encodeFormat(
                file.value(QStringLiteral("proposed_format")).toObject());
        }
        encodedFiles += QByteArrayLiteral(",\"warnings\":");
        encodedFiles += encodeWarnings(file.value(QStringLiteral("warnings")).toArray());
        encodedFiles += QByteArrayLiteral(",\"diff\":");
        encodedFiles += encodeDiff(diff);
        encodedFiles += '}';
    }
    references += ']';
    encodedFiles += ']';

    QByteArray material = QByteArrayLiteral("{\"file_count\":");
    material += compactJsonValue(summary.value(QStringLiteral("file_count")));
    material += QByteArrayLiteral(",\"additions\":");
    material += compactJsonValue(summary.value(QStringLiteral("additions")));
    material += QByteArrayLiteral(",\"deletions\":");
    material += compactJsonValue(summary.value(QStringLiteral("deletions")));
    material += QByteArrayLiteral(",\"warning_count\":");
    material += compactJsonValue(summary.value(QStringLiteral("warning_count")));
    material += QByteArrayLiteral(",\"applicable\":");
    material += compactJsonValue(summary.value(QStringLiteral("applicable")));
    material += QByteArrayLiteral(",\"aggregate_diff_reference\":");
    material += compactJsonValue(aggregate.value(QStringLiteral("reference")));
    material += QByteArrayLiteral(",\"file_diff_references\":");
    material += references;
    if (schema == QStringLiteral("workspace-edit-proposal/0.2")) {
        material += QByteArrayLiteral(",\"aggregate_diff\":");
        material += encodeDiff(aggregate);
        material += QByteArrayLiteral(",\"files\":");
        material += encodedFiles;
    }
    material += '}';
    QByteArray input = QByteArrayLiteral("workspace-edit-preview\0");
    input += material;
    return QStringLiteral("workspace-edit-preview:sha256:%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()));
}

QString AgentRuntimeClient::workspaceEditProposalArtifactPageIdentity(
    const QJsonObject &page)
{
    const QJsonObject artifact = page.value(QStringLiteral("artifact")).toObject();
    const QJsonValue nextOffset = page.value(QStringLiteral("next_offset"));
    const auto safeInteger = [](const QJsonValue &value) {
        if (!value.isDouble()) return false;
        const double number = value.toDouble();
        return std::isfinite(number) && std::floor(number) == number
            && number >= 0 && number <= kMaximumSafeJsonInteger;
    };
    if (!hasExactKeys(page, {
            QStringLiteral("schema_version"), QStringLiteral("session_id"),
            QStringLiteral("proposal_id"), QStringLiteral("project_id"),
            QStringLiteral("edit_id"), QStringLiteral("artifact"),
            QStringLiteral("offset"), QStringLiteral("next_offset"),
            QStringLiteral("total_bytes"), QStringLiteral("data_base64"),
            QStringLiteral("chunk_sha256"), QStringLiteral("page_identity"),
            QStringLiteral("file_mutation_authority"),
            QStringLiteral("approval_recorded"), QStringLiteral("apply_available"),
        })
        || !hasExactKeys(artifact, {
            QStringLiteral("kind"), QStringLiteral("reference"),
            QStringLiteral("sha256"), QStringLiteral("bytes"),
            QStringLiteral("media_type"),
        })
        || page.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("workspace-edit-proposal-artifact-page/0.1")
        || !safeInteger(artifact.value(QStringLiteral("bytes")))
        || !safeInteger(page.value(QStringLiteral("offset")))
        || !safeInteger(page.value(QStringLiteral("total_bytes")))
        || (!nextOffset.isNull() && !safeInteger(nextOffset))) {
        return {};
    }
    QByteArray material = QByteArrayLiteral(
        "{\"schema_version\":\"workspace-edit-proposal-artifact-page/0.1\",\"session_id\":");
    material += compactJsonValue(page.value(QStringLiteral("session_id")));
    material += QByteArrayLiteral(",\"proposal_id\":");
    material += compactJsonValue(page.value(QStringLiteral("proposal_id")));
    material += QByteArrayLiteral(",\"project_id\":");
    material += compactJsonValue(page.value(QStringLiteral("project_id")));
    material += QByteArrayLiteral(",\"edit_id\":");
    material += compactJsonValue(page.value(QStringLiteral("edit_id")));
    material += QByteArrayLiteral(",\"artifact_kind\":");
    material += compactJsonValue(artifact.value(QStringLiteral("kind")));
    material += QByteArrayLiteral(",\"reference\":");
    material += compactJsonValue(artifact.value(QStringLiteral("reference")));
    material += QByteArrayLiteral(",\"sha256\":");
    material += compactJsonValue(artifact.value(QStringLiteral("sha256")));
    material += QByteArrayLiteral(",\"bytes\":");
    material += compactJsonValue(artifact.value(QStringLiteral("bytes")));
    material += QByteArrayLiteral(",\"media_type\":");
    material += compactJsonValue(artifact.value(QStringLiteral("media_type")));
    material += QByteArrayLiteral(",\"offset\":");
    material += compactJsonValue(page.value(QStringLiteral("offset")));
    material += QByteArrayLiteral(",\"next_offset\":");
    material += nextOffset.isNull() ? QByteArrayLiteral("null")
                                    : compactJsonValue(nextOffset);
    material += QByteArrayLiteral(",\"chunk_sha256\":");
    material += compactJsonValue(page.value(QStringLiteral("chunk_sha256")));
    material += '}';
    QByteArray input = QByteArrayLiteral("workspace-edit-proposal-artifact-page\0");
    input += material;
    return QStringLiteral("workspace-edit-proposal-artifact-page:sha256:%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex()));
}

QString AgentRuntimeClient::commandArtifactPageBindingIdentity(
    const QJsonObject &result)
{
    const QJsonObject content = result.value(QStringLiteral("content_reference")).toObject();
    quint64 createdAt = 0;
    quint64 sourceBytes = 0;
    quint64 redactedCount = 0;
    quint64 totalBytes = 0;
    quint64 retainedBytes = 0;
    quint64 omittedBytes = 0;
    if (!result.value(QStringLiteral("session_id")).isString()
            || !content.value(QStringLiteral("reference")).isString()
            || !content.value(QStringLiteral("sha256")).isString()
            || !content.value(QStringLiteral("media_type")).isString()
            || !result.value(QStringLiteral("item_id")).isString()
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("created_at_ms")), &createdAt)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("source_bytes")), &sourceBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("redacted_count")), &redactedCount)
            || !result.value(QStringLiteral("redacted")).isBool()
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("total_bytes")), &totalBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("retained_bytes")), &retainedBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("omitted_bytes")), &omittedBytes)
            || !result.value(QStringLiteral("truncated")).isBool()) {
        return {};
    }
    QByteArray material = QByteArrayLiteral("command-output-artifact-page-binding\0");
    const QList<QByteArray> components{
        QByteArrayLiteral("command-output-artifact-page-binding/0.1"),
        result.value(QStringLiteral("session_id")).toString().toUtf8(),
        content.value(QStringLiteral("reference")).toString().toUtf8(),
        content.value(QStringLiteral("sha256")).toString().toUtf8(),
        content.value(QStringLiteral("media_type")).toString().toUtf8(),
        result.value(QStringLiteral("item_id")).toString().toUtf8(),
        unsignedBigEndian(createdAt),
        unsignedBigEndian(sourceBytes),
        unsignedBigEndian(redactedCount),
        QByteArray(1, static_cast<char>(result.value(
            QStringLiteral("redacted")).toBool())),
        unsignedBigEndian(totalBytes),
        unsignedBigEndian(retainedBytes),
        unsignedBigEndian(omittedBytes),
        QByteArray(1, static_cast<char>(result.value(
            QStringLiteral("truncated")).toBool())),
    };
    for (const QByteArray &component : components) {
        appendLengthFramed(&material, component);
    }
    return QStringLiteral("content-reference-binding:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            material, QCryptographicHash::Sha256).toHex());
}

QString AgentRuntimeClient::contentPreviewIdentity(const QJsonObject &preview)
{
    quint64 contentBytes = 0;
    quint64 previewBytes = 0;
    quint64 lineCount = std::numeric_limits<quint64>::max();
    quint64 width = std::numeric_limits<quint64>::max();
    quint64 height = std::numeric_limits<quint64>::max();
    const auto optionalInteger = [](const QJsonValue &value, quint64 *number) {
        return value.isNull() || readUnsignedSafeJsonInteger(value, number);
    };
    if (!preview.value(QStringLiteral("reference")).isString()
            || !preview.value(QStringLiteral("sha256")).isString()
            || !preview.value(QStringLiteral("media_type")).isString()
            || !readUnsignedSafeJsonInteger(
                preview.value(QStringLiteral("content_bytes")), &contentBytes)
            || !readUnsignedSafeJsonInteger(
                preview.value(QStringLiteral("preview_bytes")), &previewBytes)
            || !preview.value(QStringLiteral("truncated")).isBool()
            || !optionalInteger(preview.value(QStringLiteral("line_count")), &lineCount)
            || !optionalInteger(preview.value(QStringLiteral("width")), &width)
            || !optionalInteger(preview.value(QStringLiteral("height")), &height)) {
        return {};
    }
    return lengthFramedIdentity(
        QByteArrayLiteral("content-preview:sha256:"), {
            preview.value(QStringLiteral("reference")).toString().toUtf8(),
            preview.value(QStringLiteral("sha256")).toString().toUtf8(),
            preview.value(QStringLiteral("media_type")).toString().toUtf8(),
            unsignedBigEndian(contentBytes),
            unsignedBigEndian(previewBytes),
            QByteArray(1, static_cast<char>(preview.value(
                QStringLiteral("truncated")).toBool())),
            unsignedBigEndian(lineCount),
            unsignedBigEndian(width),
            unsignedBigEndian(height),
        });
}

QString AgentRuntimeClient::contentInlineLimitsIdentity(const QJsonObject &limits)
{
    quint64 maxItemBytes = 0;
    quint64 maxTotalBytes = 0;
    if (!readUnsignedSafeJsonInteger(
            limits.value(QStringLiteral("max_item_bytes")), &maxItemBytes)
            || !readUnsignedSafeJsonInteger(
                limits.value(QStringLiteral("max_total_bytes")), &maxTotalBytes)) {
        return {};
    }
    return lengthFramedIdentity(
        QByteArrayLiteral("content-inline-limits:sha256:"), {
            QByteArrayLiteral("content-inline-limits/0.1"),
            unsignedBigEndian(maxItemBytes),
            unsignedBigEndian(maxTotalBytes),
        });
}

QString AgentRuntimeClient::contentReferenceCursorIdentity(const QJsonObject &cursor)
{
    quint64 bytes = 0;
    quint64 offset = 0;
    quint64 pageSize = 0;
    const QJsonObject limits = cursor.value(QStringLiteral("limits")).toObject();
    if (!cursor.value(QStringLiteral("reference")).isString()
            || !cursor.value(QStringLiteral("sha256")).isString()
            || !cursor.value(QStringLiteral("media_type")).isString()
            || !readUnsignedSafeJsonInteger(cursor.value(QStringLiteral("bytes")), &bytes)
            || !readUnsignedSafeJsonInteger(cursor.value(QStringLiteral("offset")), &offset)
            || !readUnsignedSafeJsonInteger(
                cursor.value(QStringLiteral("page_size")), &pageSize)
            || !limits.value(QStringLiteral("identity")).isString()
            || (cursor.contains(QStringLiteral("binding_identity"))
                && !cursor.value(QStringLiteral("binding_identity")).isString())) {
        return {};
    }
    QList<QByteArray> components{
        cursor.value(QStringLiteral("reference")).toString().toUtf8(),
        cursor.value(QStringLiteral("sha256")).toString().toUtf8(),
        unsignedBigEndian(bytes),
        cursor.value(QStringLiteral("media_type")).toString().toUtf8(),
        unsignedBigEndian(offset),
        unsignedBigEndian(pageSize),
        limits.value(QStringLiteral("identity")).toString().toUtf8(),
    };
    if (cursor.contains(QStringLiteral("binding_identity"))) {
        components.append(cursor.value(QStringLiteral("binding_identity")).toString().toUtf8());
    }
    return lengthFramedIdentity(
        QByteArrayLiteral("content-reference-cursor:sha256:"), components);
}

QString AgentRuntimeClient::contentReferencePageIdentity(const QJsonObject &page)
{
    quint64 bytes = 0;
    quint64 offset = 0;
    quint64 pageSize = 0;
    quint64 pageBytes = 0;
    const QJsonObject limits = page.value(QStringLiteral("limits")).toObject();
    const QJsonValue inlineValue = page.value(QStringLiteral("inline"));
    const QJsonValue nextCursor = page.value(QStringLiteral("next_cursor"));
    if (!page.value(QStringLiteral("reference")).isString()
            || !page.value(QStringLiteral("sha256")).isString()
            || !page.value(QStringLiteral("media_type")).isString()
            || !readUnsignedSafeJsonInteger(page.value(QStringLiteral("bytes")), &bytes)
            || !readUnsignedSafeJsonInteger(page.value(QStringLiteral("offset")), &offset)
            || !readUnsignedSafeJsonInteger(
                page.value(QStringLiteral("page_size")), &pageSize)
            || !readUnsignedSafeJsonInteger(
                page.value(QStringLiteral("page_bytes")), &pageBytes)
            || (!inlineValue.isNull() && !inlineValue.isString())
            || !page.value(QStringLiteral("inline_truncated")).isBool()
            || !limits.value(QStringLiteral("identity")).isString()
            || (!nextCursor.isNull() && !nextCursor.isObject())
            || (page.contains(QStringLiteral("binding_identity"))
                && !page.value(QStringLiteral("binding_identity")).isString())) {
        return {};
    }
    const QByteArray inlineBytes = inlineValue.isString()
        ? inlineValue.toString().toUtf8() : QByteArray{};
    const QByteArray inlineHash = QCryptographicHash::hash(
        inlineBytes, QCryptographicHash::Sha256);
    const QByteArray nextIdentity = nextCursor.isObject()
        ? nextCursor.toObject().value(QStringLiteral("identity")).toString().toUtf8()
        : QByteArrayLiteral("none");
    QList<QByteArray> components{
        page.value(QStringLiteral("reference")).toString().toUtf8(),
        page.value(QStringLiteral("sha256")).toString().toUtf8(),
        unsignedBigEndian(bytes),
        page.value(QStringLiteral("media_type")).toString().toUtf8(),
        unsignedBigEndian(offset),
        unsignedBigEndian(pageSize),
        unsignedBigEndian(pageBytes),
        inlineHash,
        QByteArray(1, static_cast<char>(page.value(
            QStringLiteral("inline_truncated")).toBool())),
        limits.value(QStringLiteral("identity")).toString().toUtf8(),
    };
    if (page.contains(QStringLiteral("binding_identity"))) {
        components.append(page.value(QStringLiteral("binding_identity")).toString().toUtf8());
    }
    components.append(nextIdentity);
    return lengthFramedIdentity(
        QByteArrayLiteral("content-reference-page:sha256:"), components);
}

bool AgentRuntimeClient::isValidCommandArtifactPage(
    const QJsonObject &result, const QJsonObject &request)
{
    constexpr quint64 kMaximumContentBytes = 16 * 1024 * 1024;
    constexpr quint64 kMaximumInlineItemBytes = 64 * 1024;
    constexpr quint64 kMaximumInlineTotalBytes = 256 * 1024;
    constexpr quint64 kMaximumPreviewLines = 1'000'000;
    constexpr quint64 kMaximumRetainedCommandBytes = 2 * 1024 * 1024;
    const QString mediaType = QStringLiteral("text/plain; charset=utf-8");
    const bool continuation = request.contains(QStringLiteral("cursor"));
    const QStringList firstRequestKeys{
        QStringLiteral("session_id"), QStringLiteral("item_id"),
        QStringLiteral("reference"), QStringLiteral("limit"),
        QStringLiteral("max_total_inline_bytes"),
    };
    const QStringList continuationRequestKeys{
        QStringLiteral("session_id"), QStringLiteral("item_id"),
        QStringLiteral("reference"), QStringLiteral("cursor"),
    };
    if (!hasExactKeys(request, continuation ? continuationRequestKeys : firstRequestKeys)
            || !isAsciiGraphicalId(request.value(QStringLiteral("session_id")), 128)
            || !isAsciiGraphicalId(request.value(QStringLiteral("item_id")), 128)
            || !request.value(QStringLiteral("reference")).isString()) {
        return false;
    }
    if (!hasExactKeys(result, {
            QStringLiteral("schema_version"), QStringLiteral("session_id"),
            QStringLiteral("item_id"), QStringLiteral("binding_identity"),
            QStringLiteral("created_at_ms"), QStringLiteral("content_reference"),
            QStringLiteral("page"), QStringLiteral("source_bytes"),
            QStringLiteral("redacted_count"), QStringLiteral("redacted"),
            QStringLiteral("total_bytes"), QStringLiteral("retained_bytes"),
            QStringLiteral("omitted_bytes"), QStringLiteral("truncated"),
            QStringLiteral("read_only"),
        })
        || result.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("command-output-artifact-page/0.1")
        || result.value(QStringLiteral("session_id"))
            != request.value(QStringLiteral("session_id"))
        || result.value(QStringLiteral("item_id"))
            != request.value(QStringLiteral("item_id"))
        || !result.value(QStringLiteral("binding_identity")).isString()
        || !result.value(QStringLiteral("read_only")).isBool()
        || !result.value(QStringLiteral("read_only")).toBool()
        || !result.value(QStringLiteral("redacted")).isBool()
        || !result.value(QStringLiteral("truncated")).isBool()
        || !result.value(QStringLiteral("content_reference")).isObject()
        || !result.value(QStringLiteral("page")).isObject()) {
        return false;
    }

    quint64 createdAt = 0;
    quint64 sourceBytes = 0;
    quint64 redactedCount = 0;
    quint64 totalBytes = 0;
    quint64 retainedBytes = 0;
    quint64 omittedBytes = 0;
    if (!readUnsignedSafeJsonInteger(
            result.value(QStringLiteral("created_at_ms")), &createdAt)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("source_bytes")), &sourceBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("redacted_count")), &redactedCount)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("total_bytes")), &totalBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("retained_bytes")), &retainedBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("omitted_bytes")), &omittedBytes)
            || retainedBytes > kMaximumRetainedCommandBytes
            || totalBytes != retainedBytes + omittedBytes
            || result.value(QStringLiteral("redacted")).toBool()
                != (redactedCount > 0)
            || result.value(QStringLiteral("truncated")).toBool()
                != (omittedBytes > 0)) {
        return false;
    }
    Q_UNUSED(createdAt);
    Q_UNUSED(sourceBytes);

    const QJsonObject content = result.value(QStringLiteral("content_reference")).toObject();
    if (!hasExactKeys(content, {
            QStringLiteral("schema_version"), QStringLiteral("reference"),
            QStringLiteral("sha256"), QStringLiteral("bytes"),
            QStringLiteral("media_type"), QStringLiteral("preview"),
        })
        || content.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("content-reference/0.1")
        || !content.value(QStringLiteral("reference")).isString()
        || !content.value(QStringLiteral("sha256")).isString()
        || !content.value(QStringLiteral("media_type")).isString()
        || !content.value(QStringLiteral("preview")).isObject()) {
        return false;
    }
    quint64 contentBytes = 0;
    const QString reference = content.value(QStringLiteral("reference")).toString();
    const QString sha256 = content.value(QStringLiteral("sha256")).toString();
    if (!readUnsignedSafeJsonInteger(
            content.value(QStringLiteral("bytes")), &contentBytes)
            || contentBytes > kMaximumContentBytes
            || content.value(QStringLiteral("media_type")).toString() != mediaType
            || request.value(QStringLiteral("reference")).toString() != reference
            || !isCommandOutputReference(reference, sha256)) {
        return false;
    }
    const quint64 omissionMarkerBytes = omittedBytes > 0
        ? static_cast<quint64>(QStringLiteral(
            "\n[Aegisy omitted %1 command output bytes]\n")
                .arg(omittedBytes).toUtf8().size())
        : 0;
    if (contentBytes != retainedBytes + omissionMarkerBytes) return false;

    const QJsonObject preview = content.value(QStringLiteral("preview")).toObject();
    quint64 previewContentBytes = 0;
    quint64 previewBytes = 0;
    quint64 lineCount = 0;
    if (!hasExactKeys(preview, {
            QStringLiteral("schema_version"), QStringLiteral("identity"),
            QStringLiteral("reference"), QStringLiteral("sha256"),
            QStringLiteral("media_type"), QStringLiteral("content_bytes"),
            QStringLiteral("preview_bytes"), QStringLiteral("truncated"),
            QStringLiteral("line_count"), QStringLiteral("width"),
            QStringLiteral("height"),
        })
        || preview.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("content-preview/0.1")
        || preview.value(QStringLiteral("reference")).toString() != reference
        || preview.value(QStringLiteral("sha256")).toString() != sha256
        || preview.value(QStringLiteral("media_type")).toString() != mediaType
        || !readUnsignedSafeJsonInteger(
            preview.value(QStringLiteral("content_bytes")), &previewContentBytes)
        || !readUnsignedSafeJsonInteger(
            preview.value(QStringLiteral("preview_bytes")), &previewBytes)
        || !readUnsignedSafeJsonInteger(
            preview.value(QStringLiteral("line_count")), &lineCount)
        || !preview.value(QStringLiteral("truncated")).isBool()
        || !preview.value(QStringLiteral("width")).isNull()
        || !preview.value(QStringLiteral("height")).isNull()
        || previewContentBytes != contentBytes
        || previewBytes > contentBytes || previewBytes > kMaximumInlineItemBytes
        || preview.value(QStringLiteral("truncated")).toBool()
            != (previewBytes < contentBytes)
        || lineCount > kMaximumPreviewLines || lineCount > contentBytes + 1
        || !isSha256Identity(preview.value(QStringLiteral("identity")).toString(),
                            QStringLiteral("content-preview:sha256:"))
        || preview.value(QStringLiteral("identity")).toString()
            != contentPreviewIdentity(preview)) {
        return false;
    }

    const QJsonObject page = result.value(QStringLiteral("page")).toObject();
    const QJsonObject limits = page.value(QStringLiteral("limits")).toObject();
    if (!hasExactKeys(limits, {
            QStringLiteral("schema_version"), QStringLiteral("max_item_bytes"),
            QStringLiteral("max_total_bytes"), QStringLiteral("identity"),
        })
        || limits.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("content-inline-limits/0.1")) {
        return false;
    }
    quint64 maxItemBytes = 0;
    quint64 maxTotalBytes = 0;
    if (!readUnsignedSafeJsonInteger(
            limits.value(QStringLiteral("max_item_bytes")), &maxItemBytes)
        || !readUnsignedSafeJsonInteger(
            limits.value(QStringLiteral("max_total_bytes")), &maxTotalBytes)
        || maxItemBytes == 0 || maxItemBytes > kMaximumInlineItemBytes
        || maxTotalBytes == 0 || maxTotalBytes > kMaximumInlineTotalBytes
        || maxItemBytes > maxTotalBytes
        || limits.value(QStringLiteral("identity")).toString()
            != contentInlineLimitsIdentity(limits)) {
        return false;
    }

    if (!hasExactKeys(page, {
            QStringLiteral("schema_version"), QStringLiteral("reference"),
            QStringLiteral("sha256"), QStringLiteral("bytes"),
            QStringLiteral("media_type"), QStringLiteral("offset"),
            QStringLiteral("page_size"), QStringLiteral("page_bytes"),
            QStringLiteral("inline"), QStringLiteral("inline_truncated"),
            QStringLiteral("limits"), QStringLiteral("binding_identity"),
            QStringLiteral("next_cursor"), QStringLiteral("identity"),
        })
        || page.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("content-reference-page/0.1")
        || page.value(QStringLiteral("reference")).toString() != reference
        || page.value(QStringLiteral("sha256")).toString() != sha256
        || page.value(QStringLiteral("media_type")).toString() != mediaType
        || page.value(QStringLiteral("bytes")) != content.value(QStringLiteral("bytes"))
        || page.value(QStringLiteral("limits")) != QJsonValue(limits)
        || !page.value(QStringLiteral("inline")).isString()
        || !isUnicodeScalarString(page.value(QStringLiteral("inline")).toString())
        || !page.value(QStringLiteral("inline_truncated")).isBool()
        || page.value(QStringLiteral("inline_truncated")).toBool()
        || !page.value(QStringLiteral("binding_identity")).isString()
        || page.value(QStringLiteral("binding_identity"))
            != result.value(QStringLiteral("binding_identity"))) {
        return false;
    }
    quint64 pageContentBytes = 0;
    quint64 offset = 0;
    quint64 pageSize = 0;
    quint64 pageBytes = 0;
    const quint64 inlineBytes = static_cast<quint64>(
        page.value(QStringLiteral("inline")).toString().toUtf8().size());
    if (!readUnsignedSafeJsonInteger(page.value(QStringLiteral("bytes")), &pageContentBytes)
        || !readUnsignedSafeJsonInteger(page.value(QStringLiteral("offset")), &offset)
        || !readUnsignedSafeJsonInteger(page.value(QStringLiteral("page_size")), &pageSize)
        || !readUnsignedSafeJsonInteger(page.value(QStringLiteral("page_bytes")), &pageBytes)
        || pageContentBytes != contentBytes || pageSize == 0
        || pageSize > kMaximumInlineItemBytes || pageSize > maxItemBytes
        || pageBytes > pageSize || pageBytes > contentBytes - qMin(offset, contentBytes)
        || offset > contentBytes || (contentBytes > 0 && pageBytes == 0)
        || inlineBytes != pageBytes
        || page.value(QStringLiteral("identity")).toString()
            != contentReferencePageIdentity(page)) {
        return false;
    }

    const QString binding = commandArtifactPageBindingIdentity(result);
    if (binding.isEmpty()
            || result.value(QStringLiteral("binding_identity")).toString() != binding) {
        return false;
    }

    const QJsonValue nextValue = page.value(QStringLiteral("next_cursor"));
    if (nextValue.isObject()) {
        const QJsonObject cursor = nextValue.toObject();
        if (!hasExactKeys(cursor, {
                QStringLiteral("schema_version"), QStringLiteral("reference"),
                QStringLiteral("sha256"), QStringLiteral("bytes"),
                QStringLiteral("media_type"), QStringLiteral("offset"),
                QStringLiteral("page_size"), QStringLiteral("limits"),
                QStringLiteral("binding_identity"), QStringLiteral("identity"),
            })
            || cursor.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("content-reference-cursor/0.1")
            || cursor.value(QStringLiteral("reference")).toString() != reference
            || cursor.value(QStringLiteral("sha256")).toString() != sha256
            || cursor.value(QStringLiteral("bytes")) != page.value(QStringLiteral("bytes"))
            || cursor.value(QStringLiteral("media_type")).toString() != mediaType
            || cursor.value(QStringLiteral("offset")).toDouble()
                != static_cast<double>(offset + pageBytes)
            || cursor.value(QStringLiteral("page_size"))
                != page.value(QStringLiteral("page_size"))
            || cursor.value(QStringLiteral("limits")) != page.value(QStringLiteral("limits"))
            || cursor.value(QStringLiteral("binding_identity"))
                != result.value(QStringLiteral("binding_identity"))
            || cursor.value(QStringLiteral("identity")).toString()
                != contentReferenceCursorIdentity(cursor)
            || offset + pageBytes >= contentBytes) {
            return false;
        }
    } else if (!nextValue.isNull() || offset + pageBytes != contentBytes) {
        return false;
    }

    if (continuation) {
        const QJsonObject requestedCursor = request.value(QStringLiteral("cursor")).toObject();
        const QJsonObject requestedLimits = requestedCursor.value(
            QStringLiteral("limits")).toObject();
        if (!request.value(QStringLiteral("cursor")).isObject()
            || !hasExactKeys(requestedCursor, {
                QStringLiteral("schema_version"), QStringLiteral("reference"),
                QStringLiteral("sha256"), QStringLiteral("bytes"),
                QStringLiteral("media_type"), QStringLiteral("offset"),
                QStringLiteral("page_size"), QStringLiteral("limits"),
                QStringLiteral("binding_identity"), QStringLiteral("identity"),
            })
            || requestedCursor.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("content-reference-cursor/0.1")
            || !hasExactKeys(requestedLimits, {
                QStringLiteral("schema_version"), QStringLiteral("max_item_bytes"),
                QStringLiteral("max_total_bytes"), QStringLiteral("identity"),
            })
            || requestedLimits.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("content-inline-limits/0.1")
            || requestedLimits.value(QStringLiteral("identity")).toString()
                != contentInlineLimitsIdentity(requestedLimits)
            || requestedCursor.value(QStringLiteral("identity")).toString()
                != contentReferenceCursorIdentity(requestedCursor)
            || requestedCursor.value(QStringLiteral("reference")).toString() != reference
            || requestedCursor.value(QStringLiteral("sha256")).toString() != sha256
            || requestedCursor.value(QStringLiteral("bytes"))
                != content.value(QStringLiteral("bytes"))
            || requestedCursor.value(QStringLiteral("media_type")).toString() != mediaType
            || requestedCursor.value(QStringLiteral("binding_identity")).toString() != binding
            || requestedCursor.value(QStringLiteral("limits")) != page.value(QStringLiteral("limits"))
            || requestedCursor.value(QStringLiteral("offset")) != page.value(QStringLiteral("offset"))
            || requestedCursor.value(QStringLiteral("page_size"))
                != page.value(QStringLiteral("page_size"))) {
            return false;
        }
    } else {
        quint64 requestedItem = 0;
        quint64 requestedTotal = 0;
        if (!readUnsignedSafeJsonInteger(
                request.value(QStringLiteral("limit")), &requestedItem)
            || !readUnsignedSafeJsonInteger(
                request.value(QStringLiteral("max_total_inline_bytes")), &requestedTotal)
            || requestedItem == 0 || requestedTotal == 0) {
            return false;
        }
        const quint64 expectedTotal = qMin(requestedTotal, kMaximumInlineTotalBytes);
        const quint64 expectedItem = qMin(qMin(requestedItem, kMaximumInlineItemBytes),
                                          expectedTotal);
        if (offset != 0 || maxItemBytes != expectedItem
                || maxTotalBytes != expectedTotal || pageSize != expectedItem) {
            return false;
        }
    }
    return true;
}

bool AgentRuntimeClient::isValidCompleteCommandArtifact(
    const QJsonObject &result, const QByteArray &content)
{
    constexpr qsizetype kArtifactHeadLimit = 1024 * 1024;
    constexpr qsizetype kArtifactTailLimit = 1024 * 1024;
    const QJsonObject reference = result.value(
        QStringLiteral("content_reference")).toObject();
    quint64 contentBytes = 0;
    quint64 totalBytes = 0;
    quint64 retainedBytes = 0;
    quint64 omittedBytes = 0;
    if (!readUnsignedSafeJsonInteger(reference.value(QStringLiteral("bytes")),
                                     &contentBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("total_bytes")), &totalBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("retained_bytes")), &retainedBytes)
            || !readUnsignedSafeJsonInteger(
                result.value(QStringLiteral("omitted_bytes")), &omittedBytes)
            || !result.value(QStringLiteral("truncated")).isBool()
            || !reference.value(QStringLiteral("sha256")).isString()
            || static_cast<quint64>(content.size()) != contentBytes
            || totalBytes != retainedBytes + omittedBytes
            || result.value(QStringLiteral("truncated")).toBool()
                != (omittedBytes > 0)) {
        return false;
    }
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(
        content, QCryptographicHash::Sha256).toHex());
    if (reference.value(QStringLiteral("sha256")).toString() != sha256
            || reference.value(QStringLiteral("reference")).toString()
                != QStringLiteral("command-output:sha256:") + sha256) {
        return false;
    }
    if (omittedBytes == 0) {
        return static_cast<quint64>(content.size()) == retainedBytes;
    }

    const QByteArray marker = QStringLiteral(
        "\n[Aegisy omitted %1 command output bytes]\n")
                                  .arg(omittedBytes).toUtf8();
    const qsizetype markerOffset = content.indexOf(marker);
    if (markerOffset < 0 || markerOffset != content.lastIndexOf(marker)) return false;
    const qsizetype tailBytes = content.size() - markerOffset - marker.size();
    return markerOffset >= kArtifactHeadLimit - 3
        && markerOffset <= kArtifactHeadLimit
        && tailBytes >= kArtifactTailLimit - 3
        && tailBytes <= kArtifactTailLimit
        && retainedBytes
            == static_cast<quint64>(markerOffset + tailBytes)
        && contentBytes
            == retainedBytes + static_cast<quint64>(marker.size());
}

bool AgentRuntimeClient::isReady() const
{
    return m_ready && !m_reconnectRecoveryPending && isHeartbeatHealthy();
}

bool AgentRuntimeClient::isHeartbeatHealthy() const
{
    return !m_heartbeatNegotiated || m_heartbeatHealthy;
}

bool AgentRuntimeClient::isControlAvailable() const
{
    return m_handshakeComplete
        && !m_reconnectRecoveryPending
        && m_process->state() != QProcess::NotRunning;
}

bool AgentRuntimeClient::isReconnectRecoveryAvailable() const
{
    return m_reconnectRecoveryPending && m_handshakeComplete
        && isHeartbeatHealthy()
        && m_process->state() != QProcess::NotRunning;
}

bool AgentRuntimeClient::isRecoveryMode() const
{
    return m_recoveryMode;
}

AgentRuntimeClient::ReconnectState AgentRuntimeClient::reconnectState() const
{
    return m_reconnectState;
}

int AgentRuntimeClient::reconnectAttempt() const
{
    return m_reconnectAttempt;
}

int AgentRuntimeClient::maximumReconnectAttempts() const
{
    return m_reconnectBackoffMs.size();
}

quint64 AgentRuntimeClient::processGeneration() const
{
    return m_processGeneration;
}

QString AgentRuntimeClient::runtimePath() const
{
    return m_runtimePath;
}

bool AgentRuntimeClient::usesVerifiedUnixSocket() const
{
    // This predicate is retained for the shared QLocalSocket transport path.
    // Platform-specific endpoint creation and peer checks remain guarded below.
    return m_transportMode == TransportMode::VerifiedUnixSocket
        || m_transportMode == TransportMode::VerifiedWindowsNamedPipe;
}

bool AgentRuntimeClient::usesVerifiedWindowsNamedPipe() const
{
    return m_transportMode == TransportMode::VerifiedWindowsNamedPipe;
}

QJsonObject AgentRuntimeClient::expectedTransportSecurity() const
{
    if (usesVerifiedWindowsNamedPipe()) {
        return verifiedWindowsNamedPipeTransportSecurity();
    }
    return usesVerifiedUnixSocket() ? verifiedUnixSocketTransportSecurity()
                                    : stdioTransportSecurity();
}

bool AgentRuntimeClient::prepareUnixSocketEndpoint()
{
#if defined(Q_OS_MACOS)
    if (usesVerifiedWindowsNamedPipe()) return false;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    struct stat baseStatus {};
    const QByteArray encodedBase = QFile::encodeName(base);
    if (base.isEmpty() || ::lstat(encodedBase.constData(), &baseStatus) != 0
        || !S_ISDIR(baseStatus.st_mode) || baseStatus.st_uid != ::geteuid()
        || (baseStatus.st_mode & 0777) != 0700) {
        return false;
    }
    QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    suffix.remove(QLatin1Char('-'));
    suffix = suffix.left(16).toLower();
    const QString directory = QDir(base).absoluteFilePath(
        QStringLiteral("aegisy-agent-%1").arg(suffix));
    const QString socket = QDir(directory).absoluteFilePath(QStringLiteral("agent.sock"));
    struct sockaddr_un address {};
    const QByteArray encodedSocket = QFile::encodeName(socket);
    if (encodedSocket.isEmpty()
        || encodedSocket.size() >= qsizetype(sizeof(address.sun_path))
        || QFileInfo::exists(directory)) {
        return false;
    }
    m_unixSocketDirectory = directory;
    m_unixSocketPath = socket;
    m_unixSocketDirectoryDevice = 0;
    m_unixSocketDirectoryInode = 0;
    m_unixSocketDevice = 0;
    m_unixSocketInode = 0;
    m_unixSocketIdentityCaptured = false;
    m_unixSocketCleanupRetryCount = 0;
    return true;
#elif defined(Q_OS_WIN)
    // QLocalSocket maps this opaque server name to a Windows named pipe.  The
    // Rust listener owns the ACL; Qt verifies the supervised server PID after
    // connecting and never treats the pipe as authenticated.
    if (!usesVerifiedWindowsNamedPipe()) return false;
    QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    suffix.remove(QLatin1Char('-'));
    suffix = suffix.left(24).toLower();
    m_unixSocketDirectory.clear();
    m_unixSocketPath = QStringLiteral("aegisy-agent-%1").arg(suffix);
    m_unixSocketDirectoryDevice = 0;
    m_unixSocketDirectoryInode = 0;
    m_unixSocketDevice = 0;
    m_unixSocketInode = 0;
    m_unixSocketIdentityCaptured = false;
    m_unixSocketCleanupRetryCount = 0;
    return true;
#else
    return false;
#endif
}

void AgentRuntimeClient::cleanupUnixSocketEndpoint()
{
#if defined(Q_OS_MACOS)
    if (!m_unixSocketDirectory.isEmpty()) {
        const QByteArray directory = QFile::encodeName(m_unixSocketDirectory);
        const QByteArray socket = QFile::encodeName(m_unixSocketPath);
        struct stat directoryStatus {};
        const int directoryResult = ::lstat(directory.constData(), &directoryStatus);
        const bool directoryMatches = directoryResult == 0
            && m_unixSocketIdentityCaptured
            && S_ISDIR(directoryStatus.st_mode)
            && directoryStatus.st_uid == ::geteuid()
            && (directoryStatus.st_mode & 0777) == 0700
            && quint64(directoryStatus.st_dev) == m_unixSocketDirectoryDevice
            && quint64(directoryStatus.st_ino) == m_unixSocketDirectoryInode;
        if (directoryMatches) {
            struct stat socketStatus {};
            bool socketRemovedOrMissing = false;
            if (::lstat(socket.constData(), &socketStatus) == 0) {
                if (S_ISSOCK(socketStatus.st_mode)
                    && socketStatus.st_uid == ::geteuid()
                    && (socketStatus.st_mode & 0777) == 0600
                    && quint64(socketStatus.st_dev) == m_unixSocketDevice
                    && quint64(socketStatus.st_ino) == m_unixSocketInode) {
                    socketRemovedOrMissing = ::unlink(socket.constData()) == 0;
                }
            } else if (errno == ENOENT) {
                socketRemovedOrMissing = true;
                const QFileInfoList quarantinedEntries = QDir(m_unixSocketDirectory)
                    .entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System
                                       | QDir::NoDotAndDotDot,
                                   QDir::Name);
                for (const QFileInfo &entry : quarantinedEntries) {
                    if (!entry.fileName().startsWith(
                            QStringLiteral(".aegisy-socket-"))) {
                        continue;
                    }
                    const QByteArray candidate = QFile::encodeName(
                        entry.absoluteFilePath());
                    struct stat candidateStatus {};
                    if (::lstat(candidate.constData(), &candidateStatus) != 0) {
                        if (errno == ENOENT) continue;
                        socketRemovedOrMissing = false;
                        break;
                    }
                    if (!S_ISSOCK(candidateStatus.st_mode)
                        || candidateStatus.st_uid != ::geteuid()
                        || (candidateStatus.st_mode & 0777) != 0600
                        || quint64(candidateStatus.st_dev) != m_unixSocketDevice
                        || quint64(candidateStatus.st_ino) != m_unixSocketInode) {
                        socketRemovedOrMissing = false;
                        break;
                    }
                    if (::unlink(candidate.constData()) != 0 && errno != ENOENT) {
                        socketRemovedOrMissing = false;
                        break;
                    }
                }
            }
            if (socketRemovedOrMissing) {
                if (::rmdir(directory.constData()) != 0) {
                    const int removeError = errno;
                    constexpr int maximumCleanupRetries = 25;
                    if (m_unixSocketCleanupRetryCount < maximumCleanupRetries) {
                        ++m_unixSocketCleanupRetryCount;
                        const QString expectedDirectory = m_unixSocketDirectory;
                        const quint64 expectedDevice = m_unixSocketDirectoryDevice;
                        const quint64 expectedInode = m_unixSocketDirectoryInode;
                        QTimer::singleShot(20, this,
                                           [this, expectedDirectory,
                                            expectedDevice, expectedInode]() {
                            if (m_unixSocketDirectory == expectedDirectory
                                && m_unixSocketDirectoryDevice == expectedDevice
                                && m_unixSocketDirectoryInode == expectedInode) {
                                cleanupUnixSocketEndpoint();
                            }
                        });
                        return;
                    }
                    emit diagnosticMessage(QStringLiteral(
                        "Unix 运行时端点清理失败（unix-socket-cleanup-directory-failed:%1）")
                            .arg(removeError));
                }
            } else {
                emit diagnosticMessage(QStringLiteral(
                    "Unix 运行时端点清理保留身份不符对象（unix-socket-cleanup-mismatch）"));
            }
        } else if (directoryResult == 0) {
            emit diagnosticMessage(QStringLiteral(
                "Unix 运行时端点清理保留身份不符对象（unix-socket-cleanup-mismatch）"));
        }
    }
#endif
    m_unixSocketDirectory.clear();
    m_unixSocketPath.clear();
    m_unixSocketDirectoryDevice = 0;
    m_unixSocketDirectoryInode = 0;
    m_unixSocketDevice = 0;
    m_unixSocketInode = 0;
    m_unixSocketIdentityCaptured = false;
    m_unixSocketCleanupRetryCount = 0;
}

void AgentRuntimeClient::scheduleUnixSocketConnect(quint64 generation)
{
    if (!usesVerifiedUnixSocket() || generation == 0
        || generation != m_processGeneration || !m_startupTimer->isActive()
        || m_process->state() == QProcess::NotRunning) {
        return;
    }
    m_unixSocketConnectGeneration = generation;
    QTimer::singleShot(20, this, [this, generation]() {
        connectUnixSocket(generation);
    });
}

void AgentRuntimeClient::connectUnixSocket(quint64 generation)
{
#if defined(Q_OS_MACOS)
    if (!usesVerifiedUnixSocket() || generation == 0
        || generation != m_processGeneration
        || generation != m_unixSocketConnectGeneration
        || !m_startupTimer->isActive()
        || m_process->state() == QProcess::NotRunning
        || (m_localSocket
            && (m_localSocket->state() == QLocalSocket::ConnectedState
                || m_localSocket->state() == QLocalSocket::ConnectingState))) {
        return;
    }
    UnixEndpointIdentity identity;
    const UnixEndpointState state = inspectUnixEndpoint(
        m_unixSocketDirectory, m_unixSocketPath, &identity);
    if (state == UnixEndpointState::NotReady) {
        scheduleUnixSocketConnect(generation);
        return;
    }
    if (state == UnixEndpointState::Invalid) {
        const QString detail = QStringLiteral(
            "Unix 运行时端点校验失败（unix-socket-endpoint-invalid）");
        suppressAutomaticReconnect();
        clearNegotiationState();
        failPending(detail);
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        terminateOwnedProcessGeneration(generation);
        return;
    }
    m_unixSocketDirectoryDevice = identity.directoryDevice;
    m_unixSocketDirectoryInode = identity.directoryInode;
    m_unixSocketDevice = identity.socketDevice;
    m_unixSocketInode = identity.socketInode;
    m_unixSocketIdentityCaptured = true;
#elif defined(Q_OS_WIN)
    if (!usesVerifiedWindowsNamedPipe() || generation == 0
        || generation != m_processGeneration
        || generation != m_unixSocketConnectGeneration
        || !m_startupTimer->isActive()
        || m_process->state() == QProcess::NotRunning
        || (m_localSocket
            && (m_localSocket->state() == QLocalSocket::ConnectedState
                || m_localSocket->state() == QLocalSocket::ConnectingState))
        || m_unixSocketPath.isEmpty()) {
        return;
    }
#else
    Q_UNUSED(generation)
    return;
#endif

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    retireLocalSocket();
    if (++m_localSocketAttemptEpoch == 0) ++m_localSocketAttemptEpoch;
    auto *socket = new QLocalSocket(this);
    m_localSocket = socket;
    configureLocalSocket(socket, generation, m_localSocketAttemptEpoch);
    socket->connectToServer(m_unixSocketPath, QIODevice::ReadWrite);
#endif
}

void AgentRuntimeClient::configureLocalSocket(QLocalSocket *socket,
                                              quint64 generation,
                                              quint64 attemptEpoch)
{
    if (!socket || generation == 0 || attemptEpoch == 0) return;
    const QPointer<QLocalSocket> guardedSocket(socket);
    connect(socket, &QLocalSocket::readyRead, this,
            [this, guardedSocket, generation, attemptEpoch]() {
        if (!guardedSocket
            || !isCurrentLocalSocket(guardedSocket.data(), generation,
                                     attemptEpoch)) {
            return;
        }
        processSocketInput(guardedSocket.data(), generation, attemptEpoch);
    });
    connect(socket, &QLocalSocket::connected, this,
            [this, guardedSocket, generation, attemptEpoch]() {
        if (!guardedSocket
            || !isCurrentLocalSocket(guardedSocket.data(), generation,
                                     attemptEpoch)) {
            return;
        }
        if (!usesVerifiedUnixSocket()
            || m_unixSocketConnectGeneration != generation
            || m_process->state() == QProcess::NotRunning) {
            retireLocalSocket();
            return;
        }
        if (!verifyUnixSocketPeer(guardedSocket.data())) {
            const QString detail = usesVerifiedWindowsNamedPipe()
                ? QStringLiteral(
                    "Windows named pipe 运行时对端校验失败（named-pipe-peer-mismatch）")
                : QStringLiteral(
                    "Unix 运行时对端校验失败（unix-socket-peer-mismatch）");
            suppressAutomaticReconnect();
            clearNegotiationState();
            failPending(detail);
            emit connectionStateChanged(false, detail);
            retireLocalSocket();
            terminateOwnedProcessGeneration(generation);
            return;
        }
        m_unixSocketPeerVerifiedGeneration = generation;
        m_localSocketPeerVerifiedAttemptEpoch = attemptEpoch;
        sendInitializeRequest();
    });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket, &QLocalSocket::errorOccurred, this,
            [this, guardedSocket, generation,
             attemptEpoch](QLocalSocket::LocalSocketError error) {
#else
    connect(socket,
            QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error), this,
            [this, guardedSocket, generation,
             attemptEpoch](QLocalSocket::LocalSocketError error) {
#endif
        if (!guardedSocket
            || !isCurrentLocalSocket(guardedSocket.data(), generation,
                                     attemptEpoch)) {
            return;
        }
        if (!usesVerifiedUnixSocket()
            || m_unixSocketConnectGeneration != generation
            || m_stopping || m_autoReconnectSuppressed) {
            return;
        }
        if ((error == QLocalSocket::ServerNotFoundError
             || error == QLocalSocket::ConnectionRefusedError)
            && m_startupTimer->isActive()
            && m_process->state() != QProcess::NotRunning) {
            retireLocalSocket();
            scheduleUnixSocketConnect(generation);
            return;
        }
        handleUnixSocketDisconnected(guardedSocket.data(), generation,
                                     attemptEpoch);
    });
    connect(socket, &QLocalSocket::disconnected, this,
            [this, guardedSocket, generation, attemptEpoch]() {
        if (!guardedSocket
            || !isCurrentLocalSocket(guardedSocket.data(), generation,
                                     attemptEpoch)) {
            return;
        }
        handleUnixSocketDisconnected(guardedSocket.data(), generation,
                                     attemptEpoch);
    });
}

bool AgentRuntimeClient::isCurrentLocalSocket(const QLocalSocket *socket,
                                              quint64 generation,
                                              quint64 attemptEpoch) const
{
    return socket && socket == m_localSocket && usesVerifiedUnixSocket()
        && generation != 0 && generation == m_processGeneration
        && generation == m_unixSocketConnectGeneration
        && attemptEpoch != 0 && attemptEpoch == m_localSocketAttemptEpoch;
}

bool AgentRuntimeClient::verifyUnixSocketPeer(const QLocalSocket *socket) const
{
#if defined(Q_OS_MACOS)
    if (!usesVerifiedUnixSocket() || usesVerifiedWindowsNamedPipe()
        || !socket || socket->state() != QLocalSocket::ConnectedState
        || m_process->state() == QProcess::NotRunning) {
        return false;
    }
    const qintptr descriptor = socket->socketDescriptor();
    if (descriptor < 0) return false;
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(int(descriptor), &uid, &gid) != 0 || uid != ::geteuid()) {
        return false;
    }
    pid_t pid = 0;
    socklen_t length = sizeof(pid);
    if (::getsockopt(int(descriptor), SOL_LOCAL, LOCAL_PEERPID,
                     &pid, &length) != 0
        || length != sizeof(pid)
        || pid != pid_t(m_process->processId())) {
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    if (!usesVerifiedWindowsNamedPipe()
        || !socket || socket->state() != QLocalSocket::ConnectedState
        || m_process->state() == QProcess::NotRunning) {
        return false;
    }
    // Qt's Windows QLocalSocket backend stores the named-pipe HANDLE in its
    // native descriptor; GetNamedPipeServerProcessId is therefore valid here.
    const qintptr descriptor = socket->socketDescriptor();
    if (descriptor == qintptr(-1) || descriptor == 0) return false;
    const HANDLE pipe = reinterpret_cast<HANDLE>(descriptor);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return false;
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid)
        || serverPid == 0
        || serverPid != static_cast<ULONG>(m_process->processId())) {
        return false;
    }
    return true;
#else
    return false;
#endif
}

void AgentRuntimeClient::handleUnixSocketDisconnected(QLocalSocket *socket,
                                                      quint64 generation,
                                                      quint64 attemptEpoch)
{
    if (!isCurrentLocalSocket(socket, generation, attemptEpoch)) return;
    m_unixSocketPeerVerifiedGeneration = 0;
    m_localSocketPeerVerifiedAttemptEpoch = 0;
    if (!usesVerifiedUnixSocket() || m_stopping
        || m_autoReconnectSuppressed || m_processTerminationPending
        || m_process->state() == QProcess::NotRunning
        || m_unixSocketConnectGeneration != generation) {
        return;
    }
    if (!m_handshakeComplete && m_initializeRequestId.isEmpty()
        && m_startupTimer->isActive() && !m_autoReconnectSuppressed) {
        retireLocalSocket();
        scheduleUnixSocketConnect(generation);
        return;
    }
    if (m_unixSocketDisconnectGeneration == generation) return;
    m_unixSocketDisconnectGeneration = generation;
    const QString detail = usesVerifiedWindowsNamedPipe()
        ? QStringLiteral("Windows named pipe 运行时连接已断开")
        : QStringLiteral("Unix 运行时连接已断开");
    retireLocalSocket();
    clearNegotiationState();
    failPending(detail);
    emit connectionStateChanged(false, detail);
    terminateOwnedProcessGeneration(generation);
}

void AgentRuntimeClient::retireLocalSocket()
{
    QLocalSocket *socket = std::exchange(m_localSocket, nullptr);
    if (!socket) return;
    m_unixSocketPeerVerifiedGeneration = 0;
    m_localSocketPeerVerifiedAttemptEpoch = 0;
    QObject::disconnect(socket, nullptr, this, nullptr);
    if (socket->state() != QLocalSocket::UnconnectedState) socket->abort();
    socket->deleteLater();
}

void AgentRuntimeClient::terminateOwnedProcessGeneration(quint64 generation)
{
    if (generation == 0 || generation != m_processGeneration) {
        return;
    }
    m_unixSocketPeerVerifiedGeneration = 0;
    m_localSocketPeerVerifiedAttemptEpoch = 0;
    m_ownedTerminationGeneration = generation;
    if (m_process->state() == QProcess::NotRunning) {
        cleanupUnixSocketEndpoint();
        return;
    }
    closeTransportWrite();
    m_process->terminate();
    QTimer::singleShot(kReconnectTerminationGraceMs, this, [this, generation]() {
        if (generation == m_processGeneration
            && m_ownedTerminationGeneration == generation
            && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
        }
    });
}

void AgentRuntimeClient::closeTransportWrite()
{
    if (usesVerifiedUnixSocket()) {
        if (m_localSocket
            && m_localSocket->state() != QLocalSocket::UnconnectedState) {
            m_localSocket->disconnectFromServer();
        }
        return;
    }
    if (m_process->state() != QProcess::NotRunning) m_process->closeWriteChannel();
}

QProcessEnvironment AgentRuntimeClient::sanitizedSidecarEnvironment(
    const QProcessEnvironment &environment)
{
    QProcessEnvironment sanitized;
    for (const QString &name : environment.keys()) {
        if (!sensitiveSidecarEnvironmentName(name)) {
            sanitized.insert(name, environment.value(name));
        }
    }
    return sanitized;
}

void AgentRuntimeClient::start()
{
    if (m_process->state() != QProcess::NotRunning
        || m_reconnectState == ReconnectState::Waiting
        || m_reconnectState == ReconnectState::Restarting) {
        return;
    }
    m_reconnectTimer->stop();
    m_reconnectStabilityTimer->stop();
    m_reconnectStabilityGeneration = 0;
    m_reconnectAttempt = 0;
    m_reconnectCycleActive = false;
    m_autoReconnectSuppressed = false;
    m_stopping = false;
    setReconnectState(ReconnectState::Idle, 0,
                      QStringLiteral("正在连接本地运行时…"));
    launchProcess(false);
}

void AgentRuntimeClient::setEmergencyDisabled(bool disabled)
{
    if (m_emergencyDisabled == disabled) return;
    m_emergencyDisabled = disabled;
    m_reconnectTimer->stop();
    if (m_reconnectState == ReconnectState::Waiting) {
        setReconnectState(ReconnectState::Idle, 0,
                          QStringLiteral("正在应用工作台应急策略"));
    }
    if (m_process->state() == QProcess::NotRunning) {
        QTimer::singleShot(0, this, &AgentRuntimeClient::start);
        return;
    }
    if (m_processStartedEmergency == disabled) return;
    m_policyRestartPending = true;
    m_stopping = true;
    m_autoReconnectSuppressed = true;
    setReconnectState(ReconnectState::Idle, 0,
                      QStringLiteral("正在应用工作台应急策略"));
    failPending(disabled
        ? QStringLiteral("服务器应急策略已暂停新的工作台操作")
        : QStringLiteral("工作台应急策略已更新，正在重新连接运行时"));
    const quint64 generation = m_processGeneration;
    if (!m_handshakeComplete || sendRequest(QStringLiteral("shutdown")).isEmpty()) {
        m_process->terminate();
    }
    QTimer::singleShot(kReconnectTerminationGraceMs, this, [this, generation]() {
        if (generation == m_processGeneration && m_policyRestartPending
            && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
        }
    });
}

bool AgentRuntimeClient::launchProcess(bool reconnectAttempt)
{
    if (m_stopping || m_autoReconnectSuppressed
        || m_process->state() != QProcess::NotRunning) {
        return false;
    }
    m_runtimePath = locateRuntime();
    if (m_runtimePath.isEmpty()) {
        const QString detail = QStringLiteral("未找到 aegisy-agentd，请先构建 agent-runtime");
        m_autoReconnectSuppressed = true;
        if (reconnectAttempt) {
            setReconnectState(ReconnectState::Exhausted, 0, detail);
        }
        emit connectionStateChanged(false, detail);
        return false;
    }

    // Packaged bundles place a content-addressed manifest beside the sidecar.
    // Developer builds may omit it, but a present manifest is always required
    // to verify before any sidecar process is started.
    const QString manifestPath = QFileInfo(m_runtimePath).absolutePath()
        + QStringLiteral("/aegisy-agentd.manifest.json");
    if (QFileInfo::exists(manifestPath)) {
        const ArtifactManifest::VerificationResult verification =
            ArtifactManifest::verifyFile(manifestPath, m_runtimePath);
        if (!verification.ok) {
            const QString detail = QStringLiteral("运行时完整性校验失败：%1")
                .arg(verification.reason);
            m_autoReconnectSuppressed = true;
            if (reconnectAttempt) setReconnectState(ReconnectState::Exhausted, 0, detail);
            emit connectionStateChanged(false, detail);
            return false;
        }
    }

    clearNegotiationState();
    m_processTerminationPending = false;
    if (++m_processGeneration == 0) ++m_processGeneration;
    m_unixSocketConnectGeneration = 0;
    m_unixSocketDisconnectGeneration = 0;
    m_unixSocketPeerVerifiedGeneration = 0;
    m_localSocketPeerVerifiedAttemptEpoch = 0;
    m_ownedTerminationGeneration = 0;
    retireLocalSocket();
    cleanupUnixSocketEndpoint();
    if (usesVerifiedUnixSocket() && !m_unixSocketDirectory.isEmpty()) {
        const QString detail = QStringLiteral(
            "旧 Unix 运行时端点仍在安全清理中（unix-socket-cleanup-pending）");
        m_autoReconnectSuppressed = true;
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        return false;
    }
    if (usesVerifiedUnixSocket() && !prepareUnixSocketEndpoint()) {
        const QString detail = QStringLiteral(
            "无法创建安全的 Unix 运行时端点（unix-socket-path-unavailable）");
        m_autoReconnectSuppressed = true;
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        return false;
    }
    m_discardProcessOutput = false;
    m_stdoutBuffer.clear();
    m_pendingRequests.clear();
    const QString connectingDetail = reconnectAttempt
        ? QStringLiteral("正在执行第 %1/%2 次运行时重连")
              .arg(m_reconnectAttempt)
              .arg(maximumReconnectAttempts())
        : QStringLiteral("正在连接本地运行时…");
    emit connectionStateChanged(false, connectingDetail);
    QProcessEnvironment environment = sanitizedSidecarEnvironment(
        QProcessEnvironment::systemEnvironment());
    environment.remove(QStringLiteral("AEGISY_AGENTD_UNIX_SOCKET_DIR"));
    environment.remove(QStringLiteral("AEGISY_AGENTD_NAMED_PIPE"));
    if (usesVerifiedWindowsNamedPipe()) {
        environment.insert(QStringLiteral("AEGISY_AGENTD_NAMED_PIPE"),
                           m_unixSocketPath);
    } else if (usesVerifiedUnixSocket()) {
        environment.insert(QStringLiteral("AEGISY_AGENTD_UNIX_SOCKET_DIR"),
                           m_unixSocketDirectory);
    }
    if (m_emergencyDisabled) {
        environment.insert(QStringLiteral("AEGISY_WORKBENCH_EMERGENCY_DISABLED"),
                           QStringLiteral("1"));
    } else {
        environment.remove(QStringLiteral("AEGISY_WORKBENCH_EMERGENCY_DISABLED"));
    }
    if (environment.value(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT")).isEmpty()) {
        const QString dataRoot = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .absoluteFilePath(QStringLiteral("workbench"));
        if (!QDir().mkpath(dataRoot)) {
            const QString detail = QStringLiteral("无法创建工作台数据目录");
            m_autoReconnectSuppressed = true;
            setReconnectState(ReconnectState::Exhausted, 0, detail);
            emit connectionStateChanged(false, detail);
            return false;
        }
        environment.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"), dataRoot);
    }
    m_process->setProgram(m_runtimePath);
    m_process->setArguments({});
    m_process->setProcessEnvironment(environment);
    m_processStartedEmergency = m_emergencyDisabled;
    m_process->start();
    if (!m_process->waitForStarted(1000)) {
        if (!m_stopping && !m_autoReconnectSuppressed
            && m_reconnectState != ReconnectState::Waiting
            && m_reconnectState != ReconnectState::Exhausted) {
            handleRetryableProcessFailure(
                QStringLiteral("运行时启动失败：%1").arg(m_process->errorString()));
        }
        return false;
    }
    m_startupTimer->start(kStartupTimeoutMs);
    m_startupGeneration = m_processGeneration;
    if (usesVerifiedUnixSocket()) {
        scheduleUnixSocketConnect(m_processGeneration);
    } else {
        sendInitializeRequest();
    }
    return !m_initializeRequestId.isEmpty() || usesVerifiedUnixSocket();
}

void AgentRuntimeClient::sendInitializeRequest()
{
    if (m_process->state() == QProcess::NotRunning
        || (usesVerifiedUnixSocket()
            && (!m_localSocket
                || m_localSocket->state() != QLocalSocket::ConnectedState
                || m_unixSocketPeerVerifiedGeneration != m_processGeneration
                || m_localSocketPeerVerifiedAttemptEpoch
                    != m_localSocketAttemptEpoch))
        || !m_initializeRequestId.isEmpty()) {
        return;
    }
    const QJsonObject params{
        {QStringLiteral("protocol"), QJsonObject{
            {QStringLiteral("minimum"), kMinimumProtocolVersion},
            {QStringLiteral("maximum"), kMaximumProtocolVersion},
            {QStringLiteral("preferred"), kPreferredProtocolVersion},
        }},
        {QStringLiteral("client"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("aegisy-client")},
            {QStringLiteral("version"), QStringLiteral(AEGISY_APP_VERSION)},
        }},
        {QStringLiteral("platform"), localPlatform()},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("stable"), QJsonArray::fromStringList(declaredCapabilities())},
            {QStringLiteral("experimental"), QJsonArray{}},
        }},
        {QStringLiteral("limits"), QJsonObject{
            {QStringLiteral("max_frame_bytes"), kMaximumFrameBytes},
        }},
        {QStringLiteral("transport_security"), expectedTransportSecurity()},
    };
    m_initializeRequestId = sendRequest(QStringLiteral("initialize"), params);
    m_initializeGeneration = m_processGeneration;
    if (m_initializeRequestId.isEmpty()) {
        handleRetryableProcessFailure(QStringLiteral("无法发送运行时握手请求"));
    }
}

void AgentRuntimeClient::stop()
{
    const bool hadReconnect = m_reconnectState != ReconnectState::Idle;
    m_reconnectTimer->stop();
    m_reconnectStabilityTimer->stop();
    m_reconnectStabilityGeneration = 0;
    m_autoReconnectSuppressed = true;
    m_reconnectCycleActive = false;
    m_reconnectScheduledGeneration = 0;
    m_processTerminationPending = false;
    m_stopping = true;
    setReconnectState(ReconnectState::Idle, 0, QStringLiteral("运行时停止中"));
    if (m_process->state() == QProcess::NotRunning) {
        m_stopping = false;
        if (hadReconnect) {
            emit connectionStateChanged(false, QStringLiteral("运行时已停止"));
        }
        return;
    }
    if (m_handshakeComplete
        && m_process->state() != QProcess::NotRunning) {
        if (sendRequest(QStringLiteral("shutdown")).isEmpty()) {
            m_process->terminate();
        }
    } else {
        m_process->terminate();
    }
    const quint64 generation = m_processGeneration;
    QTimer::singleShot(kReconnectTerminationGraceMs, this, [this, generation]() {
        if (generation == m_processGeneration && m_stopping
            && m_process->state() != QProcess::NotRunning) {
            m_process->kill();
        }
    });
}

bool AgentRuntimeClient::completeReconnectRecovery(quint64 generation,
                                                   bool success,
                                                   const QString &detail)
{
    if (!m_reconnectRecoveryPending || generation == 0
        || generation != m_processGeneration
        || !m_handshakeComplete
        || m_process->state() == QProcess::NotRunning) {
        return false;
    }

    m_reconnectRecoveryPending = false;
    if (!success) {
        m_reconnectInitializeResult = {};
        m_autoReconnectSuppressed = true;
        const QString failureDetail = detail.isEmpty()
            ? QStringLiteral("运行时重连后的工作区恢复未完成") : detail;
        setReconnectState(ReconnectState::Exhausted, 0, failureDetail);
        emit connectionStateChanged(false, failureDetail);
        return true;
    }

    const QJsonObject result = m_reconnectInitializeResult;
    m_reconnectInitializeResult = {};
    m_reconnectCycleActive = false;
    const QString readyDetail = detail.isEmpty()
        ? QStringLiteral("运行时连接与工作区状态已恢复") : detail;
    setReconnectState(ReconnectState::Idle, 0, readyDetail);
    if (m_reconnectAttempt > 0) {
        m_reconnectStabilityGeneration = m_processGeneration;
        m_reconnectStabilityTimer->start(kReconnectStabilityWindowMs);
    }
    emit runtimeInitialized(result);
    emit connectionStateChanged(m_ready && isHeartbeatHealthy(), readyDetail);
    if (m_ready && isHeartbeatHealthy()) {
        if (containsCapability(m_negotiatedStableCapabilities, "runtime.health")) {
            runtimeHealth();
        }
        if (containsCapability(m_negotiatedStableCapabilities, "runtime.degradations")) {
            runtimeDegradations();
        }
        if (containsCapability(m_negotiatedStableCapabilities,
                               "model.catalog.read-only")) {
            modelCatalog();
        }
        if (containsCapability(m_negotiatedStableCapabilities,
                               "model.catalog.cache.read-only")) {
            modelCatalogCache();
        }
        if (containsCapability(m_negotiatedStableCapabilities,
                               "model.catalog.refresh.status.read-only")) {
            modelCatalogRefreshStatus();
        }
        if (containsCapability(m_negotiatedStableCapabilities,
                               "model.profile.read-only")) {
            listModelProfiles();
        }
        if (m_recoveryMode
            && containsCapability(m_negotiatedStableCapabilities,
                                  "runtime.recovery.status")) {
            runtimeRecoveryStatus();
        } else if (!m_recoveryMode
                   && containsCapability(m_negotiatedStableCapabilities,
                                         "runtime.projection-recovery.status")) {
            projectionRecoveryStatus();
        }
    }
    return true;
}

QString AgentRuntimeClient::runtimeHealth()
{
    return sendRequest(QStringLiteral("runtime/health"));
}

QString AgentRuntimeClient::runtimeDegradations()
{
    const QString requestId = sendRequest(QStringLiteral("runtime/degradations"));
    if (!requestId.isEmpty()) emit runtimeDegradationRequestCreated(requestId);
    return requestId;
}

QString AgentRuntimeClient::modelCatalog()
{
    return sendRequest(QStringLiteral("model/catalog"));
}

QString AgentRuntimeClient::modelCatalogCache()
{
    return sendRequest(QStringLiteral("model/catalog-cache"));
}

QString AgentRuntimeClient::modelCatalogRefreshStatus()
{
    return sendRequest(QStringLiteral("model/catalog-refresh-status"));
}

QString AgentRuntimeClient::checkModelCapabilities(const QString &modelId,
                                                   const QJsonObject &requirements)
{
    return sendRequest(QStringLiteral("model/capability-check"), {
        {QStringLiteral("model_id"), modelId},
        {QStringLiteral("requirements"), requirements},
    });
}

QString AgentRuntimeClient::listModelProfiles(const QString &projectId)
{
    QJsonObject params;
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    return sendRequest(QStringLiteral("model/profile/list"), params);
}

QString AgentRuntimeClient::readModelProfile(const QString &profileId)
{
    return sendRequest(QStringLiteral("model/profile/read"), {
        {QStringLiteral("profile_id"), profileId},
    });
}

QString AgentRuntimeClient::restartRuntime()
{
    return sendRequest(QStringLiteral("runtime/restart"));
}

QString AgentRuntimeClient::listProjects(int limit)
{
    return sendRequest(QStringLiteral("project/list"), {
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::updateProjectNavigation(const QString &projectId, bool pinned)
{
    return sendRequest(QStringLiteral("project/navigation"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("pinned"), pinned},
    });
}

QString AgentRuntimeClient::openProject(const QString &root)
{
    return sendRequest(QStringLiteral("project/open"),
                       {{QStringLiteral("root"), root}});
}

QString AgentRuntimeClient::relinkProject(const QString &projectId, const QString &rootId,
                                          const QString &root,
                                          const QString &expectedRootIdentity)
{
    return sendRequest(QStringLiteral("project/relink"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("root"), root},
        {QStringLiteral("expected_root_identity"), expectedRootIdentity},
    });
}

QString AgentRuntimeClient::previewProjectTrustReview(const QString &root)
{
    return sendRequest(QStringLiteral("project/trust-review"),
                       {{QStringLiteral("root"), root}});
}

QString AgentRuntimeClient::acknowledgeProjectTrustReview(
    const QString &projectId, const QString &rootId, const QString &rootIdentity,
    const QString &reviewId)
{
    return sendRequest(QStringLiteral("project/trust-acknowledge"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("root_identity"), rootIdentity},
        {QStringLiteral("review_id"), reviewId},
    });
}

QString AgentRuntimeClient::listProjectRoots(const QString &projectId)
{
    return sendRequest(QStringLiteral("project/root-list"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::addProjectRoot(const QString &projectId,
                                           const QString &root,
                                           const QString &access)
{
    return sendRequest(QStringLiteral("project/root-add"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root"), root},
        {QStringLiteral("access"), access},
    });
}

QString AgentRuntimeClient::removeProjectRoot(const QString &projectId,
                                              const QString &rootId)
{
    return sendRequest(QStringLiteral("project/root-remove"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
    });
}

QString AgentRuntimeClient::startSession(const QString &mode, const QString &projectId)
{
    QJsonObject params{{QStringLiteral("mode"), mode},
                       {QStringLiteral("title"), mode == QStringLiteral("work")
                            ? QStringLiteral("Project work") : QStringLiteral("New chat")}};
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    return sendRequest(QStringLiteral("session/start"), params);
}

QString AgentRuntimeClient::resumeSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/resume"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::forkSession(const QString &sessionId,
                                        const QString &lastTurnId,
                                        const QString &title)
{
    QJsonObject params{{QStringLiteral("session_id"), sessionId}};
    if (!lastTurnId.isEmpty()) params.insert(QStringLiteral("last_turn_id"), lastTurnId);
    if (!title.isEmpty()) params.insert(QStringLiteral("title"), title);
    return sendRequest(QStringLiteral("session/fork"), params);
}

QString AgentRuntimeClient::listSessions(const QString &projectId, const QString &mode,
                                         bool includeArchived, int limit)
{
    QJsonObject params{
        {QStringLiteral("include_archived"), includeArchived},
        {QStringLiteral("limit"), qBound(1, limit, 200)},
    };
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    if (!mode.isEmpty()) params.insert(QStringLiteral("mode"), mode);
    return sendRequest(QStringLiteral("session/list"), params);
}

QString AgentRuntimeClient::searchSessions(const QString &query, const QString &projectId,
                                           bool includeArchived, int limit)
{
    QJsonObject params{
        {QStringLiteral("text"), query},
        {QStringLiteral("include_archived"), includeArchived},
        {QStringLiteral("limit"), qBound(1, limit, 100)},
    };
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    return sendRequest(QStringLiteral("session/search"), params);
}

QString AgentRuntimeClient::renameSession(const QString &sessionId, const QString &title)
{
    return sendRequest(QStringLiteral("session/title"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("title"), title},
    });
}

QString AgentRuntimeClient::archiveSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/archive"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::unarchiveSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/unarchive"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::previewSessionDeletion(const QString &sessionId,
                                                    const QString &scope)
{
    return sendRequest(QStringLiteral("session/delete/preview"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("scope"), scope},
    });
}

QString AgentRuntimeClient::scheduleSessionDeletion(const QString &sessionId,
                                                     const QString &scope,
                                                     const QJsonObject &planHash,
                                                     qint64 undoWindowMs)
{
    return sendRequest(QStringLiteral("session/delete/schedule"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("scope"), scope},
        {QStringLiteral("plan_hash"), planHash},
        {QStringLiteral("undo_window_ms"), undoWindowMs},
    });
}

QString AgentRuntimeClient::undoSessionDeletion(const QString &deletionId)
{
    return sendRequest(QStringLiteral("session/delete/undo"), {
        {QStringLiteral("deletion_id"), deletionId},
    });
}

QString AgentRuntimeClient::sessionDeletionStatus(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/deletion/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::previewPortableSessionExport(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/export/preview"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::exportPortableSession(const QString &sessionId,
                                                  const QJsonObject &packageHash)
{
    return sendRequest(QStringLiteral("session/export"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("package_hash"), packageHash},
    });
}

QString AgentRuntimeClient::previewPortableSessionImport(
    const QJsonObject &package, const QString &targetProjectId,
    const QString &collisionStrategy)
{
    QJsonObject params{
        {QStringLiteral("package"), package},
        {QStringLiteral("collision_strategy"), collisionStrategy},
    };
    if (!targetProjectId.isEmpty()) {
        params.insert(QStringLiteral("target_project_id"), targetProjectId);
    }
    return sendRequest(QStringLiteral("session/import/preview"), params);
}

QString AgentRuntimeClient::importPortableSession(const QJsonObject &package,
                                                  const QString &targetProjectId,
                                                  const QString &collisionStrategy)
{
    QJsonObject params{
        {QStringLiteral("package"), package},
        {QStringLiteral("collision_strategy"), collisionStrategy},
    };
    if (!targetProjectId.isEmpty()) {
        params.insert(QStringLiteral("target_project_id"), targetProjectId);
    }
    return sendRequest(QStringLiteral("session/import"), params);
}

QString AgentRuntimeClient::readRetentionPolicy(const QString &scopeKind,
                                                const QString &scopeId)
{
    return sendRequest(QStringLiteral("retention/policy/read"), {
        {QStringLiteral("scope_kind"), scopeKind},
        {QStringLiteral("scope_id"), scopeId},
    });
}

QString AgentRuntimeClient::setRetentionPolicy(const QJsonObject &policy)
{
    return sendRequest(QStringLiteral("retention/policy/set"), policy);
}

QString AgentRuntimeClient::removeRetentionPolicy(const QString &scopeKind,
                                                   const QString &scopeId)
{
    return sendRequest(QStringLiteral("retention/policy/remove"), {
        {QStringLiteral("scope_kind"), scopeKind},
        {QStringLiteral("scope_id"), scopeId},
    });
}

QString AgentRuntimeClient::runRetentionMaintenance()
{
    return sendRequest(QStringLiteral("retention/maintenance/run"));
}

QString AgentRuntimeClient::startTurn(const QString &sessionId, const QString &input,
                                      const QJsonArray &context,
                                      const QString &pinnedContextSetIdentity,
                                      const QStringList &pinnedContextIds,
                                      const QString &idempotencyKey)
{
    if (!isBoundedTimelineIdentity(QJsonValue(sessionId))
            || input.trimmed().isEmpty()
            || !isBoundedTimelineIdentity(QJsonValue(idempotencyKey))
            || m_processGeneration == 0
            || m_processGeneration
                > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return {};
    }
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("input"), input},
        {QStringLiteral("context"), context},
        {QStringLiteral("idempotency_key"), idempotencyKey},
        {QStringLiteral("generation"), static_cast<double>(m_processGeneration)},
    };
    if (!pinnedContextIds.isEmpty()) {
        params.insert(QStringLiteral("pinned_context_set_identity"),
                      pinnedContextSetIdentity);
        params.insert(QStringLiteral("pinned_context_ids"),
                      QJsonArray::fromStringList(pinnedContextIds));
    }
    return sendRequest(QStringLiteral("turn/start"), params,
                       TurnStartValidation{
                           sessionId,
                           idempotencyKey,
                           m_processGeneration,
                       });
}

QString AgentRuntimeClient::inspectTurnContext(const QString &sessionId,
                                                const QJsonArray &context,
                                                const QString &pinnedContextSetIdentity,
                                                const QStringList &pinnedContextIds)
{
    if (sessionId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("context"), context},
    };
    if (!pinnedContextIds.isEmpty()) {
        params.insert(QStringLiteral("pinned_context_set_identity"),
                      pinnedContextSetIdentity);
        params.insert(QStringLiteral("pinned_context_ids"),
                      QJsonArray::fromStringList(pinnedContextIds));
    }
    return sendRequest(QStringLiteral("turn/context/inspect"), params);
}

QString AgentRuntimeClient::listPinnedContext(const QString &projectId)
{
    if (projectId.isEmpty()) return {};
    return sendRequest(QStringLiteral("workspace/pinned-context/list"), {
        {QStringLiteral("project_id"), projectId},
    });
}

QString AgentRuntimeClient::savePinnedContext(const QString &projectId,
                                               const QJsonArray &items,
                                               const QString &expectedSetIdentity)
{
    if (projectId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("set"), QJsonObject{
            {QStringLiteral("schema_version"), QStringLiteral("pinned-context/0.1")},
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("items"), items},
        }},
    };
    if (!expectedSetIdentity.isEmpty()) {
        params.insert(QStringLiteral("expected_set_identity"), expectedSetIdentity);
    }
    return sendRequest(QStringLiteral("workspace/pinned-context/save"), params);
}

QString AgentRuntimeClient::removePinnedContext(const QString &projectId,
                                                 const QString &itemId,
                                                 const QString &expectedSetIdentity)
{
    if (projectId.isEmpty() || itemId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("item_id"), itemId},
    };
    if (!expectedSetIdentity.isEmpty()) {
        params.insert(QStringLiteral("expected_set_identity"), expectedSetIdentity);
    }
    return sendRequest(QStringLiteral("workspace/pinned-context/remove"), params);
}

QString AgentRuntimeClient::importPinnedImage(const QString &sessionId,
                                               const QString &rootId,
                                               const QString &label,
                                               const QString &mediaType,
                                               const QByteArray &content)
{
    if (sessionId.isEmpty() || label.isEmpty() || mediaType.isEmpty() || content.isEmpty()) {
        return {};
    }
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("label"), label},
        {QStringLiteral("media_type"), mediaType},
        {QStringLiteral("data_base64"), QString::fromLatin1(content.toBase64())},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/image/import-user"), params);
}

QString AgentRuntimeClient::readPinnedImage(const QString &sessionId,
                                             const QString &reference)
{
    if (sessionId.isEmpty() || reference.isEmpty()) return {};
    return sendRequest(QStringLiteral("workspace/image/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("reference"), reference},
    });
}

QString AgentRuntimeClient::cancelTurn(const QString &sessionId, const QString &turnId)
{
    if (sessionId.isEmpty() || turnId.isEmpty()) return {};
    return sendRequest(QStringLiteral("turn/cancel"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("turn_id"), turnId},
    });
}

QString AgentRuntimeClient::readSession(const QString &sessionId, const QString &cursor,
                                        int limit)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("limit"), qBound(1, limit, 200)},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("session/read"), params);
}

QString AgentRuntimeClient::syncTimeline(const QString &sessionId,
                                         quint64 afterSequence,
                                         const QString &afterEventId,
                                         const QJsonObject &watermark,
                                         int limit)
{
    const QString method = QStringLiteral("timeline/sync");
    const QJsonObject after{
        {QStringLiteral("sequence"), static_cast<double>(afterSequence)},
        {QStringLiteral("event_id"), afterSequence == 0
             ? QJsonValue(QJsonValue::Null) : QJsonValue(afterEventId)},
    };
    const bool validWatermark = watermark.isEmpty()
        || (isValidTimelineAnchor(watermark)
            && watermark.value(QStringLiteral("sequence")).toDouble()
                >= static_cast<double>(afterSequence)
            && (watermark.value(QStringLiteral("sequence")).toDouble()
                    != static_cast<double>(afterSequence)
                || watermark.value(QStringLiteral("event_id"))
                    == after.value(QStringLiteral("event_id"))));
    if (sessionId.isEmpty()
        || !isBoundedTimelineIdentity(sessionId)
        || afterSequence > static_cast<quint64>(kMaximumSafeJsonInteger)
        || (afterSequence == 0 ? !afterEventId.isEmpty()
                               : !isValidTimelineEventId(afterEventId))
        || !validWatermark) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 同步游标无效"), -32602);
        return {};
    }
    const QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("after"), after},
        {QStringLiteral("watermark"), watermark.isEmpty()
             ? QJsonValue(QJsonValue::Null) : QJsonValue(watermark)},
        {QStringLiteral("limit"), qBound(1, limit, 200)},
    };
    return sendRequest(method, params, TimelineSyncValidation{params});
}

QString AgentRuntimeClient::timelineSnapshot(const QString &sessionId,
                                             const QString &snapshotIdentity,
                                             const QJsonObject &watermark,
                                             const QJsonObject &after,
                                             int limit)
{
    const QString method = QStringLiteral("timeline/snapshot");
    const bool hasIdentity = !snapshotIdentity.isEmpty();
    const bool hasWatermark = !watermark.isEmpty();
    const bool hasAfter = !after.isEmpty();
    bool validContinuation = false;
    if (hasIdentity && hasWatermark && hasAfter
        && isValidTimelineSnapshotIdentity(snapshotIdentity)
        && isValidTimelineAnchor(watermark)
        && watermark.value(QStringLiteral("sequence")).toDouble() > 0.0
        && isValidTimelineSnapshotCursor(after)) {
        validContinuation = true;
    }
    const bool validInitial = !hasIdentity && !hasWatermark && !hasAfter;
    if (sessionId.isEmpty() || !isBoundedTimelineIdentity(sessionId)
        || (!validInitial && !validContinuation)
        || limit < 1 || limit > 200) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 快照请求无效"), -32602);
        return {};
    }

    return sendRequest(method, {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("snapshot_identity"), hasIdentity
             ? QJsonValue(snapshotIdentity) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("watermark"), hasWatermark
             ? QJsonValue(watermark) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("after"), hasAfter
             ? QJsonValue(after) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::subscribeTimeline(const QString &sessionId,
                                              quint64 connectionGeneration,
                                              quint64 cursorSequence,
                                              const QString &cursorEventId,
                                              const QJsonObject &watermark,
                                              QString *subscriptionId)
{
    const QString method = QStringLiteral("timeline/subscribe");
    if (subscriptionId) subscriptionId->clear();
    const QJsonObject cursor{
        {QStringLiteral("sequence"), static_cast<double>(cursorSequence)},
        {QStringLiteral("event_id"), cursorSequence == 0
             ? QJsonValue(QJsonValue::Null) : QJsonValue(cursorEventId)},
    };
    if (connectionGeneration == 0 || connectionGeneration != m_processGeneration
        || connectionGeneration > static_cast<quint64>(kMaximumSafeJsonInteger)
        || !isBoundedTimelineIdentity(sessionId)
        || cursorSequence > static_cast<quint64>(kMaximumSafeJsonInteger)
        || (cursorSequence == 0 ? !cursorEventId.isEmpty()
                                : !isValidTimelineEventId(cursorEventId))
        || (!watermark.isEmpty() && !isValidTimelineAnchor(watermark))) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅请求无效"), -32602);
        return {};
    }
    if (++m_nextTimelineSubscriptionId == 0) ++m_nextTimelineSubscriptionId;
    const QString createdSubscriptionId = QStringLiteral("qt-subscription-%1-%2")
        .arg(connectionGeneration).arg(m_nextTimelineSubscriptionId);
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscribe-request/0.1")},
        {QStringLiteral("connection_generation"),
         static_cast<double>(connectionGeneration)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), createdSubscriptionId},
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("watermark"), watermark.isEmpty()
             ? QJsonValue(QJsonValue::Null) : QJsonValue(watermark)},
    };
    if (timelineSubscriptionRequestIdentity(QStringLiteral("subscribe"), params).isEmpty()) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅请求无效"), -32602);
        return {};
    }
    const QString requestId = sendRequest(
        method, params, TimelineSubscriptionValidation{params, {}});
    if (requestId.isEmpty()) return {};
    if (subscriptionId) *subscriptionId = createdSubscriptionId;
    return requestId;
}

QString AgentRuntimeClient::syncTimelineSubscription(
    quint64 connectionGeneration, const QString &sessionId,
    const QString &subscriptionId, quint64 afterSequence,
    const QString &afterEventId, const QJsonObject &watermark, int limit)
{
    const QString method = QStringLiteral("timeline/subscription-sync");
    const QJsonObject after{
        {QStringLiteral("sequence"), static_cast<double>(afterSequence)},
        {QStringLiteral("event_id"), afterSequence == 0
             ? QJsonValue(QJsonValue::Null) : QJsonValue(afterEventId)},
    };
    if (connectionGeneration == 0 || connectionGeneration != m_processGeneration
        || connectionGeneration > static_cast<quint64>(kMaximumSafeJsonInteger)
        || !isBoundedTimelineIdentity(sessionId)
        || !isBoundedTimelineIdentity(subscriptionId)
        || afterSequence > static_cast<quint64>(kMaximumSafeJsonInteger)
        || (afterSequence == 0 ? !afterEventId.isEmpty()
                               : !isValidTimelineEventId(afterEventId))
        || !isValidTimelineAnchor(watermark)
        || watermark.value(QStringLiteral("sequence")).toDouble()
            < static_cast<double>(afterSequence)
        || limit < 1 || limit > 200) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅同步请求无效"), -32602);
        return {};
    }
    const QJsonObject nested{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("after"), after},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("limit"), limit},
    };
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-sync-request/0.1")},
        {QStringLiteral("connection_generation"),
         static_cast<double>(connectionGeneration)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), subscriptionId},
        {QStringLiteral("request"), nested},
    };
    if (timelineSubscriptionRequestIdentity(
            QStringLiteral("subscription-sync"), params).isEmpty()) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅同步请求无效"), -32602);
        return {};
    }
    return sendRequest(
        method, params, TimelineSubscriptionValidation{params, {}});
}

QString AgentRuntimeClient::snapshotTimelineSubscription(
    quint64 connectionGeneration, const QString &sessionId,
    const QString &subscriptionId, const QJsonObject &subscriptionCursor,
    const QString &snapshotIdentity, const QJsonObject &watermark,
    const QJsonObject &after, int limit)
{
    const QString method = QStringLiteral("timeline/subscription-snapshot");
    const bool hasIdentity = !snapshotIdentity.isEmpty();
    const bool hasWatermark = !watermark.isEmpty();
    const bool hasAfter = !after.isEmpty();
    const bool validInitial = !hasIdentity && !hasWatermark && !hasAfter;
    const bool validContinuation = hasIdentity && hasWatermark && hasAfter
        && isValidTimelineSnapshotIdentity(snapshotIdentity)
        && isValidTimelineAnchor(watermark)
        && isValidTimelineSnapshotCursor(after);
    if (connectionGeneration == 0 || connectionGeneration != m_processGeneration
        || connectionGeneration > static_cast<quint64>(kMaximumSafeJsonInteger)
        || !isBoundedTimelineIdentity(sessionId)
        || !isBoundedTimelineIdentity(subscriptionId)
        || !isValidTimelineAnchor(subscriptionCursor)
        || (!validInitial && !validContinuation) || limit < 1 || limit > 200) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅快照请求无效"), -32602);
        return {};
    }
    const QJsonObject nested{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("snapshot_identity"), hasIdentity
             ? QJsonValue(snapshotIdentity) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("watermark"), hasWatermark
             ? QJsonValue(watermark) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("after"), hasAfter
             ? QJsonValue(after) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("limit"), limit},
    };
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-snapshot-request/0.1")},
        {QStringLiteral("connection_generation"),
         static_cast<double>(connectionGeneration)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), subscriptionId},
        {QStringLiteral("request"), nested},
    };
    if (timelineSubscriptionRequestIdentity(
            QStringLiteral("subscription-snapshot"), params).isEmpty()) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅快照请求无效"), -32602);
        return {};
    }
    return sendRequest(method, params,
                       TimelineSubscriptionValidation{
                           params,
                           subscriptionCursor,
                       });
}

QString AgentRuntimeClient::activateTimelineSubscription(
    quint64 connectionGeneration, const QString &sessionId,
    const QString &subscriptionId, const QString &source,
    const QJsonObject &cursor, const QJsonObject &watermark,
    const QString &snapshotIdentity)
{
    const QString method = QStringLiteral("timeline/subscription-activate");
    const bool sourceValid = (source == QStringLiteral("sync")
                              && snapshotIdentity.isEmpty())
        || (source == QStringLiteral("snapshot")
            && isValidTimelineSnapshotIdentity(snapshotIdentity));
    if (connectionGeneration == 0 || connectionGeneration != m_processGeneration
        || connectionGeneration > static_cast<quint64>(kMaximumSafeJsonInteger)
        || !isBoundedTimelineIdentity(sessionId)
        || !isBoundedTimelineIdentity(subscriptionId)
        || !isValidTimelineAnchor(cursor) || cursor != watermark || !sourceValid) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅激活请求无效"), -32602);
        return {};
    }
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-activate-request/0.1")},
        {QStringLiteral("connection_generation"),
         static_cast<double>(connectionGeneration)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), subscriptionId},
        {QStringLiteral("source"), source},
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("snapshot_identity"), snapshotIdentity.isEmpty()
             ? QJsonValue(QJsonValue::Null) : QJsonValue(snapshotIdentity)},
    };
    if (timelineSubscriptionRequestIdentity(QStringLiteral("activate"), params).isEmpty()) {
        reportRequestFailure({}, method, QStringLiteral("Timeline 订阅激活请求无效"), -32602);
        return {};
    }
    return sendRequest(
        method, params, TimelineSubscriptionValidation{params, {}});
}

QString AgentRuntimeClient::listMutationAcknowledgements(
    const QString &sessionId, const QJsonObject &after, int limit)
{
    const QString method = QStringLiteral("session/mutation-acknowledgements");
    const bool hasAfter = !after.isEmpty();
    if (!isBoundedTimelineIdentity(QJsonValue(sessionId))
            || (hasAfter && !isValidDurableMutationCursor(QJsonValue(after)))
            || limit < 1 || limit > 100) {
        reportRequestFailure({}, method,
                             QStringLiteral("持久化操作确认列表请求无效"), -32602);
        return {};
    }
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("mutation-acknowledgement-list-request/0.1")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("after"), hasAfter
             ? QJsonValue(after) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("limit"), limit},
    };
    return sendRequest(method, params, MutationListValidation{params});
}

QString AgentRuntimeClient::consumeMutationAcknowledgement(
    const QString &sessionId, const QString &operationIdentity,
    quint64 expectedRevision, const QString &target,
    const QJsonObject &confirmedAnchor)
{
    const QString method = QStringLiteral("mutation/acknowledgement/consume");
    if (!isBoundedTimelineIdentity(QJsonValue(sessionId))
            || !isValidDurableMutationOperationIdentity(
                QJsonValue(operationIdentity))
            || expectedRevision == 0
            || expectedRevision
                > static_cast<quint64>(kMaximumSafeJsonInteger)
            || (target != QStringLiteral("accepted")
                && target != QStringLiteral("terminal"))
            || !isValidPositiveTimelineAnchor(QJsonValue(confirmedAnchor))) {
        reportRequestFailure({}, method,
                             QStringLiteral("持久化操作确认消费请求无效"), -32602);
        return {};
    }
    const QJsonObject params{
        {QStringLiteral("schema_version"),
         QStringLiteral("mutation-acknowledgement-consume-request/0.1")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("operation_identity"), operationIdentity},
        {QStringLiteral("expected_revision"),
         static_cast<double>(expectedRevision)},
        {QStringLiteral("target"), target},
        {QStringLiteral("confirmed_anchor"), confirmedAnchor},
    };
    return sendRequest(method, params, MutationConsumeValidation{params});
}

QString AgentRuntimeClient::backgroundNotifications(const QString &sessionId,
                                                    const QJsonObject &cursor, int limit)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("limit"), qBound(1, limit, 100)},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("session/background-notifications"), params);
}

QString AgentRuntimeClient::backgroundRecovery(const QString &sessionId,
                                               const QJsonObject &cursor, int limit)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("limit"), qBound(1, limit, 100)},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("session/background-recovery"), params);
}

QString AgentRuntimeClient::projectionRecoveryStatus()
{
    return sendRequest(QStringLiteral("runtime/projection-recovery/status"));
}

QString AgentRuntimeClient::sessionRecoveryStatus(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/recovery/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::operationStatus(const QString &sessionId)
{
    if (sessionId.isEmpty()) return {};
    return sendRequest(QStringLiteral("operation/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::operationProbe(const QJsonObject &params)
{
    return sendRequest(QStringLiteral("operation/probe"), params);
}

QString AgentRuntimeClient::operationReconcile(const QJsonObject &params)
{
    return sendRequest(QStringLiteral("operation/reconcile"), params);
}

QString AgentRuntimeClient::createCompactionCheckpoint(
    const QString &sessionId, const QString &checkpointId,
    const QString &preservationInstructions, const QJsonObject &summary)
{
    if (sessionId.isEmpty() || checkpointId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("checkpoint_id"), checkpointId},
        {QStringLiteral("summary"), summary},
    };
    if (!preservationInstructions.isEmpty()) {
        params.insert(QStringLiteral("preservation_instructions"), preservationInstructions);
    }
    return sendRequest(QStringLiteral("session/compaction/checkpoint/create"), params);
}

QString AgentRuntimeClient::readCompactionCheckpoint(const QString &sessionId,
                                                     const QString &checkpointId)
{
    if (sessionId.isEmpty() || checkpointId.isEmpty()) return {};
    return sendRequest(QStringLiteral("session/compaction/checkpoint/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("checkpoint_id"), checkpointId},
    });
}

QString AgentRuntimeClient::reviseCompactionCheckpoint(
    const QString &sessionId, const QString &sourceCheckpointId,
    const QString &sourceReviewId, const QString &checkpointId,
    const QString &preservationInstructions, const QJsonObject &summary)
{
    if (sessionId.isEmpty() || sourceCheckpointId.isEmpty() || sourceReviewId.isEmpty()
            || checkpointId.isEmpty() || sourceCheckpointId == checkpointId) {
        return {};
    }
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("source_checkpoint_id"), sourceCheckpointId},
        {QStringLiteral("source_review_id"), sourceReviewId},
        {QStringLiteral("checkpoint_id"), checkpointId},
        {QStringLiteral("summary"), summary},
    };
    if (!preservationInstructions.isEmpty()) {
        params.insert(QStringLiteral("preservation_instructions"), preservationInstructions);
    }
    return sendRequest(QStringLiteral("session/compaction/checkpoint/revise"), params);
}

QString AgentRuntimeClient::runtimeRecoveryStatus()
{
    return sendRequest(QStringLiteral("runtime/recovery/status"));
}

QString AgentRuntimeClient::listWorkspace(const QString &projectId, const QString &path,
                                          const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/list"), params);
}

QString AgentRuntimeClient::readWorkspaceFile(const QString &projectId, const QString &path,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/read"), params);
}

QString AgentRuntimeClient::saveWorkspaceFile(const QString &projectId, const QString &path,
                                              const QString &content,
                                              const QString &expectedRevision,
                                              const QString &encoding,
                                              const QString &newline,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("expected_revision"), expectedRevision},
        {QStringLiteral("encoding"), encoding},
        {QStringLiteral("newline"), newline},
        {QStringLiteral("origin"), QStringLiteral("user")},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/save-user-text"), params);
}

QString AgentRuntimeClient::workspaceMetadata(const QString &projectId, const QString &path,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/metadata"), params);
}

QString AgentRuntimeClient::workspaceGitStatus(const QString &projectId)
{
    return sendRequest(QStringLiteral("workspace/git-status"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::gitOverview(const QString &projectId)
{
    return sendRequest(QStringLiteral("workspace/git/overview"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::gitLog(const QString &projectId, int limit,
                                   const QString &cursor)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId},
                       {QStringLiteral("limit"), limit}};
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("workspace/git/log"), params);
}

QString AgentRuntimeClient::gitCommit(const QString &projectId, const QString &oid)
{
    return sendRequest(QStringLiteral("workspace/git/commit"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("oid"), oid},
    });
}

QString AgentRuntimeClient::gitDiff(const QString &projectId, const QString &scope,
                                    const QString &oid, const QString &path)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId},
                       {QStringLiteral("scope"), scope}};
    if (!oid.isEmpty()) params.insert(QStringLiteral("oid"), oid);
    if (!path.isEmpty()) params.insert(QStringLiteral("path"), path);
    return sendRequest(QStringLiteral("workspace/git/diff"), params);
}

QString AgentRuntimeClient::gitContext(const QString &projectId, const QString &kind,
                                       const QString &scope, const QString &oid)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId},
                       {QStringLiteral("kind"), kind}};
    if (!scope.isEmpty()) params.insert(QStringLiteral("scope"), scope);
    if (!oid.isEmpty()) params.insert(QStringLiteral("oid"), oid);
    return sendRequest(QStringLiteral("workspace/git/context/read"), params);
}

QString AgentRuntimeClient::searchWorkspace(const QString &projectId,
                                            const QString &searchId,
                                            const QString &query,
                                            const QString &mode,
                                            bool caseSensitive,
                                            const QString &cursor,
                                            int limit,
                                            const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("search_id"), searchId},
        {QStringLiteral("query"), query},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("case_sensitive"), caseSensitive},
        {QStringLiteral("limit"), limit},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/search"), params);
}

QString AgentRuntimeClient::cancelWorkspaceSearch(const QString &searchId,
                                                  const QString &projectId,
                                                  const QString &rootId)
{
    QJsonObject params{{QStringLiteral("search_id"), searchId}};
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/search/cancel"), params);
}

QString AgentRuntimeClient::indexWorkspace(const QString &projectId, const QString &indexId,
                                           const QString &rootId)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId}};
    if (!indexId.isEmpty()) params.insert(QStringLiteral("index_id"), indexId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/index"), params);
}

QString AgentRuntimeClient::cancelWorkspaceIndex(const QString &projectId,
                                                 const QString &indexId,
                                                 const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("index_id"), indexId},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/index/cancel"), params);
}

QString AgentRuntimeClient::repositoryMap(const QString &projectId, int tokenBudget,
                                          const QStringList &focusPaths,
                                          const QString &rootId)
{
    QJsonArray paths;
    for (const QString &path : focusPaths) paths.append(path);
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("token_budget"), tokenBudget},
        {QStringLiteral("focus_paths"), paths},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/repository-map"), params);
}

QString AgentRuntimeClient::languageServers(const QString &projectId, const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-servers"), params);
}

QString AgentRuntimeClient::startLanguageServer(const QString &projectId,
                                                const QString &path,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-server/start"), params);
}

QString AgentRuntimeClient::stopLanguageServer(const QString &projectId,
                                               const QString &path,
                                               const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-server/stop"), params);
}

QString AgentRuntimeClient::workspaceDefinition(const QString &projectId,
                                                const QString &path,
                                                const QString &content,
                                                const QString &revision,
                                                int line, int column,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), column},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/definition"), params);
}

QString AgentRuntimeClient::workspaceReferences(const QString &projectId,
                                                const QString &path,
                                                const QString &content,
                                                const QString &revision,
                                                int line, int column,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), column},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/references"), params);
}

QString AgentRuntimeClient::workspaceDiagnostics(const QString &projectId,
                                                 const QString &path,
                                                 const QString &content,
                                                 const QString &revision,
                                                 const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/diagnostics"), params);
}

QString AgentRuntimeClient::observedDiagnostics(const QString &projectId,
                                                const QString &path,
                                                bool includeStale,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("include_stale"), includeStale},
    };
    if (!path.isEmpty()) params.insert(QStringLiteral("path"), path);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/observed-diagnostics"), params);
}

QString AgentRuntimeClient::diagnosticRaw(const QString &projectId,
                                          const QString &reference,
                                          const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("reference"), reference},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/diagnostics/raw"), params);
}

QString AgentRuntimeClient::previewWorkspaceEdit(const QString &sessionId,
                                                 const QJsonObject &edit,
                                                 const QJsonArray &contents)
{
    return sendRequest(QStringLiteral("workspace/edit/preview"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("edit"), edit},
        {QStringLiteral("contents"), contents},
    });
}

QString AgentRuntimeClient::readWorkspaceEditArtifact(const QString &sessionId,
                                                      const QString &projectId,
                                                      const QString &editId,
                                                      const QString &reference,
                                                      qint64 offset, int limit)
{
    return sendRequest(QStringLiteral("workspace/edit/artifact/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("edit_id"), editId},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::latestWorkspaceEditProposal(const QString &sessionId)
{
    return sendRequest(QStringLiteral("workspace/edit/proposal/latest"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::readWorkspaceEditProposal(const QString &sessionId,
                                                      const QString &proposalId)
{
    return sendRequest(QStringLiteral("workspace/edit/proposal/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("proposal_id"), proposalId},
    });
}

QString AgentRuntimeClient::readWorkspaceEditProposalArtifact(
    const QString &sessionId, const QString &proposalId,
    const QString &reference, qint64 offset, int limit)
{
    return sendRequest(QStringLiteral("workspace/edit/proposal/artifact/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("proposal_id"), proposalId},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::watchWorkspace(const QString &projectId, const QStringList &paths,
                                           const QString &watchId,
                                           const QString &rootId)
{
    QJsonArray pathValues;
    for (const QString &path : paths) pathValues.append(path);
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("paths"), pathValues},
    };
    if (!watchId.isEmpty()) params.insert(QStringLiteral("watch_id"), watchId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/watch"), params);
}

QString AgentRuntimeClient::pollWorkspaceWatch(const QString &watchId)
{
    return sendRequest(QStringLiteral("workspace/watch/poll"),
                       {{QStringLiteral("watch_id"), watchId}});
}

QString AgentRuntimeClient::openUserTerminal(const QString &sessionId,
                                             const QString &kind,
                                             const QString &name,
                                             int rows, int cols)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("cols"), cols},
    };
    if (!name.isEmpty()) params.insert(QStringLiteral("name"), name);
    return sendRequest(QStringLiteral("terminal/open-user"), params);
}

QString AgentRuntimeClient::listTerminals(const QString &sessionId)
{
    return sendRequest(QStringLiteral("terminal/list"),
                       {{QStringLiteral("session_id"), sessionId}});
}

QString AgentRuntimeClient::attachTerminal(const QString &sessionId,
                                           const QString &terminalId,
                                           quint64 after)
{
    return sendRequest(QStringLiteral("terminal/attach"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("after"), static_cast<qint64>(after)},
    });
}

QString AgentRuntimeClient::readTerminalExcerpt(const QString &sessionId,
                                                const QString &terminalId,
                                                int maxBytes)
{
    return sendRequest(QStringLiteral("terminal/excerpt/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("max_bytes"), maxBytes},
    });
}

QString AgentRuntimeClient::inputUserTerminal(const QString &sessionId,
                                              const QString &terminalId,
                                              const QByteArray &data)
{
    return sendRequest(QStringLiteral("terminal/input-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("data_base64"), QString::fromLatin1(data.toBase64())},
    });
}

QString AgentRuntimeClient::resizeTerminal(const QString &sessionId,
                                           const QString &terminalId,
                                           int rows, int cols)
{
    return sendRequest(QStringLiteral("terminal/resize"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("cols"), cols},
    });
}

QString AgentRuntimeClient::signalUserTerminal(const QString &sessionId,
                                               const QString &terminalId,
                                               const QString &signal)
{
    return sendRequest(QStringLiteral("terminal/signal-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("signal"), signal},
    });
}

QString AgentRuntimeClient::stopUserTerminal(const QString &sessionId,
                                             const QString &terminalId)
{
    return sendRequest(QStringLiteral("terminal/stop-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    });
}

QString AgentRuntimeClient::restartUserTerminal(const QString &sessionId,
                                                const QString &terminalId,
                                                int rows, int cols)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    };
    if (rows > 0) params.insert(QStringLiteral("rows"), rows);
    if (cols > 0) params.insert(QStringLiteral("cols"), cols);
    return sendRequest(QStringLiteral("terminal/restart-user"), params);
}

QString AgentRuntimeClient::removeUserTerminal(const QString &sessionId,
                                               const QString &terminalId)
{
    return sendRequest(QStringLiteral("terminal/remove-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    });
}

QString AgentRuntimeClient::readCommandArtifact(const QString &sessionId,
                                                const QString &reference)
{
    return sendRequest(QStringLiteral("artifact/read-command-output"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("reference"), reference},
    });
}

QString AgentRuntimeClient::readCommandArtifactPage(const QString &sessionId,
                                                    const QString &itemId,
                                                    const QString &reference,
                                                    const QJsonObject &cursor)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("item_id"), itemId},
        {QStringLiteral("reference"), reference},
    };
    if (cursor.isEmpty()) {
        params.insert(QStringLiteral("limit"), 64 * 1024);
        params.insert(QStringLiteral("max_total_inline_bytes"), 64 * 1024);
    } else {
        params.insert(QStringLiteral("cursor"), cursor);
    }
    return sendRequest(QStringLiteral("artifact/read-command-output-page"), params,
                       CommandArtifactPageValidation{params});
}

QString AgentRuntimeClient::locateRuntime() const
{
    const QString executableName =
#ifdef Q_OS_WIN
        QStringLiteral("aegisy-agentd.exe");
#else
        QStringLiteral("aegisy-agentd");
#endif
    const QString fromEnvironment = qEnvironmentVariable("AEGISY_AGENTD_PATH");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        fromEnvironment,
        QDir(appDir).filePath(executableName),
        QDir(appDir).filePath(QStringLiteral("../Resources/") + executableName),
#ifdef AEGISY_AGENTD_DEV_PATH
        QStringLiteral(AEGISY_AGENTD_DEV_PATH),
#endif
    };
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty()) continue;
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.absoluteFilePath();
    }
    return {};
}

QString AgentRuntimeClient::sendRequest(const QString &method, const QJsonObject &params,
                                        PendingValidation validation)
{
    if (m_process->state() == QProcess::NotRunning) {
        reportRequestFailure({}, method, QStringLiteral("本地运行时未启动"), -1);
        return {};
    }
    if (method != QStringLiteral("initialize") && !m_handshakeComplete) {
        reportRequestFailure({}, method, QStringLiteral("本地运行时握手尚未完成"), -32003);
        return {};
    }
    if (m_emergencyDisabled && !emergencyRequestAllowed(method)) {
        reportRequestFailure({}, method,
                             QStringLiteral("服务器应急策略已暂停新的工作台操作"), -32153);
        return {};
    }
    if (m_heartbeatNegotiated && !m_heartbeatHealthy
        && !isLivenessControlMethod(method)) {
        reportRequestFailure({}, method, QStringLiteral("本地运行时响应状态未知"), -1);
        return {};
    }
    if (m_reconnectRecoveryPending && !isReconnectRecoveryMethod(method)) {
        reportRequestFailure({}, method,
                             QStringLiteral("运行时重连恢复尚未完成"), -32003);
        return {};
    }
    if (method != QStringLiteral("initialize")) {
        const QStringList required = requiredCapabilitiesForMethod(method, params);
        for (const QString &capability : required) {
            if (!m_negotiatedStableCapabilities.contains(capability)) {
                reportRequestFailure({}, method,
                                     QStringLiteral("本地运行时未协商此操作所需能力"),
                                     -32601);
                return {};
            }
        }
    }
    const QString id = QString::number(++m_nextRequestId);
    PendingRequestContext pending{
        {id, method, std::nullopt},
        m_processGeneration,
        std::move(validation),
    };
    if (const auto *subscription = std::get_if<TimelineSubscriptionValidation>(
            &pending.validation)) {
        const QString stage = timelineSubscriptionIdentityStageForMethod(method);
        const QString identity = timelineSubscriptionRequestIdentity(
            stage, subscription->request);
        if (stage.isEmpty() || identity.isEmpty()) {
            reportRequestFailure({}, method,
                                 QStringLiteral("Timeline 订阅请求身份无效"), -32602);
            return {};
        }
        pending.transport.typed_error_request_identity = identity;
    }
    m_pendingRequests.insert(id, pending);
    const int writeError = writeMessage({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
    if (writeError != 0) {
        removePendingRequest(id);
        const QString failureDetail = writeError == -32005
            ? QStringLiteral("请求超过 AAP 帧上限")
            : QStringLiteral("无法写入本地运行时");
        reportRequestFailure(id, method, failureDetail, writeError);
        if (writeError == -1) {
            suppressAutomaticReconnect();
            m_discardProcessOutput = true;
            clearNegotiationState();
            failPending(failureDetail);
            setReconnectState(ReconnectState::Exhausted, 0, failureDetail);
            emit connectionStateChanged(false, failureDetail);
        }
        return {};
    }
    return id;
}

bool AgentRuntimeClient::emergencyRequestAllowed(const QString &method)
{
    static const QSet<QString> allowed{
        QStringLiteral("initialize"),
        QStringLiteral("shutdown"),
        QStringLiteral("runtime/health"),
        QStringLiteral("runtime/degradations"),
        QStringLiteral("runtime/projection-recovery/status"),
        QStringLiteral("runtime/recovery/status"),
        QStringLiteral("runtime/recovery/export"),
        QStringLiteral("model/catalog"),
        QStringLiteral("model/catalog-cache"),
        QStringLiteral("model/catalog-refresh-status"),
        QStringLiteral("model/capability-check"),
        QStringLiteral("model/profile/list"),
        QStringLiteral("model/profile/read"),
        QStringLiteral("project/list"),
        QStringLiteral("project/root-list"),
        QStringLiteral("project/trust-review"),
        QStringLiteral("session/list"),
        QStringLiteral("session/search"),
        QStringLiteral("session/read"),
        QStringLiteral("session/delete/preview"),
        QStringLiteral("session/deletion/status"),
        QStringLiteral("session/export/preview"),
        QStringLiteral("session/export"),
        QStringLiteral("session/mutation-acknowledgements"),
        QStringLiteral("session/background-notifications"),
        QStringLiteral("session/background-recovery"),
        QStringLiteral("session/recovery/status"),
        QStringLiteral("session/compaction/checkpoint/read"),
        QStringLiteral("timeline/sync"),
        QStringLiteral("timeline/snapshot"),
        QStringLiteral("timeline/subscribe"),
        QStringLiteral("timeline/subscription-sync"),
        QStringLiteral("timeline/subscription-snapshot"),
        QStringLiteral("timeline/subscription-activate"),
        QStringLiteral("operation/status"),
        QStringLiteral("operation/probe"),
        QStringLiteral("turn/cancel"),
        QStringLiteral("turn/context/inspect"),
        QStringLiteral("workspace/pinned-context/list"),
        QStringLiteral("workspace/image/read"),
        QStringLiteral("workspace/list"),
        QStringLiteral("workspace/read"),
        QStringLiteral("workspace/metadata"),
        QStringLiteral("workspace/git-status"),
        QStringLiteral("workspace/git/overview"),
        QStringLiteral("workspace/git/log"),
        QStringLiteral("workspace/git/commit"),
        QStringLiteral("workspace/git/diff"),
        QStringLiteral("workspace/git/context/read"),
        QStringLiteral("workspace/search"),
        QStringLiteral("workspace/search/cancel"),
        QStringLiteral("workspace/index"),
        QStringLiteral("workspace/index/cancel"),
        QStringLiteral("workspace/repository-map"),
        QStringLiteral("workspace/language-servers"),
        QStringLiteral("workspace/definition"),
        QStringLiteral("workspace/references"),
        QStringLiteral("workspace/diagnostics"),
        QStringLiteral("workspace/observed-diagnostics"),
        QStringLiteral("workspace/diagnostics/raw"),
        QStringLiteral("workspace/edit/artifact/read"),
        QStringLiteral("workspace/edit/proposal/latest"),
        QStringLiteral("workspace/edit/proposal/read"),
        QStringLiteral("workspace/edit/proposal/artifact/read"),
        QStringLiteral("terminal/list"),
        QStringLiteral("terminal/attach"),
        QStringLiteral("terminal/read"),
        QStringLiteral("terminal/excerpt/read"),
        QStringLiteral("terminal/stop-user"),
        QStringLiteral("terminal/close-user"),
        QStringLiteral("terminal/remove-user"),
        QStringLiteral("artifact/read-command-output"),
        QStringLiteral("artifact/read-command-output-page"),
    };
    return allowed.contains(method);
}

bool AgentRuntimeClient::sendNotification(const QString &method, const QJsonObject &params)
{
    return writeMessage({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    }) == 0;
}

int AgentRuntimeClient::writeMessage(const QJsonObject &message)
{
    QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact);
    const int maximumFrameBytes = m_handshakeComplete
        ? m_negotiatedMaximumFrameBytes : kMaximumFrameBytes;
    if (maximumFrameBytes <= 0 || frame.size() > maximumFrameBytes) return -32005;
    frame.append('\n');
    const qint64 written = usesVerifiedUnixSocket()
        ? (m_localSocket
               && m_localSocket->state() == QLocalSocket::ConnectedState
               && m_unixSocketPeerVerifiedGeneration == m_processGeneration
               && m_localSocketPeerVerifiedAttemptEpoch
                   == m_localSocketAttemptEpoch
               ? m_localSocket->write(frame) : -1)
        : m_process->write(frame);
    return written == frame.size() ? 0 : -1;
}

void AgentRuntimeClient::processStdout()
{
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (usesVerifiedUnixSocket()) return;
    processTransportBytes(bytes);
}

void AgentRuntimeClient::processSocketInput(QLocalSocket *socket,
                                            quint64 generation,
                                            quint64 attemptEpoch)
{
    if (!isCurrentLocalSocket(socket, generation, attemptEpoch)) return;
    if (m_unixSocketPeerVerifiedGeneration != generation
        || m_localSocketPeerVerifiedAttemptEpoch != attemptEpoch) {
        socket->readAll();
        return;
    }
    processTransportBytes(socket->readAll());
}

void AgentRuntimeClient::processTransportBytes(const QByteArray &bytes)
{
    if (m_discardProcessOutput) {
        m_stdoutBuffer.clear();
        return;
    }
    m_stdoutBuffer.append(bytes);
    while (true) {
        const int maximumFrameBytes = m_handshakeComplete
            ? m_negotiatedMaximumFrameBytes : kMaximumFrameBytes;
        if (maximumFrameBytes <= 0) {
            rejectProtocolMessage(QStringLiteral("frame-limit-unavailable"));
            return;
        }
        const qsizetype newline = m_stdoutBuffer.indexOf('\n');
        if (newline < 0) {
            if (m_stdoutBuffer.size() > maximumFrameBytes) {
                if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
                    rejectInitializeResponse(QStringLiteral("frame-too-large"));
                } else {
                    rejectProtocolMessage(QStringLiteral("frame-too-large"));
                }
            }
            return;
        }
        QByteArray line = m_stdoutBuffer.left(newline);
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.size() > maximumFrameBytes) {
            if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
                rejectInitializeResponse(QStringLiteral("frame-too-large"));
            } else {
                rejectProtocolMessage(QStringLiteral("frame-too-large"));
            }
            return;
        }
        if (line.trimmed().isEmpty()) continue;
        transport_generated::TransportMessage message;
        transport_generated::TransportDecodeError error;
        if (!transport_generated::parseTransportMessageRaw(line, &message, &error)) {
            if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
                rejectInitializeResponse(QStringLiteral("invalid-json"));
            } else {
                rejectProtocolMessage(QStringLiteral("invalid-json"));
            }
            return;
        }
        processMessage(message);
    }
}

void AgentRuntimeClient::processMessage(
    const transport_generated::TransportMessage &message)
{
    const TransportJsonObject *root = transportObject(message.value);
    const auto failDispatch = [this](const transport_generated::TransportDispatchError &error,
                                     const QString &reason) {
        const bool initializing = !m_handshakeComplete
            && !m_initializeRequestId.isEmpty();
        if (error.kind
            == transport_generated::TransportDispatchErrorKind::ValidatorUnavailable) {
            emit diagnosticMessage(QStringLiteral("本地 AAP Transport 校验器不可用"));
        }
        if (initializing) rejectInitializeResponse(reason);
        else rejectProtocolMessage(reason);
    };

    if (root && root->contains(QStringLiteral("method"))) {
        transport_generated::TransportRequestOrNotification dispatched;
        transport_generated::TransportDispatchError dispatchError;
        if (!transport_generated::decodeTransportRequestOrNotification(
                message, &dispatched, &dispatchError)) {
            failDispatch(dispatchError,
                         dispatchError.kind
                                 == transport_generated::TransportDispatchErrorKind::ValidatorUnavailable
                             ? QStringLiteral("validator-unavailable")
                             : QStringLiteral("notification-shape"));
            return;
        }
        if (!m_handshakeComplete) {
            if (!m_initializeRequestId.isEmpty()) {
                rejectInitializeResponse(QStringLiteral("message-before-handshake"));
            } else {
                rejectProtocolMessage(QStringLiteral("message-before-handshake"));
            }
            return;
        }
        if (dispatched.kind
            == transport_generated::TransportRequestOrNotificationKind::UnknownRequest) {
            rejectProtocolMessage(QStringLiteral("notification-shape"));
            return;
        }
        if (dispatched.kind
            == transport_generated::TransportRequestOrNotificationKind::UnknownNotification) {
            emit diagnosticMessage(QStringLiteral("忽略未支持的 AAP 通知"));
            return;
        }
        const QString *methodField = transportStringField(*root, QStringLiteral("method"));
        const auto *paramsField = transportField(*root, QStringLiteral("params"));
        QJsonValue projectedParams;
        if (!methodField || !paramsField
            || !projectTransportValue(*paramsField, &projectedParams)
            || !projectedParams.isObject()) {
            rejectProtocolMessage(QStringLiteral("notification-projection"));
            return;
        }
        const QString method = *methodField;
        const QJsonObject params = projectedParams.toObject();
        if (method == QStringLiteral("event")) {
            if (m_negotiatedStableCapabilities.contains(
                    QStringLiteral("timeline.subscription.fixed-watermark"))) {
                emit diagnosticMessage(QStringLiteral(
                    "忽略订阅模式下未绑定的 Timeline 通知"));
                return;
            }
            if (!m_negotiatedStableCapabilities.contains(
                    QStringLiteral("timeline.streaming"))) {
                rejectProtocolMessage(QStringLiteral("notification-capability"));
                return;
            }
            const QJsonObject event = params;
            if (!isValidTimelineEventEnvelope(event)) {
                rejectProtocolMessage(QStringLiteral("event-envelope"));
                return;
            }
            emit timelineEvent(event);
        } else if (method == QStringLiteral("timeline/subscription-event")) {
            if (!m_negotiatedStableCapabilities.contains(
                    QStringLiteral("timeline.subscription.fixed-watermark"))) {
                rejectProtocolMessage(QStringLiteral("notification-capability"));
                return;
            }
            const QJsonObject event = params;
            if (!isValidTimelineSubscriptionEvent(event)) {
                rejectProtocolMessage(QStringLiteral("timeline-subscription-event"));
                return;
            }
            if (static_cast<quint64>(event.value(
                    QStringLiteral("connection_generation")).toDouble())
                != m_processGeneration) {
                return;
            }
            emit timelineSubscriptionEvent(event);
        } else if (method == QStringLiteral("timeline/subscription-failure")) {
            if (!m_negotiatedStableCapabilities.contains(
                    QStringLiteral("timeline.subscription.fixed-watermark"))) {
                rejectProtocolMessage(QStringLiteral("notification-capability"));
                return;
            }
            const QJsonObject failure = params;
            if (!isValidTimelineSubscriptionFailure(failure)
                || failure.value(QStringLiteral("stage")).toString()
                    != QStringLiteral("live")) {
                rejectProtocolMessage(QStringLiteral("timeline-subscription-failure"));
                return;
            }
            if (static_cast<quint64>(failure.value(
                    QStringLiteral("connection_generation")).toDouble())
                != m_processGeneration) {
                return;
            }
            emit timelineSubscriptionFailed({}, failure);
        } else {
            emit diagnosticMessage(QStringLiteral("忽略未支持的 AAP 通知"));
        }
        return;
    }

    const QString *idField = root
        ? transportStringField(*root, QStringLiteral("id")) : nullptr;
    const QString id = idField ? *idField : QString();
    const auto pendingIterator = id.isEmpty()
        ? m_pendingRequests.constEnd() : m_pendingRequests.constFind(id);
    const bool pendingCurrent = pendingIterator != m_pendingRequests.constEnd()
        && pendingIterator.value().processGeneration == m_processGeneration
        && !m_retiredResponseIds.contains(id);
    const std::optional<transport_generated::TransportPendingRequest> generatedPending =
        pendingCurrent
        ? std::optional<transport_generated::TransportPendingRequest>(
              pendingIterator.value().transport)
        : std::nullopt;
    transport_generated::TransportResponse dispatched;
    transport_generated::TransportDispatchError dispatchError;
    if (!transport_generated::decodeTransportResponse(
            generatedPending, message, &dispatched, &dispatchError)) {
        failDispatch(dispatchError,
                     dispatchError.kind
                             == transport_generated::TransportDispatchErrorKind::ValidatorUnavailable
                         ? QStringLiteral("validator-unavailable")
                         : QStringLiteral("response-shape"));
        return;
    }
    if (dispatched.kind == transport_generated::TransportResponseKind::Unmatched) {
        return;
    }
    if (!pendingCurrent) return;

    const PendingRequestContext pending = pendingIterator.value();
    const QString pendingMethod = pending.transport.method;
    if (!m_handshakeComplete && (id != m_initializeRequestId
            || pendingMethod != QStringLiteral("initialize")
            || m_initializeGeneration != m_processGeneration)) {
        rejectInitializeResponse(QStringLiteral("response-id"));
        return;
    }
    if (m_handshakeComplete && pendingMethod == QStringLiteral("initialize")) {
        rejectProtocolMessage(QStringLiteral("response-correlation"));
        return;
    }

    const bool hasResult = root && root->contains(QStringLiteral("result"));
    const bool hasError = root && root->contains(QStringLiteral("error"));
    if (hasResult == hasError) {
        failDispatch(dispatchError, QStringLiteral("response-shape"));
        return;
    }

    if (!m_handshakeComplete) {
        if (hasError) {
            const auto *errorValue = transportField(*root, QStringLiteral("error"));
            const TransportJsonObject *errorObjectValue = errorValue
                ? transportObject(*errorValue) : nullptr;
            const auto *code = errorObjectValue
                ? transportNumberField(*errorObjectValue, QStringLiteral("code")) : nullptr;
            const QString *errorMessage = errorObjectValue
                ? transportStringField(*errorObjectValue, QStringLiteral("message")) : nullptr;
            QString reasonCode = QStringLiteral("runtime-rejected");
            if (!code || !errorMessage) {
                rejectInitializeResponse(QStringLiteral("error-shape"));
                return;
            }
            if (transport_runtime::transportJsonIntegerEqualsQint64(*code, -32003)) {
                const auto *data = transportField(*errorObjectValue,
                                                  QStringLiteral("data"));
                QJsonValue projectedData;
                if (!data || !projectTransportValue(*data, &projectedData)
                    || !projectedData.isObject()) {
                    rejectInitializeResponse(QStringLiteral("initialize-error-data"));
                    return;
                }
                const QJsonObject projectedError{
                    {QStringLiteral("code"), -32003},
                    {QStringLiteral("message"), *errorMessage},
                    {QStringLiteral("data"), projectedData},
                };
                if (!validateInitializeError(projectedError, &reasonCode)) {
                    rejectInitializeResponse(reasonCode);
                    return;
                }
            }
            rejectInitializeResponse(reasonCode);
            return;
        }
        const auto *resultValue = transportField(*root, QStringLiteral("result"));
        QJsonValue projectedResult;
        if (!resultValue || !projectTransportValue(*resultValue, &projectedResult)
            || !projectedResult.isObject()) {
            rejectInitializeResponse(QStringLiteral("result-type"));
            return;
        }
        QString reasonCode;
        QSet<QString> stableCapabilities;
        int maximumFrameBytes = 0;
        const QJsonObject result = projectedResult.toObject();
        if (!validateInitializeResult(result, expectedTransportSecurity(), &stableCapabilities,
                                      &maximumFrameBytes, &reasonCode)) {
            rejectInitializeResponse(reasonCode);
            return;
        }
        retireResponseId(m_initializeRequestId);
        removePendingRequest(m_initializeRequestId);
        m_initializeRequestId.clear();
        m_initializeGeneration = 0;
        m_negotiatedStableCapabilities = stableCapabilities;
        m_negotiatedMaximumFrameBytes = maximumFrameBytes;
        acceptInitializeResponse(result);
        return;
    }

    if (hasError) {
        const auto *errorValue = transportField(*root, QStringLiteral("error"));
        const TransportJsonObject *errorObjectValue = errorValue
            ? transportObject(*errorValue) : nullptr;
        const auto *errorCode = errorObjectValue
            ? transportNumberField(*errorObjectValue, QStringLiteral("code")) : nullptr;
        const QString *errorMessage = errorObjectValue
            ? transportStringField(*errorObjectValue, QStringLiteral("message")) : nullptr;
        if (!errorObjectValue || !errorCode || !errorMessage) {
            rejectProtocolMessage(QStringLiteral("error-shape"));
            return;
        }
        const auto *dataValue = transportField(*errorObjectValue,
                                               QStringLiteral("data"));
        const auto emitFailure = [this, &id, &pendingMethod, errorCode, errorMessage]() {
            const QString canonicalCode = errorCode->canonical;
            emit requestFailedExact(id, pendingMethod, *errorMessage, canonicalCode);
            qint64 code64 = 0;
            const auto conversion = transport_runtime::transportJsonIntegerToQint64(
                *errorCode, &code64);
            if (conversion == transport_runtime::TransportIntegerConversion::Ok
                && code64 >= std::numeric_limits<int>::min()
                && code64 <= std::numeric_limits<int>::max()) {
                emit requestFailed(id, pendingMethod, *errorMessage,
                                   static_cast<int>(code64));
            }
        };
        const QString subscriptionIdentityStage =
            timelineSubscriptionIdentityStageForMethod(pendingMethod);
        if (!subscriptionIdentityStage.isEmpty()) {
            QJsonValue projectedData;
            if (!dataValue || !projectTransportValue(*dataValue, &projectedData)) {
                rejectProtocolMessage(QStringLiteral("error-data-projection"));
                return;
            }
            const auto *subscription = std::get_if<TimelineSubscriptionValidation>(
                &pending.validation);
            const QJsonObject metadata = subscription
                ? QJsonObject{
                      {QStringLiteral("request"), subscription->request},
                      {QStringLiteral("subscription_cursor"),
                       subscription->subscriptionCursor},
                  }
                : QJsonObject{};
            if (!projectedData.isObject()
                || !isValidTimelineSubscriptionFailureForRequest(
                    projectedData.toObject(), pendingMethod, metadata)) {
                rejectProtocolMessage(QStringLiteral("timeline-subscription-failure"));
                return;
            }
            const QJsonObject failure = projectedData.toObject();
            removePendingRequest(id);
            emit timelineSubscriptionFailed(id, failure);
            return;
        }
        if (pendingMethod == QStringLiteral("runtime/heartbeat")) {
            emitFailure();
            handleHeartbeatTimeout();
            return;
        }
        if (pendingMethod == QStringLiteral("timeline/sync")
            && transport_runtime::transportJsonIntegerEqualsQint64(
                *errorCode, -32148)) {
            QJsonValue projectedData;
            if (!dataValue || !projectTransportValue(*dataValue, &projectedData)) {
                rejectProtocolMessage(QStringLiteral("error-data-projection"));
                return;
            }
            const auto *sync = std::get_if<TimelineSyncValidation>(&pending.validation);
            if (!sync || !projectedData.isObject()
                || !isValidTimelineRetentionGapData(
                    projectedData.toObject(), sync->request)
                || projectedData.toObject()
                       .value(QStringLiteral("snapshot_available")).toBool()
                    != m_negotiatedStableCapabilities.contains(
                        QStringLiteral("timeline.snapshot.current"))) {
                rejectProtocolMessage(QStringLiteral("timeline-retention-gap"));
                return;
            }
            removePendingRequest(id);
            emit timelineRetentionGap(id, projectedData.toObject());
            return;
        }
        removePendingRequest(id);
        emitFailure();
        return;
    }
    const auto *resultValue = transportField(*root, QStringLiteral("result"));
    QJsonValue projectedResult;
    if (!resultValue || !projectTransportValue(*resultValue, &projectedResult)
        || (!projectedResult.isObject()
            && !(pendingMethod == QStringLiteral("shutdown")
                 && projectedResult.isNull()))) {
        rejectProtocolMessage(QStringLiteral("result-type"));
        return;
    }
    const QJsonObject result = projectedResult.toObject();
    if (pendingMethod == QStringLiteral("runtime/heartbeat")) {
        if (id != m_heartbeatRequestId
            || m_heartbeatGeneration != m_processGeneration
            || !isValidHeartbeatResult(result, m_heartbeatNonce)) {
            rejectProtocolMessage(QStringLiteral("heartbeat-response"));
            return;
        }
        removePendingRequest(id);
        m_heartbeatDeadlineTimer->stop();
        m_heartbeatRequestId.clear();
        m_heartbeatDeadlineRequestId.clear();
        m_heartbeatNonce.clear();
        m_heartbeatGeneration = 0;
        m_heartbeatDeadlineGeneration = 0;
        const bool changed = !m_heartbeatHealthy;
        m_heartbeatHealthy = true;
        m_heartbeatRecoveryAttempt = 0;
        m_heartbeatIntervalTimer->start();
        if (changed) {
            emit runtimeLivenessChanged(true, QStringLiteral("运行时响应正常"));
        }
        return;
    }
    const QString subscriptionIdentityStage =
        timelineSubscriptionIdentityStageForMethod(pendingMethod);
    if (!subscriptionIdentityStage.isEmpty()) {
        const auto *subscription = std::get_if<TimelineSubscriptionValidation>(
            &pending.validation);
        if (!subscription || subscription->request.isEmpty()) {
            rejectProtocolMessage(QStringLiteral("timeline-subscription-response"));
            return;
        }
        const QJsonObject request = subscription->request;
        if (pendingMethod == QStringLiteral("timeline/subscribe")
            && !isValidTimelineSubscribeResult(result, request)) {
            rejectProtocolMessage(QStringLiteral("timeline-subscribe-result"));
            return;
        }
        if (pendingMethod == QStringLiteral("timeline/subscription-activate")
            && !isValidTimelineActivateResult(result, request)) {
            rejectProtocolMessage(QStringLiteral("timeline-subscription-active"));
            return;
        }
        removePendingRequest(id);
        if (pendingMethod == QStringLiteral("timeline/subscribe")) {
            emit timelineSubscribed(id, result);
        } else if (pendingMethod == QStringLiteral("timeline/subscription-sync")) {
            emit timelineSubscriptionSynced(id, result);
        } else if (pendingMethod
                   == QStringLiteral("timeline/subscription-snapshot")) {
            emit timelineSubscriptionSnapshotReceived(id, result);
        } else {
            emit timelineSubscriptionActivated(id, result);
        }
        return;
    }
    if (pendingMethod == QStringLiteral("turn/start")) {
        const auto *turn = std::get_if<TurnStartValidation>(&pending.validation);
        const QJsonObject turnRequest = turn
            ? QJsonObject{
                  {QStringLiteral("request_id"), id},
                  {QStringLiteral("session_id"), turn->sessionId},
                  {QStringLiteral("idempotency_key"), turn->idempotencyKey},
                  {QStringLiteral("generation"), static_cast<double>(turn->generation)},
              }
            : QJsonObject{};
        if (!turn || turn->generation != m_processGeneration
                || !isValidTurnStartResult(result, turnRequest)) {
            rejectProtocolMessage(QStringLiteral("turn-start-result"));
            return;
        }
        removePendingRequest(id);
        emit turnStarted(id, turn->generation, result);
        return;
    }
    if (pendingMethod
            == QStringLiteral("session/mutation-acknowledgements")) {
        const auto *list = std::get_if<MutationListValidation>(&pending.validation);
        if (!list || !isValidDurableMutationPage(result, list->request)) {
            rejectProtocolMessage(QStringLiteral("mutation-acknowledgement-page"));
            return;
        }
        removePendingRequest(id);
        emit mutationAcknowledgementsListed(id, result);
        return;
    }
    if (pendingMethod
            == QStringLiteral("mutation/acknowledgement/consume")) {
        const auto *consume = std::get_if<MutationConsumeValidation>(
            &pending.validation);
        if (!consume
            || !isValidDurableMutationConsumeResult(result, consume->request)) {
            rejectProtocolMessage(QStringLiteral("mutation-acknowledgement-consume"));
            return;
        }
        removePendingRequest(id);
        emit mutationAcknowledgementConsumed(id, result);
        return;
    }
    if (pendingMethod == QStringLiteral("artifact/read-command-output-page")) {
        const auto *page = std::get_if<CommandArtifactPageValidation>(
            &pending.validation);
        if (!page || !isValidCommandArtifactPage(result, page->request)) {
            rejectProtocolMessage(QStringLiteral("command-artifact-page"));
            return;
        }
        removePendingRequest(id);
        emit commandArtifactPageRead(id, result);
        return;
    }
    removePendingRequest(id);
    if (pendingMethod == QStringLiteral("project/list")) {
        emit projectsListed(id, result);
    } else if (pendingMethod == QStringLiteral("project/navigation")) {
        emit projectNavigationChanged(id, result);
    } else if (pendingMethod == QStringLiteral("project/open")) {
        const QJsonObject project = result.value(QStringLiteral("project")).toObject();
        const QJsonObject identity = result.value(QStringLiteral("identity")).toObject();
        if (!identity.value(QStringLiteral("relink_required")).toBool()) {
            emit projectOpened(id, project);
        } else {
            emit projectRelinkRequired(id, project, identity);
        }
        const QJsonObject trustReview = result.value(QStringLiteral("trust_review")).toObject();
        if (!trustReview.isEmpty() && !identity.value(QStringLiteral("relink_required")).toBool()) {
            emit projectTrustReviewRequired(id, project, trustReview);
        }
    } else if (pendingMethod == QStringLiteral("project/relink")) {
        emit projectOpened(id, result.value(QStringLiteral("project")).toObject());
    } else if (pendingMethod == QStringLiteral("project/trust-acknowledge")) {
        emit projectTrustAcknowledged(id, result);
    } else if (pendingMethod == QStringLiteral("project/root-list")) {
        emit projectRootsListed(id, result);
    } else if (pendingMethod == QStringLiteral("project/root-add")
               || pendingMethod == QStringLiteral("project/root-remove")) {
        emit projectRootChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/start")) {
        QJsonObject session = result.value(QStringLiteral("session")).toObject();
        session.insert(QStringLiteral("runtime"), result.value(QStringLiteral("runtime")));
        session.insert(QStringLiteral("workspace"), result.value(QStringLiteral("workspace")));
        session.insert(QStringLiteral("context_threshold"),
                       result.value(QStringLiteral("context_threshold")));
        emit sessionStarted(id, session);
    } else if (pendingMethod == QStringLiteral("session/resume")) {
        emit sessionResumed(id, result);
    } else if (pendingMethod == QStringLiteral("session/fork")) {
        emit sessionForked(id, result);
    } else if (pendingMethod == QStringLiteral("session/list")) {
        emit sessionsListed(id, result);
    } else if (pendingMethod == QStringLiteral("session/search")) {
        emit sessionsListed(id, result);
    } else if (pendingMethod == QStringLiteral("session/title")
               || pendingMethod == QStringLiteral("session/archive")
               || pendingMethod == QStringLiteral("session/unarchive")) {
        emit sessionChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/delete/preview")) {
        emit sessionDeletionPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/delete/schedule")
               || pendingMethod == QStringLiteral("session/delete/undo")) {
        emit sessionDeletionChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/deletion/status")) {
        emit sessionDeletionStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("session/export/preview")) {
        emit portableSessionExportPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/export")) {
        emit portableSessionExported(id, result);
    } else if (pendingMethod == QStringLiteral("session/import/preview")) {
        emit portableSessionImportPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/import")) {
        emit portableSessionImported(id, result);
    } else if (pendingMethod == QStringLiteral("retention/maintenance/run")) {
        emit retentionMaintenanceCompleted(id, result);
    } else if (pendingMethod == QStringLiteral("retention/policy/read")) {
        emit retentionPolicyRead(id, result);
    } else if (pendingMethod == QStringLiteral("retention/policy/set")
               || pendingMethod == QStringLiteral("retention/policy/remove")) {
        emit retentionPolicyChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/read")) {
        emit sessionRead(id, result);
    } else if (pendingMethod == QStringLiteral("timeline/sync")) {
        emit timelineSynced(id, result);
    } else if (pendingMethod == QStringLiteral("timeline/snapshot")) {
        emit timelineSnapshotReceived(id, result);
    } else if (pendingMethod == QStringLiteral("session/background-notifications")) {
        emit backgroundNotificationsRead(id, result);
    } else if (pendingMethod == QStringLiteral("session/background-recovery")) {
        emit backgroundRecoveryRead(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/projection-recovery/status")) {
        emit projectionRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("runtime/health")) {
        emit runtimeHealthRead(result);
    } else if (pendingMethod == QStringLiteral("runtime/degradations")) {
        emit runtimeDegradationsRead(id, result);
    } else if (pendingMethod == QStringLiteral("model/catalog")) {
        emit modelCatalogRead(id, result);
    } else if (pendingMethod == QStringLiteral("model/catalog-cache")) {
        emit modelCatalogCacheRead(id, result);
    } else if (pendingMethod == QStringLiteral("model/catalog-refresh-status")) {
        emit modelCatalogRefreshStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("model/capability-check")) {
        emit modelCapabilityChecked(id, result);
    } else if (pendingMethod == QStringLiteral("model/profile/list")) {
        emit modelProfilesListed(id, result);
    } else if (pendingMethod == QStringLiteral("model/profile/read")) {
        emit modelProfileRead(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/restart")) {
        emit runtimeRestarted(id, result);
    } else if (pendingMethod == QStringLiteral("session/recovery/status")) {
        emit sessionRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("operation/status")) {
        emit operationStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("operation/probe")) {
        emit operationProbeRead(id, result);
    } else if (pendingMethod == QStringLiteral("operation/reconcile")) {
        emit operationReconciled(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/create")) {
        emit compactionCheckpointCreated(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/read")) {
        emit compactionCheckpointRead(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/revise")) {
        emit compactionCheckpointRevised(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/recovery/status")) {
        emit runtimeRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("turn/cancel")) {
        emit turnCancellationRequested(id, result);
    } else if (pendingMethod == QStringLiteral("turn/context/inspect")) {
        emit turnContextInspected(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/pinned-context/list")) {
        emit pinnedContextListed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/pinned-context/save")
               || pendingMethod == QStringLiteral("workspace/pinned-context/remove")) {
        emit pinnedContextChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("workspace/image/import-user")) {
        emit pinnedImageImported(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/image/read")) {
        emit pinnedImageRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/list")) {
        emit workspaceListed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/read")) {
        emit workspaceFileRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/save-user-text")) {
        emit workspaceFileSaved(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/metadata")) {
        emit workspaceMetadataRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git-status")) {
        emit workspaceGitStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/overview")) {
        emit gitOverviewRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/log")) {
        emit gitLogRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/commit")) {
        emit gitCommitRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/diff")) {
        emit gitDiffRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/context/read")) {
        emit gitContextRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/search")) {
        emit workspaceSearchCompleted(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/search/cancel")) {
        emit workspaceSearchCancelled(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/index")) {
        emit workspaceIndexed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/index/cancel")) {
        emit workspaceIndexCancelled(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/repository-map")) {
        emit repositoryMapRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-servers")) {
        emit languageServersRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-server/start")) {
        emit languageServerStarted(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-server/stop")) {
        emit languageServerStopped(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/definition")) {
        emit workspaceDefinitionsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/references")) {
        emit workspaceReferencesRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/diagnostics")) {
        emit workspaceDiagnosticsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/observed-diagnostics")) {
        emit observedDiagnosticsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/diagnostics/raw")) {
        emit diagnosticRawRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/preview")) {
        emit workspaceEditPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/artifact/read")) {
        emit workspaceEditArtifactRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/proposal/latest")) {
        emit workspaceEditProposalLatestRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/proposal/read")) {
        emit workspaceEditProposalRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/proposal/artifact/read")) {
        emit workspaceEditProposalArtifactRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/watch")) {
        emit workspaceWatchConfigured(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/watch/poll")) {
        emit workspaceChanged(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/open-user")) {
        emit terminalOpened(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/list")) {
        emit terminalsListed(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/read")
               || pendingMethod == QStringLiteral("terminal/attach")) {
        emit terminalAttached(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/excerpt/read")) {
        emit terminalExcerptRead(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/input-user")) {
        emit terminalInputAccepted(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/resize")) {
        emit terminalResized(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/signal-user")) {
        emit terminalSignalled(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/close-user")
               || pendingMethod == QStringLiteral("terminal/stop-user")) {
        emit terminalStopped(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/restart-user")) {
        emit terminalRestarted(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/remove-user")) {
        emit terminalRemoved(id, result);
    } else if (pendingMethod == QStringLiteral("artifact/read-command-output")) {
        emit commandArtifactRead(id, result);
    }
}

void AgentRuntimeClient::acceptInitializeResponse(const QJsonObject &result)
{
    m_startupTimer->stop();
    m_startupGeneration = 0;
    m_handshakeComplete = true;
    const QJsonObject runtime = result.value(QStringLiteral("runtime")).toObject();
    const QJsonObject backend = result.value(QStringLiteral("backend")).toObject();
    const QString backendStatus = backend.value(QStringLiteral("status")).toString();
    m_recoveryMode = backendStatus == QStringLiteral("read-only-recovery");
    m_ready = backendStatus == QStringLiteral("ready") || m_recoveryMode;
    m_heartbeatNegotiated = m_negotiatedStableCapabilities.contains(
        QStringLiteral("runtime.heartbeat.out-of-band"));
    m_heartbeatHealthy = m_heartbeatNegotiated;

    if (!sendNotification(QStringLiteral("initialized"))) {
        rejectProtocolMessage(QStringLiteral("initialized-write"));
        return;
    }
    const QString detail = QStringLiteral("%1 %2 · %3 %4 · AAP %5")
        .arg(runtime.value(QStringLiteral("name")).toString(),
             runtime.value(QStringLiteral("version")).toString(),
             backend.value(QStringLiteral("adapter")).toString(),
             backend.value(QStringLiteral("version")).toString(),
             result.value(QStringLiteral("protocol")).toObject()
                 .value(QStringLiteral("selected")).toString());
    if (m_heartbeatNegotiated) {
        emit runtimeLivenessChanged(true, QStringLiteral("运行时响应正常"));
        sendHeartbeat(false);
        if (m_heartbeatHealthy) m_heartbeatIntervalTimer->start();
    }

    if (m_reconnectCycleActive) {
        m_reconnectRecoveryPending = true;
        m_reconnectInitializeResult = result;
        const QString recoveryDetail = QStringLiteral(
            "运行时握手已恢复，正在校验工作区状态");
        emit connectionStateChanged(false, recoveryDetail);
        emit reconnectHandshakeReady(m_processGeneration, result);
        return;
    }

    emit runtimeInitialized(result);
    emit connectionStateChanged(m_ready, detail);

    if (containsCapability(m_negotiatedStableCapabilities, "runtime.health")) runtimeHealth();
    if (containsCapability(m_negotiatedStableCapabilities, "runtime.degradations")) {
        runtimeDegradations();
    }
    if (containsCapability(m_negotiatedStableCapabilities, "model.catalog.read-only")) {
        modelCatalog();
    }
    if (containsCapability(m_negotiatedStableCapabilities,
                           "model.catalog.cache.read-only")) {
        modelCatalogCache();
    }
    if (containsCapability(m_negotiatedStableCapabilities,
                           "model.catalog.refresh.status.read-only")) {
        modelCatalogRefreshStatus();
    }
    if (containsCapability(m_negotiatedStableCapabilities, "model.profile.read-only")) {
        listModelProfiles();
    }
    if (m_recoveryMode
        && containsCapability(m_negotiatedStableCapabilities,
                              "runtime.recovery.status")) {
        runtimeRecoveryStatus();
    } else if (!m_recoveryMode
               && containsCapability(m_negotiatedStableCapabilities,
                                     "runtime.projection-recovery.status")) {
        projectionRecoveryStatus();
    }
}

void AgentRuntimeClient::rejectInitializeResponse(const QString &reasonCode)
{
    m_startupTimer->stop();
    const bool reconnecting = m_reconnectCycleActive;
    const QString requestId = m_initializeRequestId;
    if (!requestId.isEmpty()) {
        retireResponseId(requestId);
        removePendingRequest(requestId);
    }
    suppressAutomaticReconnect();
    m_discardProcessOutput = true;
    clearNegotiationState();
    const QString detail = QStringLiteral("运行时握手响应无效（%1）").arg(reasonCode);
    if (reconnecting) setReconnectState(ReconnectState::Exhausted, 0, detail);
    reportRequestFailure(requestId, QStringLiteral("initialize"), detail, -32003);
    emit connectionStateChanged(false, detail);
    closeTransportWrite();
}

void AgentRuntimeClient::reportRequestFailure(const QString &requestId,
                                              const QString &method,
                                              const QString &message,
                                              int code)
{
    // The QString signal is the lossless production contract. Keep the legacy
    // int signal only for clients that still consume it, and never synthesize
    // an int when the wire code cannot be represented exactly.
    emit requestFailedExact(requestId, method, message, QString::number(code));
    emit requestFailed(requestId, method, message, code);
}

void AgentRuntimeClient::rejectProtocolMessage(const QString &reasonCode)
{
    m_startupTimer->stop();
    const bool reconnecting = m_reconnectCycleActive;
    suppressAutomaticReconnect();
    m_discardProcessOutput = true;
    clearNegotiationState();
    const QString detail = QStringLiteral("运行时协议消息无效（%1）").arg(reasonCode);
    if (reconnecting) setReconnectState(ReconnectState::Exhausted, 0, detail);
    failPending(detail);
    emit connectionStateChanged(false, detail);
    closeTransportWrite();
}

void AgentRuntimeClient::clearNegotiationState()
{
    const bool livenessWasKnown = m_heartbeatNegotiated && m_heartbeatHealthy;
    m_startupTimer->stop();
    m_startupGeneration = 0;
    m_heartbeatIntervalTimer->stop();
    m_heartbeatDeadlineTimer->stop();
    if (!m_heartbeatRequestId.isEmpty()) {
        retireResponseId(m_heartbeatRequestId);
        removePendingRequest(m_heartbeatRequestId);
    }
    m_ready = false;
    m_heartbeatNegotiated = false;
    m_heartbeatHealthy = false;
    m_recoveryMode = false;
    m_handshakeComplete = false;
    m_initializeRequestId.clear();
    m_initializeGeneration = 0;
    m_heartbeatRequestId.clear();
    m_heartbeatDeadlineRequestId.clear();
    m_heartbeatNonce.clear();
    m_heartbeatGeneration = 0;
    m_heartbeatDeadlineGeneration = 0;
    m_heartbeatRecoveryAttempt = 0;
    m_reconnectStabilityGeneration = 0;
    m_reconnectRecoveryPending = false;
    m_reconnectInitializeResult = {};
    m_negotiatedStableCapabilities.clear();
    m_negotiatedMaximumFrameBytes = 0;
    m_unixSocketPeerVerifiedGeneration = 0;
    m_localSocketPeerVerifiedAttemptEpoch = 0;
    if (livenessWasKnown) {
        emit runtimeLivenessChanged(false, QStringLiteral("运行时连接不可用"));
    }
}

void AgentRuntimeClient::failPending(const QString &message)
{
    const auto pending = m_pendingRequests;
    m_pendingRequests.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        retireResponseId(it.key());
        reportRequestFailure(it.key(), it.value().transport.method, message, -1);
    }
}

void AgentRuntimeClient::failOrdinaryPending(const QString &message)
{
    const auto pending = m_pendingRequests;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (isLivenessControlMethod(it.value().transport.method)) continue;
        retireResponseId(it.key());
        removePendingRequest(it.key());
        reportRequestFailure(it.key(), it.value().transport.method, message, -1);
    }
}

void AgentRuntimeClient::abandonAmbiguousTimelineSubscriptionConnection(
    const QString &detail)
{
    const quint64 generation = m_processGeneration;
    const bool livenessChanged = m_heartbeatHealthy;
    const bool canReconnect = !m_stopping && !m_autoReconnectSuppressed
        && m_reconnectAttempt < maximumReconnectAttempts();

    m_heartbeatHealthy = false;
    m_discardProcessOutput = true;
    if (canReconnect) {
        m_reconnectTimer->stop();
        m_reconnectStabilityTimer->stop();
        m_reconnectStabilityGeneration = 0;
        m_reconnectScheduledGeneration = 0;
        m_reconnectCycleActive = true;
        ++m_reconnectAttempt;
        m_reconnectTerminationPending = true;
        m_reconnectTerminationGeneration = generation;
    } else {
        suppressAutomaticReconnect();
    }

    // A request may have reached Runtime even though its response is no longer
    // trustworthy. With no unsubscribe contract, only a new process generation
    // can retire the connection-owned subscription attempt without guessing.
    clearNegotiationState();
    failPending(detail);
    if (canReconnect) {
        setReconnectState(ReconnectState::Restarting, 0,
                          QStringLiteral("Timeline 订阅所有权未知，正在启动新的本地运行时代际"));
    } else {
        setReconnectState(ReconnectState::Exhausted, 0, detail);
    }
    if (livenessChanged) emit runtimeLivenessChanged(false, detail);
    emit connectionStateChanged(false, detail);

    if (m_process->state() == QProcess::NotRunning) {
        if (canReconnect) {
            m_reconnectTerminationPending = false;
            m_reconnectTerminationGeneration = 0;
            launchProcess(true);
        }
        return;
    }

    closeTransportWrite();
    m_process->terminate();
    QTimer::singleShot(kReconnectTerminationGraceMs, this,
                       [this, generation, canReconnect]() {
        if (generation != m_processGeneration
            || m_process->state() == QProcess::NotRunning) {
            return;
        }
        const bool terminationStillOwned = canReconnect
            ? (m_reconnectTerminationPending
               && m_reconnectTerminationGeneration == generation)
            : m_autoReconnectSuppressed;
        if (terminationStillOwned) m_process->kill();
    });
}

void AgentRuntimeClient::abandonTimelineSubscriptionConnection(
    const QString &detail)
{
    if (m_stopping || m_reconnectTerminationPending
            || m_processTerminationPending || m_autoReconnectSuppressed) {
        return;
    }
    abandonAmbiguousTimelineSubscriptionConnection(detail.isEmpty()
        ? QStringLiteral("Timeline 订阅状态无法在当前连接上安全恢复")
        : detail);
}

void AgentRuntimeClient::sendHeartbeat(bool recoveryProbe)
{
    if (!m_heartbeatNegotiated
        || !m_handshakeComplete
        || m_process->state() == QProcess::NotRunning
        || !m_heartbeatRequestId.isEmpty()
        || (!m_heartbeatHealthy && !recoveryProbe)) {
        return;
    }
    if (++m_nextHeartbeatNonce == 0) ++m_nextHeartbeatNonce;
    const QString nonce = QStringLiteral("%1-%2")
        .arg(m_processGeneration)
        .arg(m_nextHeartbeatNonce);
    const QString requestId = sendRequest(QStringLiteral("runtime/heartbeat"), {
        {QStringLiteral("schema_version"),
         QStringLiteral("runtime-heartbeat-request/0.1")},
        {QStringLiteral("nonce"), nonce},
    });
    if (requestId.isEmpty()) {
        if (m_autoReconnectSuppressed) return;
        m_heartbeatIntervalTimer->stop();
        const bool changed = m_heartbeatHealthy;
        m_heartbeatHealthy = false;
        const QString detail = QStringLiteral(
            "运行时控制通道写入失败，需显式停止后重试");
        failOrdinaryPending(detail);
        if (changed) emit runtimeLivenessChanged(false, detail);
        suppressAutomaticReconnect();
        m_discardProcessOutput = true;
        clearNegotiationState();
        failPending(detail);
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        return;
    }
    m_heartbeatRequestId = requestId;
    m_heartbeatDeadlineRequestId = requestId;
    m_heartbeatNonce = nonce;
    m_heartbeatGeneration = m_processGeneration;
    m_heartbeatDeadlineGeneration = m_processGeneration;
    m_heartbeatDeadlineTimer->start();
}

void AgentRuntimeClient::handleHeartbeatTimeout()
{
    if (!m_heartbeatNegotiated || !m_handshakeComplete
        || m_heartbeatRequestId.isEmpty()
        || m_heartbeatDeadlineRequestId != m_heartbeatRequestId
        || m_heartbeatDeadlineGeneration != m_heartbeatGeneration
        || m_heartbeatDeadlineGeneration != m_processGeneration) {
        return;
    }
    bool subscriptionOwnershipUnknown = false;
    for (auto it = m_pendingRequests.cbegin(); it != m_pendingRequests.cend(); ++it) {
        if (it.value().processGeneration == m_processGeneration
            && !timelineSubscriptionIdentityStageForMethod(
                    it.value().transport.method).isEmpty()) {
            subscriptionOwnershipUnknown = true;
            break;
        }
    }
    retireResponseId(m_heartbeatRequestId);
    removePendingRequest(m_heartbeatRequestId);
    m_heartbeatRequestId.clear();
    m_heartbeatDeadlineRequestId.clear();
    m_heartbeatNonce.clear();
    m_heartbeatGeneration = 0;
    m_heartbeatDeadlineGeneration = 0;
    m_heartbeatIntervalTimer->stop();
    m_heartbeatDeadlineTimer->stop();
    if (subscriptionOwnershipUnknown) {
        abandonAmbiguousTimelineSubscriptionConnection(QStringLiteral(
            "Timeline 订阅请求的连接所有权未知，已放弃当前运行时连接"));
        return;
    }
    const bool changed = m_heartbeatHealthy;
    m_heartbeatHealthy = false;
    const QString detail = QStringLiteral("运行时响应状态未知（心跳超时）");
    failOrdinaryPending(detail);
    if (changed) emit runtimeLivenessChanged(false, detail);
    scheduleHeartbeatRecoveryProbe();
}

void AgentRuntimeClient::scheduleHeartbeatRecoveryProbe()
{
    static const QList<int> recoveryBackoffMs{0, 500, 2000};
    if (!m_heartbeatNegotiated || m_heartbeatHealthy || !m_handshakeComplete
        || m_process->state() == QProcess::NotRunning
        || m_stopping || m_autoReconnectSuppressed) {
        return;
    }
    if (m_heartbeatRecoveryAttempt >= recoveryBackoffMs.size()) {
        const QString detail = QStringLiteral(
            "运行时心跳恢复探针已耗尽，控制连接保持不变");
        emit heartbeatRecoveryExhausted(m_heartbeatRecoveryAttempt, detail);
        return;
    }
    const int attempt = ++m_heartbeatRecoveryAttempt;
    const int delayMs = recoveryBackoffMs.at(attempt - 1);
    const quint64 generation = m_processGeneration;
    QTimer::singleShot(delayMs, this, [this, attempt, generation]() {
        if (generation != m_processGeneration
            || attempt != m_heartbeatRecoveryAttempt
            || m_heartbeatHealthy || !m_handshakeComplete
            || m_process->state() == QProcess::NotRunning
            || m_stopping || m_autoReconnectSuppressed
            || !m_heartbeatRequestId.isEmpty()) {
            return;
        }
        sendHeartbeat(true);
    });
}

void AgentRuntimeClient::handleRetryableProcessFailure(const QString &detail)
{
    if (m_stopping || m_autoReconnectSuppressed) return;
    if (m_process->state() != QProcess::NotRunning) {
        suppressAutomaticReconnect();
        m_discardProcessOutput = true;
        clearNegotiationState();
        failPending(detail);
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        return;
    }

    scheduleReconnect(detail);
    clearNegotiationState();
    failPending(detail);
    emit connectionStateChanged(false, detail);
}

void AgentRuntimeClient::scheduleReconnect(const QString &detail)
{
    if (m_stopping || m_autoReconnectSuppressed) return;
    if (m_process->state() != QProcess::NotRunning) {
        suppressAutomaticReconnect();
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        return;
    }
    if (m_reconnectState == ReconnectState::Waiting
        && m_reconnectTimer->isActive()) {
        return;
    }

    m_reconnectStabilityTimer->stop();
    m_reconnectStabilityGeneration = 0;
    m_reconnectCycleActive = true;
    if (m_reconnectAttempt >= maximumReconnectAttempts()) {
        m_reconnectTimer->stop();
        m_reconnectScheduledGeneration = 0;
        const QString exhaustedDetail = QStringLiteral(
            "运行时自动重连已耗尽（%1/%2）：%3")
            .arg(m_reconnectAttempt)
            .arg(maximumReconnectAttempts())
            .arg(detail);
        setReconnectState(ReconnectState::Exhausted, 0, exhaustedDetail);
        return;
    }

    ++m_reconnectAttempt;
    const int delayMs = m_reconnectBackoffMs.at(m_reconnectAttempt - 1);
    m_reconnectScheduledGeneration = m_processGeneration;
    const QString waitingDetail = QStringLiteral(
        "运行时将在 %1 毫秒后执行第 %2/%3 次重连")
        .arg(delayMs)
        .arg(m_reconnectAttempt)
        .arg(maximumReconnectAttempts());
    setReconnectState(ReconnectState::Waiting, delayMs, waitingDetail);
    m_reconnectTimer->start(delayMs);
}

void AgentRuntimeClient::beginReconnectAttempt()
{
    if (m_stopping || m_autoReconnectSuppressed
        || m_reconnectState != ReconnectState::Waiting
        || m_reconnectScheduledGeneration != m_processGeneration) {
        return;
    }
    m_reconnectScheduledGeneration = 0;
    if (m_process->state() != QProcess::NotRunning) {
        const QString detail = QStringLiteral(
            "运行时进程仍在运行，自动重连已停止");
        suppressAutomaticReconnect();
        setReconnectState(ReconnectState::Exhausted, 0, detail);
        emit connectionStateChanged(false, detail);
        return;
    }

    setReconnectState(ReconnectState::Restarting, 0,
                      QStringLiteral("正在启动新的本地运行时代际"));
    launchProcess(true);
}

void AgentRuntimeClient::suppressAutomaticReconnect()
{
    m_reconnectTimer->stop();
    m_reconnectStabilityTimer->stop();
    m_reconnectStabilityGeneration = 0;
    m_reconnectScheduledGeneration = 0;
    m_reconnectTerminationPending = false;
    m_reconnectTerminationGeneration = 0;
    m_reconnectCycleActive = false;
    m_autoReconnectSuppressed = true;
}

void AgentRuntimeClient::setReconnectState(ReconnectState state,
                                           int nextDelayMs,
                                           const QString &detail)
{
    m_reconnectState = state;
    emit runtimeReconnectStateChanged(state, m_reconnectAttempt,
                                      maximumReconnectAttempts(),
                                      qMax(0, nextDelayMs), detail);
}

void AgentRuntimeClient::retireResponseId(const QString &requestId)
{
    if (requestId.isEmpty() || m_retiredResponseIds.contains(requestId)) return;
    m_retiredResponseIds.insert(requestId);
    m_retiredResponseOrder.append(requestId);
    while (m_retiredResponseOrder.size() > kMaximumRetiredResponseIds) {
        m_retiredResponseIds.remove(m_retiredResponseOrder.takeFirst());
    }
}

void AgentRuntimeClient::removePendingRequest(const QString &requestId)
{
    m_pendingRequests.remove(requestId);
}
