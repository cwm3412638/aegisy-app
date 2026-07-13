#ifndef GATEWAY_MANAGER_H
#define GATEWAY_MANAGER_H

#include <QObject>
#include <QJsonObject>
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
    QList<QJsonObject> requestLogs() const { return m_requestLogs; }
    void clearRequestLogs();

signals:
    void runningChanged(bool running);
    void requestLogged(const QJsonObject &entry);
    void gatewayError(const QString &error);

private:
    QString ensureGatewayScript();
    void processOutput();
    void handleEvent(const QJsonObject &event);
    static QString toolSlug(AiTool tool);

    QProcess *m_process = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_localToken;
    QString m_lastError;
    bool m_running = false;
    QList<QJsonObject> m_requestLogs;
};

#endif // GATEWAY_MANAGER_H
