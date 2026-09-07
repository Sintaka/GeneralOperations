#pragma once

#include "core/ScriptRegistry.h"

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>

/// 把 ScriptRegistry 暴露给 QML ListView。
///
/// 这是 core 与 QML 之间唯一的桥：core 层不认识 QML，QML 也不直接碰
/// ScriptRegistry。QML 里绝不写死 ListElement —— 数据一律从这里来。
///
/// 模型只出扁平脚本行，分组头不进模型 —— 前端（ScriptOutliner）从 group
/// role 自行推导两级树；前提是 registry 已按分组排好序（见 ScriptRegistry）。
class ScriptListModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        GroupRole,
        DescRole,
        FilePathRole,
        ValidRole,          ///< false: UI 要灰掉且禁止执行
        ErrorTextRole,      ///< errors 拼成一行，给 tooltip
        DestructiveRole,    ///< bool
        DestructiveReasonRole,
        ParamCountRole,
        RequiresTextRole,   ///< 依赖列表拼成一行，可选项带 ? 后缀
        HostRole            ///< "python" / "blender"
    };
    Q_ENUM(Roles)

    explicit ScriptListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /// 扫描目录并重建模型。QML 侧可直接调。
    Q_INVOKABLE void load(const QString &dir);

    /// 扫描期的错误（目录不存在等），QML 里显示在列表底部。
    Q_INVOKABLE QStringList scanErrors() const;

    /// 某个分组里的脚本数。
    ///
    /// Outliner 折叠分组后成员全部不可见，需要显示"这组有几个"，
    /// 否则收起来就完全没有信息了。
    ///
    /// 放在 C++ 而不是 QML 里数：QML 侧拿不到任意行的数据（只有 delegate
    /// 能看见自己那行的 role），要在 QML 里统计就得先开一个"按行取值"的
    /// 通用接口，那等于把模型内部结构漏给视图。这是个纯数据查询，
    /// 留在模型层更合适。
    Q_INVOKABLE int groupCount(const QString &group) const;

    /// 选中脚本的契约快照（拖放区要按 accepts/ext/multi/destructive
    /// 决定接受什么、怎么提示）。找不到返回空字典，QML 以此判断未选中。
    Q_INVOKABLE QVariantMap scriptInfo(const QString &filePath) const;

    /// 拖放校验 —— accepts / @ext / @multi 的唯一裁决处。
    ///
    /// 规则放 C++ 不放 QML：acceptsExtension() 已经在 core 里实现了，
    /// 在 QML 里再写一遍匹配就是两处真相源。urls 是 DropArea 的
    /// drag.urls（file:// URL 列表）。
    ///
    /// 返回 { ok, reason, files, uncertain }：reason 是给用户看的一句话，
    /// ok 时为空；files 是校验通过的本地绝对路径，直接喂 ScriptRunner。
    /// uncertain（ok=true 但 files 为空）表示拖入源用了 OLE 延迟渲染、
    /// 悬停阶段拿不到路径（DOpus 等）—— 悬停时给中性反馈，裁决推迟到
    /// onDropped（那时数据已就绪，QML 侧须检查 files 非空再执行）。
    Q_INVOKABLE QVariantMap validateDrop(const QString &filePath,
                                         const QVariantList &urls) const;

    /// 命令行直跑用：把用户给的一个字符串解析成注册表里的脚本。
    /// 依次尝试 绝对/相对路径 → 文件名 → @name（后两级大小写不敏感）。
    /// 找不到返回空串。
    Q_INVOKABLE QString resolveScript(const QString &query) const;

    /// main.cpp 组装 ScriptRunner 用。返回只读引用，不破坏封装。
    const ScriptRegistry &registry() const { return m_registry; }

signals:
    void countChanged();

private:
    const ScriptManifest *find(const QString &filePath) const;

    ScriptRegistry m_registry;
};

