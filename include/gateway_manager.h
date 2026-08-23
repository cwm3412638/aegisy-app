#ifndef GATEWAY_MANAGER_H
#define GATEWAY_MANAGER_H

#include <QObject>
#include <QJsonObject>
#include <QHash>
#include <QList>

#include "tool_manager.h"

class QProcess;

class GatewayManager : public QObject
{
    Q_OBJECT

public:
    explicit GatewayManager(QObject *parent = nullptr);
    ~GatewayManager() override;

    bool start();
    void stop();
    bool isRunning() const { return m_running; }
    QString localToken() const { return m_localToken; }
    QString endpoint(AiTool tool) const;
    QString lastError() const { return m_lastError; }

    bool configureProfile(AiTool tool, const QString &apiKey);
    bool prepareProfile(AiTool tool, const QString &apiKey, QString *transactionId);
    bool commitProfile(AiTool tool, const QString &transactionId);
    bool abortProfile(AiTool tool, const QString &transactionId);
    bool removeProfile(AiTool tool);
    QList<QJsonObject> requestLogs() const { return m_requestLogs; }
    void clearRequestLogs();

signals:
    void runningChanged(bool running);
    void requestLogged(const QJsonObject &entry);
    void runtimeEvent(const QJsonObject &event);
    void gatewayError(const QString &error);

private:
    QString ensureGatewayScript();
    void processOutput(QProcess *process, quint64 generation);
    void handleEvent(const QJsonObject &event, quint64 generation);
    bool sendControlAndWait(AiTool tool, const QString &operation,
                            const QString &transactionId,
                            const QString &apiKey = QString());
    void failCurrentGeneration(const QString &errorCode);
    static QString toolSlug(AiTool tool);

    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_localToken;
    QString m_lastError;
    bool m_running = false;
    quint64 m_generation = 0;
    QHash<int, qint64> m_toolRevisions;
    QString m_expectedRequestId;
    QString m_expectedTransactionId;
    QString m_expectedOperation;
    QString m_expectedTool;
    quint64 m_expectedGeneration = 0;
    bool m_controlWaiting = false;
    bool m_controlSucceeded = false;
    QList<QJsonObject> m_requestLogs;
};

#endif // GATEWAY_MANAGER_H
