#include "core/ScriptRegistry.h"

#include <QDir>
#include <QDirIterator>

#include <algorithm>

void ScriptRegistry::scan(const QString &dir)
{
    m_scripts.clear();
    m_scanErrors.clear();

    QDir d(dir);
    if (!d.exists()) {
        m_scanErrors << QStringLiteral("目录不存在: %1").arg(dir);
        return;
    }

    // 递归枚举 scripts 树下所有 .py（脚本按分组收进子目录，@group 与相对
    // 路径一致）。收集相对路径后整体排序保证确定性：QDirIterator 的遍历
    // 顺序依赖文件系统，直接用会在不同机器/不同次运行之间不一致，Outliner
    // 每次启动脚本排列就会跳来跳去。排序键用相对路径（QDir 统一给正斜杠），
    // 同目录脚本自然聚在一起，目录之间顺序也确定。
    QStringList relPaths;
    QDirIterator it(dir, QStringList{QStringLiteral("*.py")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
        relPaths << d.relativeFilePath(it.next());
    std::sort(relPaths.begin(), relPaths.end());

    if (relPaths.isEmpty()) {
        m_scanErrors << QStringLiteral("目录下没有 .py 文件: %1").arg(dir);
        return;
    }

    // 先按相对路径序解析出全部脚本（含解析失败的，UI 要灰掉显示而不是丢弃）。
    QVector<ScriptManifest> flat;
    flat.reserve(relPaths.size());
    for (const QString &relPath : relPaths) {
        const QString path = d.filePath(relPath);
        flat << ScriptManifest::fromFile(path);
    }

    // 再按 group 重排：同组聚在一起，组内保持刚才的相对路径序。
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
