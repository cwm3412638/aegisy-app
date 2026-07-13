#ifndef CHAT_DIALOG_H
#define CHAT_DIALOG_H

#include <QDialog>
#include <QJsonArray>
#include <QList>

class ApiClient;
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
    explicit ChatDialog(ApiClient *apiClient, QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void reject() override;

private slots:
    void onApiKeysReceived(const QJsonArray &keys);
    void onModelsReceived(const QJsonArray &models);
    void onKeyChanged(int index);
    void onSendClicked();
    void onStopClicked();
    void onNewSession();
    void onSessionChanged(int row);
    void onDeleteSession();
    void onChatChunk(const QString &requestId, const QString &chunk);
    void onChatCompleted(const QString &requestId, const QString &content);
    void onChatFailed(const QString &requestId, const QString &error);
    void onRequestFailed(const QString &error);

private:
    struct ChatSession {
        QString id;
        QString title;
        QJsonArray messages;
    };

    void setupUi();
    void setGenerating(bool generating);
    void rebuildMessages();
    void addMessageWidget(const QString &role, const QString &content,
                          QTextBrowser **contentBrowser = nullptr);
    void scrollToBottom();
    void updateSessionTitle(const QString &firstMessage);
    QString selectedApiKey() const;

    ApiClient *m_apiClient = nullptr;
    QListWidget *m_sessionList = nullptr;
    QComboBox *m_keyCombo = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QLabel *m_statusLabel = nullptr;
    QScrollArea *m_scrollArea = nullptr;
    QWidget *m_messagesContainer = nullptr;
    QVBoxLayout *m_messagesLayout = nullptr;
    QPlainTextEdit *m_inputEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QTextBrowser *m_streamBrowser = nullptr;

    QList<ChatSession> m_sessions;
    int m_currentSession = -1;
    QString m_requestId;
    QString m_streamContent;
    bool m_generating = false;
};

#endif // CHAT_DIALOG_H
