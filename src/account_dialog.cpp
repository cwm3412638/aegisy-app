#include "account_dialog.h"

#include "api_client.h"
#include "app_theme.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace {

QLabel *hintLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    return label;
}

} // namespace

AccountDialog::AccountDialog(ApiClient *apiClient, const QJsonObject &userInfo,
                             QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setWindowTitle(QStringLiteral("账号中心"));
    resize(560, 500);
    setMinimumSize(500, 440);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(14);

    auto *header = new QHBoxLayout;
    auto *avatar = new QLabel(this);
    const QString name = userInfo.value(QStringLiteral("username")).toString().trimmed();
    const QString email = userInfo.value(QStringLiteral("email")).toString().trimmed();
    avatar->setText((!name.isEmpty() ? name : email).left(1).toUpper());
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(46, 46);
    avatar->setStyleSheet(QStringLiteral(
        "background: #0f766e; color: white; border-radius: 23px;"
        "font-size: 18px; font-weight: 700;"));
    header->addWidget(avatar);
    auto *identity = new QVBoxLayout;
    auto *nameLabel = new QLabel(name.isEmpty() ? QStringLiteral("Aegisy 用户") : name, this);
    nameLabel->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: #101828;"));
    identity->addWidget(nameLabel);
    identity->addWidget(hintLabel(email, this));
    header->addLayout(identity, 1);
    m_balanceLabel = new QLabel(QStringLiteral("余额 $%1")
        .arg(userInfo.value(QStringLiteral("balance")).toDouble(), 0, 'f', 2), this);
    m_balanceLabel->setStyleSheet(QStringLiteral(
        "background: #ecfdf3; color: #067647; border: 1px solid #abefc6;"
        "border-radius: 7px; padding: 6px 10px; font-weight: 600;"));
    header->addWidget(m_balanceLabel);
    root->addLayout(header);

    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    auto *passwordPage = new QWidget(tabs);
    auto *passwordLayout = new QVBoxLayout(passwordPage);
    passwordLayout->setContentsMargins(18, 18, 18, 14);
    passwordLayout->setSpacing(12);
    passwordLayout->addWidget(hintLabel(
        QStringLiteral("修改后下次登录使用新密码。为保护账号，请使用至少 8 位且不重复使用的密码。"),
        passwordPage));
    auto *passwordForm = new QFormLayout;
    passwordForm->setVerticalSpacing(12);
    m_oldPassword = new QLineEdit(passwordPage);
    m_oldPassword->setEchoMode(QLineEdit::Password);
    m_oldPassword->setPlaceholderText(QStringLiteral("当前登录密码"));
    passwordForm->addRow(QStringLiteral("当前密码"), m_oldPassword);
    m_newPassword = new QLineEdit(passwordPage);
    m_newPassword->setEchoMode(QLineEdit::Password);
    m_newPassword->setPlaceholderText(QStringLiteral("至少 8 位"));
    passwordForm->addRow(QStringLiteral("新密码"), m_newPassword);
    m_confirmPassword = new QLineEdit(passwordPage);
    m_confirmPassword->setEchoMode(QLineEdit::Password);
    m_confirmPassword->setPlaceholderText(QStringLiteral("再次输入新密码"));
    passwordForm->addRow(QStringLiteral("确认新密码"), m_confirmPassword);
    passwordLayout->addLayout(passwordForm);
    m_showPasswords = new QCheckBox(QStringLiteral("显示密码"), passwordPage);
    passwordLayout->addWidget(m_showPasswords);
    m_passwordStatus = hintLabel(QString(), passwordPage);
    passwordLayout->addWidget(m_passwordStatus);
    auto *passwordActions = new QHBoxLayout;
    passwordActions->addStretch();
    m_passwordButton = new QPushButton(QStringLiteral("修改密码"), passwordPage);
    m_passwordButton->setStyleSheet(AppTheme::primaryButtonStyle());
    m_passwordButton->setMinimumHeight(36);
    passwordActions->addWidget(m_passwordButton);
    passwordLayout->addLayout(passwordActions);
    passwordLayout->addStretch();
    tabs->addTab(passwordPage, QStringLiteral("修改密码"));

    auto *redeemPage = new QWidget(tabs);
    auto *redeemLayout = new QVBoxLayout(redeemPage);
    redeemLayout->setContentsMargins(18, 18, 18, 14);
    redeemLayout->setSpacing(12);
    auto *redeemTitle = new QLabel(QStringLiteral("密卡兑换充值"), redeemPage);
    redeemTitle->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700; color: #101828;"));
    redeemLayout->addWidget(redeemTitle);
    redeemLayout->addWidget(hintLabel(
        QStringLiteral("输入从 Aegisy 官方渠道获得的兑换码。兑换成功后余额或订阅权益会立即到账。"),
        redeemPage));
    m_redeemCode = new QLineEdit(redeemPage);
    m_redeemCode->setPlaceholderText(QStringLiteral("请输入密卡兑换码"));
    m_redeemCode->setClearButtonEnabled(true);
    m_redeemCode->setMinimumHeight(42);
    redeemLayout->addWidget(m_redeemCode);
    m_redeemStatus = hintLabel(QString(), redeemPage);
    redeemLayout->addWidget(m_redeemStatus);
    auto *redeemActions = new QHBoxLayout;
    redeemActions->addStretch();
    m_redeemButton = new QPushButton(QStringLiteral("立即兑换"), redeemPage);
    m_redeemButton->setStyleSheet(AppTheme::primaryButtonStyle());
    m_redeemButton->setMinimumHeight(36);
    redeemActions->addWidget(m_redeemButton);
    redeemLayout->addLayout(redeemActions);
    redeemLayout->addStretch();
    tabs->addTab(redeemPage, QStringLiteral("密卡充值"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(m_showPasswords, &QCheckBox::toggled, this, [this](bool visible) {
        const auto mode = visible ? QLineEdit::Normal : QLineEdit::Password;
        m_oldPassword->setEchoMode(mode);
        m_newPassword->setEchoMode(mode);
        m_confirmPassword->setEchoMode(mode);
    });
    connect(m_passwordButton, &QPushButton::clicked,
            this, &AccountDialog::submitPasswordChange);
    connect(m_redeemButton, &QPushButton::clicked, this, &AccountDialog::submitRedeem);
    connect(m_redeemCode, &QLineEdit::returnPressed, this, &AccountDialog::submitRedeem);
    connect(m_apiClient, &ApiClient::passwordChanged,
            this, &AccountDialog::onPasswordChanged);
    connect(m_apiClient, &ApiClient::passwordChangeFailed,
            this, &AccountDialog::onPasswordChangeFailed);
    connect(m_apiClient, &ApiClient::redeemCompleted,
            this, &AccountDialog::onRedeemCompleted);
    connect(m_apiClient, &ApiClient::redeemFailed,
            this, &AccountDialog::onRedeemFailed);
}

void AccountDialog::submitPasswordChange()
{
    const QString oldPassword = m_oldPassword->text();
    const QString newPassword = m_newPassword->text();
    if (oldPassword.isEmpty()) {
        m_passwordStatus->setText(QStringLiteral("请输入当前密码。"));
        return;
    }
    if (newPassword.size() < 8) {
        m_passwordStatus->setText(QStringLiteral("新密码不能少于 8 位。"));
        return;
    }
    if (newPassword != m_confirmPassword->text()) {
        m_passwordStatus->setText(QStringLiteral("两次输入的新密码不一致。"));
        return;
    }
    m_passwordButton->setEnabled(false);
    m_passwordButton->setText(QStringLiteral("修改中..."));
    m_passwordStatus->setText(QStringLiteral("正在验证当前密码..."));
    m_apiClient->changePassword(oldPassword, newPassword);
}

void AccountDialog::submitRedeem()
{
    const QString code = m_redeemCode->text().trimmed();
    if (code.isEmpty()) {
        m_redeemStatus->setText(QStringLiteral("请输入密卡兑换码。"));
        return;
    }
    m_redeemButton->setEnabled(false);
    m_redeemButton->setText(QStringLiteral("兑换中..."));
    m_redeemStatus->setText(QStringLiteral("正在提交兑换码..."));
    m_apiClient->redeemCode(code);
}

void AccountDialog::onPasswordChanged()
{
    m_passwordButton->setEnabled(true);
    m_passwordButton->setText(QStringLiteral("修改密码"));
    m_oldPassword->clear();
    m_newPassword->clear();
    m_confirmPassword->clear();
    m_passwordStatus->setText(QStringLiteral("密码修改成功。"));
    m_passwordStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
}

void AccountDialog::onPasswordChangeFailed(const QString &error)
{
    m_passwordButton->setEnabled(true);
    m_passwordButton->setText(QStringLiteral("修改密码"));
    m_passwordStatus->setText(error);
    m_passwordStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}

void AccountDialog::onRedeemCompleted(const QJsonObject &result)
{
    m_redeemButton->setEnabled(true);
    m_redeemButton->setText(QStringLiteral("立即兑换"));
    m_redeemCode->clear();
    QString detail = result.value(QStringLiteral("message")).toString();
    if (detail.isEmpty()) {
        const QString type = result.value(QStringLiteral("type")).toString();
        const double value = result.value(QStringLiteral("value")).toDouble();
        detail = type == QStringLiteral("subscription")
            ? QStringLiteral("订阅权益兑换成功")
            : QStringLiteral("兑换成功，到账 %1").arg(value, 0, 'f', 2);
    }
    if (result.contains(QStringLiteral("new_balance"))) {
        const double balance = result.value(QStringLiteral("new_balance")).toDouble();
        m_balanceLabel->setText(QStringLiteral("余额 $%1").arg(balance, 0, 'f', 2));
    }
    m_redeemStatus->setText(detail);
    m_redeemStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
    emit accountBalanceChanged();
}

void AccountDialog::onRedeemFailed(const QString &error)
{
    m_redeemButton->setEnabled(true);
    m_redeemButton->setText(QStringLiteral("立即兑换"));
    m_redeemStatus->setText(error);
    m_redeemStatus->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}
