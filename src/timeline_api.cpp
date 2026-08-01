#include "timeline_api.h"
#include <QDateTime>
#include <QUuid>
#include <QTimer>

TimelineAPI::TimelineAPI(QObject *parent)
    : QObject(parent)
{
}

QJsonArray TimelineAPI::getTimelineItems(int offset, int limit)
{
    QJsonArray result;
    int start = qMax(0, offset);
    int end = qMin(m_items.size(), start + limit);
    for (int i = start; i < end; ++i) {
        result.append(m_items[i]);
    }
    return result;
}

int TimelineAPI::getItemCount()
{
    return m_items.size();
}

void TimelineAPI::sendMessage(const QString &message)
{
    QJsonObject userItem;
    userItem["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    userItem["type"] = "user";
    userItem["content"] = message;
    userItem["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    userItem["state"] = "complete";

    m_items.append(userItem);
    emit itemAppended(userItem);

    QJsonObject agentItem;
    QString agentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    agentItem["id"] = agentId;
    agentItem["type"] = "agent";
    agentItem["content"] = "Processing your request...";
    agentItem["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    agentItem["state"] = "streaming";

    m_items.append(agentItem);
    emit itemAppended(agentItem);

    QTimer::singleShot(2000, this, [this, agentId]() {
        QJsonObject delta;
        delta["state"] = "complete";
        delta["content"] = "I've analyzed your request. Here's what I found...";
        emit itemUpdated(agentId, delta);

        for (int i = 0; i < m_items.size(); ++i) {
            QJsonObject item = m_items[i].toObject();
            if (item["id"].toString() == agentId) {
                item["state"] = "complete";
                item["content"] = delta["content"].toString();
                m_items[i] = item;
                break;
            }
        }
    });
}

void TimelineAPI::updateItemState(const QString &itemId, const QString &state)
{
    for (int i = 0; i < m_items.size(); ++i) {
        QJsonObject item = m_items[i].toObject();
        if (item["id"].toString() == itemId) {
            item["state"] = state;
            m_items[i] = item;
            QJsonObject delta;
            delta["state"] = state;
            emit itemUpdated(itemId, delta);
            break;
        }
    }
}

QString TimelineAPI::findItemId(const QString &id)
{
    for (const QJsonValue &val : m_items) {
        QJsonObject item = val.toObject();
        if (item["id"].toString() == id) {
            return id;
        }
    }
    return QString();
}
