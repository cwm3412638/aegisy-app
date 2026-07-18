#ifndef TERMINAL_WEB_BRIDGE_H
#define TERMINAL_WEB_BRIDGE_H

#include <QObject>
#include <QString>

class TerminalWebBridge : public QObject
{
    Q_OBJECT

public:
    explicit TerminalWebBridge(QObject *parent = nullptr);

    void resetTerminal(quint64 generation);
    void writeOutput(const QString &base64);
    void setInputEnabled(bool enabled);
    void focusTerminal();
    void pasteText(const QString &text);

public slots:
    void ready();
    void input(const QString &data);
    void resized(int rows, int cols);
    void selectionChanged(const QString &text);
    void copy(const QString &text);
    void paste();

signals:
    void resetRequested(const QString &generation);
    void outputReceived(const QString &base64);
    void inputEnabledChanged(bool enabled);
    void focusRequested();
    void pasteReceived(const QString &text);

    void terminalReady();
    void inputRequested(const QString &data);
    void sizeRequested(int rows, int cols);
    void selectionUpdated(const QString &text);
    void clipboardCopyRequested(const QString &text);
    void clipboardPasteRequested();
};

#endif
