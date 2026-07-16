#include "runtime_status_bar.h"

#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>

namespace {

constexpr int kBarWidth = 660;
constexpr int kBarHeight = 62;
const QString kPositionKey = QStringLiteral("runtimeStatusBar/pos");
const QString kLegacyPositionKey = QStringLiteral("balanceOrb/pos");

QPoint globalPosition(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QString compactTokens(qint64 tokens)
{
    if (tokens < 0) return QStringLiteral("--");
    if (tokens >= 1000000) {
        return QStringLiteral("%1M").arg(tokens / 1000000.0, 0, 'f', 1);
    }
    if (tokens >= 1000) {
        return QStringLiteral("%1K").arg(tokens / 1000.0, 0, 'f', 1);
    }
    return QString::number(tokens);
}

QString compactBalance(double balance)
{
    if (balance >= 1000000.0) {
        return QStringLiteral("$%1M").arg(balance / 1000000.0, 0, 'f', 1);
    }
    if (balance >= 1000.0) {
        return QStringLiteral("$%1K").arg(balance / 1000.0, 0, 'f', 1);
    }
    return QStringLiteral("$%1").arg(balance, 0, 'f', 2);
}

QPoint positionInsideScreen(const QPoint &position, const QSize &size,
                            const QRect &available)
{
    const int maxX = qMax(available.left(), available.right() - size.width() + 1);
    const int maxY = qMax(available.top(), available.bottom() - size.height() + 1);
    return QPoint(qBound(available.left(), position.x(), maxX),
                  qBound(available.top(), position.y(), maxY));
}

} // namespace

RuntimeStatusBar::RuntimeStatusBar(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
              | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowTitle(QStringLiteral("Aegisy Runtime Status"));
    setObjectName(QStringLiteral("runtimeStatusBar"));
    setFixedSize(kBarWidth, kBarHeight);
    setCursor(Qt::PointingHandCursor);
    restorePosition();
    setSnapshot(RuntimeStatusSnapshot{});
}

void RuntimeStatusBar::setSnapshot(const RuntimeStatusSnapshot &snapshot)
{
    m_snapshot = snapshot;
    const QString description = accessibleStatus();
    setAccessibleName(description);
    setToolTip(description + QStringLiteral("\n单击打开 Aegisy，可拖动，右键退出"));
    update();
}

QString RuntimeStatusBar::contextText() const
{
    if (m_snapshot.usageKnown() && m_snapshot.contextLimitKnown()) {
        const double percent = qBound(0.0,
            100.0 * m_snapshot.inputTokens / m_snapshot.contextLimit, 999.0);
        return QStringLiteral("上下文 %1 / %2  %3%")
            .arg(compactTokens(m_snapshot.inputTokens),
                 compactTokens(m_snapshot.contextLimit),
                 QString::number(percent, 'f', 0));
    }
    if (m_snapshot.usageKnown()) {
        return QStringLiteral("上下文 %1 / --")
            .arg(compactTokens(m_snapshot.inputTokens));
    }
    if (m_snapshot.active) return QStringLiteral("上下文 请求中");
    if (m_snapshot.monitored && m_snapshot.requestObserved) {
        return QStringLiteral("上下文 未返回用量");
    }
    if (m_snapshot.monitored) return QStringLiteral("上下文 等待请求");
    if (m_snapshot.provenance == RuntimeStatusProvenance::Configured) {
        return QStringLiteral("上下文 未监控");
    }
    return QStringLiteral("上下文 --");
}

QString RuntimeStatusBar::accessibleStatus() const
{
    const QString tool = m_snapshot.toolName.isEmpty()
        ? QStringLiteral("等待运行状态") : m_snapshot.toolName;
    const QString model = m_snapshot.model.isEmpty()
        ? QStringLiteral("模型未知") : QStringLiteral("模型 %1").arg(m_snapshot.model);
    const QString reasoning = m_snapshot.reasoning.isEmpty()
        ? QStringLiteral("思考深度未知")
        : QStringLiteral("思考深度 %1").arg(m_snapshot.reasoning);
    const QString balance = m_snapshot.balanceKnown
        ? QStringLiteral("余额 %1 美元").arg(m_snapshot.balance, 0, 'f', 2)
        : QStringLiteral("余额未知");
    return QStringLiteral("%1，%2，%3，%4，%5")
        .arg(tool, model, reasoning, contextText(), balance);
}

void RuntimeStatusBar::restorePosition()
{
    QSettings settings;
    QVariant saved = settings.value(kPositionKey);
    if (!saved.isValid()) saved = settings.value(kLegacyPositionKey);
    if (saved.isValid()) {
        const QPoint position = saved.toPoint();
        const QRect candidate(position, size());
        for (QScreen *screen : QGuiApplication::screens()) {
            if (screen->availableGeometry().intersects(candidate)) {
                move(positionInsideScreen(position, size(), screen->availableGeometry()));
                return;
            }
        }
    }
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect area = screen->availableGeometry();
        move(area.right() - width() - 24, area.bottom() - height() - 24);
    }
}

void RuntimeStatusBar::savePosition()
{
    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) move(positionInsideScreen(pos(), size(), screen->availableGeometry()));
    QSettings settings;
    settings.setValue(kPositionKey, pos());
}

void RuntimeStatusBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF surface = QRectF(rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(QColor(QStringLiteral("#33404d")), 1));
    painter.setBrush(QColor(QStringLiteral("#10161d")));
    painter.drawRoundedRect(surface, 8, 8);

    const QColor activeColor = m_snapshot.active
        ? QColor(QStringLiteral("#2dd4bf"))
        : (m_snapshot.monitored ? QColor(QStringLiteral("#5eead4"))
                                : QColor(QStringLiteral("#718096")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(activeColor);
    painter.drawRoundedRect(QRectF(1, 12, 3, 38), 1.5, 1.5);
    painter.drawEllipse(QRectF(17, 24, 10, 10));

    QFont labelFont = painter.font();
    labelFont.setPixelSize(10);
    labelFont.setBold(true);
    QFont valueFont = painter.font();
    valueFont.setPixelSize(13);
    valueFont.setBold(true);

    painter.setFont(labelFont);
    painter.setPen(QColor(QStringLiteral("#82909f")));
    painter.drawText(QRect(36, 10, 185, 16), Qt::AlignLeft | Qt::AlignVCenter,
                     m_snapshot.active ? QStringLiteral("正在请求")
                                       : QStringLiteral("当前连接"));
    painter.setFont(valueFont);
    painter.setPen(QColor(QStringLiteral("#f4f7fa")));
    const QString toolModel = m_snapshot.toolName.isEmpty()
        ? QStringLiteral("等待运行状态")
        : (m_snapshot.model.isEmpty() ? m_snapshot.toolName
           : QStringLiteral("%1 · %2").arg(m_snapshot.toolName, m_snapshot.model));
    painter.drawText(QRect(36, 27, 185, 24),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(toolModel, Qt::ElideRight, 185));

    painter.setPen(QColor(QStringLiteral("#2a3541")));
    painter.drawLine(232, 13, 232, 49);
    painter.drawLine(353, 13, 353, 49);
    painter.drawLine(548, 13, 548, 49);

    painter.setFont(labelFont);
    painter.setPen(QColor(QStringLiteral("#82909f")));
    painter.drawText(QRect(248, 10, 90, 16), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("思考深度"));
    painter.setFont(valueFont);
    painter.setPen(QColor(QStringLiteral("#dbe5ed")));
    const QString reasoning = m_snapshot.reasoning.isEmpty()
        ? QStringLiteral("--") : m_snapshot.reasoning;
    painter.drawText(QRect(248, 27, 90, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(reasoning, Qt::ElideRight, 90));

    const QString context = contextText();
    painter.setFont(labelFont);
    painter.setPen(QColor(QStringLiteral("#82909f")));
    painter.drawText(QRect(369, 9, 164, 18), Qt::AlignLeft | Qt::AlignVCenter, context);
    const QRectF track(369, 35, 164, 6);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#27313b")));
    painter.drawRoundedRect(track, 3, 3);
    if (m_snapshot.usageKnown() && m_snapshot.contextLimitKnown()) {
        const double ratio = qBound(0.0,
            static_cast<double>(m_snapshot.inputTokens) / m_snapshot.contextLimit, 1.0);
        QColor usageColor(QStringLiteral("#2dd4bf"));
        if (ratio >= 0.9) usageColor = QColor(QStringLiteral("#ef4444"));
        else if (ratio >= 0.75) usageColor = QColor(QStringLiteral("#f59e0b"));
        painter.setBrush(usageColor);
        painter.drawRoundedRect(QRectF(track.x(), track.y(), track.width() * ratio,
                                       track.height()), 3, 3);
    }

    painter.setFont(labelFont);
    painter.setPen(QColor(QStringLiteral("#82909f")));
    painter.drawText(QRect(564, 10, 78, 16), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("账户余额"));
    painter.setFont(valueFont);
    painter.setPen(m_snapshot.balanceKnown && m_snapshot.balance < 1.0
        ? QColor(QStringLiteral("#f87171")) : QColor(QStringLiteral("#f4f7fa")));
    const QString balance = m_snapshot.balanceKnown
        ? compactBalance(m_snapshot.balance)
        : QStringLiteral("--");
    painter.drawText(QRect(564, 27, 78, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     painter.fontMetrics().elidedText(balance, Qt::ElideRight, 78));
}

void RuntimeStatusBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    m_dragging = true;
    m_moved = false;
    m_dragOffset = globalPosition(event) - frameGeometry().topLeft();
    event->accept();
}

void RuntimeStatusBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) return;
    m_moved = true;
    move(globalPosition(event) - m_dragOffset);
    event->accept();
}

void RuntimeStatusBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging) return;
    m_dragging = false;
    if (m_moved) savePosition();
    else emit restoreRequested();
    event->accept();
}

void RuntimeStatusBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit restoreRequested();
        event->accept();
    }
}

void RuntimeStatusBar::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *restoreAction = menu.addAction(QStringLiteral("打开 Aegisy"));
    connect(restoreAction, &QAction::triggered, this, &RuntimeStatusBar::restoreRequested);
    menu.addSeparator();
    QAction *quitAction = menu.addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, this, &RuntimeStatusBar::quitRequested);
    menu.exec(event->globalPos());
}
