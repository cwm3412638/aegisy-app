#ifndef CHAT_DIALOG_H
#define CHAT_DIALOG_H

#include <QDateTime>
#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

class ApiClient;
class SkillManager;
class ProfileManager;
class RuntimeStatusStore;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(ApiClient *apiClient,
                        SkillManager *skillManager,
                        ProfileManager *profileManager = nullptr,
                        RuntimeStatusStore *runtimeStatusStore = nullptr,
                        QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void reject() override;

private slots:
    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onCompanionModelsReceived(const QString &requestId,
                                   const QString &keyIdentity,
                                   const QJsonObject &projection);
    void onCompanionModelsFailed(const QString &requestId,
                                 const QString &keyIdentity,
                                 const QString &errorCode);
    void onKeyChanged(int index);
    void onModelChanged(int index);
    void onSendClicked();
    void onStopClicked();
    void onNewSession();
    void onSessionChanged(int row);
    void onDeleteSession();
    void onCopyConversation();
    void onChatChunk(const QString &requestId, const QString &chunk);
    void onChatUsage(const QString &requestId,
                     int promptTokens,
                     int completionTokens,
                     int totalTokens);
    void onChatCompleted(const QString &requestId, const QString &content);
    void onChatFailed(const QString &requestId, const QString &error);
    void onSkillImageGenerated(const QString &requestId,
                               const QByteArray &imageData,
                               const QString &outputFormat,
                               const QString &revisedPrompt);
    void onSkillImageFailed(const QString &requestId, const QString &error);
    void onPresentationPlanReceived(const QString &requestId, const QJsonObject &plan);
    void onPresentationPlanFailed(const QString &requestId, const QString &error);

private:
    struct ChatSession {
        QString id;
        QString title;
        QString keyIdentity;
        QString keyName;
        QString model;
        QJsonArray messages;
        QDateTime createdAt;
        QDateTime updatedAt;
        int promptTokens = 0;
        int completionTokens = 0;
        int totalTokens = 0;
    };

    void setupUi();
    void setGenerating(bool generating);
    void rebuildSessionList();
    void rebuildMessages();
    void addMessageWidget(const QString &role,
                          const QString &content,
                          int messageIndex,
                          QTextBrowser **contentBrowser = nullptr);
    void startRequest();
    bool startMatchedSkill(const QString &requestText);
    void selectQuickSkill(const QString &skillId);
    void clearQuickSkill();
    void finishSkillRun(const QString &content,
                        const QString &attachmentPath = QString(),
                        const QString &attachmentType = QString());
    int imageSkillCandidateIndex() const;
    void resendUserMessage(int messageIndex);
    void editUserMessage(int messageIndex);
    void regenerateAssistantMessage(int messageIndex);
    void copyMessage(const QString &content);
    void truncateMessagesAfter(int messageIndex);
    void scrollToBottom();
    void updateSessionTitle(const QString &firstMessage);
    void updateContextInfo();
    void applyCurrentSessionSelection();
    // 让 Key 下拉跟随最近激活的档案：匹配到则选中，返回是否命中。
    bool selectActiveProfileKey(bool force);
    void loadHistory();
    void saveHistory() const;
    QString historyPath() const;
    QString selectedCredentialHandle() const;
    QString selectedAccountIdentity() const;
    QString selectedKeyIdentity() const;
    QString selectedProjectionSha256() const;
    QString selectedPlatform() const;
    QString selectedKeyName() const;
    int estimatedContextTokens(const QJsonArray &messages) const;
    int selectedContextWindow() const;

    ApiClient *m_apiClient = nullptr;
    SkillManager *m_skillManager = nullptr;
    ProfileManager *m_profileManager = nullptr;
    RuntimeStatusStore *m_runtimeStatusStore = nullptr;
    QListWidget *m_sessionList = nullptr;
    QComboBox *m_keyCombo = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_contextLabel = nullptr;
    QLabel *m_editingLabel = nullptr;
    QLabel *m_skillsLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_messagesContainer = nullptr;
    QVBoxLayout *m_messagesLayout = nullptr;
    QPlainTextEdit *m_inputEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_copyConversationButton = nullptr;
    QLabel *m_sessionStatsLabel = nullptr;
    QPushButton *m_imageQuickButton = nullptr;
    QPushButton *m_presentationQuickButton = nullptr;
    QTextBrowser *m_streamBrowser = nullptr;

    QList<ChatSession> m_sessions;
    int m_currentSession = -1;
    int m_editingMessageIndex = -1;
    QString m_pendingModel;
    QString m_requestId;
    QString m_streamContent;
    QString m_pendingSkillId;
    QString m_pendingSkillRequest;
    QString m_skillRequestId;
    QString m_forcedSkillId;
    QString m_instructionSkillId;
    QJsonObject m_companionProjection;
    QString m_modelRequestId;
    QString m_modelRequestKeyIdentity;
    QString m_modelRequestHandle;
    QString m_modelRequestAccountIdentity;
    QString m_modelRequestProjectionSha256;
    QString m_modelRequestPlatform;
    quint64 m_presentationJobGeneration = 0;
    bool m_generating = false;
    bool m_applyingSessionSelection = false;
};

#endif // CHAT_DIALOG_H
