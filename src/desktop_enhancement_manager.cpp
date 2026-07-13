#include "desktop_enhancement_manager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QWebSocket>

namespace {

QString commandPath(const QString &name)
{
    QString path = QStandardPaths::findExecutable(name);
#if defined(Q_OS_WIN)
    if (path.isEmpty() && name.compare(QStringLiteral("codex"), Qt::CaseInsensitive) == 0) {
        path = QStandardPaths::findExecutable(QStringLiteral("codex.cmd"));
    }
#endif
    return path;
}

QString rootTomlString(const QString &text, const QString &key)
{
    const QString root = text.section(QRegularExpression(QStringLiteral("^\\s*\\[")), 0, 0);
    const QRegularExpression expression(
        QStringLiteral("^\\s*%1\\s*=\\s*[\\\"']([^\\\"']+)[\\\"']\\s*(?:#.*)?$")
            .arg(QRegularExpression::escape(key)),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = expression.match(root);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

QString jsonString(const QString &value)
{
    const QByteArray encoded = QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QString backupNameFor(const QString &path)
{
    const QByteArray digest = QCryptographicHash::hash(
        path.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return QString::fromLatin1(digest) + QLatin1Char('-') + QFileInfo(path).fileName();
}

bool writeAtomically(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入 %1：%2").arg(path, file.errorString());
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("无法原子更新 %1：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}

} // namespace

DesktopEnhancementManager::DesktopEnhancementManager(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

QList<CodexPluginInfo> DesktopEnhancementManager::listCodexPlugins(QString *error) const
{
    QList<CodexPluginInfo> result;
    const QString codex = commandPath(QStringLiteral("codex"));
    if (codex.isEmpty()) {
        if (error) *error = QStringLiteral("未找到 Codex CLI，请先在系统体检中安装。");
        return result;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(codex, { QStringLiteral("plugin"), QStringLiteral("list"),
                           QStringLiteral("--available"), QStringLiteral("--json") });
    if (!process.waitForStarted(3000) || !process.waitForFinished(15000)) {
        process.kill();
        if (error) *error = QStringLiteral("Codex 插件列表查询超时。");
        return result;
    }
    const QByteArray output = process.readAll();
    if (process.exitCode() != 0) {
        if (error) *error = QString::fromUtf8(output).trimmed();
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Codex 返回的插件列表不是有效 JSON：%1")
            .arg(parseError.errorString());
        return result;
    }

    const auto append = [&result](const QJsonArray &items) {
        for (const QJsonValue &value : items) {
            const QJsonObject object = value.toObject();
            CodexPluginInfo plugin;
            plugin.id = object.value(QStringLiteral("pluginId")).toString();
            plugin.name = object.value(QStringLiteral("name")).toString();
            plugin.marketplace = object.value(QStringLiteral("marketplaceName")).toString();
            plugin.version = object.value(QStringLiteral("version")).toString();
            plugin.installed = object.value(QStringLiteral("installed")).toBool();
            plugin.enabled = object.value(QStringLiteral("enabled")).toBool();
            plugin.path = object.value(QStringLiteral("source")).toObject()
                .value(QStringLiteral("path")).toString();
            if (!plugin.id.isEmpty()) result.append(plugin);
        }
    };
    const QJsonObject root = document.object();
    append(root.value(QStringLiteral("installed")).toArray());
    append(root.value(QStringLiteral("available")).toArray());
    return result;
}

bool DesktopEnhancementManager::installCodexPlugin(const QString &pluginId,
                                                    QString *output,
                                                    QString *error) const
{
    const QString codex = commandPath(QStringLiteral("codex"));
    if (codex.isEmpty()) {
        if (error) *error = QStringLiteral("未找到 Codex CLI。");
        return false;
    }
    if (pluginId.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("没有选择插件。");
        return false;
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(codex, { QStringLiteral("plugin"), QStringLiteral("add"),
                           pluginId, QStringLiteral("--json") });
    if (!process.waitForStarted(3000) || !process.waitForFinished(120000)) {
        process.kill();
        if (error) *error = QStringLiteral("安装插件超时。");
        return false;
    }
    const QString text = QString::fromUtf8(process.readAll()).trimmed();
    if (output) *output = text;
    if (process.exitCode() != 0) {
        if (error) *error = text.isEmpty() ? QStringLiteral("Codex 插件安装失败。") : text;
        return false;
    }
    return true;
}

SessionSyncReport DesktopEnhancementManager::syncCodexHistory(QString *error) const
{
    return syncCodexHistoryAt(QDir::homePath() + QStringLiteral("/.codex"), error);
}

SessionSyncReport DesktopEnhancementManager::syncCodexHistoryAt(const QString &codexHome,
                                                                 QString *error)
{
    SessionSyncReport report;
    const QString configPath = codexHome + QStringLiteral("/config.toml");
    QFile config(configPath);
    if (config.open(QIODevice::ReadOnly)) {
        report.provider = rootTomlString(QString::fromUtf8(config.readAll()),
                                         QStringLiteral("model_provider"));
    }
    if (report.provider.isEmpty()) report.provider = QStringLiteral("openai");

    const QString backupRoot = codexHome + QStringLiteral("/backups_state/aegisy-provider-sync/")
        + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    if (!QDir().mkpath(backupRoot)) {
        if (error) *error = QStringLiteral("无法创建会话同步备份目录：%1").arg(backupRoot);
        return report;
    }
    report.backupPath = backupRoot;

    QJsonArray manifest;
    QHash<QString, QString> cwdByThread;
    QSet<QString> userEventThreads;
    QStringList changedPaths;
    QList<QPair<QString, QByteArray>> changedData;
    for (const QString &folder : { QStringLiteral("sessions"), QStringLiteral("archived_sessions") }) {
        const QString root = codexHome + QLatin1Char('/') + folder;
        QDirIterator iterator(root, { QStringLiteral("rollout-*.jsonl") }, QDir::Files,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray original = file.readAll();
            const QList<QByteArray> lines = original.split('\n');
            QByteArray updated;
            bool changed = false;
            QString threadId;
            for (int i = 0; i < lines.size(); ++i) {
                QByteArray line = lines[i];
                QJsonParseError parseError;
                QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
                if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                    QJsonObject object = document.object();
                    const QString type = object.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("user_message")
                            || type == QStringLiteral("user_input")) {
                        if (!threadId.isEmpty()) userEventThreads.insert(threadId);
                    }
                    if (type == QStringLiteral("session_meta")) {
                        QJsonObject payload = object.value(QStringLiteral("payload")).toObject();
                        threadId = payload.value(QStringLiteral("id")).toString();
                        const QString cwd = payload.value(QStringLiteral("cwd")).toString();
                        if (!threadId.isEmpty() && !cwd.isEmpty()) cwdByThread.insert(threadId, cwd);
                        if (payload.value(QStringLiteral("model_provider")).toString()
                                != report.provider) {
                            payload.insert(QStringLiteral("model_provider"), report.provider);
                            object.insert(QStringLiteral("payload"), payload);
                            line = QJsonDocument(object).toJson(QJsonDocument::Compact);
                            changed = true;
                        }
                    }
                }
                updated += line;
                if (i + 1 < lines.size()) updated += '\n';
            }
            if (!threadId.isEmpty() && (original.contains("\"user_message\"")
                                         || original.contains("\"user_input\""))) {
                userEventThreads.insert(threadId);
            }
            if (changed) {
                changedPaths.append(path);
                changedData.append({ path, updated });
            }
        }
    }

    for (const QString &path : changedPaths) {
        const QString backup = backupRoot + QLatin1Char('/') + backupNameFor(path);
        if (!QFile::copy(path, backup)) {
            if (error) *error = QStringLiteral("无法备份会话文件：%1").arg(path);
            return SessionSyncReport{};
        }
        manifest.append(QJsonObject{
            { QStringLiteral("path"), path },
            { QStringLiteral("backup"), backup }
        });
    }
    for (const auto &item : changedData) {
        if (!writeAtomically(item.first, item.second, error)) return SessionSyncReport{};
        ++report.sessionFilesChanged;
    }

    QStringList databasePaths;
    QDirIterator dbIterator(codexHome + QStringLiteral("/sqlite"),
                            { QStringLiteral("*.db"), QStringLiteral("*.sqlite"),
                              QStringLiteral("*.sqlite3") }, QDir::Files);
    while (dbIterator.hasNext()) databasePaths.append(dbIterator.next());
    const QString legacyDb = codexHome + QStringLiteral("/state_5.sqlite");
    if (QFileInfo::exists(legacyDb)) databasePaths.append(legacyDb);

    for (const QString &databasePath : databasePaths) {
        const QString connectionName = QStringLiteral("aegisy-history-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        bool opened = false;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                               connectionName);
            database.setDatabaseName(databasePath);
            if (database.open()) {
                QSqlQuery schema(database);
                QSet<QString> columns;
                if (schema.exec(QStringLiteral("PRAGMA table_info(threads)"))) {
                    while (schema.next()) columns.insert(schema.value(1).toString());
                }
                if (columns.contains(QStringLiteral("model_provider"))) {
                    const QString backup = backupRoot + QLatin1Char('/') + backupNameFor(databasePath);
                    database.close();
                    if (!QFile::copy(databasePath, backup)) {
                        ++report.databasesSkipped;
                    } else if (database.open() && database.transaction()) {
                        QSqlQuery update(database);
                        update.prepare(QStringLiteral(
                            "UPDATE threads SET model_provider = ? "
                            "WHERE COALESCE(model_provider, '') <> ?"));
                        update.addBindValue(report.provider);
                        update.addBindValue(report.provider);
                        if (update.exec()) report.databaseRowsChanged += update.numRowsAffected();

                        if (columns.contains(QStringLiteral("has_user_event"))) {
                            QSqlQuery userUpdate(database);
                            userUpdate.prepare(QStringLiteral(
                                "UPDATE threads SET has_user_event = 1 WHERE id = ? "
                                "AND COALESCE(has_user_event, 0) <> 1"));
                            for (const QString &threadId : userEventThreads) {
                                userUpdate.bindValue(0, threadId);
                                if (userUpdate.exec()) report.databaseRowsChanged += userUpdate.numRowsAffected();
                            }
                        }
                        if (columns.contains(QStringLiteral("cwd"))) {
                            QSqlQuery cwdUpdate(database);
                            cwdUpdate.prepare(QStringLiteral(
                                "UPDATE threads SET cwd = ? WHERE id = ? AND COALESCE(cwd, '') <> ?"));
                            for (auto it = cwdByThread.cbegin(); it != cwdByThread.cend(); ++it) {
                                cwdUpdate.bindValue(0, it.value());
                                cwdUpdate.bindValue(1, it.key());
                                cwdUpdate.bindValue(2, it.value());
                                if (cwdUpdate.exec()) report.databaseRowsChanged += cwdUpdate.numRowsAffected();
                            }
                        }
                        database.commit();
                        opened = true;
                        manifest.append(QJsonObject{
                            { QStringLiteral("path"), databasePath },
                            { QStringLiteral("backup"), backup }
                        });
                    }
                } else {
                    opened = true;
                }
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
        if (!opened) ++report.databasesSkipped;
    }

    QJsonObject metadata{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("managedBy"), QStringLiteral("Aegisy provider sync") },
        { QStringLiteral("provider"), report.provider },
        { QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
        { QStringLiteral("files"), manifest }
    };
    writeAtomically(backupRoot + QStringLiteral("/manifest.json"),
                    QJsonDocument(metadata).toJson(QJsonDocument::Indented), nullptr);
    return report;
}

bool DesktopEnhancementManager::localizationRunning() const
{
    return m_localizationRunning;
}

void DesktopEnhancementManager::localizeClaudeDesktop()
{
    if (m_localizationRunning) return;
    m_localizationRunning = true;
    m_pollAttempts = 0;
    emit localizationProgress(QStringLiteral("正在启动 Claude Desktop 调试会话..."));
    QString error;
    if (!launchClaudeWithInspector(&error)) {
        finishLocalization(false, error.isEmpty()
            ? QStringLiteral("无法启动 Claude Desktop 调试会话。") : error);
        return;
    }
#if defined(Q_OS_WIN)
    QTimer::singleShot(1800, this, &DesktopEnhancementManager::requestWindowsInspectorAttach);
#endif
    QTimer::singleShot(700, this, &DesktopEnhancementManager::pollInspector);
}

bool DesktopEnhancementManager::launchClaudeWithInspector(QString *error)
{
#if defined(Q_OS_MAC)
    QString appPath = QStringLiteral("/Applications/Claude.app");
    if (!QFileInfo::exists(appPath)) {
        appPath = QDir::homePath() + QStringLiteral("/Applications/Claude.app");
    }
    if (!QFileInfo::exists(appPath)) {
        if (error) *error = QStringLiteral("未检测到 Claude Desktop。");
        return false;
    }
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"),
        { QStringLiteral("-na"), appPath, QStringLiteral("--args"),
          QStringLiteral("--inspect=9229") });
#elif defined(Q_OS_WIN)
    QStringList candidates;
    const QString localAppData = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
    candidates << localAppData + QStringLiteral("\\Programs\\Claude\\Claude.exe")
               << localAppData + QStringLiteral("\\AnthropicClaude\\Claude.exe")
               << QStandardPaths::findExecutable(QStringLiteral("Claude.exe"));
    for (const QString &candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isFile()) {
            return QProcess::startDetached(candidate, { QStringLiteral("--inspect=9229") });
        }
    }
    const QString powershell = !QStandardPaths::findExecutable(QStringLiteral("pwsh.exe")).isEmpty()
        ? QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"))
        : QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (!powershell.isEmpty()) {
        const QString script = QStringLiteral(
            "$p=Get-AppxPackage | Where-Object {$_.Name -match 'Claude'} | Select-Object -First 1;"
            "if($p){Start-Process ('shell:AppsFolder\\'+$p.PackageFamilyName+'!App');exit 0};exit 2");
        return QProcess::startDetached(powershell,
            { QStringLiteral("-NoProfile"), QStringLiteral("-Command"), script });
    }
    if (error) *error = QStringLiteral("未检测到 Claude Desktop 或 PowerShell。");
    return false;
#else
    if (error) *error = QStringLiteral("Claude Desktop 运行时汉化目前支持 Windows 和 macOS。");
    return false;
#endif
}

void DesktopEnhancementManager::requestWindowsInspectorAttach()
{
#if defined(Q_OS_WIN)
    const QString node = commandPath(QStringLiteral("node"));
    const QString powershell = !QStandardPaths::findExecutable(QStringLiteral("pwsh.exe")).isEmpty()
        ? QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"))
        : QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
    if (node.isEmpty() || powershell.isEmpty()) return;
    const QString script = QStringLiteral(
        "const{execFileSync}=require('child_process');let s='';try{s=execFileSync(%1,['-NoProfile','-Command',"
        "\"Get-Process -Name Claude -ErrorAction SilentlyContinue | ForEach-Object {$_.Id}\"],{encoding:'utf8'})}catch{};"
        "for(const p of s.split(/\\s+/).map(Number).filter(Boolean)){try{process._debugProcess(p)}catch{}}")
        .arg(jsonString(powershell));
    QProcess::startDetached(node, { QStringLiteral("-e"), script });
#endif
}

void DesktopEnhancementManager::pollInspector()
{
    if (!m_localizationRunning) return;
    if (++m_pollAttempts > 30) {
        finishLocalization(false,
            QStringLiteral("未能打开 Claude 调试端口。请关闭 Claude 后重试；Windows 用户可能需要以管理员身份运行 Aegisy。"));
        return;
    }
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:9229/json/list")));
    QNetworkReply *reply = m_networkManager->get(request);
    QTimer::singleShot(900, reply, [reply]() {
        if (reply->isRunning()) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (ok) {
            const QJsonArray targets = QJsonDocument::fromJson(data).array();
            for (const QJsonValue &value : targets) {
                const QString url = value.toObject()
                    .value(QStringLiteral("webSocketDebuggerUrl")).toString();
                if (!url.isEmpty()) {
                    emit localizationProgress(QStringLiteral("已连接 Claude 调试器，正在注入中文界面..."));
                    connectInspector(url);
                    return;
                }
            }
        }
        QTimer::singleShot(600, this, &DesktopEnhancementManager::pollInspector);
    });
}

void DesktopEnhancementManager::connectInspector(const QString &webSocketUrl)
{
    if (m_webSocket) {
        m_webSocket->deleteLater();
        m_webSocket = nullptr;
    }
    m_webSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    QWebSocket *socket = m_webSocket;
    connect(socket, &QWebSocket::connected, this, [socket]() {
        const QJsonObject request{
            { QStringLiteral("id"), 1 },
            { QStringLiteral("method"), QStringLiteral("Runtime.evaluate") },
            { QStringLiteral("params"), QJsonObject{
                { QStringLiteral("expression"), claudeLocalizationExpression() },
                { QStringLiteral("awaitPromise"), true },
                { QStringLiteral("returnByValue"), true }
            }}
        };
        socket->sendTextMessage(QString::fromUtf8(
            QJsonDocument(request).toJson(QJsonDocument::Compact)));
    });
    connect(socket, &QWebSocket::textMessageReceived, this,
            [this, socket](const QString &message) {
        const QJsonObject response = QJsonDocument::fromJson(message.toUtf8()).object();
        if (response.value(QStringLiteral("id")).toInt() != 1) return;
        const QJsonObject result = response.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("result")).toObject();
        const QString value = result.value(QStringLiteral("value")).toString();
        const QJsonObject payload = QJsonDocument::fromJson(value.toUtf8()).object();
        socket->close();
        if (payload.value(QStringLiteral("ok")).toBool()) {
            finishLocalization(true, QStringLiteral("Claude Desktop 中文界面已注入，共处理 %1 个窗口。")
                .arg(payload.value(QStringLiteral("windows")).toInt()));
        } else {
            finishLocalization(false, payload.value(QStringLiteral("error")).toString(
                QStringLiteral("Claude 拒绝了运行时汉化请求。")));
        }
    });
    QTimer::singleShot(10000, socket, [this, socket]() {
        if (m_localizationRunning && socket == m_webSocket) {
            socket->abort();
            finishLocalization(false, QStringLiteral("Claude 汉化注入超时。"));
        }
    });
    socket->open(QUrl(webSocketUrl));
}

QString DesktopEnhancementManager::claudeLocalizationExpression()
{
    const QString renderer = QString::fromUtf8(R"JS(
(() => {
  const dictionary = {
    "New chat":"新对话","Chats":"对话","Projects":"项目","Artifacts":"工件",
    "Settings":"设置","General":"通用","Appearance":"外观","Account":"账号",
    "Profile":"个人资料","Search":"搜索","History":"历史记录","Help":"帮助",
    "Sign in":"登录","Sign out":"退出登录","Continue":"继续","Cancel":"取消",
    "Save":"保存","Delete":"删除","Close":"关闭","Open":"打开","Retry":"重试",
    "Upload":"上传","Download":"下载","Copy":"复制","Edit":"编辑","Done":"完成",
    "Allow":"允许","Always allow":"始终允许","Deny":"拒绝","Model":"模型",
    "Privacy":"隐私","Advanced":"高级","Notifications":"通知","Keyboard shortcuts":"快捷键"
  };
  const replacements = [
    [/New conversation/gi,"新对话"],[/Start a new chat/gi,"开始新对话"],
    [/What can I help you with\?/gi,"我能帮你做什么？"],
    [/Search chats/gi,"搜索对话"],[/Recent chats/gi,"最近对话"]
  ];
  const translate = value => {
    const trimmed = value.trim();
    if (!trimmed || /[\u3400-\u9fff]/.test(trimmed)) return value;
    let next = dictionary[trimmed] || trimmed;
    for (const [pattern, replacement] of replacements) next = next.replace(pattern, replacement);
    if (next === trimmed) return value;
    return value.replace(trimmed, next);
  };
  const apply = root => {
    if (!root) return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      if (node.parentElement?.closest('script,style,textarea,[contenteditable="true"]')) continue;
      if ((node.nodeValue || '').length <= 240) node.nodeValue = translate(node.nodeValue || '');
    }
    for (const element of root.querySelectorAll?.('[aria-label],[title],[placeholder]') || []) {
      for (const attr of ['aria-label','title','placeholder']) {
        const value = element.getAttribute(attr);
        if (value) element.setAttribute(attr, translate(value));
      }
    }
  };
  window.__AEGISY_CLAUDE_ZH__?.observer?.disconnect?.();
  apply(document.documentElement);
  document.documentElement.lang = 'zh-CN';
  const observer = new MutationObserver(records => {
    for (const record of records) {
      for (const node of record.addedNodes) {
        if (node.nodeType === Node.ELEMENT_NODE) apply(node);
        else if (node.nodeType === Node.TEXT_NODE) node.nodeValue = translate(node.nodeValue || '');
      }
    }
  });
  observer.observe(document.documentElement, {subtree:true,childList:true});
  window.__AEGISY_CLAUDE_ZH__ = {observer, version:1};
  return true;
})()
)JS");

    return QStringLiteral(R"JS((async () => {
  try {
    const req = typeof require === 'function'
      ? require
      : process.getBuiltinModule('module').createRequire(process.execPath);
    const electron = req('electron');
    const app = electron.app;
    const identity = [app?.getName?.(), app?.getAppPath?.(), process.execPath].join(' ').toLowerCase();
    if (!identity.includes('claude')) return JSON.stringify({ok:false,error:'调试端口不属于 Claude Desktop。'});
    const windows = electron.BrowserWindow.getAllWindows();
    const source = %1;
    await Promise.all(windows.map(win => win.webContents.executeJavaScript(source, true).catch(() => false)));
    globalThis.__AEGISY_CLAUDE_ZH_MAIN__ = {version:1, injectedAt:Date.now()};
    return JSON.stringify({ok:true,windows:windows.length});
  } catch (error) {
    return JSON.stringify({ok:false,error:String(error?.message || error)});
  }
})())JS").arg(jsonString(renderer));
}

void DesktopEnhancementManager::finishLocalization(bool success, const QString &message)
{
    if (!m_localizationRunning) return;
    m_localizationRunning = false;
    if (m_webSocket) {
        m_webSocket->deleteLater();
        m_webSocket = nullptr;
    }
    emit localizationFinished(success, message);
}
