#include "gateway_control_contract.h"

#include <QCoreApplication>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

QJsonObject result(const QString &outcome = QStringLiteral("prepared"),
                   const QString &error = QString(), qint64 revision = 7)
{
    return {
        {QStringLiteral("schema"), QStringLiteral("aegisy-gateway-control/0.1")},
        {QStringLiteral("type"), QStringLiteral("control-result")},
        {QStringLiteral("request_id"), QStringLiteral("request_1")},
        {QStringLiteral("transaction_id"), QStringLiteral("transaction_1")},
        {QStringLiteral("operation"), QStringLiteral("prepare-configure")},
        {QStringLiteral("tool"), QStringLiteral("codex")},
        {QStringLiteral("outcome"), outcome},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("credential_included"), false},
        {QStringLiteral("error_code"), error},
    };
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const GatewayControlExpectation expected{
        QStringLiteral("request_1"), QStringLiteral("transaction_1"),
        QStringLiteral("prepare-configure"), QStringLiteral("codex")};
    if (!expect(GatewayControlContract::evaluate(result(), expected).decision
                    == GatewayControlDecision::Accepted,
                "valid result was not accepted")) return 1;
    if (!expect(GatewayControlContract::evaluate(
                    result(QStringLiteral("rejected"),
                           QStringLiteral("gateway-control-invalid")), expected).decision
                    == GatewayControlDecision::Rejected,
                "valid rejection was not preserved")) return 1;

    for (const QString &field : {QStringLiteral("request_id"),
                                 QStringLiteral("transaction_id"),
                                 QStringLiteral("operation"),
                                 QStringLiteral("tool")}) {
        QJsonObject changed = result();
        changed.insert(field, QStringLiteral("drifted"));
        if (!expect(GatewayControlContract::evaluate(changed, expected).decision
                        == GatewayControlDecision::Invalid,
                    "cross-bound result was accepted")) return 1;
    }
    QJsonObject unknown = result();
    unknown.insert(QStringLiteral("unexpected"), true);
    QJsonObject credential = result();
    credential.insert(QStringLiteral("credential_included"), true);
    QJsonObject fractional = result();
    fractional.insert(QStringLiteral("revision"), 1.5);
    QJsonObject falseSuccess = result();
    falseSuccess.insert(QStringLiteral("error_code"), QStringLiteral("error"));
    QJsonObject wrongOutcome = result(QStringLiteral("committed"));
    QJsonObject dynamicError = result(
        QStringLiteral("rejected"), QStringLiteral("Error: /private/path"));
    for (const QJsonObject &invalid : {
             unknown, credential, fractional, falseSuccess, wrongOutcome, dynamicError}) {
        if (!expect(GatewayControlContract::evaluate(invalid, expected).decision
                        == GatewayControlDecision::Invalid,
                    "invalid control result was accepted")) return 1;
    }
    return 0;
}
