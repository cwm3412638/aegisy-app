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
#include "companion_configuration_cache_presentation.h"
#include "profile_manager.h"

class StatusBadge;

// 两步配置向导：先命名并选择唯一工具，再选择 Key 与模型。
class ConnectWizardDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectWizardDialog(ApiClient *client,
                                 ProfileManager *profileManager,
                                 int editIndex = -1,
                                 QWidget *parent = nullptr);
    ConnectWizardDialog(
        ApiClient *client,
        ProfileManager *profileManager,
        int editIndex,
        const QString &expectedAccountIdentity,
        const CompanionConfigurationCachePresentation &cachedPresentation,
        QWidget *parent = nullptr);

    int resultIndex() const { return m_resultIndex; }
    void setCreateReplacementOnEdit(bool enabled);

private slots:
    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onCompanionModelsReceived(const QString &requestId,
                                   const QString &keyIdentity,
                                   const QJsonObject &projection);
    void onCompanionModelsFailed(const QString &requestId,
                                 const QString &keyIdentity,
                                 const QString &errorCode);
    void onQueryModels();
    void onKeyChanged(int index);
    void onTestConnection();
    void onConnectionTested(const QString &requestId, bool success,
                            const QString &detail, int latencyMs);
    void onTypeChanged(int id);
    void goNext();
    void goBack();

private:
    friend class CompanionCachedDialogsProjectionTestAccess;

    void setupUi();
    QWidget *buildIdentityPage();
    QWidget *buildConnectionPage();
    void updateNavigation();
    void updateToolContext();
    void populateKeyDropdown();
    void setModelLoading(bool loading, const QString &message = QString());
    void applyModels(const QJsonArray &models);
    void applyCachedModels();
    void updateSelectionControls(bool loading = false);
    void scheduleCachedPresentationRefresh();
    void finishProfile();

    AiTool selectedTool() const;
    static QStringList toolModelSuggestions(AiTool tool);
    QString currentKey() const;
    QString currentModel() const;
    ProfileWebsiteBinding currentWebsiteBinding() const;
    QString currentModelKeyIdentity() const;
    bool currentWebsiteSelectionIsCurrent() const;
    bool currentSelectionIsCached() const;

    ApiClient      *m_apiClient;
    ProfileManager *m_profileManager;
    int             m_editIndex = -1;
    int             m_resultIndex = -1;
    bool            m_createReplacementOnEdit = false;

    ProfileType m_selectedType = ProfileType::Codex;
    ProfileType m_existingType = ProfileType::Codex;
    QString     m_existingKey;
    QString     m_existingModel;
    QString     m_existingProfileId;
    ProfileWebsiteBinding m_existingWebsiteBinding;
    bool        m_waitingModels = false;
    bool        m_waitingCompanionModels = false;
    QString     m_modelRequestId;
    QString     m_modelRequestKeyIdentity;
    QString     m_modelRequestAccountIdentity;
    QString     m_modelRequestCredentialHandle;
    QString     m_modelRequestProjectionSha256;
    QString     m_modelRequestPlatform;
    QString     m_connectionRequestId;
    QString     m_connectionRequestKeyIdentity;

    QJsonObject m_companionProjection;
    QString m_expectedAccountIdentity;
    CompanionConfigurationCachePresentation m_cachedPresentation;
    quint64 m_cachePresentationTimerGeneration = 0;

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
    StatusBadge *m_loadingLabel = nullptr;
    QComboBox   *m_modelCombo = nullptr;
    QWidget     *m_modelSuggestions = nullptr;
    bool         m_waitingConnectionTest = false;
};

#endif // CONNECT_WIZARD_H
