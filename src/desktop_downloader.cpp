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
        fail(QStringLiteral("下载失败：%1").arg(m_reply->errorString()));
        cleanupReply();
        return;
    }

    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        fail(QStringLiteral("服务器返回错误（HTTP %1），请稍后重试或联系管理员。")
                 .arg(status));
        cleanupReply();
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

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    QDir().mkpath(dir);
    m_savePath = QDir(dir).filePath(fileName);

    m_file = new QFile(m_savePath, this);
    if (!m_file->open(QIODevice::WriteOnly)
            || m_file->write(m_reply->readAll()) < 0) {
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
        "%1 已下载完成，正在打开安装包。\n如果没有自动弹出，请到「下载」目录手动运行：\n%2")
        .arg(m_appName, m_savePath));
    m_actionButton->setText(QStringLiteral("完成"));
    disconnect(m_actionButton, nullptr, this, nullptr);
    connect(m_actionButton, &QPushButton::clicked, this, &QDialog::accept);

    // 打开安装包（macOS 挂载 dmg / Windows 运行 exe）。
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_savePath));
}

void DesktopDownloader::fail(const QString &message)
{
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 13px; color: #b42318;"));
    m_statusLabel->setText(message);
    m_actionButton->setText(QStringLiteral("关闭"));
    disconnect(m_actionButton, nullptr, this, nullptr);
    connect(m_actionButton, &QPushButton::clicked, this, &QDialog::reject);
}

void DesktopDownloader::cleanupReply()
{
    if (m_reply) {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}
