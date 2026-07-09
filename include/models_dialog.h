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

    void onModelsReceived(const QJsonArray &models);
    void onApiKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);

private:
    void setupUi();
    void loadApiKeys();
    void loadModels();
    void updateModelsTable(const QList<ModelInfo> &models);
    void rebuildProviderFilter();
    ModelInfo getSelectedModel() const;
    void filterModels();

    ApiClient *m_apiClient;

    // UI Elements
    QLineEdit *m_keyEdit;
    QComboBox *m_providerCombo;
    QLineEdit *m_searchEdit;
    QPushButton *m_refreshButton;
    QPushButton *m_copyButton;
    QTableWidget *m_modelsTable;
    QLabel *m_totalLabel;
    QLabel *m_statusLabel;

    QList<ModelInfo> m_models;
    QString m_selectedProvider;
};

#endif // MODELS_DIALOG_H
