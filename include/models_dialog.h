#ifndef MODELS_DIALOG_H
#define MODELS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include "api_client.h"
#include "companion_configuration_cache_presentation.h"

// 模型信息结构（对应 OpenAI 兼容的 /v1/models 返回）
struct ModelInfo {
    QString id;        // 模型 ID，即调用时使用的模型名称
    QString name;      // 同 id，便于展示
    QString provider;  // 提供方 owned_by
    QString created;   // 创建时间（已格式化）
    QString source;    // 网站实时或本地认证缓存

    static ModelInfo fromJson(const QJsonObject &obj);
};

class ModelsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModelsDialog(ApiClient *apiClient, QWidget *parent = nullptr);
    ModelsDialog(
        ApiClient *apiClient,
        const QString &expectedAccountIdentity,
        const CompanionConfigurationCachePresentation &cachedPresentation,
        QWidget *parent = nullptr);

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
    friend class CompanionCachedDialogsProjectionTestAccess;

    void setupUi();
    void loadApiKeys();
    void loadModels();
    void renderCachedPresentation(const QString &liveError = QString());
    void renderCachedModels();
    void clearModels();
    void scheduleCachedPresentationRefresh();
    void updateModelsTable(const QList<ModelInfo> &models);
    void rebuildProviderFilter();
    QString currentKeyIdentity() const;
    ModelInfo getSelectedModel() const;
    void filterModels();

    ApiClient *m_apiClient;
    enum class SourceMode { None, CachedDisplay, LiveWebsite };
    SourceMode m_sourceMode = SourceMode::None;
    QString m_expectedAccountIdentity;
    CompanionConfigurationCachePresentation m_cachedPresentation;
    quint64 m_cachePresentationTimerGeneration = 0;

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
