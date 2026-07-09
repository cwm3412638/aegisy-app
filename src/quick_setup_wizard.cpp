#include "quick_setup_wizard.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

static const QString kFixedBaseUrl = "https://www.aegisy.cc/v1";

QuickSetupWizard::QuickSetupWizard(ApiClient *apiClient,
                                   ConfigManager *configManager,
                                   EnvDetector *envDetector,
                                   QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
    , m_configManager(configManager)
    , m_envDetector(envDetector)
    , m_baseUrl(kFixedBaseUrl)
    , m_setupInProgress(false)
{
    setupUi();
    setWindowTitle("快速配置向导");
    resize(760, 720);

    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &QuickSetupWizard::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::apiKeyTested, this, &QuickSetupWizard::onApiKeyTested);
    connect(m_apiClient, &ApiClient::requestFailed, this, &QuickSetupWizard::onRequestFailed);

    populateAppList();
    loadApiKeys();
}

void QuickSetupWizard::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(24, 24, 24, 24);

    // Title
    QLabel *titleLabel = new QLabel("🚀 快速配置 — 一键接入 Aegisy", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(17);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setStyleSheet("color: #2c3e50;");
    mainLayout->addWidget(titleLabel);

    QLabel *subtitle = new QLabel(
        "① 勾选要接入的应用 → ② 选择一个可用的 API Key → ③ 开始接入", this);
    subtitle->setStyleSheet("color: #7f8c8d;");
    mainLayout->addWidget(subtitle);

    // ① 应用选择
    QGroupBox *appGroup = new QGroupBox("① 选择要接入的应用", this);
    QVBoxLayout *appLayout = new QVBoxLayout(appGroup);
    appLayout->setSpacing(8);

    auto makeAppRow = [&](QCheckBox *&checkBox, const QString &name) {
        QHBoxLayout *row = new QHBoxLayout();
        checkBox = new QCheckBox(name, this);
        row->addWidget(checkBox);
        row->addStretch();
        appLayout->addLayout(row);
    };
    makeAppRow(m_claudeCheck, "Claude Desktop");
    makeAppRow(m_cursorCheck, "Cursor 编辑器");
    makeAppRow(m_continueCheck, "Continue.dev");
    makeAppRow(m_codexCheck, "Codex CLI");

    mainLayout->addWidget(appGroup);

    // ② API Key 选择
    QGroupBox *keyGroup = new QGroupBox("② 选择 API Key（自动检测可用性）", this);
    QVBoxLayout *keyLayout = new QVBoxLayout(keyGroup);

    m_keyTable = new QTableWidget(this);
    m_keyTable->setColumnCount(3);
    m_keyTable->setHorizontalHeaderLabels({"名称", "Key", "状态"});
    m_keyTable->horizontalHeader()->setStretchLastSection(true);
    m_keyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_keyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_keyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_keyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_keyTable->setAlternatingRowColors(true);
    m_keyTable->verticalHeader()->setVisible(false);
    m_keyTable->setMinimumHeight(160);
    keyLayout->addWidget(m_keyTable);

    mainLayout->addWidget(keyGroup);

    // ③ 接入 + 进度 + 日志
    QGroupBox *runGroup = new QGroupBox("③ 接入", this);
    QVBoxLayout *runLayout = new QVBoxLayout(runGroup);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    runLayout->addWidget(m_progressBar);

    m_statusLabel = new QLabel("准备就绪", this);
    m_statusLabel->setStyleSheet("color: #7f8c8d; font-weight: bold;");
    runLayout->addWidget(m_statusLabel);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(140);
    m_logOutput->setStyleSheet(
        "QTextEdit {"
        "  background-color: #f8f9fa;"
        "  border: 1px solid #ddd;"
        "  border-radius: 4px;"
        "  font-family: monospace;"
        "  font-size: 11px;"
        "}"
    );
    runLayout->addWidget(m_logOutput);

    mainLayout->addWidget(runGroup);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();

    m_skipButton = new QPushButton("跳过", this);
    m_skipButton->setMinimumHeight(38);
    buttonLayout->addWidget(m_skipButton);

    buttonLayout->addStretch();

    m_startButton = new QPushButton("🚀 开始接入", this);
    m_startButton->setMinimumHeight(38);
    m_startButton->setMinimumWidth(180);
    m_startButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #27ae60;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 5px;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background-color: #229954; }"
        "QPushButton:disabled { background-color: #bdc3c7; }"
    );
    buttonLayout->addWidget(m_startButton);

    m_closeButton = new QPushButton("完成", this);
    m_closeButton->setMinimumHeight(38);
    m_closeButton->setMinimumWidth(120);
    m_closeButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #3498db;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 5px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background-color: #2980b9; }"
    );
    m_closeButton->setVisible(false);
    buttonLayout->addWidget(m_closeButton);

    mainLayout->addLayout(buttonLayout);

    connect(m_startButton, &QPushButton::clicked, this, &QuickSetupWizard::onStartAccess);
    connect(m_skipButton, &QPushButton::clicked, this, &QuickSetupWizard::onSkipSetup);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_keyTable, &QTableWidget::itemSelectionChanged, this, &QuickSetupWizard::onKeySelectionChanged);
}

void QuickSetupWizard::populateAppList()
{
    // 显示各应用安装状态：已安装默认勾选，未安装仍可勾选配置
    struct AppRow { QCheckBox *cb; QString name; bool installed; };
    const QList<AppRow> rows = {
        { m_claudeCheck,   QStringLiteral("Claude Desktop"), m_envDetector->isClaudeInstalled() },
        { m_cursorCheck,   QStringLiteral("Cursor 编辑器"),  m_envDetector->isCursorInstalled() },
        { m_continueCheck, QStringLiteral("Continue.dev"),   m_envDetector->isContinueInstalled() },
        { m_codexCheck,    QStringLiteral("Codex CLI"),      m_envDetector->isCodexCliInstalled() },
    };

    for (const AppRow &r : rows) {
        r.cb->setChecked(r.installed);
        const QString base = r.name;
        if (r.installed) {
            r.cb->setText(base + "  —  ✓ 已安装");
            r.cb->setStyleSheet("color: #27ae60;");
        } else {
            r.cb->setText(base + "  —  ✗ 未安装（仍可配置）");
            r.cb->setStyleSheet("color: #95a5a6;");
        }
    }
}

void QuickSetupWizard::loadApiKeys()
{
    logMessage("正在获取账号 API Key 并检测可用性...", "#3498db");
    m_apiClient->getApiKeys();
}

void QuickSetupWizard::onApiKeysReceived(const QJsonArray &keys)
{
    m_keyTable->setRowCount(0);
    m_keyValue.clear();
    m_keySupported.clear();
    m_keyRow.clear();
    m_selectedKeyId.clear();

    int row = 0;
    for (const QJsonValue &val : keys) {
        const QJsonObject obj = val.toObject();
        const QString key = obj["key"].toString();
        if (key.isEmpty()) {
            continue;
        }
        const QString id = QString::number(obj["id"].toInt());
        const QString name = obj["name"].toString();

        QString masked = key;
        if (masked.length() > 12) {
            masked = masked.left(8) + "..." + masked.right(4);
        }

        m_keyTable->insertRow(row);

        QTableWidgetItem *nameItem = new QTableWidgetItem(name.isEmpty() ? "(未命名)" : name);
        nameItem->setData(Qt::UserRole, id);   // 行 -> keyId
        m_keyTable->setItem(row, 0, nameItem);
        m_keyTable->setItem(row, 1, new QTableWidgetItem(masked));

        QTableWidgetItem *statusItem = new QTableWidgetItem("⏳ 测试中…");
        statusItem->setForeground(QBrush(QColor("#7f8c8d")));
        m_keyTable->setItem(row, 2, statusItem);

        m_keyValue.insert(id, key);
        m_keyRow.insert(id, row);

        // 触发可用性测试
        m_apiClient->testApiKey(id, key);

        row++;
    }

    if (row == 0) {
        logMessage("✗ 未找到可用的 API Key，请先在 aegisy.cc 创建", "#e74c3c");
        m_statusLabel->setText("未找到 API Key");
        return;
    }

    logMessage(QString("✓ 找到 %1 个 API Key，正在逐个检测可用性...").arg(row), "#27ae60");
}

void QuickSetupWizard::onApiKeyTested(const QString &keyId, bool supported, const QString &detail)
{
    m_keySupported.insert(keyId, supported);

    const int row = m_keyRow.value(keyId, -1);
    if (row >= 0) {
        QTableWidgetItem *statusItem = m_keyTable->item(row, 2);
        if (statusItem) {
            statusItem->setText((supported ? "✓ 支持  " : "✗ 不支持  ") + detail);
            statusItem->setForeground(QBrush(QColor(supported ? "#27ae60" : "#e74c3c")));
        }
    }

    // 首个可用 Key 自动选中
    if (supported && m_selectedKeyId.isEmpty() && row >= 0) {
        m_keyTable->selectRow(row);   // 触发 onKeySelectionChanged
    }
}

void QuickSetupWizard::onKeySelectionChanged()
{
    const int row = m_keyTable->currentRow();
    if (row < 0) {
        m_selectedKeyId.clear();
        return;
    }
    QTableWidgetItem *nameItem = m_keyTable->item(row, 0);
    if (nameItem) {
        m_selectedKeyId = nameItem->data(Qt::UserRole).toString();
    }
}

QString QuickSetupWizard::selectedApiKey() const
{
    return m_keyValue.value(m_selectedKeyId);
}

void QuickSetupWizard::onStartAccess()
{
    if (m_setupInProgress) return;

    // 至少勾选一个应用
    if (!m_claudeCheck->isChecked() && !m_cursorCheck->isChecked() &&
        !m_continueCheck->isChecked() && !m_codexCheck->isChecked()) {
        QMessageBox::warning(this, "未选择应用", "请至少勾选一个要接入的应用。");
        return;
    }

    // 必须选中一个 Key
    const QString apiKey = selectedApiKey();
    if (apiKey.isEmpty()) {
        QMessageBox::warning(this, "未选择 API Key", "请在列表中选择一个 API Key。");
        return;
    }

    // 选中的 Key 若测试未通过，二次确认
    if (!m_keySupported.value(m_selectedKeyId, false)) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "该 Key 可能不可用",
            "所选 API Key 的可用性检测未通过，仍要用它接入吗？",
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
    }

    m_setupInProgress = true;
    m_startButton->setEnabled(false);
    m_skipButton->setEnabled(false);

    applyConfiguration();
}

void QuickSetupWizard::applyConfiguration()
{
    const QString apiKey = selectedApiKey();
    const QString baseUrl = kFixedBaseUrl;

    struct Target { QCheckBox *cb; QString name; std::function<bool()> write; };
    const QList<Target> targets = {
        { m_claudeCheck,   "Claude Desktop", [&]{ return m_configManager->writeClaudeConfig(apiKey, baseUrl); } },
        { m_cursorCheck,   "Cursor",         [&]{ return m_configManager->writeCursorConfig(apiKey, baseUrl); } },
        { m_continueCheck, "Continue.dev",   [&]{ return m_configManager->writeContinueConfig(apiKey, baseUrl); } },
        { m_codexCheck,    "Codex CLI",      [&]{ return m_configManager->writeCodexConfig(apiKey, baseUrl); } },
    };

    int total = 0;
    for (const Target &t : targets) {
        if (t.cb->isChecked()) total++;
    }

    logMessage("========================================");
    logMessage("开始接入...", "#3498db");

    int done = 0;
    bool allOk = true;
    for (const Target &t : targets) {
        if (!t.cb->isChecked()) continue;

        logMessage(QString("🔧 正在配置 %1 ...").arg(t.name));
        const bool ok = t.write();
        if (ok) {
            logMessage(QString("   ✓ %1 已接入").arg(t.name), "#27ae60");
        } else {
            logMessage(QString("   ✗ %1 配置写入失败").arg(t.name), "#e74c3c");
            allOk = false;
        }

        done++;
        updateProgress(total > 0 ? (done * 100 / total) : 100,
                       QString("配置中 %1/%2 ...").arg(done).arg(total));
    }

    if (allOk) {
        showSuccess();
    } else {
        updateProgress(100, "⚠️ 部分应用配置失败");
        m_statusLabel->setStyleSheet("color: #e67e22; font-weight: bold;");
        logMessage("部分应用未能写入配置，请查看上方日志。", "#e67e22");
        m_startButton->setEnabled(true);
        m_skipButton->setEnabled(true);
        m_setupInProgress = false;
        m_closeButton->setVisible(true);
        emit setupCompleted();
    }
}

void QuickSetupWizard::showSuccess()
{
    updateProgress(100, "✅ 接入完成！");
    m_statusLabel->setStyleSheet("color: #27ae60; font-weight: bold;");

    logMessage("========================================");
    logMessage("🎉 所有选中的应用已成功接入 Aegisy！", "#27ae60");
    logMessage("💡 提示：请重启对应应用（Codex 重开终端）以使配置生效。", "#f39c12");

    m_startButton->setVisible(false);
    m_skipButton->setVisible(false);
    m_closeButton->setVisible(true);

    emit setupCompleted();
}

void QuickSetupWizard::onSkipSetup()
{
    reject();
}

void QuickSetupWizard::onRequestFailed(const QString &error)
{
    logMessage(QString("✗ API 请求失败：%1").arg(error), "#e74c3c");
    m_statusLabel->setText("获取 API Key 失败");
}

void QuickSetupWizard::showError(const QString &error)
{
    updateProgress(0, "❌ 失败");
    logMessage(error, "#e74c3c");
    m_startButton->setEnabled(true);
    m_skipButton->setEnabled(true);
    m_setupInProgress = false;
    QMessageBox::critical(this, "失败", error);
}

void QuickSetupWizard::updateProgress(int value, const QString &status)
{
    m_progressBar->setValue(value);
    m_statusLabel->setText(status);
}

void QuickSetupWizard::logMessage(const QString &message, const QString &color)
{
    QString html = QString("<span style='color: %1;'>%2</span>").arg(color, message);
    m_logOutput->append(html);
}
