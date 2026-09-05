#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QHash>
#include <QList>
#include <QSet>

class ApiClient : public QObject
{
    Q_OBJECT

public:
    explicit ApiClient(QObject *parent = nullptr);
    ~ApiClient();

    // 设置 API 基础 URL
    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    // 设置认证 Token
    void setAuthToken(const QString &token);

    // API 方法
    void login(const QString &email, const QString &password);
    void getApiKeys();
    void getUserInfo();
    void getUsageStats(int days);
    void getUsageModels(int days);
    void getCompanionApiKeyUsage(const QString &requestId,
                                 const QString &accountIdentity,
                                 const QString &projectionSha256);
    void getWorkbenchEmergencyPolicy();
    void getChannels();        // 获取渠道列表
    void changePassword(const QString &oldPassword, const QString &newPassword);
    void redeemCode(const QString &code);
    void getCompanionKeyManagement(const QString &requestId,
                                   const QString &accountIdentity,
                                   const QString &projectionSha256);
    void createCompanionApiKey(const QString &requestId,
                               const QString &accountIdentity,
                               const QString &projectionSha256,
                               const QString &managementProjectionSha256,
                               const QString &createHandle,
                               const QString &groupHandle,
                               const QString &name,
                               qint64 quota);
    void updateCompanionApiKey(const QString &requestId,
                               const QString &accountIdentity,
                               const QString &keyIdentity,
                               const QString &updateHandle,
                               const QString &projectionSha256,
                               const QString &managementProjectionSha256,
                               const QJsonObject &data);
    void deleteCompanionApiKey(const QString &requestId,
                               const QString &accountIdentity,
                               const QString &keyIdentity,
                               const QString &deleteHandle,
                               const QString &projectionSha256,
                               const QString &managementProjectionSha256);
    void testCompanionApiKey(const QString &requestId,
                             const QString &accountIdentity,
                             const QString &keyIdentity,
                             const QString &testHandle,
                             const QString &projectionSha256,
                             const QString &managementProjectionSha256);
    void getCompanionModels(const QString &requestId,
                            const QString &accountIdentity,
                            const QString &keyIdentity,
                            const QString &credentialHandle,
                            const QString &projectionSha256,
                            const QString &platform);
    void getProfileModels(const QString &requestId,
                          const QString &profileIdentity,
                          const QString &credential);
    void sendCompanionChatMessage(const QString &requestId,
                                  const QString &accountIdentity,
                                  const QString &keyIdentity,
                                  const QString &credentialHandle,
                                  const QString &projectionSha256,
                                  const QString &platform,
                                  const QString &model,
                                  const QJsonArray &messages);
    void generateCompanionImage(const QString &requestId,
                                const QString &accountIdentity,
                                const QString &keyIdentity,
                                const QString &credentialHandle,
                                const QString &projectionSha256,
                                const QString &platform,
                                const QString &model,
                                const QString &prompt,
                                const QString &size,
                                const QString &quality,
                                const QString &outputFormat);
    void requestCompanionPresentationPlan(const QString &requestId,
                                          const QString &accountIdentity,
                                          const QString &keyIdentity,
                                          const QString &credentialHandle,
                                          const QString &projectionSha256,
                                          const QString &platform,
                                          const QString &model,
                                          const QString &request);

    // 使用指定的图片分组 API Key 调用 OpenAI 兼容的生图端点。
    void generateImage(const QString &apiKey,
                       const QString &model,
                       const QString &prompt,
                       const QString &size,
                       const QString &quality,
                       const QString &outputFormat);
    void cancelImageGeneration();
    void sendChatMessage(const QString &requestId,
                         const QString &apiKey,
                         const QString &model,
                         const QJsonArray &messages);
    void cancelChatMessage();
    void requestPresentationPlan(const QString &requestId,
                                 const QString &apiKey,
                                 const QString &model,
                                 const QString &request);

    void testConnection(const QString &requestId,
                        const QString &apiKey,
                        const QString &model = QString());

signals:
    // 登录成功信号
    void loginSuccess(const QString &token, const QJsonObject &userData);

    // 登录失败信号
    void loginFailed(const QString &errorMessage);
    void authenticationExpired();

    // API Keys 获取成功
    void companionConfigurationReceived(const QJsonObject &projection);
    void companionConfigurationFailed(const QString &errorCode);

    // 用户信息获取成功
    void userInfoReceived(const QJsonObject &userInfo);
    void usageStatsReceived(const QJsonObject &stats);
    void usageModelsReceived(const QJsonArray &models);
    void companionApiKeyUsageReceived(const QString &requestId,
                                      const QJsonObject &projection);
    void companionApiKeyUsageFailed(const QString &requestId,
                                    const QString &errorCode);
    void companionKeyManagementReceived(const QString &requestId,
                                        const QJsonObject &projection);
    void companionKeyOperationCompleted(const QString &requestId,
                                        const QString &action,
                                        bool credentialCleanupComplete);
    void companionKeyOperationFailed(const QString &requestId,
                                     const QString &action,
                                     const QString &errorCode);
    void workbenchEmergencyPolicyReceived(const QJsonObject &policy);
    void workbenchEmergencyPolicyFailed(const QString &errorCode);

    // 渠道列表获取成功
    void channelsReceived(const QJsonArray &channels);
    void passwordChanged();
    void passwordChangeFailed(const QString &errorMessage);
    void redeemCompleted(const QJsonObject &result);
    void redeemFailed(const QString &errorMessage);

    void companionModelsReceived(const QString &requestId,
                                 const QString &keyIdentity,
                                 const QJsonObject &projection);
    void companionModelsFailed(const QString &requestId,
                               const QString &keyIdentity,
                               const QString &errorCode);
    void companionWebsiteModelsObserved(
        const QString &accountIdentity,
        const QString &configurationSha256,
        const QString &keyIdentity,
        const QString &platform,
        const QJsonObject &projection,
        qint64 observedAtMs);

    // 图片生成完成，imageData 为已解码的原始图片字节。
    void imageGenerated(const QByteArray &imageData,
                        const QString &outputFormat,
                        const QString &revisedPrompt);
    void imageGenerationFailed(const QString &errorMessage);
    void companionImageGenerated(const QString &requestId,
                                 const QByteArray &imageData,
                                 const QString &outputFormat,
                                 const QString &revisedPrompt);
    void companionImageFailed(const QString &requestId,
                              const QString &errorCode);
    void chatChunkReceived(const QString &requestId, const QString &chunk);
    void chatUsageReceived(const QString &requestId,
                           int promptTokens,
                           int completionTokens,
                           int totalTokens);
    void chatCompleted(const QString &requestId, const QString &content);
    void chatFailed(const QString &requestId, const QString &errorMessage);
    void presentationPlanReceived(const QString &requestId, const QJsonObject &plan);
    void presentationPlanFailed(const QString &requestId, const QString &errorMessage);

    // 某个 API Key 测试完成：supported 表示是否可用，detail 为说明
    void connectionTested(const QString &requestId,
                          bool success,
                          const QString &detail,
                          int latencyMs);

    // 通用错误信号
    void requestFailed(const QString &errorMessage);

private slots:
    void onLoginFinished();
    void onApiKeysFinished();
    void onUserInfoFinished();
    void onUsageStatsFinished();
    void onUsageModelsFinished();
    void onChannelsFinished();
    void onCompanionModelsFinished();
    void onCompanionApiKeyUsageFinished();
    void onCompanionKeyOperationFinished();
    void onImageGenerationFinished();

private:
    friend class CompanionKeyManagementApiTestAccess;

    struct CompanionCredentialBinding {
        QString requestId;
        QString accountIdentity;
        QString keyIdentity;
        QString credentialHandle;
        QString projectionSha256;
        QString platform;
        quint64 authGeneration = 0;

        bool isEmpty() const { return requestId.isEmpty(); }
    };

    struct CompanionUsageSource {
        QJsonValue rawKeyId;
        QString rawLookupKey;
        QString keyIdentity;
        QString updateHandle;
        QString deleteHandle;
        QString testHandle;
        QString groupHandle;
        QString credentialHandle;
        qint64 rawGroupId = 0;
        double quotaUsed = 0.0;
        double quota = 0.0;
        QString createdAt;
        QString expiresAt;
        qint64 managementExpiresAtMs = 0;
    };

    struct CompanionGroupSource {
        qint64 rawGroupId = 0;
        QString groupIdentity;
        QString groupHandle;
        QString createHandle;
        QString displayName;
        QString platform;
        qint64 managementExpiresAtMs = 0;
    };

    struct PendingCompanionUsageRequest {
        QString accountIdentity;
        QString projectionSha256;
        quint64 authGeneration = 0;
        QList<CompanionUsageSource> sources;
    };

    struct PendingCompanionKeyOperation {
        QString action;
        QString accountIdentity;
        QString projectionSha256;
        QString managementProjectionSha256;
        QString keyIdentity;
        QString actionHandle;
        QString credentialHandle;
        quint64 authGeneration = 0;
    };

    void requestApiKeysPage(int page, int generation);
    void requestUserInfo(const QString &endpoint, quint64 requestGeneration);
    void requestPresentationPlanAttempt(const QString &requestId,
                                        const QString &apiKey,
                                        const QString &model,
                                        const QString &requestText,
                                        const QString &invalidContent,
                                        int attempt,
                                        bool structuredOutput,
                                        int exhaustedRetries = 0,
                                        bool companionBound = false);
    void processImageGenerationEvents(bool flushTrailingData);
    void processImageGenerationPayload(const QByteArray &payload);
    void processChatEvents(bool flushTrailingData);
    void processChatEventLine(const QByteArray &line);
    void startCorrelatedModelRequest(const QString &requestId,
                                     const QString &accountIdentity,
                                     const QString &keyIdentity,
                                     const QString &credential,
                                     const QString &projectionSha256 = QString(),
                                     const QString &platform = QString(),
                                     const QString &credentialHandle = QString());
    void retireCompanionModelRequests(const QString &errorCode);
    void retireCompanionUsageRequests(const QString &errorCode);
    void retireCompanionKeyOperations(const QString &errorCode);
    void clearCompanionConfigurationAuthority();
    void failCurrentCompanionConfiguration(const QString &errorCode);
    void failCurrentCompanionAccount(const QString &errorCode);
    bool companionKeyManagementBindingIsCurrent(
        const QString &accountIdentity,
        const QString &projectionSha256,
        const QString &managementProjectionSha256,
        const QString &keyIdentity = QString(),
        const QString &action = QString(),
        const QString &actionHandle = QString(),
        CompanionUsageSource *source = nullptr) const;
    void startCompanionKeyOperation(
        const QString &requestId,
        const QString &action,
        const QString &accountIdentity,
        const QString &projectionSha256,
        const QString &managementProjectionSha256,
        const QString &actionHandle,
        const QJsonObject &data,
        const CompanionUsageSource *source = nullptr);
    bool resolveCompanionCredential(const QString &requestId,
                                    const QString &accountIdentity,
                                    const QString &keyIdentity,
                                    const QString &credentialHandle,
                                    const QString &projectionSha256,
                                    const QString &platform,
                                    CompanionCredentialBinding *binding,
                                    QString *credential,
                                    QString *errorCode) const;
    bool companionBindingIsCurrent(
        const CompanionCredentialBinding &binding) const;
    void retireCompanionOperationRequests(const QString &errorCode);

    QNetworkAccessManager *m_networkManager;
    QString m_baseUrl;
    QString m_authToken;
    QJsonArray m_apiKeyAccumulator;
    int m_apiKeyGeneration = 0;
    quint64 m_authGeneration = 0;
    quint64 m_userInfoGeneration = 0;
    quint64 m_verifiedAccountAuthGeneration = 0;
    QString m_verifiedCompanionAccountIdentity;
    QHash<QString, QString> m_pendingCompanionModelRequests;
    QList<CompanionUsageSource> m_companionUsageSources;
    QHash<QString, PendingCompanionUsageRequest> m_pendingCompanionUsageRequests;
    QHash<QString, PendingCompanionKeyOperation> m_pendingCompanionKeyOperations;
    QHash<QString, PendingCompanionKeyOperation> m_pendingCompanionKeyTests;
    QJsonObject m_currentCompanionKeyManagementProjection;
    QList<CompanionGroupSource> m_currentCompanionGroupSources;
    QJsonObject m_currentCompanionProjection;
    QHash<QString, QJsonObject> m_currentCompanionModelProjections;
    QString m_companionModelProjectionConfigurationSha256;
    CompanionCredentialBinding m_companionChatBinding;
    CompanionCredentialBinding m_companionImageBinding;
    QHash<QString, CompanionCredentialBinding> m_companionPresentationBindings;
    QNetworkReply *m_imageGenerationReply = nullptr;
    QByteArray m_imageGenerationBuffer;
    QString m_imageGenerationBase64;
    QString m_imageGenerationPartialBase64;
    QString m_imageGenerationFormat;
    QString m_imageGenerationRevisedPrompt;
    QString m_imageGenerationError;
    QNetworkReply *m_chatReply = nullptr;
    QByteArray m_chatBuffer;
    QString m_chatContent;
    QString m_chatRequestId;
    bool m_chatSawStreamEvent = false;
    bool m_chatSawDone = false;
    bool m_chatMalformedEvent = false;
    bool m_authExpirationEmitted = false;

    // 通用 POST 请求
    QNetworkReply* post(const QString &endpoint, const QJsonObject &data);
    QNetworkReply* put(const QString &endpoint, const QJsonObject &data);

    // 通用 GET 请求（bearerToken 为空时使用已设置的 m_authToken）
    QNetworkReply* get(const QString &endpoint, const QString &bearerToken = QString());

    // 解析响应
    QJsonObject parseResponse(QNetworkReply *reply, bool &ok);
};

#endif // API_CLIENT_H
