#include "desktop_downloader.h"
#include "app_theme.h"
#include "tool_manager.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr qint64 kMaximumInstallerBytes = 2LL * 1024 * 1024 * 1024;
constexpr int kSignaturePrefixBytes = 64;
constexpr int kSignatureSuffixBytes = 512;

QString megabytes(qint64 bytes)
{
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1);
}

} // namespace

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
    setMinimumWidth(480);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(16);

    auto *stateRow = new QHBoxLayout;
    stateRow->setSpacing(12);
    m_stateIcon = new QLabel(this);
    m_stateIcon->setFixedSize(34, 34);
    m_stateIcon->setAlignment(Qt::AlignCenter);
    stateRow->addWidget(m_stateIcon, 0, Qt::AlignTop);

    auto *stateText = new QVBoxLayout;
    stateText->setSpacing(5);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    m_titleLabel = new QLabel(this);
    m_titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 15px; font-weight: 700; color: #101828;"));
    titleRow->addWidget(m_titleLabel);
    m_sourceBadge = new QLabel(this);
    m_sourceBadge->setAlignment(Qt::AlignCenter);
    titleRow->addWidget(m_sourceBadge);
    titleRow->addStretch();
    stateText->addLayout(titleRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    stateText->addWidget(m_statusLabel);
    stateRow->addLayout(stateText, 1);
    root->addLayout(stateRow);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(8);
    root->addWidget(m_progressBar);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    m_secondaryButton = new QPushButton(this);
    m_secondaryButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_secondaryButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_secondaryButton->hide();
    buttonRow->addWidget(m_secondaryButton);
    m_actionButton = new QPushButton(this);
    m_actionButton->setStyleSheet(AppTheme::primaryButtonStyle());
    buttonRow->addWidget(m_actionButton);
    root->addLayout(buttonRow);

    QTimer::singleShot(0, this, &DesktopDownloader::startDownload);
}

bool DesktopDownloader::platformSupported()
{
    return !platformSlug().isEmpty();
}

bool DesktopDownloader::productSupported(Product product)
{
    if (!platformSupported()) return false;
#if defined(Q_OS_MAC)
    // OpenAI currently requires Apple Silicon for the macOS desktop app.
    return product != Product::ChatGpt
        || platformSlug() == QStringLiteral("mac-arm64");
#elif defined(Q_OS_WIN)
    Q_UNUSED(product);
    return true;
#else
    Q_UNUSED(product);
    return true;
#endif
}

std::optional<DesktopDownloader::Product>
DesktopDownloader::proxiedProductForTool(AiTool tool)
{
    Product product;
    switch (tool) {
    case AiTool::ClaudeCode:
        product = Product::Claude;
        break;
    case AiTool::CodexCli:
        product = Product::ChatGpt;
        break;
    case AiTool::GeminiCli:
    case AiTool::OpenCode:
        return std::nullopt;
    default:
        return std::nullopt;
    }
    return productSupported(product) ? std::optional<Product>(product) : std::nullopt;
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
    return arch.contains(QStringLiteral("arm"))
        ? QStringLiteral("mac-arm64") : QStringLiteral("mac-x64");
#elif defined(Q_OS_WIN)
    return QStringLiteral("win-x64");
#else
    return QString();
#endif
}

QString DesktopDownloader::requestPath(Product product)
{
    if (!productSupported(product)) return QString();
    const QString platform = platformSlug();
    return QStringLiteral("/api/v1/desktop-downloads/%1/%2")
        .arg(productSlug(product), platform);
}

QString DesktopDownloader::defaultFileName(Product product)
{
    const QString base = product == Product::Claude
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

bool DesktopDownloader::validateInstallerParts(const QString &contentType,
                                               const QString &installerFormat,
                                               const QByteArray &firstBytes,
                                               const QByteArray &lastBytes,
                                               qint64 size,
                                               QString *error)
{
    const QByteArray trimmed = firstBytes.trimmed().toLower();
    const QString type = contentType.toLower();
    const QString format = installerFormat.trimmed().toLower();
    if (size <= 0
            || type.contains(QStringLiteral("text/html"))
            || type.contains(QStringLiteral("application/json"))
            || trimmed.startsWith("<!doctype html")
            || trimmed.startsWith("<html")
            || trimmed.startsWith("{")) {
        if (error) {
            *error = QStringLiteral("下载服务返回了网页或错误信息，而不是安装包。");
        }
        return false;
    }
#if defined(Q_OS_WIN)
    const bool valid = (format == QStringLiteral("exe") && firstBytes.startsWith("MZ"))
        || (format == QStringLiteral("msix")
            && firstBytes.startsWith(QByteArrayLiteral("PK\x03\x04")));
    if (!valid) {
        if (error) *error = QStringLiteral("下载内容不是有效的 Windows 安装程序。");
        return false;
    }
#elif defined(Q_OS_MAC)
    const bool validPkg = format == QStringLiteral("pkg")
        && firstBytes.startsWith(QByteArrayLiteral("xar!"));
    const bool validDmg = format == QStringLiteral("dmg")
        && size >= kSignatureSuffixBytes
        && lastBytes.size() >= kSignatureSuffixBytes
        && lastBytes.left(4) == QByteArrayLiteral("koly");
    if (!validPkg && !validDmg) {
        if (error) *error = QStringLiteral("下载内容不是有效的 macOS 安装包。");
        return false;
    }
#endif
    return true;
}

QString DesktopDownloader::responseFileName() const
{
    const QString format = m_reply
        ? QString::fromUtf8(m_reply->rawHeader("X-Aegisy-Installer-Format"))
              .trimmed().toLower()
        : QString();
    QString fileName = defaultFileName(m_product);
#if defined(Q_OS_MAC)
    if (format == QStringLiteral("pkg")) {
        fileName = m_product == Product::Claude
            ? QStringLiteral("Claude.pkg") : QStringLiteral("ChatGPT.pkg");
    }
#elif defined(Q_OS_WIN)
    if (format == QStringLiteral("msix")) {
        fileName = m_product == Product::Claude
            ? QStringLiteral("Claude.msix") : QStringLiteral("ChatGPT.msix");
    }
#endif
    if (!m_reply) return fileName;

    const QString disposition = QString::fromUtf8(
        m_reply->rawHeader("Content-Disposition"));
    static const QRegularExpression expression(
        QStringLiteral("filename\\*?=(?:UTF-8''|\")?([^\";]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(disposition);
    if (match.hasMatch()) {
        const QString parsed = QUrl::fromPercentEncoding(
            match.captured(1).trimmed().toUtf8());
        if (!parsed.isEmpty()) fileName = parsed;
    }

    fileName.replace(QLatin1Char('\\'), QLatin1Char('/'));
    fileName = QFileInfo(fileName).fileName();
#if defined(Q_OS_MAC)
    const bool validSuffix = format == QStringLiteral("pkg")
        ? fileName.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive)
        : format == QStringLiteral("dmg")
            && fileName.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive);
    if (!validSuffix) {
        fileName = format == QStringLiteral("pkg")
            ? QStringLiteral("Claude.pkg") : defaultFileName(m_product);
    }
#elif defined(Q_OS_WIN)
    const bool validSuffix = format == QStringLiteral("msix")
        ? fileName.endsWith(QStringLiteral(".msix"), Qt::CaseInsensitive)
        : format == QStringLiteral("exe")
            && fileName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
    if (!validSuffix) {
        fileName = format == QStringLiteral("msix")
            ? QStringLiteral("Claude.msix") : defaultFileName(m_product);
    }
#endif
    return fileName.isEmpty() ? defaultFileName(m_product) : fileName;
}

QString DesktopDownloader::uniqueSavePath(const QString &directory,
                                          const QString &fileName) const
{
    const QFileInfo info(fileName);
    const QString base = info.completeBaseName();
    const QString suffix = info.completeSuffix();
    QString candidate = QDir(directory).filePath(fileName);
    for (int number = 1; QFileInfo::exists(candidate); ++number) {
        const QString numbered = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(number)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(number).arg(suffix);
        candidate = QDir(directory).filePath(numbered);
    }
    return candidate;
}

bool DesktopDownloader::prepareTemporaryFile(QString *error)
{
    QString directory = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    if (directory.isEmpty()) {
        directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        if (error) *error = QStringLiteral("无法创建下载目录。");
        return false;
    }

    m_file = new QTemporaryFile(
        QDir(directory).filePath(QStringLiteral(".aegisy-XXXXXX.part")), this);
    m_file->setAutoRemove(true);
    if (!m_file->open()) {
        if (error) {
            *error = QStringLiteral("无法创建临时下载文件：%1").arg(m_file->errorString());
        }
        cleanupTemporaryFile();
        return false;
    }
    return true;
}

bool DesktopDownloader::finalizeTemporaryFile(QString *error)
{
    if (!m_file || !m_reply) {
        if (error) *error = QStringLiteral("下载文件状态无效。");
        return false;
    }

    const QString contentType = m_reply->header(
        QNetworkRequest::ContentTypeHeader).toString();
    const QString installerFormat = QString::fromUtf8(
        m_reply->rawHeader("X-Aegisy-Installer-Format"));
    if (!validateInstallerParts(contentType, installerFormat,
                                m_firstBytes, m_lastBytes,
                                m_receivedBytes, error)) {
        return false;
    }
    if (!m_file->flush()) {
        if (error) *error = QStringLiteral("无法写入下载文件：%1").arg(m_file->errorString());
        return false;
    }
    m_file->close();

    const QString directory = QFileInfo(m_file->fileName()).absolutePath();
    m_savePath = uniqueSavePath(directory, responseFileName());
    m_file->setAutoRemove(false);
    if (!m_file->rename(m_savePath)) {
        if (error) *error = QStringLiteral("无法保存安装包：%1").arg(m_file->errorString());
        m_file->setAutoRemove(true);
        m_savePath.clear();
        return false;
    }
    m_file->deleteLater();
    m_file = nullptr;
    return true;
}

void DesktopDownloader::setVisualState(const QString &title,
                                       const QString &detail,
                                       const QString &badge,
                                       const QString &badgeStyle,
                                       QStyle::StandardPixmap icon)
{
    m_stateIcon->setPixmap(style()->standardIcon(icon).pixmap(26, 26));
    m_titleLabel->setText(title);
    m_statusLabel->setText(detail);
    m_sourceBadge->setText(badge);
    m_sourceBadge->setStyleSheet(badgeStyle);
}

void DesktopDownloader::configureActions(
    const QString &primaryText,
    void (DesktopDownloader::*primarySlot)(),
    const QString &secondaryText,
    void (DesktopDownloader::*secondarySlot)())
{
    disconnect(m_actionButton, nullptr, this, nullptr);
    m_actionButton->setText(primaryText);
    m_actionButton->setIcon(primarySlot == &DesktopDownloader::startDownload
        ? style()->standardIcon(QStyle::SP_BrowserReload)
        : (primarySlot == &DesktopDownloader::openDownloadedFile
            ? style()->standardIcon(QStyle::SP_DialogOpenButton)
            : style()->standardIcon(QStyle::SP_DialogCancelButton)));
    connect(m_actionButton, &QPushButton::clicked, this, primarySlot);

    disconnect(m_secondaryButton, nullptr, this, nullptr);
    if (secondaryText.isEmpty() || secondarySlot == nullptr) {
        m_secondaryButton->hide();
    } else {
        m_secondaryButton->setText(secondaryText);
        m_secondaryButton->setIcon(secondarySlot == &DesktopDownloader::openDownloadFolder
            ? style()->standardIcon(QStyle::SP_DirOpenIcon)
            : style()->standardIcon(QStyle::SP_BrowserReload));
        connect(m_secondaryButton, &QPushButton::clicked, this, secondarySlot);
        m_secondaryButton->show();
    }
}

void DesktopDownloader::startDownload()
{
    cleanupReply();
    cleanupTemporaryFile();
    m_savePath.clear();
    m_firstBytes.clear();
    m_lastBytes.clear();
    m_streamError.clear();
    m_receivedBytes = 0;
    m_finished = false;

    const QString path = requestPath(m_product);
    if (path.isEmpty()) {
        fail(QStringLiteral("当前系统暂无可用的桌面安装包。"));
        return;
    }
    if (m_authToken.trimmed().isEmpty()) {
        fail(QStringLiteral("登录状态不可用，请重新登录后再试。"));
        return;
    }
    QString fileError;
    if (!prepareTemporaryFile(&fileError)) {
        fail(fileError);
        return;
    }

    QString base = m_baseUrl;
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    const QUrl url(base + path);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")) {
        fail(QStringLiteral("下载服务地址无效。"));
        return;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         QStringLiteral("Bearer %1").arg(m_authToken).toUtf8());
    request.setRawHeader("Accept", "application/octet-stream");
    request.setRawHeader("X-Aegisy-Client", "desktop");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(60000);
#endif

    setVisualState(QStringLiteral("正在下载 %1").arg(m_appName),
                   QStringLiteral("正在连接 Aegisy 下载节点..."),
                   QStringLiteral("等待代理"), AppTheme::neutralBadgeStyle(),
                   QStyle::SP_BrowserReload);
    m_progressBar->setRange(0, 0);
    configureActions(QStringLiteral("取消"), &DesktopDownloader::cancelDownload);

    m_reply = m_network->get(request);
    connect(m_reply, &QIODevice::readyRead,
            this, &DesktopDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &DesktopDownloader::onProgress);
    connect(m_reply, &QNetworkReply::finished,
            this, &DesktopDownloader::onFinished);
}

void DesktopDownloader::onReadyRead()
{
    if (!m_reply || !m_file || !m_streamError.isEmpty()) return;

    while (m_reply->bytesAvailable() > 0) {
        const qint64 chunkSize = qMin<qint64>(64 * 1024, m_reply->bytesAvailable());
        const QByteArray chunk = m_reply->read(chunkSize);
        if (chunk.isEmpty()) break;

        if (m_receivedBytes + chunk.size() > kMaximumInstallerBytes) {
            m_streamError = QStringLiteral("安装包超过 2 GB 安全限制，下载已停止。");
            m_reply->abort();
            return;
        }
        if (m_firstBytes.size() < kSignaturePrefixBytes) {
            m_firstBytes.append(chunk.left(kSignaturePrefixBytes - m_firstBytes.size()));
        }
        m_lastBytes.append(chunk);
        if (m_lastBytes.size() > kSignatureSuffixBytes) {
            m_lastBytes = m_lastBytes.right(kSignatureSuffixBytes);
        }
        if (m_file->write(chunk) != chunk.size()) {
            m_streamError = QStringLiteral("无法写入下载文件：%1").arg(m_file->errorString());
            m_reply->abort();
            return;
        }
        m_receivedBytes += chunk.size();
    }
}

void DesktopDownloader::onProgress(qint64 received, qint64 total)
{
    if (total > kMaximumInstallerBytes) {
        m_streamError = QStringLiteral("安装包超过 2 GB 安全限制，下载已停止。");
        if (m_reply) m_reply->abort();
        return;
    }
    if (total > 0) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(static_cast<int>(received * 100 / total));
        m_statusLabel->setText(QStringLiteral("%1 MB / %2 MB")
            .arg(megabytes(received), megabytes(total)));
    } else {
        m_progressBar->setRange(0, 0);
        m_statusLabel->setText(QStringLiteral("已接收 %1 MB").arg(megabytes(received)));
    }
}

void DesktopDownloader::onFinished()
{
    if (!m_reply) return;
    onReadyRead();

    const int status = m_reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString networkError = m_reply->errorString();
    if (!m_streamError.isEmpty()) {
        const QString message = m_streamError;
        cleanupReply();
        fail(message);
        return;
    }
    if (status >= 300 && status < 400) {
        cleanupReply();
        fail(QStringLiteral("下载服务尝试重定向到其他节点，已按代理安全策略拒绝。"));
        return;
    }
    if (m_reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        const QString message = status == 401
            ? QStringLiteral("登录状态已过期，请重新登录后再试。")
            : (status > 0
                ? QStringLiteral("下载服务返回 HTTP %1：%2").arg(status).arg(networkError)
                : QStringLiteral("无法连接下载服务：%1").arg(networkError));
        cleanupReply();
        fail(message);
        return;
    }

    const QString proxyMode = QString::fromUtf8(
        m_reply->rawHeader("X-Aegisy-Download-Mode")).trimmed().toLower();
    if (proxyMode != QStringLiteral("proxy")) {
        cleanupReply();
        fail(QStringLiteral("响应未通过 Aegisy 代理校验，已停止保存安装包。"));
        return;
    }

    QString validationError;
    if (!finalizeTemporaryFile(&validationError)) {
        cleanupReply();
        fail(validationError);
        return;
    }
    cleanupReply();

    m_finished = true;
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(100);
    setVisualState(QStringLiteral("%1 下载完成").arg(m_appName),
                   QStringLiteral("安装包已保存到：%1").arg(m_savePath),
                   QStringLiteral("代理已验证"), AppTheme::successBadgeStyle(),
                   QStyle::SP_DialogApplyButton);
    configureActions(QStringLiteral("打开安装包"),
                     &DesktopDownloader::openDownloadedFile,
                     QStringLiteral("打开目录"),
                     &DesktopDownloader::openDownloadFolder);
}

void DesktopDownloader::cancelDownload()
{
    if (m_reply) m_reply->abort();
    cleanupReply();
    cleanupTemporaryFile();
    reject();
}

void DesktopDownloader::openDownloadedFile()
{
    if (m_savePath.isEmpty() || !QFileInfo::exists(m_savePath)
            || !QDesktopServices::openUrl(QUrl::fromLocalFile(m_savePath))) {
        setVisualState(QStringLiteral("安装包已保存"),
                       QStringLiteral("系统未能自动打开安装包，请从下载目录手动运行。"),
                       QStringLiteral("需要手动打开"), AppTheme::warningBadgeStyle(),
                       QStyle::SP_MessageBoxWarning);
    }
}

void DesktopDownloader::openDownloadFolder()
{
    if (!m_savePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(m_savePath).absolutePath()));
    }
}

void DesktopDownloader::openOfficialDownloadPage()
{
    if (!QDesktopServices::openUrl(QUrl(officialDownloadUrl()))) {
        setVisualState(QStringLiteral("无法打开浏览器"),
                       officialDownloadUrl(),
                       QStringLiteral("手动访问"), AppTheme::warningBadgeStyle(),
                       QStyle::SP_MessageBoxWarning);
    }
}

void DesktopDownloader::fail(const QString &message)
{
    cleanupTemporaryFile();
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    setVisualState(QStringLiteral("下载未完成"), message,
                   QStringLiteral("可重试"), AppTheme::warningBadgeStyle(),
                   QStyle::SP_MessageBoxCritical);
    configureActions(QStringLiteral("重试"), &DesktopDownloader::startDownload,
                     QStringLiteral("打开官网"),
                     &DesktopDownloader::openOfficialDownloadPage);
}

void DesktopDownloader::cleanupReply()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        if (m_reply->isRunning()) m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void DesktopDownloader::cleanupTemporaryFile()
{
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file->deleteLater();
        m_file = nullptr;
    }
}
