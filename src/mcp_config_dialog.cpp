#include "mcp_config_dialog.h"
#include "app_theme.h"
#include "extension_staging_backup_capture.h"
#include "extension_staging_backup_retention.h"
#include "mcp_configuration_inventory.h"
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

namespace {

// 该对话框编辑的是整个共享设置文件，而暂存捕获对 `mcp:` 主体的诚实备份单元同样是
// 整个文件（恢复语义也是整文件），因此备份主体是稳定的单一个：按单个服务器命名会
// 暗示备份只覆盖那一个服务器的条目——那会是一份 dishonest 的命名。
const QString kStagingBackupSubject = QStringLiteral("mcp:claude-settings");

// 修剪备注如实区分三种现实：计划失败（退化清点——零删除、旧备份全部保留）、无需修剪、
// 逐条汇总（删了几份 / 损坏作为证据原地保留几份 / 失败几份加诊断）。修剪是保存与捕获
// 都成功之后的收尾清理，它的任何失败都绝不代表保存或捕获失败——措辞必须说出这一点。
QString retentionNoteFor(const ExtensionStagingBackupRetentionRun &run)
{
    if (run.planFailed) {
        return QStringLiteral(
            "；备份修剪未能执行（%1），旧备份全部保留，本次保存与捕获不受影响")
            .arg(run.planError);
    }
    if (run.removedCount == 0 && run.corruptKeptCount == 0
            && run.failures.isEmpty()) {
        return QStringLiteral("；备份数量在保留上限之内，无需修剪");
    }
    QString note = QStringLiteral("；已按保留上限修剪 %1 份旧备份")
        .arg(run.removedCount);
    if (run.corruptKeptCount > 0) {
        note += QStringLiteral("，%1 份损坏备份作为证据原地保留")
            .arg(run.corruptKeptCount);
    }
    if (!run.failures.isEmpty()) {
        note += QStringLiteral("，%1 份修剪失败（%2），未删除的备份全部保留")
            .arg(run.failures.size()).arg(run.failures.first().diagnostic);
    }
    return note;
}

} // namespace

QString McpConfigDialog::settingsFilePath()
{
    const QString overrideRoot = QString::fromLocal8Bit(
        qgetenv("AEGISY_CONFIG_HOME")).trimmed();
    const QString root = overrideRoot.isEmpty() ? QDir::homePath() : overrideRoot;
    return QDir(root).filePath(QStringLiteral(".claude/settings.json"));
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

McpConfigDialog::McpConfigDialog(
        ConfigurationBackupKeyProvider *stagingBackupKeyProvider,
        const QString &stagingBackupRoot, QWidget *parent)
    : McpConfigDialog(parent)
{
    m_stagingBackupKeyProvider = stagingBackupKeyProvider;
    m_stagingBackupRoot = stagingBackupRoot;
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
    const McpConfigurationInventoryResult inventory =
        McpConfigurationInventory::inspectFile(settingsFilePath());
    m_sourceValid = inventory.state == McpConfigurationInventoryState::Empty
        || inventory.state == McpConfigurationInventoryState::Ready;
    m_sourceIdentity = m_sourceValid ? inventory.sourceIdentity : QString();
    m_mcpServers = m_sourceValid
        ? inventory.root.value(QStringLiteral("mcpServers")).toObject()
        : QJsonObject();
    rebuildTable();

    m_addButton->setEnabled(m_sourceValid);
    m_saveButton->setEnabled(m_sourceValid);
    if (!m_sourceValid) {
        m_editButton->setEnabled(false);
        m_removeButton->setEnabled(false);
        m_statusLabel->setState(
            inventory.state == McpConfigurationInventoryState::Invalid
                ? QStringLiteral("配置损坏，已阻止写入")
                : QStringLiteral("配置不可用，已阻止写入"),
            StatusBadge::Tone::Error,
            style()->standardIcon(QStyle::SP_MessageBoxCritical));
        return;
    }

    const int count = m_mcpServers.size();
    m_statusLabel->setState(
        count > 0 ? QStringLiteral("%1 个服务器").arg(count)
                  : QStringLiteral("暂无配置"),
        StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_FileDialogListView));
}

bool McpConfigDialog::saveToSettings()
{
    m_lastSaveError.clear();
    m_lastRetentionNote.clear();
    if (!m_sourceValid || m_sourceIdentity.isEmpty()) {
        m_lastSaveError = QStringLiteral("配置源已损坏或不可用，未确认保存");
        return false;
    }
    const QString path = settingsFilePath();
    const McpConfigurationInventoryResult current =
        McpConfigurationInventory::inspectFile(path);
    if ((current.state != McpConfigurationInventoryState::Empty
            && current.state != McpConfigurationInventoryState::Ready)
            || current.sourceIdentity != m_sourceIdentity) {
        m_sourceValid = false;
        m_lastSaveError = QStringLiteral("配置已被外部修改或损坏，未确认保存");
        return false;
    }

    // 保存前备份（仅在注入暂存备份接线后启用）：顺序是安全性质——捕获发生在身份
    // 复查确认文件仍是读入时的那份之后、写入之前；捕获自身带漂移复查，漂移即拒绝
    // 保存，与身份复查的语义一致。来源为 Empty（文件不存在）时没有可丢失的字节，
    // 诚实跳过捕获而不是假装备份了一份"空"。备份失败即拒绝保存：这与激活路径
    // "无法创建可验证安全备份则未修改配置"的 fail-closed 先例逐字一致。
    const bool backupWired = m_stagingBackupKeyProvider
        && !m_stagingBackupRoot.isEmpty();
    const bool capturedBackup = backupWired
        && current.state == McpConfigurationInventoryState::Ready;
    if (capturedBackup) {
        ExtensionStagingBackupCaptureResult backup;
        QString backupError;
        if (!ExtensionStagingBackupCapture::capture(
                kStagingBackupSubject, path, m_stagingBackupRoot,
                m_stagingBackupKeyProvider, &backup, &backupError)) {
            m_lastSaveError = QStringLiteral("保存前备份失败，未确认保存：%1")
                .arg(backupError);
            return false;
        }
        if (m_afterBackupCaptureHook) m_afterBackupCaptureHook();
        // 捕获后、写入前的身份复查：捕获与写入之间文件被换掉时，保存不得落在陈旧
        // 字节上（备份本身仍是捕获时刻真实字节的诚实备份，留在暂存域里）。
        const McpConfigurationInventoryResult stillCurrent =
            McpConfigurationInventory::inspectFile(path);
        if (stillCurrent.state != McpConfigurationInventoryState::Ready
                || stillCurrent.sourceIdentity != m_sourceIdentity) {
            m_sourceValid = false;
            m_lastSaveError = QStringLiteral("配置在备份后发生变化，未确认保存");
            return false;
        }
    }

    QJsonObject root = current.root;
    root[QStringLiteral("mcpServers")] = m_mcpServers;
    if (!writeSettingsFile(root)) {
        m_lastSaveError = QStringLiteral("配置写入失败，未确认保存");
        return false;
    }
    const McpConfigurationInventoryResult verified =
        McpConfigurationInventory::inspectFile(path);
    if (verified.state != McpConfigurationInventoryState::Ready
            || verified.root.value(QStringLiteral("mcpServers")).toObject()
                != m_mcpServers) {
        m_sourceValid = false;
        m_lastSaveError = QStringLiteral("写入后校验失败，未确认保存");
        return false;
    }
    m_sourceIdentity = verified.sourceIdentity;
    // 捕获成功且保存校验通过后的保留期修剪（共享唯一入口）：保存与备份都已经真实存在，
    // 修剪是收尾清理——它的任何失败都绝不翻转本次保存的成功，只如实记入备注随保存结果
    // 上屏。对话框是应用模态（既有事实），捕获在哪里同步发生，修剪就在哪里同步发生。
    if (capturedBackup) {
        m_lastRetentionNote = retentionNoteFor(
            ExtensionStagingBackupRetention::pruneAfterCapture(
                m_stagingBackupRoot, m_stagingBackupKeyProvider,
                kStagingBackupSubject));
    }
    return true;
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
    const bool hasSelection = m_sourceValid && m_table->currentRow() >= 0;
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
            QStringLiteral("已保存") + m_lastRetentionNote,
            StatusBadge::Tone::Success,
            style()->standardIcon(QStyle::SP_DialogApplyButton));
    } else {
        m_statusLabel->setState(
            QStringLiteral("保存失败"), StatusBadge::Tone::Error,
            style()->standardIcon(QStyle::SP_MessageBoxCritical));
        QMessageBox::critical(this, QStringLiteral("保存失败"),
            m_lastSaveError.isEmpty()
                ? QStringLiteral("配置已损坏、被外部修改或无法验证写入，未确认保存。")
                : m_lastSaveError);
    }
}
