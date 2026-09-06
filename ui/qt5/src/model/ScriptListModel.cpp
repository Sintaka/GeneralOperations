#include "model/ScriptListModel.h"

#include <QFileInfo>
#include <QUrl>

// core 层拼好数据，这层只做只读投影：QAbstractListModel 需要的 row/role
// 查表，以及把 ScriptManifest 的字段名翻成 QML 侧的小驼峰角色名。
// 不在这里做任何解析或排序 —— 那是 ScriptRegistry 的职责，重复一遍
// 就是两处真相源，迟早会不一致。

namespace {

/// 把拖进来的一个元素归一成本地路径，失败返回空串。
///
/// 【为什么不能只走 QUrl::isLocalFile】实测各来源给的形态五花八门：
///   - 资源管理器：file:///D:/x.jpg（标准三斜杠，QUrl 认）
///   - Directory Opus：疑似 file://D:/x.jpg（盘符落 authority 位）或裸路径
///   - 裸路径 D:/x.jpg 或 D:\x.jpg：QUrl 会把 "D" 解析成 scheme！
///
/// 还有一个实测踩到的坑：QUrl("file://D:/x") 可能给出 isLocalFile()==true
/// 但路径是 "//D:/x" 的组合 —— 存在性为假。所以这里不信任任何单一来源：
/// 每个分支出一个候选，**统一用存在性仲裁**，第一个真实存在的候选胜出，
/// 全灭才算失败。归一化 surgery 全部在原始字符串上做，不依赖 QUrl 的
/// 解析细节。
QString toLocalPath(const QVariant &v)
{
    if (!v.isValid())
        return {};

    QStringList candidates;

    const QUrl url = v.canConvert<QUrl>() ? v.toUrl() : QUrl(v.toString());
    if (url.isLocalFile())
        candidates.append(url.toLocalFile());

    const QString s = v.toString();
    if (s.isEmpty())
        return {};

    if (!s.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
        // 裸路径（无 scheme，盘符或反斜杠开头）：QUrl 会把 "D" 当 scheme，
        // 所以必须用原始字符串。
        candidates.append(s);
    } else {
        // file: 系。剥掉 scheme 和随后所有斜杠（二/三斜杠统一），
        // percent 解码，再按三种形态出候选。
        QString rest = s.mid(5);
        while (rest.startsWith(QLatin1Char('/')))
            rest.remove(0, 1);
        rest = QUrl::fromPercentEncoding(rest.toUtf8());

        candidates.append(rest);   // "D:/x.jpg" 或 "D:\x.jpg"

        if (rest.size() >= 3 && rest[0].isLetter() && rest[1] == QLatin1Char('/')) {
            // 盘符冒号被 QUrl 当 host 吞掉的形态："d/tmp/x.jpg" → "d:/tmp/x.jpg"。
            candidates.append(QString(rest[0]) + QLatin1Char(':') + rest.mid(1));
        }
        if (!rest.isEmpty() && rest[0] != QLatin1Char('/')) {
            // UNC：file://server/share/x → \\server\share\x。
            QString unc = QStringLiteral("\\\\") + rest;
            unc.replace(QLatin1Char('/'), QLatin1Char('\\'));
            candidates.append(unc);
        }
    }

    for (const QString &c : candidates) {
        if (!c.isEmpty() && QFileInfo::exists(c))
            return c;
    }
    return {};
}

} // namespace

ScriptListModel::ScriptListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ScriptListModel::rowCount(const QModelIndex &parent) const
{
    // 列表模型没有子项，非顶层 parent 一律 0 行，这是 Qt 的标准约定。
    if (parent.isValid())
        return 0;
    return m_registry.scripts().size();
}

QVariant ScriptListModel::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    if (!index.isValid() || row < 0 || row >= m_registry.scripts().size())
        return QVariant();

    const ScriptManifest &m = m_registry.scripts().at(row);

    switch (role) {
    case NameRole:
        return m.name;
    case GroupRole:
        return m.group;
    case DescRole:
        return m.desc;
    case FilePathRole:
        return m.filePath;
    case ValidRole:
        return m.isValid();
    case ErrorTextRole:
        // "; " 拼接：给 tooltip/副标题用的一行摘要，不是给程序解析的结构化数据。
        return m.errors.join(QStringLiteral("; "));
    case DestructiveRole:
        return m.isDestructive();
    case DestructiveReasonRole:
        return m.destructiveReason;
    case ParamCountRole:
        return m.params.size();
    case RequiresTextRole: {
        // 可选依赖带 "?" 后缀，跟 @requires 声明里的写法保持一致，
        // 这样用户在 UI 上看到的和脚本头部注释里写的对得上号。
        QStringList parts;
        parts.reserve(m.requires_.size());
        for (const ScriptRequirement &req : m.requires_) {
            parts.append(req.optional ? req.name + QStringLiteral("?") : req.name);
        }
        return parts.join(QStringLiteral(", "));
    }
    case HostRole:
        return m.host == ScriptManifest::Blender
            ? QStringLiteral("blender")
            : QStringLiteral("python");
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> ScriptListModel::roleNames() const
{
    return {
        { NameRole,              "name" },
        { GroupRole,             "group" },
        { DescRole,              "desc" },
        { FilePathRole,          "filePath" },
        { ValidRole,             "valid" },
        { ErrorTextRole,         "errorText" },
        { DestructiveRole,       "destructive" },
        { DestructiveReasonRole, "destructiveReason" },
        { ParamCountRole,        "paramCount" },
        { RequiresTextRole,      "requiresText" },
        { HostRole,              "host" },
    };
}

void ScriptListModel::load(const QString &dir)
{
    // beginResetModel/endResetModel 夹住整次扫描：scan() 会整体重建
    // m_scripts，而不是增量增删，所以精确的 begin/endInsertRows 没有
    // 意义，直接告诉视图"数据全变了，重新拉一遍"最简单也最不容易出错。
    beginResetModel();
    m_registry.scan(dir);
    endResetModel();

    emit countChanged();
}

QStringList ScriptListModel::scanErrors() const
{
    return m_registry.scanErrors();
}

int ScriptListModel::groupCount(const QString &group) const
{
    int n = 0;
    for (const ScriptManifest &m : m_registry.scripts()) {
        if (m.group == group)
            ++n;
    }
    return n;
}

const ScriptManifest *ScriptListModel::find(const QString &filePath) const
{
    for (const ScriptManifest &m : m_registry.scripts()) {
        if (m.filePath == filePath)
            return &m;
    }
    return nullptr;
}

QString ScriptListModel::resolveScript(const QString &query) const
{
    if (query.isEmpty())
        return {};

    const QFileInfo qfi(query);
    const QString qAbs = qfi.absoluteFilePath();
    const QString qName = qfi.fileName();

    for (const ScriptManifest &m : m_registry.scripts()) {
        if (QFileInfo(m.filePath).absoluteFilePath() == qAbs)
            return m.filePath;
    }
    for (const ScriptManifest &m : m_registry.scripts()) {
        if (QFileInfo(m.filePath).fileName().compare(qName, Qt::CaseInsensitive) == 0)
            return m.filePath;
    }
    for (const ScriptManifest &m : m_registry.scripts()) {
        if (m.name.compare(query, Qt::CaseInsensitive) == 0)
            return m.filePath;
    }
    return {};
}

QVariantMap ScriptListModel::scriptInfo(const QString &filePath) const
{
    QVariantMap info;
    const ScriptManifest *m = find(filePath);
    if (!m)
        return info;

    info.insert(QStringLiteral("name"), m->name);
    info.insert(QStringLiteral("accepts"),
                m->accepts == ScriptManifest::File ? QStringLiteral("file")
              : m->accepts == ScriptManifest::Dir  ? QStringLiteral("dir")
                                                   : QStringLiteral("both"));
    QVariantList extensions;
    for (const QString &ext : m->extensions)
        extensions.append(ext);
    info.insert(QStringLiteral("extensions"), extensions);
    info.insert(QStringLiteral("multi"), m->multi);
    info.insert(QStringLiteral("destructive"), m->isDestructive());
    info.insert(QStringLiteral("destructiveReason"), m->destructiveReason);
    info.insert(QStringLiteral("host"),
                m->host == ScriptManifest::Blender ? QStringLiteral("blender")
                                                   : QStringLiteral("python"));

    // 参数契约快照：ParamEditor 按它渲染控件。type 用 typeName() 字符串
    // 传递（QML 不认识 C++ 枚举），min/max 只在有范围时给 —— QML 用
    // contains() 判断而不是用 0 当哨兵，0 是合法的 min。
    QVariantList params;
    for (const ScriptParam &p : m->params) {
        QVariantMap pm;
        pm.insert(QStringLiteral("name"), p.name);
        pm.insert(QStringLiteral("type"), ScriptParam::typeName(p.type));
        pm.insert(QStringLiteral("label"), p.label);
        pm.insert(QStringLiteral("default"), p.defaultValue);
        if (p.hasRange) {
            pm.insert(QStringLiteral("min"), p.minValue);
            pm.insert(QStringLiteral("max"), p.maxValue);
        }
        if (!p.choices.isEmpty()) {
            QVariantList choices;
            for (const QString &c : p.choices)
                choices.append(c);
            pm.insert(QStringLiteral("choices"), choices);
        }
        if (!p.presets.isEmpty())
            pm.insert(QStringLiteral("presets"), p.presets);
        params.append(pm);
    }
    info.insert(QStringLiteral("params"), params);
    return info;
}

QVariantMap ScriptListModel::validateDrop(const QString &filePath,
                                          const QVariantList &urls) const
{
    QVariantMap result;
    QStringList files;
    QString reason;

    const ScriptManifest *m = find(filePath);
    if (!m) {
        reason = QStringLiteral("脚本不存在");
    } else if (!m->isValid()) {
        // 理论上到不了这里：无效脚本在 Outliner 里就不给选中。
        // 留作防御，别让崩溃面扩大。
        reason = m->errors.join(QStringLiteral("; "));
    } else {
        for (const QVariant &v : urls) {
            // 拖进来的元素可能是 QUrl 也可能是字符串，统一交给归一化。
            const QString path = toLocalPath(v);
            if (!path.isEmpty())
                files.append(path);
        }

        const bool uncertain = files.isEmpty() && !urls.isEmpty();
        if (uncertain) {
            // OLE 延迟渲染（Directory Opus 等来源）：DragEnter/DragOver 阶段
            // 数据对象只支持 QueryGetData（格式广告），GetData 取 CF_HDROP
            // 要等松手才成功 —— 微软对 IDropTarget::DragEnter 的建议就是
            // 只查格式、不取数据。此时无法区分"数据还没渲染"和"真不是
            // 文件"，不判死刑：ok 给 true 并标 uncertain（files 为空），
            // UI 给中性反馈，最终裁决在 onDropped（那时数据已就绪）。
            //
            // 拿得到真实路径的失败（扩展名/文件夹/单目标/不存在）在上面
            // 的循环和后面的检查里已经设了 reason，走确定拒绝。
            result.insert(QStringLiteral("uncertain"), true);
        } else if (files.isEmpty()) {
            reason = QStringLiteral("没有拖入任何文件");
        }

        for (const QString &path : files) {
            if (!reason.isEmpty())
                break;
            const QFileInfo fi(path);
            if (m->accepts == ScriptManifest::File && fi.isDir()) {
                reason = fi.fileName() + QStringLiteral(" 是文件夹，此脚本只接受文件");
            } else if (m->accepts == ScriptManifest::Dir && !fi.isDir()) {
                reason = fi.fileName() + QStringLiteral(" 不是文件夹，此脚本只接受文件夹");
            } else if (m->accepts != ScriptManifest::Dir && !fi.exists()) {
                // 目录上面 isDir 已经查过存在性，这里只补文件一侧。
                reason = fi.fileName() + QStringLiteral(" 不存在");
            } else if (m->accepts != ScriptManifest::Dir
                       && !m->acceptsExtension(fi.suffix())) {
                reason = fi.fileName() + QStringLiteral(" 的扩展名不受支持（")
                         + m->extensions.join(QStringLiteral(" "))
                         + QStringLiteral("）");
            }
        }

        if (reason.isEmpty() && !m->multi && files.size() > 1) {
            // 多文件拖进来但脚本只收一个：整体拒绝而不是悄悄截断，
            // 用户应该知道有文件被丢掉了。
            reason = QStringLiteral("此脚本一次只接受一个目标");
        }
    }

    if (!reason.isEmpty())
        files.clear();

    result.insert(QStringLiteral("ok"), reason.isEmpty());
    result.insert(QStringLiteral("reason"), reason);
    result.insert(QStringLiteral("files"), files);
    return result;
}
