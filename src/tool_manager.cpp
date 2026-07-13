#include "tool_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>

// 官方指南规定：BASE_URL 一律裸域名，不带 /v1
static const QString kBaseUrl = "https://aegisy.cc";
constexpr int kMaxBackupsPerTool = 10;

static QString toolSlug(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli:   return QStringLiteral("codex");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    case AiTool::OpenCode:   return QStringLiteral("opencode");
    }
    return QStringLiteral("unknown");
}

static QString backupRootPath(AiTool tool)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/backups/") + toolSlug(tool);
}

#ifdef Q_OS_WIN
static const QString kNpmCmd  = "npm.cmd";
static const QString kWhichCmd = "where";
#else
static const QString kNpmCmd  = "npm";
static const QString kWhichCmd = "which";
#endif

static void addSearchPath(QStringList &paths, const QString &path, bool prepend = false)
{
    if (path.isEmpty() || !QDir(path).exists() || paths.contains(path)) {
        return;
    }
    if (prepend) {
        paths.prepend(path);
    } else {
        paths.append(path);
    }
}

static QStringList commandSearchPaths()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList paths = environment.value(QStringLiteral("PATH"))
        .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    const QString home = QDir::homePath();

#if defined(Q_OS_MAC)
    const QStringList preferredPaths = {
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        home + QStringLiteral("/.volta/bin"),
        home + QStringLiteral("/.local/bin"),
        home + QStringLiteral("/.npm-global/bin"),
        home + QStringLiteral("/Library/pnpm"),
        home + QStringLiteral("/.bun/bin"),
        home + QStringLiteral("/.asdf/shims"),
        home + QStringLiteral("/.local/share/mise/shims"),
        home + QStringLiteral("/.local/share/fnm/aliases/default/bin"),
    };
    for (auto it = preferredPaths.crbegin(); it != preferredPaths.crend(); ++it) {
        addSearchPath(paths, *it, true);
    }

    QDir nvmVersions(home + QStringLiteral("/.nvm/versions/node"));
    const QFileInfoList nodeVersions = nvmVersions.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (auto it = nodeVersions.crbegin(); it != nodeVersions.crend(); ++it) {
        addSearchPath(paths, it->filePath() + QStringLiteral("/bin"), true);
    }
#elif defined(Q_OS_WIN)
    addSearchPath(paths,
                  QString::fromLocal8Bit(qgetenv("APPDATA")) + QStringLiteral("\\npm"),
                  true);
    addSearchPath(paths,
                  QString::fromLocal8Bit(qgetenv("ProgramFiles"))
                      + QStringLiteral("\\nodejs"),
                  true);
    addSearchPath(paths,
                  QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"))
                      + QStringLiteral("\\nodejs"),
                  true);
    addSearchPath(paths,
                  QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"))
                      + QStringLiteral("\\Programs\\nodejs"),
                  true);
#else
    addSearchPath(paths, home + QStringLiteral("/.local/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.volta/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.npm-global/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.asdf/shims"), true);
#endif
    return paths;
}

static QProcessEnvironment commandEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("PATH"), commandSearchPaths().join(QDir::listSeparator()));
    return environment;
}

static const QStringList kProviderEnvironmentVariables = {
    QStringLiteral("ANTHROPIC_API_KEY"),
    QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
    QStringLiteral("ANTHROPIC_BASE_URL"),
    QStringLiteral("OPENAI_API_KEY"),
    QStringLiteral("OPENAI_BASE_URL"),
    QStringLiteral("OPENAI_API_BASE"),
    QStringLiteral("GEMINI_API_KEY"),
    QStringLiteral("GOOGLE_API_KEY"),
    QStringLiteral("GOOGLE_GEMINI_BASE_URL"),
};

static QString shellEnvironmentReset()
{
    QStringList arguments;
    for (const QString &name : kProviderEnvironmentVariables) {
        arguments << QStringLiteral("-u") << name;
    }
    return QStringLiteral("env %1 TERM=xterm-256color COLORTERM=truecolor")
        .arg(arguments.join(QLatin1Char(' ')));
}

static bool startDetachedWithEnvironment(const QString &program,
                                         const QStringList &arguments,
                                         const QString &workingDirectory,
                                         const QProcessEnvironment &environment)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(workingDirectory);
    process.setProcessEnvironment(environment);
    return process.startDetached();
}

static QString extractVersion(const QString &output)
{
    static const QRegularExpression versionPattern(
        QStringLiteral("(\\d+(?:\\.\\d+){1,3}(?:[-+][0-9A-Za-z.-]+)?)"));
    const QRegularExpressionMatch match = versionPattern.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

static QString npmVersionFromJson(const QByteArray &data, const QString &packageName)
{
    const QJsonDocument document = QJsonDocument::fromJson(data);
    if (!document.isObject()) {
        return QString();
    }
    return document.object()
        .value(QStringLiteral("dependencies")).toObject()
        .value(packageName).toObject()
        .value(QStringLiteral("version")).toString();
}

static QString shellQuote(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

static QString appleScriptQuote(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return value;
}

ToolManager::ToolManager(QObject *parent)
    : QObject(parent)
{
}

QString ToolManager::toolName(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("Claude Code");
    case AiTool::CodexCli:   return QStringLiteral("Codex CLI");
    case AiTool::GeminiCli:  return QStringLiteral("Gemini CLI");
    case AiTool::OpenCode:   return QStringLiteral("OpenCode");
    }
    return QString();
}

QString ToolManager::toolPlatform(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("anthropic");
    case AiTool::CodexCli:   return QStringLiteral("openai");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    case AiTool::OpenCode:   return QStringLiteral("anthropic");
    }
    return QString();
}

QString ToolManager::npmPackage(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("@anthropic-ai/claude-code");
    case AiTool::CodexCli:   return QStringLiteral("@openai/codex");
    case AiTool::GeminiCli:  return QStringLiteral("@google/gemini-cli");
    case AiTool::OpenCode:   return QStringLiteral("opencode-ai");
    }
    return QString();
}

QString ToolManager::cliCommand(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli:   return QStringLiteral("codex");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    case AiTool::OpenCode:   return QStringLiteral("opencode");
    }
    return QString();
}

QString ToolManager::configFilePath(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("~/.claude/settings.json");
    case AiTool::CodexCli:   return QStringLiteral("~/.codex/auth.json");
    case AiTool::GeminiCli:  return QStringLiteral("~/.gemini/.env");
    case AiTool::OpenCode:   return QStringLiteral("~/.config/opencode/config.json");
    }
    return QString();
}

bool ToolManager::commandExists(const QString &command, int timeoutMs)
{
    return !resolveCommand(command, timeoutMs).isEmpty();
}

QString ToolManager::resolveCommand(const QString &command, int timeoutMs) const
{
    const QFileInfo directPath(command);
    if (directPath.isAbsolute() && directPath.exists() && directPath.isExecutable()) {
        return directPath.absoluteFilePath();
    }

    const QString executable = QStandardPaths::findExecutable(command, commandSearchPaths());
    if (!executable.isEmpty()) {
        return executable;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(kWhichCmd, QStringList() << command);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return QString();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return QString();
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts)
        .value(0).trimmed();
}

QString ToolManager::commandVersion(const QString &executable, int timeoutMs) const
{
    if (executable.isEmpty()) {
        return QString();
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, QStringList() << QStringLiteral("--version"));
    if (!process.waitForStarted(qMin(timeoutMs, 1000))) {
        return QString();
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return QString();
    }
    return extractVersion(QString::fromUtf8(process.readAll()));
}

QString ToolManager::npmPackageVersion(AiTool tool, int timeoutMs) const
{
    const QString npmExecutable = resolveCommand(kNpmCmd, timeoutMs);
    if (npmExecutable.isEmpty()) {
        return QString();
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.start(npmExecutable,
                  QStringList() << QStringLiteral("list") << QStringLiteral("-g")
                                << npmPackage(tool) << QStringLiteral("--depth=0")
                                << QStringLiteral("--json"));
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return QString();
    }
    return npmVersionFromJson(process.readAllStandardOutput(), npmPackage(tool));
}

bool ToolManager::isNodeAvailable()
{
    return commandExists("node");
}

ConfigurationPreview ToolManager::previewConfiguration(AiTool tool,
                                                        const QString &model,
                                                        bool gatewayMode)
{
    ConfigurationPreview preview;
    preview.files = managedConfigPaths(tool);
    const QString targetModel = model.trimmed().isEmpty()
        ? QStringLiteral("工具默认模型") : model.trimmed();

    for (const QString &path : preview.files) {
        preview.changes.append(QFileInfo::exists(path)
            ? QStringLiteral("更新现有文件：%1").arg(path)
            : QStringLiteral("创建配置文件：%1").arg(path));
    }
    switch (tool) {
    case AiTool::ClaudeCode:
        preview.changes.append(QStringLiteral(
            "设置 ANTHROPIC_BASE_URL = %1")
            .arg(gatewayMode
                ? QStringLiteral("http://127.0.0.1:43112/tools/claude")
                : QStringLiteral("https://aegisy.cc")));
        preview.changes.append(QStringLiteral(
            "%1 ANTHROPIC_AUTH_TOKEN，并移除冲突的 ANTHROPIC_API_KEY 字段")
            .arg(gatewayMode ? QStringLiteral("写入本地网关令牌到")
                             : QStringLiteral("更新")));
        break;
    case AiTool::CodexCli:
        preview.changes.append(QStringLiteral(
            "设置模型提供方和 Base URL = %1")
            .arg(gatewayMode
                ? QStringLiteral("http://127.0.0.1:43112/tools/codex/v1")
                : QStringLiteral("https://aegisy.cc")));
        preview.changes.append(gatewayMode
            ? QStringLiteral("OPENAI_API_KEY 仅写入本地网关令牌，真实 Key 保留在网关内存")
            : QStringLiteral("更新 OPENAI_API_KEY 安全认证字段"));
        preview.changes.append(QStringLiteral("模型：%1").arg(targetModel));
        break;
    case AiTool::GeminiCli:
        preview.changes.append(QStringLiteral(
            "设置 GOOGLE_GEMINI_BASE_URL = %1")
            .arg(gatewayMode
                ? QStringLiteral("http://127.0.0.1:43112/tools/gemini")
                : QStringLiteral("https://aegisy.cc")));
        preview.changes.append(gatewayMode
            ? QStringLiteral("GEMINI_API_KEY 仅写入本地网关令牌，真实 Key 保留在网关内存")
            : QStringLiteral("更新 GEMINI_API_KEY 安全认证字段"));
        preview.changes.append(QStringLiteral("模型：%1").arg(targetModel));
        break;
    case AiTool::OpenCode:
        preview.changes.append(QStringLiteral(
            "设置 provider.anthropic.base_url = %1")
            .arg(gatewayMode
                ? QStringLiteral("http://127.0.0.1:43112/tools/claude")
                : QStringLiteral("https://aegisy.cc")));
        preview.changes.append(gatewayMode
            ? QStringLiteral("provider.anthropic.api_key 仅写入本地网关令牌")
            : QStringLiteral("更新 provider.anthropic.api_key"));
        preview.changes.append(QStringLiteral("模型：%1").arg(targetModel));
        break;
    }

    const ToolStatus status = detectFast(tool);
    if (!status.conflictWarning.isEmpty()) {
        preview.warnings.append(status.conflictWarning);
    }
    preview.changes.append(QStringLiteral("写入前自动创建可恢复备份"));
    preview.changes.append(QStringLiteral("保留配置文件中的其它非托管字段"));
    return preview;
}

QList<RuntimeStatus> ToolManager::detectRuntimes(int timeoutMs) const
{
    struct Definition {
        QString id;
        QString name;
        QString command;
        bool required;
    };
    const Definition definitions[] = {
        { QStringLiteral("node"), QStringLiteral("Node.js"), QStringLiteral("node"), true },
        { QStringLiteral("npm"), QStringLiteral("npm"), kNpmCmd, true },
        { QStringLiteral("git"), QStringLiteral("Git"), QStringLiteral("git"), true },
        { QStringLiteral("pnpm"), QStringLiteral("pnpm"), QStringLiteral("pnpm"), false },
        { QStringLiteral("bun"), QStringLiteral("Bun"), QStringLiteral("bun"), false },
    };

    QList<RuntimeStatus> result;
    for (const Definition &definition : definitions) {
        RuntimeStatus status;
        status.id = definition.id;
        status.category = QStringLiteral("系统依赖");
        status.name = definition.name;
        status.command = definition.command;
        status.required = definition.required;
        status.executablePath = resolveCommand(status.command, timeoutMs);
        status.installed = !status.executablePath.isEmpty();
        if (status.installed) {
            status.version = commandVersion(status.executablePath, timeoutMs);
        }
        result.append(status);
    }
    return result;
}

QList<RuntimeStatus> ToolManager::detectCompanionTools(int timeoutMs) const
{
    struct Definition { const char *id; const char *name; const char *command; };
    const Definition definitions[] = {
        { "opencode", "OpenCode", "opencode" },
        { "openclaw", "OpenClaw", "openclaw" },
        { "hermes", "Hermes", "hermes" },
        { "vscode", "Visual Studio Code", "code" },
    };
    QList<RuntimeStatus> result;
    for (const Definition &definition : definitions) {
        RuntimeStatus status;
        status.id = QString::fromLatin1(definition.id);
        status.category = QStringLiteral("其它工具");
        status.name = QString::fromLatin1(definition.name);
        status.command = QString::fromLatin1(definition.command);
        status.executablePath = resolveCommand(status.command, timeoutMs);
        status.installed = !status.executablePath.isEmpty();
        if (status.installed) status.version = commandVersion(status.executablePath, timeoutMs);
        result.append(status);
    }
    return result;
}

bool ToolManager::launch(AiTool tool, const QString &workingDirectory)
{
    m_lastError.clear();
    const QString executable = resolveCommand(cliCommand(tool), 1500);
    if (executable.isEmpty()) {
        m_lastError = QStringLiteral("未找到 %1 可执行程序，请先安装运行环境。")
            .arg(toolName(tool));
        return false;
    }

    QString directory = workingDirectory.trimmed();
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    if (!QFileInfo(directory).isDir()) {
        m_lastError = QStringLiteral("工作目录不存在：%1").arg(directory);
        return false;
    }

#if defined(Q_OS_MAC)
    const QString command = QStringLiteral("cd %1 && exec %2 %3")
        .arg(shellQuote(directory), shellEnvironmentReset(), shellQuote(executable));
    bool started = false;
    if (QDir(QStringLiteral("/Applications/iTerm.app")).exists()) {
        const QString script = QStringLiteral(
            "tell application \"iTerm\"\n"
            "activate\n"
            "set newWindow to (create window with default profile)\n"
            "tell current session of newWindow to write text \"%1\"\n"
            "end tell").arg(appleScriptQuote(command));
        started = QProcess::startDetached(
            QStringLiteral("osascript"), { QStringLiteral("-e"), script });
    }
    if (!started) {
        const QString script = QStringLiteral(
            "tell application \"Terminal\" to do script \"%1\"")
            .arg(appleScriptQuote(command));
        started = QProcess::startDetached(
            QStringLiteral("osascript"), { QStringLiteral("-e"), script,
                                            QStringLiteral("-e"),
                                            QStringLiteral("tell application \"Terminal\" to activate") });
    }
#elif defined(Q_OS_WIN)
    QString quotedDirectory = directory;
    quotedDirectory.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    QString quotedExecutable = executable;
    quotedExecutable.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    const QString command = QStringLiteral("cd /d \"%1\" && \"%2\"")
        .arg(quotedDirectory, quotedExecutable);
    const QProcessEnvironment environment = launchEnvironment(tool);
    QString windowsTerminal = QStandardPaths::findExecutable(QStringLiteral("wt.exe"));
    if (windowsTerminal.isEmpty()) {
        const QString candidate = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"))
            + QStringLiteral("\\Microsoft\\WindowsApps\\wt.exe");
        if (QFileInfo(candidate).isFile()) windowsTerminal = candidate;
    }
    QString powershell = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
    if (powershell.isEmpty()) {
        powershell = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    }
    if (powershell.isEmpty()) {
        const QString candidate = QString::fromLocal8Bit(qgetenv("SystemRoot"))
            + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe");
        if (QFileInfo(candidate).isFile()) powershell = candidate;
    }
    bool started = false;
    if (!windowsTerminal.isEmpty()) {
        started = startDetachedWithEnvironment(
            windowsTerminal,
            { QStringLiteral("-d"), directory,
              QStringLiteral("cmd.exe"), QStringLiteral("/K"), command }, directory,
            environment);
    } else if (!powershell.isEmpty()) {
        started = startDetachedWithEnvironment(
            powershell,
            { QStringLiteral("-NoExit"), QStringLiteral("-Command"),
              QStringLiteral("& \"%1\"").arg(quotedExecutable) }, directory,
            environment);
    } else {
        started = startDetachedWithEnvironment(
            QStringLiteral("cmd.exe"), { QStringLiteral("/K"), command }, directory,
            environment);
    }
#else
    const QString gnomeTerminal = QStandardPaths::findExecutable(QStringLiteral("gnome-terminal"));
    const QString konsole = QStandardPaths::findExecutable(QStringLiteral("konsole"));
    const QString terminal = QStandardPaths::findExecutable(QStringLiteral("x-terminal-emulator"));
    const QString xfceTerminal = QStandardPaths::findExecutable(QStringLiteral("xfce4-terminal"));
    const QString kitty = QStandardPaths::findExecutable(QStringLiteral("kitty"));
    const QString alacritty = QStandardPaths::findExecutable(QStringLiteral("alacritty"));
    bool started = false;
    if (!gnomeTerminal.isEmpty()) {
        started = startDetachedWithEnvironment(
            gnomeTerminal,
            { QStringLiteral("--working-directory=%1").arg(directory),
              QStringLiteral("--"), executable }, directory, launchEnvironment(tool));
    } else if (!konsole.isEmpty()) {
        started = startDetachedWithEnvironment(
            konsole,
            { QStringLiteral("--workdir"), directory,
              QStringLiteral("-e"), executable }, directory, launchEnvironment(tool));
    } else if (!xfceTerminal.isEmpty()) {
        started = startDetachedWithEnvironment(
            xfceTerminal,
            { QStringLiteral("--working-directory"), directory,
              QStringLiteral("--command"), executable }, directory, launchEnvironment(tool));
    } else if (!kitty.isEmpty()) {
        started = startDetachedWithEnvironment(
            kitty, { QStringLiteral("--directory"), directory, executable }, directory,
            launchEnvironment(tool));
    } else if (!alacritty.isEmpty()) {
        started = startDetachedWithEnvironment(
            alacritty,
            { QStringLiteral("--working-directory"), directory,
              QStringLiteral("-e"), executable }, directory, launchEnvironment(tool));
    } else if (!terminal.isEmpty()) {
        const QString command = QStringLiteral("cd %1 && exec %2 %3")
            .arg(shellQuote(directory), shellEnvironmentReset(), shellQuote(executable));
        started = startDetachedWithEnvironment(
            terminal,
            { QStringLiteral("-e"), QStringLiteral("sh"),
              QStringLiteral("-lc"), command }, directory, launchEnvironment(tool));
    }
#endif

    if (!started) {
        m_lastError = QStringLiteral("无法启动系统终端。请确认终端应用可用。");
    }
    return started;
}

bool ToolManager::isCliRunning(AiTool tool) const
{
    const QString command = cliCommand(tool);
    if (command.isEmpty()) {
        return false;
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);

#if defined(Q_OS_WIN)
    // tasklist 按精确映像名过滤；命中会输出对应行，否则输出提示信息。
    const QString imageName = command + QStringLiteral(".exe");
    process.start(QStringLiteral("tasklist"),
                  { QStringLiteral("/FI"),
                    QStringLiteral("IMAGENAME eq %1").arg(imageName),
                    QStringLiteral("/NH") });
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    const QString output = QString::fromLocal8Bit(process.readAll());
    return output.contains(imageName, Qt::CaseInsensitive);
#else
    // pgrep -x 精确匹配进程名，避免匹配到启动器命令行里的 "codex" 等误报。
    process.start(QStringLiteral("pgrep"), { QStringLiteral("-x"), command });
    if (!process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    // pgrep 命中返回 0 并打印 PID；无匹配返回 1。
    return process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0
        && !process.readAll().trimmed().isEmpty();
#endif
}

QString ToolManager::resolvedExecutable(AiTool tool, int timeoutMs) const
{
    return resolveCommand(cliCommand(tool), timeoutMs);
}

QString ToolManager::resolvedRuntimeCommand(const QString &command, int timeoutMs) const
{
    return resolveCommand(command, timeoutMs);
}

QProcessEnvironment ToolManager::launchEnvironment(AiTool tool) const
{
    Q_UNUSED(tool);
    QProcessEnvironment environment = commandEnvironment();
    for (const QString &name : kProviderEnvironmentVariables) {
        environment.remove(name);
    }
    environment.remove(QStringLiteral("NO_COLOR"));
    environment.remove(QStringLiteral("CI"));
    environment.remove(QStringLiteral("CODEX_CI"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.insert(QStringLiteral("COLORTERM"), QStringLiteral("truecolor"));

    return environment;
}

QString ToolManager::homeFilePath(const QString &relative)
{
    return QDir::homePath() + "/" + relative;
}

QStringList ToolManager::managedConfigPaths(AiTool tool) const
{
    switch (tool) {
    case AiTool::ClaudeCode:
        return { homeFilePath(QStringLiteral(".claude/settings.json")) };
    case AiTool::CodexCli:
        return {
            homeFilePath(QStringLiteral(".codex/auth.json")),
            homeFilePath(QStringLiteral(".codex/config.toml")),
        };
    case AiTool::GeminiCli:
        return { homeFilePath(QStringLiteral(".gemini/.env")) };
    case AiTool::OpenCode:
        return { homeFilePath(QStringLiteral(".config/opencode/config.json")) };
    }
    return {};
}

QString ToolManager::createBackup(AiTool tool)
{
    const QString rootPath = backupRootPath(tool);
    QDir root;
    if (!root.mkpath(rootPath)) {
        m_lastError = QStringLiteral("无法创建备份目录：%1").arg(rootPath);
        return QString();
    }

    const QString id = QStringLiteral("%1_%2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
             QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    const QString backupPath = rootPath + QLatin1Char('/') + id;
    if (!root.mkpath(backupPath)) {
        m_lastError = QStringLiteral("无法创建备份批次：%1").arg(backupPath);
        return QString();
    }

    QJsonArray files;
    const QStringList paths = managedConfigPaths(tool);
    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths[i];
        const bool existed = QFileInfo::exists(path);
        const QString payloadName = QStringLiteral("file_%1.bin").arg(i);
        if (existed) {
            QFile source(path);
            if (!source.open(QIODevice::ReadOnly)) {
                QDir(backupPath).removeRecursively();
                m_lastError = QStringLiteral("无法读取待备份配置：%1").arg(path);
                return QString();
            }
            const QByteArray payloadData = source.readAll();
            QSaveFile payload(backupPath + QLatin1Char('/') + payloadName);
            if (!payload.open(QIODevice::WriteOnly)
                    || payload.write(payloadData) != payloadData.size()
                    || !payload.commit()) {
                QDir(backupPath).removeRecursively();
                m_lastError = QStringLiteral("无法保存配置快照：%1").arg(path);
                return QString();
            }
            QFile::setPermissions(
                backupPath + QLatin1Char('/') + payloadName,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }

        QJsonObject entry;
        entry.insert(QStringLiteral("path"), path);
        entry.insert(QStringLiteral("existed"), existed);
        entry.insert(QStringLiteral("payload"), payloadName);
        files.append(entry);
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), 1);
    manifest.insert(QStringLiteral("tool"), static_cast<int>(tool));
    manifest.insert(
        QStringLiteral("created_at"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("files"), files);

    const QString manifestPath = backupPath + QStringLiteral("/manifest.json");
    QSaveFile manifestFile(manifestPath);
    const QByteArray manifestData = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!manifestFile.open(QIODevice::WriteOnly)
            || manifestFile.write(manifestData) != manifestData.size()
            || !manifestFile.commit()) {
        QDir(backupPath).removeRecursively();
        m_lastError = QStringLiteral("无法写入备份清单：%1").arg(manifestPath);
        return QString();
    }
    QFile::setPermissions(
        manifestPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return id;
}

QList<ConfigBackup> ToolManager::backupHistory(AiTool tool) const
{
    QList<ConfigBackup> result;
    QDir root(backupRootPath(tool));
    const QStringList ids = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &id : ids) {
        QFile manifest(root.filePath(id + QStringLiteral("/manifest.json")));
        if (!manifest.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(manifest.readAll()).object();
        if (object.value(QStringLiteral("tool")).toInt(-1) != static_cast<int>(tool)) {
            continue;
        }
        ConfigBackup backup;
        backup.id = id;
        backup.tool = tool;
        backup.createdAt = QDateTime::fromString(
            object.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        backup.fileCount = object.value(QStringLiteral("files")).toArray().size();
        result.append(backup);
    }
    std::sort(result.begin(), result.end(), [](const ConfigBackup &left, const ConfigBackup &right) {
        return left.createdAt > right.createdAt;
    });
    return result;
}

void ToolManager::pruneBackups(AiTool tool)
{
    const QList<ConfigBackup> history = backupHistory(tool);
    const QString rootPath = backupRootPath(tool);
    for (int i = kMaxBackupsPerTool; i < history.size(); ++i) {
        QDir(rootPath + QLatin1Char('/') + history[i].id).removeRecursively();
    }
}

bool ToolManager::restoreBackupInternal(const QString &backupId, AiTool tool)
{
    const QString backupPath = backupRootPath(tool) + QLatin1Char('/') + backupId;
    QFile manifestFile(backupPath + QStringLiteral("/manifest.json"));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("无法读取备份清单。");
        return false;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    if (manifest.value(QStringLiteral("tool")).toInt(-1) != static_cast<int>(tool)) {
        m_lastError = QStringLiteral("备份类型与目标终端不匹配。");
        return false;
    }

    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : files) {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || !managedConfigPaths(tool).contains(path)) {
            m_lastError = QStringLiteral("备份清单包含无效路径。");
            return false;
        }
        if (!entry.value(QStringLiteral("existed")).toBool()) {
            if (QFileInfo::exists(path) && !QFile::remove(path)) {
                m_lastError = QStringLiteral("无法删除备份前不存在的配置：%1").arg(path);
                return false;
            }
            continue;
        }

        QFile payload(backupPath + QLatin1Char('/')
                      + entry.value(QStringLiteral("payload")).toString());
        if (!payload.open(QIODevice::ReadOnly)) {
            m_lastError = QStringLiteral("备份内容缺失：%1").arg(path);
            return false;
        }
        if (!writeTextFile(path, payload.readAll())) {
            return false;
        }
    }
    return true;
}

bool ToolManager::restoreBackup(const QString &backupId, AiTool tool)
{
    m_lastError.clear();
    const QString safetyBackupId = createBackup(tool);
    if (safetyBackupId.isEmpty()) {
        return false;
    }
    if (restoreBackupInternal(backupId, tool)) {
        pruneBackups(tool);
        return true;
    }

    const QString restoreError = m_lastError;
    const bool recovered = restoreBackupInternal(safetyBackupId, tool);
    pruneBackups(tool);
    m_lastError = recovered
        ? QStringLiteral("%1（当前配置已恢复）").arg(restoreError)
        : QStringLiteral("%1；恢复当前配置也失败：%2").arg(restoreError, m_lastError);
    return false;
}

bool ToolManager::writeTextFile(const QString &path, const QByteArray &data)
{
    QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        m_lastError = QStringLiteral("无法创建目录：%1").arg(dir.path());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }
    if (file.write(data) != data.size()) {
        m_lastError = QStringLiteral("配置文件写入不完整：%1").arg(path);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        m_lastError = QStringLiteral("无法提交配置文件：%1").arg(path);
        return false;
    }
    return true;
}

QString ToolManager::readConfiguredKey(AiTool tool) const
{
    switch (tool) {
    case AiTool::ClaudeCode: {
        QFile file(homeFilePath(QStringLiteral(".claude/settings.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        return root.value(QStringLiteral("env")).toObject()
            .value(QStringLiteral("ANTHROPIC_AUTH_TOKEN")).toString();
    }
    case AiTool::CodexCli: {
        QFile file(homeFilePath(QStringLiteral(".codex/auth.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QJsonDocument::fromJson(file.readAll()).object()
            .value(QStringLiteral("OPENAI_API_KEY")).toString();
    }
    case AiTool::GeminiCli: {
        QFile file(homeFilePath(QStringLiteral(".gemini/.env")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const QString content = QString::fromUtf8(file.readAll());
        for (const QString &line : content.split(QLatin1Char('\n'))) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
                continue;
            }
            const int separator = trimmed.indexOf(QLatin1Char('='));
            if (separator < 0
                    || trimmed.left(separator).trimmed() != QStringLiteral("GEMINI_API_KEY")) {
                continue;
            }
            QString value = trimmed.mid(separator + 1).trimmed();
            if (value.size() >= 2
                    && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                        || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
        return QString();
    }
    case AiTool::OpenCode: {
        QFile file(homeFilePath(QStringLiteral(".config/opencode/config.json")));
        if (!file.open(QIODevice::ReadOnly)) return QString();
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        return root.value(QStringLiteral("provider")).toObject()
            .value(QStringLiteral("anthropic")).toObject()
            .value(QStringLiteral("api_key")).toString();
    }
    }
    return QString();
}

// ── 核心检测（可指定超时）────────────────────────────────────────
ToolStatus ToolManager::detectWithTimeout(AiTool tool, int timeoutMs)
{
    ToolStatus status;
    status.nodeOk = commandExists(QStringLiteral("node"), timeoutMs);
    const QString executable = resolveCommand(cliCommand(tool), timeoutMs);
    status.installed = !executable.isEmpty();
    if (status.installed) {
        status.version = commandVersion(executable, timeoutMs);
    }

    // 兜底：PATH 里找不到时查 npm 全局包（有些环境 npm bin 不在 PATH）
    if (!status.installed && status.nodeOk) {
        status.version = npmPackageVersion(tool, timeoutMs);
        status.installed = !status.version.isEmpty();
    }

    switch (tool) {
    case AiTool::ClaudeCode: {
        const QString key = readConfiguredKey(tool);
        QFile file(homeFilePath(".claude/settings.json"));
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const QJsonObject env = root["env"].toObject();
            const QString baseUrl = env["ANTHROPIC_BASE_URL"].toString();
            if ((baseUrl.contains(QStringLiteral("aegisy.cc"))
                 || baseUrl.contains(QStringLiteral("127.0.0.1:43112")))
                    && !key.isEmpty()) {
                status.configured = true;
                status.configuredKey = key;
            }
        }
        const QString envApiKey = QString::fromLocal8Bit(qgetenv("ANTHROPIC_API_KEY"));
        const QString envToken = QString::fromLocal8Bit(qgetenv("ANTHROPIC_AUTH_TOKEN"));
        const QString envBase = QString::fromLocal8Bit(qgetenv("ANTHROPIC_BASE_URL"));
        if (!envApiKey.isEmpty()
                || (!envToken.isEmpty() && envToken != key)
                || (!envBase.isEmpty() && !envBase.contains(QStringLiteral("aegisy.cc")))) {
            status.conflictWarning =
                QStringLiteral("检测到旧的 Anthropic 环境变量，可能覆盖当前档案；Aegisy 启动终端时会自动清除，外部终端请删除后重启");
        }
        break;
    }
    case AiTool::CodexCli: {
        const QString key = readConfiguredKey(tool);
        QFile toml(homeFilePath(".codex/config.toml"));
        bool baseOk = false;
        if (toml.exists() && toml.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(toml.readAll());
            toml.close();
            baseOk = content.contains(QStringLiteral("aegisy.cc"))
                || content.contains(QStringLiteral("127.0.0.1:43112"));
        }
        status.configured = baseOk && !key.isEmpty();
        if (status.configured) status.configuredKey = key;
        // 检查 OPENAI_API_KEY 是否指向别的账号
        const QString envKey = QString::fromLocal8Bit(qgetenv("OPENAI_API_KEY"));
        const QString envBase = QString::fromLocal8Bit(qgetenv("OPENAI_BASE_URL"));
        const QString envApiBase = QString::fromLocal8Bit(qgetenv("OPENAI_API_BASE"));
        if ((!envKey.isEmpty() && (!status.configured || envKey != key))
                || (!envBase.isEmpty() && !envBase.contains(QStringLiteral("aegisy.cc")))
                || (!envApiBase.isEmpty() && !envApiBase.contains(QStringLiteral("aegisy.cc")))) {
            status.conflictWarning =
                QStringLiteral("检测到旧的 OpenAI 环境变量，可能覆盖当前档案；Aegisy 启动终端时会自动清除，外部终端请删除后重启");
        }
        break;
    }
    case AiTool::GeminiCli: {
        const QString key = readConfiguredKey(tool);
        QFile envFile(homeFilePath(".gemini/.env"));
        if (envFile.exists() && envFile.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(envFile.readAll());
            envFile.close();
            bool baseOk = false;
            for (const QString &line : content.split('\n')) {
                const QString t = line.trimmed();
                if (t.startsWith("GOOGLE_GEMINI_BASE_URL")
                        && (t.contains(QStringLiteral("aegisy.cc"))
                            || t.contains(QStringLiteral("127.0.0.1:43112")))) {
                    baseOk = true;
                }
            }
            status.configured = baseOk && !key.isEmpty();
            if (status.configured) status.configuredKey = key;
        }
        const QString envKey = QString::fromLocal8Bit(qgetenv("GEMINI_API_KEY"));
        const QString googleKey = QString::fromLocal8Bit(qgetenv("GOOGLE_API_KEY"));
        const QString envBase = QString::fromLocal8Bit(qgetenv("GOOGLE_GEMINI_BASE_URL"));
        if ((!envKey.isEmpty() && envKey != key) || !googleKey.isEmpty()
                || (!envBase.isEmpty() && !envBase.contains(QStringLiteral("aegisy.cc")))) {
            status.conflictWarning =
                QStringLiteral("检测到旧的 Gemini 环境变量，可能覆盖当前档案；Aegisy 启动终端时会自动清除，外部终端请删除后重启");
        }
        break;
    }
    case AiTool::OpenCode: {
        QFile file(homeFilePath(".config/opencode/config.json"));
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const QString key = root.value(QStringLiteral("provider")).toObject()
                .value(QStringLiteral("anthropic")).toObject()
                .value(QStringLiteral("api_key")).toString();
            const QString base = root.value(QStringLiteral("provider")).toObject()
                .value(QStringLiteral("anthropic")).toObject()
                .value(QStringLiteral("base_url")).toString();
            if (!key.isEmpty() && (base.contains(QStringLiteral("aegisy.cc"))
                    || base.contains(QStringLiteral("127.0.0.1:43112")))) {
                status.configured = true;
                status.configuredKey = key;
            }
        }
        break;
    }
    }

    return status;
}

ToolStatus ToolManager::detect(AiTool tool)
{
    return detectWithTimeout(tool, 5000);
}

ToolStatus ToolManager::detectFast(AiTool tool)
{
    return detectWithTimeout(tool, 2000);
}

void ToolManager::detectVersion(AiTool tool)
{
    const QString executable = resolveCommand(cliCommand(tool), 500);
    if (executable.isEmpty()) {
        detectNpmVersion(tool);
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessEnvironment(commandEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);

    const auto complete = [this, process, tool](bool installed, const QString &version) {
        if (process->property("aegisyVersionComplete").toBool()) {
            return;
        }
        process->setProperty("aegisyVersionComplete", true);
        emit toolVersionDetected(tool, installed, version);
        process->deleteLater();
    };

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [process, complete](int, QProcess::ExitStatus) {
        complete(true, extractVersion(QString::fromUtf8(process->readAll())));
    });
    connect(process, &QProcess::errorOccurred, this,
            [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            complete(false, QString());
        }
    });
    QTimer::singleShot(4000, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
    process->start(executable, QStringList() << QStringLiteral("--version"));
}

void ToolManager::checkLatestVersion(AiTool tool)
{
    const QString npmExecutable = resolveCommand(kNpmCmd, 800);
    if (npmExecutable.isEmpty()) {
        emit toolLatestVersionDetected(
            tool, false, QString(), QStringLiteral("未找到 npm"));
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessEnvironment(commandEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);

    const auto complete = [this, process, timeout, tool](bool success,
                                                         const QString &version,
                                                         const QString &error) {
        if (process->property("aegisyCompleted").toBool()) {
            return;
        }
        process->setProperty("aegisyCompleted", true);
        timeout->stop();
        emit toolLatestVersionDetected(tool, success, version, error);
        process->deleteLater();
    };

    connect(timeout, &QTimer::timeout, this, [process, complete]() {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
        complete(false, QString(), QStringLiteral("查询最新版本超时"));
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [process, complete](int exitCode, QProcess::ExitStatus exitStatus) {
        const QString output = QString::fromUtf8(process->readAll()).trimmed();
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            complete(false, QString(), output.isEmpty()
                ? QStringLiteral("npm registry 查询失败") : output.left(200));
            return;
        }
        const QString version = extractVersion(output);
        complete(!version.isEmpty(), version,
                 version.isEmpty() ? QStringLiteral("无法解析最新版本") : QString());
    });
    connect(process, &QProcess::errorOccurred, this,
            [process, complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            complete(false, QString(), process->errorString());
        }
    });

    timeout->start(10000);
    process->start(npmExecutable,
                   { QStringLiteral("view"), npmPackage(tool),
                     QStringLiteral("version"), QStringLiteral("--json") });
}

QString ToolManager::latestVersion(AiTool tool, int timeoutMs) const
{
    const QString npmExecutable = resolveCommand(kNpmCmd, 800);
    if (npmExecutable.isEmpty()) return QString();

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(npmExecutable,
                  { QStringLiteral("view"), npmPackage(tool),
                    QStringLiteral("version"), QStringLiteral("--json") });
    if (!process.waitForStarted(2000) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return QString();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return QString();
    }
    return extractVersion(QString::fromUtf8(process.readAll()).trimmed());
}

void ToolManager::detectNpmVersion(AiTool tool)
{
    const QString npmExecutable = resolveCommand(kNpmCmd, 500);
    if (npmExecutable.isEmpty()) {
        QTimer::singleShot(0, this, [this, tool]() {
            emit toolVersionDetected(tool, false, QString());
        });
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessEnvironment(commandEnvironment());

    const auto complete = [this, process, tool](const QString &version) {
        if (process->property("aegisyVersionComplete").toBool()) {
            return;
        }
        process->setProperty("aegisyVersionComplete", true);
        emit toolVersionDetected(tool, !version.isEmpty(), version);
        process->deleteLater();
    };

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [process, tool, complete](int, QProcess::ExitStatus) {
        complete(npmVersionFromJson(process->readAllStandardOutput(),
                                    ToolManager::npmPackage(tool)));
    });
    connect(process, &QProcess::errorOccurred, this,
            [complete](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            complete(QString());
        }
    });
    QTimer::singleShot(4000, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
    process->start(npmExecutable,
                   QStringList() << QStringLiteral("list") << QStringLiteral("-g")
                                 << npmPackage(tool) << QStringLiteral("--depth=0")
                                 << QStringLiteral("--json"));
}

// ── 桌面应用检测 ─────────────────────────────────────────────────
DesktopAppStatus ToolManager::detectDesktop(AiTool tool)
{
    DesktopAppStatus result;

    switch (tool) {
    case AiTool::ClaudeCode: {
        result.appName     = QStringLiteral("Claude 桌面版");
        result.downloadUrl = QStringLiteral("https://claude.ai/download");
#if defined(Q_OS_MAC)
        result.installed = QFile::exists("/Applications/Claude.app");
#elif defined(Q_OS_WIN)
        // 常见安装路径
        const QString localApp = qgetenv("LOCALAPPDATA");
        result.installed = QFile::exists(localApp + "\\Programs\\Claude\\claude.exe")
                        || QFile::exists(localApp + "\\Claude\\claude.exe");
#else
        // Linux：检查常见位置
        result.installed = commandExists("claude-desktop", 2000)
                        || QFile::exists(QDir::homePath() + "/.local/share/applications/claude.desktop");
#endif
        break;
    }
    case AiTool::CodexCli: {
        // Codex 本身是 CLI 工具；OpenAI 桌面客户端是 ChatGPT
        result.appName     = QStringLiteral("ChatGPT 桌面版");
        result.downloadUrl = QStringLiteral("https://chatgpt.com/download");
#if defined(Q_OS_MAC)
        result.installed = QFile::exists("/Applications/ChatGPT.app");
#elif defined(Q_OS_WIN)
        const QString localApp = qgetenv("LOCALAPPDATA");
        result.installed = QFile::exists(localApp + "\\Programs\\OpenAI\\ChatGPT\\chatgpt.exe");
#else
        result.installed = false;  // Linux 暂无官方桌面版
#endif
        break;
    }
    case AiTool::GeminiCli: {
        // Gemini 暂无独立桌面版，指向 Web
        result.appName     = QStringLiteral("Gemini Web");
        result.downloadUrl = QStringLiteral("https://gemini.google.com/");
        result.installed   = true;  // 网页版始终"可用"
        break;
    }
    case AiTool::OpenCode: {
        result.appName     = QStringLiteral("OpenCode");
        result.downloadUrl = QStringLiteral("https://opencode.ai");
        result.installed   = false;
        break;
    }
    }

    return result;
}

// ── 异步安装 ─────────────────────────────────────────────────────
void ToolManager::install(AiTool tool, int requestId)
{
    if (!resolveCommand(kNpmCmd, 1000).isEmpty()) {
        installCli(tool, requestId);
        return;
    }

    QString installer;
    QStringList arguments;
    QString displayCommand;

#if defined(Q_OS_WIN)
    installer = resolveCommand(QStringLiteral("winget.exe"), 1000);
    arguments = QStringList({
        QStringLiteral("install"), QStringLiteral("--id"),
        QStringLiteral("OpenJS.NodeJS.LTS"), QStringLiteral("--exact"),
        QStringLiteral("--source"), QStringLiteral("winget"),
        QStringLiteral("--accept-package-agreements"),
        QStringLiteral("--accept-source-agreements"),
        QStringLiteral("--silent"),
    });
    displayCommand = QStringLiteral("winget install OpenJS.NodeJS.LTS");
#elif defined(Q_OS_MAC)
    installer = resolveCommand(QStringLiteral("brew"), 1000);
    arguments = { QStringLiteral("install"), QStringLiteral("node") };
    displayCommand = QStringLiteral("brew install node");
#else
    const QString privilegeTool = resolveCommand(QStringLiteral("pkexec"), 1000);
    const QString apt = resolveCommand(QStringLiteral("apt-get"), 1000);
    const QString dnf = resolveCommand(QStringLiteral("dnf"), 1000);
    const QString pacman = resolveCommand(QStringLiteral("pacman"), 1000);
    if (!privilegeTool.isEmpty() && !apt.isEmpty()) {
        installer = privilegeTool;
        arguments = { apt, QStringLiteral("install"), QStringLiteral("-y"),
                      QStringLiteral("nodejs"), QStringLiteral("npm") };
        displayCommand = QStringLiteral("pkexec apt-get install -y nodejs npm");
    } else if (!privilegeTool.isEmpty() && !dnf.isEmpty()) {
        installer = privilegeTool;
        arguments = { dnf, QStringLiteral("install"), QStringLiteral("-y"),
                      QStringLiteral("nodejs"), QStringLiteral("npm") };
        displayCommand = QStringLiteral("pkexec dnf install -y nodejs npm");
    } else if (!privilegeTool.isEmpty() && !pacman.isEmpty()) {
        installer = privilegeTool;
        arguments = { pacman, QStringLiteral("-S"), QStringLiteral("--noconfirm"),
                      QStringLiteral("nodejs"), QStringLiteral("npm") };
        displayCommand = QStringLiteral("pkexec pacman -S --noconfirm nodejs npm");
    }
#endif

    if (installer.isEmpty()) {
        emit installOutput(
            tool,
            QStringLiteral("未找到可用的 Node.js 安装器，请先安装 Node.js LTS。"));
        emit installFinished(tool, requestId, false);
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessEnvironment(commandEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);

    const auto complete = [this, process, tool, requestId](bool success) {
        if (process->property("aegisyCompletionEmitted").toBool()) {
            return;
        }
        process->setProperty("aegisyCompletionEmitted", true);
        process->deleteLater();
        if (!success) {
            emit installFinished(tool, requestId, false);
            return;
        }
        emit installOutput(tool, QStringLiteral("Node.js 安装完成，正在安装 CLI..."));
        QTimer::singleShot(0, this, [this, tool, requestId]() {
            installCli(tool, requestId);
        });
    };

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, tool]() {
        const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (!text.isEmpty()) {
            emit installOutput(tool, text);
        }
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [complete](int exitCode, QProcess::ExitStatus exitStatus) {
        complete(exitStatus == QProcess::NormalExit && exitCode == 0);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, tool, complete](QProcess::ProcessError) {
        emit installOutput(
            tool, QStringLiteral("无法启动 Node.js 安装器：%1").arg(process->errorString()));
        complete(false);
    });

    emit installOutput(tool, QStringLiteral("$ %1").arg(displayCommand));
    process->start(installer, arguments);
}

void ToolManager::installCli(AiTool tool, int requestId)
{
    QProcess *process = new QProcess(this);
    process->setProcessEnvironment(commandEnvironment());
    process->setProcessChannelMode(QProcess::MergedChannels);

    const auto complete = [this, process, tool, requestId](bool success) {
        if (process->property("aegisyCompletionEmitted").toBool()) {
            return;
        }
        process->setProperty("aegisyCompletionEmitted", true);
        process->deleteLater();
        emit installFinished(tool, requestId, success);
    };

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, tool]() {
        const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (!text.isEmpty()) {
            emit installOutput(tool, text);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [complete](int exitCode, QProcess::ExitStatus exitStatus) {
        complete(exitStatus == QProcess::NormalExit && exitCode == 0);
    });

    connect(process, &QProcess::errorOccurred, this, [this, process, tool, complete](QProcess::ProcessError) {
        emit installOutput(tool, QStringLiteral("无法启动 npm：%1").arg(process->errorString()));
        complete(false);
    });

    const QString npmExecutable = resolveCommand(kNpmCmd, 1000);
    if (npmExecutable.isEmpty()) {
        emit installOutput(
            tool,
            QStringLiteral("Node.js 已安装，但当前进程仍未找到 npm，请重启应用后重试。"));
        complete(false);
        return;
    }

    emit installOutput(tool, QStringLiteral("$ %1 install -g %2").arg(kNpmCmd, npmPackage(tool)));
    process->start(npmExecutable, QStringList() << "install" << "-g" << npmPackage(tool));
}

// ── 配置写入 ─────────────────────────────────────────────────────
bool ToolManager::configure(AiTool tool, const QString &apiKey, const QString &model)
{
    m_lastError.clear();
    if (apiKey.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("API Key 不能为空");
        return false;
    }

    const QString backupId = createBackup(tool);
    if (backupId.isEmpty()) {
        return false;
    }

    bool success = false;
    switch (tool) {
    case AiTool::ClaudeCode: success = configureClaudeCode(apiKey, model); break;
    case AiTool::CodexCli:   success = configureCodexCli(apiKey, model); break;
    case AiTool::GeminiCli:  success = configureGeminiCli(apiKey, model); break;
    case AiTool::OpenCode:   success = configureOpenCode(apiKey, model); break;
    }
    if (!success) {
        const QString writeError = m_lastError;
        const bool rolledBack = restoreBackupInternal(backupId, tool);
        pruneBackups(tool);
        m_lastError = rolledBack
            ? QStringLiteral("%1（已自动回滚）").arg(writeError)
            : QStringLiteral("%1；自动回滚失败：%2").arg(writeError, m_lastError);
        return false;
    }

    if (readConfiguredKey(tool) != apiKey) {
        const QString validationError = QStringLiteral(
            "写入后校验失败：%1").arg(configFilePath(tool));
        const bool rolledBack = restoreBackupInternal(backupId, tool);
        pruneBackups(tool);
        m_lastError = rolledBack
            ? QStringLiteral("%1（已自动回滚）").arg(validationError)
            : QStringLiteral("%1；自动回滚失败：%2").arg(validationError, m_lastError);
        return false;
    }
    pruneBackups(tool);
    return true;
}

bool ToolManager::configureGateway(AiTool tool, const QString &localToken,
                                   const QString &model, int port)
{
    m_lastError.clear();
    if (localToken.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("本地网关令牌为空");
        return false;
    }
    const QString backupId = createBackup(tool);
    if (backupId.isEmpty()) return false;

    const QString root = QStringLiteral("http://127.0.0.1:%1/tools/").arg(port);
    bool success = false;
    switch (tool) {
    case AiTool::ClaudeCode:
        success = configureClaudeCodeEndpoint(localToken, root + QStringLiteral("claude"));
        break;
    case AiTool::CodexCli:
        success = configureCodexCliEndpoint(localToken, model,
            root + QStringLiteral("codex/v1"), QStringLiteral("aegisy_local"));
        break;
    case AiTool::GeminiCli:
        success = configureGeminiCliEndpoint(localToken, model,
            root + QStringLiteral("gemini"));
        break;
    case AiTool::OpenCode:
        success = configureOpenCodeEndpoint(localToken, model,
            root + QStringLiteral("claude"));
        break;
    }
    if (!success || readConfiguredKey(tool) != localToken) {
        const QString writeError = success
            ? QStringLiteral("本地网关配置写入后校验失败") : m_lastError;
        const bool rolledBack = restoreBackupInternal(backupId, tool);
        pruneBackups(tool);
        m_lastError = rolledBack
            ? QStringLiteral("%1（已自动回滚）").arg(writeError)
            : QStringLiteral("%1；自动回滚失败：%2").arg(writeError, m_lastError);
        return false;
    }
    pruneBackups(tool);
    return true;
}

bool ToolManager::configureClaudeCode(const QString &apiKey, const QString &/*model*/)
{
    return configureClaudeCodeEndpoint(apiKey, kBaseUrl);
}

bool ToolManager::configureClaudeCodeEndpoint(const QString &apiKey, const QString &baseUrl)
{
    // ~/.claude/settings.json：合并 env 三变量，保留其它字段
    const QString path = homeFilePath(".claude/settings.json");

    QJsonObject root;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    QJsonObject env = root["env"].toObject();
    env["ANTHROPIC_BASE_URL"]                      = baseUrl;
    env["ANTHROPIC_AUTH_TOKEN"]                    = apiKey;
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = QStringLiteral("1");
    root["env"] = env;

    return writeTextFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool ToolManager::configureCodexCli(const QString &apiKey, const QString &model)
{
    return configureCodexCliEndpoint(
        apiKey, model, kBaseUrl, QStringLiteral("OpenAI"));
}

bool ToolManager::configureCodexCliEndpoint(const QString &apiKey,
                                            const QString &model,
                                            const QString &baseUrl,
                                            const QString &providerId)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gpt-4o") : model;
    // 1) ~/.codex/auth.json
    const QString authPath = homeFilePath(".codex/auth.json");
    QJsonObject auth;
    {
        QFile f(authPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            auth = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
    }
    auth["OPENAI_API_KEY"] = apiKey;
    if (!writeTextFile(authPath, QJsonDocument(auth).toJson(QJsonDocument::Indented))) {
        return false;
    }

    // 2) ~/.codex/config.toml：移除我们管理的旧行/旧段，追加官方指南全量配置
    const QString tomlPath = homeFilePath(".codex/config.toml");
    QString existing;
    {
        QFile f(tomlPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            existing = QString::fromUtf8(f.readAll());
            f.close();
        }
    }
    static const QStringList managedTopKeys = {
        "model_provider", "model", "review_model", "model_reasoning_effort",
        "disable_response_storage", "network_access", "windows_wsl_setup_acknowledged",
    };
    static const QStringList managedSections = {
        "[model_providers.OpenAI]", "[model_providers.aegisy]",
        "[model_providers.aegisy_local]", "[features]",
    };

    QStringList outLines;
    bool skippingSection = false;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        if (t.startsWith('[')) {
            skippingSection = managedSections.contains(t);
            if (skippingSection) continue;
        }
        if (skippingSection) continue;
        bool managed = false;
        for (const QString &k : managedTopKeys) {
            if (t.startsWith(k) && t.mid(k.length()).trimmed().startsWith('=')) {
                managed = true;
                break;
            }
        }
        if (managed) continue;
        outLines.append(raw);
    }
    while (!outLines.isEmpty() && outLines.last().trimmed().isEmpty()) {
        outLines.removeLast();
    }

    QString result = outLines.join('\n');
    if (!result.isEmpty()) result += "\n\n";

    result += QStringLiteral(
        "model_provider = \"%2\"\n"
        "model = \"%1\"\n"
        "review_model = \"%1\"\n").arg(effectiveModel, providerId);
    result += QStringLiteral(
        "model_reasoning_effort = \"xhigh\"\n"
        "disable_response_storage = true\n"
        "network_access = \"enabled\"\n"
        "windows_wsl_setup_acknowledged = true\n"
        "\n"
        "[model_providers.%2]\n"
        "name = \"%2\"\n"
        "base_url = \"%1\"\n"
        "wire_api = \"responses\"\n"
        "requires_openai_auth = true\n"
        "\n"
        "[features]\n"
        "goals = true\n").arg(baseUrl, providerId);

    return writeTextFile(tomlPath, result.toUtf8());
}

bool ToolManager::configureGeminiCli(const QString &apiKey, const QString &model)
{
    return configureGeminiCliEndpoint(apiKey, model, kBaseUrl);
}

bool ToolManager::configureGeminiCliEndpoint(const QString &apiKey,
                                             const QString &model,
                                             const QString &baseUrl)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gemini-2.5-pro") : model;
    const QString path = homeFilePath(".gemini/.env");

    QString existing;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        existing = QString::fromUtf8(file.readAll());
        file.close();
    }

    static const QStringList managedKeys = {
        "GOOGLE_GEMINI_BASE_URL", "GEMINI_API_KEY", "GEMINI_MODEL",
    };

    QStringList outLines;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        const int separator = t.indexOf(QLatin1Char('='));
        const QString variable = separator >= 0 ? t.left(separator).trimmed() : QString();
        const bool managed = managedKeys.contains(variable);
        if (!managed) outLines.append(raw);
    }
    while (!outLines.isEmpty() && outLines.last().trimmed().isEmpty()) {
        outLines.removeLast();
    }

    QString result = outLines.join('\n');
    if (!result.isEmpty()) result += "\n";
    result += QStringLiteral(
        "GOOGLE_GEMINI_BASE_URL=\"%1\"\n"
        "GEMINI_API_KEY=\"%2\"\n"
        "GEMINI_MODEL=\"%3\"\n").arg(baseUrl, apiKey, effectiveModel);

    return writeTextFile(path, result.toUtf8());
}

// ── OpenCode 配置写入 ─────────────────────────────────────────────
bool ToolManager::configureOpenCode(const QString &apiKey, const QString &model)
{
    return configureOpenCodeEndpoint(apiKey, model, kBaseUrl);
}

bool ToolManager::configureOpenCodeEndpoint(const QString &apiKey,
                                             const QString &model,
                                             const QString &baseUrl)
{
    const QString effectiveModel = model.isEmpty()
        ? QStringLiteral("anthropic/claude-opus-4-5") : model;
    const QString path = homeFilePath(QStringLiteral(".config/opencode/config.json"));

    QJsonObject root;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    QJsonObject providers = root.value(QStringLiteral("provider")).toObject();
    QJsonObject anthropic = providers.value(QStringLiteral("anthropic")).toObject();
    anthropic[QStringLiteral("api_key")]  = apiKey;
    anthropic[QStringLiteral("base_url")] = baseUrl;
    providers[QStringLiteral("anthropic")] = anthropic;
    root[QStringLiteral("provider")] = providers;
    root[QStringLiteral("model")]    = effectiveModel;

    return writeTextFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}
