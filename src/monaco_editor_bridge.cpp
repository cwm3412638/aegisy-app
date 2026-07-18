#include "monaco_editor_bridge.h"

MonacoEditorBridge::MonacoEditorBridge(QObject *parent)
    : QObject(parent)
{
}

void MonacoEditorBridge::activateModel(int group, const QString &path,
                                       const QString &content, bool readOnly,
                                       int cursorPosition,
                                       int anchorPosition, int verticalScroll,
                                       int horizontalScroll)
{
    emit modelActivated(group, path, content, readOnly, cursorPosition, anchorPosition,
                        verticalScroll, horizontalScroll);
}

void MonacoEditorBridge::setSplitEnabled(bool enabled)
{
    emit splitEnabledChanged(enabled);
}

void MonacoEditorBridge::focusGroup(int group)
{
    emit editorGroupFocusRequested(group);
}

void MonacoEditorBridge::setModelReadOnly(const QString &path, bool readOnly)
{
    emit modelReadOnlyChanged(path, readOnly);
}

void MonacoEditorBridge::closeModel(const QString &path)
{
    emit modelClosed(path);
}

void MonacoEditorBridge::closeAllModels()
{
    emit allModelsClosed();
}

void MonacoEditorBridge::requestContentForSave(int group)
{
    emit contentRequested(group);
}

void MonacoEditorBridge::ready()
{
    emit editorReady();
}

void MonacoEditorBridge::contentChanged(const QString &path, const QString &content,
                                        int cursorPosition, int anchorPosition)
{
    emit contentEdited(path, content, cursorPosition, anchorPosition);
}

void MonacoEditorBridge::viewChanged(int group, const QString &path,
                                     int cursorPosition, int anchorPosition,
                                     int verticalScroll, int horizontalScroll)
{
    emit editorViewChanged(group, path, cursorPosition, anchorPosition,
                           verticalScroll, horizontalScroll);
}

void MonacoEditorBridge::groupActivated(int group, const QString &path)
{
    emit editorGroupActivated(group, path);
}

void MonacoEditorBridge::saveRequested(int group, const QString &path)
{
    emit saveInvoked(group, path);
}
