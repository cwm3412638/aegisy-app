#ifndef API_KEYS_DIALOG_H
#define API_KEYS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QJsonArray>
#include "api_client.h"

// API Key 数据结构
struct ApiKeyInfo {
    QString id;
    QString name;
    QString key;
    QString status;
    int quota;
    int used;
    QString createdAt;
    QString expiresAt;
    bool isActive;

    static ApiKeyInfo fromJson(const QJsonObject &obj);
};

class ApiKeysDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApiKeysDialog(ApiClient *apiClient, QWidget *parent = nullptr);

    // 设置当前激活的 Key
    void setActiveKey(const QString &keyId);

signals:
    // Key 切换信号
    void keyActivated(const QString &keyId, const QString &key);

private slots:
    void onRefreshClicked();
    void onCopyKeyClicked();
    void onActivateKeyClicked();
    void onKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);
    void onTableSelectionChanged();

private:
    void setupUi();
    void loadApiKeys();
    void updateKeysTable(const QList<ApiKeyInfo> &keys);
    ApiKeyInfo getSelectedKey() const;

    ApiClient *m_apiClient;
    QTableWidget *m_keysTable;
    QPushButton *m_refreshButton;
    QPushButton *m_copyButton;
    QPushButton *m_activateButton;
    QLabel *m_statusLabel;
    QLabel *m_totalKeysLabel;

    QList<ApiKeyInfo> m_keys;
    QString m_activeKeyId;
};

#endif // API_KEYS_DIALOG_H
