#include "tool_manager.h"
#include "credential_metadata.h"
#include "process_command.h"
#include "secure_storage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVersionNumber>
#include <algorithm>

#include <openssl/crypto.h>
#include <openssl/rand.h>

// 官方指南规定：BASE_URL 一律裸域名，不带 /v1
static const QString kBaseUrl = "https://aegisy.cc";
constexpr int kMaxBackupsPerTool = 10;
static const QString kCodexCapabilityHeader = QStringLiteral("x-openai-actor-authorization");
static const QString kCodexCapabilityHeaderValue = QStringLiteral("aegisy");
static const QString kCodexEncodingHeader = QStringLiteral("accept-encoding");
static const QString kCodexEncodingHeaderValue = QStringLiteral("identity");

static bool usesGpt56CompatibilityProfile(const QString &model)
{
    const QString normalized = model.trimmed().toLower();
    return normalized == QStringLiteral("gpt-5.6")
        || normalized.startsWith(QStringLiteral("gpt-5.6-sol"));
}

static QString tomlBasicString(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\b'), QStringLiteral("\\b"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\f'), QStringLiteral("\\f"));
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

static bool hasCodexCapabilityHeader(const QString &inlineTable)
{
    static const QRegularExpression headerPattern(QStringLiteral(
        R"((?:^|[,{])\s*["']?x-openai-actor-authorization["']?\s*=\s*["']aegisy["'])"),
        QRegularExpression::CaseInsensitiveOption);
    return headerPattern.match(inlineTable).hasMatch();
}

static bool hasCodexIdentityEncodingHeader(const QString &inlineTable)
{
    static const QRegularExpression headerPattern(QStringLiteral(
        R"((?:^|[,{])\s*["']?accept-encoding["']?\s*=\s*["']identity["'])"),
        QRegularExpression::CaseInsensitiveOption);
    return headerPattern.match(inlineTable).hasMatch();
}

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

static QString maskedCredentialHint(const QString &credential)
{
    return credentialFingerprint(credential);
}

static bool isAegisyBaseUrl(const QString &value,
                            const QString &gatewayPath,
                            bool *gatewayMode = nullptr)
{
    const QUrl url(value.trimmed());
    const bool gateway = url.host() == QStringLiteral("127.0.0.1")
        && url.path() == gatewayPath;
    const bool direct = url.scheme() == QStringLiteral("https")
        && (url.host() == QStringLiteral("aegisy.cc")
            || url.host() == QStringLiteral("www.aegisy.cc"));
    if (gatewayMode) {
        *gatewayMode = gateway;
    }
    return direct || gateway;
}

static QString configScalar(QString value, bool *ok = nullptr)
{
    value = value.trimmed();
    bool valid = !value.isEmpty();
    QString result;
    if (valid && (value.startsWith(QLatin1Char('"'))
                  || value.startsWith(QLatin1Char('\'')))) {
        const QChar quote = value.front();
        int closing = -1;
        bool escaped = false;
        for (int i = 1; i < value.size(); ++i) {
            const QChar current = value.at(i);
            if (quote == QLatin1Char('"') && current == QLatin1Char('\\') && !escaped) {
                escaped = true;
                continue;
            }
            if (current == quote && !escaped) {
                closing = i;
                break;
            }
            escaped = false;
        }
        const QString trailing = closing >= 0
            ? value.mid(closing + 1).trimmed() : QString();
        valid = closing >= 0
            && (trailing.isEmpty() || trailing.startsWith(QLatin1Char('#')));
        if (valid) {
            result = value.mid(1, closing - 1);
        }
    } else if (valid) {
        const int comment = value.indexOf(QLatin1Char('#'));
        result = (comment >= 0 ? value.left(comment) : value).trimmed();
        valid = !result.isEmpty();
    }
    if (ok) {
        *ok = valid;
    }
    return valid ? result : QString();
}

#ifdef Q_OS_WIN
static const QString kNpmCmd  = "npm.cmd";
static const QString kWhichCmd = "where";
#else
static const QString kNpmCmd  = "npm";
static const QString kWhichCmd = "which";
#endif

static QString normalizedSearchPath(QString path)
{
    path = path.trimmed();
    if (path.size() >= 2 && path.front() == QLatin1Char('"')
            && path.back() == QLatin1Char('"')) {
        path = path.mid(1, path.size() - 2).trimmed();
    }
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

static void addSearchPath(QStringList &paths, const QString &path, bool prepend = false)
{
    const QString normalized = normalizedSearchPath(path);
    if (normalized.isEmpty() || !QDir(normalized).exists()) return;

#ifdef Q_OS_WIN
    const bool duplicate = std::any_of(
        paths.cbegin(), paths.cend(), [&normalized](const QString &candidate) {
            return candidate.compare(normalized, Qt::CaseInsensitive) == 0;
        });
#else
    const bool duplicate = paths.contains(normalized);
#endif
    if (duplicate) return;

    if (prepend) {
        paths.prepend(normalized);
    } else {
        paths.append(normalized);
    }
}

static QStringList commandSearchPaths()
{
#ifdef AEGISY_TOOL_MANAGER_RUNTIME_TEST
    const QString isolatedPath = qEnvironmentVariable(
        "AEGISY_TOOL_MANAGER_TEST_PATH").trimmed();
    if (!isolatedPath.isEmpty()) {
        QStringList isolatedPaths;
        addSearchPath(isolatedPaths, isolatedPath);
        return isolatedPaths;
    }
#endif
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList paths;
    const QStringList inheritedPaths = environment.value(QStringLiteral("PATH"))
        .split(QDir::listSeparator(), Qt::SkipEmptyParts);
    for (const QString &path : inheritedPaths) addSearchPath(paths, path);
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
    const QString appData = QString::fromLocal8Bit(qgetenv("APPDATA"));
    const QString localAppData = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
    const QString programFiles = QString::fromLocal8Bit(qgetenv("ProgramFiles"));
    const QString programFilesX86 =
        QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"));
    const QString npmPrefix = QString::fromLocal8Bit(qgetenv("npm_config_prefix"));
    const QString nvmSymlink = QString::fromLocal8Bit(qgetenv("NVM_SYMLINK"));
    const auto childPath = [](const QString &base, const QString &child) {
        return base.isEmpty() ? QString() : QDir(base).filePath(child);
    };
    const QStringList preferredPaths = {
        childPath(appData, QStringLiteral("npm")),
        npmPrefix,
        nvmSymlink,
        childPath(programFiles, QStringLiteral("nodejs")),
        childPath(programFilesX86, QStringLiteral("nodejs")),
        childPath(localAppData, QStringLiteral("Programs/nodejs")),
        childPath(localAppData, QStringLiteral("Microsoft/WinGet/Links")),
    };
    for (auto it = preferredPaths.crbegin(); it != preferredPaths.crend(); ++it) {
        addSearchPath(paths, *it, true);
    }
#else
    addSearchPath(paths, home + QStringLiteral("/.local/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.volta/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.npm-global/bin"), true);
    addSearchPath(paths, home + QStringLiteral("/.asdf/shims"), true);
#endif
    return paths;
}

static bool isNpmBusyError(const QString &output)
{
    return output.contains(QStringLiteral("EBUSY"), Qt::CaseInsensitive)
        || output.contains(QStringLiteral("resource busy or locked"),
                           Qt::CaseInsensitive);
}

#ifdef Q_OS_WIN
static bool isWindowsCommandFile(const QFileInfo &file)
{
    if (!file.isFile()) return false;
    const QString suffix = file.suffix();
    return suffix.compare(QStringLiteral("com"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("exe"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("cmd"), Qt::CaseInsensitive) == 0
        || suffix.compare(QStringLiteral("bat"), Qt::CaseInsensitive) == 0;
}

static QString findWindowsCommand(const QString &command,
                                  const QStringList &searchPaths)
{
    const QFileInfo commandInfo(command);
    QStringList names;
    if (commandInfo.suffix().isEmpty()) {
        // npm installs both an extensionless POSIX shim and a .cmd shim.
        // CreateProcess cannot run the POSIX shim, so never select it on Windows.
        for (const QString &extension : {
                 QStringLiteral(".com"), QStringLiteral(".exe"),
                 QStringLiteral(".cmd"), QStringLiteral(".bat") }) {
            names.append(command + extension);
        }
    } else {
        names.append(command);
    }

    const bool containsDirectory = commandInfo.isAbsolute()
        || command.contains(QLatin1Char('/')) || command.contains(QLatin1Char('\\'));
    if (containsDirectory) {
        for (const QString &name : names) {
            const QFileInfo candidate(name);
            if (isWindowsCommandFile(candidate)) return candidate.absoluteFilePath();
        }
        return QString();
    }

    for (const QString &path : searchPaths) {
        const QDir directory(path);
        for (const QString &name : names) {
            const QFileInfo candidate(directory.filePath(name));
            if (isWindowsCommandFile(candidate)) return candidate.absoluteFilePath();
        }
    }
    return QString();
}
#endif

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

class SecureStorageConfigurationBackupKeyProvider final
    : public ConfigurationBackupKeyProvider
{
public:
    bool keyForScope(const QString &scope, bool allowCreate,
                     QByteArray *key, QString *error) override
    {
        static const QRegularExpression scopePattern(QStringLiteral(
            "^tool-manager/config-backup-master/v1/"
            "(claude|codex|gemini|opencode)$"));
        if (key) key->clear();
        if (!key || !scopePattern.match(scope).hasMatch()
                || !SecureStorage::isAvailable()) {
            if (error) *error = QStringLiteral("configuration-backup-key-unavailable");
            return false;
        }

        const auto decode = [](const QString &encoded, QByteArray *decoded) {
            const QByteArray latin = encoded.toLatin1();
            if (QString::fromLatin1(latin) != encoded) return false;
            const QByteArray value = QByteArray::fromBase64(latin);
            if (value.size() != 32 || value.toBase64() != latin) return false;
            *decoded = value;
            return true;
        };

        const QString stored = SecureStorage::loadEncrypted(scope);
        if (!stored.isEmpty()) {
            if (!decode(stored, key)) {
                if (error) *error = QStringLiteral("configuration-backup-key-invalid");
                return false;
            }
            return true;
        }
        if (!allowCreate) {
            if (error) *error = QStringLiteral("configuration-backup-key-unavailable");
            return false;
        }

        QByteArray generated(32, '\0');
        if (RAND_bytes(reinterpret_cast<unsigned char *>(generated.data()),
                       generated.size()) != 1) {
            OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
            if (error) *error = QStringLiteral("configuration-backup-random-failed");
            return false;
        }
        const QString encoded = QString::fromLatin1(generated.toBase64());
        if (!SecureStorage::saveEncrypted(scope, encoded)
                || SecureStorage::loadEncrypted(scope) != encoded
                || !decode(encoded, key)) {
            OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
            if (error) *error = QStringLiteral("configuration-backup-key-write-failed");
            return false;
        }
        OPENSSL_cleanse(generated.data(), static_cast<size_t>(generated.size()));
        return true;
    }
};

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

ToolManager::ToolManager(
        QObject *parent, ConfigurationBackupKeyProvider *backupKeyProvider,
        const QString &backupRootOverride)
    : QObject(parent)
    , m_backupRootOverride(backupRootOverride.isEmpty()
          ? QString() : QDir::cleanPath(backupRootOverride))
    , m_backupKeyProvider(backupKeyProvider)
{
    if (!m_backupKeyProvider) {
        m_ownedBackupKeyProvider =
            std::make_unique<SecureStorageConfigurationBackupKeyProvider>();
        m_backupKeyProvider = m_ownedBackupKeyProvider.get();
    }
}

ToolManager::~ToolManager()
{
    clearCapturedConfigurationWrites();
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

qint64 ToolManager::configuredContextLimit(AiTool tool, const QString &model)
{
    if (tool != AiTool::CodexCli) return -1;
    return usesGpt56CompatibilityProfile(model)
        ? CodexGpt56ContextLimit : CodexConfiguredContextLimit;
}

QString ToolManager::configuredReasoning(AiTool tool, const QString &model)
{
    if (tool != AiTool::CodexCli) return QString();
    return usesGpt56CompatibilityProfile(model)
        ? QStringLiteral("high") : QStringLiteral("xhigh");
}

bool ToolManager::commandExists(const QString &command, int timeoutMs)
{
    return !resolveCommand(command, timeoutMs).isEmpty();
}

QString ToolManager::resolveCommand(const QString &command, int timeoutMs) const
{
    const QFileInfo directPath(command);
#ifdef Q_OS_WIN
    if (directPath.isAbsolute() && isWindowsCommandFile(directPath)) {
#else
    if (directPath.isAbsolute() && directPath.exists() && directPath.isExecutable()) {
#endif
        return directPath.absoluteFilePath();
    }

    const QStringList searchPaths = commandSearchPaths();
#ifdef Q_OS_WIN
    const QString executable = findWindowsCommand(command, searchPaths);
#else
    const QString executable = QStandardPaths::findExecutable(command, searchPaths);
#endif
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
    const QStringList matches = QString::fromLocal8Bit(process.readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    for (const QString &match : matches) {
        const QFileInfo candidate(match.trimmed());
        if (isWindowsCommandFile(candidate)) return candidate.absoluteFilePath();
    }
    return QString();
#else
    return matches.value(0).trimmed();
#endif
}

QString ToolManager::commandVersion(const QString &executable, int timeoutMs) const
{
    if (executable.isEmpty()) {
        return QString();
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    process.setProcessChannelMode(QProcess::MergedChannels);
    ProcessCommand::start(&process, executable, { QStringLiteral("--version") });
    if (!process.waitForStarted(qMin(timeoutMs, 1000))) {
        return QString();
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return QString();
    }
    return extractVersion(ProcessCommand::decodeOutput(process.readAll()));
}

QString ToolManager::npmPackageVersion(AiTool tool, int timeoutMs) const
{
    const QString npmExecutable = resolveCommand(kNpmCmd, timeoutMs);
    if (npmExecutable.isEmpty()) {
        return QString();
    }

    QProcess process;
    process.setProcessEnvironment(commandEnvironment());
    ProcessCommand::start(&process, npmExecutable,
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
        preview.changes.append(QStringLiteral(
            "启用第三方 Provider 兼容：关闭官方账号鉴权并写入能力请求头"));
        preview.changes.append(QStringLiteral("启用实时 Web Search（web_search = live）"));
        preview.changes.append(QStringLiteral("上下文：%1 / 思考深度：%2")
            .arg(configuredContextLimit(tool, model))
            .arg(configuredReasoning(tool, model)));
        if (!gatewayMode) {
            preview.warnings.append(QStringLiteral(
                "Codex 自定义 Provider 不读取 auth.json；直连 Key 将同步写入 config.toml"));
        }
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
                ? QStringLiteral("http://127.0.0.1:43112/tools/opencode")
                : QStringLiteral("https://aegisy.cc")));
        preview.changes.append(gatewayMode
            ? QStringLiteral("provider.anthropic.api_key 仅写入本地网关令牌")
            : QStringLiteral("更新 provider.anthropic.api_key"));
        preview.changes.append(QStringLiteral("模型：%1").arg(targetModel));
        break;
    }

    const ToolStatus status = detectFast(tool);
    if (!status.nodeOk) {
        preview.changes.prepend(QStringLiteral(
            "安装 Node.js LTS 与 npm，成功后再安装目标 CLI"));
    }
    if (!status.installed) {
        preview.changes.prepend(status.repairRequired
            ? QStringLiteral("修复 %1 的残缺安装").arg(toolName(tool))
            : QStringLiteral("安装并验证 %1").arg(toolName(tool)));
        preview.warnings.append(QStringLiteral(
            "本次激活需要先安装或修复 %1；安装失败不会切换活动档案或写入配置")
            .arg(toolName(tool)));
    }
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
    const LocalConfigurationStatus configuration = inspectConfiguration(tool);
    if (!configuration.isReady()) {
        m_lastError = configuration.detail;
        return false;
    }
    const QString executable = resolveCommand(cliCommand(tool), 1500);
    if (executable.isEmpty() || commandVersion(executable, 2000).isEmpty()) {
        m_lastError = QStringLiteral(
            "%1 CLI 不完整或无法运行，请在系统体检中点击“安装/修复”。")
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
    QStringList imageNames = { command + QStringLiteral(".exe") };
    if (tool == AiTool::CodexCli) {
        imageNames.append(QStringLiteral("codex-code-mode-host.exe"));
    }
    for (const QString &imageName : imageNames) {
        process.start(QStringLiteral("tasklist"),
                      { QStringLiteral("/FI"),
                        QStringLiteral("IMAGENAME eq %1").arg(imageName),
                        QStringLiteral("/NH") });
        if (!process.waitForFinished(2000)) {
            process.kill();
            process.waitForFinished();
            continue;
        }
        const QString output = QString::fromLocal8Bit(process.readAll());
        if (output.contains(imageName, Qt::CaseInsensitive)) return true;
    }
    return false;
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
    const QString configuredHome = QString::fromLocal8Bit(
        qgetenv("AEGISY_CONFIG_HOME")).trimmed();
    const QString home = configuredHome.isEmpty()
        ? QDir::homePath() : QDir::cleanPath(configuredHome);
    return QDir(home).filePath(relative);
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

QStringList ToolManager::configurationFiles(AiTool tool) const
{
    return managedConfigPaths(tool);
}

QString ToolManager::backupRootPath(AiTool tool) const
{
    const QString base = m_backupRootOverride.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/backups")
        : m_backupRootOverride;
    return QDir(base).filePath(toolSlug(tool));
}

void ToolManager::cleanseSnapshot(ConfigurationBackupSnapshot *snapshot)
{
    if (!snapshot) return;
    for (ConfigurationBackupFile &file : snapshot->files) {
        if (!file.content.isEmpty()) {
            OPENSSL_cleanse(file.content.data(), static_cast<size_t>(file.content.size()));
            file.content.clear();
        }
    }
    snapshot->files.clear();
}

bool ToolManager::snapshotsHaveSameFiles(
        const ConfigurationBackupSnapshot &left,
        const ConfigurationBackupSnapshot &right)
{
    if (left.tool != right.tool || left.files.size() != right.files.size()) return false;
    for (int i = 0; i < left.files.size(); ++i) {
        const ConfigurationBackupFile &a = left.files.at(i);
        const ConfigurationBackupFile &b = right.files.at(i);
        if (a.slot != b.slot || a.existed != b.existed || a.content != b.content) {
            return false;
        }
    }
    return true;
}

QString ToolManager::snapshotFilesIdentity(
        const ConfigurationBackupSnapshot &snapshot)
{
    QByteArray input = QByteArrayLiteral("aegisy-configuration-files/0.1\0");
    const auto append = [&input](const QByteArray &value) {
        const quint64 size = static_cast<quint64>(value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            input.append(static_cast<char>((size >> shift) & 0xff));
        }
        input.append(value);
    };
    append(snapshot.tool.toUtf8());
    for (const ConfigurationBackupFile &file : snapshot.files) {
        append(QByteArray::number(file.slot));
        append(file.existed ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
        append(QByteArray::number(file.content.size()));
        append(QCryptographicHash::hash(
            file.content, QCryptographicHash::Sha256).toHex());
    }
    return QStringLiteral("configuration-files:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

bool ToolManager::captureConfigurationSnapshot(
        AiTool tool, const QString &backupId, const QDateTime &createdAt,
        ConfigurationBackupSnapshot *snapshot, QString *error) const
{
    if (snapshot) *snapshot = ConfigurationBackupSnapshot();
    const QStringList paths = managedConfigPaths(tool);
    if (!snapshot || !ConfigurationBackupStore::isValidBackupId(backupId)
            || paths.isEmpty() || paths.size() > ConfigurationBackupStore::MaxFiles) {
        if (error) *error = QStringLiteral("configuration-backup-capture-invalid");
        return false;
    }

    ConfigurationBackupSnapshot captured;
    captured.backupId = backupId;
    captured.tool = toolSlug(tool);
    captured.createdAt = createdAt.toUTC();
    qint64 aggregate = 0;
    for (int slot = 0; slot < paths.size(); ++slot) {
        const QString &path = paths.at(slot);
        const QFileInfo before(path);
        if (before.isSymLink()) {
            if (error) *error = QStringLiteral("configuration-backup-source-invalid");
            cleanseSnapshot(&captured);
            return false;
        }
        if (!before.exists()) {
            captured.files.append({ slot, false, QByteArray() });
            continue;
        }
        if (!before.isFile() || before.size() < 0
                || before.size() > ConfigurationBackupStore::MaxFileBytes) {
            if (error) *error = QStringLiteral("configuration-backup-source-invalid");
            cleanseSnapshot(&captured);
            return false;
        }
        const auto readBounded = [&path](QByteArray *bytes) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return false;
            *bytes = file.read(ConfigurationBackupStore::MaxFileBytes + 1);
            return bytes->size() <= ConfigurationBackupStore::MaxFileBytes;
        };
        QByteArray first;
        QByteArray second;
        if (!readBounded(&first)) {
            first.fill('\0');
            if (error) *error = QStringLiteral("configuration-backup-source-unavailable");
            cleanseSnapshot(&captured);
            return false;
        }
        const QFileInfo middle(path);
        if (middle.isSymLink() || !middle.isFile() || middle.size() != first.size()
                || middle.size() != before.size()
                || middle.lastModified() != before.lastModified()
                || !readBounded(&second)) {
            first.fill('\0');
            second.fill('\0');
            if (error) *error = QStringLiteral("configuration-backup-source-drifted");
            cleanseSnapshot(&captured);
            return false;
        }
        const QFileInfo after(path);
        if (after.isSymLink() || !after.isFile() || after.size() != second.size()
                || after.lastModified() != middle.lastModified() || first != second
                || aggregate > ConfigurationBackupStore::MaxPayloadBytes - second.size()) {
            first.fill('\0');
            second.fill('\0');
            if (error) *error = QStringLiteral("configuration-backup-source-drifted");
            cleanseSnapshot(&captured);
            return false;
        }
        aggregate += second.size();
        first.fill('\0');
        captured.files.append({ slot, true, second });
    }
    *snapshot = captured;
    return true;
}

ConfigBackupInventory ToolManager::backupInventory(AiTool tool) const
{
    ConfigurationBackupStore store(backupRootPath(tool), m_backupKeyProvider);
    const ConfigurationBackupInventoryResult source = store.inventory(
        toolSlug(tool), static_cast<int>(tool), managedConfigPaths(tool));
    ConfigBackupInventory result;
    switch (source.state) {
    case ConfigurationBackupInventoryState::Empty:
        result.state = ConfigBackupSubsystemState::Empty;
        break;
    case ConfigurationBackupInventoryState::Ready:
        result.state = ConfigBackupSubsystemState::Ready;
        break;
    case ConfigurationBackupInventoryState::Unavailable:
        result.state = ConfigBackupSubsystemState::Unavailable;
        break;
    case ConfigurationBackupInventoryState::Invalid:
        result.state = ConfigBackupSubsystemState::Invalid;
        break;
    }
    result.errorCode = source.issue;
    if (result.state == ConfigBackupSubsystemState::Ready) {
        for (const ConfigurationBackupInventoryEntry &entry : source.entries) {
            ConfigBackup backup;
            backup.id = entry.backupId;
            backup.tool = tool;
            backup.createdAt = entry.createdAt;
            backup.fileCount = entry.fileCount;
            backup.manifestIdentity = entry.identity;
            result.backups.append(backup);
        }
    }
    return result;
}

QList<ConfigBackup> ToolManager::backupHistory(AiTool tool) const
{
    return backupInventory(tool).backups;
}

QString ToolManager::createBackup(
        AiTool tool, ConfigurationBackupSnapshot *verifiedSnapshot)
{
    if (verifiedSnapshot) *verifiedSnapshot = ConfigurationBackupSnapshot();
    const ConfigBackupInventory inventory = backupInventory(tool);
    if (inventory.state != ConfigBackupSubsystemState::Empty
            && inventory.state != ConfigBackupSubsystemState::Ready) {
        m_lastError = QStringLiteral("安全备份子系统不可用：%1")
            .arg(inventory.errorCode.isEmpty()
                ? QStringLiteral("configuration-backup-inventory-invalid")
                : inventory.errorCode);
        return QString();
    }
    if (inventory.backups.size() >= ConfigurationBackupStore::MaxBackups) {
        m_lastError = QStringLiteral("安全备份数量已达上限，未修改配置");
        return QString();
    }

    const QString id = QStringLiteral("%1_%2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
             QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    ConfigurationBackupSnapshot captured;
    QString error;
    if (!captureConfigurationSnapshot(
            tool, id, QDateTime::currentDateTimeUtc(), &captured, &error)) {
        m_lastError = QStringLiteral("无法读取安全备份源：%1").arg(error);
        return QString();
    }

    ConfigurationBackupStore store(backupRootPath(tool), m_backupKeyProvider);
    ConfigurationBackupSnapshot verified;
    if (!store.create(captured, &error)
            || !store.read(toolSlug(tool), id, &verified, &error)
            || verified.backupId != captured.backupId
            || verified.createdAt != captured.createdAt
            || !snapshotsHaveSameFiles(captured, verified)) {
        cleanseSnapshot(&captured);
        cleanseSnapshot(&verified);
        m_lastError = QStringLiteral("无法创建可验证安全备份：%1")
            .arg(error.isEmpty()
                ? QStringLiteral("configuration-backup-readback-mismatch") : error);
        return QString();
    }
    cleanseSnapshot(&captured);
    if (verifiedSnapshot) {
        *verifiedSnapshot = verified;
    } else {
        cleanseSnapshot(&verified);
    }
    return id;
}

bool ToolManager::readBackup(
        const QString &backupId, AiTool tool,
        ConfigurationBackupSnapshot *snapshot)
{
    if (snapshot) *snapshot = ConfigurationBackupSnapshot();
    const ConfigBackupInventory inventory = backupInventory(tool);
    if (inventory.state != ConfigBackupSubsystemState::Ready
            || std::none_of(
                inventory.backups.cbegin(), inventory.backups.cend(),
                [&backupId](const ConfigBackup &backup) { return backup.id == backupId; })) {
        m_lastError = QStringLiteral("安全备份不可读取：%1")
            .arg(inventory.errorCode.isEmpty()
                ? QStringLiteral("configuration-backup-not-found")
                : inventory.errorCode);
        return false;
    }
    QString error;
    ConfigurationBackupStore store(backupRootPath(tool), m_backupKeyProvider);
    if (!store.read(toolSlug(tool), backupId, snapshot, &error)) {
        m_lastError = QStringLiteral("安全备份认证失败：%1").arg(error);
        return false;
    }
    return true;
}

bool ToolManager::restoreBackupInternal(
        const ConfigurationBackupSnapshot &snapshot, AiTool tool)
{
    const QStringList paths = managedConfigPaths(tool);
    if (snapshot.tool != toolSlug(tool) || snapshot.files.size() != paths.size()) {
        m_lastError = QStringLiteral("备份类型或文件数量与目标终端不匹配");
        return false;
    }
    qint64 aggregate = 0;
    for (int i = 0; i < snapshot.files.size(); ++i) {
        const ConfigurationBackupFile &file = snapshot.files.at(i);
        const QFileInfo current(paths.at(i));
        if (file.slot != i || (!file.existed && !file.content.isEmpty())
                || file.content.size() > ConfigurationBackupStore::MaxFileBytes
                || aggregate > ConfigurationBackupStore::MaxPayloadBytes
                    - file.content.size()
                || current.isSymLink() || (current.exists() && !current.isFile())) {
            m_lastError = QStringLiteral("备份内容或目标路径未通过完整校验");
            return false;
        }
        aggregate += file.content.size();
    }

    for (int i = 0; i < snapshot.files.size(); ++i) {
        const ConfigurationBackupFile &file = snapshot.files.at(i);
        const QString &path = paths.at(i);
        if (!file.existed) {
            if (QFileInfo::exists(path) && !QFile::remove(path)) {
                m_lastError = QStringLiteral("无法恢复备份中的缺失文件状态：%1").arg(path);
                return false;
            }
        } else if (!writeTextFile(path, file.content)) {
            return false;
        }
    }
    return true;
}

bool ToolManager::pruneBackups(AiTool tool)
{
    const ConfigBackupInventory inventory = backupInventory(tool);
    if (inventory.state == ConfigBackupSubsystemState::Empty) return true;
    if (inventory.state != ConfigBackupSubsystemState::Ready) {
        m_lastWarning = QStringLiteral("备份保留清理已跳过：%1")
            .arg(inventory.errorCode.isEmpty()
                ? QStringLiteral("configuration-backup-inventory-invalid")
                : inventory.errorCode);
        return false;
    }

    const ConfigurationBackupInventoryResult source = ConfigurationBackupStore(
        backupRootPath(tool), m_backupKeyProvider).inventory(
            toolSlug(tool), static_cast<int>(tool), managedConfigPaths(tool));
    if (source.state != ConfigurationBackupInventoryState::Ready) {
        m_lastWarning = QStringLiteral("备份保留清理已跳过：%1").arg(source.issue);
        return false;
    }
    ConfigurationBackupStore store(backupRootPath(tool), m_backupKeyProvider);
    for (int i = kMaxBackupsPerTool; i < source.entries.size(); ++i) {
        QString error;
        const ConfigurationBackupInventoryEntry &entry = source.entries.at(i);
        if (!store.removeVerified(
                toolSlug(tool), entry.backupId, entry.identity, &error)) {
            m_lastWarning = QStringLiteral("备份保留清理失败：%1").arg(error);
            return false;
        }
    }
    return true;
}

bool ToolManager::restoreBackup(const QString &backupId, AiTool tool)
{
    m_lastError.clear();
    m_lastWarning.clear();
    ConfigurationBackupSnapshot target;
    if (!readBackup(backupId, tool, &target)) return false;

    ConfigurationBackupSnapshot safety;
    const QString safetyBackupId = createBackup(tool, &safety);
    if (safetyBackupId.isEmpty()) {
        cleanseSnapshot(&target);
        m_lastError = QStringLiteral("无法创建恢复前安全备份，当前配置未修改：%1")
            .arg(m_lastError);
        return false;
    }

    ConfigurationBackupSnapshot rechecked;
    QString captureError;
    if (!captureConfigurationSnapshot(
            tool, safety.backupId, safety.createdAt, &rechecked, &captureError)
            || !snapshotsHaveSameFiles(safety, rechecked)) {
        cleanseSnapshot(&target);
        cleanseSnapshot(&safety);
        cleanseSnapshot(&rechecked);
        m_lastError = QStringLiteral("当前配置在恢复前发生变化，未执行恢复：%1")
            .arg(captureError.isEmpty()
                ? QStringLiteral("configuration-backup-source-drifted") : captureError);
        return false;
    }
    cleanseSnapshot(&rechecked);

    if (restoreBackupInternal(target, tool)) {
        cleanseSnapshot(&target);
        cleanseSnapshot(&safety);
        pruneBackups(tool);
        return true;
    }

    const QString restoreError = m_lastError;
    const bool recovered = restoreBackupInternal(safety, tool);
    const QString recoveryError = m_lastError;
    cleanseSnapshot(&target);
    cleanseSnapshot(&safety);
    if (recovered) {
        pruneBackups(tool);
        m_lastError = QStringLiteral("%1（当前配置已从安全快照恢复）")
            .arg(restoreError);
    } else {
        m_lastError = QStringLiteral("%1；恢复当前配置也失败，当前状态不确定：%2")
            .arg(restoreError, recoveryError);
    }
    return false;
}

bool ToolManager::writeTextFile(const QString &path, const QByteArray &data)
{
    if (m_captureConfigurationWrites) {
        const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (m_capturedConfigurationWrites.contains(normalized)) {
            m_lastError = QStringLiteral("配置候选重复写入同一目标");
            return false;
        }
        m_capturedConfigurationWrites.insert(normalized, data);
        return true;
    }
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

void ToolManager::clearCapturedConfigurationWrites()
{
    for (auto it = m_capturedConfigurationWrites.begin();
         it != m_capturedConfigurationWrites.end(); ++it) {
        if (!it.value().isEmpty()) {
            OPENSSL_cleanse(it.value().data(), static_cast<size_t>(it.value().size()));
        }
    }
    m_capturedConfigurationWrites.clear();
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

LocalConfigurationStatus ToolManager::inspectConfiguration(AiTool tool) const
{
    const auto failure = [](LocalConfigurationState state, const QString &detail) {
        LocalConfigurationStatus status;
        status.state = state;
        status.detail = detail;
        return status;
    };
    const auto ready = [](const QString &key, bool gatewayMode) {
        LocalConfigurationStatus status;
        status.state = LocalConfigurationState::Ready;
        status.gatewayMode = gatewayMode;
        status.keyHint = maskedCredentialHint(key);
        return status;
    };
    const auto readJsonObject = [&](const QString &path,
                                    const QString &displayPath,
                                    QJsonObject *object) {
        if (!QFileInfo::exists(path)) {
            return failure(LocalConfigurationState::Missing,
                           QStringLiteral("%1 已被删除或尚未创建").arg(displayPath));
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("无法读取 %1").arg(displayPath));
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("%1 不是有效的 JSON：%2")
                               .arg(displayPath, error.errorString()));
        }
        *object = document.object();
        return LocalConfigurationStatus { LocalConfigurationState::Ready };
    };

    switch (tool) {
    case AiTool::ClaudeCode: {
        QJsonObject root;
        const LocalConfigurationStatus parsed = readJsonObject(
            homeFilePath(QStringLiteral(".claude/settings.json")),
            QStringLiteral("~/.claude/settings.json"), &root);
        if (!parsed.isReady()) return parsed;
        const QJsonObject env = root.value(QStringLiteral("env")).toObject();
        const QString key = env.value(QStringLiteral("ANTHROPIC_AUTH_TOKEN")).toString();
        const QString baseUrl = env.value(QStringLiteral("ANTHROPIC_BASE_URL")).toString();
        bool gatewayMode = false;
        if (key.isEmpty()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("settings.json 缺少 ANTHROPIC_AUTH_TOKEN"));
        }
        if (!isAegisyBaseUrl(
                baseUrl, QStringLiteral("/tools/claude"), &gatewayMode)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("settings.json 中的 ANTHROPIC_BASE_URL 无效"));
        }
        return ready(key, gatewayMode);
    }
    case AiTool::CodexCli: {
        QJsonObject auth;
        LocalConfigurationStatus parsed = readJsonObject(
            homeFilePath(QStringLiteral(".codex/auth.json")),
            QStringLiteral("~/.codex/auth.json"), &auth);
        if (!parsed.isReady()) return parsed;
        const QString key = auth.value(QStringLiteral("OPENAI_API_KEY")).toString();
        if (key.isEmpty()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("auth.json 缺少 OPENAI_API_KEY"));
        }

        const QString configPath = homeFilePath(QStringLiteral(".codex/config.toml"));
        if (!QFileInfo::exists(configPath)) {
            return failure(LocalConfigurationState::Missing,
                           QStringLiteral("~/.codex/config.toml 已被删除或尚未创建"));
        }
        QFile config(configPath);
        if (!config.open(QIODevice::ReadOnly)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("无法读取 ~/.codex/config.toml"));
        }

        static const QStringList managedRootKeys = {
            QStringLiteral("model_provider"), QStringLiteral("model"),
            QStringLiteral("review_model"), QStringLiteral("model_reasoning_effort"),
            QStringLiteral("model_context_window"),
            QStringLiteral("model_auto_compact_token_limit"),
            QStringLiteral("disable_response_storage"), QStringLiteral("network_access"),
            QStringLiteral("windows_wsl_setup_acknowledged"), QStringLiteral("web_search"),
        };
        static const QStringList managedStringKeys = {
            QStringLiteral("model_provider"), QStringLiteral("model"),
            QStringLiteral("review_model"), QStringLiteral("model_reasoning_effort"),
            QStringLiteral("network_access"), QStringLiteral("web_search"),
        };
        static const QStringList managedBooleanKeys = {
            QStringLiteral("disable_response_storage"),
            QStringLiteral("windows_wsl_setup_acknowledged"),
        };
        static const QStringList managedIntegerKeys = {
            QStringLiteral("model_context_window"),
            QStringLiteral("model_auto_compact_token_limit"),
        };
        QHash<QString, QString> rootValues;
        QHash<QString, QHash<QString, QString>> tableValues;
        QString currentTable;
        bool misplacedRootKey = false;
        bool duplicateRootKey = false;
        bool invalidManagedValue = false;
        const QString content = QString::fromUtf8(config.readAll());
        for (const QString &raw : content.split(QLatin1Char('\n'))) {
            const QString trimmed = raw.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
            if (trimmed.startsWith(QLatin1Char('['))) {
                const int closing = trimmed.indexOf(QLatin1Char(']'));
                currentTable = closing > 1
                    ? trimmed.mid(1, closing - 1).trimmed() : QStringLiteral("__invalid__");
                continue;
            }
            const int separator = trimmed.indexOf(QLatin1Char('='));
            if (separator <= 0) continue;
            const QString name = trimmed.left(separator).trimmed();
            const QString rawValue = trimmed.mid(separator + 1).trimmed();
            bool scalarOk = false;
            const QString value = configScalar(rawValue, &scalarOk);
            if (managedRootKeys.contains(name)) {
                if (!currentTable.isEmpty()) {
                    misplacedRootKey = true;
                } else if (rootValues.contains(name)) {
                    duplicateRootKey = true;
                } else {
                    rootValues.insert(name, value);
                }
                if (managedStringKeys.contains(name)
                        && !(rawValue.startsWith(QLatin1Char('"'))
                             || rawValue.startsWith(QLatin1Char('\'')))) {
                    scalarOk = false;
                }
                if (managedBooleanKeys.contains(name)
                        && value != QStringLiteral("true")
                        && value != QStringLiteral("false")) {
                    scalarOk = false;
                }
                if (managedIntegerKeys.contains(name)) {
                    bool integerOk = false;
                    value.toLongLong(&integerOk);
                    if (!integerOk
                            || rawValue.startsWith(QLatin1Char('"'))
                            || rawValue.startsWith(QLatin1Char('\''))) {
                        scalarOk = false;
                    }
                }
                if (!scalarOk) invalidManagedValue = true;
            } else if (name == QStringLiteral("base_url")
                       || name == QStringLiteral("wire_api")
                       || name == QStringLiteral("requires_openai_auth")
                       || name == QStringLiteral("experimental_bearer_token")
                       || name == QStringLiteral("http_headers")
                       || name == QStringLiteral("request_max_retries")
                       || name == QStringLiteral("stream_max_retries")
                       || name == QStringLiteral("stream_idle_timeout_ms")
                       || name == QStringLiteral("supports_websockets")) {
                tableValues[currentTable].insert(name, value);
                if ((name == QStringLiteral("base_url")
                     || name == QStringLiteral("wire_api")
                     || name == QStringLiteral("experimental_bearer_token"))
                        && !(rawValue.startsWith(QLatin1Char('"'))
                             || rawValue.startsWith(QLatin1Char('\'')))) {
                    scalarOk = false;
                }
                if (name == QStringLiteral("requires_openai_auth")
                        || name == QStringLiteral("supports_websockets")) {
                    if (value != QStringLiteral("true")
                            && value != QStringLiteral("false")) {
                        scalarOk = false;
                    }
                }
                if (name == QStringLiteral("request_max_retries")
                        || name == QStringLiteral("stream_max_retries")
                        || name == QStringLiteral("stream_idle_timeout_ms")) {
                    bool integerOk = false;
                    value.toLongLong(&integerOk);
                    if (!integerOk || rawValue.startsWith(QLatin1Char('"'))
                            || rawValue.startsWith(QLatin1Char('\''))) {
                        scalarOk = false;
                    }
                }
                if (name == QStringLiteral("http_headers")
                        && (!rawValue.startsWith(QLatin1Char('{'))
                            || !rawValue.endsWith(QLatin1Char('}')))) {
                    scalarOk = false;
                }
                if (!scalarOk) invalidManagedValue = true;
            }
        }

        if (misplacedRootKey) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 的顶层配置误写到了其他 TOML 表中"));
        }
        if (duplicateRootKey || invalidManagedValue) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 包含重复或无效的托管字段"));
        }
        const QString provider = rootValues.value(QStringLiteral("model_provider"));
        const QString model = rootValues.value(QStringLiteral("model"));
        if (provider.isEmpty() || model.isEmpty()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 缺少 model_provider 或 model"));
        }
        const qint64 configuredLimit = configuredContextLimit(AiTool::CodexCli, model);
        const QString expectedContextWindow = QString::number(configuredLimit);
        if (rootValues.value(QStringLiteral("model_context_window"))
                    != expectedContextWindow
                || rootValues.value(QStringLiteral("model_auto_compact_token_limit"))
                    != expectedContextWindow) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 的上下文窗口或自动压缩阈值不是 %1")
                               .arg(configuredLimit));
        }
        if (rootValues.value(QStringLiteral("review_model")) != model
                || rootValues.value(QStringLiteral("model_reasoning_effort"))
                    != configuredReasoning(AiTool::CodexCli, model)
                || rootValues.value(QStringLiteral("web_search"))
                    != QStringLiteral("live")) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 的模型、思考深度或 Web Search 配置无效"));
        }
        const QString providerTable = QStringLiteral("model_providers.%1").arg(provider);
        const QHash<QString, QString> providerValues = tableValues.value(providerTable);
        const QString baseUrl = providerValues.value(QStringLiteral("base_url"));
        bool gatewayMode = false;
        if (!isAegisyBaseUrl(
                baseUrl, QStringLiteral("/tools/codex/v1"), &gatewayMode)
                || providerValues.value(QStringLiteral("wire_api"))
                    != QStringLiteral("responses")
                || providerValues.value(QStringLiteral("requires_openai_auth"))
                    != QStringLiteral("false")
                || providerValues.value(QStringLiteral("experimental_bearer_token")) != key
                || !hasCodexCapabilityHeader(
                    providerValues.value(QStringLiteral("http_headers")))
                || !hasCodexIdentityEncodingHeader(
                    providerValues.value(QStringLiteral("http_headers")))
                || providerValues.value(QStringLiteral("request_max_retries"))
                    != QStringLiteral("4")
                || providerValues.value(QStringLiteral("stream_max_retries"))
                    != QStringLiteral("5")
                || providerValues.value(QStringLiteral("stream_idle_timeout_ms"))
                    != QStringLiteral("600000")
                || providerValues.value(QStringLiteral("supports_websockets"))
                    != QStringLiteral("false")) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.toml 的第三方 Provider 兼容配置无效"));
        }
        return ready(key, gatewayMode);
    }
    case AiTool::GeminiCli: {
        const QString path = homeFilePath(QStringLiteral(".gemini/.env"));
        if (!QFileInfo::exists(path)) {
            return failure(LocalConfigurationState::Missing,
                           QStringLiteral("~/.gemini/.env 已被删除或尚未创建"));
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("无法读取 ~/.gemini/.env"));
        }
        QHash<QString, QString> values;
        const QStringList managed = {
            QStringLiteral("GOOGLE_GEMINI_BASE_URL"),
            QStringLiteral("GEMINI_API_KEY"), QStringLiteral("GEMINI_MODEL"),
        };
        for (const QString &raw : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
            const QString trimmed = raw.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
            const int separator = trimmed.indexOf(QLatin1Char('='));
            const QString name = separator > 0
                ? trimmed.left(separator).trimmed() : trimmed;
            if (!managed.contains(name)) continue;
            if (separator <= 0) {
                return failure(LocalConfigurationState::Invalid,
                               QStringLiteral(".env 中的 %1 格式无效").arg(name));
            }
            bool scalarOk = false;
            const QString value = configScalar(trimmed.mid(separator + 1), &scalarOk);
            if (!scalarOk) {
                return failure(LocalConfigurationState::Invalid,
                               QStringLiteral(".env 中的 %1 值无效").arg(name));
            }
            values.insert(name, value);
        }
        const QString key = values.value(QStringLiteral("GEMINI_API_KEY"));
        const QString model = values.value(QStringLiteral("GEMINI_MODEL"));
        bool gatewayMode = false;
        if (key.isEmpty() || model.isEmpty()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral(".env 缺少 Gemini API Key 或模型"));
        }
        if (!isAegisyBaseUrl(
                values.value(QStringLiteral("GOOGLE_GEMINI_BASE_URL")),
                QStringLiteral("/tools/gemini"), &gatewayMode)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral(".env 中的 GOOGLE_GEMINI_BASE_URL 无效"));
        }
        return ready(key, gatewayMode);
    }
    case AiTool::OpenCode: {
        QJsonObject root;
        const LocalConfigurationStatus parsed = readJsonObject(
            homeFilePath(QStringLiteral(".config/opencode/config.json")),
            QStringLiteral("~/.config/opencode/config.json"), &root);
        if (!parsed.isReady()) return parsed;
        const QJsonObject provider = root.value(QStringLiteral("provider")).toObject()
            .value(QStringLiteral("anthropic")).toObject();
        const QString key = provider.value(QStringLiteral("api_key")).toString();
        const QString baseUrl = provider.value(QStringLiteral("base_url")).toString();
        const QString model = root.value(QStringLiteral("model")).toString();
        bool gatewayMode = false;
        if (key.isEmpty() || model.isEmpty()) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.json 缺少 API Key 或模型"));
        }
        if (!isAegisyBaseUrl(
                baseUrl, QStringLiteral("/tools/opencode"), &gatewayMode)) {
            return failure(LocalConfigurationState::Invalid,
                           QStringLiteral("config.json 中的 provider.anthropic.base_url 无效"));
        }
        return ready(key, gatewayMode);
    }
    }
    return failure(LocalConfigurationState::Invalid, QStringLiteral("不支持的工具配置"));
}

// ── 核心检测（可指定超时）────────────────────────────────────────
ToolStatus ToolManager::detectWithTimeout(AiTool tool, int timeoutMs)
{
    ToolStatus status;
    status.nodeOk = commandExists(QStringLiteral("node"), timeoutMs);
    const QString executable = resolveCommand(cliCommand(tool), timeoutMs);
    if (!executable.isEmpty()) {
        status.version = commandVersion(executable, timeoutMs);
        status.installed = !status.version.isEmpty();
        if (!status.installed) {
            status.repairRequired = true;
            status.installationIssue = QStringLiteral(
                "检测到 %1 命令入口，但该命令无法运行；上次安装或升级可能被中断。")
                .arg(cliCommand(tool));
        }
    }

    // npm 元数据不能证明 CLI 可运行。升级被 Ctrl+C 中断后，包目录可能
    // 仍在而 codex.cmd 等入口已经丢失，此时必须显示“修复”而非“已安装”。
    if (!status.installed && status.nodeOk) {
        const QString packageVersion = npmPackageVersion(tool, timeoutMs);
        if (!packageVersion.isEmpty()) {
            status.version = packageVersion;
            status.repairRequired = true;
            status.installationIssue = QStringLiteral(
                "npm 中仍有 %1 %2，但未找到可运行的 %3 命令；安装可能已损坏。")
                .arg(npmPackage(tool), packageVersion, cliCommand(tool));
        }
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

    const LocalConfigurationStatus configuration = inspectConfiguration(tool);
    status.configured = configuration.isReady();
    status.configuredKey = configuration.keyHint;
    if (!configuration.isReady()) {
        status.configurationIssue = configuration.detail;
    }

    return status;
}

ToolStatus ToolManager::detect(AiTool tool)
{
    return detectWithTimeout(tool, 5000);
}

ToolStatus ToolManager::detectFast(AiTool tool, int timeoutMs)
{
    return detectWithTimeout(tool, timeoutMs);
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
    const auto inspectPackageResidue = [this, process, tool]() {
        if (process->property("aegisyVersionComplete").toBool()) {
            return;
        }
        process->setProperty("aegisyVersionComplete", true);
        process->deleteLater();
        detectNpmVersion(tool);
    };

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [process, complete, inspectPackageResidue](
                      int exitCode, QProcess::ExitStatus exitStatus) {
        const QString version = extractVersion(
            ProcessCommand::decodeOutput(process->readAll()));
        if (exitStatus == QProcess::NormalExit && exitCode == 0
                && !version.isEmpty()) {
            complete(true, version);
        } else {
            inspectPackageResidue();
        }
    });
    connect(process, &QProcess::errorOccurred, this,
            [inspectPackageResidue](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            inspectPackageResidue();
        }
    });
    // A cold npm on a loaded or antivirus-scanning host can take well over
    // four seconds to answer `npm list -g`.
    QTimer::singleShot(30000, process, [process]() {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
    ProcessCommand::start(process, executable, { QStringLiteral("--version") });
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
        const QString output = ProcessCommand::decodeOutput(process->readAll()).trimmed();
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
    ProcessCommand::start(process, npmExecutable,
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
    ProcessCommand::start(&process, npmExecutable,
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
    return extractVersion(ProcessCommand::decodeOutput(process.readAll()).trimmed());
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
        // 包元数据存在但命令入口缺失属于损坏安装，不能报告“已安装”。
        emit toolVersionDetected(tool, false, version);
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
    ProcessCommand::start(process, npmExecutable,
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
        const QString text = ProcessCommand::decodeOutput(
            process->readAllStandardOutput()).trimmed();
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
    const QString npmExecutable = resolveCommand(kNpmCmd, 1000);
    if (npmExecutable.isEmpty()) {
        emit installOutput(
            tool,
            QStringLiteral("Node.js 已安装，但当前进程仍未找到 npm，请重启应用后重试。"));
        emit installFinished(tool, requestId, false);
        return;
    }

#ifdef Q_OS_WIN
    if (tool == AiTool::CodexCli && isCliRunning(tool)) {
        emit installOutput(tool, QStringLiteral(
            "检测到 %1 正在运行。Windows 会锁定其程序文件，当前无法安装或更新；"
            "请关闭所有 %1 窗口和终端后重试。")
            .arg(toolName(tool)));
        emit installFinished(tool, requestId, false);
        return;
    }
#endif

    const QString executable = resolveCommand(cliCommand(tool), 1000);
    const bool runnable = !executable.isEmpty()
        && !commandVersion(executable, 1500).isEmpty();
    const bool hasPackageResidue = !npmPackageVersion(tool, 1500).isEmpty();
    if (runnable || !hasPackageResidue) {
        installCliPackage(tool, requestId, npmExecutable);
        return;
    }

    emit installOutput(tool, QStringLiteral(
        "检测到上次安装或升级留下的残缺包，正在清理后重新安装..."));
    auto *cleanup = new QProcess(this);
    cleanup->setProcessEnvironment(commandEnvironment());
    cleanup->setProcessChannelMode(QProcess::MergedChannels);

    const auto continueInstall = [this, cleanup, tool, requestId, npmExecutable]() {
        if (cleanup->property("aegisyCleanupComplete").toBool()) return;
        cleanup->setProperty("aegisyCleanupComplete", true);
        cleanup->deleteLater();
        installCliPackage(tool, requestId, npmExecutable, true);
    };
    connect(cleanup, &QProcess::readyReadStandardOutput, this, [this, cleanup, tool]() {
        const QString text = ProcessCommand::decodeOutput(
            cleanup->readAllStandardOutput());
        cleanup->setProperty("aegisyInstallOutput",
            cleanup->property("aegisyInstallOutput").toString() + text);
        const QString trimmed = text.trimmed();
        if (!trimmed.isEmpty()) emit installOutput(tool, trimmed);
    });
    connect(cleanup, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, cleanup, tool, requestId, continueInstall](
                      int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            continueInstall();
            return;
        }
        if (isNpmBusyError(cleanup->property("aegisyInstallOutput").toString())) {
            cleanup->setProperty("aegisyCleanupComplete", true);
            cleanup->deleteLater();
            emit installOutput(tool, QStringLiteral(
                "%1 程序文件仍被占用，已停止覆盖安装。请关闭所有相关窗口和终端后重试。")
                .arg(toolName(tool)));
            emit installFinished(tool, requestId, false);
            return;
        }
        continueInstall();
    });
    connect(cleanup, &QProcess::errorOccurred, this,
            [this, cleanup, tool, continueInstall](QProcess::ProcessError) {
        emit installOutput(tool, QStringLiteral("清理残缺包时出现问题：%1；将继续尝试覆盖安装。")
            .arg(cleanup->errorString()));
        continueInstall();
    });
    emit installOutput(tool, QStringLiteral("$ %1 uninstall -g %2")
        .arg(kNpmCmd, npmPackage(tool)));
    ProcessCommand::start(cleanup, npmExecutable,
                 { QStringLiteral("uninstall"), QStringLiteral("-g"), npmPackage(tool) });
}

void ToolManager::installCliPackage(AiTool tool, int requestId,
                                    const QString &npmExecutable,
                                    bool forceRepair)
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
        if (!success && isNpmBusyError(
                process->property("aegisyInstallOutput").toString())) {
            emit installOutput(tool, QStringLiteral(
                "程序文件正被运行中的 %1 占用。请关闭所有相关窗口和终端后重试。")
                .arg(toolName(tool)));
        }
        bool verified = success;
        if (verified) {
            const QString executable = resolveCommand(cliCommand(tool), 2000);
            const QString installedVersion = commandVersion(executable, 3000);
            verified = !executable.isEmpty() && !installedVersion.isEmpty();
            if (!verified) {
                emit installOutput(tool, QStringLiteral(
                    "npm 已结束，但 %1 命令仍不可运行。请重启应用后再点“修复”。")
                    .arg(cliCommand(tool)));
            } else {
                const QString packageVersion = npmPackageVersion(tool, 3000);
                const QVersionNumber installed = QVersionNumber::fromString(
                    installedVersion);
                const QVersionNumber package = QVersionNumber::fromString(
                    packageVersion);
                if (!installed.isNull() && !package.isNull()
                        && QVersionNumber::compare(installed, package) < 0) {
                    verified = false;
                    emit installOutput(tool, QStringLiteral(
                        "npm 已更新到 %1，但当前命令 %2 仍指向 %3（版本 %4）。"
                        "检测到多份安装，请关闭终端并在系统体检中重新检测。")
                        .arg(packageVersion, cliCommand(tool), executable,
                             installedVersion));
                }
            }
        }
        emit installFinished(tool, requestId, verified);
    };

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, tool]() {
        const QString text = ProcessCommand::decodeOutput(
            process->readAllStandardOutput());
        process->setProperty("aegisyInstallOutput",
            process->property("aegisyInstallOutput").toString() + text);
        const QString trimmed = text.trimmed();
        if (!trimmed.isEmpty()) {
            emit installOutput(tool, trimmed);
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

    const QString packageSpec = npmPackage(tool) + QStringLiteral("@latest");
    emit installOutput(tool, QStringLiteral("$ %1 install -g %2")
        .arg(kNpmCmd, packageSpec));
    QStringList arguments = { QStringLiteral("install"), QStringLiteral("-g"),
                              packageSpec };
    if (forceRepair) arguments.append(QStringLiteral("--force"));
    ProcessCommand::start(process, npmExecutable, arguments);
}

// ── 配置写入 ─────────────────────────────────────────────────────
bool ToolManager::captureConfigurationCandidate(
        AiTool tool, bool gatewayMode, const QString &credential,
        const QString &model, int port,
        const ConfigurationBackupSnapshot &preimage,
        ConfigurationBackupSnapshot *candidate)
{
    if (!candidate || m_captureConfigurationWrites || credential.trimmed().isEmpty()
            || (gatewayMode && (port < 1 || port > 65535))) {
        m_lastError = QStringLiteral("配置候选参数无效");
        return false;
    }
    clearCapturedConfigurationWrites();
    m_captureConfigurationWrites = true;
    bool generated = false;
    if (gatewayMode) {
        const QString root = QStringLiteral("http://127.0.0.1:%1/tools/").arg(port);
        switch (tool) {
        case AiTool::ClaudeCode:
            generated = configureClaudeCodeEndpoint(
                credential, root + QStringLiteral("claude"));
            break;
        case AiTool::CodexCli:
            generated = configureCodexCliEndpoint(
                credential, model, root + QStringLiteral("codex/v1"),
                QStringLiteral("aegisy_local"));
            break;
        case AiTool::GeminiCli:
            generated = configureGeminiCliEndpoint(
                credential, model, root + QStringLiteral("gemini"));
            break;
        case AiTool::OpenCode:
            generated = configureOpenCodeEndpoint(
                credential, model, root + QStringLiteral("opencode"));
            break;
        }
    } else {
        switch (tool) {
        case AiTool::ClaudeCode: generated = configureClaudeCode(credential, model); break;
        case AiTool::CodexCli: generated = configureCodexCli(credential, model); break;
        case AiTool::GeminiCli: generated = configureGeminiCli(credential, model); break;
        case AiTool::OpenCode: generated = configureOpenCode(credential, model); break;
        }
    }
    m_captureConfigurationWrites = false;
    if (!generated) {
        clearCapturedConfigurationWrites();
        return false;
    }

    const QStringList paths = managedConfigPaths(tool);
    if (m_capturedConfigurationWrites.size() != paths.size()
            || preimage.files.size() != paths.size()) {
        clearCapturedConfigurationWrites();
        m_lastError = QStringLiteral("配置候选目标集合不完整");
        return false;
    }
    *candidate = preimage;
    for (int slot = 0; slot < paths.size(); ++slot) {
        const QString normalized = QDir::cleanPath(
            QFileInfo(paths.at(slot)).absoluteFilePath());
        const auto found = m_capturedConfigurationWrites.constFind(normalized);
        if (found == m_capturedConfigurationWrites.cend()
                || candidate->files.at(slot).slot != slot) {
            cleanseSnapshot(candidate);
            clearCapturedConfigurationWrites();
            m_lastError = QStringLiteral("配置候选目标身份不一致");
            return false;
        }
        candidate->files[slot].existed = true;
        if (!candidate->files[slot].content.isEmpty()) {
            OPENSSL_cleanse(candidate->files[slot].content.data(),
                            static_cast<size_t>(candidate->files[slot].content.size()));
        }
        candidate->files[slot].content = found.value();
    }
    clearCapturedConfigurationWrites();
    return true;
}

bool ToolManager::prepareConfigurationApply(
        AiTool tool, bool gatewayMode, const QString &credential,
        const QString &model, ConfigurationApplyReceipt *receipt, int port)
{
    m_lastError.clear();
    m_lastWarning.clear();
    m_lastConfigurationOutcomeUnknown = false;
    if (receipt) *receipt = ConfigurationApplyReceipt();
    if (!receipt || credential.trimmed().isEmpty()
            || (gatewayMode && (port < 1 || port > 65535))) {
        m_lastError = QStringLiteral("配置事务 receipt、凭据或端口无效");
        return false;
    }
    ConfigurationBackupSnapshot preimage;
    const QString backupId = createBackup(tool, &preimage);
    if (backupId.isEmpty()) return false;

    ConfigurationBackupSnapshot rechecked;
    QString captureError;
    const bool sourceStable = captureConfigurationSnapshot(
        tool, preimage.backupId, preimage.createdAt, &rechecked, &captureError)
        && snapshotsHaveSameFiles(preimage, rechecked);
    cleanseSnapshot(&rechecked);
    if (!sourceStable) {
        cleanseSnapshot(&preimage);
        m_lastError = QStringLiteral("配置在安全备份后发生变化，未准备写入：%1")
            .arg(captureError.isEmpty()
                ? QStringLiteral("configuration-backup-source-drifted") : captureError);
        return false;
    }
    const QString sourceIdentity = snapshotFilesIdentity(preimage);
    ConfigurationBackupSnapshot candidate;
    if (!captureConfigurationCandidate(
            tool, gatewayMode, credential, model, port, preimage, &candidate)) {
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&candidate);
        return false;
    }
    const QString candidateIdentity = snapshotFilesIdentity(candidate);
    cleanseSnapshot(&candidate);
    cleanseSnapshot(&preimage);
    const ConfigBackupInventory inventory = backupInventory(tool);
    const auto found = std::find_if(
        inventory.backups.cbegin(), inventory.backups.cend(),
        [&backupId](const ConfigBackup &backup) { return backup.id == backupId; });
    if (inventory.state != ConfigBackupSubsystemState::Ready
            || found == inventory.backups.cend()
            || found->manifestIdentity.isEmpty()) {
        m_lastError = QStringLiteral("配置事务备份身份无法验证");
        return false;
    }
    receipt->tool = tool;
    receipt->backupId = backupId;
    receipt->backupManifestIdentity = found->manifestIdentity;
    receipt->sourceFilesIdentity = sourceIdentity;
    receipt->candidateFilesIdentity = candidateIdentity;
    receipt->gatewayMode = gatewayMode;
    return true;
}

bool ToolManager::applyPreparedConfiguration(
        ConfigurationApplyReceipt *receipt, const QString &credential,
        const QString &model, int port)
{
    m_lastError.clear();
    m_lastWarning.clear();
    m_lastConfigurationOutcomeUnknown = false;
    if (!receipt || !receipt->isPrepared() || credential.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("配置事务 receipt 或凭据无效");
        return false;
    }
    const ConfigBackupInventory inventory = backupInventory(receipt->tool);
    const auto found = std::find_if(
        inventory.backups.cbegin(), inventory.backups.cend(),
        [receipt](const ConfigBackup &backup) {
            return backup.id == receipt->backupId
                && backup.manifestIdentity == receipt->backupManifestIdentity;
        });
    if (inventory.state != ConfigBackupSubsystemState::Ready
            || found == inventory.backups.cend()) {
        m_lastError = QStringLiteral("配置事务备份身份已漂移");
        return false;
    }
    ConfigurationBackupSnapshot preimage;
    if (!readBackup(receipt->backupId, receipt->tool, &preimage)
            || snapshotFilesIdentity(preimage) != receipt->sourceFilesIdentity) {
        cleanseSnapshot(&preimage);
        m_lastError = QStringLiteral("配置事务 preimage 无法认证");
        return false;
    }
    ConfigurationBackupSnapshot current;
    QString captureError;
    if (!captureConfigurationSnapshot(
            receipt->tool, preimage.backupId, preimage.createdAt,
            &current, &captureError)
            || snapshotFilesIdentity(current) != receipt->sourceFilesIdentity) {
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&current);
        m_lastError = QStringLiteral("配置源在 apply 前发生变化：%1")
            .arg(captureError.isEmpty()
                ? QStringLiteral("configuration-backup-source-drifted") : captureError);
        return false;
    }
    cleanseSnapshot(&current);

    ConfigurationBackupSnapshot planned;
    if (!captureConfigurationCandidate(
            receipt->tool, receipt->gatewayMode, credential, model, port,
            preimage, &planned)
            || snapshotFilesIdentity(planned) != receipt->candidateFilesIdentity) {
        cleanseSnapshot(&planned);
        cleanseSnapshot(&preimage);
        m_lastError = QStringLiteral("配置候选身份已漂移");
        return false;
    }
    cleanseSnapshot(&planned);

    const auto restorePreimage = [this, &preimage, receipt]() {
        if (!restoreBackupInternal(preimage, receipt->tool)) return false;
        ConfigurationBackupSnapshot restored;
        QString restoreCaptureError;
        const bool verified = captureConfigurationSnapshot(
            receipt->tool, preimage.backupId, preimage.createdAt,
            &restored, &restoreCaptureError)
            && snapshotFilesIdentity(restored) == receipt->sourceFilesIdentity;
        cleanseSnapshot(&restored);
        return verified;
    };

    bool success = false;
    if (receipt->gatewayMode) {
        const QString root = QStringLiteral("http://127.0.0.1:%1/tools/").arg(port);
        switch (receipt->tool) {
        case AiTool::ClaudeCode:
            success = configureClaudeCodeEndpoint(
                credential, root + QStringLiteral("claude"));
            break;
        case AiTool::CodexCli:
            success = configureCodexCliEndpoint(
                credential, model, root + QStringLiteral("codex/v1"),
                QStringLiteral("aegisy_local"));
            break;
        case AiTool::GeminiCli:
            success = configureGeminiCliEndpoint(
                credential, model, root + QStringLiteral("gemini"));
            break;
        case AiTool::OpenCode:
            success = configureOpenCodeEndpoint(
                credential, model, root + QStringLiteral("opencode"));
            break;
        }
    } else {
        switch (receipt->tool) {
        case AiTool::ClaudeCode: success = configureClaudeCode(credential, model); break;
        case AiTool::CodexCli: success = configureCodexCli(credential, model); break;
        case AiTool::GeminiCli: success = configureGeminiCli(credential, model); break;
        case AiTool::OpenCode: success = configureOpenCode(credential, model); break;
        }
    }
    if (success) success = readConfiguredKey(receipt->tool) == credential;
    if (!success) {
        const QString writeError = m_lastError.isEmpty()
            ? QStringLiteral("配置写入后校验失败") : m_lastError;
        const bool restored = restorePreimage();
        const QString restoreError = m_lastError;
        cleanseSnapshot(&preimage);
        m_lastError = restored
            ? QStringLiteral("%1（已自动回滚）").arg(writeError)
            : QStringLiteral("%1；自动回滚失败，当前状态不确定：%2")
                .arg(writeError, restoreError);
        m_lastConfigurationOutcomeUnknown = !restored;
        return false;
    }

    ConfigurationBackupSnapshot applied;
    if (!captureConfigurationSnapshot(
            receipt->tool, preimage.backupId, preimage.createdAt,
            &applied, &captureError)) {
        const bool restored = restorePreimage();
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&applied);
        m_lastError = restored
            ? QStringLiteral("写入结果无法绑定，已自动回滚")
            : QStringLiteral("写入结果无法绑定且回滚失败，当前状态不确定");
        m_lastConfigurationOutcomeUnknown = !restored;
        return false;
    }
    const QString appliedIdentity = snapshotFilesIdentity(applied);
    if (appliedIdentity != receipt->candidateFilesIdentity) {
        const bool restored = restorePreimage();
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&applied);
        m_lastError = restored
            ? QStringLiteral("配置写入与候选身份不一致，已自动回滚")
            : QStringLiteral("配置写入与候选身份不一致且回滚失败，当前状态不确定");
        m_lastConfigurationOutcomeUnknown = !restored;
        return false;
    }
    receipt->appliedFilesIdentity = appliedIdentity;
    cleanseSnapshot(&applied);
    cleanseSnapshot(&preimage);
    return true;
}

bool ToolManager::rollbackPreparedConfiguration(
        const ConfigurationApplyReceipt &receipt)
{
    m_lastError.clear();
    m_lastWarning.clear();
    m_lastConfigurationOutcomeUnknown = false;
    if (receipt.backupId.isEmpty() || receipt.backupManifestIdentity.isEmpty()
            || receipt.sourceFilesIdentity.isEmpty()
            || receipt.candidateFilesIdentity.isEmpty()
            || (!receipt.appliedFilesIdentity.isEmpty()
                && receipt.appliedFilesIdentity != receipt.candidateFilesIdentity)) {
        m_lastError = QStringLiteral("配置回滚 receipt 无效");
        return false;
    }
    const ConfigBackupInventory inventory = backupInventory(receipt.tool);
    const auto found = std::find_if(
        inventory.backups.cbegin(), inventory.backups.cend(),
        [&receipt](const ConfigBackup &backup) {
            return backup.id == receipt.backupId
                && backup.manifestIdentity == receipt.backupManifestIdentity;
        });
    if (inventory.state != ConfigBackupSubsystemState::Ready
            || found == inventory.backups.cend()) {
        m_lastError = QStringLiteral("配置回滚备份身份已漂移");
        return false;
    }
    ConfigurationBackupSnapshot preimage;
    if (!readBackup(receipt.backupId, receipt.tool, &preimage)
            || snapshotFilesIdentity(preimage) != receipt.sourceFilesIdentity) {
        cleanseSnapshot(&preimage);
        m_lastError = QStringLiteral("配置回滚 preimage 无法认证");
        return false;
    }
    ConfigurationBackupSnapshot current;
    QString error;
    if (!captureConfigurationSnapshot(
            receipt.tool, preimage.backupId, preimage.createdAt, &current, &error)) {
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&current);
        m_lastError = QStringLiteral("配置回滚前状态已漂移");
        return false;
    }
    const QString currentIdentity = snapshotFilesIdentity(current);
    if (currentIdentity != receipt.candidateFilesIdentity
            && currentIdentity != receipt.sourceFilesIdentity) {
        cleanseSnapshot(&preimage);
        cleanseSnapshot(&current);
        m_lastError = QStringLiteral("配置回滚前状态已漂移");
        return false;
    }
    const bool alreadyRestored = currentIdentity == receipt.sourceFilesIdentity;
    cleanseSnapshot(&current);
    if (alreadyRestored) {
        cleanseSnapshot(&preimage);
        return true;
    }
    if (!restoreBackupInternal(preimage, receipt.tool)) {
        m_lastConfigurationOutcomeUnknown = true;
        cleanseSnapshot(&preimage);
        return false;
    }
    ConfigurationBackupSnapshot restored;
    const bool verified = captureConfigurationSnapshot(
        receipt.tool, preimage.backupId, preimage.createdAt, &restored, &error)
        && snapshotFilesIdentity(restored) == receipt.sourceFilesIdentity;
    cleanseSnapshot(&preimage);
    cleanseSnapshot(&restored);
    if (!verified) {
        m_lastError = QStringLiteral("配置回滚结果无法验证");
        m_lastConfigurationOutcomeUnknown = true;
        return false;
    }
    return true;
}

bool ToolManager::finalizePreparedConfiguration(
        const ConfigurationApplyReceipt &receipt)
{
    m_lastError.clear();
    const auto validFilesIdentity = [](const QString &value) {
        static const QRegularExpression expression(QStringLiteral(
            "^configuration-files:sha256:[0-9a-f]{64}$"));
        return expression.match(value).hasMatch();
    };
    if (!ConfigurationBackupStore::isValidBackupId(receipt.backupId)
            || !validFilesIdentity(receipt.sourceFilesIdentity)
            || !validFilesIdentity(receipt.candidateFilesIdentity)
            || !validFilesIdentity(receipt.appliedFilesIdentity)
            || receipt.appliedFilesIdentity != receipt.candidateFilesIdentity) {
        m_lastError = QStringLiteral("配置 finalize receipt 无效");
        return false;
    }
    const ConfigBackupInventory inventory = backupInventory(receipt.tool);
    const bool exactBackup = inventory.state == ConfigBackupSubsystemState::Ready
        && std::any_of(
            inventory.backups.cbegin(), inventory.backups.cend(),
            [&receipt](const ConfigBackup &backup) {
                return backup.id == receipt.backupId
                    && backup.manifestIdentity == receipt.backupManifestIdentity;
            });
    if (!exactBackup) {
        m_lastError = QStringLiteral("配置 finalize 备份身份已漂移");
        return false;
    }
    pruneBackups(receipt.tool);
    return true;
}

bool ToolManager::configure(AiTool tool, const QString &apiKey,
                            const QString &model, QString *rollbackBackupId)
{
    if (rollbackBackupId) rollbackBackupId->clear();
    ConfigurationApplyReceipt receipt;
    if (!prepareConfigurationApply(tool, false, apiKey, model, &receipt)
            || !applyPreparedConfiguration(&receipt, apiKey, model)) return false;
    if (rollbackBackupId) *rollbackBackupId = receipt.backupId;
    finalizePreparedConfiguration(receipt);
    return true;
}

bool ToolManager::configureGateway(AiTool tool, const QString &localToken,
                                   const QString &model, int port,
                                   QString *rollbackBackupId)
{
    if (rollbackBackupId) rollbackBackupId->clear();
    ConfigurationApplyReceipt receipt;
    if (!prepareConfigurationApply(
            tool, true, localToken, model, &receipt, port)
            || !applyPreparedConfiguration(&receipt, localToken, model, port)) return false;
    if (rollbackBackupId) *rollbackBackupId = receipt.backupId;
    finalizePreparedConfiguration(receipt);
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
        apiKey, model, kBaseUrl, QStringLiteral("aegisy"));
}

bool ToolManager::configureCodexCliEndpoint(const QString &apiKey,
                                            const QString &model,
                                            const QString &baseUrl,
                                            const QString &providerId)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gpt-4o") : model;
    const qint64 contextLimit = configuredContextLimit(AiTool::CodexCli, effectiveModel);
    const QString reasoningEffort = configuredReasoning(AiTool::CodexCli, effectiveModel);
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
        "model_context_window", "model_auto_compact_token_limit",
        "disable_response_storage", "network_access", "windows_wsl_setup_acknowledged",
        "web_search",
    };
    static const QStringList managedProviderSections = {
        "[model_providers.OpenAI]", "[model_providers.aegisy]",
        "[model_providers.aegisy_local]",
    };

    QStringList outLines;
    QStringList featureLines;
    bool skippingSection = false;
    bool readingFeatures = false;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        if (t.startsWith('[')) {
            readingFeatures = t == QStringLiteral("[features]");
            skippingSection = managedProviderSections.contains(t);
            if (skippingSection || readingFeatures) continue;
        }
        if (skippingSection) continue;
        if (readingFeatures) {
            static const QStringList managedFeatureKeys = {
                QStringLiteral("goals"), QStringLiteral("web_search"),
                QStringLiteral("web_search_cached"),
                QStringLiteral("web_search_request"),
            };
            bool managedFeature = false;
            for (const QString &key : managedFeatureKeys) {
                if (t.startsWith(key)
                        && t.mid(key.length()).trimmed().startsWith('=')) {
                    managedFeature = true;
                    break;
                }
            }
            if (!managedFeature) {
                featureLines.append(raw);
            }
            continue;
        }
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
    while (!featureLines.isEmpty() && featureLines.first().trimmed().isEmpty()) {
        featureLines.removeFirst();
    }
    while (!featureLines.isEmpty() && featureLines.last().trimmed().isEmpty()) {
        featureLines.removeLast();
    }

    // TOML table headers remain active until the next table header. Codex commonly
    // leaves [projects.*] or [tui.model_availability_nux] at the end of this file,
    // so appending root keys would accidentally place them inside that table.
    QString result = QStringLiteral(
        "model_provider = %1\n"
        "model = %2\n"
        "review_model = %2\n")
        .arg(tomlBasicString(providerId), tomlBasicString(effectiveModel));
    result += QStringLiteral(
        "model_reasoning_effort = %1\n"
        "model_context_window = %2\n"
        "model_auto_compact_token_limit = %2\n"
        "web_search = \"live\"\n")
        .arg(tomlBasicString(reasoningEffort), QString::number(contextLimit));

    const QString preserved = outLines.join('\n');
    if (!preserved.isEmpty()) {
        result += QLatin1Char('\n') + preserved + QLatin1Char('\n');
    }

    result += QStringLiteral(
        "\n"
        "[model_providers.%1]\n"
        "name = %2\n"
        "base_url = %3\n"
        "wire_api = \"responses\"\n"
        "requires_openai_auth = false\n"
        "request_max_retries = 4\n"
        "stream_max_retries = 5\n"
        "stream_idle_timeout_ms = 600000\n"
        "supports_websockets = false\n"
        "experimental_bearer_token = %4\n"
        "http_headers = { %5 = %6, %7 = %8 }\n"
        "\n"
        "[features]\n")
        .arg(providerId,
             tomlBasicString(providerId),
             tomlBasicString(baseUrl),
             tomlBasicString(apiKey),
             tomlBasicString(kCodexCapabilityHeader),
             tomlBasicString(kCodexCapabilityHeaderValue),
             tomlBasicString(kCodexEncodingHeader),
             tomlBasicString(kCodexEncodingHeaderValue));
    if (!featureLines.isEmpty()) {
        result += featureLines.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }
    result += QStringLiteral("goals = true\n");

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
