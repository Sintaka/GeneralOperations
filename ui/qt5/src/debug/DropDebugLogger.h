#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QObject>

class QWindow;

/// 拖放诊断（仅 --dropdebug 启用时打开日志文件）。
///
/// 双通道记录，看的是同一个拖放在两层的样子：
///   1. 平台层：挂在 QQuickWindow 上的事件过滤器，直接看 QEvent::
///      DragEnter/DragMove/Drop 里的 QMimeData —— 这是平台插件从
///      OLE IDataObject 转出来的第一手数据（格式列表、URL），QML 的
///      DropArea 只能看到它的下游。
///   2. QML 层：DropArea 的 onEntered/onDropped 事件参数，由 QML 调
///      log() 写入。
///
/// 日志写 exe 旁的 dropdebug.log，逐行 flush（诊断会话很短，
/// flush 的代价可以忽略），文件在构造时截断、析构时关闭。
class DropDebugLogger : public QObject
{
    Q_OBJECT

public:
    /// enabled=false 时构造出的是个哑实例：log() 是空操作，也不装过滤器，
    /// 但 context property 依然有效 —— QML 侧不用关心模式开关。
    explicit DropDebugLogger(bool enabled, QObject *parent = nullptr);
    ~DropDebugLogger() override;

    /// QML 侧的记录口。
    Q_INVOKABLE void log(const QString &msg);

    /// 在窗口上装平台层事件过滤器。
    void attachWindow(QWindow *window);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void write(const QString &line);

    QFile m_file;
    QElapsedTimer m_clock;
};
