#ifndef DESKTOP_ENHANCEMENT_MANAGER_H
#define DESKTOP_ENHANCEMENT_MANAGER_H

#include <QObject>
#include <QList>
#include <QString>

class QNetworkAccessManager;
class QWebSocket;

struct CodexPluginInfo
{
    QString id;
    QString name;
    QString marketplace;
    QString version;
    QString path;
    QString description;
    QString officialDescription;
    bool installed = false;
    bool enabled = false;
};

struct SessionSyncReport
{
    QString provider;
    QString backupPath;
    int sessionFilesChanged = 0;
    int databaseRowsChanged = 0;
    int databasesSkipped = 0;
};

class DesktopEnhancementManager : public QObject
{
    Q_OBJECT

public:
    explicit DesktopEnhancementManager(QObject *parent = nullptr);

    QList<CodexPluginInfo> listCodexPlugins(QString *error = nullptr) const;
    bool installCodexPlugin(const QString &pluginId, QString *output = nullptr,
                            QString *error = nullptr) const;
    SessionSyncReport syncCodexHistory(QString *error = nullptr) const;
    static SessionSyncReport syncCodexHistoryAt(const QString &codexHome,
                                                QString *error = nullptr);

    void localizeClaudeDesktop();
    bool localizationRunning() const;

signals:
    void localizationProgress(const QString &message);
    void localizationFinished(bool success, const QString &message);

private:
    bool launchClaudeWithInspector(QString *error);
    void requestWindowsInspectorAttach();
    void pollInspector();
    void connectInspector(const QString &webSocketUrl);
    void finishLocalization(bool success, const QString &message);
    static QString claudeLocalizationExpression();

    QNetworkAccessManager *m_networkManager;
    QWebSocket *m_webSocket = nullptr;
    int m_pollAttempts = 0;
    bool m_localizationRunning = false;
};

#endif // DESKTOP_ENHANCEMENT_MANAGER_H
