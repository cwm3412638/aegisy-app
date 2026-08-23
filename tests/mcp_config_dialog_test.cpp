#include "mcp_config_dialog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

class McpConfigDialogTestAccess
{
public:
    static bool sourceValid(const McpConfigDialog &dialog)
    {
        return dialog.m_sourceValid;
    }
    static bool save(McpConfigDialog &dialog) { return dialog.saveToSettings(); }
    static void setServers(McpConfigDialog &dialog, const QJsonObject &servers)
    {
        dialog.m_mcpServers = servers;
    }
};

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    qputenv("AEGISY_CONFIG_HOME", root.path().toUtf8());
    QApplication application(argc, argv);
    const QString path = QDir(root.path()).filePath(
        QStringLiteral(".claude/settings.json"));

    const QByteArray invalid = QByteArrayLiteral("{invalid-json");
    if (!writeBytes(path, invalid)) return 1;
    {
        McpConfigDialog dialog;
        if (!expect(!McpConfigDialogTestAccess::sourceValid(dialog)
                        && !McpConfigDialogTestAccess::save(dialog)
                        && readBytes(path) == invalid,
                    "invalid MCP source was overwritten")) return 1;
    }

    const QByteArray initial = QByteArrayLiteral(
        "{\"other\":true,\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
    if (!writeBytes(path, initial)) return 1;
    {
        McpConfigDialog dialog;
        const QByteArray external = QByteArrayLiteral(
            "{\"other\":\"external\",\"mcpServers\":{\"one\":{\"command\":\"npx\"}}}");
        if (!writeBytes(path, external)) return 1;
        if (!expect(!McpConfigDialogTestAccess::save(dialog)
                        && readBytes(path) == external,
                    "externally drifted MCP source was overwritten")) return 1;
    }

    if (!writeBytes(path, initial)) return 1;
    {
        McpConfigDialog dialog;
        McpConfigDialogTestAccess::setServers(dialog, QJsonObject{
            {QStringLiteral("two"), QJsonObject{
                {QStringLiteral("url"), QStringLiteral("https://example.com/sse")}}}});
        if (!expect(McpConfigDialogTestAccess::save(dialog),
                    "valid MCP source did not save")) return 1;
        const QByteArray saved = readBytes(path);
        if (!expect(saved.contains("\"other\": true")
                        && saved.contains("\"two\""),
                    "valid MCP save lost non-MCP data or new server")) return 1;
    }
    return 0;
}
