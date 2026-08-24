#include "mcp_configuration_inventory.h"
#include "strict_json_validator.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace {

QString hashIdentity(const QByteArray &domain, const QByteArray &bytes,
                     const QString &prefix)
{
    QByteArray input = domain;
    const quint64 size = static_cast<quint64>(bytes.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.append(static_cast<char>((size >> shift) & 0xff));
    }
    input.append(bytes);
    return prefix + QString::fromLatin1(QCryptographicHash::hash(
        input, QCryptographicHash::Sha256).toHex());
}

bool safeScalar(const QString &value, int maximum, bool allowEmpty = false)
{
    if ((!allowEmpty && value.isEmpty()) || value.size() > maximum) return false;
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    return true;
}

bool validUrl(const QString &value)
{
    if (!safeScalar(value, 4096)) return false;
    const QUrl url(value, QUrl::StrictMode);
    if (!url.isValid() || !url.userInfo().isEmpty() || !url.fragment().isEmpty()) {
        return false;
    }
    if (url.scheme() == QStringLiteral("https")) return !url.host().isEmpty();
    if (url.scheme() != QStringLiteral("http")) return false;
    const QString host = url.host().toLower();
    return host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1");
}

bool validateStdio(const QJsonObject &server)
{
    const QSet<QString> allowed{QStringLiteral("command"), QStringLiteral("args"),
                                QStringLiteral("env")};
    const QStringList keys = server.keys();
    if (!QSet<QString>(keys.cbegin(), keys.cend()).subtract(allowed).isEmpty()
            || !server.value(QStringLiteral("command")).isString()) return false;
    const QString command = server.value(QStringLiteral("command")).toString();
    if (!safeScalar(command, 4096) || command.contains(QRegularExpression(
            QStringLiteral("[\\s;&|`$<>]")))) return false;
    if (server.contains(QStringLiteral("args"))) {
        if (!server.value(QStringLiteral("args")).isArray()) return false;
        const QJsonArray args = server.value(QStringLiteral("args")).toArray();
        if (args.size() > 64) return false;
        for (const QJsonValue &argument : args) {
            if (!argument.isString() || !safeScalar(argument.toString(), 4096, true)) {
                return false;
            }
        }
    }
    if (server.contains(QStringLiteral("env"))) {
        if (!server.value(QStringLiteral("env")).isObject()) return false;
        const QJsonObject env = server.value(QStringLiteral("env")).toObject();
        if (env.size() > 64) return false;
        for (auto it = env.begin(); it != env.end(); ++it) {
            if (!QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,127}$"))
                        .match(it.key()).hasMatch()
                    || !it.value().isString()
                    || !safeScalar(it.value().toString(), 4096, true)) return false;
        }
    }
    return true;
}

} // namespace

McpConfigurationInventoryResult McpConfigurationInventory::inspectFile(
    const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) {
        return {McpConfigurationInventoryState::Invalid, {}, {}, {},
                QStringLiteral("mcp-config-file-invalid")};
    }
    if (!info.exists()) return inspectBytes({}, false);
    if (!info.isFile() || info.size() < 0
            || info.size() > MaxFileBytes) {
        return {McpConfigurationInventoryState::Invalid, {}, {}, {},
                QStringLiteral("mcp-config-file-invalid")};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {McpConfigurationInventoryState::Unavailable, {}, {}, {},
                QStringLiteral("mcp-config-file-unavailable")};
    }
    const QByteArray bytes = file.read(MaxFileBytes + 1);
    if (bytes.size() > MaxFileBytes) {
        return {McpConfigurationInventoryState::Invalid, {}, {}, {},
                QStringLiteral("mcp-config-file-oversized")};
    }
    return inspectBytes(bytes, true);
}

McpConfigurationInventoryResult McpConfigurationInventory::inspectBytes(
    const QByteArray &bytes, bool sourceExists)
{
    McpConfigurationInventoryResult result;
    result.sourceIdentity = hashIdentity(
        QByteArrayLiteral("aegisy-mcp-config-source/0.1\0"),
        sourceExists ? bytes : QByteArrayLiteral("missing"),
        QStringLiteral("mcp-config-source:sha256:"));
    if (!sourceExists) {
        result.state = McpConfigurationInventoryState::Empty;
        return result;
    }
    if (bytes.isEmpty() || bytes.size() > MaxFileBytes) {
        result.state = McpConfigurationInventoryState::Invalid;
        result.errorCode = QStringLiteral("mcp-config-json-invalid");
        return result;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
            || !StrictJsonValidator::accepts(bytes)) {
        result.state = McpConfigurationInventoryState::Invalid;
        result.errorCode = QStringLiteral("mcp-config-json-invalid");
        return result;
    }
    result.root = document.object();
    if (!result.root.contains(QStringLiteral("mcpServers"))) {
        result.state = McpConfigurationInventoryState::Ready;
        return result;
    }
    if (!result.root.value(QStringLiteral("mcpServers")).isObject()) {
        result.state = McpConfigurationInventoryState::Invalid;
        result.errorCode = QStringLiteral("mcp-config-servers-invalid");
        return result;
    }
    const QJsonObject servers = result.root.value(QStringLiteral("mcpServers")).toObject();
    if (servers.size() > MaxServers) {
        result.state = McpConfigurationInventoryState::Invalid;
        result.errorCode = QStringLiteral("mcp-config-server-limit");
        return result;
    }
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        if (!QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
                    .match(it.key()).hasMatch() || !it.value().isObject()) {
            result.state = McpConfigurationInventoryState::Invalid;
            result.errorCode = QStringLiteral("mcp-config-server-invalid");
            return result;
        }
        const QJsonObject server = it.value().toObject();
        const bool urlMode = server.size() == 1
            && server.value(QStringLiteral("url")).isString()
            && validUrl(server.value(QStringLiteral("url")).toString());
        const bool stdioMode = !server.contains(QStringLiteral("url"))
            && validateStdio(server);
        if (!urlMode && !stdioMode) {
            result.state = McpConfigurationInventoryState::Invalid;
            result.errorCode = QStringLiteral("mcp-config-server-shape-invalid");
            return result;
        }
        const QByteArray canonical = QJsonDocument(server).toJson(QJsonDocument::Compact);
        ExtensionRegistryRecord record;
        record.kind = ExtensionKind::Mcp;
        record.id = it.key();
        record.name = it.key();
        record.sourceKind = ExtensionSourceKind::ToolConfiguration;
        record.sourceIdentity = hashIdentity(
            QByteArrayLiteral("aegisy-mcp-extension-source/0.1\0"),
            result.sourceIdentity.toUtf8() + '\0' + it.key().toUtf8(),
            QStringLiteral("extension-source:sha256:"));
        record.contentIdentity = hashIdentity(
            QByteArrayLiteral("aegisy-mcp-extension-content/0.1\0"), canonical,
            QStringLiteral("extension-content:sha256:"));
        record.trust = ExtensionTrustState::Unverified;
        // 来源不自我声明兼容性；判定由 ExtensionCompatibilityPolicy 统一做出。
        record.compatibility = ExtensionCompatibilityState::Unknown;
        record.compatibilityReason = QStringLiteral("mcp-compatibility-unevaluated");
        record.scope = QStringLiteral("user");
        record.requestedCapabilities = urlMode
            ? QStringList{QStringLiteral("network"), QStringLiteral("mcp-tools")}
            : QStringList{QStringLiteral("process"), QStringLiteral("mcp-tools")};
        record.installed = true;
        result.records.append(record);
    }
    result.state = McpConfigurationInventoryState::Ready;
    return result;
}
