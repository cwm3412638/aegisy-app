#ifndef GATEWAY_CONTROL_CONTRACT_H
#define GATEWAY_CONTROL_CONTRACT_H

#include <QJsonObject>
#include <QString>

enum class GatewayControlDecision {
    Accepted,
    Rejected,
    Invalid,
};

struct GatewayControlExpectation {
    QString requestId;
    QString transactionId;
    QString operation;
    QString tool;
};

struct GatewayControlEvaluation {
    GatewayControlDecision decision = GatewayControlDecision::Invalid;
    qint64 revision = -1;
    QString errorCode;
};

class GatewayControlContract
{
public:
    static GatewayControlEvaluation evaluate(
        const QJsonObject &event,
        const GatewayControlExpectation &expectation);
};

#endif // GATEWAY_CONTROL_CONTRACT_H
