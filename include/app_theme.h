#ifndef APP_THEME_H
#define APP_THEME_H

#include <QApplication>
#include <QString>

namespace AppTheme {

// 应用全局样式（Fusion 风格 + 全局 stylesheet）
void apply(QApplication &application);

// ── 按钮样式 ──────────────────────────────────────────────────
// 主要操作（绿色实心）
QString primaryButtonStyle();
// 次要操作（白色边框）
QString secondaryButtonStyle();
// 危险操作（红色文字、白底）
QString dangerButtonStyle();
// 纯图标小按钮（透明背景，hover 变浅灰）
QString iconButtonStyle();
// 顶栏功能按钮（比 secondary 更紧凑，无固定高度约束）
QString topbarButtonStyle();

// ── 标签 / 状态 badge ─────────────────────────────────────────
// 成功 badge（绿）
QString successBadgeStyle();
// 警告 badge（橙）
QString warningBadgeStyle();
// 信息 badge（蓝）
QString infoBadgeStyle();
// 中性 badge（灰）
QString neutralBadgeStyle();

// ── 侧边栏导航按钮 ─────────────────────────────────────────────
// 带高亮选中态的左对齐按钮
QString sideNavButtonStyle();

} // namespace AppTheme

#endif // APP_THEME_H
