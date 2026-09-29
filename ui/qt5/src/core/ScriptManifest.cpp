#include "core/ScriptManifest.h"

#include <go/manifest/catalog.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

QStringList stringListFromJson(const QJsonValue &value)
{
    QStringList result;
    const QJsonArray array = value.toArray();
    result.reserve(array.size());
    for (const QJsonValue &item : array) {
        if (item.isString())
            result.append(item.toString());
    }
    return result;
}

bool paramTypeFromJson(const QString &kind, ScriptParam::Type *type)
{
    if (kind == QLatin1String("int")) *type = ScriptParam::Int;
    else if (kind == QLatin1String("float")) *type = ScriptParam::Float;
    else if (kind == QLatin1String("bool")) *type = ScriptParam::Bool;
    else if (kind == QLatin1String("choice")) *type = ScriptParam::Choice;
    else if (kind == QLatin1String("str")) *type = ScriptParam::Str;
    else if (kind == QLatin1String("path")) *type = ScriptParam::Path;
    else return false;
    return true;
}

QVariant paramDefaultFromJson(const QJsonValue &value, ScriptParam::Type type)
{
    // QJson represents both integer and floating point numbers as JSON numbers.
    // Preserve the existing Qt model's int defaults for int parameters.
    switch (type) {
    case ScriptParam::Int:
        return value.toInt();
    case ScriptParam::Float:
        return value.toDouble();
    case ScriptParam::Bool:
        return value.toBool();
    case ScriptParam::Choice:
    case ScriptParam::Str:
    case ScriptParam::Path:
        return value.toString();
    }
    return {};
}

ScriptParam scriptParamFromJson(const QJsonObject &object, bool *ok)
{
    ScriptParam param;
    param.name = object.value(QStringLiteral("name")).toString();
    if (!paramTypeFromJson(object.value(QStringLiteral("kind")).toString(), &param.type)) {
        *ok = false;
        return param;
    }

    param.defaultValue = paramDefaultFromJson(
        object.value(QStringLiteral("defaultValue")), param.type);
    param.label = object.value(QStringLiteral("label")).toString();
    param.hasRange = object.value(QStringLiteral("hasRange")).toBool();
    param.minValue = object.value(QStringLiteral("minValue")).toDouble();
    param.maxValue = object.value(QStringLiteral("maxValue")).toDouble();
    param.choices = stringListFromJson(object.value(QStringLiteral("choices")));

    const QJsonArray presets = object.value(QStringLiteral("presets")).toArray();
    param.presets.reserve(presets.size());
    for (const QJsonValue &preset : presets) {
        if (preset.isDouble()) {
            param.presets.append(preset.toDouble());
        } else if (preset.isString()) {
            bool ok = false;
            const double value = preset.toString().toDouble(&ok);
            if (ok)
                param.presets.append(value);
        }
    }

    *ok = true;
    return param;
}

ScriptManifest fromSharedJson(const QByteArray &jsonUtf8, const QString &filePath)
{
    ScriptManifest manifest;
    manifest.filePath = filePath;

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(jsonUtf8, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        manifest.errors << QStringLiteral("共享脚本声明解析器返回无效 JSON: %1")
                               .arg(parseError.errorString());
        return manifest;
    }

    const QJsonObject object = document.object();
    manifest.name = object.value(QStringLiteral("name")).toString();
    manifest.group = object.value(QStringLiteral("group")).toString();
    manifest.desc = object.value(QStringLiteral("description")).toString();

    const QString accepts = object.value(QStringLiteral("accepts")).toString();
    if (accepts == QLatin1String("dir")) manifest.accepts = ScriptManifest::Dir;
    else if (accepts == QLatin1String("both")) manifest.accepts = ScriptManifest::Both;
    else manifest.accepts = ScriptManifest::File;

    manifest.extensions = stringListFromJson(object.value(QStringLiteral("extensions")));
    manifest.multi = object.value(QStringLiteral("multi")).toBool(true);
    for (const QString &requirement :
         stringListFromJson(object.value(QStringLiteral("requires")))) {
        ScriptRequirement parsed;
        parsed.optional = requirement.endsWith(QLatin1Char('?'));
        parsed.name = parsed.optional ? requirement.left(requirement.size() - 1)
                                      : requirement;
        manifest.requires_.append(parsed);
    }

    const QJsonValue destructive = object.value(QStringLiteral("destructive"));
    if (destructive.isString())
        manifest.destructiveReason = destructive.toString();

    const QString host = object.value(QStringLiteral("host")).toString();
    if (host == QLatin1String("blender")) manifest.host = ScriptManifest::Blender;
    else if (host == QLatin1String("exe")) manifest.host = ScriptManifest::Exe;
    else manifest.host = ScriptManifest::Python;

    manifest.blenderPath = object.value(QStringLiteral("blenderPath")).toString();
    manifest.exeName = object.value(QStringLiteral("exe")).toString();

    const QJsonArray params = object.value(QStringLiteral("params")).toArray();
    manifest.params.reserve(params.size());
    for (const QJsonValue &value : params) {
        if (!value.isObject())
            continue;
        bool converted = false;
        const ScriptParam param = scriptParamFromJson(value.toObject(), &converted);
        if (converted)
            manifest.params.append(param);
    }

    manifest.errors = stringListFromJson(object.value(QStringLiteral("errors")));
    manifest.warnings = stringListFromJson(object.value(QStringLiteral("warnings")));
    if (!object.value(QStringLiteral("valid")).toBool(true) && manifest.errors.isEmpty())
        manifest.errors << QStringLiteral("共享脚本声明解析器标记脚本无效");

    return manifest;
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
    return QString();
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
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ScriptManifest manifest;
        manifest.filePath = path;
        manifest.errors << QStringLiteral("无法打开文件: %1").arg(path);
        return manifest;
    }
    return fromSource(QString::fromUtf8(file.readAll()), path);
}

ScriptManifest ScriptManifest::fromSource(const QString &source, const QString &pathForReport)
{
    const QByteArray sourceUtf8 = source.toUtf8();
    const QByteArray idUtf8 = pathForReport.toUtf8();
    const std::string json = go::manifest::parse_source_json(
        std::string_view(sourceUtf8.constData(),
                         static_cast<std::size_t>(sourceUtf8.size())),
        std::string_view(idUtf8.constData(), static_cast<std::size_t>(idUtf8.size())));
    return fromSharedJson(QByteArray::fromStdString(json), pathForReport);
}
