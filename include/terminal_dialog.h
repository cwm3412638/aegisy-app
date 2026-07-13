#ifndef TERMINAL_DIALOG_H
#define TERMINAL_DIALOG_H

#include <QDialog>
#include <QProcessEnvironment>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;

class TerminalDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TerminalDialog(const QString &toolName,
                            const QString &executable,
                            const QString &workingDirectory,
                            const QProcessEnvironment &environment,
                            QWidget *parent = nullptr);
    ~TerminalDialog() override;

protected:
    void reject() override;

private slots:
    void sendInput();
    void stopProcess();
    void readOutput();

private:
    void setupUi();
    void startProcess();
    QString cleanOutput(const QString &text) const;

    QString m_toolName;
    QString m_executable;
    QString m_workingDirectory;
    QProcessEnvironment m_environment;
    QProcess *m_process = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QLineEdit *m_input = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPushButton *m_stopButton = nullptr;
};

#endif // TERMINAL_DIALOG_H
