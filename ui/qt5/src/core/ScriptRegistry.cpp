#include "core/ScriptRegistry.h"

#include <QDir>

void ScriptRegistry::scan(const QString &dir)
{
    m_scripts.clear();
    m_scanErrors.clear();

    QDir d(dir);
    if (!d.exists()) {
        m_scanErrors << QStringLiteral("目录不存在: %1").arg(dir);
        return;
    }

    // 按名字排序枚举，否则文件系统给的顺序在不同机器/不同次运行之间不保证一致，
    // Outliner 每次启动脚本排列就会跳来跳去。
    d.setSorting(QDir::Name);
    d.setFilter(QDir::Files);
    const QStringList files = d.entryList(QStringList{QStringLiteral("*.py")});

    if (files.isEmpty()) {
        m_scanErrors << QStringLiteral("目录下没有 .py 文件: %1").arg(dir);
        return;
    }

    // 先按文件名序解析出全部脚本（含解析失败的，UI 要灰掉显示而不是丢弃）。
    QVector<ScriptManifest> flat;
    flat.reserve(files.size());
    for (const QString &fileName : files) {
        const QString path = d.filePath(fileName);
        flat << ScriptManifest::fromFile(path);
    }

    // 再按 group 重排：同组聚在一起，组内保持刚才的文件名序。
    // QML ListView 的 section.property 要求同一个 section 的条目连续排列，
    // 这一步就是在满足这个前提；分组顺序取首次出现顺序，不额外排序
    // （交给 groups() 和这里共用同一套"首次出现"语义，两者才不会互相矛盾）。
    QStringList groupOrder;
    for (const ScriptManifest &m : flat) {
        if (!groupOrder.contains(m.group))
            groupOrder << m.group;
    }

    m_scripts.reserve(flat.size());
    for (const QString &g : groupOrder) {
        for (const ScriptManifest &m : flat) {
            if (m.group == g)
                m_scripts << m;
        }
    }
}

QStringList ScriptRegistry::groups() const
{
    QStringList result;
    for (const ScriptManifest &m : m_scripts) {
        if (!result.contains(m.group))
            result << m.group;
    }
    return result;
}
