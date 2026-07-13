#ifndef ACCOUNT_DIALOG_H
#define ACCOUNT_DIALOG_H

#include <QDialog>
#include <QJsonObject>

class ApiClient;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

class AccountDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AccountDialog(ApiClient *apiClient, const QJsonObject &userInfo,
                           QWidget *parent = nullptr);

signals:
    void accountBalanceChanged();

private slots:
    void submitPasswordChange();
    void submitRedeem();
    void onPasswordChanged();
    void onPasswordChangeFailed(const QString &error);
    void onRedeemCompleted(const QJsonObject &result);
    void onRedeemFailed(const QString &error);

private:
    ApiClient *m_apiClient;
    QLineEdit *m_oldPassword;
    QLineEdit *m_newPassword;
    QLineEdit *m_confirmPassword;
    QCheckBox *m_showPasswords;
    QPushButton *m_passwordButton;
    QLabel *m_passwordStatus;
    QLineEdit *m_redeemCode;
    QPushButton *m_redeemButton;
    QLabel *m_redeemStatus;
    QLabel *m_balanceLabel;
};

#endif // ACCOUNT_DIALOG_H
