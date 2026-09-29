#pragma once

#include "core/ScriptManifest.h"

#include <QString>
#include <QStringList>
#include <QVector>

/// 递归扫描一个目录树下所有 *.py，逐个解析成 ScriptManifest。
///
/// 纯 C++，不继承 QObject —— core 层不认识 UI，也不需要信号槽。
/// 要暴露给 QML 由 model/ScriptListModel 负责包装。
class ScriptRegistry
{
public:
    /// 递归扫描（含全部子目录）。重复调用会先清空。
    /// 目录不存在或树下一个 .py 都没有都不算异常，记进 scanErrors() 让 UI 显示出来。
    void scan(const QString &dir);

    /// 全部脚本，含解析失败的（UI 要把它们灰掉显示，而不是丢弃）。
    /// 按 group 聚在一起，同组内按相对路径排序 —— QML ListView 的
    /// section.property 要求数据已按分组排好，且顺序必须稳定，
    /// 否则每次启动 Outliner 的排列都不一样。
    const QVector<ScriptManifest> &scripts() const { return m_scripts; }

    /// 分组名，按首次出现顺序（不排序：Image/Geometry/System 的自然顺序
    /// 比字母序更符合直觉）。
    QStringList groups() const;

    const QStringList &scanErrors() const { return m_scanErrors; }

private:
    QVector<ScriptManifest> m_scripts;
    QStringList             m_scanErrors;
};
