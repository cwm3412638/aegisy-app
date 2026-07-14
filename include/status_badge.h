#ifndef STATUS_BADGE_H
#define STATUS_BADGE_H

#include <QFrame>
#include <QIcon>

class QLabel;

class StatusBadge : public QFrame
{
public:
    enum class Tone { Neutral, Success, Warning, Error, Info };

    explicit StatusBadge(QWidget *parent = nullptr);

    void setState(const QString &text, Tone tone, const QIcon &icon = QIcon());
    QString text() const;

private:
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
};

#endif // STATUS_BADGE_H
