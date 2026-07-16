#include "runtime_status_store.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) qCritical() << message;
    return condition;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    RuntimeStatusStore store;

    store.setBalance(42.5, true);
    store.setConfiguredProfile(AiTool::CodexCli, QStringLiteral("gpt-test"),
                               QStringLiteral("xhigh"), 272000);
    RuntimeStatusSnapshot status = store.snapshot();
    if (!expect(status.balanceKnown && status.balance == 42.5,
                "balance was not retained")
            || !expect(status.toolId == QStringLiteral("codex"),
                       "configured tool id should match gateway events")
            || !expect(status.model == QStringLiteral("gpt-test"),
                       "configured model missing")
            || !expect(status.provenance == RuntimeStatusProvenance::Configured,
                       "configured provenance missing")
            || !expect(!status.usageKnown(),
                       "configured status fabricated usage")) {
        return 1;
    }

    store.setGatewayRunning(true);
    store.observeGatewayEvent(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("request_started") },
        { QStringLiteral("tool"), QStringLiteral("codex") },
        { QStringLiteral("model"), QStringLiteral("gpt-live") },
        { QStringLiteral("reasoning_effort"), QStringLiteral("high") },
    });
    status = store.snapshot();
    if (!expect(status.active && status.monitored,
                "gateway request did not become active")
            || !expect(status.model == QStringLiteral("gpt-live"),
                       "gateway model did not override configured model")
            || !expect(!status.usageKnown(),
                       "started request fabricated usage")) {
        return 1;
    }

    store.observeGatewayEvent(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("request") },
        { QStringLiteral("tool"), QStringLiteral("codex") },
        { QStringLiteral("input_tokens"), 84000 },
        { QStringLiteral("output_tokens"), 1200 },
        { QStringLiteral("total_tokens"), 85200 },
    });
    status = store.snapshot();
    if (!expect(!status.active && status.usageKnown(),
                "finished request usage missing")
            || !expect(status.inputTokens == 84000 && status.totalTokens == 85200,
                       "gateway usage values changed")
            || !expect(status.contextLimit == 272000,
                       "configured context limit was not retained")) {
        return 1;
    }

    store.observeGatewayEvent(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("request_started") },
        { QStringLiteral("tool"), QStringLiteral("claude") },
        { QStringLiteral("model"), QStringLiteral("claude-live") },
    });
    status = store.snapshot();
    if (!expect(status.contextLimit == -1,
                "tool changes must clear a stale context limit")
            || !expect(status.reasoning.isEmpty(),
                       "tool changes must clear stale reasoning metadata")) {
        return 1;
    }

    store.observeGatewayEvent(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("request") },
        { QStringLiteral("tool"), QStringLiteral("claude") },
    });
    status = store.snapshot();
    if (!expect(status.requestObserved && !status.active && !status.usageKnown(),
                "usage-free completion should remain an explicit observation")) {
        return 1;
    }

    store.setGatewayRunning(false);
    status = store.snapshot();
    if (!expect(!status.monitored && !status.active,
                "stopped gateway must not remain monitored")
            || !expect(status.provenance == RuntimeStatusProvenance::Configured,
                       "stopped gateway should fall back to configured provenance")) {
        return 1;
    }

    store.clearConfiguredProfile();
    status = store.snapshot();
    if (!expect(status.toolId.isEmpty() && status.balanceKnown,
                "clearing a profile should retain account balance only")
            || !expect(status.provenance == RuntimeStatusProvenance::Unknown,
                       "clearing a profile should reset provenance")) {
        return 1;
    }

    store.beginChat(QStringLiteral("chat-model"), QString(), 128000);
    store.updateChatUsage(1000, 200, 1200);
    store.finishChat();
    status = store.snapshot();
    if (!expect(status.provenance == RuntimeStatusProvenance::InAppChat,
                "chat provenance missing")
            || !expect(!status.active && status.inputTokens == 1000,
                       "chat completion state incorrect")) {
        return 1;
    }

    RuntimeStatusStore gpt56Store;
    gpt56Store.setGatewayRunning(true);
    gpt56Store.observeGatewayEvent(QJsonObject{
        { QStringLiteral("type"), QStringLiteral("request_started") },
        { QStringLiteral("tool"), QStringLiteral("codex") },
        { QStringLiteral("model"), QStringLiteral("gpt-5.6-sol") },
    });
    if (!expect(gpt56Store.snapshot().contextLimit == 372000,
                "GPT-5.6 gateway fallback did not use the 372K context threshold")) {
        return 1;
    }

    return 0;
}
