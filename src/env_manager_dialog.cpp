#include "env_manager_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QGroupBox>
#include <QUuid>

// ============================================================================
// EnvManagerDialog 实现
// ============================================================================

EnvManagerDialog::EnvManagerDialog(ConfigManager *configManager, QWidget *parent)
    : QDialog(parent)
    , m_configManager(configManager)
{
    setupUi();
    setWindowTitle("Environment Manager");
    resize(800, 600);

    loadEnvironments();
}

void EnvManagerDialog::setupUi()
{
    QHBoxLayout *mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Left Panel - Environment List
    QVBoxLayout *leftLayout = new QVBoxLayout();

    QLabel *titleLabel = new QLabel("Environments", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    leftLayout->addWidget(titleLabel);

    m_envList = new QListWidget(this);
    m_envList->setMinimumWidth(250);
    m_envList->setStyleSheet(
        "QListWidget {"
        "  border: 1px solid #ddd;"
        "  border-radius: 4px;"
        "  background-color: white;"
        "}"
        "QListWidget::item {"
        "  padding: 10px;"
        "  border-bottom: 1px solid #f0f0f0;"
        "}"
        "QListWidget::item:selected {"
        "  background-color: #e3f2fd;"
        "  color: #1976d2;"
        "}"
        "QListWidget::item:hover {"
        "  background-color: #f5f5f5;"
        "}"
    );
    leftLayout->addWidget(m_envList);

    // Left Panel Buttons
    QHBoxLayout *leftButtonLayout = new QHBoxLayout();

    m_addButton = new QPushButton("➕ Add", this);
    m_addButton->setMinimumHeight(35);
    leftButtonLayout->addWidget(m_addButton);

    m_editButton = new QPushButton("✏️ Edit", this);
    m_editButton->setMinimumHeight(35);
    m_editButton->setEnabled(false);
    leftButtonLayout->addWidget(m_editButton);

    m_deleteButton = new QPushButton("🗑️ Delete", this);
    m_deleteButton->setMinimumHeight(35);
    m_deleteButton->setEnabled(false);
    leftButtonLayout->addWidget(m_deleteButton);

    leftLayout->addLayout(leftButtonLayout);

    mainLayout->addLayout(leftLayout);

    // Right Panel - Details and Actions
    QVBoxLayout *rightLayout = new QVBoxLayout();

    QLabel *detailsTitle = new QLabel("Environment Details", this);
    QFont detailsTitleFont = detailsTitle->font();
    detailsTitleFont.setPointSize(14);
    detailsTitleFont.setBold(true);
    detailsTitle->setFont(detailsTitleFont);
    rightLayout->addWidget(detailsTitle);

    // Details Display
    m_detailsLabel = new QLabel(this);
    m_detailsLabel->setWordWrap(true);
    m_detailsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_detailsLabel->setMinimumHeight(200);
    m_detailsLabel->setStyleSheet(
        "QLabel {"
        "  background-color: #f8f9fa;"
        "  border: 1px solid #ddd;"
        "  border-radius: 4px;"
        "  padding: 15px;"
        "}"
    );
    m_detailsLabel->setText("<i>Select an environment to view details</i>");
    rightLayout->addWidget(m_detailsLabel);

    rightLayout->addSpacing(20);

    // Action Buttons
    m_activateButton = new QPushButton("✓ Activate Environment", this);
    m_activateButton->setMinimumHeight(45);
    m_activateButton->setEnabled(false);
    m_activateButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #27ae60;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #229954;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #bdc3c7;"
        "}"
    );
    rightLayout->addWidget(m_activateButton);

    // Status Label
    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #666; font-size: 12px;");
    rightLayout->addWidget(m_statusLabel);

    rightLayout->addStretch();

    // Close Button
    m_closeButton = new QPushButton("Close", this);
    m_closeButton->setMinimumHeight(35);
    rightLayout->addWidget(m_closeButton);

    mainLayout->addLayout(rightLayout);

    // Connections
    connect(m_addButton, &QPushButton::clicked, this, &EnvManagerDialog::onAddClicked);
    connect(m_editButton, &QPushButton::clicked, this, &EnvManagerDialog::onEditClicked);
    connect(m_deleteButton, &QPushButton::clicked, this, &EnvManagerDialog::onDeleteClicked);
    connect(m_activateButton, &QPushButton::clicked, this, &EnvManagerDialog::onActivateClicked);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_envList, &QListWidget::itemSelectionChanged, this, &EnvManagerDialog::onEnvSelectionChanged);
    connect(m_envList, &QListWidget::itemDoubleClicked, this, &EnvManagerDialog::onEnvDoubleClicked);
}

void EnvManagerDialog::loadEnvironments()
{
    m_environments = m_configManager->getEnvironments();
    updateEnvList();

    if (m_environments.isEmpty()) {
        m_statusLabel->setText("No environments configured. Click 'Add' to create one.");
        m_statusLabel->setStyleSheet("color: #f39c12; font-size: 12px;");
    }
}

void EnvManagerDialog::updateEnvList()
{
    m_envList->clear();

    for (const Environment &env : m_environments) {
        QString displayName = env.name;
        if (env.active) {
            displayName = "★ " + displayName;
        }

        QListWidgetItem *item = new QListWidgetItem(displayName);
        item->setData(Qt::UserRole, env.id);

        if (env.active) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
            item->setForeground(QBrush(QColor("#27ae60")));
        }

        m_envList->addItem(item);
    }
}

void EnvManagerDialog::onEnvSelectionChanged()
{
    Environment env = getSelectedEnvironment();
    bool hasSelection = !env.id.isEmpty();

    m_editButton->setEnabled(hasSelection);
    m_deleteButton->setEnabled(hasSelection);
    m_activateButton->setEnabled(hasSelection && !env.active);

    if (hasSelection) {
        // Display environment details
        QString details;
        details += QString("<b>Name:</b> %1<br>").arg(env.name);
        details += QString("<b>Status:</b> %1<br>").arg(env.active ? "★ Active" : "Inactive");
        details += "<br>";

        details += QString("<b>API Key:</b> %1<br>").arg(
            env.activeKey.isEmpty() ?
            (env.apiKeys.isEmpty() ? "<i>Not set</i>" : env.apiKeys.first().left(12) + "...") :
            env.activeKey.left(12) + "...");

        details += QString("<b>Base URL:</b> %1<br>").arg(
            env.baseUrl.isEmpty() ? "<i>Not set</i>" : env.baseUrl);
        details += "<br>";

        details += "<b>Target Applications:</b><br>";
        if (env.targets.isEmpty()) {
            details += "<i>None selected</i>";
        } else {
            for (const QString &target : env.targets) {
                details += QString("  • %1<br>").arg(target);
            }
        }

        m_detailsLabel->setText(details);
    } else {
        m_detailsLabel->setText("<i>Select an environment to view details</i>");
    }
}

void EnvManagerDialog::onEnvDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    onEditClicked();
}

Environment EnvManagerDialog::getSelectedEnvironment() const
{
    int currentRow = m_envList->currentRow();
    if (currentRow >= 0 && currentRow < m_environments.size()) {
        return m_environments[currentRow];
    }
    return Environment();
}

void EnvManagerDialog::onAddClicked()
{
    Environment newEnv;
    newEnv.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    newEnv.active = false;
    newEnv.baseUrl = "https://www.aegisy.cc/v1";

    showEnvEditor(newEnv, false);
}

void EnvManagerDialog::onEditClicked()
{
    Environment env = getSelectedEnvironment();
    if (env.id.isEmpty()) {
        return;
    }

    showEnvEditor(env, true);
}

void EnvManagerDialog::onDeleteClicked()
{
    Environment env = getSelectedEnvironment();
    if (env.id.isEmpty()) {
        return;
    }

    if (env.active) {
        QMessageBox::warning(this, "Cannot Delete",
                           "Cannot delete the active environment.\n"
                           "Please activate another environment first.");
        return;
    }

    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "Confirm Delete",
                                  QString("Are you sure you want to delete the environment '%1'?\n\n"
                                         "This action cannot be undone.")
                                  .arg(env.name),
                                  QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        if (m_configManager->removeEnvironment(env.id)) {
            m_statusLabel->setText(QString("✓ Environment '%1' deleted").arg(env.name));
            m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
            loadEnvironments();
        } else {
            m_statusLabel->setText("✗ Failed to delete environment");
            m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
        }
    }
}

void EnvManagerDialog::onActivateClicked()
{
    Environment env = getSelectedEnvironment();
    if (env.id.isEmpty()) {
        return;
    }

    QMessageBox::StandardButton reply;
    reply = QMessageBox::question(this, "Confirm Activation",
                                  QString("Activate environment '%1'?\n\n"
                                         "This will apply the configuration to selected applications.")
                                  .arg(env.name),
                                  QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        if (m_configManager->setActiveEnvironment(env.id)) {
            m_statusLabel->setText(QString("✓ Environment '%1' activated!").arg(env.name));
            m_statusLabel->setStyleSheet("color: #27ae60; font-weight: bold; font-size: 12px;");

            emit environmentSwitched(env.id);

            loadEnvironments();

            QMessageBox::information(this, "Environment Activated",
                                   QString("Environment '%1' is now active!\n\n"
                                          "Note: You may need to restart your applications "
                                          "for the changes to take effect.")
                                   .arg(env.name));
        } else {
            m_statusLabel->setText("✗ Failed to activate environment");
            m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
        }
    }
}

void EnvManagerDialog::showEnvEditor(const Environment &env, bool isEdit)
{
    EnvEditorDialog *editor = new EnvEditorDialog(env, isEdit, this);

    if (editor->exec() == QDialog::Accepted) {
        Environment newEnv = editor->getEnvironment();

        if (isEdit) {
            if (m_configManager->updateEnvironment(env.id, newEnv)) {
                m_statusLabel->setText(QString("✓ Environment '%1' updated").arg(newEnv.name));
                m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
            } else {
                m_statusLabel->setText("✗ Failed to update environment");
                m_statusLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
            }
        } else {
            m_configManager->addEnvironment(newEnv);
            m_statusLabel->setText(QString("✓ Environment '%1' created").arg(newEnv.name));
            m_statusLabel->setStyleSheet("color: #27ae60; font-size: 12px;");
        }

        loadEnvironments();
    }

    editor->deleteLater();
}

// ============================================================================
// EnvEditorDialog 实现
// ============================================================================

EnvEditorDialog::EnvEditorDialog(const Environment &env, bool isEdit, QWidget *parent)
    : QDialog(parent)
    , m_environment(env)
    , m_isEdit(isEdit)
{
    setupUi();
    setWindowTitle(isEdit ? "Edit Environment" : "New Environment");
    resize(500, 400);
}

void EnvEditorDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Title
    QLabel *titleLabel = new QLabel(m_isEdit ? "Edit Environment" : "Create New Environment", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(14);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    // Form
    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(10);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText("e.g., Production, Development, Testing");
    m_nameEdit->setText(m_environment.name);
    m_nameEdit->setMinimumHeight(35);
    formLayout->addRow("Name:", m_nameEdit);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setPlaceholderText("sk-your-api-key");
    m_apiKeyEdit->setText(m_environment.activeKey.isEmpty() ?
                         (m_environment.apiKeys.isEmpty() ? "" : m_environment.apiKeys.first()) :
                         m_environment.activeKey);
    m_apiKeyEdit->setMinimumHeight(35);
    formLayout->addRow("API Key:", m_apiKeyEdit);

    m_baseUrlEdit = new QLineEdit(this);
    m_baseUrlEdit->setPlaceholderText("https://www.aegisy.cc/v1");
    m_baseUrlEdit->setText(m_environment.baseUrl);
    m_baseUrlEdit->setMinimumHeight(35);
    formLayout->addRow("Base URL:", m_baseUrlEdit);

    mainLayout->addLayout(formLayout);

    // Target Applications
    QGroupBox *targetGroup = new QGroupBox("Target Applications", this);
    QVBoxLayout *targetLayout = new QVBoxLayout(targetGroup);

    m_claudeCheckBox = new QCheckBox("Claude Desktop", this);
    m_claudeCheckBox->setChecked(m_environment.targets.contains("claude"));
    targetLayout->addWidget(m_claudeCheckBox);

    m_cursorCheckBox = new QCheckBox("Cursor Editor", this);
    m_cursorCheckBox->setChecked(m_environment.targets.contains("cursor"));
    targetLayout->addWidget(m_cursorCheckBox);

    m_continueCheckBox = new QCheckBox("Continue.dev", this);
    m_continueCheckBox->setChecked(m_environment.targets.contains("continue"));
    targetLayout->addWidget(m_continueCheckBox);

    mainLayout->addWidget(targetGroup);

    mainLayout->addStretch();

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    QPushButton *cancelButton = new QPushButton("Cancel", this);
    cancelButton->setMinimumHeight(35);
    cancelButton->setMinimumWidth(100);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    QPushButton *saveButton = new QPushButton(m_isEdit ? "Save" : "Create", this);
    saveButton->setMinimumHeight(35);
    saveButton->setMinimumWidth(100);
    saveButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #3498db;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 4px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #2980b9;"
        "}"
    );
    connect(saveButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(saveButton);

    mainLayout->addLayout(buttonLayout);
}

Environment EnvEditorDialog::getEnvironment() const
{
    Environment env = m_environment;

    env.name = m_nameEdit->text().trimmed();

    QString apiKey = m_apiKeyEdit->text().trimmed();
    if (!apiKey.isEmpty()) {
        env.apiKeys.clear();
        env.apiKeys.append(apiKey);
        env.activeKey = apiKey;
    }

    env.baseUrl = m_baseUrlEdit->text().trimmed();

    env.targets.clear();
    if (m_claudeCheckBox->isChecked()) {
        env.targets.append("claude");
    }
    if (m_cursorCheckBox->isChecked()) {
        env.targets.append("cursor");
    }
    if (m_continueCheckBox->isChecked()) {
        env.targets.append("continue");
    }

    return env;
}
