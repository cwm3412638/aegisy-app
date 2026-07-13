#include "terminal_dialog.h"

#include "app_theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTextCursor>
#include <QVBoxLayout>

TerminalDialog::TerminalDialog(const QString &toolName,
                               const QString &executable,
                               const QString &workingDirectory,
                               const QProcessEnvironment &environment,
                               QWidget *parent)
    : QDialog(parent)
    , m_toolName(toolName)
    , m_executable(executable)
    , m_workingDirectory(workingDirectory)
    , m_environment(environment)
{
    setupUi();
    setWindowTitle(QStringLiteral("%1 终端").arg(toolName));
    setMinimumSize(760, 500);
    resize(900, 620);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    QTimer::singleShot(0, this, &TerminalDialog::startProcess);
}

TerminalDialog::~TerminalDialog()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(500);
    }
}

void TerminalDialog::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(10);
    auto *header = new QHBoxLayout();
    auto *title = new QLabel(m_toolName, this);
    title->setStyleSheet(QStringLiteral("font-size:17px; font-weight:700; color:#101828;"));
    header->addWidget(title);
    m_stateLabel = new QLabel(QStringLiteral("准备启动"), this);
    m_stateLabel->setStyleSheet(QStringLiteral("font-size:11px; color:#667085;"));
    header->addWidget(m_stateLabel);
    header->addStretch();
    auto *path = new QLabel(m_workingDirectory, this);
    path->setToolTip(m_workingDirectory);
    path->setStyleSheet(QStringLiteral("font-family:monospace; font-size:10px; color:#667085;"));
    header->addWidget(path);
    root->addLayout(header);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setMaximumBlockCount(5000);
    m_output->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#111827; color:#e5e7eb; border:1px solid #344054;"
        "border-radius:8px; padding:10px; font-family:monospace; font-size:12px; }"));
    root->addWidget(m_output, 1);

    auto *inputRow = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("输入命令内容并按回车发送..."));
    inputRow->addWidget(m_input, 1);
    auto *sendButton = new QPushButton(QStringLiteral("发送"), this);
    sendButton->setStyleSheet(AppTheme::primaryButtonStyle());
    inputRow->addWidget(sendButton);
    m_stopButton = new QPushButton(QStringLiteral("停止"), this);
    m_stopButton->setStyleSheet(AppTheme::dangerButtonStyle());
    inputRow->addWidget(m_stopButton);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    inputRow->addWidget(closeButton);
    root->addLayout(inputRow);

    connect(m_input, &QLineEdit::returnPressed, this, &TerminalDialog::sendInput);
    connect(sendButton, &QPushButton::clicked, this, &TerminalDialog::sendInput);
    connect(m_stopButton, &QPushButton::clicked, this, &TerminalDialog::stopProcess);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

void TerminalDialog::startProcess()
{
    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_workingDirectory);
    m_process->setProcessEnvironment(m_environment);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &TerminalDialog::readOutput);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
        readOutput();
        m_stateLabel->setText(QStringLiteral("已退出，代码 %1").arg(exitCode));
        m_stateLabel->setStyleSheet(exitCode == 0
            ? QStringLiteral("font-size:11px; color:#067647;")
            : QStringLiteral("font-size:11px; color:#b42318;"));
        m_input->setEnabled(false);
        m_stopButton->setEnabled(false);
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
        m_output->appendPlainText(QStringLiteral("启动失败：%1").arg(m_process->errorString()));
    });

#if defined(Q_OS_MAC)
    m_process->start(QStringLiteral("/usr/bin/script"),
                     { QStringLiteral("-q"), QStringLiteral("/dev/null"), m_executable });
#elif defined(Q_OS_WIN)
    m_process->start(QStringLiteral("cmd.exe"),
                     { QStringLiteral("/Q"), QStringLiteral("/K"),
                       QStringLiteral("\"%1\"").arg(m_executable) });
#else
    const QString script = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (!script.isEmpty()) {
        QString quoted = m_executable;
        quoted.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
        m_process->start(script,
                         { QStringLiteral("-q"), QStringLiteral("-c"),
                           QStringLiteral("'%1'").arg(quoted), QStringLiteral("/dev/null") });
    } else {
        m_process->start(m_executable);
    }
#endif
    if (m_process->waitForStarted(2500)) {
        m_stateLabel->setText(QStringLiteral("运行中"));
        m_stateLabel->setStyleSheet(QStringLiteral("font-size:11px; color:#067647;"));
        m_input->setFocus();
    }
}

QString TerminalDialog::cleanOutput(const QString &text) const
{
    QString result = text;
    static const QRegularExpression ansi(
        QStringLiteral("\\x1B(?:[@-Z\\\\-_]|\\[[0-?]*[ -/]*[@-~])"));
    result.remove(ansi);
    result.remove(QChar(0x08));
    result.remove(QChar(0x04));
    return result;
}

void TerminalDialog::readOutput()
{
    const QString output = cleanOutput(QString::fromUtf8(m_process->readAllStandardOutput()));
    if (!output.isEmpty()) {
        m_output->moveCursor(QTextCursor::End);
        m_output->insertPlainText(output);
        m_output->moveCursor(QTextCursor::End);
    }
}

void TerminalDialog::sendInput()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) return;
    const QString input = m_input->text();
    m_process->write(input.toUtf8() + '\n');
    m_input->clear();
}

void TerminalDialog::stopProcess()
{
    if (!m_process || m_process->state() == QProcess::NotRunning) return;
    m_process->terminate();
    QTimer::singleShot(1200, m_process, [process = m_process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
}

void TerminalDialog::reject()
{
    if (m_process && m_process->state() != QProcess::NotRunning) stopProcess();
    QDialog::reject();
}
