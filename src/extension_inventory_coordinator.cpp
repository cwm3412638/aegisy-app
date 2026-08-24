#include "extension_inventory_coordinator.h"

#include "codex_plugin_inventory.h"
#include "extension_compatibility_policy.h"
#include "mcp_configuration_inventory.h"
#include "process_command.h"
#include "skill_extension_inventory.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

namespace {

struct ExecutableIdentity {
    QString canonicalPath;
    qint64 size = -1;
    qint64 modifiedMs = -1;
};

bool executableIdentity(const QString &path, ExecutableIdentity *identity)
{
    const QFileInfo supplied(path);
    if (!supplied.isAbsolute()) return false;
    const QString canonical = supplied.canonicalFilePath();
    if (canonical.isEmpty()) return false;
    const QFileInfo resolved(canonical);
    if (!resolved.isFile() || !resolved.isExecutable() || resolved.size() < 0) {
        return false;
    }
    identity->canonicalPath = canonical;
    identity->size = resolved.size();
    identity->modifiedMs = resolved.lastModified().toMSecsSinceEpoch();
    return true;
}

bool sameExecutable(const ExecutableIdentity &expected)
{
    ExecutableIdentity current;
    return executableIdentity(expected.canonicalPath, &current)
        && current.canonicalPath == expected.canonicalPath
        && current.size == expected.size
        && current.modifiedMs == expected.modifiedMs;
}

QString fixedIssue(QString code, const QString &fallback)
{
    if (code.isEmpty()) return fallback;
    for (const QChar character : code) {
        const ushort value = character.unicode();
        if (!((value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9') || value == '-')) {
            return fallback;
        }
    }
    return code.size() <= 96 ? code : fallback;
}

bool captureCodexPlugins(const ExtensionInventoryInputs &inputs,
                         QList<ExtensionRegistryRecord> *records,
                         QString *issueCode)
{
    ExecutableIdentity executable;
    if (!executableIdentity(inputs.codexExecutable, &executable)) {
        *issueCode = QStringLiteral("codex-plugin-source-unavailable");
        return false;
    }
    if (inputs.codexTimeoutMs < 100 || inputs.codexTimeoutMs > 60000) {
        *issueCode = QStringLiteral("codex-plugin-timeout-invalid");
        return false;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProcessEnvironment(
        ExtensionInventoryCoordinator::scrubbedEnvironment(inputs.sourceEnvironment));
    ProcessCommand::start(
        &process, executable.canonicalPath,
        {QStringLiteral("plugin"), QStringLiteral("list"),
         QStringLiteral("--available"), QStringLiteral("--json")});
    if (!process.waitForStarted(3000)) {
        *issueCode = QStringLiteral("codex-plugin-start-failed");
        return false;
    }

    QByteArray output;
    qint64 stderrBytes = 0;
    QElapsedTimer timer;
    timer.start();
    bool overflow = false;
    while (process.state() != QProcess::NotRunning) {
        const qint64 remaining = inputs.codexTimeoutMs - timer.elapsed();
        if (remaining <= 0) {
            process.kill();
            process.waitForFinished(1000);
            *issueCode = QStringLiteral("codex-plugin-timeout");
            return false;
        }
        process.waitForReadyRead(static_cast<int>(qMin<qint64>(remaining, 50)));
        output.append(process.readAllStandardOutput());
        stderrBytes += process.readAllStandardError().size();
        if (output.size() > CodexPluginInventory::MaxCapturedBytes
                || stderrBytes > ExtensionInventoryCoordinator::MaxCodexStderrBytes) {
            overflow = true;
            process.kill();
            process.waitForFinished(1000);
            break;
        }
    }
    output.append(process.readAllStandardOutput());
    stderrBytes += process.readAllStandardError().size();
    if (overflow || output.size() > CodexPluginInventory::MaxCapturedBytes
            || stderrBytes > ExtensionInventoryCoordinator::MaxCodexStderrBytes) {
        *issueCode = QStringLiteral("codex-plugin-output-limit");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *issueCode = QStringLiteral("codex-plugin-command-failed");
        return false;
    }
    if (!sameExecutable(executable)) {
        *issueCode = QStringLiteral("codex-plugin-executable-drift");
        return false;
    }
    const CodexPluginInventoryResult result =
        CodexPluginInventory::inspectCapturedOutput(output);
    if (result.state != CodexPluginInventoryState::Ready) {
        *issueCode = fixedIssue(
            result.errorCode, QStringLiteral("codex-plugin-output-invalid"));
        return false;
    }
    records->append(result.records);
    return true;
}

void appendIssue(QStringList *issues, const QString &code, const QString &fallback)
{
    issues->append(fixedIssue(code, fallback));
}

} // namespace

QProcessEnvironment ExtensionInventoryCoordinator::scrubbedEnvironment(
    const QProcessEnvironment &source)
{
    static const QSet<QString> allowed{
        QStringLiteral("APPDATA"), QStringLiteral("CODEX_HOME"),
        QStringLiteral("ComSpec"), QStringLiteral("HOME"),
        QStringLiteral("HOMEDRIVE"), QStringLiteral("HOMEPATH"),
        QStringLiteral("LANG"), QStringLiteral("LC_ALL"),
        QStringLiteral("LOCALAPPDATA"), QStringLiteral("PATH"),
        QStringLiteral("PATHEXT"), QStringLiteral("SystemRoot"),
        QStringLiteral("TEMP"), QStringLiteral("TMP"),
        QStringLiteral("TMPDIR"), QStringLiteral("USERPROFILE"),
        QStringLiteral("XDG_CONFIG_HOME"), QStringLiteral("XDG_DATA_HOME")};
    QProcessEnvironment result;
    for (const QString &name : allowed) {
        if (!source.contains(name)) continue;
        const QString value = source.value(name);
        if (value.size() > 16384) continue;
        bool safe = true;
        for (const QChar character : value) {
            if (character == QChar(0) || character == QChar('\r')
                    || character == QChar('\n')) {
                safe = false;
                break;
            }
        }
        if (safe) result.insert(name, value);
    }
    result.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
    return result;
}

ExtensionInventorySnapshot ExtensionInventoryCoordinator::collect(
    const ExtensionInventoryInputs &inputs)
{
    ExtensionInventorySnapshot snapshot;
    QString codexIssue;
    captureCodexPlugins(inputs, &snapshot.records, &codexIssue);
    if (!codexIssue.isEmpty()) snapshot.sourceIssueCodes.append(codexIssue);

    const SkillExtensionInventoryResult skills =
        SkillExtensionInventory::inspectRoot(inputs.skillsRoot);
    if (skills.state == SkillExtensionInventoryState::Ready) {
        snapshot.records.append(skills.records);
    } else if (skills.state != SkillExtensionInventoryState::Empty) {
        appendIssue(&snapshot.sourceIssueCodes, skills.errorCode,
                    QStringLiteral("skill-source-invalid"));
    }

    const McpConfigurationInventoryResult mcp =
        McpConfigurationInventory::inspectFile(inputs.mcpConfigurationPath);
    if (mcp.state == McpConfigurationInventoryState::Ready) {
        snapshot.records.append(mcp.records);
    } else if (mcp.state != McpConfigurationInventoryState::Empty) {
        appendIssue(&snapshot.sourceIssueCodes, mcp.errorCode,
                    QStringLiteral("mcp-source-invalid"));
    }

    // 兼容性在这里统一判定：各来源只报告可核查的事实，不自我声明兼容，也不因此
    // 获得启用授权。
    ExtensionCompatibilityPolicy::apply(&snapshot.records, inputs.host);

    ExtensionRegistryProjection projection;
    QString registryError;
    snapshot.registryValid = ExtensionRegistry::build(
        snapshot.records, &projection, &registryError);
    if (!snapshot.registryValid) {
        snapshot.records.clear();
        snapshot.registryIdentity.clear();
        snapshot.sourceIssueCodes.append(QStringLiteral("extension-registry-invalid"));
    } else {
        snapshot.registryIdentity = projection.identity;
    }
    snapshot.sourceIssueCodes.removeDuplicates();
    snapshot.sourceIssueCodes.sort();
    return snapshot;
}
