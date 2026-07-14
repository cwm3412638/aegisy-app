#include "mcp_config_dialog.h"
#include "app_theme.h"
#include "status_badge.h"

#include <QDir>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QVBoxLayout>

// ── helpers ───────────────────────────────────────────────────────

QString McpConfigDialog::settingsFilePath()
{
    return QDir::homePath() + QStringLiteral("/.claude/settings.json");
}

QJsonObject McpConfigDialog::readSettingsFile()
{
    QFile file(settingsFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    return doc.isObject() ? doc.object() : QJsonObject();
}

bool McpConfigDialog::writeSettingsFile(const QJsonObject &root)
{
    const QString path = settingsFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

// ── ctor ─────────────────────────────────────────────────────────

McpConfigDialog::McpConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle(QStringLiteral("MCP 服务器共享配置"));
    resize(760, 480);
    setMinimumSize(680, 400);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    loadFromSettings();
}

// ── UI ──────────────────────────────────────────────────────────

void McpConfigDialog::setupUi()
{
    setStyleSheet(QStringLiteral(
        "QDialog { background: #f4f7f9; }"
        "QLabel { color: #182230; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 16);
    root->setSpacing(12);

    // 标题行
    auto *titleRow = new QHBoxLayout;
    auto *titleIcon = new QLabel(this);
    titleIcon->setPixmap(style()->standardIcon(
        QStyle::SP_FileDialogDetailedView).pixmap(20, 20));
    titleRow->addWidget(titleIcon);
    auto *title = new QLabel(QStringLiteral("MCP 服务器共享配置"), this);
    title->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 700; color: #101828;"));
    titleRow->addWidget(title);
    titleRow->addStretch();
    root->addLayout(titleRow);

    // 表格
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("名称"),
        QStringLiteral("命令 / URL"),
        QStringLiteral("类型")
    });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(false);
    m_table->setStyleSheet(QStringLiteral(
        "QTableWidget { background: white; border: 1px solid #e4e7ec;"
        "border-radius: 8px; font-size: 13px; }"
        "QTableWidget::item { padding: 8px 12px; color: #344054; }"
        "QTableWidget::item:selected { background: #e7f5f2; color: #0f5f59; }"));
    root->addWidget(m_table, 1);

    // 按钮行
    auto *btnRow = new QHBoxLayout;
    m_addButton    = new QPushButton(QStringLiteral("添加"), this);
    m_editButton   = new QPushButton(QStringLiteral("编辑"), this);
    m_removeButton = new QPushButton(QStringLiteral("删除"), this);
    m_addButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_editButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_removeButton->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
    m_addButton->setFixedHeight(34);
    m_editButton->setFixedHeight(34);
    m_removeButton->setFixedHeight(34);
    m_addButton->setStyleSheet(AppTheme::primaryButtonStyle());
    m_editButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_removeButton->setStyleSheet(AppTheme::dangerButtonStyle());
    m_editButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    btnRow->addWidget(m_addButton);
    btnRow->addWidget(m_editButton);
    btnRow->addWidget(m_removeButton);
    btnRow->addStretch();
    root->addLayout(btnRow);

    // 状态 + 保存
    auto *footRow = new QHBoxLayout;
    m_statusLabel = new StatusBadge(this);
    m_saveButton = new QPushButton(QStringLiteral("保存"), this);
    m_saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_saveButton->setFixedHeight(36);
    m_saveButton->setMinimumWidth(100);
    m_saveButton->setStyleSheet(AppTheme::primaryButtonStyle());
    auto *cancelBtn = new QPushButton(QStringLiteral("关闭"), this);
    cancelBtn->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    cancelBtn->setFixedHeight(36);
    cancelBtn->setStyleSheet(AppTheme::secondaryButtonStyle());
    footRow->addWidget(m_statusLabel, 1);
    footRow->addWidget(cancelBtn);
    footRow->addWidget(m_saveButton);
    root->addLayout(footRow);

    connect(m_addButton,    &QPushButton::clicked, this, &McpConfigDialog::onAddServer);
    connect(m_editButton,   &QPushButton::clicked, this, &McpConfigDialog::onEditServer);
    connect(m_removeButton, &QPushButton::clicked, this, &McpConfigDialog::onRemoveServer);
    connect(m_saveButton,   &QPushButton::clicked, this, &McpConfigDialog::onSave);
    connect(cancelBtn,      &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &McpConfigDialog::onSelectionChanged);
    connect(m_table, &QTableWidget::itemDoubleClicked,
            this, [this]() { onEditServer(); });
}

// ── data ─────────────────────────────────────────────────────────

void McpConfigDialog::loadFromSettings()
{
    const QJsonObject root = readSettingsFile();
    m_mcpServers = root.value(QStringLiteral("mcpServers")).toObject();
    rebuildTable();

    const int count = m_mcpServers.size();
    m_statusLabel->setState(
        count > 0 ? QStringLiteral("%1 个服务器").arg(count)
                  : QStringLiteral("暂无配置"),
        StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_FileDialogListView));
}

bool McpConfigDialog::saveToSettings()
{
    QJsonObject root = readSettingsFile();
    root[QStringLiteral("mcpServers")] = m_mcpServers;
    return writeSettingsFile(root);
}

void McpConfigDialog::rebuildTable()
{
    m_table->setRowCount(0);
    const QStringList keys = m_mcpServers.keys();
    for (const QString &name : keys) {
        const QJsonObject server = m_mcpServers.value(name).toObject();
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        // 名称
        m_table->setItem(row, 0, new QTableWidgetItem(name));

        // 命令/URL
        QString cmd;
        if (server.contains(QStringLiteral("command"))) {
            cmd = server.value(QStringLiteral("command")).toString();
        } else if (server.contains(QStringLiteral("url"))) {
            cmd = server.value(QStringLiteral("url")).toString();
        } else {
            cmd = QStringLiteral("（自定义配置）");
        }
        m_table->setItem(row, 1, new QTableWidgetItem(cmd));

        // 类型
        const QString type = server.contains(QStringLiteral("url"))
            ? QStringLiteral("SSE")
            : QStringLiteral("Stdio");
        m_table->setItem(row, 2, new QTableWidgetItem(type));
    }
    m_table->sortByColumn(0, Qt::AscendingOrder);
    onSelectionChanged();
}

// ── slots ─────────────────────────────────────────────────────────

void McpConfigDialog::onSelectionChanged()
{
    const bool hasSelection = m_table->currentRow() >= 0;
    m_editButton->setEnabled(hasSelection);
    m_removeButton->setEnabled(hasSelection);
}

void McpConfigDialog::onAddServer()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("添加 MCP 服务器"),
        QStringLiteral("服务器名称（唯一标识，如 filesystem）："),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    if (m_mcpServers.contains(name.trimmed())) {
        QMessageBox::warning(this, QStringLiteral("名称重复"),
            QStringLiteral("已存在名为「%1」的服务器。").arg(name));
        return;
    }

    const QString cmd = QInputDialog::getText(
        this, QStringLiteral("添加 MCP 服务器"),
        QStringLiteral("命令路径（stdio 模式，如 /usr/bin/node mcp.js）\n"
                       "或 URL（SSE 模式，如 http://localhost:3000/sse）："),
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    const QString trimmedName = name.trimmed();
    const QString trimmedCmd  = cmd.trimmed();
    QJsonObject server;
    if (trimmedCmd.startsWith(QStringLiteral("http://"))
            || trimmedCmd.startsWith(QStringLiteral("https://"))) {
        server[QStringLiteral("url")] = trimmedCmd;
    } else if (!trimmedCmd.isEmpty()) {
        // 把命令字符串拆成 command + args
        const QStringList parts = trimmedCmd.split(QStringLiteral(" "), Qt::SkipEmptyParts);
        server[QStringLiteral("command")] = parts.first();
        if (parts.size() > 1) {
            QJsonArray args;
            for (int i = 1; i < parts.size(); ++i)
                args.append(parts[i]);
            server[QStringLiteral("args")] = args;
        }
    }
    m_mcpServers[trimmedName] = server;
    rebuildTable();
    m_statusLabel->setState(
        QStringLiteral("已添加「%1」· 未保存").arg(trimmedName),
        StatusBadge::Tone::Warning,
        style()->standardIcon(QStyle::SP_MessageBoxWarning));
}

void McpConfigDialog::onEditServer()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    const QString name = m_table->item(row, 0)->text();
    const QJsonObject server = m_mcpServers.value(name).toObject();

    QString current;
    if (server.contains(QStringLiteral("url"))) {
        current = server.value(QStringLiteral("url")).toString();
    } else {
        QStringList parts;
        parts << server.value(QStringLiteral("command")).toString();
        const QJsonArray args = server.value(QStringLiteral("args")).toArray();
        for (const QJsonValue &v : args)
            parts << v.toString();
        current = parts.join(QLatin1Char(' '));
    }

    bool ok = false;
    const QString newCmd = QInputDialog::getText(
        this, QStringLiteral("编辑 MCP 服务器"),
        QStringLiteral("命令路径或 URL："),
        QLineEdit::Normal, current, &ok);
    if (!ok) return;

    const QString trimmedCmd = newCmd.trimmed();
    QJsonObject updated;
    if (trimmedCmd.startsWith(QStringLiteral("http://"))
            || trimmedCmd.startsWith(QStringLiteral("https://"))) {
        updated[QStringLiteral("url")] = trimmedCmd;
    } else if (!trimmedCmd.isEmpty()) {
        const QStringList parts = trimmedCmd.split(QStringLiteral(" "), Qt::SkipEmptyParts);
        updated[QStringLiteral("command")] = parts.first();
        if (parts.size() > 1) {
            QJsonArray args;
            for (int i = 1; i < parts.size(); ++i)
                args.append(parts[i]);
            updated[QStringLiteral("args")] = args;
        }
    }
    m_mcpServers[name] = updated;
    rebuildTable();
    m_statusLabel->setState(
        QStringLiteral("已修改「%1」· 未保存").arg(name),
        StatusBadge::Tone::Warning,
        style()->standardIcon(QStyle::SP_MessageBoxWarning));
}

void McpConfigDialog::onRemoveServer()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    const QString name = m_table->item(row, 0)->text();
    const auto reply = QMessageBox::question(
        this, QStringLiteral("删除服务器"),
        QStringLiteral("确定删除「%1」吗？").arg(name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    m_mcpServers.remove(name);
    rebuildTable();
    m_statusLabel->setState(
        QStringLiteral("已删除「%1」· 未保存").arg(name),
        StatusBadge::Tone::Warning,
        style()->standardIcon(QStyle::SP_MessageBoxWarning));
}

void McpConfigDialog::onSave()
{
    if (saveToSettings()) {
        m_statusLabel->setState(
            QStringLiteral("已保存"), StatusBadge::Tone::Success,
            style()->standardIcon(QStyle::SP_DialogApplyButton));
    } else {
        m_statusLabel->setState(
            QStringLiteral("保存失败"), StatusBadge::Tone::Error,
            style()->standardIcon(QStyle::SP_MessageBoxCritical));
        QMessageBox::critical(this, QStringLiteral("保存失败"),
            QStringLiteral("无法写入 ~/.claude/settings.json，请检查文件权限。"));
    }
}
