#include "runtime_status_store.h"

#include <QtGlobal>

RuntimeStatusStore::RuntimeStatusStore(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<RuntimeStatusSnapshot>("RuntimeStatusSnapshot");
}

void RuntimeStatusStore::setConfiguredProfile(AiTool tool,
                                              const QString &model,
                                              const QString &reasoning,
                                              qint64 contextLimit)
{
    m_snapshot.toolId = configuredToolId(tool);
    m_snapshot.toolName = configuredToolName(tool);
    m_snapshot.model = model.trimmed();
    m_snapshot.reasoning = reasoning.trimmed();
    m_snapshot.inputTokens = -1;
    m_snapshot.outputTokens = -1;
    m_snapshot.totalTokens = -1;
    m_snapshot.contextLimit = contextLimit > 0 ? contextLimit : -1;
    m_snapshot.active = false;
    m_snapshot.monitored = m_snapshot.gatewayRunning;
    m_snapshot.requestObserved = false;
    m_snapshot.provenance = RuntimeStatusProvenance::Configured;
    publish();
}

void RuntimeStatusStore::clearConfiguredProfile()
{
    m_snapshot.toolId.clear();
    m_snapshot.toolName.clear();
    m_snapshot.model.clear();
    m_snapshot.reasoning.clear();
    m_snapshot.inputTokens = -1;
    m_snapshot.outputTokens = -1;
    m_snapshot.totalTokens = -1;
    m_snapshot.contextLimit = -1;
    m_snapshot.active = false;
    m_snapshot.monitored = false;
    m_snapshot.requestObserved = false;
    m_snapshot.provenance = RuntimeStatusProvenance::Unknown;
    publish();
}

void RuntimeStatusStore::setBalance(double balance, bool known)
{
    m_snapshot.balance = balance;
    m_snapshot.balanceKnown = known;
    publish();
}

void RuntimeStatusStore::setGatewayRunning(bool running)
{
    m_snapshot.gatewayRunning = running;
    if (m_snapshot.provenance == RuntimeStatusProvenance::Configured) {
        m_snapshot.monitored = running;
    } else if (!running && m_snapshot.provenance == RuntimeStatusProvenance::Gateway) {
        m_snapshot.active = false;
        m_snapshot.monitored = false;
        m_snapshot.inputTokens = -1;
        m_snapshot.outputTokens = -1;
        m_snapshot.totalTokens = -1;
        m_snapshot.requestObserved = false;
        m_snapshot.provenance = RuntimeStatusProvenance::Configured;
    }
    publish();
}

void RuntimeStatusStore::observeGatewayEvent(const QJsonObject &event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("request_started")
            && type != QStringLiteral("request")) {
        return;
    }

    const QString toolId = event.value(QStringLiteral("tool")).toString().trimmed();
    const bool toolChanged = !toolId.isEmpty() && toolId != m_snapshot.toolId;
    if (!toolId.isEmpty()) {
        m_snapshot.toolId = toolId;
        m_snapshot.toolName = gatewayToolName(toolId);
    }
    const QString model = event.value(QStringLiteral("model")).toString().trimmed();
    const bool modelChanged = !model.isEmpty() && model != m_snapshot.model;
    const QString reasoning = event.value(QStringLiteral("reasoning_effort"))
        .toString().trimmed();

    const qint64 limit = event.value(QStringLiteral("context_limit")).toVariant().toLongLong();

    m_snapshot.gatewayRunning = true;
    m_snapshot.monitored = true;
    m_snapshot.requestObserved = true;
    m_snapshot.provenance = RuntimeStatusProvenance::Gateway;
    if (type == QStringLiteral("request_started")) {
        m_snapshot.model = model;
        m_snapshot.reasoning = reasoning;
        if (limit > 0) {
            m_snapshot.contextLimit = limit;
        } else if (toolChanged || modelChanged) {
            const QString normalizedModel = model.trimmed().toLower();
            const bool gpt56 = normalizedModel == QStringLiteral("gpt-5.6")
                || normalizedModel.startsWith(QStringLiteral("gpt-5.6-sol"));
            m_snapshot.contextLimit = toolId == QStringLiteral("codex")
                ? (gpt56 ? ToolManager::CodexGpt56ContextLimit
                         : ToolManager::CodexConfiguredContextLimit)
                : -1;
        }
        m_snapshot.active = true;
        m_snapshot.inputTokens = -1;
        m_snapshot.outputTokens = -1;
        m_snapshot.totalTokens = -1;
    } else {
        if (!model.isEmpty()) m_snapshot.model = model;
        if (!reasoning.isEmpty()) m_snapshot.reasoning = reasoning;
        if (limit > 0) m_snapshot.contextLimit = limit;
        m_snapshot.active = false;
        const qint64 input = event.value(QStringLiteral("input_tokens"))
            .toVariant().toLongLong();
        const qint64 output = event.value(QStringLiteral("output_tokens"))
            .toVariant().toLongLong();
        const qint64 total = event.value(QStringLiteral("total_tokens"))
            .toVariant().toLongLong();
        m_snapshot.inputTokens = input >= 0 && event.contains(QStringLiteral("input_tokens"))
            ? input : -1;
        m_snapshot.outputTokens = output >= 0 && event.contains(QStringLiteral("output_tokens"))
            ? output : -1;
        m_snapshot.totalTokens = total >= 0 && event.contains(QStringLiteral("total_tokens"))
            ? total : (m_snapshot.inputTokens >= 0 && m_snapshot.outputTokens >= 0
                ? m_snapshot.inputTokens + m_snapshot.outputTokens : -1);
    }
    publish();
}

void RuntimeStatusStore::beginChat(const QString &model,
                                   const QString &reasoning,
                                   qint64 contextLimit)
{
    m_snapshot.toolId = QStringLiteral("chat");
    m_snapshot.toolName = QStringLiteral("AI 对话");
    m_snapshot.model = model.trimmed();
    m_snapshot.reasoning = reasoning.trimmed();
    m_snapshot.inputTokens = -1;
    m_snapshot.outputTokens = -1;
    m_snapshot.totalTokens = -1;
    m_snapshot.contextLimit = contextLimit > 0 ? contextLimit : -1;
    m_snapshot.active = true;
    m_snapshot.monitored = true;
    m_snapshot.requestObserved = true;
    m_snapshot.provenance = RuntimeStatusProvenance::InAppChat;
    publish();
}

void RuntimeStatusStore::updateChatUsage(qint64 inputTokens,
                                         qint64 outputTokens,
                                         qint64 totalTokens)
{
    if (m_snapshot.provenance != RuntimeStatusProvenance::InAppChat) return;
    m_snapshot.inputTokens = qMax<qint64>(0, inputTokens);
    m_snapshot.outputTokens = qMax<qint64>(0, outputTokens);
    m_snapshot.totalTokens = totalTokens >= 0
        ? totalTokens : m_snapshot.inputTokens + m_snapshot.outputTokens;
    publish();
}

void RuntimeStatusStore::finishChat()
{
    if (m_snapshot.provenance != RuntimeStatusProvenance::InAppChat) return;
    m_snapshot.active = false;
    publish();
}

void RuntimeStatusStore::publish()
{
    m_snapshot.updatedAt = QDateTime::currentDateTimeUtc();
    emit statusChanged(m_snapshot);
}

QString RuntimeStatusStore::gatewayToolName(const QString &toolId)
{
    if (toolId == QStringLiteral("claude")) return QStringLiteral("Claude Code");
    if (toolId == QStringLiteral("codex")) return QStringLiteral("Codex CLI");
    if (toolId == QStringLiteral("gemini")) return QStringLiteral("Gemini CLI");
    if (toolId == QStringLiteral("opencode")) return QStringLiteral("OpenCode");
    return toolId.isEmpty() ? QStringLiteral("AI 工具") : toolId;
}

QString RuntimeStatusStore::configuredToolId(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli: return QStringLiteral("codex");
    case AiTool::GeminiCli: return QStringLiteral("gemini");
    case AiTool::OpenCode: return QStringLiteral("opencode");
    }
    return QString();
}

QString RuntimeStatusStore::configuredToolName(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("Claude Code");
    case AiTool::CodexCli: return QStringLiteral("Codex CLI");
    case AiTool::GeminiCli: return QStringLiteral("Gemini CLI");
    case AiTool::OpenCode: return QStringLiteral("OpenCode");
    }
    return QStringLiteral("AI 工具");
}
