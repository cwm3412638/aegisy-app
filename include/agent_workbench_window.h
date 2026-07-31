#ifndef AGENT_WORKBENCH_WINDOW_H
#define AGENT_WORKBENCH_WINDOW_H

#include <QMainWindow>

class QWebEngineView;
class QWebEnginePage;
class QWebEngineProfile;

class AgentWorkbenchWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit AgentWorkbenchWindow(QWidget *parent = nullptr);
    ~AgentWorkbenchWindow() override;

private:
    void setupUi();
    void loadWorkbenchBundle();

    QWebEngineProfile *m_profile;
    QWebEnginePage *m_page;
    QWebEngineView *m_view;
};

#endif // AGENT_WORKBENCH_WINDOW_H
