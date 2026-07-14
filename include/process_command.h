#ifndef PROCESS_COMMAND_H
#define PROCESS_COMMAND_H

#include <QByteArray>
#include <QString>
#include <QStringList>

class QProcess;

namespace ProcessCommand {

// Exposed for regression tests: QProcess must pass this part of the Windows
// command line verbatim, otherwise cmd.exe sees \"path\" as the command name.
QString windowsBatchNativeArguments(const QString &program,
                                    const QStringList &arguments);

void start(QProcess *process,
           const QString &program,
           const QStringList &arguments = {});

QString decodeOutput(const QByteArray &data);

} // namespace ProcessCommand

#endif // PROCESS_COMMAND_H
