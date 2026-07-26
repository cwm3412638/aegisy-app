#ifndef API_CLIENT_H
#define API_CLIENT_H

#include <QObject>
#include <QString>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

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
    void getApiKeyUsage(const QJsonArray &apiKeyIds);
    void getWorkbenchEmergencyPolicy();
    void getChannels();        // 获取渠道列表
    void getGroups();
    void changePassword(const QString &oldPassword, const QString &newPassword);
    void redeemCode(const QString &code);
    void createApiKey(const QJsonObject &data);
    void updateApiKey(const QString &keyId, const QJsonObject &data);
    void deleteApiKey(const QString &keyId);
    // 获取账号支持的模型列表（调用 OpenAI 兼容的 /v1/models，需传入 sk- API Key）
    void getModels(const QString &apiKey);

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

    // 测试某个 API Key 是否可用（对 /v1/models 发起请求），结果通过 apiKeyTested 返回
    void testApiKey(const QString &keyId, const QString &apiKey);
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
    void apiKeysReceived(const QJsonArray &keys);

    // 用户信息获取成功
    void userInfoReceived(const QJsonObject &userInfo);
    void usageStatsReceived(const QJsonObject &stats);
    void usageModelsReceived(const QJsonArray &models);
    void apiKeyUsageReceived(const QJsonObject &usageByKey);
    void workbenchEmergencyPolicyReceived(const QJsonObject &policy);
    void workbenchEmergencyPolicyFailed(const QString &errorCode);

    // 渠道列表获取成功
    void channelsReceived(const QJsonArray &channels);
    void groupsReceived(const QJsonArray &groups);
    void passwordChanged();
    void passwordChangeFailed(const QString &errorMessage);
    void redeemCompleted(const QJsonObject &result);
    void redeemFailed(const QString &errorMessage);
    void apiKeyOperationCompleted(const QString &action, const QJsonObject &result);
    void apiKeyOperationFailed(const QString &action, const QString &errorMessage);

    // 模型列表获取成功
    void modelsReceived(const QJsonArray &models);

    // 图片生成完成，imageData 为已解码的原始图片字节。
    void imageGenerated(const QByteArray &imageData,
                        const QString &outputFormat,
                        const QString &revisedPrompt);
    void imageGenerationFailed(const QString &errorMessage);
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
    void apiKeyTested(const QString &keyId, bool supported, const QString &detail);
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
    void onApiKeyUsageFinished();
    void onChannelsFinished();
    void onModelsFinished();
    void onImageGenerationFinished();

private:
    void requestApiKeysPage(int page, int generation);
    void requestUserInfo(const QString &endpoint);
    void requestPresentationPlanAttempt(const QString &requestId,
                                        const QString &apiKey,
                                        const QString &model,
                                        const QString &requestText,
                                        const QString &invalidContent,
                                        int attempt,
                                        bool structuredOutput,
                                        int exhaustedRetries = 0);
    void processImageGenerationEvents(bool flushTrailingData);
    void processImageGenerationPayload(const QByteArray &payload);

    QNetworkAccessManager *m_networkManager;
    QString m_baseUrl;
    QString m_authToken;
    QJsonArray m_apiKeyAccumulator;
    int m_apiKeyGeneration = 0;
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
    bool m_authExpirationEmitted = false;

    // 通用 POST 请求
    QNetworkReply* post(const QString &endpoint, const QJsonObject &data);
    QNetworkReply* put(const QString &endpoint, const QJsonObject &data);
    QNetworkReply* deleteRequest(const QString &endpoint);

    // 通用 GET 请求（bearerToken 为空时使用已设置的 m_authToken）
    QNetworkReply* get(const QString &endpoint, const QString &bearerToken = QString());

    // 解析响应
    QJsonObject parseResponse(QNetworkReply *reply, bool &ok);
};

#endif // API_CLIENT_H
