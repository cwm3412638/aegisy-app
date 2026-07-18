#ifndef MONACO_EDITOR_BRIDGE_H
#define MONACO_EDITOR_BRIDGE_H

#include <QObject>
#include <QString>

class MonacoEditorBridge : public QObject
{
    Q_OBJECT

public:
    explicit MonacoEditorBridge(QObject *parent = nullptr);

    void activateModel(int group, const QString &path, const QString &content, bool readOnly,
                       int cursorPosition, int anchorPosition,
                       int verticalScroll, int horizontalScroll);
    void setSplitEnabled(bool enabled);
    void focusGroup(int group);
    void setModelReadOnly(const QString &path, bool readOnly);
    void closeModel(const QString &path);
    void closeAllModels();
    void requestContentForSave(int group);

public slots:
    void ready();
    void contentChanged(const QString &path, const QString &content,
                        int cursorPosition, int anchorPosition);
    void viewChanged(int group, const QString &path, int cursorPosition, int anchorPosition,
                     int verticalScroll, int horizontalScroll);
    void groupActivated(int group, const QString &path);
    void saveRequested(int group, const QString &path);

signals:
    void modelActivated(int group, const QString &path, const QString &content, bool readOnly,
                        int cursorPosition, int anchorPosition,
                        int verticalScroll, int horizontalScroll);
    void splitEnabledChanged(bool enabled);
    void editorGroupFocusRequested(int group);
    void modelReadOnlyChanged(const QString &path, bool readOnly);
    void modelClosed(const QString &path);
    void allModelsClosed();
    void contentRequested(int group);

    void editorReady();
    void contentEdited(const QString &path, const QString &content,
                       int cursorPosition, int anchorPosition);
    void editorViewChanged(int group, const QString &path,
                           int cursorPosition, int anchorPosition,
                           int verticalScroll, int horizontalScroll);
    void editorGroupActivated(int group, const QString &path);
    void saveInvoked(int group, const QString &path);
};

#endif
