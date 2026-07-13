#include "image_generation_dialog.h"

#include "api_client.h"
#include "app_theme.h"

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
#include <QVBoxLayout>

namespace {

QString jsonId(const QJsonValue &value)
{
    if (value.isNull() || value.isUndefined()) {
        return QString();
    }
    return value.isString()
        ? value.toString()
        : QString::number(value.toVariant().toLongLong());
}

bool isImageGroup(const QJsonObject &group)
{
    const QString name = group.value(QStringLiteral("name")).toString().trimmed();
    return name.compare(QStringLiteral("gpt-image"), Qt::CaseInsensitive) == 0
        || group.value(QStringLiteral("allow_image_generation")).toBool();
}

QString maskedKey(const QString &key)
{
    if (key.size() <= 12) {
        return key;
    }
    return key.left(8) + QStringLiteral("...") + key.right(4);
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

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &ImageGenerationDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &ImageGenerationDialog::onRequestFailed);
    connect(m_apiClient, &ApiClient::imageGenerated,
            this, &ImageGenerationDialog::onImageGenerated);
    connect(m_apiClient, &ApiClient::imageGenerationFailed,
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

void ImageGenerationDialog::onApiKeysReceived(const QJsonArray &keys)
{
    m_allKeys = keys;
    populateGroups();
}

void ImageGenerationDialog::populateGroups()
{
    m_groupCombo->blockSignals(true);
    m_groupCombo->clear();

    QSet<QString> groupIds;
    for (const QJsonValue &value : m_allKeys) {
        const QJsonObject group = value.toObject().value(QStringLiteral("group")).toObject();
        if (!isImageGroup(group)) {
            continue;
        }
        const QString groupId = jsonId(group.value(QStringLiteral("id")));
        if (groupId.isEmpty() || groupIds.contains(groupId)) {
            continue;
        }
        groupIds.insert(groupId);
        m_groupCombo->addItem(group.value(QStringLiteral("name")).toString(), groupId);
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
    const QString selectedGroupId = m_groupCombo->currentData().toString();
    m_keyCombo->clear();

    for (const QJsonValue &value : m_allKeys) {
        const QJsonObject keyObject = value.toObject();
        const QJsonObject group = keyObject.value(QStringLiteral("group")).toObject();
        if (jsonId(group.value(QStringLiteral("id"))) != selectedGroupId) {
            continue;
        }
        const QString key = keyObject.value(QStringLiteral("key")).toString();
        if (key.isEmpty() || keyObject.value(QStringLiteral("status")).toString() != QStringLiteral("active")) {
            continue;
        }
        const QString name = keyObject.value(QStringLiteral("name")).toString().trimmed();
        m_keyCombo->addItem(name.isEmpty()
            ? maskedKey(key)
            : QStringLiteral("%1 (%2)").arg(name, maskedKey(key)), key);
    }

    if (m_keyCombo->count() == 0) {
        m_keyCombo->addItem(QStringLiteral("该分组没有可用 Key"), QString());
    }
    m_generateButton->setEnabled(!selectedApiKey().isEmpty() && !m_generating);
}

void ImageGenerationDialog::onGroupChanged(int)
{
    populateKeys();
}

QString ImageGenerationDialog::selectedApiKey() const
{
    return m_keyCombo->currentData().toString();
}

void ImageGenerationDialog::onGenerateClicked()
{
    const QString key = selectedApiKey();
    const QString prompt = m_promptEdit->toPlainText().trimmed();
    if (key.isEmpty()) {
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
    m_apiClient->generateImage(
        key,
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
    m_generateButton->setEnabled(!generating && !selectedApiKey().isEmpty());
    m_generateButton->setText(generating
        ? QStringLiteral("正在生成...") : QStringLiteral("生成图片"));
}

void ImageGenerationDialog::onImageGenerated(const QByteArray &imageData,
                                              const QString &outputFormat,
                                              const QString &revisedPrompt)
{
    QImage image;
    if (!image.loadFromData(imageData)) {
        onImageGenerationFailed(QStringLiteral("图片已返回，但客户端无法解析该图片格式。"));
        return;
    }

    m_generatedImage = image;
    m_generatedBytes = imageData;
    m_generatedFormat = outputFormat.toLower();
    setGenerating(false);
    m_saveButton->setEnabled(true);
    updatePreview();
    m_statusLabel->setText(revisedPrompt.isEmpty()
        ? QStringLiteral("图片生成完成。")
        : QStringLiteral("图片生成完成，服务已优化提示词。"));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
}

void ImageGenerationDialog::onImageGenerationFailed(const QString &error)
{
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("生成失败：%1").arg(error));
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
    QMessageBox::warning(this, QStringLiteral("生图失败"), error);
}

void ImageGenerationDialog::onRequestFailed(const QString &error)
{
    if (m_allKeys.isEmpty() && !m_generating) {
        m_statusLabel->setText(QStringLiteral("读取 API Key 失败：%1").arg(error));
        m_statusLabel->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
    }
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
