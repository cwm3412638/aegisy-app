#include "skill_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QUrl>

#include <algorithm>

namespace {

constexpr qint64 kMaximumSkillFileSize = 2 * 1024 * 1024;
constexpr qint64 kMaximumSkillPackageSize = 12 * 1024 * 1024;

QStringList jsonStringList(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) result.append(text);
    }
    return result;
}

bool safeRelativePath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(path);
    return !cleaned.isEmpty() && cleaned != QStringLiteral(".")
        && !QDir::isAbsolutePath(cleaned)
        && !cleaned.startsWith(QStringLiteral("../"))
        && !cleaned.contains(QStringLiteral("/../"))
        && !cleaned.contains(QLatin1Char('\\'));
}

QString yamlDoubleQuoted(QString value)
{
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QJsonObject parseFrontmatter(const QByteArray &markdown)
{
    const QString text = QString::fromUtf8(markdown);
    if (!text.startsWith(QStringLiteral("---"))) return QJsonObject();
    const int end = text.indexOf(QStringLiteral("\n---"), 3);
    if (end < 0) return QJsonObject();
    QJsonObject result;
    const QStringList lines = text.mid(3, end - 3).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) continue;
        QString value = line.mid(colon + 1).trimmed();
        if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
            value = value.mid(1, value.size() - 2);
        }
        result.insert(line.left(colon).trimmed(), value);
    }
    return result;
}

} // namespace

SkillManager::SkillManager(QObject *parent, const QString &skillsRoot)
    : QObject(parent)
    , m_skillsRoot(skillsRoot.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                + QStringLiteral("/skills")
          : skillsRoot)
    , m_networkManager(new QNetworkAccessManager(this))
{
    QDir().mkpath(m_skillsRoot);
    ensureBuiltInSkills();
    refresh();
}

QList<SkillInfo> SkillManager::skills() const
{
    return m_skills;
}

QList<SkillCatalogInfo> SkillManager::catalogSkills() const
{
    QFile file(QStringLiteral(":/skill-catalog/catalog.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonArray entries = QJsonDocument::fromJson(file.readAll()).array();
    QList<SkillCatalogInfo> result;
    result.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject object = value.toObject();
        SkillCatalogInfo entry;
        entry.id = object.value(QStringLiteral("id")).toString();
        entry.directory = object.value(QStringLiteral("directory")).toString();
        entry.skillName = object.value(QStringLiteral("skill_name")).toString(entry.directory);
        entry.name = object.value(QStringLiteral("name")).toString(entry.skillName);
        entry.category = object.value(QStringLiteral("category")).toString();
        entry.description = object.value(QStringLiteral("description")).toString();
        entry.source = object.value(QStringLiteral("source")).toString(
            QStringLiteral("Aegisy 精选"));
        entry.requirements = jsonStringList(object.value(QStringLiteral("requirements")));
        entry.triggers = jsonStringList(object.value(QStringLiteral("triggers")));
        entry.instructions = object.value(QStringLiteral("instructions")).toString();
        if (!entry.id.isEmpty() && !entry.directory.isEmpty()) result.append(entry);
    }
    return result;
}

SkillInfo SkillManager::skill(const QString &id) const
{
    for (const SkillInfo &item : m_skills) {
        if (item.id == id) return item;
    }
    return SkillInfo();
}

QString SkillManager::skillInstructions(const QString &id) const
{
    const SkillInfo current = skill(id);
    if (current.id.isEmpty() || !current.enabled || !current.compatible || !current.trusted) {
        return QString();
    }
    QFile file(current.path + QStringLiteral("/SKILL.md"));
    if (!file.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(file.read(64 * 1024)).trimmed();
}

SkillInfo SkillManager::matchSkill(const QString &text) const
{
    const QString normalized = text.trimmed().toLower();
    SkillInfo best;
    int bestLength = 0;
    for (const SkillInfo &item : m_skills) {
        if (!item.enabled || !item.compatible) continue;
        if (item.executor == QStringLiteral("instruction") && !item.trusted) continue;
        const bool createAction = normalized.contains(QStringLiteral("生成"))
            || normalized.contains(QStringLiteral("制作"))
            || normalized.contains(QStringLiteral("创建"))
            || normalized.contains(QStringLiteral("帮我做"))
            || normalized.startsWith(QStringLiteral("/"));
        const bool presentationTarget = normalized.contains(QStringLiteral("ppt"))
            || normalized.contains(QStringLiteral("powerpoint"))
            || normalized.contains(QStringLiteral("幻灯片"))
            || normalized.contains(QStringLiteral("演示文稿"));
        if (item.executor == QStringLiteral("presentation")
                && createAction && presentationTarget) {
            return item;
        }
        const bool imageAction = createAction || normalized.contains(QStringLiteral("画"))
            || normalized.contains(QStringLiteral("设计"))
            || normalized.startsWith(QStringLiteral("/image"));
        const bool imageTarget = normalized.contains(QStringLiteral("图片"))
            || normalized.contains(QStringLiteral("生图"))
            || normalized.contains(QStringLiteral("海报"))
            || normalized.contains(QStringLiteral("插画"))
            || normalized.contains(QStringLiteral("产品图"));
        const bool negative = normalized.contains(QStringLiteral("不要生成"))
            || normalized.contains(QStringLiteral("不需要生成"))
            || normalized.contains(QStringLiteral("不要制作"));
        if (item.executor == QStringLiteral("image") && negative) continue;
        if (item.executor == QStringLiteral("image") && imageAction && imageTarget && !negative) {
            return item;
        }
        for (const QString &trigger : item.triggers) {
            const QString candidate = trigger.trimmed().toLower();
            if (!candidate.isEmpty() && normalized.contains(candidate)
                    && candidate.size() > bestLength) {
                best = item;
                bestLength = candidate.size();
            }
        }
    }
    return best;
}

QString SkillManager::skillsRoot() const
{
    return m_skillsRoot;
}

void SkillManager::refresh()
{
    QList<SkillInfo> loaded;
    const QFileInfoList directories = QDir(m_skillsRoot).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &directory : directories) {
        if (directory.fileName().startsWith(QStringLiteral(".install-"))) continue;
        SkillInfo info = loadSkill(directory.absoluteFilePath());
        if (!info.id.isEmpty()) loaded.append(info);
    }
    std::sort(loaded.begin(), loaded.end(), [](const SkillInfo &left, const SkillInfo &right) {
        if (left.builtin != right.builtin) return left.builtin > right.builtin;
        return left.name.localeAwareCompare(right.name) < 0;
    });
    m_skills = loaded;
    emit skillsChanged();
}

SkillInfo SkillManager::loadSkill(const QString &directory) const
{
    SkillInfo info;
    info.path = directory;
    QFile manifestFile(directory + QStringLiteral("/aegisy-skill.json"));
    QJsonObject manifest;
    if (manifestFile.open(QIODevice::ReadOnly)) {
        manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    }
    QFile skillFile(directory + QStringLiteral("/SKILL.md"));
    QJsonObject frontmatter;
    if (skillFile.open(QIODevice::ReadOnly)) frontmatter = parseFrontmatter(skillFile.readAll());

    info.id = manifest.value(QStringLiteral("id")).toString();
    if (info.id.isEmpty()) info.id = QFileInfo(directory).fileName();
    info.name = manifest.value(QStringLiteral("name")).toString(
        frontmatter.value(QStringLiteral("name")).toString(info.id));
    info.description = manifest.value(QStringLiteral("description")).toString(
        frontmatter.value(QStringLiteral("description")).toString());
    info.version = manifest.value(QStringLiteral("version")).toString(QStringLiteral("1.0.0"));
    info.executor = manifest.value(QStringLiteral("executor")).toString(QStringLiteral("instruction"));
    info.source = manifest.value(QStringLiteral("source")).toString(
        manifest.value(QStringLiteral("builtin")).toBool()
            ? QStringLiteral("Aegisy 内置") : QStringLiteral("本地导入"));
    info.requiredGroup = manifest.value(QStringLiteral("required_group")).toString();
    info.triggers = jsonStringList(manifest.value(QStringLiteral("triggers")));
    info.permissions = jsonStringList(manifest.value(QStringLiteral("permissions")));
    info.enabled = manifest.value(QStringLiteral("enabled")).toBool(false);
    info.trusted = manifest.value(QStringLiteral("trusted")).toBool(false);
    info.builtin = manifest.value(QStringLiteral("builtin")).toBool(false);
    info.compatible = QFileInfo::exists(directory + QStringLiteral("/SKILL.md"));
    if (info.executor == QStringLiteral("presentation")) {
        info.compatible = info.compatible
            && QFileInfo::exists(directory + QStringLiteral("/scripts/create_ppt.py"));
    }
    return info;
}

bool SkillManager::writeManifest(const QString &directory,
                                 const QJsonObject &manifest,
                                 QString *error) const
{
    QSaveFile file(directory + QStringLiteral("/aegisy-skill.json"));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入 Skill 清单：%1").arg(file.errorString());
        return false;
    }
    file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = QStringLiteral("无法提交 Skill 清单：%1").arg(file.errorString());
        return false;
    }
    return true;
}

bool SkillManager::setEnabled(const QString &id, bool enabled, QString *error)
{
    const SkillInfo current = skill(id);
    if (current.id.isEmpty()) {
        if (error) *error = QStringLiteral("找不到 Skill：%1").arg(id);
        return false;
    }
    QFile file(current.path + QStringLiteral("/aegisy-skill.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取 Skill 清单。");
        return false;
    }
    QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
    manifest.insert(QStringLiteral("enabled"), enabled);
    if (!writeManifest(current.path, manifest, error)) return false;
    refresh();
    return true;
}

bool SkillManager::removeSkill(const QString &id, QString *error)
{
    const SkillInfo current = skill(id);
    if (current.id.isEmpty()) return true;
    if (current.builtin) {
        if (error) *error = QStringLiteral("内置 Skill 不能删除，可以将其禁用。");
        return false;
    }
    if (!QDir(current.path).removeRecursively()) {
        if (error) *error = QStringLiteral("无法删除 Skill 目录：%1").arg(current.path);
        return false;
    }
    refresh();
    return true;
}

bool SkillManager::copyDirectory(const QString &source,
                                 const QString &destination,
                                 QString *error,
                                 qint64 *totalSize) const
{
    QDir().mkpath(destination);
    const QDir sourceDir(source);
    const QFileInfoList entries = sourceDir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo &entry : entries) {
        const QString target = destination + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            if (!copyDirectory(entry.absoluteFilePath(), target, error, totalSize)) return false;
        } else {
            *totalSize += entry.size();
            if (entry.size() > kMaximumSkillFileSize || *totalSize > kMaximumSkillPackageSize) {
                if (error) *error = QStringLiteral("Skill 包超过安全大小限制。");
                return false;
            }
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (!QFile::copy(entry.absoluteFilePath(), target)) {
                if (error) *error = QStringLiteral("复制 Skill 文件失败：%1").arg(entry.fileName());
                return false;
            }
        }
    }
    return true;
}

QString SkillManager::safeSkillId(const QString &value) const
{
    QString id = value.trimmed().toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9._-]+")), QStringLiteral("-"));
    id.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return id.left(80);
}

bool SkillManager::installFromDirectory(const QString &sourceDirectory, QString *error)
{
    QFile skillFile(sourceDirectory + QStringLiteral("/SKILL.md"));
    if (!skillFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("所选目录没有 SKILL.md。");
        return false;
    }
    const QJsonObject frontmatter = parseFrontmatter(skillFile.readAll());
    const QString id = safeSkillId(frontmatter.value(QStringLiteral("name")).toString(
        QFileInfo(sourceDirectory).fileName()));
    if (id.isEmpty()) {
        if (error) *error = QStringLiteral("无法识别 Skill 名称。");
        return false;
    }
    const QString destination = m_skillsRoot + QLatin1Char('/') + id;
    if (QFileInfo::exists(destination)) {
        if (error) *error = QStringLiteral("Skill 已存在：%1").arg(id);
        return false;
    }
    const QString temporary = m_skillsRoot + QStringLiteral("/.install-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    qint64 totalSize = 0;
    if (!copyDirectory(sourceDirectory, temporary, error, &totalSize)) {
        QDir(temporary).removeRecursively();
        return false;
    }
    QJsonObject manifest{
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), frontmatter.value(QStringLiteral("name")).toString(id) },
        { QStringLiteral("description"), frontmatter.value(QStringLiteral("description")).toString() },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("executor"), QStringLiteral("instruction") },
        { QStringLiteral("enabled"), false },
        { QStringLiteral("trusted"), false },
        { QStringLiteral("builtin"), false },
        { QStringLiteral("source"), QStringLiteral("本地目录：%1").arg(sourceDirectory) }
    };
    if (!writeManifest(temporary, manifest, error)
            || !QDir().rename(temporary, destination)) {
        QDir(temporary).removeRecursively();
        if (error && error->isEmpty()) *error = QStringLiteral("无法完成 Skill 安装。");
        return false;
    }
    refresh();
    return true;
}

QString SkillManager::normalizedPackageUrl(const QString &url) const
{
    QString normalized = url.trimmed();
    if (normalized.endsWith(QStringLiteral("/INSTALL.md"), Qt::CaseInsensitive)) {
        normalized.chop(QStringLiteral("/INSTALL.md").size());
    } else if (normalized.endsWith(QStringLiteral("/SKILL.md"), Qt::CaseInsensitive)) {
        normalized.chop(QStringLiteral("/SKILL.md").size());
    }
    while (normalized.endsWith(QLatin1Char('/'))) normalized.chop(1);
    return normalized;
}

QByteArray SkillManager::download(const QUrl &url, bool required, QString *error) const
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        if (required && error) *error = QStringLiteral("只允许通过 HTTPS 安装 Skill。");
        return QByteArray();
    }
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "text/plain, text/markdown, application/json, */*");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(20000);
#endif
    QNetworkReply *reply = m_networkManager->get(request);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(20000);
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    const QString networkError = reply->errorString();
    const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300
        && data.size() <= kMaximumSkillFileSize;
    reply->deleteLater();
    if (!ok && required && error) {
        *error = data.size() > kMaximumSkillFileSize
            ? QStringLiteral("Skill 文件超过 2 MB 限制。")
            : QStringLiteral("下载失败：%1").arg(networkError);
    }
    return ok ? data : QByteArray();
}

bool SkillManager::installFromUrl(const QString &url, QString *error)
{
    const QString base = normalizedPackageUrl(url);
    const QUrl baseUrl(base);
    if (!baseUrl.isValid() || baseUrl.scheme() != QStringLiteral("https")) {
        if (error) *error = QStringLiteral("请输入有效的 HTTPS Skill 地址。");
        return false;
    }
    const QByteArray skillMarkdown = download(QUrl(base + QStringLiteral("/SKILL.md")), true, error);
    if (skillMarkdown.isEmpty()) return false;
    const QJsonObject frontmatter = parseFrontmatter(skillMarkdown);
    const QString id = safeSkillId(frontmatter.value(QStringLiteral("name")).toString(
        QFileInfo(baseUrl.path()).fileName()));
    if (id.isEmpty()) {
        if (error) *error = QStringLiteral("SKILL.md 缺少有效 name。");
        return false;
    }
    const QString destination = m_skillsRoot + QLatin1Char('/') + id;
    if (QFileInfo::exists(destination)) {
        if (error) *error = QStringLiteral("Skill 已存在：%1").arg(id);
        return false;
    }
    const QString temporary = m_skillsRoot + QStringLiteral("/.install-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(temporary);

    QStringList files{ QStringLiteral("SKILL.md"), QStringLiteral("INSTALL.md"),
                       QStringLiteral("package.json"), QStringLiteral("agents/openai.yaml") };
    const QString markdown = QString::fromUtf8(skillMarkdown);
    QRegularExpression resourcePattern(QStringLiteral(
        "(?:`|\\()((?:scripts|references|agents)/[A-Za-z0-9._/-]+|package\\.json|INSTALL\\.md)(?:`|\\))"));
    QRegularExpressionMatchIterator iterator = resourcePattern.globalMatch(markdown);
    while (iterator.hasNext()) files.append(iterator.next().captured(1));
    files.removeDuplicates();

    qint64 total = 0;
    for (const QString &relativePath : files) {
        if (!safeRelativePath(relativePath)) continue;
        QByteArray data = relativePath == QStringLiteral("SKILL.md")
            ? skillMarkdown : download(QUrl(base + QLatin1Char('/') + relativePath), false, nullptr);
        if (data.isEmpty()) continue;
        total += data.size();
        if (total > kMaximumSkillPackageSize) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("Skill 包超过 12 MB 限制。");
            return false;
        }
        const QString path = temporary + QLatin1Char('/') + relativePath;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("保存 Skill 文件失败：%1").arg(relativePath);
            return false;
        }
    }
    QJsonObject manifest{
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), frontmatter.value(QStringLiteral("name")).toString(id) },
        { QStringLiteral("description"), frontmatter.value(QStringLiteral("description")).toString() },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("executor"), QStringLiteral("instruction") },
        { QStringLiteral("enabled"), false },
        { QStringLiteral("trusted"), false },
        { QStringLiteral("builtin"), false },
        { QStringLiteral("source"), base }
    };
    if (!writeManifest(temporary, manifest, error)
            || !QDir().rename(temporary, destination)) {
        QDir(temporary).removeRecursively();
        if (error && error->isEmpty()) *error = QStringLiteral("无法完成 Skill 安装。");
        return false;
    }
    refresh();
    return true;
}

bool SkillManager::installBuiltInSkill(const QString &directoryName,
                                       const QStringList &resourceFiles)
{
    const QString destination = m_skillsRoot + QLatin1Char('/') + directoryName;
    const QString manifestPath = destination + QStringLiteral("/aegisy-skill.json");
    if (QFileInfo::exists(manifestPath)) {
        bool enabled = true;
        QFile existingManifest(manifestPath);
        if (existingManifest.open(QIODevice::ReadOnly)) {
            enabled = QJsonDocument::fromJson(existingManifest.readAll()).object()
                .value(QStringLiteral("enabled")).toBool(true);
        }
        for (const QString &relative : resourceFiles) {
            QFile source(QStringLiteral(":/builtin-skills/") + directoryName
                         + QLatin1Char('/') + relative);
            if (!source.open(QIODevice::ReadOnly)) return false;
            const QByteArray data = source.readAll();
            if (relative == QStringLiteral("aegisy-skill.json")) {
                QJsonObject manifest = QJsonDocument::fromJson(data).object();
                manifest.insert(QStringLiteral("enabled"), enabled);
                if (!writeManifest(destination, manifest, nullptr)) return false;
                continue;
            }
            const QString target = destination + QLatin1Char('/') + relative;
            QDir().mkpath(QFileInfo(target).absolutePath());
            QSaveFile output(target);
            if (!output.open(QIODevice::WriteOnly)
                    || output.write(data) != data.size() || !output.commit()) {
                return false;
            }
        }
        return true;
    }
    return installResourceSkill(QStringLiteral(":/builtin-skills"), directoryName,
                                resourceFiles, nullptr);
}

bool SkillManager::installResourceSkill(const QString &resourcePrefix,
                                        const QString &directoryName,
                                        const QStringList &resourceFiles,
                                        QString *error)
{
    const QString destination = m_skillsRoot + QLatin1Char('/') + directoryName;
    if (QFileInfo::exists(destination + QStringLiteral("/aegisy-skill.json"))) return true;
    const QString temporary = m_skillsRoot + QStringLiteral("/.install-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(temporary);
    for (const QString &relative : resourceFiles) {
        QFile source(resourcePrefix + QLatin1Char('/') + directoryName
                     + QLatin1Char('/') + relative);
        if (!source.open(QIODevice::ReadOnly)) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("安装包缺少文件：%1").arg(relative);
            return false;
        }
        const QString target = temporary + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("无法写入 Skill 文件：%1").arg(relative);
            return false;
        }
        output.write(source.readAll());
        if (!output.commit()) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("无法保存 Skill 文件：%1").arg(relative);
            return false;
        }
    }
    if (!QDir().rename(temporary, destination)) {
        QDir(temporary).removeRecursively();
        if (error) *error = QStringLiteral("无法完成 Skill 安装。");
        return false;
    }
    return true;
}

bool SkillManager::installCatalogSkill(const QString &id, QString *error)
{
    const QList<SkillCatalogInfo> catalog = catalogSkills();
    const auto found = std::find_if(catalog.cbegin(), catalog.cend(), [&](const SkillCatalogInfo &item) {
        return item.id == id;
    });
    if (found == catalog.cend()) {
        if (error) *error = QStringLiteral("找不到可安装 Skill：%1").arg(id);
        return false;
    }
    if (!skill(id).id.isEmpty()) {
        if (error) *error = QStringLiteral("Skill 已安装。");
        return false;
    }
    const QString resourceManifest = QStringLiteral(":/catalog-skills/")
        + found->directory + QStringLiteral("/aegisy-skill.json");
    if (QFileInfo::exists(resourceManifest)) {
        if (!installResourceSkill(QStringLiteral(":/catalog-skills"), found->directory,
                                  { QStringLiteral("aegisy-skill.json"), QStringLiteral("SKILL.md") },
                                  error)) {
            return false;
        }
    } else {
        const QString temporary = m_skillsRoot + QStringLiteral("/.install-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QDir().mkpath(temporary);
        const QJsonArray triggers = QJsonArray::fromStringList(found->triggers);
        const QJsonObject manifest{
            { QStringLiteral("id"), found->id },
            { QStringLiteral("name"), found->name },
            { QStringLiteral("version"), QStringLiteral("1.0.0") },
            { QStringLiteral("description"), found->description },
            { QStringLiteral("executor"), QStringLiteral("instruction") },
            { QStringLiteral("enabled"), true },
            { QStringLiteral("trusted"), true },
            { QStringLiteral("builtin"), false },
            { QStringLiteral("source"), QStringLiteral("Aegisy Skills") },
            { QStringLiteral("permissions"), QJsonArray{
                QStringLiteral("conversation-context")
            }},
            { QStringLiteral("triggers"), triggers }
        };
        if (!writeManifest(temporary, manifest, error)) {
            QDir(temporary).removeRecursively();
            return false;
        }
        const QString markdown = QStringLiteral(
            "---\nname: %1\ndescription: %2\n---\n\n# %3\n\n%4\n")
            .arg(found->skillName, yamlDoubleQuoted(found->description),
                 found->name, found->instructions);
        QSaveFile skillFile(temporary + QStringLiteral("/SKILL.md"));
        if (!skillFile.open(QIODevice::WriteOnly)
                || skillFile.write(markdown.toUtf8()) != markdown.toUtf8().size()
                || !skillFile.commit()
                || !QDir().rename(temporary, m_skillsRoot + QLatin1Char('/') + found->directory)) {
            QDir(temporary).removeRecursively();
            if (error) *error = QStringLiteral("无法写入精选 Skill 文件。");
            return false;
        }
    }
    refresh();
    return true;
}

void SkillManager::ensureBuiltInSkills()
{
    installBuiltInSkill(QStringLiteral("image-generation"),
                        { QStringLiteral("aegisy-skill.json"), QStringLiteral("SKILL.md") });
    installBuiltInSkill(QStringLiteral("presentation"),
                        { QStringLiteral("aegisy-skill.json"), QStringLiteral("SKILL.md"),
                          QStringLiteral("scripts/create_ppt.py") });
}

QString SkillManager::presentationPython() const
{
#ifdef Q_OS_WIN
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/skill-runtime/presentation/Scripts/python.exe");
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/skill-runtime/presentation/bin/python3");
#endif
}

bool SkillManager::presentationRuntimeReady() const
{
    const QString python = presentationPython();
    if (!QFileInfo::exists(python)) return false;
    QProcess process;
    process.start(python, { QStringLiteral("-c"), QStringLiteral("import pptx") });
    return process.waitForStarted(3000) && process.waitForFinished(10000)
        && process.exitCode() == 0;
}

bool SkillManager::installPresentationRuntime(QString *error)
{
    const QString venvRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/skill-runtime/presentation");
    QDir().mkpath(QFileInfo(venvRoot).absolutePath());
    QString systemPython = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (systemPython.isEmpty()) systemPython = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (systemPython.isEmpty()) {
        if (error) *error = QStringLiteral("未找到 Python 3，请先在系统体检中安装。");
        return false;
    }
    if (!QFileInfo::exists(presentationPython())) {
        QProcess create;
        create.setProcessChannelMode(QProcess::MergedChannels);
        create.start(systemPython, { QStringLiteral("-m"), QStringLiteral("venv"),
                                     venvRoot });
        if (!create.waitForStarted(5000) || !create.waitForFinished(120000)
                || create.exitCode() != 0) {
            if (error) *error = QString::fromUtf8(create.readAll()).trimmed();
            return false;
        }
    }
    QProcess install;
    install.setProcessChannelMode(QProcess::MergedChannels);
    install.start(presentationPython(), { QStringLiteral("-m"), QStringLiteral("pip"),
                                          QStringLiteral("install"), QStringLiteral("--disable-pip-version-check"),
                                          QStringLiteral("python-pptx>=1.0,<2") });
    if (!install.waitForStarted(5000) || !install.waitForFinished(5 * 60 * 1000)
            || install.exitCode() != 0) {
        if (error) *error = QString::fromUtf8(install.readAll()).trimmed();
        return false;
    }
    return presentationRuntimeReady();
}

bool SkillManager::executePresentation(const QJsonObject &plan,
                                       const QString &outputPath,
                                       QString *error) const
{
    if (!presentationRuntimeReady()) {
        if (error) *error = QStringLiteral("PPT 运行环境尚未安装，请先在 Skills 管理中安装。");
        return false;
    }
    const SkillInfo presentation = skill(QStringLiteral("aegisy.presentation.create"));
    if (presentation.id.isEmpty()) {
        if (error) *error = QStringLiteral("找不到 PPT Skill。");
        return false;
    }
    const QString temporary = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/aegisy-ppt-") + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QStringLiteral(".json");
    QSaveFile spec(temporary);
    if (!spec.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法创建 PPT 计划文件。");
        return false;
    }
    spec.write(QJsonDocument(plan).toJson(QJsonDocument::Compact));
    if (!spec.commit()) return false;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(presentationPython(), {
        presentation.path + QStringLiteral("/scripts/create_ppt.py"),
        QStringLiteral("--spec"), temporary,
        QStringLiteral("--out"), outputPath
    });
    const bool finished = process.waitForStarted(5000) && process.waitForFinished(120000);
    QFile::remove(temporary);
    if (!finished || process.exitCode() != 0 || !QFileInfo::exists(outputPath)) {
        if (error) *error = QString::fromUtf8(process.readAll()).trimmed();
        if (error && error->isEmpty()) *error = QStringLiteral("PPT 文件生成失败。");
        return false;
    }
    return true;
}
