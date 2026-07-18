#include "app_theme.h"

#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace AppTheme {

// ── 按钮样式 ─────────────────────────────────────────────────────

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: %1; color: #ffffff;"
        "  border: 1px solid %1; border-radius: 8px;"
        "  padding: 0 18px; font-size: 13px; font-weight: 600;"
        "  min-height: 34px;"
        "}"
        "QPushButton:hover  { background: #0F4FD8; border-color: #0F4FD8; }"
        "QPushButton:pressed{ background: #0D3FB3; border-color: #0D3FB3; }"
        "QPushButton:disabled{ background: #d1d9e0; border-color: #d1d9e0; color: #8a96a3; }")
        .arg(QString::fromLatin1(Tokens::Accent));
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
        "QPushButton:hover  { background: #F5F8FF; border-color: #98B3F6; color: #174EA6; }"
        "QPushButton:pressed{ background: #E8EFFF; border-color: #165DFF; }"
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
        "QPushButton:hover  { background: #F5F8FF; border-color: #98B3F6; color: #174EA6; }"
        "QPushButton:pressed{ background: #E8EFFF; border-color: #165DFF; }"
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
        "  background: transparent; color: #a9b5c1; border: none; border-radius: 7px;"
        "  text-align: left; padding: 0 10px;"
        "  font-size: 13px; font-weight: 400; min-height: 36px;"
        "}"
        "QPushButton:hover  { background: %1; color: #f4f7fa; }"
        "QPushButton:checked{ background: #1F3A5F; color: #D6E4FF; font-weight: 600; }"
        "QPushButton:pressed{ background: #263640; }")
        .arg(QString::fromLatin1(Tokens::ShellRaised));
}

// ── 全局样式 ──────────────────────────────────────────────────────

void apply(QApplication &application)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }
    const QStringList preferredFonts = {
#ifdef Q_OS_WIN
        QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Segoe UI"),
#elif defined(Q_OS_MACOS)
        QStringLiteral("PingFang SC"), QStringLiteral("SF Pro Text"),
#else
        QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans"),
#endif
        application.font().family()
    };
    const QStringList availableFonts = QFontDatabase::families();
    for (const QString &family : preferredFonts) {
        if (!availableFonts.contains(family)) continue;
        QFont font(family);
        font.setPointSize(10);
        font.setStyleStrategy(QFont::PreferAntialias);
        application.setFont(font);
        break;
    }
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#F5F7FB")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#182230")));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#F8FAFC")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#182230")));
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#344054")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#DCE7FF")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#163B7A")));
    palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#98A2B3")));
    application.setPalette(palette);
    application.setStyleSheet(QStringLiteral(

        // ── 窗口/对话框 ─────────────────────────────────────────
        "QMainWindow, QDialog { background: #F5F7FB; }"
        // ── 标签 ────────────────────────────────────────────────
        "QLabel { color: #182230; background: transparent; }"

        // ── 输入控件（统一高度 36px、圆角 8px）────────────────────
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {"
        "  background: #ffffff; color: #182230;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 0 12px; min-height: 36px; font-size: 13px;"
        "  selection-background-color: #C8D8FF; selection-color: #163B7A;"
        "}"
        "QLineEdit:hover, QComboBox:hover,  QSpinBox:hover { border-color: #9fb2c3; }"
        "QLineEdit:focus, QComboBox:focus,  QSpinBox:focus {"
        "  border: 1.5px solid #165DFF; background: #fdfeff;"
        "}"
        "QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled {"
        "  background: #f1f4f6; color: #98a2b3; border-color: #e4e7ec;"
        "}"

        // TextEdit / PlainTextEdit
        "QTextEdit, QPlainTextEdit {"
        "  background: #ffffff; color: #182230;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 8px 12px; font-size: 13px;"
        "  selection-background-color: #C8D8FF;"
        "}"
        "QTextEdit:hover, QPlainTextEdit:hover { border-color: #9fb2c3; }"
        "QTextEdit:focus, QPlainTextEdit:focus { border: 1.5px solid #165DFF; }"
        "QTextEdit:disabled, QPlainTextEdit:disabled { background: #f1f4f6; color: #98a2b3; }"

        // ── QComboBox 下拉箭头 & 列表 ─────────────────────────────
        "QComboBox::drop-down {"
        "  subcontrol-origin: padding; subcontrol-position: right center;"
        "  width: 28px; border: none;"
        "}"
        "QComboBox::down-arrow {"
        "  image: url(:/icons/lucide/chevron-down.svg);"
        "  width: 14px; height: 14px; border: none;"
        "}"
        "QComboBox QAbstractItemView {"
        "  background: #ffffff; color: #182230;"
        "  border: 1px solid #d0d5dd; border-radius: 8px;"
        "  padding: 4px; outline: none;"
        "  selection-background-color: #E8EFFF; selection-color: #174EA6;"
        "}"
        "QComboBox QAbstractItemView::item {"
        "  min-height: 32px; padding: 0 10px; border-radius: 5px;"
        "}"
        "QComboBox QAbstractItemView::item:hover { background: #F5F8FF; }"

        // ── 按钮（默认主色绿）────────────────────────────────────
        "QPushButton {"
        "  background: #165DFF; color: #ffffff;"
        "  border: 1px solid #165DFF; border-radius: 8px;"
        "  padding: 0 16px; min-height: 34px;"
        "  font-size: 13px; font-weight: 500;"
        "}"
        "QPushButton:hover  { background: #0F4FD8; border-color: #0F4FD8; }"
        "QPushButton:pressed{ background: #0D3FB3; border-color: #0D3FB3; }"
        "QPushButton:disabled{ background: #d1d9e0; border-color: #d1d9e0; color: #8a96a3; }"
        "QPushButton:flat   { background: transparent; border: none; }"

        // ── ToolButton ───────────────────────────────────────────
        "QToolButton { border-radius: 7px; padding: 5px; font-size: 12px; }"
        "QToolButton:hover { background: #E8EFFF; }"
        "QToolButton:pressed{ background: #DCE7FF; }"

        // ── CheckBox ─────────────────────────────────────────────
        "QCheckBox { color: #344054; spacing: 8px; font-size: 13px; }"
        "QCheckBox::indicator {"
        "  width: 16px; height: 16px;"
        "  border: 1.5px solid #b7c1cd; border-radius: 4px; background: #ffffff;"
        "}"
        "QCheckBox::indicator:hover  { border-color: #165DFF; }"
        "QCheckBox::indicator:checked{ background: #165DFF; border-color: #165DFF;"
        "  image: url(:/icons/lucide/check.svg); }"
        "QCheckBox::indicator:disabled{ background: #f1f4f6; border-color: #dde2e8; }"

        // ── RadioButton ──────────────────────────────────────────
        "QRadioButton { color: #344054; spacing: 8px; font-size: 13px; }"
        "QRadioButton::indicator {"
        "  width: 16px; height: 16px;"
        "  border: 1.5px solid #b7c1cd; border-radius: 8px; background: #ffffff;"
        "}"
        "QRadioButton::indicator:hover   { border-color: #165DFF; }"
        "QRadioButton::indicator:checked {"
        "  background: #165DFF; border-color: #165DFF;"
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
        "QTabBar::tab:selected  { color: #165DFF; border-bottom-color: #165DFF; font-weight: 600; }"
        "QTabBar::tab:hover:!selected { color: #344054; background: #f3f4f6; border-radius: 6px 6px 0 0; }"

        // ── QTableWidget ─────────────────────────────────────────
        "QTableWidget {"
        "  background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px;"
        "  gridline-color: #f2f4f7; font-size: 13px; outline: none;"
        "}"
        "QTableWidget::item { padding: 0 10px; color: #344054; min-height: 40px; }"
        "QTableWidget::item:selected { background: #E8EFFF; color: #174EA6; }"
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
        "QListWidget::item:selected { background: #E8EFFF; color: #174EA6; }"
        "QListWidget::item:hover:!selected { background: #eef2f6; }"

        // ── QScrollBar（细滑轨）──────────────────────────────────
        "QScrollBar:vertical {"
        "  background: transparent; width: 10px; margin: 2px 1px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #c7d1dc; border-radius: 4px; min-height: 28px;"
        "}"
        "QScrollBar::handle:vertical:hover { background: #9eacba; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal {"
        "  background: transparent; height: 10px; margin: 1px 2px;"
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
        "QMenu::item:selected { background: #F5F8FF; color: #174EA6; }"
        "QMenu::item:disabled { color: #98a2b3; }"
        "QMenu::separator { height: 1px; background: #f0f2f5; margin: 4px 8px; }"

        // ── ToolTip ──────────────────────────────────────────────
        "QToolTip {"
        "  background: #1d2939; color: #f2f4f7; border: none;"
        "  padding: 6px 10px; border-radius: 6px; font-size: 12px;"
        "}"

        // ── QMessageBox ──────────────────────────────────────────
        "QMessageBox { background: #F5F7FB; }"
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
        "QProgressBar::chunk { background: #165DFF; border-radius: 4px; }"

        // ── Shell details ───────────────────────────────────────
        "QMenuBar { background:#ffffff; color:#344054; border-bottom:1px solid #E2E8F0; }"
        "QMenuBar::item { padding:6px 10px; background:transparent; border-radius:5px; }"
        "QMenuBar::item:selected { background:#EEF4FF; color:#174EA6; }"
        "QStatusBar { background:#ffffff; color:#667085; border-top:1px solid #E2E8F0; }"
        "QTreeView { background:#ffffff; color:#344054; border:none; outline:none; }"
        "QTreeView::item { min-height:28px; padding:2px 4px; }"
        "QTreeView::item:selected { background:#E8EFFF; color:#174EA6; }"
        "QTreeView::item:hover:!selected { background:#F5F8FF; }"
        "QScrollArea { background:transparent; border:none; }"
        "QScrollArea > QWidget > QWidget { background:transparent; }"
    ));
}

} // namespace AppTheme
