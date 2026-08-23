#include "image_generation_dialog.h"

#include "api_client.h"
#include "app_theme.h"
#include "companion_config_projection.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QPixmap>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSet>
#include <QStyle>
#include <QTextEdit>
#include <QUuid>
#include <QVBoxLayout>

namespace {

constexpr int kCredentialHandleRole = Qt::UserRole;
constexpr int kKeyIdentityRole = Qt::UserRole + 1;
constexpr int kAccountIdentityRole = Qt::UserRole + 2;
constexpr int kProjectionSha256Role = Qt::UserRole + 3;
constexpr int kPlatformRole = Qt::UserRole + 5;
constexpr int kGroupLabelRole = Qt::UserRole + 6;

bool isImageGroup(const QString &groupLabel)
{
    return groupLabel.compare(QStringLiteral("gpt-image"), Qt::CaseInsensitive) == 0;
}

} // namespace

ImageGenerationDialog::ImageGenerationDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle(QStringLiteral("GPT Image 生图"));
    setMinimumSize(900, 620);
    resize(1080, 700);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &ImageGenerationDialog::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationFailed,
            this, &ImageGenerationDialog::onCompanionConfigurationFailed);
    connect(m_apiClient, &ApiClient::companionImageGenerated,
            this, &ImageGenerationDialog::onImageGenerated);
    connect(m_apiClient, &ApiClient::companionImageFailed,
            this, &ImageGenerationDialog::onImageGenerationFailed);

    m_statusLabel->setText(QStringLiteral("正在读取生图分组和 API Key..."));
    m_generateButton->setEnabled(false);
    m_apiClient->getApiKeys();
}

void ImageGenerationDialog::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(14);

    auto *header = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("GPT Image"), this);
    title->setStyleSheet(QStringLiteral(
        "font-size: 20px; font-weight: 700; color: #101828;"));
    header->addWidget(title);
    header->addStretch();
    auto *groupBadge = new QLabel(QStringLiteral("Aegisy 生图服务"), this);
    groupBadge->setStyleSheet(QStringLiteral(
        "color: #0f5f59; background: #e7f5f2; border: 1px solid #b7e4da;"
        "border-radius: 7px; padding: 4px 10px; font-size: 11px; font-weight: 600;"));
    header->addWidget(groupBadge);
    root->addLayout(header);

    auto *content = new QHBoxLayout();
    content->setSpacing(16);

    auto *controls = new QFrame(this);
    controls->setFixedWidth(370);
    controls->setStyleSheet(QStringLiteral(
        "QFrame { background: white; border: 1px solid #dfe6ee; border-radius: 8px; }"
        "QLabel { border: none; background: transparent; }"));
    auto *form = new QVBoxLayout(controls);
    form->setContentsMargins(16, 16, 16, 16);
    form->setSpacing(9);

    const QString labelStyle = QStringLiteral(
        "font-size: 12px; font-weight: 600; color: #344054;");
    auto addLabel = [this, form, labelStyle](const QString &text) {
        auto *label = new QLabel(text, this);
        label->setStyleSheet(labelStyle);
        form->addWidget(label);
    };

    addLabel(QStringLiteral("生图分组"));
    m_groupCombo = new QComboBox(this);
    form->addWidget(m_groupCombo);

    addLabel(QStringLiteral("API Key"));
    m_keyCombo = new QComboBox(this);
    form->addWidget(m_keyCombo);

    auto *parameterGrid = new QGridLayout();
    parameterGrid->setHorizontalSpacing(10);
    parameterGrid->setVerticalSpacing(7);

    auto *modelLabel = new QLabel(QStringLiteral("模型"), this);
    modelLabel->setStyleSheet(labelStyle);
    parameterGrid->addWidget(modelLabel, 0, 0);
    auto *sizeLabel = new QLabel(QStringLiteral("尺寸"), this);
    sizeLabel->setStyleSheet(labelStyle);
    parameterGrid->addWidget(sizeLabel, 0, 1);
    m_modelCombo = new QComboBox(this);
    m_modelCombo->addItems({QStringLiteral("gpt-image-2"),
                            QStringLiteral("gpt-image-1.5"),
                            QStringLiteral("gpt-image-1")});
    parameterGrid->addWidget(m_modelCombo, 1, 0);
    m_sizeCombo = new QComboBox(this);
    m_sizeCombo->addItems({QStringLiteral("1024x1024"),
                           QStringLiteral("1536x1024"),
                           QStringLiteral("1024x1536"),
                           QStringLiteral("2048x2048"),
                           QStringLiteral("3840x2160"),
                           QStringLiteral("2160x3840")});
    parameterGrid->addWidget(m_sizeCombo, 1, 1);

    auto *qualityLabel = new QLabel(QStringLiteral("质量"), this);
    qualityLabel->setStyleSheet(labelStyle);
    parameterGrid->addWidget(qualityLabel, 2, 0);
    auto *formatLabel = new QLabel(QStringLiteral("格式"), this);
    formatLabel->setStyleSheet(labelStyle);
    parameterGrid->addWidget(formatLabel, 2, 1);
    m_qualityCombo = new QComboBox(this);
    m_qualityCombo->addItem(QStringLiteral("自动"), QStringLiteral("auto"));
    m_qualityCombo->addItem(QStringLiteral("低"), QStringLiteral("low"));
    m_qualityCombo->addItem(QStringLiteral("中"), QStringLiteral("medium"));
    m_qualityCombo->addItem(QStringLiteral("高"), QStringLiteral("high"));
    parameterGrid->addWidget(m_qualityCombo, 3, 0);
    m_formatCombo = new QComboBox(this);
    m_formatCombo->addItem(QStringLiteral("PNG"), QStringLiteral("png"));
    m_formatCombo->addItem(QStringLiteral("JPEG"), QStringLiteral("jpeg"));
    m_formatCombo->addItem(QStringLiteral("WebP"), QStringLiteral("webp"));
    parameterGrid->addWidget(m_formatCombo, 3, 1);
    form->addLayout(parameterGrid);

    addLabel(QStringLiteral("提示词"));
    m_promptEdit = new QTextEdit(this);
    m_promptEdit->setPlaceholderText(QStringLiteral("描述需要生成的画面..."));
    m_promptEdit->setMinimumHeight(150);
    form->addWidget(m_promptEdit, 1);

    m_generateButton = new QPushButton(QStringLiteral("生成图片"), this);
    m_generateButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_generateButton->setStyleSheet(AppTheme::primaryButtonStyle());
    m_generateButton->setMinimumHeight(40);
    form->addWidget(m_generateButton);
    content->addWidget(controls);

    auto *previewFrame = new QFrame(this);
    previewFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: white; border: 1px solid #dfe6ee; border-radius: 8px; }"
        "QLabel { border: none; background: transparent; }"));
    auto *previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(14, 14, 14, 14);
    previewLayout->setSpacing(10);
    auto *previewTitle = new QLabel(QStringLiteral("预览"), this);
    previewTitle->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 600; color: #344054;"));
    previewLayout->addWidget(previewTitle);

    m_previewLabel = new QLabel(QStringLiteral("生成后的图片会显示在这里"), this);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumSize(420, 420);
    m_previewLabel->setStyleSheet(QStringLiteral(
        "QLabel { background: #f7f9fb; color: #98a2b3; border: 1px dashed #cfd7e3;"
        "border-radius: 8px; font-size: 13px; }"));
    previewLayout->addWidget(m_previewLabel, 1);

    auto *previewActions = new QHBoxLayout();
    previewActions->addStretch();
    m_saveButton = new QPushButton(QStringLiteral("另存图片"), this);
    m_saveButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_saveButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_saveButton->setEnabled(false);
    previewActions->addWidget(m_saveButton);
    previewLayout->addLayout(previewActions);
    content->addWidget(previewFrame, 1);
    root->addLayout(content, 1);

    auto *footer = new QHBoxLayout();
    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    footer->addWidget(m_statusLabel, 1);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(m_groupCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ImageGenerationDialog::onGroupChanged);
    connect(m_generateButton, &QPushButton::clicked,
            this, &ImageGenerationDialog::onGenerateClicked);
    connect(m_saveButton, &QPushButton::clicked,
            this, &ImageGenerationDialog::onSaveClicked);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
}

void ImageGenerationDialog::onCompanionConfigurationReceived(
    const QJsonObject &projection)
{
    if (!CompanionConfigProjection::validate(projection)) {
        onCompanionConfigurationFailed(QStringLiteral("projection-response-invalid"));
        return;
    }
    m_companionProjection = projection;
    populateGroups();
}

void ImageGenerationDialog::onCompanionConfigurationFailed(
    const QString &errorCode)
{
    if (m_companionProjection.isEmpty()) {
        m_groupCombo->clear();
        m_groupCombo->addItem(QStringLiteral("未找到 gpt-image 分组"), QString());
        m_keyCombo->clear();
        m_keyCombo->addItem(QStringLiteral("请先创建该分组的 API Key"), QString());
        m_generateButton->setEnabled(false);
    }
    m_statusLabel->setText(QStringLiteral("账号配置读取失败：%1").arg(errorCode));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
}

void ImageGenerationDialog::populateGroups()
{
    m_groupCombo->blockSignals(true);
    m_groupCombo->clear();

    QSet<QString> groupLabels;
    for (const QJsonValue &value : m_companionProjection.value(
         QStringLiteral("keys")).toArray()) {
        const QJsonObject candidate = value.toObject();
        const QString groupLabel = candidate.value(
            QStringLiteral("group_label")).toString();
        if (!isImageGroup(groupLabel)
                || candidate.value(QStringLiteral("state")).toString()
                    != QStringLiteral("active")
                || candidate.value(QStringLiteral("credential_state")).toString()
                    != QStringLiteral("available-in-secure-storage")) {
            continue;
        }
        const QString normalized = groupLabel.toCaseFolded();
        if (groupLabels.contains(normalized)) continue;
        groupLabels.insert(normalized);
        m_groupCombo->addItem(groupLabel, groupLabel);
    }
    m_groupCombo->blockSignals(false);

    if (m_groupCombo->count() == 0) {
        m_groupCombo->addItem(QStringLiteral("未找到 gpt-image 分组"), QString());
        m_keyCombo->clear();
        m_keyCombo->addItem(QStringLiteral("请先创建该分组的 API Key"), QString());
        m_statusLabel->setText(QStringLiteral("账号中没有可用的 gpt-image 分组 Key。"));
        m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
        m_generateButton->setEnabled(false);
        return;
    }

    populateKeys();
    m_statusLabel->setText(QStringLiteral("已加载生图分组和 API Key。"));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
}

void ImageGenerationDialog::populateKeys()
{
    const QString selectedGroupLabel = m_groupCombo->currentData().toString();
    m_keyCombo->clear();

    for (const QJsonValue &value : m_companionProjection.value(
         QStringLiteral("keys")).toArray()) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("group_label")).toString()
                    .compare(selectedGroupLabel, Qt::CaseInsensitive) != 0
                || candidate.value(QStringLiteral("state")).toString()
                    != QStringLiteral("active")
                || candidate.value(QStringLiteral("credential_state")).toString()
                    != QStringLiteral("available-in-secure-storage")) {
            continue;
        }
        const QString handle = candidate.value(
            QStringLiteral("credential_handle")).toString();
        if (handle.isEmpty()) continue;
        m_keyCombo->addItem(
            candidate.value(QStringLiteral("display_name")).toString(), handle);
        const int row = m_keyCombo->count() - 1;
        m_keyCombo->setItemData(
            row, candidate.value(QStringLiteral("key_identity")), kKeyIdentityRole);
        m_keyCombo->setItemData(
            row, m_companionProjection.value(QStringLiteral("account_identity")),
            kAccountIdentityRole);
        m_keyCombo->setItemData(
            row, m_companionProjection.value(QStringLiteral("projection_sha256")),
            kProjectionSha256Role);
        m_keyCombo->setItemData(
            row, candidate.value(QStringLiteral("platform")), kPlatformRole);
        m_keyCombo->setItemData(
            row, candidate.value(QStringLiteral("group_label")), kGroupLabelRole);
    }

    if (m_keyCombo->count() == 0) {
        m_keyCombo->addItem(QStringLiteral("该分组没有可用 Key"), QString());
    }
    m_generateButton->setEnabled(
        !selectedCredentialHandle().isEmpty() && !m_generating);
}

void ImageGenerationDialog::onGroupChanged(int)
{
    populateKeys();
}

QString ImageGenerationDialog::selectedCredentialHandle() const
{
    return m_keyCombo->currentData(kCredentialHandleRole).toString();
}

QString ImageGenerationDialog::selectedAccountIdentity() const
{
    return m_keyCombo->currentData(kAccountIdentityRole).toString();
}

QString ImageGenerationDialog::selectedKeyIdentity() const
{
    return m_keyCombo->currentData(kKeyIdentityRole).toString();
}

QString ImageGenerationDialog::selectedProjectionSha256() const
{
    return m_keyCombo->currentData(kProjectionSha256Role).toString();
}

QString ImageGenerationDialog::selectedPlatform() const
{
    return m_keyCombo->currentData(kPlatformRole).toString();
}

void ImageGenerationDialog::onGenerateClicked()
{
    const QString prompt = m_promptEdit->toPlainText().trimmed();
    if (selectedCredentialHandle().isEmpty() || selectedKeyIdentity().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少 API Key"),
                             QStringLiteral("请选择生图分组下的可用 API Key。"));
        return;
    }
    if (prompt.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少提示词"),
                             QStringLiteral("请输入需要生成的画面描述。"));
        return;
    }

    setGenerating(true);
    m_statusLabel->setText(QStringLiteral("正在生成图片，高清大图可能需要几分钟..."));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #175cd3;"));
    m_requestId = QStringLiteral("image-dialog-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_apiClient->generateCompanionImage(
        m_requestId,
        selectedAccountIdentity(),
        selectedKeyIdentity(),
        selectedCredentialHandle(),
        selectedProjectionSha256(),
        selectedPlatform(),
        m_modelCombo->currentText(),
        prompt,
        m_sizeCombo->currentText(),
        m_qualityCombo->currentData().toString(),
        m_formatCombo->currentData().toString());
}

void ImageGenerationDialog::setGenerating(bool generating)
{
    m_generating = generating;
    m_groupCombo->setEnabled(!generating);
    m_keyCombo->setEnabled(!generating);
    m_modelCombo->setEnabled(!generating);
    m_sizeCombo->setEnabled(!generating);
    m_qualityCombo->setEnabled(!generating);
    m_formatCombo->setEnabled(!generating);
    m_promptEdit->setEnabled(!generating);
    m_generateButton->setEnabled(
        !generating && !selectedCredentialHandle().isEmpty());
    m_generateButton->setText(generating
        ? QStringLiteral("正在生成...") : QStringLiteral("生成图片"));
}

void ImageGenerationDialog::onImageGenerated(const QString &requestId,
                                              const QByteArray &imageData,
                                              const QString &outputFormat,
                                              const QString &revisedPrompt)
{
    if (requestId != m_requestId) return;
    QImage image;
    if (!image.loadFromData(imageData)) {
        onImageGenerationFailed(
            requestId, QStringLiteral("图片已返回，但客户端无法解析该图片格式。"));
        return;
    }

    m_generatedImage = image;
    m_generatedBytes = imageData;
    m_generatedFormat = outputFormat.toLower();
    m_requestId.clear();
    setGenerating(false);
    m_saveButton->setEnabled(true);
    updatePreview();
    m_statusLabel->setText(revisedPrompt.isEmpty()
        ? QStringLiteral("图片生成完成。")
        : QStringLiteral("图片生成完成，服务已优化提示词。"));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
}

void ImageGenerationDialog::onImageGenerationFailed(
    const QString &requestId, const QString &error)
{
    if (requestId != m_requestId) return;
    m_requestId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("生成失败：%1").arg(error));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
    QMessageBox::warning(this, QStringLiteral("生图失败"), error);
}

void ImageGenerationDialog::updatePreview()
{
    if (m_generatedImage.isNull() || !m_previewLabel) {
        return;
    }
    const QSize target = m_previewLabel->contentsRect().size() - QSize(20, 20);
    m_previewLabel->setPixmap(QPixmap::fromImage(m_generatedImage).scaled(
        target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void ImageGenerationDialog::onSaveClicked()
{
    if (m_generatedBytes.isEmpty()) {
        return;
    }

    QString extension = m_generatedFormat;
    if (extension == QStringLiteral("jpeg")) {
        extension = QStringLiteral("jpg");
    }
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("保存生成图片"),
        QStringLiteral("aegisy-image.%1").arg(extension),
        QStringLiteral("图片 (*.%1)").arg(extension));
    if (path.isEmpty()) {
        return;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(m_generatedBytes) != m_generatedBytes.size()
        || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入所选文件。"));
        return;
    }
    m_statusLabel->setText(QStringLiteral("图片已保存到 %1").arg(QFileInfo(path).fileName()));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
}

void ImageGenerationDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    updatePreview();
}

void ImageGenerationDialog::reject()
{
    if (m_generating) {
        m_apiClient->cancelImageGeneration();
    }
    QDialog::reject();
}
