#ifndef WORKBENCH_EMERGENCY_POLICY_H
#define WORKBENCH_EMERGENCY_POLICY_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QSettings;

namespace WorkbenchEmergencyPolicy {

enum class State {
    NoPolicy,
    Enabled,
    Disabled,
    Stale,
    Invalid,
};

struct Decision {
    State state = State::NoPolicy;
    bool blocksNewWork = false;
    quint64 sequence = 0;
    qint64 expiresAtMs = 0;
    QString reasonCode;
    QString policyIdentity;
    QString errorCode;
};

struct InstallResult {
    bool accepted = false;
    Decision decision;
    QString errorCode;
};

QByteArray signaturePayload(const QJsonObject &envelope, QString *errorCode = nullptr);
Decision verify(const QJsonObject &envelope, const QByteArray &publicKeyBase64,
                qint64 nowMs, bool requireFresh = true);
Decision load(QSettings *settings, const QByteArray &publicKeyBase64, qint64 nowMs);
InstallResult install(QSettings *settings, const QJsonObject &envelope,
                      const QByteArray &publicKeyBase64, qint64 nowMs);

} // namespace WorkbenchEmergencyPolicy

#endif // WORKBENCH_EMERGENCY_POLICY_H
