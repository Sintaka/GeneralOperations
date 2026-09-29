#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

/// 运行时布局持久化：exe 同目录的 layout.json。
///
/// 记两类状态，都是"用户上次离开时的样子"：
///   expanded —— Outliner 的大类/子分类折叠表（key 语义与
///               ScriptOutliner.expandedGroups 一致：大类=首段、子分类=完整
///               group 串）；
///   params   —— 每个脚本上一次调整后的参数表。
/// 每次变化都写入内存并防抖落盘；文件只在启动时读一次，运行中不回读。
///
/// 【脚本的 key 不用绝对路径】开发形态跑 build/ 里的裸 exe 时脚本在
/// core/python/scripts/，发行形态在 <包>/scripts/ —— 同一个脚本两处的
/// 绝对路径不同，拿绝对路径做 key 换个形态记录就全丢。这里统一折算成
/// 相对 scripts 根的路径（如 "Image/ReSize/img.Resize_jpg.py"），
/// 与脚本自身的分组约定（@group = 相对路径）同一个坐标系。
class LayoutStore : public QObject
{
    Q_OBJECT

public:
    explicit LayoutStore(const QString &scriptsDir, QObject *parent = nullptr);

    Q_INVOKABLE QVariantMap expandedGroups() const;
    Q_INVOKABLE void saveExpandedGroups(const QVariantMap &groups);

    /// 找不到记录时返回空字典，QML 以此判断没有要恢复的东西。
    Q_INVOKABLE QVariantMap scriptParams(const QString &scriptPath) const;
    Q_INVOKABLE void saveScriptParams(const QString &scriptPath,
                                      const QVariantMap &values);

private:
    QString keyFor(const QString &scriptPath) const;
    void scheduleWrite();

    QString   m_filePath;
    QString   m_scriptsDir;
    QJsonObject m_expanded;
    QJsonObject m_params;
    QTimer    m_writeTimer;
};
