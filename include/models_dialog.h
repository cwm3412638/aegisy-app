#ifndef MODELS_DIALOG_H
#define MODELS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include "api_client.h"

// 模型信息结构（对应 OpenAI 兼容的 /v1/models 返回）
struct ModelInfo {
    QString id;        // 模型 ID，即调用时使用的模型名称
    QString name;      // 同 id，便于展示
    QString provider;  // 提供方 owned_by
    QString created;   // 创建时间（已格式化）

    static ModelInfo fromJson(const QJsonObject &obj);
};

class ModelsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModelsDialog(ApiClient *apiClient, QWidget *parent = nullptr);

signals:
    void modelSelected(const QString &modelName);

private slots:
    void onRefreshClicked();
    void onProviderChanged(int index);
    void onCopyModelClicked();
    void onSearchTextChanged(const QString &text);
    void onTableSelectionChanged();

    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onCompanionModelsReceived(const QString &requestId,
                                   const QString &keyIdentity,
                                   const QJsonObject &projection);
    void onCompanionModelsFailed(const QString &requestId,
                                 const QString &keyIdentity,
                                 const QString &errorCode);

private:
    void setupUi();
    void loadApiKeys();
    void loadModels();
    void updateModelsTable(const QList<ModelInfo> &models);
    void rebuildProviderFilter();
    QString currentKeyIdentity() const;
    ModelInfo getSelectedModel() const;
    void filterModels();

    ApiClient *m_apiClient;

    // UI Elements
    QComboBox *m_keyCombo;
    QComboBox *m_providerCombo;
    QLineEdit *m_searchEdit;
    QPushButton *m_refreshButton;
    QPushButton *m_copyButton;
    QTableWidget *m_modelsTable;
    QLabel *m_totalLabel;
    QLabel *m_statusLabel;

    QList<ModelInfo> m_models;
    QString m_selectedProvider;
    QJsonObject m_companionProjection;
    QString m_modelRequestId;
    QString m_modelRequestKeyIdentity;
    QString m_modelRequestHandle;
    QString m_modelRequestAccountIdentity;
    QString m_modelRequestProjectionSha256;
    QString m_modelRequestPlatform;
};

#endif // MODELS_DIALOG_H
