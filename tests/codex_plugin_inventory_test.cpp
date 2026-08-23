#include "codex_plugin_inventory.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

bool invalid(const QByteArray &bytes)
{
    return CodexPluginInventory::inspectCapturedOutput(bytes).state
        == CodexPluginInventoryState::Invalid;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QByteArray valid = R"({
      "installed":[{
        "pluginId":"documents","name":"Documents","marketplaceName":"OpenAI",
        "version":"1.2.3","installed":true,"enabled":true,
        "source":{"path":"/private/codex/plugins/documents"}
      }],
      "available":[{
        "pluginId":"spreadsheets","name":"Spreadsheets","marketplaceName":"OpenAI",
        "version":"2.0.0","installed":false,"enabled":false,
        "source":{"path":"/private/codex/plugins/spreadsheets"}
      }]
    })";
    const auto ready = CodexPluginInventory::inspectCapturedOutput(valid);
    if (!expect(ready.state == CodexPluginInventoryState::Ready
                    && ready.records.size() == 2
                    && ready.sourceIdentity.startsWith(
                        QStringLiteral("codex-plugin-list-source:sha256:")),
                "valid captured plugin output was rejected")) return 1;
    for (const ExtensionRegistryRecord &record : ready.records) {
        if (!expect(record.kind == ExtensionKind::CodexPlugin
                        && record.sourceKind == ExtensionSourceKind::CodexCli
                        && record.trust == ExtensionTrustState::Unverified
                        && record.compatibility == ExtensionCompatibilityState::Unknown
                        && !record.effectiveEnabled && !record.updateAvailable
                        && !record.recoveryAvailable
                        && record.requestedCapabilities.isEmpty(),
                    "captured output granted unsupported plugin authority")) return 1;
    }
    if (!expect(ready.records.first().installed
                    && !ready.records.last().installed,
                "installed observation was not preserved")) return 1;

    ExtensionRegistryProjection projection;
    QString error;
    if (!expect(ExtensionRegistry::build(ready.records, &projection, &error),
                "captured plugin records did not enter the extension registry")) return 1;
    const QByteArray projected = QJsonDocument(projection.object)
        .toJson(QJsonDocument::Compact);
    if (!expect(!projected.contains("/private/")
                    && !projected.contains("marketplaceName")
                    && !projected.contains("enable_authority\":true")
                    && !projected.contains("execution_authority\":true"),
                "captured-only fields or authority leaked into the registry")) return 1;

    const QByteArray reformatted = QJsonDocument::fromJson(valid)
        .toJson(QJsonDocument::Compact);
    const auto sameContent = CodexPluginInventory::inspectCapturedOutput(reformatted);
    if (!expect(sameContent.state == CodexPluginInventoryState::Ready
                    && sameContent.sourceIdentity != ready.sourceIdentity
                    && sameContent.records.first().contentIdentity
                        == ready.records.first().contentIdentity
                    && sameContent.records.first().sourceIdentity
                        != ready.records.first().sourceIdentity,
                "source and semantic content identities were not separated")) return 1;

    for (const QByteArray &bytes : {
             QByteArrayLiteral(""),
             QByteArrayLiteral("{} trailing"),
             QByteArrayLiteral("[]"),
             QByteArrayLiteral("{\"installed\":[],\"available\":[],\"extra\":true}"),
             QByteArrayLiteral("{\"installed\":{},\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[],\"\\u0069nstalled\":[],\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[{\"pluginId\":\"one\",\"plugin\\u0049d\":\"two\",\"name\":\"One\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":true,\"enabled\":false,\"source\":{\"path\":\"/one\"}}],\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[{\"pluginId\":\"one\",\"name\":\"One\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":\"true\",\"enabled\":false,\"source\":{\"path\":\"/one\"}}],\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[{\"pluginId\":\"one\",\"name\":\"One\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":true,\"enabled\":false,\"source\":{\"path\":\"/one\",\"kind\":\"local\"}}],\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[],\"available\":[{\"pluginId\":\"one\",\"name\":\"One\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":false,\"enabled\":true,\"source\":{\"path\":\"/one\"}}]}"),
             QByteArrayLiteral("{\"installed\":[{\"pluginId\":\"one\",\"name\":\"token=private-value\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":true,\"enabled\":false,\"source\":{\"path\":\"/one\"}}],\"available\":[]}"),
             QByteArrayLiteral("{\"installed\":[{\"pluginId\":\"one\",\"name\":\"One\",\"marketplaceName\":\"OpenAI\",\"version\":\"1\",\"installed\":true,\"enabled\":false,\"source\":{\"path\":\"/plugins/api_key=private\"}}],\"available\":[]}"),
         }) {
        if (!expect(invalid(bytes), "invalid captured plugin output was accepted")) return 1;
    }

    const QByteArray duplicate = R"({"installed":[{
      "pluginId":"same","name":"Same","marketplaceName":"OpenAI","version":"1",
      "installed":true,"enabled":false,"source":{"path":"/same"}}],
      "available":[{"pluginId":"same","name":"Same","marketplaceName":"OpenAI",
      "version":"1","installed":false,"enabled":false,"source":{"path":"/same"}}]})";
    if (!expect(invalid(duplicate), "duplicate plugin identity was accepted")) return 1;

    QByteArray oversized(CodexPluginInventory::MaxCapturedBytes + 1, ' ');
    if (!expect(invalid(oversized), "oversized captured output was accepted")) return 1;

    QByteArray tooMany = QByteArrayLiteral("{\"installed\":[],\"available\":[");
    for (int i = 0; i <= CodexPluginInventory::MaxPlugins; ++i) {
        if (i > 0) tooMany.append(',');
        tooMany.append(QStringLiteral(
            "{\"pluginId\":\"plugin.%1\",\"name\":\"Plugin %1\","
            "\"marketplaceName\":\"OpenAI\",\"version\":\"1\","
            "\"installed\":false,\"enabled\":false,"
            "\"source\":{\"path\":\"/plugin/%1\"}}")
                           .arg(i).toUtf8());
    }
    tooMany.append(QByteArrayLiteral("]}"));
    if (!expect(invalid(tooMany), "513th captured plugin was accepted")) return 1;
    return 0;
}
