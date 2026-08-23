#include "gateway_control_contract.h"

#include <QSet>

GatewayControlEvaluation GatewayControlContract::evaluate(
    const QJsonObject &event,
    const GatewayControlExpectation &expectation)
{
    const QSet<QString> expectedKeys{
        QStringLiteral("schema"), QStringLiteral("type"),
        QStringLiteral("request_id"), QStringLiteral("transaction_id"),
        QStringLiteral("operation"), QStringLiteral("tool"),
        QStringLiteral("outcome"), QStringLiteral("revision"),
        QStringLiteral("credential_included"), QStringLiteral("error_code")};
    const QStringList keyList = event.keys();
    const QSet<QString> actualKeys(keyList.cbegin(), keyList.cend());
    const double revisionNumber = event.value(QStringLiteral("revision")).toDouble(-1);
    const qint64 revision = static_cast<qint64>(revisionNumber);
    if (actualKeys != expectedKeys
            || event.value(QStringLiteral("schema")).toString()
                != QStringLiteral("aegisy-gateway-control/0.1")
            || event.value(QStringLiteral("type")).toString()
                != QStringLiteral("control-result")
            || event.value(QStringLiteral("request_id")).toString()
                != expectation.requestId
            || event.value(QStringLiteral("transaction_id")).toString()
                != expectation.transactionId
            || event.value(QStringLiteral("operation")).toString()
                != expectation.operation
            || event.value(QStringLiteral("tool")).toString() != expectation.tool
            || event.value(QStringLiteral("credential_included")).toBool(true)
            || revisionNumber < 0 || revisionNumber > 9007199254740991.0
            || static_cast<double>(revision) != revisionNumber) {
        return {};
    }

    const QString outcome = event.value(QStringLiteral("outcome")).toString();
    const QString errorCode = event.value(QStringLiteral("error_code")).toString();
    const QString expectedOutcome = expectation.operation == QStringLiteral("commit")
        ? QStringLiteral("committed")
        : (expectation.operation == QStringLiteral("abort")
            ? QStringLiteral("aborted") : QStringLiteral("prepared"));
    if (outcome == expectedOutcome && errorCode.isEmpty()) {
        return {GatewayControlDecision::Accepted, revision, {}};
    }
    if (outcome == QStringLiteral("rejected") && !errorCode.isEmpty()
            && errorCode.size() <= 96) {
        for (const QChar character : errorCode) {
            if (!character.isLower() && !character.isDigit()
                    && character != QLatin1Char('-')) {
                return {};
            }
        }
        return {GatewayControlDecision::Rejected, revision, errorCode};
    }
    return {};
}
