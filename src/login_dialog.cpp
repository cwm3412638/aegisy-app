#include "login_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle("Aegisy 客户端 - 登录");
    resize(400, 250);
}

void LoginDialog::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(30, 30, 30, 30);

    // Logo/Title
    QLabel *titleLabel = new QLabel("Aegisy Client", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    // Subtitle
    QLabel *subtitleLabel = new QLabel("Login to your account", this);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setStyleSheet("color: #666;");
    mainLayout->addWidget(subtitleLabel);

    mainLayout->addSpacing(10);

    // Form
    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(10);

    m_emailEdit = new QLineEdit(this);
    m_emailEdit->setPlaceholderText("your@email.com");
    m_emailEdit->setMinimumHeight(35);
    formLayout->addRow("邮箱:", m_emailEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("Password");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMinimumHeight(35);
    formLayout->addRow("密码:", m_passwordEdit);

    mainLayout->addLayout(formLayout);

    // Remember me
    m_rememberCheckBox = new QCheckBox("Remember me", this);
    mainLayout->addWidget(m_rememberCheckBox);

    // Error label
    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet("color: #e74c3c; font-size: 12px;");
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    // Status label
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #3498db; font-size: 12px;");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->hide();
    mainLayout->addWidget(m_statusLabel);

    // Login button
    m_loginButton = new QPushButton("登录", this);
    m_loginButton->setMinimumHeight(40);
    m_loginButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #3498db;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 5px;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "  background-color: #2980b9;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #21618c;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #bdc3c7;"
        "}"
    );
    mainLayout->addWidget(m_loginButton);

    mainLayout->addStretch();

    // Connections
    connect(m_loginButton, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &LoginDialog::onLoginClicked);
}

QString LoginDialog::getEmail() const
{
    return m_emailEdit->text();
}

QString LoginDialog::getPassword() const
{
    return m_passwordEdit->text();
}

bool LoginDialog::shouldRememberMe() const
{
    return m_rememberCheckBox->isChecked();
}

void LoginDialog::setEmail(const QString &email)
{
    m_emailEdit->setText(email);
}

void LoginDialog::showError(const QString &message)
{
    m_errorLabel->setText(message);
    m_errorLabel->show();
    m_statusLabel->hide();
}

void LoginDialog::setLoading(bool loading)
{
    m_loginButton->setEnabled(!loading);
    m_emailEdit->setEnabled(!loading);
    m_passwordEdit->setEnabled(!loading);
    m_rememberCheckBox->setEnabled(!loading);

    if (loading) {
        m_errorLabel->hide();
        m_statusLabel->setText("登录中...");
        m_statusLabel->show();
    } else {
        m_statusLabel->hide();
    }
}

void LoginDialog::onLoginClicked()
{
    QString email = m_emailEdit->text().trimmed();
    QString password = m_passwordEdit->text();

    if (email.isEmpty()) {
        showError("Please enter your email");
        return;
    }

    if (password.isEmpty()) {
        showError("Please enter your password");
        return;
    }

    m_errorLabel->hide();
    emit loginRequested(email, password);
}
