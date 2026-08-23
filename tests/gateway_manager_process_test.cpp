#include "gateway_manager.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

QJsonObject acceptedResult(const QJsonObject &request)
{
    const QString operation = request.value(QStringLiteral("operation")).toString();
    const QString outcome = operation == QStringLiteral("commit")
        ? QStringLiteral("committed")
        : (operation == QStringLiteral("abort")
            ? QStringLiteral("aborted") : QStringLiteral("prepared"));
    return {
        {QStringLiteral("schema"), QStringLiteral("aegisy-gateway-control/0.1")},
        {QStringLiteral("type"), QStringLiteral("control-result")},
        {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
        {QStringLiteral("transaction_id"),
         request.value(QStringLiteral("transaction_id"))},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("tool"), request.value(QStringLiteral("tool"))},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("credential_included"), false},
        {QStringLiteral("error_code"), QString()},
    };
}

int runFixtureProcess(const QString &mode)
{
    QTextStream input(stdin, QIODevice::ReadOnly);
    QTextStream output(stdout, QIODevice::WriteOnly);
    output << QJsonDocument(QJsonObject{{QStringLiteral("type"),
                                        QStringLiteral("ready")}})
                  .toJson(QJsonDocument::Compact)
           << Qt::endl;
    while (!input.atEnd()) {
        const QByteArray line = input.readLine().toUtf8();
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) continue;
        const QJsonObject request = document.object();
        if (request.value(QStringLiteral("type")).toString()
                == QStringLiteral("shutdown")) {
            return 0;
        }
        if (request.value(QStringLiteral("type")).toString()
                != QStringLiteral("control")) {
            continue;
        }
        if (mode == QStringLiteral("exit")) return 23;
        if (mode == QStringLiteral("timeout")) continue;
        QJsonObject response = acceptedResult(request);
        if (mode == QStringLiteral("malformed")) {
            response.insert(QStringLiteral("unexpected"), true);
        }
        output << QJsonDocument(response).toJson(QJsonDocument::Compact)
               << Qt::endl;
    }
    return 0;
}

void setFixtureMode(const QString &mode)
{
    qputenv("AEGISY_GATEWAY_PROCESS_FIXTURE", mode.toUtf8());
}

} // namespace

int main(int argc, char *argv[])
{
    const QString fixtureMode = QString::fromLocal8Bit(
        qgetenv("AEGISY_GATEWAY_PROCESS_FIXTURE"));
    if (!fixtureMode.isEmpty()) return runFixtureProcess(fixtureMode);

    QCoreApplication application(argc, argv);
    bool ok = true;
    GatewayManager manager;
    const auto configure = [&manager](const QString &mode) {
        setFixtureMode(mode);
        manager.configureProcessTest(
            QCoreApplication::applicationFilePath(), {},
            QStringLiteral("aegisy-local-process-fixture"), 150);
        return manager.start();
    };

    ok = expect(configure(QStringLiteral("timeout")),
                "timeout fixture did not reach ready state") && ok;
    const quint64 timedOutGeneration = manager.processTestGeneration();
    QString transactionId;
    ok = expect(!manager.prepareProfile(
                    AiTool::CodexCli, QStringLiteral("fixture-key"), &transactionId)
                    && manager.lastError()
                        == QStringLiteral("gateway-control-timeout-outcome-unknown")
                    && !manager.isRunning()
                    && manager.processTestGeneration() > timedOutGeneration
                    && manager.processTestLastRetireReaped(),
                "control timeout was not classified outcome-unknown") && ok;
    manager.injectProcessTestEvent(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("ready")}},
        timedOutGeneration);
    ok = expect(!manager.isRunning(),
                "late ready event revived a retired generation") && ok;

    ok = expect(configure(QStringLiteral("normal")),
                "replacement fixture did not reach ready state") && ok;
    const quint64 currentGeneration = manager.processTestGeneration();
    ok = expect(currentGeneration > timedOutGeneration,
                "replacement process did not advance generation") && ok;
    manager.injectProcessTestEvent(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("fatal")},
                    {QStringLiteral("error"), QStringLiteral("stale-error")}},
        timedOutGeneration);
    ok = expect(manager.isRunning()
                    && manager.processTestGeneration() == currentGeneration
                    && manager.lastError().isEmpty(),
                "late prior-generation event changed current gateway state") && ok;
    manager.stop();

    ok = expect(configure(QStringLiteral("exit")),
                "exit fixture did not reach ready state") && ok;
    ok = expect(!manager.prepareProfile(
                    AiTool::CodexCli, QStringLiteral("fixture-key"), &transactionId)
                    && manager.lastError()
                        == QStringLiteral("gateway-control-exit-outcome-unknown")
                    && !manager.isRunning()
                    && manager.processTestLastRetireReaped(),
                "process exit was not classified outcome-unknown") && ok;

    ok = expect(configure(QStringLiteral("malformed")),
                "malformed fixture did not reach ready state") && ok;
    ok = expect(!manager.prepareProfile(
                    AiTool::CodexCli, QStringLiteral("fixture-key"), &transactionId)
                    && manager.lastError()
                        == QStringLiteral("gateway-control-protocol-invalid")
                    && !manager.isRunning()
                    && manager.processTestLastRetireReaped(),
                "malformed process result did not fail the generation closed") && ok;

    ok = expect(configure(QStringLiteral("normal")),
                "normal fixture did not reach ready state") && ok;
    ok = expect(manager.prepareProfile(
                    AiTool::CodexCli, QStringLiteral("fixture-key"), &transactionId)
                    && !transactionId.isEmpty()
                    && manager.commitProfile(AiTool::CodexCli, transactionId)
                    && manager.isRunning(),
                "valid process result did not complete prepare/commit") && ok;
    manager.stop();
    qunsetenv("AEGISY_GATEWAY_PROCESS_FIXTURE");
    return ok ? 0 : 1;
}
