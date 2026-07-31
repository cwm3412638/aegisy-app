#include "agent_workbench_window.h"
#include <QLabel>
#include <QVBoxLayout>

AgentWorkbenchWindow::AgentWorkbenchWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
}

AgentWorkbenchWindow::~AgentWorkbenchWindow() = default;

void AgentWorkbenchWindow::setupUi()
{
    setWindowTitle(tr("Aegisy Agent Workbench"));
    resize(1200, 800);

    auto *centralWidget = new QWidget(this);
    auto *layout = new QVBoxLayout(centralWidget);

    auto *label = new QLabel(tr("Agent Workbench (Feature Preview)"), centralWidget);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    setCentralWidget(centralWidget);
}
