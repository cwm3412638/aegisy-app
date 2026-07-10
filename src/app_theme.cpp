#include "app_theme.h"

#include <QFont>
#include <QStyleFactory>

namespace AppTheme {

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #0f766e; color: white; border: 1px solid #0f766e;"
        "  border-radius: 8px; padding: 0 16px; font-size: 13px; font-weight: 600;"
        "}"
        "QPushButton:hover { background: #0b625c; border-color: #0b625c; }"
        "QPushButton:pressed { background: #094f4a; border-color: #094f4a; }"
        "QPushButton:disabled { background: #d7dde3; border-color: #d7dde3; color: #8a96a3; }");
}

QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #ffffff; color: #344054; border: 1px solid #cfd7e3;"
        "  border-radius: 8px; padding: 0 14px; font-size: 13px; font-weight: 500;"
        "}"
        "QPushButton:hover { background: #f3f8f7; border-color: #7bb8b0; color: #0f5f59; }"
        "QPushButton:pressed { background: #e7f5f2; }"
        "QPushButton:disabled { color: #98a2b3; background: #f8fafb; border-color: #eaecf0; }");
}

QString dangerButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #ffffff; color: #b42318; border: 1px solid #fecdca;"
        "  border-radius: 8px; padding: 0 12px; font-size: 13px; font-weight: 500;"
        "}"
        "QPushButton:hover { background: #fef3f2; border-color: #f04438; }"
        "QPushButton:pressed { background: #fee4e2; }"
        "QPushButton:disabled { background: #f8fafb; border-color: #eaecf0; color: #c7cdd5; }");
}

void apply(QApplication &application)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    QFont font = application.font();
    font.setPointSize(10);
    application.setFont(font);

    application.setStyleSheet(QStringLiteral(
        "QMainWindow, QDialog { background: #f4f7f9; }"
        "QLabel { color: #17212b; }"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit {"
        "  background: #ffffff; color: #17212b; border: 1px solid #cfd7e3;"
        "  border-radius: 8px; padding: 0 11px; selection-background-color: #bfe5df;"
        "  selection-color: #103f3b;"
        "}"
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { min-height: 36px; }"
        "QTextEdit, QPlainTextEdit { padding: 8px 10px; }"
        "QLineEdit:hover, QComboBox:hover, QSpinBox:hover, QDoubleSpinBox:hover,"
        "QTextEdit:hover, QPlainTextEdit:hover { border-color: #9fb2c3; }"
        "QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus,"
        "QTextEdit:focus, QPlainTextEdit:focus { border: 1px solid #0f766e; }"
        "QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled,"
        "QTextEdit:disabled, QPlainTextEdit:disabled { background: #f1f4f6; color: #98a2b3; }"
        "QComboBox::drop-down { width: 28px; border: none; }"
        "QComboBox QAbstractItemView {"
        "  background: #ffffff; color: #17212b; border: 1px solid #cfd7e3;"
        "  selection-background-color: #e7f5f2; selection-color: #0f5f59;"
        "}"
        "QPushButton {"
        "  background: #0f766e; color: #ffffff; border: 1px solid #0f766e;"
        "  border-radius: 8px; padding: 0 14px; min-height: 36px;"
        "}"
        "QPushButton:hover { background: #0b625c; border-color: #0b625c; }"
        "QPushButton:pressed { background: #094f4a; border-color: #094f4a; }"
        "QPushButton:disabled { background: #d7dde3; border-color: #d7dde3; color: #8a96a3; }"
        "QToolButton { border-radius: 7px; padding: 5px; }"
        "QToolButton:hover { background: #e7f5f2; }"
        "QCheckBox { color: #475467; spacing: 8px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #b7c1cd; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #0f766e; }"
        "QCheckBox::indicator:checked { background: #0f766e; border-color: #0f766e; }"
        "QTableWidget { background: #ffffff; border: 1px solid #dfe6ee; border-radius: 8px; gridline-color: transparent; }"
        "QTableWidget::item { padding: 8px; color: #344054; }"
        "QTableWidget::item:selected { background: #e7f5f2; color: #0f5f59; }"
        "QHeaderView::section { background: #f7f9fb; color: #667085; border: none; border-bottom: 1px solid #dfe6ee; padding: 9px 10px; font-weight: 600; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: #c7d1dc; border-radius: 4px; min-height: 28px; }"
        "QScrollBar::handle:vertical:hover { background: #9eacba; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar:horizontal { background: transparent; height: 8px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: #c7d1dc; border-radius: 4px; min-width: 28px; }"
        "QToolTip { background: #17212b; color: #ffffff; border: none; padding: 6px 8px; }"
        "QMessageBox { background: #f4f7f9; }"));
}

} // namespace AppTheme
