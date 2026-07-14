#include "status_badge.h"

#include <QHBoxLayout>
#include <QLabel>

StatusBadge::StatusBadge(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("statusBadge"));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 3, 8, 3);
    layout->setSpacing(5);
    m_iconLabel = new QLabel(this);
    m_iconLabel->setFixedSize(14, 14);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_iconLabel);
    m_textLabel = new QLabel(this);
    m_textLabel->setStyleSheet(QStringLiteral(
        "background: transparent; border: none; font-size: 11px; font-weight: 600;"));
    layout->addWidget(m_textLabel);

    setState(QString(), Tone::Neutral);
}

void StatusBadge::setState(const QString &text, Tone tone, const QIcon &icon)
{
    QString background;
    QString foreground;
    QString border;
    switch (tone) {
    case Tone::Success:
        background = QStringLiteral("#ecfdf3");
        foreground = QStringLiteral("#067647");
        border = QStringLiteral("#abefc6");
        break;
    case Tone::Warning:
        background = QStringLiteral("#fffaeb");
        foreground = QStringLiteral("#b54708");
        border = QStringLiteral("#fedf89");
        break;
    case Tone::Error:
        background = QStringLiteral("#fef3f2");
        foreground = QStringLiteral("#b42318");
        border = QStringLiteral("#fecdca");
        break;
    case Tone::Info:
        background = QStringLiteral("#eff8ff");
        foreground = QStringLiteral("#175cd3");
        border = QStringLiteral("#b2ddff");
        break;
    case Tone::Neutral:
        background = QStringLiteral("#f8fafc");
        foreground = QStringLiteral("#475467");
        border = QStringLiteral("#e4e7ec");
        break;
    }

    setStyleSheet(QStringLiteral(
        "QFrame#statusBadge { background: %1; border: 1px solid %2; border-radius: 6px; }"
        "QFrame#statusBadge QLabel { color: %3; }")
        .arg(background, border, foreground));
    m_textLabel->setText(text);
    if (icon.isNull()) {
        m_iconLabel->hide();
    } else {
        m_iconLabel->setPixmap(icon.pixmap(13, 13));
        m_iconLabel->show();
    }
}

QString StatusBadge::text() const
{
    return m_textLabel->text();
}
