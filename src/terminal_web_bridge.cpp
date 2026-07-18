#include "terminal_web_bridge.h"

TerminalWebBridge::TerminalWebBridge(QObject *parent)
    : QObject(parent)
{
}

void TerminalWebBridge::resetTerminal(quint64 generation)
{
    emit resetRequested(QString::number(generation));
}

void TerminalWebBridge::writeOutput(const QString &base64)
{
    if (!base64.isEmpty()) emit outputReceived(base64);
}

void TerminalWebBridge::setInputEnabled(bool enabled)
{
    emit inputEnabledChanged(enabled);
}

void TerminalWebBridge::focusTerminal()
{
    emit focusRequested();
}

void TerminalWebBridge::pasteText(const QString &text)
{
    if (!text.isEmpty()) emit pasteReceived(text);
}

void TerminalWebBridge::ready()
{
    emit terminalReady();
}

void TerminalWebBridge::input(const QString &data)
{
    if (!data.isEmpty()) emit inputRequested(data);
}

void TerminalWebBridge::resized(int rows, int cols)
{
    emit sizeRequested(rows, cols);
}

void TerminalWebBridge::selectionChanged(const QString &text)
{
    emit selectionUpdated(text);
}

void TerminalWebBridge::copy(const QString &text)
{
    if (!text.isEmpty()) emit clipboardCopyRequested(text);
}

void TerminalWebBridge::paste()
{
    emit clipboardPasteRequested();
}
