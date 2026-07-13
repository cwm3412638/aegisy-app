#ifndef CONNECT_WIZARD_H
#define CONNECT_WIZARD_H

#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStringList>
#include <QWidget>

#include "api_client.h"
#include "profile_manager.h"

// 两步配置向导：先命名并选择唯一工具，再选择 Key 与模型。
class ConnectWizardDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectWizardDialog(ApiClient *client,
                                 ProfileManager *profileManager,
                                 int editIndex = -1,
                                 QWidget *parent = nullptr);

    int resultIndex() const { return m_resultIndex; }

private slots:
    void onApiKeysReceived(const QJsonArray &keys);
    void onModelsReceived(const QJsonArray &models);
    void onRequestFailed(const QString &error);
    void onQueryModels();
    void onKeyChanged(int index);
    void onTestConnection();
    void onConnectionTested(const QString &requestId, bool success,
                            const QString &detail, int latencyMs);
    void onTypeChanged(int id);
    void goNext();
    void goBack();

private:
    void setupUi();
    QWidget *buildIdentityPage();
    QWidget *buildConnectionPage();
    void updateNavigation();
    void updateToolContext();
    void populateKeyDropdown();
    void setModelLoading(bool loading, const QString &message = QString());
    void finishProfile();

    AiTool selectedTool() const;
    static QStringList toolModelSuggestions(AiTool tool);
    QString currentKey() const;
    QString currentModel() const;

    ApiClient      *m_apiClient;
    ProfileManager *m_profileManager;
    int             m_editIndex = -1;
    int             m_resultIndex = -1;

    ProfileType m_selectedType = ProfileType::Codex;
    ProfileType m_existingType = ProfileType::Codex;
    QString     m_existingKey;
    QString     m_existingModel;
    bool        m_waitingModels = false;

    QJsonArray m_allKeys;

    QStackedWidget *m_stack = nullptr;
    QLabel         *m_stepLabel = nullptr;
    QPushButton    *m_backButton = nullptr;
    QPushButton    *m_nextButton = nullptr;

    QLineEdit    *m_nameEdit = nullptr;
    QButtonGroup *m_typeGroup = nullptr;

    QLabel      *m_toolBadge = nullptr;
    QLabel      *m_toolTitle = nullptr;
    QLabel      *m_toolPath = nullptr;
    QComboBox   *m_keyCombo = nullptr;
    QPushButton *m_queryButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel      *m_loadingLabel = nullptr;
    QComboBox   *m_modelCombo = nullptr;
    QWidget     *m_modelSuggestions = nullptr;
    bool         m_waitingConnectionTest = false;
};

#endif // CONNECT_WIZARD_H
