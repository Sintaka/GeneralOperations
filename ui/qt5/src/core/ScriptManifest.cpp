#include "core/ScriptManifest.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>

namespace {

// 冒号分隔字段两侧空白按 SPEC 要求忽略；字段内容里不会出现冒号，普通 split 够用。
QStringList splitColonFields(const QString &line)
{
    QStringList parts = line.split(QLatin1Char(':'));
    for (QString &p : parts)
        p = p.trimmed();
    return parts;
}

void parseParamLine(const QString &raw, ScriptManifest &m)
{
    const QStringList fields = splitColonFields(raw);
    if (fields.size() < 4) {
        m.errors << QStringLiteral("@param 字段不足4段: %1").arg(raw);
        return;
    }

    ScriptParam p;
    p.name = fields.at(0);
    const QString typeStr = fields.at(1).toLower();
    const QString defaultStr = fields.at(2);
    p.label = fields.at(3);
    // 约束字段如果因为标签本身含冒号被切碎，把剩余部分整体拼回。
    // 第 6 段起允许出现独立的 `presets a|b|c`（int/float 的预设档位，
    // 参数面板渲染成 1K/2K/4K 这类快捷档 + 自定义滑块），它和 min..max
    // 约束共存，不参与拼接。放第 6 段而不是塞进第 5 段，是为了让
    // 旧脚本的第 5 段写法（纯 min..max）原样兼容。
    QString constraint;
    for (int i = 4; i < fields.size(); ++i) {
        const QString f = fields.at(i);
        if (f.startsWith(QLatin1String("presets"))) {
            const QString body = f.mid(7).trimmed();
            if (body.isEmpty()) {
                m.errors << QStringLiteral("@param %1 presets 缺少候选值").arg(p.name);
                continue;
            }
            const QStringList tokens = body.split(QLatin1Char('|'));
            for (const QString &tok : tokens) {
                bool ok = false;
                const double v = tok.trimmed().toDouble(&ok);
                if (!ok) {
                    m.errors << QStringLiteral("@param %1 presets 含非法数值: %2")
                                    .arg(p.name, tok.trimmed());
                    continue;
                }
                // 不在这里校验预设是否落在 min..max 内：range 的解析在后面
                // 的类型 switch 里，此处还没解析出来；且 range 本来就是
                // 纯 UI 约束（argparse 不校验），预设出界是脚本作者的数据问题。
                p.presets.append(v);
            }
        } else {
            if (!constraint.isEmpty())
                constraint += QLatin1Char(':');
            constraint += f;
        }
    }

    if (typeStr == QLatin1String("int")) p.type = ScriptParam::Int;
    else if (typeStr == QLatin1String("float")) p.type = ScriptParam::Float;
    else if (typeStr == QLatin1String("bool")) p.type = ScriptParam::Bool;
    else if (typeStr == QLatin1String("choice")) p.type = ScriptParam::Choice;
    else if (typeStr == QLatin1String("str")) p.type = ScriptParam::Str;
    else if (typeStr == QLatin1String("path")) p.type = ScriptParam::Path;
    else {
        m.errors << QStringLiteral("@param %1 类型未知: %2").arg(p.name, fields.at(1));
        return; // 类型不认识，没法构造合理的 ScriptParam，整条丢弃
    }
    switch (p.type) {
    case ScriptParam::Int: {
        if (!constraint.isEmpty()) {
            const int sep = constraint.indexOf(QLatin1String(".."));
            if (sep < 0) {
                m.errors << QStringLiteral("@param %1 int 约束格式错误: %2").arg(p.name, constraint);
                break;
            }
            bool okMin = false, okMax = false;
            const double lo = constraint.left(sep).trimmed().toInt(&okMin);
            const double hi = constraint.mid(sep + 2).trimmed().toInt(&okMax);
            if (!okMin || !okMax) {
                m.errors << QStringLiteral("@param %1 int 约束数值无法解析: %2").arg(p.name, constraint);
                break;
            }
            p.hasRange = true;
            p.minValue = lo;
            p.maxValue = hi;
        }
        bool ok = false;
        const int v = defaultStr.toInt(&ok);
        if (!ok) {
            m.errors << QStringLiteral("@param %1 默认值不是合法 int: %2").arg(p.name, defaultStr);
            break;
        }
        if (p.hasRange && (v < p.minValue || v > p.maxValue)) {
            m.errors << QStringLiteral("@param %1 默认值 %2 超出范围 %3..%4")
                            .arg(p.name).arg(v).arg(p.minValue).arg(p.maxValue);
            break;
        }
        p.defaultValue = v;
        m.params << p;
        break;
    }
    case ScriptParam::Float: {
        if (!constraint.isEmpty()) {
            const int sep = constraint.indexOf(QLatin1String(".."));
            if (sep < 0) {
                m.errors << QStringLiteral("@param %1 float 约束格式错误: %2").arg(p.name, constraint);
                break;
            }
            bool okMin = false, okMax = false;
            const double lo = constraint.left(sep).trimmed().toDouble(&okMin);
            const double hi = constraint.mid(sep + 2).trimmed().toDouble(&okMax);
            if (!okMin || !okMax) {
                m.errors << QStringLiteral("@param %1 float 约束数值无法解析: %2").arg(p.name, constraint);
                break;
            }
            p.hasRange = true;
            p.minValue = lo;
            p.maxValue = hi;
        }
        bool ok = false;
        const double v = defaultStr.toDouble(&ok);
        if (!ok) {
            m.errors << QStringLiteral("@param %1 默认值不是合法 float: %2").arg(p.name, defaultStr);
            break;
        }
        if (p.hasRange && (v < p.minValue || v > p.maxValue)) {
            m.errors << QStringLiteral("@param %1 默认值 %2 超出范围 %3..%4")
                            .arg(p.name).arg(v).arg(p.minValue).arg(p.maxValue);
            break;
        }
        p.defaultValue = v;
        m.params << p;
        break;
    }
    case ScriptParam::Bool: {
        const QString lower = defaultStr.toLower();
        if (lower == QLatin1String("true")) p.defaultValue = true;
        else if (lower == QLatin1String("false")) p.defaultValue = false;
        else {
            m.errors << QStringLiteral("@param %1 bool 默认值只认 true/false: %2").arg(p.name, defaultStr);
            break;
        }
        m.params << p;
        break;
    }
    case ScriptParam::Choice: {
        if (constraint.isEmpty()) {
            m.errors << QStringLiteral("@param %1 choice 缺少候选约束").arg(p.name);
            break;
        }
        p.choices = constraint.split(QLatin1Char('|'));
        for (QString &c : p.choices)
            c = c.trimmed();
        if (!p.choices.contains(defaultStr)) {
            m.errors << QStringLiteral("@param %1 默认值 %2 不在候选 %3 中")
                            .arg(p.name, defaultStr, p.choices.join(QLatin1Char('|')));
            break;
        }
        p.defaultValue = defaultStr;
        m.params << p;
        break;
    }
    case ScriptParam::Str:
        p.defaultValue = defaultStr;
        m.params << p;
        break;
    case ScriptParam::Path:
        p.defaultValue = defaultStr;
        m.params << p;
        break;
    }
}

} // namespace

QString ScriptParam::cliFlag() const
{
    return QStringLiteral("--") + QString(name).replace(QLatin1Char('_'), QLatin1Char('-'));
}

QStringList ScriptParam::toCliArgs(const QVariant &value) const
{
    if (type == Bool)
        return value.toBool() ? QStringList{cliFlag()} : QStringList{};
    return {cliFlag(), value.toString()};
}

QString ScriptParam::typeName(Type t)
{
    switch (t) {
    case Int:    return QStringLiteral("int");
    case Float:  return QStringLiteral("float");
    case Bool:   return QStringLiteral("bool");
    case Choice: return QStringLiteral("choice");
    case Str:    return QStringLiteral("str");
    case Path:   return QStringLiteral("path");
    }
    return QString(); // 不可达，枚举已穷举全部分支
}

bool ScriptManifest::acceptsExtension(const QString &ext) const
{
    if (extensions.isEmpty())
        return true;
    QString needle = ext.toLower();
    if (!needle.startsWith(QLatin1Char('.')))
        needle.prepend(QLatin1Char('.'));
    return extensions.contains(needle);
}

ScriptManifest ScriptManifest::fromFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ScriptManifest m;
        m.filePath = path;
        m.errors << QStringLiteral("无法打开文件: %1").arg(path);
        return m;
    }
    const QString source = QString::fromUtf8(f.readAll());
    return fromSource(source, path);
}
ScriptManifest ScriptManifest::fromSource(const QString &source, const QString &pathForReport)
{
    ScriptManifest m;
    m.filePath = pathForReport;

    // 统一换行，避免 \r\n 混进内容字段（Windows 上的脚本文件常见）。
    QString normSource = source;
    normSource.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normSource.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    const QStringList allLines = normSource.split(QLatin1Char('\n'));

    // 跳过开头的 shebang / encoding 行（都以 # 开头）和空行，
    // 找到 docstring 语句本身。同时用 charPos 记录该行在原文中的偏移，
    // 这样后面定位结束定界符时可以直接在整段字符串里 indexOf，
    // 不需要再逐行拼接。
    int idx = 0;
    int charPos = 0;
    while (idx < allLines.size()) {
        const QString &lineStr = allLines.at(idx);
        const QString t = lineStr.trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1Char('#'))) {
            charPos += lineStr.length() + 1;
            idx++;
            continue;
        }
        break;
    }

    QString content;

    if (idx < allLines.size()) {
        const QString &candidate = allLines.at(idx);
        int lead = 0;
        while (lead < candidate.size() && candidate.at(lead).isSpace())
            lead++;
        const QString leftTrimmed = candidate.mid(lead);

        // 前缀可选（r/u/rb 等），定界符 """ 或 ''' 都认。
        static const QRegularExpression startRe(QStringLiteral("^[A-Za-z]{0,2}(\"\"\"|''')"));
        const QRegularExpressionMatch sm = startRe.match(leftTrimmed);
        if (sm.hasMatch()) {
            const QString quote = sm.captured(1);
            const int contentStartInCandidate = lead + sm.capturedLength(0);
            const int absoluteContentStart = charPos + contentStartInCandidate;
            const int contentEnd = normSource.indexOf(quote, absoluteContentStart);
            if (contentEnd >= 0) {
                content = normSource.mid(absoluteContentStart, contentEnd - absoluteContentStart);
            } else {
                m.errors << QStringLiteral("模块 docstring 未闭合（缺少结束定界符）");
            }
        }
        // 没匹配到定界符 = 没有模块 docstring，content 留空，
        // 必填键缺失会在下面统一报出来，不用在这里单独报错。
    }

    QSet<QString> seenKeys;
    bool sawAccepts = false;

    // SPEC 规定的匹配式。
    static const QRegularExpression kvRe(QStringLiteral("^@(\\w+)\\s+(.*)$"));
    // 补充：光有 "@key" 没有值也没有分隔空白的行，严格按 SPEC 正则会完全不匹配、
    // 被当成普通说明文字静默忽略 —— 但对 @destructive 这类"空原因"恰恰是要报错
    // 的情形，不能被当作没出现过。所以匹配不到 kvRe 时退一步试这个，值给空字符串。
    static const QRegularExpression bareKeyRe(QStringLiteral("^@(\\w+)$"));
    const QStringList docLines = content.split(QLatin1Char('\n'));
    for (const QString &rawLine : docLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;

        QString key;
        QString value;
        const QRegularExpressionMatch km = kvRe.match(line);
        if (km.hasMatch()) {
            key = km.captured(1);
            value = km.captured(2).trimmed();
        } else {
            const QRegularExpressionMatch bm = bareKeyRe.match(line);
            if (!bm.hasMatch())
                continue; // 普通说明性文字（如 pmx2fbx 里的 "Usage:" 段）不是 @key，静默跳过
            key = bm.captured(1);
            value.clear();
        }

        if (key != QLatin1String("param")) {
            if (seenKeys.contains(key))
                m.warnings << QStringLiteral("重复的 @%1，取最后一次出现的值").arg(key);
            seenKeys.insert(key);
        }

        if (key == QLatin1String("name")) {
            m.name = value;
        } else if (key == QLatin1String("group")) {
            m.group = value;
        } else if (key == QLatin1String("desc")) {
            m.desc = value;
        } else if (key == QLatin1String("accepts")) {
            sawAccepts = true;
            const QString v = value.toLower();
            if (v == QLatin1String("file")) m.accepts = ScriptManifest::File;
            else if (v == QLatin1String("dir")) m.accepts = ScriptManifest::Dir;
            else if (v == QLatin1String("both")) m.accepts = ScriptManifest::Both;
            else m.errors << QStringLiteral("@accepts 值未知: %1").arg(value);
        } else if (key == QLatin1String("ext")) {
            QStringList exts = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (QString &e : exts) {
                e = e.toLower();
                if (!e.startsWith(QLatin1Char('.')))
                    e.prepend(QLatin1Char('.'));
            }
            m.extensions = exts;
        } else if (key == QLatin1String("multi")) {
            const QString v = value.toLower();
            if (v == QLatin1String("true") || v == QLatin1String("1")) m.multi = true;
            else if (v == QLatin1String("false") || v == QLatin1String("0")) m.multi = false;
            else m.warnings << QStringLiteral("@multi 值未知，保留默认值 true: %1").arg(value);
        } else if (key == QLatin1String("requires")) {
            QVector<ScriptRequirement> reqs;
            const QStringList tokens = value.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (const QString &tok : tokens) {
                ScriptRequirement r;
                if (tok.endsWith(QLatin1Char('?'))) {
                    r.optional = true;
                    r.name = tok.left(tok.length() - 1);
                } else {
                    r.optional = false;
                    r.name = tok;
                }
                reqs << r;
            }
            m.requires_ = reqs;
        } else if (key == QLatin1String("destructive")) {
            if (value.isEmpty())
                m.errors << QStringLiteral("@destructive 缺少原因说明");
            else
                m.destructiveReason = value;
        } else if (key == QLatin1String("host")) {
            const QString v = value.toLower();
            if (v == QLatin1String("python")) m.host = ScriptManifest::Python;
            else if (v == QLatin1String("blender")) m.host = ScriptManifest::Blender;
            else m.warnings << QStringLiteral("@host 值未知，保留默认值 python: %1").arg(value);
        } else if (key == QLatin1String("blender")) {
            // 路径可含空格（Windows 默认装在 "Program Files" 下），kvRe 把
            // 行内剩余全部内容当成值，这里原样保留，绝不按空白切分。
            // 是否为空的校验放到运行器：只有真正要启动 blender 的那一刻，
            // "缺路径"才是错误，解析期不拦（避免拖拽列表里多一条灰掉的脚本）。
            m.blenderPath = value;
        } else if (key == QLatin1String("param")) {
            parseParamLine(value, m);
        } else {
            m.warnings << QStringLiteral("未知 @key: @%1").arg(key);
        }
    }

    if (m.name.isEmpty())
        m.errors << QStringLiteral("缺少必填键 @name");
    if (m.group.isEmpty())
        m.errors << QStringLiteral("缺少必填键 @group");
    if (m.desc.isEmpty())
        m.errors << QStringLiteral("缺少必填键 @desc");
    if (!sawAccepts)
        m.errors << QStringLiteral("缺少必填键 @accepts");

    return m;
}

