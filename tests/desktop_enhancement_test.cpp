#include "desktop_enhancement_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <iostream>

namespace {

bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

int fail(const QString &message)
{
    std::cerr << message.toStdString() << '\n';
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QString root = QDir::tempPath() + QStringLiteral("/aegisy-desktop-test-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString codexHome = root + QStringLiteral("/.codex");
    QDir().mkpath(codexHome + QStringLiteral("/sessions/2026/07/13"));
    QDir().mkpath(codexHome + QStringLiteral("/sqlite"));

#ifdef Q_OS_WIN
    const QString fixtureBin = root + QStringLiteral("/npm");
    if (!writeFile(fixtureBin + QStringLiteral("/codex"),
                   QByteArray("#!/bin/sh\necho wrong shim\n"))
            || !writeFile(fixtureBin + QStringLiteral("/codex.cmd"),
                QByteArray("@echo off\r\n"
                           "echo {\"installed\":[],\"available\":[{\"pluginId\":\"fixture\",\"name\":\"fixture\",\"version\":\"1.0.0\"}]}\r\n"))) {
        return fail(QStringLiteral("failed to write Codex command fixture"));
    }
    const QByteArray previousPath = qgetenv("PATH");
    const QByteArray previousAppData = qgetenv("APPDATA");
    qputenv("APPDATA", root.toLocal8Bit());
    qputenv("PATH", fixtureBin.toLocal8Bit()
        + QByteArray(1, QDir::listSeparator().toLatin1()) + previousPath);
    DesktopEnhancementManager pluginManager;
    QString pluginError;
    const QList<CodexPluginInfo> plugins = pluginManager.listCodexPlugins(&pluginError);
    qputenv("PATH", previousPath);
    qputenv("APPDATA", previousAppData);
    if (!pluginError.isEmpty() || plugins.size() != 1
            || plugins.first().id != QStringLiteral("fixture")) {
        return fail(QStringLiteral("Windows Codex .cmd shim was not executed: %1")
            .arg(pluginError));
    }
#endif

    if (!writeFile(codexHome + QStringLiteral("/config.toml"),
                   QByteArray("model_provider = \"aegisy\"\n\n[model_providers.aegisy]\n"))) {
        return fail(QStringLiteral("failed to write config"));
    }
    const QString sessionPath = codexHome
        + QStringLiteral("/sessions/2026/07/13/rollout-test.jsonl");
    const QByteArray session =
        "{\"type\":\"session_meta\",\"payload\":{\"id\":\"thread-a\",\"cwd\":\"C:/repo\",\"model_provider\":\"old\"}}\n"
        "{\"type\":\"user_message\",\"payload\":{\"text\":\"hello\"}}\n";
    if (!writeFile(sessionPath, session)) return fail(QStringLiteral("failed to write session"));

    const QString dbPath = codexHome + QStringLiteral("/sqlite/codex-test.db");
    const QString connection = QStringLiteral("setup-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(dbPath);
        if (!database.open()) return fail(QStringLiteral("failed to open sqlite"));
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE threads (id TEXT PRIMARY KEY, model_provider TEXT, "
                "has_user_event INTEGER, cwd TEXT)"))
                || !query.exec(QStringLiteral(
                    "INSERT INTO threads VALUES ('thread-a','old',0,'')"))) {
            return fail(QStringLiteral("failed to create sqlite fixture"));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);

    QString error;
    const SessionSyncReport report = DesktopEnhancementManager::syncCodexHistoryAt(
        codexHome, &error);
    if (!error.isEmpty()) return fail(error);
    if (report.provider != QStringLiteral("aegisy") || report.sessionFilesChanged != 1
            || report.databaseRowsChanged < 3 || report.backupPath.isEmpty()) {
        return fail(QStringLiteral("unexpected sync report"));
    }

    QFile updatedSession(sessionPath);
    if (!updatedSession.open(QIODevice::ReadOnly)
            || !updatedSession.readAll().contains("\"model_provider\":\"aegisy\"")) {
        return fail(QStringLiteral("session provider was not updated"));
    }
    if (!QFileInfo::exists(report.backupPath + QStringLiteral("/manifest.json"))) {
        return fail(QStringLiteral("backup manifest is missing"));
    }

    const QString verifyConnection = QStringLiteral("verify-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                           verifyConnection);
        database.setDatabaseName(dbPath);
        if (!database.open()) return fail(QStringLiteral("failed to reopen sqlite"));
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral(
                "SELECT model_provider, has_user_event, cwd FROM threads WHERE id='thread-a'"))
                || !query.next()
                || query.value(0).toString() != QStringLiteral("aegisy")
                || query.value(1).toInt() != 1
                || query.value(2).toString() != QStringLiteral("C:/repo")) {
            return fail(QStringLiteral("sqlite history index was not synchronized"));
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(verifyConnection);
    QDir(root).removeRecursively();
    return 0;
}
