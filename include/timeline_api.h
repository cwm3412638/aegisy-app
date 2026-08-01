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

signals:
    void itemAppended(const QJsonObject &item);
    void itemUpdated(const QString &itemId, const QJsonObject &delta);

private:
    QJsonArray m_items;
    QString findItemId(const QString &id);
};

#endif // TIMELINE_API_H
