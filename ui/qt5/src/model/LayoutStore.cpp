#include "model/LayoutStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QVariantMap>

namespace {

/// 防抖窗口：滑块拖动时 onValuesChanged 每帧都来，直接落盘就是每帧写文件。
/// 400ms 内的连续变化合并成最后一次写。
constexpr int kWriteDelayMs = 400;

QVariantMap toVariantMap(const QJsonObject &obj)
{
    QVariantMap out;
    for (auto it = obj.begin(); it != obj.end(); ++it)
        out.insert(it.key(), it.value().toVariant());
    return out;
}

} // namespace

LayoutStore::LayoutStore(const QString &scriptsDir, QObject *parent)
    : QObject(parent)
    , m_scriptsDir(scriptsDir)
{
    m_filePath = QCoreApplication::applicationDirPath()
                 + QStringLiteral("/layout.json");

    m_writeTimer.setSingleShot(true);
    m_writeTimer.setInterval(kWriteDelayMs);
    connect(&m_writeTimer, &QTimer::timeout, this, [this] {
        QJsonObject root;
        root.insert(QStringLiteral("version"), 1);
        root.insert(QStringLiteral("expanded"), m_expanded);
        root.insert(QStringLiteral("params"), m_params);
        QFile file(m_filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    });

    // 读不到（首次运行、损坏、被删）就当空表启动，下次保存自然重建。
    QFile file(m_filePath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    m_expanded = root.value(QStringLiteral("expanded")).toObject();
    m_params = root.value(QStringLiteral("params")).toObject();
}

QVariantMap LayoutStore::expandedGroups() const
{
    return toVariantMap(m_expanded);
}

void LayoutStore::saveExpandedGroups(const QVariantMap &groups)
{
    m_expanded = QJsonObject::fromVariantMap(groups);
    scheduleWrite();
}

QVariantMap LayoutStore::scriptParams(const QString &scriptPath) const
{
    const QJsonObject entry = m_params.value(keyFor(scriptPath)).toObject();
    return toVariantMap(entry);
}

void LayoutStore::saveScriptParams(const QString &scriptPath,
                                   const QVariantMap &values)
{
    m_params.insert(keyFor(scriptPath), QJsonObject::fromVariantMap(values));
    scheduleWrite();
}

QString LayoutStore::keyFor(const QString &scriptPath) const
{
    // 绝对路径落在 scripts 根之下 → 相对路径；否则（异常路径）原样兜底，
    // 至少同一形态内还能命中。
    const QDir scriptsDir(m_scriptsDir);
    const QString rel = scriptsDir.relativeFilePath(scriptPath);
    return rel.startsWith(QStringLiteral("../")) ? scriptPath : QDir::fromNativeSeparators(rel);
}

void LayoutStore::scheduleWrite()
{
    m_writeTimer.start();
}
