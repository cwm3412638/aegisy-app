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

        QJsonObject approvalItem;
        approvalItem["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        approvalItem["type"] = "approval";
        approvalItem["content"] = "Approval required for command execution";
        approvalItem["command"] = "rm -rf dist/";
        approvalItem["scope"] = "Delete directory recursively";
        approvalItem["risk"] = "High";
        approvalItem["riskReason"] = "Irreversible deletion";
        approvalItem["reason"] = "Clean build artifacts";
        approvalItem["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        approvalItem["state"] = "pending";
        m_items.append(approvalItem);
        emit itemAppended(approvalItem);
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

void TimelineAPI::approveCommand(const QString &approvalId)
{
    for (int i = 0; i < m_items.size(); ++i) {
        QJsonObject item = m_items[i].toObject();
        if (item["id"].toString() == approvalId && item["type"].toString() == "approval") {
            item["state"] = "approved";
            item["decision"] = "approved";
            m_items[i] = item;

            QJsonObject delta;
            delta["state"] = "approved";
            delta["decision"] = "approved";
            emit itemUpdated(approvalId, delta);

            QJsonObject commandItem;
            commandItem["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
            commandItem["type"] = "command";
            commandItem["content"] = item["command"].toString();
            commandItem["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
            commandItem["state"] = "running";
            m_items.append(commandItem);
            emit itemAppended(commandItem);
            break;
        }
    }
}

void TimelineAPI::denyCommand(const QString &approvalId, const QString &reason)
{
    for (int i = 0; i < m_items.size(); ++i) {
        QJsonObject item = m_items[i].toObject();
        if (item["id"].toString() == approvalId && item["type"].toString() == "approval") {
            item["state"] = "denied";
            item["decision"] = "denied";
            if (!reason.isEmpty()) {
                item["denyReason"] = reason;
            }
            m_items[i] = item;

            QJsonObject delta;
            delta["state"] = "denied";
            delta["decision"] = "denied";
            emit itemUpdated(approvalId, delta);
            break;
        }
    }
}

void TimelineAPI::answerQuestion(const QString &questionId, const QString &answer)
{
    for (int i = 0; i < m_items.size(); ++i) {
        QJsonObject item = m_items[i].toObject();
        if (item["id"].toString() == questionId && item["type"].toString() == "question") {
            item["state"] = "answered";
            item["answer"] = answer;
            m_items[i] = item;

            QJsonObject delta;
            delta["state"] = "answered";
            delta["answer"] = answer;
            emit itemUpdated(questionId, delta);
            break;
        }
    }
}

void TimelineAPI::cancelQuestion(const QString &questionId)
{
    for (int i = 0; i < m_items.size(); ++i) {
        QJsonObject item = m_items[i].toObject();
        if (item["id"].toString() == questionId && item["type"].toString() == "question") {
            item["state"] = "cancelled";
            m_items[i] = item;

            QJsonObject delta;
            delta["state"] = "cancelled";
            emit itemUpdated(questionId, delta);
            break;
        }
    }
}
