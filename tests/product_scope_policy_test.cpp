#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace {

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll());
}

bool requireContains(const QString &text, const QString &value, const char *message)
{
    if (text.contains(value)) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

bool requireAbsent(const QString &text, const QString &value, const char *message)
{
    if (!text.contains(value)) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        QTextStream(stderr) << "source root argument is required" << Qt::endl;
        return 1;
    }

    const QDir root(application.arguments().at(1));
    const QString mainWindow = readFile(root.filePath(QStringLiteral("src/main_window.cpp")));
    const QString workbenchWindow = readFile(root.filePath(
        QStringLiteral("src/agent_workbench_window.cpp")));
    const QString connectWizard = readFile(root.filePath(
        QStringLiteral("src/connect_wizard.cpp")));
    const QString toolHeader = readFile(root.filePath(QStringLiteral("include/tool_manager.h")));
    const QString runtime = readFile(root.filePath(
        QStringLiteral("agent-runtime/crates/aegisy-agentd/src/lib.rs")));
    const QString proposal = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/proposal.md")));
    const QString companionSpec = readFile(root.filePath(
        QStringLiteral("openspec/changes/build-aegisy-agent-workbench/specs/"
                       "aegisy-companion-control-center/spec.md")));
    if (mainWindow.isEmpty() || workbenchWindow.isEmpty() || connectWizard.isEmpty()
            || toolHeader.isEmpty() || runtime.isEmpty()
            || proposal.isEmpty() || companionSpec.isEmpty()) {
        QTextStream(stderr) << "product scope source could not be read" << Qt::endl;
        return 1;
    }

    bool valid = true;
    for (const QString &label : {
             QStringLiteral("Aegisy - 网站配套助手"),
             QStringLiteral("配置中心"),
             QStringLiteral("桌面增强"),
             QStringLiteral("插件与 Skills"),
             QStringLiteral("Codex 编程"),
         }) {
        valid &= requireContains(mainWindow, label, "companion navigation label is missing");
    }
    valid &= requireAbsent(mainWindow, QStringLiteral("\"Agent 工作台\""),
                           "generic Agent Workbench remains in primary navigation");
    valid &= requireContains(workbenchWindow, QStringLiteral("Aegisy Codex Programming"),
                             "retained Workbench preview is not Codex-scoped");
    valid &= requireContains(workbenchWindow, QStringLiteral("Model: Codex"),
                             "retained Workbench preview does not identify Codex");
    valid &= requireAbsent(workbenchWindow, QStringLiteral("Claude Opus"),
                           "retained Workbench preview advertises a deferred model runtime");
    valid &= requireContains(connectWizard,
                             QStringLiteral("companionConfigurationReceived"),
                             "ConnectWizard does not consume companion metadata");
    valid &= requireContains(connectWizard,
                             QStringLiteral("CompanionCredentialBroker::resolve"),
                             "ConnectWizard does not resolve an exact credential handle");
    valid &= requireContains(connectWizard,
                             QStringLiteral("companionModelsReceived"),
                             "ConnectWizard does not consume correlated model metadata");
    valid &= requireContains(connectWizard,
                             QStringLiteral("m_modelRequestKeyIdentity"),
                             "ConnectWizard does not bind model responses to a Key identity");
    valid &= requireAbsent(connectWizard, QStringLiteral("&ApiClient::modelsReceived"),
                           "ConnectWizard still consumes the global model signal");
    valid &= requireAbsent(connectWizard, QStringLiteral("m_allKeys"),
                           "ConnectWizard retains the raw website Key array");

    for (const QString &tool : {
             QStringLiteral("ClaudeCode"), QStringLiteral("CodexCli"),
             QStringLiteral("GeminiCli"), QStringLiteral("OpenCode"),
         }) {
        valid &= requireContains(toolHeader, tool,
                                 "supported configuration target is missing");
    }

    valid &= requireContains(runtime, QStringLiteral("mod codex_adapter;"),
                             "Codex adapter is not retained");
    for (const QString &adapter : {
             QStringLiteral("mod claude_adapter;"),
             QStringLiteral("mod gemini_adapter;"),
             QStringLiteral("mod acp_adapter;"),
         }) {
        valid &= requireAbsent(runtime, adapter,
                               "deferred non-Codex adapter is compiled into Runtime");
    }

    valid &= requireContains(proposal, QStringLiteral("website companion"),
                             "proposal does not define the companion direction");
    valid &= requireContains(proposal, QStringLiteral("Codex remains the only"),
                             "proposal does not constrain integrated programming");
    valid &= requireContains(companionSpec, QStringLiteral("configuration target"),
                             "companion spec does not separate config and Agent targets");
    valid &= requireContains(companionSpec, QStringLiteral("Codex-only"),
                             "companion spec does not define Codex-only programming");

    return valid ? 0 : 1;
}
