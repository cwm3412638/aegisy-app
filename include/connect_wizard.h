#ifndef CONNECT_WIZARD_H
#define CONNECT_WIZARD_H

#include <QDialog>
#include <QStackedWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QJsonArray>
#include "api_client.h"
#include "profile_manager.h"
#include "tool_manager.h"

// 三步接入向导
// 用法：
//   ConnectWizardDialog dlg(apiClient, profileMgr, -1, this);   // 新建
//   ConnectWizardDialog dlg(apiClient, profileMgr, idx, this);  // 编辑
//   if (dlg.exec() == QDialog::Accepted) { ... dlg.resultIndex() ... }
class ConnectWizardDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConnectWizardDialog(ApiClient *client,
                                  ProfileManager *pm,
                                  int editIndex = -1,
                                  QWidget *parent = nullptr);

    // 成功完成后返回新建或编辑的 Profile index；未完成返回 -1
    int resultIndex() const { return m_resultIndex; }

private slots:
    void onApiKeysReceived(const QJsonArray &keys);
    void onModelsReceived(const QJsonArray &models);
    void onRequestFailed(const QString &error);
    void onQueryModels(AiTool tool);
    void goNext();
    void goBack();
    void finish();

private:
    // 每个工具对应的 UI 控件集合
    struct ToolSection {
        AiTool        tool;
        QCheckBox    *enableCheck  = nullptr;
        QComboBox    *keyCombo     = nullptr;
        QPushButton  *queryButton  = nullptr;
        QLabel       *loadingLabel = nullptr;
        QComboBox    *modelCombo   = nullptr;
    };

    void setupUi();
    QWidget* buildPage1();  // 档案命名
    QWidget* buildPage2();  // 工具配置
    QWidget* buildPage3();  // 确认摘要
    void refreshPage3();
    void populateKeyDropdowns();
    void setPage2Loading(AiTool tool, bool loading);
    void updateNavButtons();
    QString currentKey(const ToolSection &s) const;

    ApiClient       *m_apiClient;
    ProfileManager  *m_profileManager;
    int              m_editIndex;     // -1 = 新建
    int              m_resultIndex = -1;
    AiTool           m_queryingTool = AiTool::ClaudeCode;
    bool             m_waitingModels = false;

    QStackedWidget  *m_stack        = nullptr;
    QLabel          *m_stepLabel    = nullptr;
    QPushButton     *m_backBtn      = nullptr;
    QPushButton     *m_nextBtn      = nullptr;

    // Page 1
    QLineEdit *m_nameEdit = nullptr;

    // Page 2 — 三个工具 section
    QList<ToolSection> m_sections;

    // Page 3
    QLabel *m_summaryLabel = nullptr;

    // 账号 Keys 原始数据
    QJsonArray m_allKeys;
};

#endif // CONNECT_WIZARD_H
