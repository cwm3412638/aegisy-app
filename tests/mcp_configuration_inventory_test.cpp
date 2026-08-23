#include "mcp_configuration_inventory.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const auto missing = McpConfigurationInventory::inspectBytes({}, false);
    if (!expect(missing.state == McpConfigurationInventoryState::Empty,
                "missing MCP source was not empty")) return 1;
    const QByteArray valid = R"({"other":true,"mcpServers":{
      "filesystem":{"command":"npx","args":["-y","server"],"env":{"ACCESS_TOKEN":"private"}},
      "local":{"url":"http://127.0.0.1:3000/sse"},
      "remote":{"url":"https://example.com/sse"}
    }})";
    const auto ready = McpConfigurationInventory::inspectBytes(valid);
    if (!expect(ready.state == McpConfigurationInventoryState::Ready
                    && ready.records.size() == 3,
                "valid MCP inventory failed")) return 1;
    ExtensionRegistryProjection projection;
    QString error;
    if (!expect(ExtensionRegistry::build(ready.records, &projection, &error),
                "MCP records did not enter extension registry")
            || !expect(!QJsonDocument(projection.object).toJson().contains("private")
                           && !QJsonDocument(projection.object).toJson().contains("npx")
                           && !QJsonDocument(projection.object).toJson().contains("server")
                           && !QJsonDocument(projection.object).toJson().contains("ACCESS_TOKEN")
                           && !QJsonDocument(projection.object).toJson().contains("example.com"),
                       "MCP command, URL, args, or env leaked into registry")) return 1;
    for (const QByteArray &invalid : {
             QByteArrayLiteral("{} trailing"),
             QByteArrayLiteral("{\"mcpServers\":[]}"),
             QByteArrayLiteral("{\"mcpServers\":{\"Bad ID\":{\"command\":\"npx\"}}}"),
             QByteArrayLiteral("{\"mcpServers\":{\"bad\":{\"url\":\"http://example.com\"}}}"),
             QByteArrayLiteral("{\"mcpServers\":{\"bad\":{\"command\":\"sh -c\"}}}"),
             QByteArrayLiteral("{\"mcpServers\":{\"bad\":{\"command\":\"npx\",\"unknown\":true}}}")}) {
        if (!expect(McpConfigurationInventory::inspectBytes(invalid).state
                        == McpConfigurationInventoryState::Invalid,
                    "invalid MCP source was accepted")) return 1;
    }
    QTemporaryDir root;
    if (!root.isValid()) return 1;
    const QString link = root.filePath(QStringLiteral("settings.json"));
    if (QFile::link(root.filePath(QStringLiteral("missing.json")), link)) {
        if (!expect(McpConfigurationInventory::inspectFile(link).state
                        == McpConfigurationInventoryState::Invalid,
                    "symlink MCP source was accepted")) return 1;
    }
    return 0;
}
