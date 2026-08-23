#ifndef API_KEYS_DIALOG_H
#define API_KEYS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QJsonArray>
#include "api_client.h"

struct ApiKeyInfo {
    QString keyIdentity;
    QString updateHandle;
    QString deleteHandle;
    QString testHandle;
    QString groupHandle;
    QString name;
    QString status;
    double quota = 0.0;
    double used = 0.0;
    QString groupName;
    QString platform;
    QString createdAt;
    QString expiresAt;

    static ApiKeyInfo fromJson(const QJsonObject &obj);
};

class ApiKeysDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ApiKeysDialog(ApiClient *apiClient, QWidget *parent = nullptr);

private slots:
    void onRefreshClicked();
    void onTestKeyClicked();
    void onCreateKeyClicked();
    void onEditKeyClicked();
    void onChangeGroupClicked();
    void onToggleStatusClicked();
    void onDeleteKeyClicked();
    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onManagementReceived(const QString &requestId,
                              const QJsonObject &projection);
    void onKeyOperationCompleted(const QString &requestId,
                                 const QString &action,
                                 bool credentialCleanupComplete);
    void onKeyOperationFailed(const QString &requestId,
                              const QString &action,
                              const QString &errorCode);
    void onCompanionModelsReceived(const QString &requestId,
                                   const QString &keyIdentity,
                                   const QJsonObject &projection);
    void onCompanionModelsFailed(const QString &requestId,
                                 const QString &keyIdentity,
                                 const QString &errorCode);
    void onTableSelectionChanged();

private:
    friend class ApiKeysDialogTestAccess;

    void setupUi();
    void loadApiKeys();
    void updateKeysTable(const QList<ApiKeyInfo> &keys);
    ApiKeyInfo getSelectedKey() const;
    void showKeyEditor(const ApiKeyInfo *existing = nullptr);
    void setMutationControlsEnabled(bool enabled);
    void clearManagementView();

    ApiClient *m_apiClient;
    QTableWidget *m_keysTable;
    QPushButton *m_refreshButton;
    QPushButton *m_testButton;
    QPushButton *m_createButton;
    QPushButton *m_editButton;
    QPushButton *m_groupButton;
    QPushButton *m_toggleButton;
    QPushButton *m_deleteButton;
    QLabel *m_statusLabel;
    QLabel *m_totalKeysLabel;

    QList<ApiKeyInfo> m_keys;
    QJsonArray m_groups;
    QString m_accountIdentity;
    QString m_configurationProjectionSha256;
    QString m_managementProjectionSha256;
    QString m_managementRequestId;
    QString m_operationRequestId;
    QString m_pendingAction;
    QString m_testRequestId;
    QString m_testKeyIdentity;
};

#endif // API_KEYS_DIALOG_H
