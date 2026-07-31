#ifndef AGENT_WORKBENCH_WINDOW_H
#define AGENT_WORKBENCH_WINDOW_H

#include <QMainWindow>

class AgentWorkbenchWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit AgentWorkbenchWindow(QWidget *parent = nullptr);
    ~AgentWorkbenchWindow() override;

private:
    void setupUi();
};

#endif // AGENT_WORKBENCH_WINDOW_H
