#include "app_theme.h"

#include <QFont>
#include <QStyleFactory>

namespace AppTheme {

// ── 按钮样式 ─────────────────────────────────────────────────────

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #0f766e; color: #ffffff;"
        "  border: 1px solid #0f766e; border-radius: 8px;"
        "  padding: 0 18px; font-size: 13px; font-weight: 600;"
        "  min-height: 34px;"
        "}"
        "QPushButton:hover  { background: #0b625c; border-color: #0b625c; }"
        "QPushButton:pressed{ background: #094f4a; border-color: #094f4a; }"
        "QPushButton:disabled{ background: #d1d9e0; border-color: #d1d9e0; color: #8a96a3; }");
}

QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #ffffff; color: #344054;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 0 14px; font-size: 13px; font-weight: 500;"
        "  min-height: 34px;"
        "}"
        "QPushButton:hover  { background: #f3f8f7; border-color: #7bb8b0; color: #0f5f59; }"
        "QPushButton:pressed{ background: #e7f5f2; border-color: #0f766e; }"
        "QPushButton:disabled{ color: #98a2b3; background: #f8fafb; border-color: #eaecf0; }");
}

QString dangerButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #ffffff; color: #b42318;"
        "  border: 1px solid #fecdca; border-radius: 8px;"
        "  padding: 0 14px; font-size: 13px; font-weight: 500;"
        "  min-height: 34px;"
        "}"
        "QPushButton:hover  { background: #fef3f2; border-color: #f97066; }"
        "QPushButton:pressed{ background: #fee4e2; }"
        "QPushButton:disabled{ background: #f8fafb; border-color: #eaecf0; color: #c7cdd5; }");
}

QString iconButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 7px; }"
        "QPushButton:hover  { background: #eaecf0; }"
        "QPushButton:pressed{ background: #d0d5dd; }"
        "QPushButton:disabled{ opacity: 0.4; }");
}

QString topbarButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #ffffff; color: #344054;"
        "  border: 1px solid #d0d5dd; border-radius: 7px;"
        "  padding: 0 11px; font-size: 12px; font-weight: 500;"
        "  min-height: 30px;"
        "}"
        "QPushButton:hover  { background: #f3f8f7; border-color: #7bb8b0; color: #0f5f59; }"
        "QPushButton:pressed{ background: #e7f5f2; border-color: #0f766e; }"
        "QPushButton::menu-indicator{ image: none; width: 0; }"
        "QPushButton:disabled{ color: #98a2b3; background: #f8fafb; border-color: #eaecf0; }");
}

// ── 状态 badge ────────────────────────────────────────────────────

QString successBadgeStyle()
{
    return QStringLiteral(
        "background: #ecfdf3; color: #067647; border: 1px solid #abefc6;"
        "border-radius: 6px; padding: 3px 9px; font-size: 11px; font-weight: 600;");
}

QString warningBadgeStyle()
{
    return QStringLiteral(
        "background: #fffaeb; color: #b54708; border: 1px solid #fedf89;"
        "border-radius: 6px; padding: 3px 9px; font-size: 11px; font-weight: 600;");
}

QString infoBadgeStyle()
{
    return QStringLiteral(
        "background: #eff8ff; color: #175cd3; border: 1px solid #b2ddff;"
        "border-radius: 6px; padding: 3px 9px; font-size: 11px; font-weight: 600;");
}

QString neutralBadgeStyle()
{
    return QStringLiteral(
        "background: #f8fafc; color: #475467; border: 1px solid #e4e7ec;"
        "border-radius: 6px; padding: 3px 9px; font-size: 11px;");
}

// ── 侧边栏导航 ────────────────────────────────────────────────────

QString sideNavButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #475467; border: none; border-radius: 7px;"
        "  text-align: left; padding: 0 10px;"
        "  font-size: 13px; font-weight: 400; min-height: 36px;"
        "}"
        "QPushButton:hover  { background: #f0f2f5; color: #182230; }"
        "QPushButton:checked{ background: #e7f5f2; color: #0f5f59; font-weight: 600; }"
        "QPushButton:pressed{ background: #d8f0eb; }");
}

// ── 全局样式 ──────────────────────────────────────────────────────

void apply(QApplication &application)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    // 使用系统字体，11pt 提升可读性
    QFont font = application.font();
    font.setPointSize(11);
    application.setFont(font);

    application.setStyleSheet(QStringLiteral(

        // ── 窗口/对话框 ─────────────────────────────────────────
        "QMainWindow, QDialog { background: #f4f7f9; }"
        "QWidget { font-size: 13px; }"

        // ── 标签 ────────────────────────────────────────────────
        "QLabel { color: #17212b; background: transparent; }"

        // ── 输入控件（统一高度 36px、圆角 8px）────────────────────
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {"
        "  background: #ffffff; color: #17212b;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 0 12px; min-height: 36px; font-size: 13px;"
        "  selection-background-color: #bfe5df; selection-color: #103f3b;"
        "}"
        "QLineEdit:hover, QComboBox:hover,  QSpinBox:hover { border-color: #9fb2c3; }"
        "QLineEdit:focus, QComboBox:focus,  QSpinBox:focus {"
        "  border: 1.5px solid #0f766e; background: #fdfeff;"
        "}"
        "QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {"
        "  background: #f1f4f6; color: #98a2b3; border-color: #e4e7ec;"
        "}"

        // TextEdit / PlainTextEdit
        "QTextEdit, QPlainTextEdit {"
        "  background: #ffffff; color: #17212b;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 8px 12px; font-size: 13px;"
        "  selection-background-color: #bfe5df;"
        "}"
        "QTextEdit:hover, QPlainTextEdit:hover { border-color: #9fb2c3; }"
        "QTextEdit:focus, QPlainTextEdit:focus { border: 1.5px solid #0f766e; }"
        "QTextEdit:disabled, QPlainTextEdit:disabled { background: #f1f4f6; color: #98a2b3; }"

        // ── QComboBox 下拉箭头 & 列表 ─────────────────────────────
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding; subcontrol-position: right center;"
        "  width: 28px; border: none;"
        "}"
        "QComboBox::down-arrow {"
        "  width: 10px; height: 10px;"
        "  image: none;"
        "  border-left: 5px solid transparent;"
        "  border-right: 5px solid transparent;"
        "  border-top: 6px solid #667085;"
        "}"
        "QComboBox::down-arrow:hover { border-top-color: #0f766e; }"
        "QComboBox QAbstractItemView {"
        "  background: #ffffff; color: #17212b;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 4px; outline: none;"
        "  selection-background-color: #e7f5f2; selection-color: #0f5f59;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  min-height: 32px; padding: 0 10px; border-radius: 5px;"
        "}"
        "QComboBox QAbstractItemView::item:hover { background: #f0fdf9; }"

        // ── 按钮（默认主色绿）────────────────────────────────────
        "QPushButton {"
        "  background: #0f766e; color: #ffffff;"
        "  border: 1px solid #0f766e; border-radius: 8px;"
        "  padding: 0 16px; min-height: 34px;"
        "  font-size: 13px; font-weight: 500;"
        "}"
        "QPushButton:hover  { background: #0b625c; border-color: #0b625c; }"
        "QPushButton:pressed{ background: #094f4a; border-color: #094f4a; }"
        "QPushButton:disabled{ background: #d1d9e0; border-color: #d1d9e0; color: #8a96a3; }"
        "QPushButton:flat   { background: transparent; border: none; }"

        // ── ToolButton ───────────────────────────────────────────
        "QToolButton { border-radius: 7px; padding: 5px; font-size: 12px; }"
        "QToolButton:hover { background: #e7f5f2; }"
        "QToolButton:pressed{ background: #d2eee9; }"

        // ── CheckBox ─────────────────────────────────────────────
        "QCheckBox { color: #344054; spacing: 8px; font-size: 13px; }"
        "QCheckBox::indicator {"
        "  width: 16px; height: 16px;"
        "  border: 1.5px solid #b7c1cd; border-radius: 4px; background: #ffffff;"
        "}"
        "QCheckBox::indicator:hover  { border-color: #0f766e; }"
        "QCheckBox::indicator:checked{ background: #0f766e; border-color: #0f766e; }"
        "QCheckBox::indicator:disabled{ background: #f1f4f6; border-color: #dde2e8; }"

        // ── RadioButton ──────────────────────────────────────────
        "QRadioButton { color: #344054; spacing: 8px; font-size: 13px; }"
        "QRadioButton::indicator {"
        "  width: 16px; height: 16px;"
        "  border: 1.5px solid #b7c1cd; border-radius: 8px; background: #ffffff;"
        "}"
        "QRadioButton::indicator:hover   { border-color: #0f766e; }"
        "QRadioButton::indicator:checked {"
        "  background: #0f766e; border-color: #0f766e;"
        "  image: url(none);"
        "}"

        // ── QTabWidget ───────────────────────────────────────────
        "QTabWidget::pane {"
        "  border: 1px solid #e4e7ec; border-radius: 0 0 8px 8px;"
        "  background: #ffffff; margin-top: -1px;"
        "}"
        "QTabBar {"
        "  background: transparent;"
        "}"
        "QTabBar::tab {"
        "  background: transparent; color: #667085;"
        "  border: none; border-bottom: 2px solid transparent;"
        "  padding: 9px 18px; font-size: 13px; font-weight: 500;"
        "  margin-bottom: -1px; margin-right: 2px;"
        "}"
        "QTabBar::tab:selected  { color: #0f766e; border-bottom-color: #0f766e; font-weight: 600; }"
        "QTabBar::tab:hover:!selected { color: #344054; background: #f3f4f6; border-radius: 6px 6px 0 0; }"

        // ── QTableWidget ─────────────────────────────────────────
        "QTableWidget {"
        "  background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px;"
        "  gridline-color: #f2f4f7; font-size: 13px; outline: none;"
        "}"
        "QTableWidget::item { padding: 0 10px; color: #344054; min-height: 40px; }"
        "QTableWidget::item:selected { background: #e7f5f2; color: #0f5f59; }"
        "QTableWidget::item:hover:!selected { background: #f8fafb; }"
        "QHeaderView { background: transparent; }"
        "QHeaderView::section {"
        "  background: #f7f9fb; color: #667085;"
        "  border: none; border-bottom: 1px solid #e4e7ec;"
        "  padding: 0 10px; min-height: 38px;"
        "  font-weight: 600; font-size: 12px;"
        "}"
        "QHeaderView::section:first { border-top-left-radius: 8px; }"
        "QHeaderView::section:last  { border-top-right-radius: 8px; }"
        "QHeaderView::section:hover { background: #eef2f6; color: #344054; }"

        // ── QListWidget ──────────────────────────────────────────
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { color: #475467; padding: 7px 8px; border-radius: 6px; font-size: 13px; }"
        "QListWidget::item:selected { background: #e7f5f2; color: #0f5f59; }"
        "QListWidget::item:hover:!selected { background: #eef2f6; }"

        // ── QScrollBar（细滑轨）──────────────────────────────────
        "QScrollBar:vertical {"
        "  background: transparent; width: 8px; margin: 2px 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #c7d1dc; border-radius: 4px; min-height: 28px;"
        "}"
        "QScrollBar::handle:vertical:hover { background: #9eacba; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal {"
        "  background: transparent; height: 8px; margin: 0 2px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "  background: #c7d1dc; border-radius: 4px; min-width: 28px;"
        "}"
        "QScrollBar::handle:horizontal:hover { background: #9eacba; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"

        // ── QGroupBox ────────────────────────────────────────────
        "QGroupBox {"
        "  background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px;"
        "  margin-top: 12px; padding: 12px 14px 10px 14px; font-size: 12px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin; subcontrol-position: top left;"
        "  padding: 0 6px; color: #475467; font-weight: 600;"
        "  left: 10px;"
        "}"

        // ── QMenu ────────────────────────────────────────────────
        "QMenu {"
        "  background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px;"
        "  padding: 4px 0; font-size: 13px;"
        "}"
        "QMenu::item { padding: 7px 16px; color: #344054; }"
        "QMenu::item:selected { background: #f0fdf9; color: #0f5f59; }"
        "QMenu::item:disabled { color: #98a2b3; }"
        "QMenu::separator { height: 1px; background: #f0f2f5; margin: 4px 8px; }"

        // ── ToolTip ──────────────────────────────────────────────
        "QToolTip {"
        "  background: #1d2939; color: #f2f4f7; border: none;"
        "  padding: 6px 10px; border-radius: 6px; font-size: 12px;"
        "}"

        // ── QMessageBox ──────────────────────────────────────────
        "QMessageBox { background: #f4f7f9; }"
        "QMessageBox QLabel { font-size: 13px; color: #344054; line-height: 1.5; }"
        "QMessageBox QPushButton { min-width: 72px; }"

        // ── QSplitter ────────────────────────────────────────────
        "QSplitter::handle { background: #e4e7ec; }"
        "QSplitter::handle:horizontal { width: 1px; }"
        "QSplitter::handle:vertical   { height: 1px; }"

        // ── QProgressBar ─────────────────────────────────────────
        "QProgressBar {"
        "  background: #e4e7ec; border-radius: 4px; height: 6px; border: none; text-align: center;"
        "}"
        "QProgressBar::chunk { background: #0f766e; border-radius: 4px; }"
    ));
}

} // namespace AppTheme
