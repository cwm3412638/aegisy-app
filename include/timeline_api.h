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
    QJsonArray getTimelineItems();
    void sendMessage(const QString &message);

signals:
    void itemAppended(const QJsonObject &item);
    void itemUpdated(const QString &itemId, const QJsonObject &delta);

private:
    QJsonArray m_items;
};

#endif // TIMELINE_API_H
