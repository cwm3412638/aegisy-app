#ifndef LOGIN_DIALOG_H
#define LOGIN_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = nullptr);

    QString getEmail() const;
    QString getPassword() const;
    bool shouldRememberMe() const;

    void setEmail(const QString &email);
    void showError(const QString &message);
    void setLoading(bool loading);

signals:
    void loginRequested(const QString &email, const QString &password);

private slots:
    void onLoginClicked();

private:
    QLineEdit *m_emailEdit;
    QLineEdit *m_passwordEdit;
    QPushButton *m_loginButton;
    QLabel *m_errorLabel;
    QCheckBox *m_rememberCheckBox;
    QLabel *m_statusLabel;

    void setupUi();
};

#endif // LOGIN_DIALOG_H
