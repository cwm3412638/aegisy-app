#include "runtime_status_bar.h"

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) qCritical() << message;
    return condition;
}

int opaquePixelCount(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 32) ++count;
        }
    }
    return count;
}

bool containsColor(const QImage &image, const QColor &color)
{
    const QRgb expected = color.rgb();
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixel(x, y) == expected) return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    RuntimeStatusBar bar;

    RuntimeStatusSnapshot live;
    live.toolName = QStringLiteral("Codex CLI");
    live.model = QStringLiteral("gpt-5.6-sol");
    live.reasoning = QStringLiteral("high");
    live.inputTokens = 84000;
    live.outputTokens = 1200;
    live.totalTokens = 85200;
    live.contextLimit = 272000;
    live.balance = 998993.65;
    live.balanceKnown = true;
    live.monitored = true;
    live.active = true;
    live.provenance = RuntimeStatusProvenance::Gateway;
    bar.setSnapshot(live);
    bar.show();
    application.processEvents();

    const QImage image = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const int pixels = image.width() * image.height();
    if (!expect(image.size() == QSize(660, 62), "status bar dimensions changed")
            || !expect(opaquePixelCount(image) > pixels * 8 / 10,
                       "status bar rendered mostly transparent")
            || !expect(bar.accessibleName().contains(QStringLiteral("gpt-5.6-sol")),
                       "model missing from accessible status")
            || !expect(bar.accessibleName().contains(QStringLiteral("high")),
                       "reasoning missing from accessible status")
            || !expect(bar.accessibleName().contains(QStringLiteral("84.0K / 272.0K")),
                       "context usage missing from accessible status")) {
        return 1;
    }

    bool restoreRequested = false;
    QObject::connect(&bar, &RuntimeStatusBar::restoreRequested,
                     [&restoreRequested]() { restoreRequested = true; });
    const QPointF localPosition(12, 12);
    const QPointF globalPosition = bar.mapToGlobal(localPosition.toPoint());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QMouseEvent pressEvent(QEvent::MouseButtonPress, localPosition, globalPosition,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, localPosition, globalPosition,
                             Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#else
    QMouseEvent pressEvent(QEvent::MouseButtonPress, localPosition, localPosition,
                           globalPosition, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, localPosition, localPosition,
                             globalPosition, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
#endif
    QApplication::sendEvent(&bar, &pressEvent);
    QApplication::sendEvent(&bar, &releaseEvent);
    if (!expect(restoreRequested, "clicking the status bar did not request restore")) {
        return 1;
    }

    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--snapshot")) {
        if (!image.save(QString::fromLocal8Bit(argv[2]))) {
            qCritical() << "failed to save status bar snapshot";
            return 1;
        }
    }

    live.active = false;
    live.inputTokens = 210000;
    bar.setSnapshot(live);
    application.processEvents();
    if (!expect(containsColor(bar.grab().toImage().convertToFormat(QImage::Format_ARGB32),
                              QColor(QStringLiteral("#f59e0b"))),
                "warning context color was not rendered")) {
        return 1;
    }

    live.inputTokens = 250000;
    bar.setSnapshot(live);
    application.processEvents();
    if (!expect(containsColor(bar.grab().toImage().convertToFormat(QImage::Format_ARGB32),
                              QColor(QStringLiteral("#ef4444"))),
                "critical context color was not rendered")) {
        return 1;
    }

    RuntimeStatusSnapshot direct;
    direct.toolName = QStringLiteral("Claude Code");
    direct.model = QStringLiteral("claude-opus-4-8");
    direct.balance = 0.62;
    direct.balanceKnown = true;
    direct.provenance = RuntimeStatusProvenance::Configured;
    bar.setSnapshot(direct);
    if (!expect(bar.accessibleName().contains(QStringLiteral("上下文 未监控")),
                "direct mode must not fabricate context usage")) {
        return 1;
    }

    direct.monitored = true;
    direct.requestObserved = true;
    direct.provenance = RuntimeStatusProvenance::Gateway;
    bar.setSnapshot(direct);
    if (!expect(bar.accessibleName().contains(QStringLiteral("上下文 未返回用量")),
                "usage-free requests must remain explicit")) {
        return 1;
    }

    return 0;
}
