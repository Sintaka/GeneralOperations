#include "debug/DropDebugLogger.h"

#include <QCoreApplication>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTime>
#include <QUrl>
#include <QWindow>

DropDebugLogger::DropDebugLogger(bool enabled, QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    if (!enabled)
        return;
    m_file.setFileName(QCoreApplication::applicationDirPath()
                       + QStringLiteral("/dropdebug.log"));
    m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    write(QStringLiteral("=== 拖放诊断会话开始 ==="));
}

DropDebugLogger::~DropDebugLogger()
{
    if (m_file.isOpen()) {
        write(QStringLiteral("=== 会话结束 ==="));
        m_file.close();
    }
}

void DropDebugLogger::log(const QString &msg)
{
    write(msg);
}

void DropDebugLogger::attachWindow(QWindow *window)
{
    if (m_file.isOpen() && window)
        window->installEventFilter(this);
}

void DropDebugLogger::write(const QString &line)
{
    if (!m_file.isOpen())
        return;
    m_file.write(QStringLiteral("[%1.%2] %3\n")
                     .arg(m_clock.elapsed() / 1000)
                     .arg(m_clock.elapsed() % 1000, 3, 10, QLatin1Char('0'))
                     .arg(line)
                     .toUtf8());
    m_file.flush();
}

bool DropDebugLogger::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)

    // QDragLeaveEvent 不是 QDragMoveEvent 的子类，单独处理。
    if (event->type() == QEvent::DragLeave) {
        write(QStringLiteral("[平台 DragLeave]"));
        return false;
    }

    const char *name = nullptr;
    switch (event->type()) {
    case QEvent::DragEnter: name = "DragEnter"; break;
    case QEvent::DragMove:  name = "DragMove";  break;
    case QEvent::Drop:      name = "Drop";      break;
    default:                return false;
    }

    const auto *drag = static_cast<const QDragMoveEvent *>(event);
    const QMimeData *mime = drag->mimeData();

    QStringList urls;
    if (mime && mime->hasUrls()) {
        for (const QUrl &u : mime->urls())
            urls.append(u.toString());
    }

    // DragMove 一秒几十条，只记 Drop/Enter；Move 的细节对诊断没用，
    // 真要看轨迹时把这行放开。
    if (event->type() == QEvent::DragMove)
        return false;

    write(QStringLiteral("[平台 %1] pos=%2,%3 hasUrls=%4 urls=[%5] hasText=%6 hasImage=%7 formats=[%8]")
              .arg(QString::fromLatin1(name))
              .arg(drag->pos().x())
              .arg(drag->pos().y())
              .arg(mime ? mime->hasUrls() : false)
              .arg(urls.join(QLatin1String(" | ")))
              .arg(mime ? mime->hasText() : false)
              .arg(mime ? mime->hasImage() : false)
              .arg(mime ? mime->formats().join(QLatin1String(", ")) : QString()));

    if (event->type() == QEvent::Drop && mime) {
        write(QStringLiteral("[平台 Drop 动作] proposedAction=%1 possibleActions=%2 buttons=%3 modifiers=%4")
                  .arg(int(drag->proposedAction()))
                  .arg(int(drag->possibleActions()))
                  .arg(int(drag->mouseButtons()))
                  .arg(int(drag->keyboardModifiers())));
    }
    return false;
}
