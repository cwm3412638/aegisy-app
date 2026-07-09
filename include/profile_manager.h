#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <QObject>
#include <QString>
#include "tool_manager.h"

// 一个配置档案：每个工具独立保存 API Key + 模型名
struct Profile {
    int     index  = -1;
    QString name;

    QString claudeKey,   claudeModel;
    QString codexKey,    codexModel;
    QString geminiKey,   geminiModel;

    // 按工具取 Key / Model（便于遍历）
    QString keyFor(AiTool tool) const {
        switch (tool) {
            case AiTool::ClaudeCode: return claudeKey;
            case AiTool::CodexCli:  return codexKey;
            case AiTool::GeminiCli: return geminiKey;
            default:                return {};
        }
    }
    QString modelFor(AiTool tool) const {
        switch (tool) {
            case AiTool::ClaudeCode: return claudeModel;
            case AiTool::CodexCli:  return codexModel;
            case AiTool::GeminiCli: return geminiModel;
            default:                return {};
        }
    }

    // 有几个工具配置了 Key
    int configuredCount() const {
        int n = 0;
        if (!claudeKey.isEmpty()) ++n;
        if (!codexKey.isEmpty())  ++n;
        if (!geminiKey.isEmpty()) ++n;
        return n;
    }
    bool hasAnyKey() const { return configuredCount() > 0; }
};

// 档案管理器：持久化到 QSettings
// 键布局：
//   profiles/count               = N
//   profiles/active              = 0
//   profiles/<i>/name            = "默认"
//   profiles/<i>/claude_key      = "sk-ant-..."
//   profiles/<i>/claude_model    = "claude-opus-4-5"
//   profiles/<i>/codex_key       = "sk-..."
//   profiles/<i>/codex_model     = "gpt-4o"
//   profiles/<i>/gemini_key      = "..."
//   profiles/<i>/gemini_model    = "gemini-2.5-pro"
class ProfileManager : public QObject
{
    Q_OBJECT

public:
    explicit ProfileManager(QObject *parent = nullptr);

    QList<Profile> allProfiles() const;
    int     activeIndex() const;
    Profile activeProfile() const;
    int     count() const;

    int  addProfile(const QString &name);
    void removeProfile(int index);
    void renameProfile(int index, const QString &name);
    void setActiveIndex(int index);

    // 保存单个工具的 Key + Model
    void saveToolConfig(int profileIndex, AiTool tool,
                        const QString &key, const QString &model);

    // 兼容旧接口（只保存 Key）
    void saveKey(int profileIndex, AiTool tool, const QString &key);

signals:
    void profilesChanged();
    void activeProfileChanged(int oldIndex, int newIndex);

private:
    void ensureDefaultProfile();
};

#endif // PROFILE_MANAGER_H
