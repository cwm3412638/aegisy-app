#include "login_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle("Aegisy 客户端 - 登录");
    resize(420, 520);
    // 禁用系统最大化按钮，保持对话框风格
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
}

void LoginDialog::setupUi()
{
    // 整体背景
    setStyleSheet("QDialog { background-color: #f1f5f9; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(44, 44, 44, 44);

    // ── Logo 徽标 ──────────────────────────────────────────
    QLabel *logoLabel = new QLabel("A", this);
    logoLabel->setAlignment(Qt::AlignCenter);
    logoLabel->setFixedSize(68, 68);
    logoLabel->setStyleSheet(
        "QLabel {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white;"
        "  border-radius: 34px;"
        "  font-size: 30px;"
        "  font-weight: bold;"
        "}"
    );

    QHBoxLayout *logoLayout = new QHBoxLayout();
    logoLayout->addStretch();
    logoLayout->addWidget(logoLabel);
    logoLayout->addStretch();
    mainLayout->addLayout(logoLayout);

    mainLayout->addSpacing(18);

    // ── 标题 ───────────────────────────────────────────────
    QLabel *titleLabel = new QLabel("Aegisy Client", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("color: #1e293b;");
    mainLayout->addWidget(titleLabel);

    mainLayout->addSpacing(6);

    // ── 副标题 ─────────────────────────────────────────────
    QLabel *subtitleLabel = new QLabel("请使用您的 Aegisy 账号登录", this);
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setStyleSheet("color: #64748b; font-size: 13px;");
    mainLayout->addWidget(subtitleLabel);

    mainLayout->addSpacing(32);

    // ── 邮箱 ───────────────────────────────────────────────
    QLabel *emailLabel = new QLabel("邮箱", this);
    emailLabel->setStyleSheet(
        "color: #374151; font-size: 13px; font-weight: bold;"
    );
    mainLayout->addWidget(emailLabel);

    mainLayout->addSpacing(6);

    m_emailEdit = new QLineEdit(this);
    m_emailEdit->setPlaceholderText("your@email.com");
    m_emailEdit->setMinimumHeight(42);
    m_emailEdit->setStyleSheet(
        "QLineEdit {"
        "  background-color: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "  padding: 0 14px;"
        "  font-size: 14px;"
        "  color: #1e293b;"
        "}"
        "QLineEdit:focus {"
        "  border-color: #6366f1;"
        "  background-color: #fafaff;"
        "}"
    );
    mainLayout->addWidget(m_emailEdit);

    mainLayout->addSpacing(16);

    // ── 密码 ───────────────────────────────────────────────
    QLabel *passwordLabel = new QLabel("密码", this);
    passwordLabel->setStyleSheet(
        "color: #374151; font-size: 13px; font-weight: bold;"
    );
    mainLayout->addWidget(passwordLabel);

    mainLayout->addSpacing(6);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText("请输入密码");
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMinimumHeight(42);
    m_passwordEdit->setStyleSheet(
        "QLineEdit {"
        "  background-color: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "  padding: 0 14px;"
        "  font-size: 14px;"
        "  color: #1e293b;"
        "}"
        "QLineEdit:focus {"
        "  border-color: #6366f1;"
        "  background-color: #fafaff;"
        "}"
    );
    mainLayout->addWidget(m_passwordEdit);

    mainLayout->addSpacing(12);

    // ── 记住我 ─────────────────────────────────────────────
    m_rememberCheckBox = new QCheckBox("记住我", this);
    m_rememberCheckBox->setStyleSheet(
        "QCheckBox {"
        "  color: #64748b;"
        "  font-size: 13px;"
        "  spacing: 8px;"
        "}"
        "QCheckBox::indicator {"
        "  width: 16px;"
        "  height: 16px;"
        "  border: 1.5px solid #cbd5e1;"
        "  border-radius: 4px;"
        "  background: white;"
        "}"
        "QCheckBox::indicator:checked {"
        "  background-color: #6366f1;"
        "  border-color: #6366f1;"
        "}"
        "QCheckBox::indicator:hover {"
        "  border-color: #6366f1;"
        "}"
    );
    mainLayout->addWidget(m_rememberCheckBox);

    mainLayout->addSpacing(10);

    // ── 错误提示 ───────────────────────────────────────────
    m_errorLabel = new QLabel(this);
    m_errorLabel->setStyleSheet(
        "QLabel {"
        "  color: #dc2626;"
        "  font-size: 12px;"
        "  background-color: #fef2f2;"
        "  border: 1px solid #fecaca;"
        "  border-radius: 7px;"
        "  padding: 8px 12px;"
        "}"
    );
    m_errorLabel->setWordWrap(true);
    m_errorLabel->hide();
    mainLayout->addWidget(m_errorLabel);

    // ── 状态提示 ───────────────────────────────────────────
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(
        "QLabel {"
        "  color: #4f46e5;"
        "  font-size: 12px;"
        "  background-color: #eef2ff;"
        "  border: 1px solid #c7d2fe;"
        "  border-radius: 7px;"
        "  padding: 8px 12px;"
        "}"
    );
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->hide();
    mainLayout->addWidget(m_statusLabel);

    mainLayout->addSpacing(20);

    // ── 登录按钮 ───────────────────────────────────────────
    m_loginButton = new QPushButton("登 录", this);
    m_loginButton->setMinimumHeight(44);
    m_loginButton->setCursor(Qt::PointingHandCursor);
    m_loginButton->setStyleSheet(
        "QPushButton {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white;"
        "  border: none;"
        "  border-radius: 8px;"
        "  font-size: 15px;"
        "  font-weight: bold;"
        "  letter-spacing: 3px;"
        "}"
        "QPushButton:hover {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #4f46e5, stop:1 #7c3aed);"
        "}"
        "QPushButton:pressed {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #4338ca, stop:1 #6d28d9);"
        "}"
        "QPushButton:disabled {"
        "  background: #e2e8f0;"
        "  color: #94a3b8;"
        "  letter-spacing: 0;"
        "}"
    );
    mainLayout->addWidget(m_loginButton);

    mainLayout->addStretch();

    // ── 信号连接 ───────────────────────────────────────────
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
        m_statusLabel->setText("登录中，请稍候…");
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
        showError("请输入邮箱地址");
        m_emailEdit->setFocus();
        return;
    }

    if (password.isEmpty()) {
        showError("请输入密码");
        m_passwordEdit->setFocus();
        return;
    }

    m_errorLabel->hide();
    emit loginRequested(email, password);
}
