#ifndef RUNTIME_STATUS_BAR_H
#define RUNTIME_STATUS_BAR_H

#include <QPoint>
#include <QWidget>

#include "runtime_status_store.h"

// 主窗口隐藏或最小化时显示的紧凑运行状态条。所有运行数据都来自
// RuntimeStatusStore；未知数据会明确显示为未监控或 --。
class RuntimeStatusBar : public QWidget
{
    Q_OBJECT

public:
    explicit RuntimeStatusBar(QWidget *parent = nullptr);

    void setSnapshot(const RuntimeStatusSnapshot &snapshot);

signals:
    void restoreRequested();
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
    QString contextText() const;
    QString accessibleStatus() const;

    RuntimeStatusSnapshot m_snapshot;
    QPoint m_dragOffset;
    bool m_dragging = false;
    bool m_moved = false;
};

#endif // RUNTIME_STATUS_BAR_H
