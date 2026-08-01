#ifndef TIMELINE_API_H
#define TIMELINE_API_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>

class TimelineAPI : public QObject
{
    Q_OBJECT

public:
    explicit TimelineAPI(QObject *parent = nullptr);

public slots:
    QJsonArray getTimelineItems(int offset = 0, int limit = 50);
    int getItemCount();
    void sendMessage(const QString &message);
    void updateItemState(const QString &itemId, const QString &state);
    void approveCommand(const QString &approvalId);
    void denyCommand(const QString &approvalId, const QString &reason = QString());
    void answerQuestion(const QString &questionId, const QString &answer);
    void cancelQuestion(const QString &questionId);
    void addAttachment(const QString &path, const QString &type);
    void removeAttachment(int index);
    QJsonArray getAttachments();
    void cancelTurn(const QString &turnId);
    void retryTurn(const QString &turnId);
    QString getCurrentTurnId();
    void updatePlanStep(const QString &planId, int stepIndex, const QString &status);
    void executeCommand(const QString &command, const QString &cwd = QString());
    void setModel(const QString &model);
    void setPermission(const QString &permission);
    QString getModel();
    QString getPermission();

signals:
    void itemAppended(const QJsonObject &item);
    void itemUpdated(const QString &itemId, const QJsonObject &delta);
    void attachmentsChanged(const QJsonArray &attachments);
    void contextChanged(const QJsonObject &context);

private:
    QJsonArray m_items;
    QJsonArray m_attachments;
    QString m_currentTurnId;
    QString m_model;
    QString m_permission;
    QString findItemId(const QString &id);
};

#endif // TIMELINE_API_H
