#include "help_dialog.h"

#include "app_theme.h"

#include <QDesktopServices>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

namespace {

struct HelpSection {
    QString title;
    QString markdown;
};

QVector<HelpSection> loadGuideSections()
{
    QFile file(QStringLiteral(":/docs/user-guide.md"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {{
            QStringLiteral("文档不可用"),
            QStringLiteral("# 文档不可用\n\n无法读取内置使用指南，请重新安装应用。")
        }};
    }

    const QString guide = QString::fromUtf8(file.readAll());
    QVector<HelpSection> sections;
    QString title = QStringLiteral("概览");
    QString body;
    const QStringList lines = guide.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QStringLiteral("## "))) {
            if (!body.trimmed().isEmpty()) {
                sections.append({title, body});
            }
            title = line.mid(3).trimmed();
            body = QStringLiteral("# %1\n").arg(title);
        } else {
            body += line + QLatin1Char('\n');
        }
    }
    if (!body.trimmed().isEmpty()) {
        sections.append({title, body});
    }
    return sections;
}

QStyle::StandardPixmap sectionIcon(int index)
{
    static const QStyle::StandardPixmap icons[] = {
        QStyle::SP_ComputerIcon,
        QStyle::SP_MediaPlay,
        QStyle::SP_FileDialogDetailedView,
        QStyle::SP_MessageBoxInformation,
        QStyle::SP_DirOpenIcon,
        QStyle::SP_BrowserReload,
        QStyle::SP_DesktopIcon,
        QStyle::SP_DriveNetIcon,
        QStyle::SP_FileDialogListView,
        QStyle::SP_DialogSaveButton,
        QStyle::SP_MessageBoxWarning,
        QStyle::SP_DialogApplyButton,
        QStyle::SP_MessageBoxQuestion,
    };
    return icons[index % (sizeof(icons) / sizeof(icons[0]))];
}

} // namespace

HelpDialog::HelpDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Aegisy 使用指南"));
    resize(940, 680);
    setMinimumSize(760, 540);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    const QVector<HelpSection> sections = loadGuideSections();

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sidebar = new QWidget(this);
    sidebar->setFixedWidth(210);
    sidebar->setStyleSheet(QStringLiteral(
        "QWidget { background: #f8fafc; border-right: 1px solid #e4e7ec; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(10, 14, 10, 12);
    sideLayout->setSpacing(8);

    auto *docHeader = new QHBoxLayout;
    docHeader->setContentsMargins(8, 0, 8, 0);
    auto *docIcon = new QLabel(sidebar);
    docIcon->setPixmap(style()->standardIcon(QStyle::SP_FileDialogInfoView).pixmap(18, 18));
    docHeader->addWidget(docIcon);
    auto *docTitle = new QLabel(QStringLiteral("使用指南"), sidebar);
    docTitle->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: #344054; border: none;"));
    docHeader->addWidget(docTitle);
    docHeader->addStretch();
    sideLayout->addLayout(docHeader);

    auto *navList = new QListWidget(sidebar);
    navList->setIconSize(QSize(16, 16));
    navList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { min-height: 34px; padding: 0 8px; border-radius: 6px;"
        " color: #475467; font-size: 12px; }"
        "QListWidget::item:selected { background: #e7f5f2; color: #0f5f59;"
        " font-weight: 600; }"
        "QListWidget::item:hover:!selected { background: #eef2f6; color: #344054; }"));
    for (int index = 0; index < sections.size(); ++index) {
        auto *item = new QListWidgetItem(
            style()->standardIcon(sectionIcon(index)), sections[index].title, navList);
        item->setToolTip(sections[index].title);
    }
    sideLayout->addWidget(navList, 1);
    root->addWidget(sidebar);

    auto *content = new QWidget(this);
    content->setStyleSheet(QStringLiteral("QWidget { background: #ffffff; }"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    auto *stack = new QStackedWidget(content);
    for (const HelpSection &section : sections) {
        auto *browser = new QTextBrowser(stack);
        browser->setOpenExternalLinks(true);
        browser->setFrameShape(QFrame::NoFrame);
        browser->setStyleSheet(QStringLiteral(
            "QTextBrowser { background: #ffffff; border: none; padding: 22px 30px;"
            " font-size: 13px; }"));
        browser->document()->setDefaultStyleSheet(QStringLiteral(
            "body { color: #17212b; font-family: system-ui; line-height: 1.6; margin: 0; }"
            "h1 { color: #101828; font-size: 20px; font-weight: 700; margin: 0 0 12px 0; }"
            "h2 { color: #101828; font-size: 16px; font-weight: 700; margin: 18px 0 8px 0; }"
            "h3 { color: #344054; font-size: 14px; font-weight: 700; margin: 14px 0 6px 0; }"
            "p { margin: 6px 0 10px 0; color: #344054; }"
            "code, pre { font-family: 'Consolas','Menlo',monospace; font-size: 12px;"
            " background: #f2f4f7; border: 1px solid #e4e7ec; border-radius: 4px;"
            " padding: 2px 6px; }"
            "pre { padding: 10px 14px; display: block; margin: 8px 0; }"
            "table { border-collapse: collapse; width: 100%; margin: 10px 0; }"
            "th { background: #f7f9fb; color: #667085; font-size: 12px; font-weight: 600;"
            " padding: 8px 10px; border-bottom: 1px solid #e4e7ec; text-align: left; }"
            "td { padding: 8px 10px; color: #344054; border-bottom: 1px solid #f0f2f5; }"
            "ul, ol { margin: 6px 0 10px 0; padding-left: 20px; color: #344054; }"
            "li { margin: 4px 0; }"
            "a { color: #0f766e; }"
            "strong { color: #101828; font-weight: 600; }"));
        browser->setMarkdown(section.markdown);
        stack->addWidget(browser);
    }
    contentLayout->addWidget(stack, 1);

    auto *footer = new QWidget(content);
    footer->setFixedHeight(54);
    footer->setStyleSheet(QStringLiteral(
        "QWidget { background: #f8fafc; border-top: 1px solid #e4e7ec; }"));
    auto *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 0, 18, 0);
    auto *officialLink = new QPushButton(QStringLiteral("打开官网"), footer);
    officialLink->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
    officialLink->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(officialLink, &QPushButton::clicked, []() {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://aegisy.cc")));
    });
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), footer);
    closeButton->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    closeButton->setStyleSheet(AppTheme::primaryButtonStyle());
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    footerLayout->addWidget(officialLink);
    footerLayout->addStretch();
    footerLayout->addWidget(closeButton);
    contentLayout->addWidget(footer);

    root->addWidget(content, 1);
    navList->setCurrentRow(0);
    connect(navList, &QListWidget::currentRowChanged,
            stack, &QStackedWidget::setCurrentIndex);
}
