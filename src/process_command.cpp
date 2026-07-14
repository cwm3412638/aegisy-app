#include "process_command.h"

#include <QFileInfo>
#include <QProcess>

namespace {

QString windowsCommandQuote(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

} // namespace

namespace ProcessCommand {

QString windowsBatchNativeArguments(const QString &program,
                                    const QStringList &arguments)
{
    QStringList tokens = { windowsCommandQuote(program) };
    for (const QString &argument : arguments) {
        tokens.append(windowsCommandQuote(argument));
    }

    // /S /C needs an outer quote pair around a command whose executable is
    // quoted. This yields: /C ""C:/Program Files/nodejs/npm.cmd" "install""
    return QStringLiteral("/D /V:OFF /S /C \"")
        + tokens.join(QLatin1Char(' ')) + QLatin1Char('"');
}

void start(QProcess *process,
           const QString &program,
           const QStringList &arguments)
{
#ifdef Q_OS_WIN
    const QString suffix = QFileInfo(program).suffix();
    if (suffix.compare(QStringLiteral("cmd"), Qt::CaseInsensitive) == 0
            || suffix.compare(QStringLiteral("bat"), Qt::CaseInsensitive) == 0) {
        process->setProgram(QStringLiteral("cmd.exe"));
        process->setArguments({});
        // setNativeArguments avoids QProcess escaping the embedded quotes as
        // backslash-quote sequences, which cmd.exe does not understand.
        process->setNativeArguments(
            windowsBatchNativeArguments(program, arguments));
        process->start();
        return;
    }
#endif
    process->start(program, arguments);
}

QString decodeOutput(const QByteArray &data)
{
    const QString utf8 = QString::fromUtf8(data);
#ifdef Q_OS_WIN
    if (utf8.contains(QChar::ReplacementCharacter)) {
        return QString::fromLocal8Bit(data);
    }
#endif
    return utf8;
}

} // namespace ProcessCommand
