#include "login_dialog.h"
#include "app_theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QString inputStyle()
{
    return QStringLiteral(
        "QLineEdit {"
        "  background: white; color: #182230; border: 1px solid #d0d5dd;"
        "  border-radius: 7px; padding: 0 13px; font-size: 13px;"
        "}"
        "QLineEdit:focus { border: 1px solid #0f766e; }"
        "QLineEdit:disabled { background: #f2f4f7; color: #98a2b3; }");
}

} // namespace

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle(QStringLiteral("登录 Aegisy"));
    resize(460, 560);
    setMinimumSize(430, 530);
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);
}

void LoginDialog::setupUi()
{
    setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f7f9; }"
        "QLabel { color: #182230; }"
        "QToolTip { background: #182230; color: white; border: none; padding: 5px; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(44, 36, 44, 32);
    root->setSpacing(0);

    auto *brandRow = new QHBoxLayout;
    brandRow->setSpacing(12);
    auto *brandMark = new QLabel(QStringLiteral("A"), this);
    brandMark->setFixedSize(44, 44);
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setStyleSheet(QStringLiteral(
        "background: #0f766e; color: white; border-radius: 8px;"
        "font-size: 20px; font-weight: 700;"));
    brandRow->addWidget(brandMark);

    auto *brandColumn = new QVBoxLayout;
    brandColumn->setSpacing(0);
    auto *brandName = new QLabel(QStringLiteral("Aegisy"), this);
    brandName->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: #101828;"));
    auto *brandProduct = new QLabel(QStringLiteral("AI 工具连接管理"), this);
    brandProduct->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
    brandColumn->addWidget(brandName);
    brandColumn->addWidget(brandProduct);
    brandRow->addLayout(brandColumn);
    brandRow->addStretch();
    root->addLayout(brandRow);

    root->addSpacing(38);
    auto *title = new QLabel(QStringLiteral("登录到你的账号"), this);
    title->setStyleSheet(QStringLiteral("font-size: 23px; font-weight: 700; color: #101828;"));
    root->addWidget(title);
    root->addSpacing(6);

    auto *subtitle = new QLabel(QStringLiteral("使用 Aegisy 账号继续管理本地 AI 工具配置"), this);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    root->addWidget(subtitle);

    root->addSpacing(28);
    auto *emailLabel = new QLabel(QStringLiteral("邮箱"), this);
    emailLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    root->addWidget(emailLabel);
    root->addSpacing(6);

    m_emailEdit = new QLineEdit(this);
    m_emailEdit->setPlaceholderText(QStringLiteral("name@example.com"));
    m_emailEdit->setFixedHeight(44);
    m_emailEdit->setStyleSheet(inputStyle());
    root->addWidget(m_emailEdit);

    root->addSpacing(16);
    auto *passwordLabel = new QLabel(QStringLiteral("密码"), this);
    passwordLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #475467;"));
    root->addWidget(passwordLabel);
    root->addSpacing(6);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setPlaceholderText(QStringLiteral("请输入密码"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setFixedHeight(44);
    m_passwordEdit->setStyleSheet(inputStyle());
    root->addWidget(m_passwordEdit);

    root->addSpacing(13);
    m_rememberCheckBox = new QCheckBox(QStringLiteral("记住登录状态"), this);
    m_rememberCheckBox->setStyleSheet(QStringLiteral(
        "QCheckBox { color: #475467; font-size: 12px; spacing: 8px; }"
        "QCheckBox::indicator {"
        "  width: 16px; height: 16px; background: white; border: 1px solid #d0d5dd;"
        "  border-radius: 4px;"
        "}"
        "QCheckBox::indicator:hover { border-color: #0f766e; }"
        "QCheckBox::indicator:checked { background: #0f766e; border-color: #0f766e; }"));
    root->addWidget(m_rememberCheckBox);

    root->addSpacing(12);
    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(QStringLiteral(
        "background: #fef3f2; color: #b42318; border: 1px solid #fecdca;"
        "border-radius: 7px; padding: 9px 11px; font-size: 11px;"));
    m_errorLabel->hide();
    root->addWidget(m_errorLabel);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "background: #f0fdf9; color: #0f5f59; border: 1px solid #b7e4da;"
        "border-radius: 7px; padding: 9px 11px; font-size: 11px;"));
    m_statusLabel->hide();
    root->addWidget(m_statusLabel);

    root->addSpacing(18);
    m_loginButton = new QPushButton(QStringLiteral("登录"), this);
    m_loginButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    m_loginButton->setFixedHeight(44);
    m_loginButton->setCursor(Qt::PointingHandCursor);
    m_loginButton->setStyleSheet(AppTheme::primaryButtonStyle());
    root->addWidget(m_loginButton);

    root->addStretch();
    auto *footer = new QLabel(QStringLiteral("安全连接至 aegisy.cc"), this);
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet(QStringLiteral("font-size: 10px; color: #98a2b3;"));
    root->addWidget(footer);

    connect(m_loginButton, &QPushButton::clicked,
            this, &LoginDialog::onLoginClicked);
    connect(m_passwordEdit, &QLineEdit::returnPressed,
            this, &LoginDialog::onLoginClicked);
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
        m_statusLabel->setText(QStringLiteral("正在登录..."));
        m_statusLabel->show();
    } else {
        m_statusLabel->hide();
    }
}

void LoginDialog::onLoginClicked()
{
    const QString email = m_emailEdit->text().trimmed();
    const QString password = m_passwordEdit->text();

    if (email.isEmpty()) {
        showError(QStringLiteral("请输入邮箱地址"));
        m_emailEdit->setFocus();
        return;
    }
    if (password.isEmpty()) {
        showError(QStringLiteral("请输入密码"));
        m_passwordEdit->setFocus();
        return;
    }

    m_errorLabel->hide();
    emit loginRequested(email, password);
}
