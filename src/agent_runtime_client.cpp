#include "agent_runtime_client.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <cmath>
#include <limits>

namespace {
constexpr int kStartupTimeoutMs = 5000;
constexpr int kMaximumFrameBytes = 4 * 1024 * 1024;
constexpr int kMaximumIdentityBytes = 64;
constexpr int kMaximumMethodBytes = 128;
constexpr int kMaximumRequestIdBytes = 128;
constexpr int kMaximumCapabilities = 128;
constexpr int kMaximumCapabilityBytes = 128;
constexpr int kMaximumErrorMessageBytes = 2048;
constexpr int kMaximumTimelineIdentityBytes = 128;
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr int kMaximumTimelineDataDepth = 16;
constexpr qsizetype kMaximumTimelineDataNodes = 4096;
constexpr qsizetype kMaximumTimelineDataObjectProperties = 128;
constexpr qsizetype kMaximumTimelineDataArrayItems = 4096;

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

bool hasExactKeys(const QJsonObject &object, const QStringList &keys)
{
    QSet<QString> expected(keys.cbegin(), keys.cend());
    QSet<QString> actual;
    const QStringList actualKeys = object.keys();
    for (const QString &key : actualKeys) actual.insert(key);
    return actual == expected;
}

bool isCanonicalRequestId(const QJsonValue &value)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern(QStringLiteral("^[1-9][0-9]*$"));
    const QString id = value.toString();
    return id.toUtf8().size() <= kMaximumRequestIdBytes
        && pattern.match(id).hasMatch();
}

bool isValidErrorObject(const QJsonValue &value)
{
    if (!value.isObject()) return false;
    const QJsonObject error = value.toObject();
    const bool hasData = error.contains(QStringLiteral("data"));
    if (!hasExactKeys(error, hasData
            ? QStringList{QStringLiteral("code"), QStringLiteral("message"),
                          QStringLiteral("data")}
            : QStringList{QStringLiteral("code"), QStringLiteral("message")})) {
        return false;
    }
    const QJsonValue code = error.value(QStringLiteral("code"));
    const QJsonValue message = error.value(QStringLiteral("message"));
    if (!code.isDouble() || code.toDouble() != std::floor(code.toDouble())
        || code.toDouble() < std::numeric_limits<int>::min()
        || code.toDouble() > std::numeric_limits<int>::max()
        || !message.isString()
        || message.toString().isEmpty()
        || message.toString().toUtf8().size() > kMaximumErrorMessageBytes) {
        return false;
    }
    return !hasData || error.value(QStringLiteral("data")).isObject();
}

bool isValidMethodName(const QJsonValue &value)
{
    if (!value.isString()) return false;
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z][a-z0-9.-]*(?:/[a-z][a-z0-9.-]*)*$"));
    const QString method = value.toString();
    return method.toUtf8().size() <= kMaximumMethodBytes
        && pattern.match(method).hasMatch();
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

    static const QRegularExpression eventIdPattern(
        QStringLiteral("^event:sha256:[0-9a-f]{64}$"));
    if (!event.value(QStringLiteral("event_id")).isString()
        || !eventIdPattern.match(
                event.value(QStringLiteral("event_id")).toString()).hasMatch()
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

bool validateInitializeResult(const QJsonObject &result,
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
        || securityValue.toObject() != stdioTransportSecurity()) {
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

AgentRuntimeClient::AgentRuntimeClient(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_startupTimer(new QTimer(this))
{
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this]() {
        if (!m_ready && m_process->state() != QProcess::NotRunning) {
            emit connectionStateChanged(false, QStringLiteral("运行时握手超时"));
            m_process->kill();
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AgentRuntimeClient::processStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString output = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!output.isEmpty()) emit diagnosticMessage(output);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        clearNegotiationState();
        const QString detail = QStringLiteral("运行时启动失败：%1").arg(m_process->errorString());
        failPending(detail);
        emit connectionStateChanged(false, detail);
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        m_startupTimer->stop();
        const bool expected = m_stopping;
        m_stopping = false;
        clearNegotiationState();
        const QString detail = expected
            ? QStringLiteral("运行时已停止")
            : QStringLiteral("运行时已退出（%1，代码 %2）")
                  .arg(status == QProcess::CrashExit ? QStringLiteral("异常")
                                                      : QStringLiteral("正常"))
                  .arg(exitCode);
        failPending(detail);
        emit connectionStateChanged(false, detail);
    });
}

AgentRuntimeClient::~AgentRuntimeClient()
{
    stop();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->waitForFinished(500);
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

bool AgentRuntimeClient::isReady() const
{
    return m_ready;
}

bool AgentRuntimeClient::isRecoveryMode() const
{
    return m_recoveryMode;
}

QString AgentRuntimeClient::runtimePath() const
{
    return m_runtimePath;
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
    if (m_process->state() != QProcess::NotRunning) return;
    m_runtimePath = locateRuntime();
    if (m_runtimePath.isEmpty()) {
        emit connectionStateChanged(
            false, QStringLiteral("未找到 aegisy-agentd，请先构建 agent-runtime"));
        return;
    }

    clearNegotiationState();
    m_stopping = false;
    m_stdoutBuffer.clear();
    m_pendingMethods.clear();
    emit connectionStateChanged(false, QStringLiteral("正在连接本地运行时…"));
    QProcessEnvironment environment = sanitizedSidecarEnvironment(
        QProcessEnvironment::systemEnvironment());
    if (environment.value(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT")).isEmpty()) {
        const QString dataRoot = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .absoluteFilePath(QStringLiteral("workbench"));
        if (!QDir().mkpath(dataRoot)) {
            emit connectionStateChanged(false, QStringLiteral("无法创建工作台数据目录"));
            return;
        }
        environment.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"), dataRoot);
    }
    m_process->setProgram(m_runtimePath);
    m_process->setArguments({});
    m_process->setProcessEnvironment(environment);
    m_process->start();
    if (!m_process->waitForStarted(1000)) return;
    m_startupTimer->start(kStartupTimeoutMs);

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
        {QStringLiteral("transport_security"), stdioTransportSecurity()},
    };
    m_initializeRequestId = sendRequest(QStringLiteral("initialize"), params);
}

void AgentRuntimeClient::stop()
{
    if (m_process->state() == QProcess::NotRunning) return;
    m_stopping = true;
    if (m_ready) sendRequest(QStringLiteral("shutdown"));
    else m_process->terminate();
}

QString AgentRuntimeClient::runtimeHealth()
{
    return sendRequest(QStringLiteral("runtime/health"));
}

QString AgentRuntimeClient::runtimeDegradations()
{
    return sendRequest(QStringLiteral("runtime/degradations"));
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
                                      const QStringList &pinnedContextIds)
{
    ++m_nextTurnKey;
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("input"), input},
        {QStringLiteral("context"), context},
        {QStringLiteral("idempotency_key"),
         QStringLiteral("qt-turn-%1").arg(m_nextTurnKey)},
    };
    if (!pinnedContextIds.isEmpty()) {
        params.insert(QStringLiteral("pinned_context_set_identity"),
                      pinnedContextSetIdentity);
        params.insert(QStringLiteral("pinned_context_ids"),
                      QJsonArray::fromStringList(pinnedContextIds));
    }
    return sendRequest(QStringLiteral("turn/start"), params);
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

QString AgentRuntimeClient::sendRequest(const QString &method, const QJsonObject &params)
{
    if (m_process->state() == QProcess::NotRunning) {
        emit requestFailed({}, method, QStringLiteral("本地运行时未启动"), -1);
        return {};
    }
    if (method != QStringLiteral("initialize") && !m_handshakeComplete) {
        emit requestFailed({}, method, QStringLiteral("本地运行时握手尚未完成"), -32003);
        return {};
    }
    if (method != QStringLiteral("initialize")) {
        const QStringList required = requiredCapabilitiesForMethod(method, params);
        for (const QString &capability : required) {
            if (!m_negotiatedStableCapabilities.contains(capability)) {
                emit requestFailed({}, method,
                                   QStringLiteral("本地运行时未协商此操作所需能力"),
                                   -32601);
                return {};
            }
        }
    }
    const QString id = QString::number(++m_nextRequestId);
    const int writeError = writeMessage({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
    if (writeError != 0) {
        emit requestFailed(
            id, method,
            writeError == -32005
                ? QStringLiteral("请求超过 AAP 帧上限")
                : QStringLiteral("无法写入本地运行时"),
            writeError);
        return {};
    }
    m_pendingMethods.insert(id, method);
    return id;
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
    return m_process->write(frame) == frame.size() ? 0 : -1;
}

void AgentRuntimeClient::processStdout()
{
    m_stdoutBuffer.append(m_process->readAllStandardOutput());
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
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
                rejectInitializeResponse(QStringLiteral("invalid-json"));
            } else {
                rejectProtocolMessage(QStringLiteral("invalid-json"));
            }
            return;
        }
        processMessage(document.object());
    }
}

void AgentRuntimeClient::processMessage(const QJsonObject &message)
{
    if (message.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")) {
        if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
            rejectInitializeResponse(QStringLiteral("jsonrpc-version"));
        } else {
            rejectProtocolMessage(QStringLiteral("jsonrpc-version"));
        }
        return;
    }

    if (!m_handshakeComplete && !m_initializeRequestId.isEmpty()) {
        const QJsonValue idValue = message.value(QStringLiteral("id"));
        const bool hasResult = message.contains(QStringLiteral("result"));
        const bool hasError = message.contains(QStringLiteral("error"));
        if (!isCanonicalRequestId(idValue)
            || idValue.toString() != m_initializeRequestId) {
            rejectInitializeResponse(QStringLiteral("response-id"));
            return;
        }
        if (hasResult == hasError
            || !hasExactKeys(message, hasResult
                ? QStringList{QStringLiteral("jsonrpc"), QStringLiteral("id"),
                              QStringLiteral("result")}
                : QStringList{QStringLiteral("jsonrpc"), QStringLiteral("id"),
                              QStringLiteral("error")})) {
            rejectInitializeResponse(QStringLiteral("response-shape"));
            return;
        }
        if (hasError) {
            if (!isValidErrorObject(message.value(QStringLiteral("error")))) {
                rejectInitializeResponse(QStringLiteral("error-shape"));
                return;
            }
            QString reasonCode;
            if (!validateInitializeError(message.value(QStringLiteral("error")).toObject(),
                                         &reasonCode)) {
                rejectInitializeResponse(reasonCode);
                return;
            }
            rejectInitializeResponse(reasonCode);
            return;
        }
        const QJsonValue resultValue = message.value(QStringLiteral("result"));
        if (!resultValue.isObject()) {
            rejectInitializeResponse(QStringLiteral("result-type"));
            return;
        }
        QString reasonCode;
        QSet<QString> stableCapabilities;
        int maximumFrameBytes = 0;
        const QJsonObject result = resultValue.toObject();
        if (!validateInitializeResult(result, &stableCapabilities,
                                      &maximumFrameBytes, &reasonCode)) {
            rejectInitializeResponse(reasonCode);
            return;
        }
        m_pendingMethods.remove(m_initializeRequestId);
        m_initializeRequestId.clear();
        m_negotiatedStableCapabilities = stableCapabilities;
        m_negotiatedMaximumFrameBytes = maximumFrameBytes;
        acceptInitializeResponse(result);
        return;
    }

    if (!m_handshakeComplete) {
        rejectProtocolMessage(QStringLiteral("message-before-handshake"));
        return;
    }

    if (message.contains(QStringLiteral("method"))) {
        if (!isValidMethodName(message.value(QStringLiteral("method")))
            || !hasExactKeys(message, {
                QStringLiteral("jsonrpc"), QStringLiteral("method"),
                QStringLiteral("params"),
            })
            || !message.value(QStringLiteral("params")).isObject()) {
            rejectProtocolMessage(QStringLiteral("notification-shape"));
            return;
        }
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("event")) {
            if (!m_negotiatedStableCapabilities.contains(
                    QStringLiteral("timeline.streaming"))) {
                rejectProtocolMessage(QStringLiteral("notification-capability"));
                return;
            }
            const QJsonObject event = message.value(QStringLiteral("params")).toObject();
            if (!isValidTimelineEventEnvelope(event)) {
                rejectProtocolMessage(QStringLiteral("event-envelope"));
                return;
            }
            emit timelineEvent(event);
        } else {
            emit diagnosticMessage(QStringLiteral("忽略未支持的 AAP 通知"));
        }
        return;
    }

    const QJsonValue idValue = message.value(QStringLiteral("id"));
    const bool hasResult = message.contains(QStringLiteral("result"));
    const bool hasError = message.contains(QStringLiteral("error"));
    if (!isCanonicalRequestId(idValue) || hasResult == hasError
        || !hasExactKeys(message, hasResult
            ? QStringList{QStringLiteral("jsonrpc"), QStringLiteral("id"),
                          QStringLiteral("result")}
            : QStringList{QStringLiteral("jsonrpc"), QStringLiteral("id"),
                          QStringLiteral("error")})) {
        rejectProtocolMessage(QStringLiteral("response-shape"));
        return;
    }
    const QString id = idValue.toString();
    const QString pendingMethod = m_pendingMethods.value(id);
    if (pendingMethod.isEmpty()) {
        rejectProtocolMessage(QStringLiteral("response-correlation"));
        return;
    }
    if (hasError) {
        if (!isValidErrorObject(message.value(QStringLiteral("error")))) {
            rejectProtocolMessage(QStringLiteral("error-shape"));
            return;
        }
        const QJsonObject error = message.value(QStringLiteral("error")).toObject();
        m_pendingMethods.remove(id);
        emit requestFailed(id, pendingMethod,
                           error.value(QStringLiteral("message")).toString(),
                           error.value(QStringLiteral("code")).toInt(-1));
        return;
    }
    const QJsonValue resultValue = message.value(QStringLiteral("result"));
    if (!resultValue.isObject()
        && !(pendingMethod == QStringLiteral("shutdown") && resultValue.isNull())) {
        rejectProtocolMessage(QStringLiteral("result-type"));
        return;
    }
    m_pendingMethods.remove(id);
    const QJsonObject result = resultValue.toObject();
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
    m_handshakeComplete = true;
    const QJsonObject runtime = result.value(QStringLiteral("runtime")).toObject();
    const QJsonObject backend = result.value(QStringLiteral("backend")).toObject();
    const QString backendStatus = backend.value(QStringLiteral("status")).toString();
    m_recoveryMode = backendStatus == QStringLiteral("read-only-recovery");
    m_ready = backendStatus == QStringLiteral("ready") || m_recoveryMode;

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
    const QString requestId = m_initializeRequestId;
    if (!requestId.isEmpty()) m_pendingMethods.remove(requestId);
    clearNegotiationState();
    const QString detail = QStringLiteral("运行时握手响应无效（%1）").arg(reasonCode);
    emit requestFailed(requestId, QStringLiteral("initialize"), detail, -32003);
    emit connectionStateChanged(false, detail);
    if (m_process->state() != QProcess::NotRunning) m_process->kill();
}

void AgentRuntimeClient::rejectProtocolMessage(const QString &reasonCode)
{
    m_startupTimer->stop();
    clearNegotiationState();
    const QString detail = QStringLiteral("运行时协议消息无效（%1）").arg(reasonCode);
    failPending(detail);
    emit connectionStateChanged(false, detail);
    if (m_process->state() != QProcess::NotRunning) m_process->kill();
}

void AgentRuntimeClient::clearNegotiationState()
{
    m_ready = false;
    m_recoveryMode = false;
    m_handshakeComplete = false;
    m_initializeRequestId.clear();
    m_negotiatedStableCapabilities.clear();
    m_negotiatedMaximumFrameBytes = 0;
}

void AgentRuntimeClient::failPending(const QString &message)
{
    const auto pending = m_pendingMethods;
    m_pendingMethods.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit requestFailed(it.key(), it.value(), message, -1);
    }
}
