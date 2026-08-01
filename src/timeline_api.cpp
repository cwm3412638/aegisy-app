#include "timeline_api.h"
#include <QDateTime>
#include <QUuid>

TimelineAPI::TimelineAPI(QObject *parent)
    : QObject(parent)
{
}

QJsonArray TimelineAPI::getTimelineItems()
{
    return m_items;
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
    agentItem["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    agentItem["type"] = "agent";
    agentItem["content"] = "Processing your request...";
    agentItem["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    agentItem["state"] = "streaming";

    m_items.append(agentItem);
    emit itemAppended(agentItem);
}
