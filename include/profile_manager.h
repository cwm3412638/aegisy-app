#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <QObject>
#include <QString>
#include "tool_manager.h"

// 一个配置档案：保存每个工具对应的 API Key
struct Profile {
    int     index  = -1;
    QString name;
    QString claudeKey;   // AiTool::ClaudeCode
    QString codexKey;    // AiTool::CodexCli
    QString geminiKey;   // AiTool::GeminiCli

    // 按工具取 key（方便遍历时统一调用）
    QString keyFor(AiTool tool) const {
        switch (tool) {
            case AiTool::ClaudeCode: return claudeKey;
            case AiTool::CodexCli:  return codexKey;
            case AiTool::GeminiCli: return geminiKey;
            default:                return {};
        }
    }

    bool hasAnyKey() const {
        return !claudeKey.isEmpty() || !codexKey.isEmpty() || !geminiKey.isEmpty();
    }
};

// 档案管理器：所有档案持久化在 QSettings 中
// QSettings key 布局：
//   profiles/count       = N
//   profiles/active      = 0
//   profiles/<i>/name    = "默认"
//   profiles/<i>/claude  = "sk-ant-..."
//   profiles/<i>/codex   = "sk-..."
//   profiles/<i>/gemini  = "..."
class ProfileManager : public QObject
{
    Q_OBJECT

public:
    explicit ProfileManager(QObject *parent = nullptr);

    // 读取
    QList<Profile> allProfiles() const;
    int     activeIndex() const;
    Profile activeProfile() const;
    int     count() const;

    // 写入
    int  addProfile(const QString &name);          // 返回新建档案的 index
    void removeProfile(int index);                 // 不能删最后一个
    void renameProfile(int index, const QString &name);
    void setActiveIndex(int index);
    void saveKey(int profileIndex, AiTool tool, const QString &key);

signals:
    void profilesChanged();
    void activeProfileChanged(int oldIndex, int newIndex);

private:
    void ensureDefaultProfile();
};

#endif // PROFILE_MANAGER_H
