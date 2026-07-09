#include "env_config_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QStandardPaths>

EnvConfigDialog::EnvConfigDialog(ConfigManager *configManager,
                                 EnvDetector *envDetector,
                                 QWidget *parent)
    : QDialog(parent)
    , m_configManager(configManager)
    , m_envDetector(envDetector)
{
    setupUi();
    setWindowTitle("环境配置");
    resize(700, 600);

    // 设置默认值
    m_baseUrlEdit->setText("https://www.aegisy.cc/v1");
}

void EnvConfigDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Title
    QLabel *titleLabel = new QLabel("配置 AI 应用", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    QLabel *subtitleLabel = new QLabel(
        "将您的 Aegisy API 配置应用到 Claude Desktop、Cursor 和 Continue.dev", this);
    subtitleLabel->setStyleSheet("color: #666;");
    subtitleLabel->setWordWrap(true);
    mainLayout->addWidget(subtitleLabel);

    // Configuration Section
    QGroupBox *configGroup = new QGroupBox("配置", this);
    QFormLayout *configLayout = new QFormLayout(configGroup);
    configLayout->setSpacing(10);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText("sk-your-api-key");
    m_apiKeyEdit->setMinimumHeight(35);
    configLayout->addRow("API Key:", m_apiKeyEdit);

    m_baseUrlEdit = new QLineEdit(this);
    m_baseUrlEdit->setPlaceholderText("https://www.aegisy.cc/v1");
    m_baseUrlEdit->setMinimumHeight(35);
    configLayout->addRow("Base URL:", m_baseUrlEdit);

    mainLayout->addWidget(configGroup);

    // Target Applications Section
    QGroupBox *targetGroup = new QGroupBox("目标应用", this);
    QVBoxLayout *targetLayout = new QVBoxLayout(targetGroup);

    m_claudeCheckBox = new QCheckBox("Claude Desktop", this);
    m_claudeCheckBox->setChecked(true);
    targetLayout->addWidget(m_claudeCheckBox);

    m_cursorCheckBox = new QCheckBox("Cursor 编辑器", this);
    m_cursorCheckBox->setChecked(true);
    targetLayout->addWidget(m_cursorCheckBox);

    m_continueCheckBox = new QCheckBox("Continue.dev", this);
    m_continueCheckBox->setChecked(true);
    targetLayout->addWidget(m_continueCheckBox);

    mainLayout->addWidget(targetGroup);

    // Progress Section
    m_progressBar = new QProgressBar(this);
    m_progressBar->setMinimum(0);
    m_progressBar->setMaximum(100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    mainLayout->addWidget(m_progressBar);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("font-weight: bold;");
    mainLayout->addWidget(m_statusLabel);

    // Log Output
    QLabel *logLabel = new QLabel("日志:", this);
    mainLayout->addWidget(logLabel);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(150);
    m_logOutput->setStyleSheet(
        "QTextEdit {"
        "  background-color: #f8f9fa;"
        "  border: 1px solid #ddd;"
        "  border-radius: 4px;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "}"
    );
    mainLayout->addWidget(m_logOutput);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();

    m_backupButton = new QPushButton("📦 备份当前配置", this);
    m_backupButton->setMinimumHeight(35);
    buttonLayout->addWidget(m_backupButton);

    m_restoreButton = new QPushButton("♻️ 恢复备份", this);
    m_restoreButton->setMinimumHeight(35);
    m_restoreButton->setEnabled(false);
    buttonLayout->addWidget(m_restoreButton);

    buttonLayout->addStretch();

    m_applyButton = new QPushButton("✓ 应用配置", this);
    m_applyButton->setMinimumHeight(35);
    m_applyButton->setMinimumWidth(150);
    m_applyButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #27ae60;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #229954;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #bdc3c7;"
        "}"
    );
    buttonLayout->addWidget(m_applyButton);

    m_closeButton = new QPushButton("Close", this);
    m_closeButton->setMinimumHeight(35);
    m_closeButton->setMinimumWidth(100);
    buttonLayout->addWidget(m_closeButton);

    mainLayout->addLayout(buttonLayout);

    // Connections
    connect(m_applyButton, &QPushButton::clicked, this, &EnvConfigDialog::onApplyClicked);
    connect(m_backupButton, &QPushButton::clicked, this, &EnvConfigDialog::onBackupClicked);
    connect(m_restoreButton, &QPushButton::clicked, this, &EnvConfigDialog::onRestoreClicked);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void EnvConfigDialog::setConfiguration(const QString &apiKey, const QString &baseUrl)
{
    m_apiKey = apiKey;
    m_baseUrl = baseUrl;

    m_apiKeyEdit->setText(apiKey);
    if (!baseUrl.isEmpty()) {
        m_baseUrlEdit->setText(baseUrl);
    }
}

void EnvConfigDialog::onApplyClicked()
{
    QString apiKey = m_apiKeyEdit->text().trimmed();
    QString baseUrl = m_baseUrlEdit->text().trimmed();

    if (apiKey.isEmpty()) {
        QMessageBox::warning(this, "输入无效", "请输入 API Key。");
        return;
    }

    if (baseUrl.isEmpty()) {
        QMessageBox::warning(this, "输入无效", "请输入 Base URL。");
        return;
    }

    // 检查是否至少选择了一个目标
    if (!m_claudeCheckBox->isChecked() &&
        !m_cursorCheckBox->isChecked() &&
        !m_continueCheckBox->isChecked()) {
        QMessageBox::warning(this, "未选择目标",
                           "请至少选择一个应用进行配置。");
        return;
    }

    // 确认对话框
    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "确认配置",
                                  "This will modify your application configurations.\n"
                                  "A backup will be created automatically.\n\n"
                                  "是否继续？",
                                  QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    // 禁用按钮
    m_applyButton->setEnabled(false);
    m_backupButton->setEnabled(false);
    m_closeButton->setEnabled(false);

    m_logOutput->clear();
    logMessage("🚀 Starting configuration process...");

    // 应用配置
    if (applyConfiguration()) {
        updateProgress(100, "✓ 配置应用成功！");
        m_statusLabel->setStyleSheet("color: #27ae60; font-weight: bold;");

        QMessageBox::information(this, "成功",
                               "Configuration applied successfully!\n\n"
                               "注意：您可能需要重启应用以使更改生效。");

        emit configurationApplied();
    } else {
        updateProgress(0, "✗ 配置失败");
        m_statusLabel->setStyleSheet("color: #e74c3c; font-weight: bold;");
    }

    // 重新启用按钮
    m_applyButton->setEnabled(true);
    m_backupButton->setEnabled(true);
    m_closeButton->setEnabled(true);
}

bool EnvConfigDialog::applyConfiguration()
{
    int totalSteps = 0;
    int currentStep = 0;

    // 计算总步骤数
    if (m_claudeCheckBox->isChecked()) totalSteps++;
    if (m_cursorCheckBox->isChecked()) totalSteps++;
    if (m_continueCheckBox->isChecked()) totalSteps++;
    totalSteps++; // 备份步骤

    // 1. 自动备份
    updateProgress(0, "创建备份...");
    logMessage("📦 Creating backup of current configurations...");

    if (!backupConfigurations()) {
        logMessage("⚠️ Backup failed, but continuing...", "#f39c12");
    } else {
        logMessage("✓ Backup created successfully", "#27ae60");
        m_restoreButton->setEnabled(true);
    }
    currentStep++;

    QString apiKey = m_apiKeyEdit->text().trimmed();
    QString baseUrl = m_baseUrlEdit->text().trimmed();

    // 2. 配置 Claude Desktop
    if (m_claudeCheckBox->isChecked()) {
        updateProgress((currentStep * 100) / totalSteps, "配置 Claude Desktop...");
        logMessage("🔧 Configuring Claude Desktop...");

        if (m_configManager->writeClaudeConfig(apiKey, baseUrl)) {
            logMessage("✓ Claude Desktop configured", "#27ae60");
        } else {
            logMessage("✗ Failed to configure Claude Desktop", "#e74c3c");
        }
        currentStep++;
    }

    // 3. 配置 Cursor
    if (m_cursorCheckBox->isChecked()) {
        updateProgress((currentStep * 100) / totalSteps, "配置 Cursor...");
        logMessage("🔧 Configuring Cursor...");

        if (m_configManager->writeCursorConfig(apiKey, baseUrl)) {
            logMessage("✓ Cursor configured", "#27ae60");
        } else {
            logMessage("✗ Failed to configure Cursor", "#e74c3c");
        }
        currentStep++;
    }

    // 4. 配置 Continue.dev
    if (m_continueCheckBox->isChecked()) {
        updateProgress((currentStep * 100) / totalSteps, "配置 Continue.dev...");
        logMessage("🔧 Configuring Continue.dev...");

        if (m_configManager->writeContinueConfig(apiKey, baseUrl)) {
            logMessage("✓ Continue.dev configured", "#27ae60");
        } else {
            logMessage("✗ Failed to configure Continue.dev", "#e74c3c");
        }
        currentStep++;
    }

    return true;
}

bool EnvConfigDialog::backupConfigurations()
{
    // 创建备份目录
    QString backupDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
    QDir dir;
    if (!dir.exists(backupDir)) {
        dir.mkpath(backupDir);
    }

    // 生成备份文件名（时间戳）
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_backupPath = backupDir + "/config_backup_" + timestamp;

    if (!dir.exists(m_backupPath)) {
        dir.mkpath(m_backupPath);
    }

    bool success = true;

    // 备份 Claude Desktop 配置
    QString claudePath = m_envDetector->getClaudeConfigPath();
    if (QFile::exists(claudePath)) {
        QString backupFile = m_backupPath + "/claude_desktop_config.json";
        if (QFile::copy(claudePath, backupFile)) {
            logMessage(QString("  ✓ Backed up: %1").arg(claudePath), "#666");
        } else {
            logMessage(QString("  ✗ Failed to backup: %1").arg(claudePath), "#e74c3c");
            success = false;
        }
    }

    // 备份 Cursor 配置
    QString cursorPath = m_envDetector->getCursorConfigPath();
    if (QFile::exists(cursorPath)) {
        QString backupFile = m_backupPath + "/cursor_settings.json";
        if (QFile::copy(cursorPath, backupFile)) {
            logMessage(QString("  ✓ Backed up: %1").arg(cursorPath), "#666");
        } else {
            logMessage(QString("  ✗ Failed to backup: %1").arg(cursorPath), "#e74c3c");
            success = false;
        }
    }

    // 备份 Continue.dev 配置
    QString continuePath = m_envDetector->getContinueConfigPath();
    if (QFile::exists(continuePath)) {
        QString backupFile = m_backupPath + "/continue_config.json";
        if (QFile::copy(continuePath, backupFile)) {
            logMessage(QString("  ✓ Backed up: %1").arg(continuePath), "#666");
        } else {
            logMessage(QString("  ✗ Failed to backup: %1").arg(continuePath), "#e74c3c");
            success = false;
        }
    }

    if (success) {
        logMessage(QString("📁 Backup location: %1").arg(m_backupPath), "#3498db");
    }

    return success;
}

void EnvConfigDialog::onBackupClicked()
{
    logMessage("📦 Creating manual backup...");

    if (backupConfigurations()) {
        m_restoreButton->setEnabled(true);
        QMessageBox::information(this, "备份完成",
                               QString("Backup created successfully!\n\nLocation:\n%1")
                               .arg(m_backupPath));
    } else {
        QMessageBox::warning(this, "备份失败",
                           "Some files could not be backed up.\nCheck the log for details.");
    }
}

void EnvConfigDialog::onRestoreClicked()
{
    if (m_backupPath.isEmpty()) {
        // 显示备份历史选择对话框
        QString backupDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/backups";
        QDir dir(backupDir);

        if (!dir.exists()) {
            QMessageBox::warning(this, "无备份", "未找到备份目录。");
            return;
        }

        // 获取所有备份目录
        QStringList backups = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

        if (backups.isEmpty()) {
            QMessageBox::warning(this, "无备份", "没有可恢复的备份。");
            return;
        }

        // 让用户选择备份
        QStringList displayList;
        for (const QString &backup : backups) {
            QString displayName = backup;
            // 格式化显示名称: config_backup_20240101_120000 -> 2024-01-01 12:00:00
            if (displayName.startsWith("config_backup_")) {
                QString timestamp = displayName.mid(14); // 移除 "config_backup_"
                if (timestamp.length() >= 15) {
                    QString date = timestamp.left(8);
                    QString time = timestamp.mid(9, 6);
                    displayName = QString("%1-%2-%3 %4:%5:%6")
                        .arg(date.mid(0, 4))
                        .arg(date.mid(4, 2))
                        .arg(date.mid(6, 2))
                        .arg(time.mid(0, 2))
                        .arg(time.mid(2, 2))
                        .arg(time.mid(4, 2));
                }
            }
            displayList.append(displayName);
        }

        bool ok;
        QString selected = QInputDialog::getItem(this, "选择备份",
                                                "选择要恢复的备份:",
                                                displayList, 0, false, &ok);
        if (!ok) {
            return;
        }

        int index = displayList.indexOf(selected);
        m_backupPath = backupDir + "/" + backups[index];
    }

    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "确认恢复",
                                  QString("This will restore configurations from:\n%1\n\n"
                                         "Current configurations will be overwritten.\n\n"
                                         "是否继续？")
                                  .arg(m_backupPath),
                                  QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    logMessage("♻️ Restoring from backup...");

    bool success = true;
    int restored = 0;

    // 恢复 Claude Desktop 配置
    QString claudeBackup = m_backupPath + "/claude_desktop_config.json";
    if (QFile::exists(claudeBackup)) {
        QString claudePath = m_envDetector->getClaudeConfigPath();

        // 删除现有文件
        if (QFile::exists(claudePath)) {
            QFile::remove(claudePath);
        }

        if (QFile::copy(claudeBackup, claudePath)) {
            logMessage(QString("  ✓ Restored: %1").arg(claudePath), "#27ae60");
            restored++;
        } else {
            logMessage(QString("  ✗ Failed to restore: %1").arg(claudePath), "#e74c3c");
            success = false;
        }
    }

    // 恢复 Cursor 配置
    QString cursorBackup = m_backupPath + "/cursor_settings.json";
    if (QFile::exists(cursorBackup)) {
        QString cursorPath = m_envDetector->getCursorConfigPath();

        if (QFile::exists(cursorPath)) {
            QFile::remove(cursorPath);
        }

        if (QFile::copy(cursorBackup, cursorPath)) {
            logMessage(QString("  ✓ Restored: %1").arg(cursorPath), "#27ae60");
            restored++;
        } else {
            logMessage(QString("  ✗ Failed to restore: %1").arg(cursorPath), "#e74c3c");
            success = false;
        }
    }

    // 恢复 Continue.dev 配置
    QString continueBackup = m_backupPath + "/continue_config.json";
    if (QFile::exists(continueBackup)) {
        QString continuePath = m_envDetector->getContinueConfigPath();

        if (QFile::exists(continuePath)) {
            QFile::remove(continuePath);
        }

        if (QFile::copy(continueBackup, continuePath)) {
            logMessage(QString("  ✓ Restored: %1").arg(continuePath), "#27ae60");
            restored++;
        } else {
            logMessage(QString("  ✗ Failed to restore: %1").arg(continuePath), "#e74c3c");
            success = false;
        }
    }

    if (success && restored > 0) {
        logMessage(QString("✓ Successfully restored %1 configuration(s)").arg(restored), "#27ae60");
        QMessageBox::information(this, "恢复完成",
                               QString("Successfully restored %1 configuration file(s)!\n\n"
                                      "Note: You may need to restart your applications "
                                      "for the changes to take effect.")
                               .arg(restored));
    } else if (restored == 0) {
        logMessage("⚠️ No configuration files found in backup", "#f39c12");
        QMessageBox::warning(this, "恢复失败",
                           "在选定的备份中未找到配置文件。");
    } else {
        logMessage("⚠️ Some files could not be restored", "#f39c12");
        QMessageBox::warning(this, "部分恢复",
                           "Some configuration files could not be restored.\n"
                           "请查看日志了解详情。");
    }
}

void EnvConfigDialog::updateProgress(int value, const QString &message)
{
    m_progressBar->setValue(value);
    m_statusLabel->setText(message);
}

void EnvConfigDialog::logMessage(const QString &message, const QString &color)
{
    QString html = QString("<span style='color:%1'>%2</span>").arg(color, message);
    m_logOutput->append(html);
}
