#include "system_doctor_dialog.h"

#include "app_theme.h"
#include "secure_storage.h"
#include "status_badge.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QVersionNumber>

SystemDoctorDialog::SystemDoctorDialog(ToolManager *toolManager, QWidget *parent)
    : QDialog(parent)
    , m_toolManager(toolManager)
{
    setupUi();
    setWindowTitle(QStringLiteral("系统体检"));
    setMinimumSize(780, 520);
    resize(900, 600);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_toolManager, &ToolManager::installFinished,
            this, &SystemDoctorDialog::onInstallFinished);
    connect(m_toolManager, &ToolManager::installOutput,
            this, &SystemDoctorDialog::onInstallOutput);
    QTimer::singleShot(0, this, &SystemDoctorDialog::refreshReport);
}

void SystemDoctorDialog::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(12);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("系统与 AI 工具体检"), this);
    title->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: 700; color: #101828;"));
    header->addWidget(title);
    header->addStretch();
    m_summaryLabel = new StatusBadge(this);
    m_summaryLabel->setState(
        QStringLiteral("等待检测"), StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_MessageBoxInformation));
    header->addWidget(m_summaryLabel);
    root->addLayout(header);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("类别"), QStringLiteral("项目"), QStringLiteral("状态"),
        QStringLiteral("版本或详情"), QStringLiteral("操作") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(m_table, 1);

    auto *footer = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    footer->addWidget(m_statusLabel, 1);
    m_refreshButton = new QPushButton(QStringLiteral("重新检测"), this);
    m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setStyleSheet(AppTheme::primaryButtonStyle());
    footer->addWidget(m_refreshButton);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(m_refreshButton, &QPushButton::clicked,
            this, &SystemDoctorDialog::refreshReport);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void SystemDoctorDialog::refreshReport()
{
    if (m_scanning) {
        return;
    }
    m_scanning = true;
    m_refreshButton->setEnabled(false);
    m_table->setRowCount(0);
    m_summaryLabel->setState(
        QStringLiteral("检测中"), StatusBadge::Tone::Info,
        style()->standardIcon(QStyle::SP_BrowserReload));
    m_statusLabel->setText(QStringLiteral("正在检查本地命令、配置文件和安全存储..."));

    QPointer<SystemDoctorDialog> guard(this);
    QThread *worker = QThread::create([guard]() {
        ToolManager detector;
        QList<RuntimeStatus> runtimes = detector.detectRuntimes();
        runtimes.append(detector.detectCompanionTools());
        for (AiTool desktopTool : { AiTool::ClaudeCode, AiTool::CodexCli }) {
            const DesktopAppStatus desktop = detector.detectDesktop(desktopTool);
            RuntimeStatus status;
            status.id = desktopTool == AiTool::ClaudeCode
                ? QStringLiteral("claude-desktop") : QStringLiteral("chatgpt-desktop");
            status.category = QStringLiteral("桌面客户端");
            status.name = desktop.appName;
            status.installed = desktop.installed;
            runtimes.append(status);
        }
        QMap<int, ToolStatus> tools;
        for (AiTool tool : { AiTool::ClaudeCode, AiTool::CodexCli,
                             AiTool::GeminiCli, AiTool::OpenCode }) {
            ToolStatus status = detector.detectFast(tool);
            if (status.installed) {
                status.latestVersion = detector.latestVersion(tool, 10000);
                const QVersionNumber local = QVersionNumber::fromString(status.version);
                const QVersionNumber latest = QVersionNumber::fromString(status.latestVersion);
                status.updateAvailable = !local.isNull() && !latest.isNull()
                    && QVersionNumber::compare(local, latest) < 0;
            }
            tools.insert(static_cast<int>(tool), status);
        }
        const bool secureStorageAvailable = SecureStorage::isAvailable();
        const QString dataDirectory = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
        QDir().mkpath(dataDirectory);
        const bool dataDirectoryWritable = QFileInfo(dataDirectory).isWritable();

        if (!guard) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(),
            [guard, runtimes, tools, secureStorageAvailable,
             dataDirectory, dataDirectoryWritable]() {
                if (guard) {
                    guard->applyReport(runtimes, tools, secureStorageAvailable,
                                       dataDirectory, dataDirectoryWritable);
                }
            }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void SystemDoctorDialog::applyReport(const QList<RuntimeStatus> &runtimes,
                                     const QMap<int, ToolStatus> &tools,
                                     bool secureStorageAvailable,
                                     const QString &dataDirectory,
                                     bool dataDirectoryWritable)
{
    int warningCount = 0;
    int errorCount = 0;

    for (const RuntimeStatus &runtime : runtimes) {
        const QString status = runtime.installed
            ? QStringLiteral("正常")
            : (runtime.required ? QStringLiteral("缺少") : QStringLiteral("可选未装"));
        const QString tone = runtime.installed
            ? QStringLiteral("ok")
            : (runtime.required ? QStringLiteral("error") : QStringLiteral("muted"));
        if (!runtime.installed && runtime.required) {
            ++errorCount;
        }
        QString detail;
        if (runtime.installed) {
            detail = runtime.executablePath.isEmpty()
                ? QStringLiteral("已检测到本地安装")
                : QStringLiteral("%1  ·  %2")
                    .arg(runtime.version.isEmpty() ? QStringLiteral("版本未知") : runtime.version,
                         runtime.executablePath);
        } else {
            detail = runtime.command.isEmpty()
                ? QStringLiteral("未检测到本地安装")
                : QStringLiteral("命令 %1 不在可用 PATH 中").arg(runtime.command);
        }
        addRow(runtime.category.isEmpty() ? QStringLiteral("系统依赖") : runtime.category,
               runtime.name, status, detail, tone);
    }

    for (AiTool tool : { AiTool::ClaudeCode, AiTool::CodexCli, AiTool::GeminiCli }) {
        const ToolStatus status = tools.value(static_cast<int>(tool));
        QString state;
        QString tone;
        if (status.repairRequired) {
            state = QStringLiteral("需修复");
            tone = QStringLiteral("error");
            ++errorCount;
        } else if (!status.installed) {
            state = QStringLiteral("未安装");
            tone = QStringLiteral("error");
            ++errorCount;
        } else if (!status.conflictWarning.isEmpty()) {
            state = QStringLiteral("有冲突");
            tone = QStringLiteral("error");
            ++errorCount;
        } else if (!status.configured) {
            state = QStringLiteral("未接入");
            tone = QStringLiteral("warning");
            ++warningCount;
        } else {
            state = status.updateAvailable ? QStringLiteral("可更新")
                                           : QStringLiteral("正常");
            tone = status.updateAvailable ? QStringLiteral("warning")
                                          : QStringLiteral("ok");
            if (status.updateAvailable) ++warningCount;
        }

        QString detail;
        if (status.repairRequired) {
            detail = status.installationIssue;
        } else if (!status.installed) {
            detail = QStringLiteral("未检测到 %1 命令").arg(ToolManager::cliCommand(tool));
        } else if (!status.configured && !status.configurationIssue.isEmpty()) {
            detail = status.configurationIssue;
        } else {
            detail = QStringLiteral("本地 %1%2  ·  %3")
                .arg(status.version.isEmpty() ? QStringLiteral("未知") : status.version,
                     status.latestVersion.isEmpty()
                         ? QString()
                         : QStringLiteral("  ·  最新 %1").arg(status.latestVersion),
                     status.configured ? QStringLiteral("已接入 Aegisy")
                                       : QStringLiteral("尚未写入 Aegisy 配置"));
        }
        if (!status.conflictWarning.isEmpty()) {
            detail = status.conflictWarning;
        }
        AiTool actionTool = tool;
        AiTool *action = !status.installed || status.updateAvailable ? &actionTool : nullptr;
        const QString passiveAction = status.installed && !status.updateAvailable
            ? (status.latestVersion.isEmpty() ? QStringLiteral("已安装")
                                              : QStringLiteral("已是最新"))
            : QString();
        addRow(QStringLiteral("AI 工具"), ToolManager::toolName(tool),
               state, detail, tone, action, status.installed, passiveAction,
               status.repairRequired);
    }

    if (!secureStorageAvailable) {
        ++errorCount;
    }
    addRow(QStringLiteral("安全"), QStringLiteral("系统凭据存储"),
           secureStorageAvailable ? QStringLiteral("正常") : QStringLiteral("不可用"),
           secureStorageAvailable
               ? QStringLiteral("API Key 与登录状态可保存到系统安全存储")
               : QStringLiteral("当前系统没有可用的安全凭据存储"),
           secureStorageAvailable ? QStringLiteral("ok") : QStringLiteral("error"));

    if (!dataDirectoryWritable) {
        ++errorCount;
    }
    addRow(QStringLiteral("应用数据"), QStringLiteral("数据目录"),
           dataDirectoryWritable ? QStringLiteral("正常") : QStringLiteral("不可写"),
           dataDirectory,
           dataDirectoryWritable ? QStringLiteral("ok") : QStringLiteral("error"));

    m_scanning = false;
    m_refreshButton->setEnabled(true);
    if (errorCount == 0 && warningCount == 0) {
        m_summaryLabel->setState(
            QStringLiteral("全部正常"), StatusBadge::Tone::Success,
            style()->standardIcon(QStyle::SP_DialogApplyButton));
    } else {
        m_summaryLabel->setState(
            QStringLiteral("%1 个问题 · %2 个提醒")
                .arg(errorCount).arg(warningCount),
            errorCount > 0 ? StatusBadge::Tone::Error : StatusBadge::Tone::Warning,
            style()->standardIcon(errorCount > 0
                ? QStyle::SP_MessageBoxCritical : QStyle::SP_MessageBoxWarning));
    }
    m_statusLabel->setText(QStringLiteral("检测完成。"));
}

void SystemDoctorDialog::addRow(const QString &category,
                                const QString &name,
                                const QString &status,
                                const QString &detail,
                                const QString &tone,
                                AiTool *actionTool,
                                bool installed,
                                const QString &passiveAction,
                                bool repair)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(category));
    m_table->setItem(row, 1, new QTableWidgetItem(name));

    auto *statusCell = new QWidget(m_table);
    auto *statusLayout = new QHBoxLayout(statusCell);
    statusLayout->setContentsMargins(4, 6, 4, 6);
    statusLayout->setSpacing(0);
    auto *statusBadge = new StatusBadge(statusCell);
    StatusBadge::Tone badgeTone = StatusBadge::Tone::Neutral;
    QStyle::StandardPixmap badgeIcon = QStyle::SP_MessageBoxInformation;
    if (tone == QStringLiteral("ok")) {
        badgeTone = StatusBadge::Tone::Success;
        badgeIcon = QStyle::SP_DialogApplyButton;
    } else if (tone == QStringLiteral("error")) {
        badgeTone = StatusBadge::Tone::Error;
        badgeIcon = QStyle::SP_MessageBoxCritical;
    } else if (tone == QStringLiteral("warning")) {
        badgeTone = StatusBadge::Tone::Warning;
        badgeIcon = QStyle::SP_MessageBoxWarning;
    }
    statusBadge->setState(status, badgeTone, style()->standardIcon(badgeIcon));
    statusLayout->addWidget(statusBadge);
    statusLayout->addStretch();
    m_table->setCellWidget(row, 2, statusCell);
    auto *detailItem = new QTableWidgetItem(detail);
    detailItem->setToolTip(detail);
    m_table->setItem(row, 3, detailItem);

    if (actionTool) {
        const AiTool tool = *actionTool;
        auto *button = new QPushButton(
            repair ? QStringLiteral("修复")
                   : (installed ? QStringLiteral("更新")
                                : QStringLiteral("安装")), m_table);
        button->setIcon(style()->standardIcon(
            repair || installed ? QStyle::SP_BrowserReload
                                : QStyle::SP_DialogApplyButton));
        button->setStyleSheet(installed ? AppTheme::secondaryButtonStyle()
                                        : AppTheme::primaryButtonStyle());
        button->setFixedSize(68, 30);
        connect(button, &QPushButton::clicked, this,
                [this, tool, installed, repair]() {
                    installOrUpdate(tool, installed, repair);
                });
        m_table->setCellWidget(row, 4, button);
    } else if (!passiveAction.isEmpty()) {
        auto *actionItem = new QTableWidgetItem(passiveAction);
        actionItem->setForeground(QColor(QStringLiteral("#667085")));
        m_table->setItem(row, 4, actionItem);
    }
    m_table->setRowHeight(row, 42);
}

void SystemDoctorDialog::installOrUpdate(AiTool tool, bool installed, bool repair)
{
    const QString action = repair ? QStringLiteral("修复")
        : (installed ? QStringLiteral("更新") : QStringLiteral("安装"));
#ifdef Q_OS_WIN
    if (tool == AiTool::CodexCli && m_toolManager->isCliRunning(tool)) {
        QMessageBox::warning(
            this,
            QStringLiteral("请先关闭 %1").arg(ToolManager::toolName(tool)),
            QStringLiteral("检测到 %1 仍在运行。Windows 会锁定其程序文件，无法就地%2。\n\n"
                           "请关闭所有相关桌面窗口和终端后再试。")
                .arg(ToolManager::toolName(tool), action));
        return;
    }
#endif
    const QString command = QStringLiteral("npm install -g %1@latest")
        .arg(ToolManager::npmPackage(tool));
    if (QMessageBox::question(
            this,
            QStringLiteral("%1 %2").arg(action, ToolManager::toolName(tool)),
            QStringLiteral("将执行：\n%1\n\n完成后会重新检测本地环境。")
                .arg(command),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_refreshButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("正在%1 %2...")
        .arg(action, ToolManager::toolName(tool)));
    m_toolManager->install(tool);
}

void SystemDoctorDialog::onInstallFinished(AiTool tool, int, bool success)
{
    m_statusLabel->setText(success
        ? QStringLiteral("%1 操作完成，正在复检...").arg(ToolManager::toolName(tool))
        : QStringLiteral("%1 操作失败，请查看主界面活动记录。")
            .arg(ToolManager::toolName(tool)));
    if (success) {
        QTimer::singleShot(300, this, &SystemDoctorDialog::refreshReport);
    } else {
        m_refreshButton->setEnabled(true);
    }
}

void SystemDoctorDialog::onInstallOutput(AiTool tool, const QString &line)
{
    const QString compact = line.trimmed().split(QLatin1Char('\n')).last();
    if (!compact.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("%1：%2")
            .arg(ToolManager::toolName(tool), compact));
    }
}
