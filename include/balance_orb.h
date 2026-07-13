#ifndef BALANCE_ORB_H
#define BALANCE_ORB_H

#include <QWidget>
#include <QPoint>

// 主窗口最小化/隐藏到托盘时显示的桌面悬浮小球：
// 实时展示账号余额，并按阈值切换绿/黄/红三色预警。
// 无边框、置顶、可拖动，位置持久化到 QSettings。
class BalanceOrb : public QWidget
{
    Q_OBJECT

public:
    explicit BalanceOrb(QWidget *parent = nullptr);

    // 更新显示的余额（美元）。传负值表示未知。
    void setBalance(double balance, bool known);

    // 三色阈值（美元），可按需调整。
    static constexpr double kGreenThreshold = 5.0;   // > 5 绿
    static constexpr double kYellowThreshold = 1.0;  // 1~5 黄，< 1 红

signals:
    // 单击/双击小球请求还原主窗口。
    void restoreRequested();
    // 右键菜单请求退出应用。
    void quitRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void restorePosition();
    void savePosition();
    QColor ringColor() const;

    double m_balance = 0.0;
    bool m_known = false;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_moved = false;
};

#endif // BALANCE_ORB_H
