#include "balance_orb.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>

namespace {
constexpr int kOrbSize = 72;
const QString kPosKey = QStringLiteral("balanceOrb/pos");

// Qt6 用 globalPosition()，Qt5 用 globalPos()。
inline QPoint eventGlobalPos(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}
}

BalanceOrb::BalanceOrb(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
              | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(kOrbSize, kOrbSize);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Aegisy 余额\n单击还原主窗口，可拖动，右键菜单退出"));
    restorePosition();
}

void BalanceOrb::setBalance(double balance, bool known)
{
    m_balance = balance;
    m_known = known;
    update();
}

QColor BalanceOrb::ringColor() const
{
    if (!m_known) return QColor(0x98, 0xa2, 0xb3);          // 灰：未知
    if (m_balance > kGreenThreshold) return QColor(0x12, 0xb7, 0x6a);  // 绿
    if (m_balance >= kYellowThreshold) return QColor(0xf7, 0x9a, 0x09); // 黄
    return QColor(0xf0, 0x44, 0x38);                         // 红
}

void BalanceOrb::restorePosition()
{
    QSettings settings;
    const QVariant saved = settings.value(kPosKey);
    if (saved.isValid()) {
        const QPoint pos = saved.toPoint();
        // 校验落点仍在某块屏幕内，避免拔掉显示器后小球消失。
        for (QScreen *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().contains(pos)) {
                move(pos);
                return;
            }
        }
    }
    // 默认放到主屏右下角。
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.right() - kOrbSize - 24, area.bottom() - kOrbSize - 24);
    }
}

void BalanceOrb::savePosition()
{
    QSettings settings;
    settings.setValue(kPosKey, pos());
}

void BalanceOrb::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor ring = ringColor();
    const QRectF full(0, 0, width(), height());
    const QRectF inner = full.adjusted(6, 6, -6, -6);

    // 外圈彩色光环
    painter.setPen(Qt::NoPen);
    QColor glow = ring;
    glow.setAlpha(60);
    painter.setBrush(glow);
    painter.drawEllipse(full);

    // 主体深色圆
    painter.setBrush(QColor(0x1f, 0x29, 0x37));
    painter.drawEllipse(inner);

    // 彩色圆环边
    QPen ringPen(ring);
    ringPen.setWidthF(3.0);
    painter.setPen(ringPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(inner.adjusted(1.5, 1.5, -1.5, -1.5));

    // 文本：$金额（未知显示 --）
    QString text = m_known
        ? QStringLiteral("$%1").arg(m_balance, 0, 'f', m_balance >= 100 ? 0 : 2)
        : QStringLiteral("--");
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(m_known && text.size() > 5 ? 15 : 18);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(inner, Qt::AlignCenter, text);
}

void BalanceOrb::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_moved = false;
        m_dragOffset = eventGlobalPos(event) - frameGeometry().topLeft();
        event->accept();
    }
}

void BalanceOrb::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        m_moved = true;
        move(eventGlobalPos(event) - m_dragOffset);
        event->accept();
    }
}

void BalanceOrb::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        if (m_moved) {
            savePosition();
        } else {
            emit restoreRequested();
        }
        event->accept();
    }
}

void BalanceOrb::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit restoreRequested();
        event->accept();
    }
}

void BalanceOrb::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *restoreAction = menu.addAction(QStringLiteral("打开 Aegisy"));
    connect(restoreAction, &QAction::triggered, this, [this]() {
        emit restoreRequested();
    });
    menu.addSeparator();
    QAction *quitAction = menu.addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, this, [this]() {
        emit quitRequested();
    });
    menu.exec(event->globalPos());
}

