#ifndef RUNTIME_STATUS_STORE_H
#define RUNTIME_STATUS_STORE_H

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include "tool_manager.h"

enum class RuntimeStatusProvenance {
    Unknown,
    Configured,
    InAppChat,
    Gateway,
};

struct RuntimeStatusSnapshot {
    QString toolId;
    QString toolName;
    QString model;
    QString reasoning;
    qint64 inputTokens = -1;
    qint64 outputTokens = -1;
    qint64 totalTokens = -1;
    qint64 contextLimit = -1;
    double balance = 0.0;
    bool balanceKnown = false;
    bool active = false;
    bool monitored = false;
    bool gatewayRunning = false;
    bool requestObserved = false;
    RuntimeStatusProvenance provenance = RuntimeStatusProvenance::Unknown;
    QDateTime updatedAt;

    bool usageKnown() const { return inputTokens >= 0; }
    bool contextLimitKnown() const { return contextLimit > 0; }
};

class RuntimeStatusStore : public QObject
{
    Q_OBJECT

public:
    explicit RuntimeStatusStore(QObject *parent = nullptr);

    RuntimeStatusSnapshot snapshot() const { return m_snapshot; }

    void setConfiguredProfile(AiTool tool, const QString &model,
                              const QString &reasoning = QString(),
                              qint64 contextLimit = -1);
    void clearConfiguredProfile();
    void setBalance(double balance, bool known);
    void setGatewayRunning(bool running);
    void observeGatewayEvent(const QJsonObject &event);

    void beginChat(const QString &model, const QString &reasoning,
                   qint64 contextLimit);
    void updateChatUsage(qint64 inputTokens, qint64 outputTokens,
                         qint64 totalTokens);
    void finishChat();

signals:
    void statusChanged(const RuntimeStatusSnapshot &snapshot);

private:
    void publish();
    static QString configuredToolId(AiTool tool);
    static QString configuredToolName(AiTool tool);
    static QString gatewayToolName(const QString &toolId);

    RuntimeStatusSnapshot m_snapshot;
};

Q_DECLARE_METATYPE(RuntimeStatusSnapshot)

#endif // RUNTIME_STATUS_STORE_H
