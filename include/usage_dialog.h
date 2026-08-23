#ifndef USAGE_DIALOG_H
#define USAGE_DIALOG_H

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class ApiClient;
class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class UsageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UsageDialog(ApiClient *apiClient, QWidget *parent = nullptr);

private slots:
    void refreshData();
    void onStatsReceived(const QJsonObject &stats);
    void onModelsReceived(const QJsonArray &models);
    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onCompanionApiKeyUsageReceived(const QString &requestId,
                                        const QJsonObject &projection);
    void onCompanionApiKeyUsageFailed(const QString &requestId,
                                      const QString &errorCode);
    void onRequestFailed(const QString &error);

private:
    void setupUi();
    void updateSummary();
    void updateModelsTable();
    void updateKeysTable();

    ApiClient *m_apiClient;
    QComboBox *m_rangeCombo = nullptr;
    QLabel *m_costValue = nullptr;
    QLabel *m_requestsValue = nullptr;
    QLabel *m_tokensValue = nullptr;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QTableWidget *m_modelsTable = nullptr;
    QTableWidget *m_keysTable = nullptr;
    QJsonObject m_stats;
    QJsonArray m_models;
    QJsonArray m_keys;
    QString m_usageRequestId;
    QString m_usageAccountIdentity;
    QString m_usageConfigurationProjectionSha256;
    int m_pendingRequests = 0;
    bool m_companionConfigurationRetired = false;
};

#endif // USAGE_DIALOG_H
