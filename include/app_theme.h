#ifndef APP_THEME_H
#define APP_THEME_H

#include <QApplication>
#include <QString>

namespace AppTheme {

namespace Tokens {
inline constexpr char Accent[] = "#165DFF";
inline constexpr char AccentStrong[] = "#0F46C6";
inline constexpr char Canvas[] = "#F5F7FB";
inline constexpr char Surface[] = "#ffffff";
inline constexpr char Shell[] = "#101828";
inline constexpr char ShellRaised[] = "#1D2939";
inline constexpr char Border[] = "#E2E8F0";
inline constexpr char Text[] = "#182230";
inline constexpr char TextMuted[] = "#667085";
inline constexpr char Focus[] = "#84A8FF";
inline constexpr char Warning[] = "#f59e0b";
inline constexpr char Critical[] = "#ef4444";
}

// 应用全局样式。保留系统原生控件风格与字体，仅提供语义化外观层。
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
