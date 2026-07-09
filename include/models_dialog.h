#ifndef MODELS_DIALOG_H
#define MODELS_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include "api_client.h"

// 模型信息结构
struct ModelInfo {
    QString id;
    QString name;
    QString channel;
    QString inputPrice;   // 输入价格（每百万tokens）
    QString outputPrice;  // 输出价格（每百万tokens）
    QString description;

    static ModelInfo fromJson(const QJsonObject &obj);
};

// 渠道信息结构
struct ChannelInfo {
    QString id;
    QString name;
    QString type;
    QStringList models;

    static ChannelInfo fromJson(const QJsonObject &obj);
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
    void onChannelChanged(int index);
    void onCopyModelClicked();
    void onSearchTextChanged(const QString &text);
    void onTableSelectionChanged();

    void onModelsReceived(const QJsonArray &models);
    void onChannelsReceived(const QJsonArray &channels);
    void onRequestFailed(const QString &error);

private:
    void setupUi();
    void loadModels();
    void loadChannels();
    void updateModelsTable(const QList<ModelInfo> &models);
    ModelInfo getSelectedModel() const;
    void filterModels();

    ApiClient *m_apiClient;

    // UI Elements
    QComboBox *m_channelCombo;
    QLineEdit *m_searchEdit;
    QPushButton *m_refreshButton;
    QPushButton *m_copyButton;
    QTableWidget *m_modelsTable;
    QLabel *m_totalLabel;
    QLabel *m_statusLabel;

    QList<ModelInfo> m_models;
    QList<ChannelInfo> m_channels;
    QString m_selectedChannel;
};

#endif // MODELS_DIALOG_H
