#include "desktop_downloader.h"
#include "app_theme.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QUrl>
#include <QSysInfo>
#include <QRegularExpression>
#include <QFileInfo>

DesktopDownloader::DesktopDownloader(Product product,
                                     const QString &appName,
                                     const QString &authToken,
                                     const QString &baseUrl,
                                     QWidget *parent)
    : QDialog(parent)
    , m_product(product)
    , m_appName(appName)
    , m_authToken(authToken)
    , m_baseUrl(baseUrl)
    , m_network(new QNetworkAccessManager(this))
{
    setWindowTitle(QStringLiteral("下载 %1").arg(appName));
    setModal(true);
    setMinimumWidth(420);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(14);

    m_statusLabel = new QLabel(
        QStringLiteral("正在通过 Aegisy 服务器下载 %1，请稍候…").arg(appName), this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #1f2937;"));
    root->addWidget(m_statusLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    root->addWidget(m_progressBar);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->addStretch();
    m_actionButton = new QPushButton(QStringLiteral("取消"), this);
    m_actionButton->setFixedHeight(34);
    m_actionButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(m_actionButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonRow->addWidget(m_actionButton);
    root->addLayout(buttonRow);

    startDownload();
}

bool DesktopDownloader::platformSupported()
{
    return !platformSlug().isEmpty();
}

QString DesktopDownloader::productSlug(Product product)
{
    switch (product) {
    case Product::Claude:  return QStringLiteral("claude");
    case Product::ChatGpt: return QStringLiteral("chatgpt");
    }
    return QStringLiteral("claude");
}

QString DesktopDownloader::platformSlug()
{
#if defined(Q_OS_MAC)
    const QString arch = QSysInfo::currentCpuArchitecture();
    // Apple Silicon 报告 "arm64"，Intel 报告 "x86_64"。
    return arch.contains(QStringLiteral("arm"))
        ? QStringLiteral("mac-arm64") : QStringLiteral("mac-x64");
#elif defined(Q_OS_WIN)
    return QStringLiteral("win-x64");
#else
    return QString();   // Linux/其他：暂无官方桌面安装包
#endif
}

QString DesktopDownloader::defaultFileName(Product product)
{
    const QString base = productSlug(product) == QStringLiteral("claude")
        ? QStringLiteral("Claude") : QStringLiteral("ChatGPT");
#if defined(Q_OS_MAC)
    return base + QStringLiteral(".dmg");
#elif defined(Q_OS_WIN)
    return base + QStringLiteral("-Setup.exe");
#else
    return base;
#endif
}

QString DesktopDownloader::officialDownloadUrl() const
{
    return m_product == Product::Claude
        ? QStringLiteral("https://claude.ai/download")
        : QStringLiteral("https://chatgpt.com/download");
}

bool DesktopDownloader::validateInstallerPayload(const QByteArray &payload,
                                                 QString *error) const
{
    const QString contentType = m_reply
        ? m_reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower()
        : QString();
    const QByteArray trimmed = payload.trimmed().left(64).toLower();
    if (payload.isEmpty()
            || contentType.contains(QStringLiteral("text/html"))
            || contentType.contains(QStringLiteral("application/json"))
            || trimmed.startsWith("<!doctype html")
            || trimmed.startsWith("<html")) {
        if (error) {
            *error = QStringLiteral(
                "下载服务返回了网页或错误信息，而不是安装包。");
        }
        return false;
    }
#if defined(Q_OS_WIN)
    if (!payload.startsWith("MZ")) {
        if (error) *error = QStringLiteral("下载内容不是有效的 Windows 安装程序。");
        return false;
    }
#elif defined(Q_OS_MAC)
    if (payload.size() < 512
            || payload.mid(payload.size() - 512, 4) != QByteArrayLiteral("koly")) {
        if (error) *error = QStringLiteral("下载内容不是有效的 macOS 磁盘映像。");
        return false;
    }
#endif
    return true;
}

void DesktopDownloader::startDownload()
{
    const QString platform = platformSlug();
    if (platform.isEmpty()) {
        fail(QStringLiteral("当前系统暂无官方桌面安装包，请前往官网获取。"));
        return;
    }

    QString base = m_baseUrl;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const QUrl url(QStringLiteral("%1/downloads/%2/%3")
                       .arg(base, productSlug(m_product), platform));

    QNetworkRequest request(url);
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization",
                             QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    }
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &DesktopDownloader::onProgress);
    connect(m_reply, &QNetworkReply::finished,
            this, &DesktopDownloader::onFinished);
}

void DesktopDownloader::onProgress(qint64 received, qint64 total)
{
    if (total > 0) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(static_cast<int>(received * 100 / total));
        m_statusLabel->setText(QStringLiteral("正在下载 %1  (%2 / %3 MB)")
            .arg(m_appName)
            .arg(received / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(total / (1024.0 * 1024.0), 0, 'f', 1));
    } else {
        // 服务器未提供 Content-Length：显示忙碌动画。
        m_progressBar->setRange(0, 0);
        m_statusLabel->setText(QStringLiteral("正在下载 %1  (%2 MB)")
            .arg(m_appName)
            .arg(received / (1024.0 * 1024.0), 0, 'f', 1));
    }
}

void DesktopDownloader::onFinished()
{
    if (!m_reply) return;

    if (m_reply->error() != QNetworkReply::NoError) {
        const QString error = m_reply->errorString();
        cleanupReply();
        const bool opened = QDesktopServices::openUrl(QUrl(officialDownloadUrl()));
        fail(QStringLiteral("下载失败：%1%2")
            .arg(error, opened ? QStringLiteral("，已为你打开官网下载页。")
                               : QStringLiteral("。")));
        return;
    }

    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        cleanupReply();
        const bool opened = QDesktopServices::openUrl(QUrl(officialDownloadUrl()));
        fail(QStringLiteral("下载服务返回 HTTP %1%2")
            .arg(status)
            .arg(opened ? QStringLiteral("，已为你打开官网下载页。")
                        : QStringLiteral("，请点击下方按钮前往官网下载。")));
        return;
    }

    const QByteArray payload = m_reply->readAll();
    QString validationError;
    if (!validateInstallerPayload(payload, &validationError)) {
        cleanupReply();
        const bool opened = QDesktopServices::openUrl(QUrl(officialDownloadUrl()));
        fail(validationError + (opened
            ? QStringLiteral(" 已停止保存，并为你打开了官网下载页。")
            : QStringLiteral(" 已停止保存，请点击下方按钮前往官网下载。")));
        return;
    }

    // 从 Content-Disposition 解析文件名，否则用默认名。
    QString fileName = defaultFileName(m_product);
    const QString disposition = QString::fromUtf8(
        m_reply->rawHeader("Content-Disposition"));
    static const QRegularExpression re(
        QStringLiteral("filename\\*?=(?:UTF-8''|\")?([^\";]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(disposition);
    if (match.hasMatch()) {
        const QString parsed = match.captured(1).trimmed();
        if (!parsed.isEmpty()) fileName = QUrl::fromPercentEncoding(parsed.toUtf8());
    }
    fileName.replace(QLatin1Char('\\'), QLatin1Char('/'));
    fileName = QFileInfo(fileName).fileName();
    if (fileName.isEmpty()) fileName = defaultFileName(m_product);

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    QDir().mkpath(dir);
    m_savePath = QDir(dir).filePath(fileName);

    m_file = new QFile(m_savePath, this);
    if (!m_file->open(QIODevice::WriteOnly)
            || m_file->write(payload) != payload.size()) {
        fail(QStringLiteral("无法写入下载文件：%1").arg(m_file->errorString()));
        m_file->remove();
        cleanupReply();
        return;
    }
    m_file->close();
    cleanupReply();

    m_finished = true;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(100);
    m_statusLabel->setText(QStringLiteral(
        "%1 已下载完成，正在打开安装包。\n保存位置：%2")
        .arg(m_appName, m_savePath));
    m_actionButton->setText(QStringLiteral("再次打开安装包"));
    disconnect(m_actionButton, nullptr, this, nullptr);
    connect(m_actionButton, &QPushButton::clicked,
            this, &DesktopDownloader::openDownloadedFile);

    // 打开安装包（macOS 挂载 dmg / Windows 运行 exe）。
    openDownloadedFile();
}

void DesktopDownloader::openDownloadedFile()
{
    if (m_savePath.isEmpty() || !QFileInfo::exists(m_savePath)
            || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_savePath))) {
        m_statusLabel->setStyleSheet(QStringLiteral(
            "font-size: 13px; color: #b54708;"));
        m_statusLabel->setText(QStringLiteral(
            "安装包已保存，但系统未能自动打开。请到「下载」目录手动运行：\n%1")
            .arg(m_savePath));
    }
}

void DesktopDownloader::openOfficialDownloadPage()
{
    if (QDesktopServices::openUrl(QUrl(officialDownloadUrl()))) {
        accept();
        return;
    }
    m_statusLabel->setText(QStringLiteral(
        "无法自动打开浏览器，请手动访问：%1").arg(officialDownloadUrl()));
}

void DesktopDownloader::fail(const QString &message)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #b42318;"));
    m_statusLabel->setText(message);
    m_actionButton->setText(QStringLiteral("打开官网下载页"));
    disconnect(m_actionButton, nullptr, this, nullptr);
    connect(m_actionButton, &QPushButton::clicked,
            this, &DesktopDownloader::openOfficialDownloadPage);
}

void DesktopDownloader::cleanupReply()
{
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}
