#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

/// 一条 @param 声明。
/// 格式见 docs/SCRIPT_SPEC.md：`@param 名字 : 类型 : 默认值 : 标签 [: 约束]`
struct ScriptParam
{
    enum Type { Int, Float, Bool, Choice, Str, Path };

    QString  name;          ///< 原样的参数名（下划线形式，如 max_pixels）
    Type     type = Int;
    QVariant defaultValue;
    QString  label;         ///< UI 上显示的中文标签

    // 约束。按类型取用，其余为空。
    bool     hasRange = false;
    double   minValue = 0.0;
    double   maxValue = 0.0;
    QStringList choices;    ///< 仅 Choice 类型
    /// int/float 的预设档位（@param 第 6 段 `presets 1024|2048|4096`）。
    /// 存 double 以兼容 float 参数；参数面板渲染成快捷档位 + 自定义滑块。
    QVariantList presets;

    /// "max_pixels" -> "--max-pixels"
    QString cliFlag() const;

    /// 按 SPEC 的命令行映射规则生成参数片段。
    /// Bool 走 store_true 语义：true 给出 {flag}，false 给出空列表。
    /// 切片 4 接参数面板时才会真正用到，这里先随类型定义一起放好。
    QStringList toCliArgs(const QVariant &value) const;

    static QString typeName(Type t);
};

/// 一个 pip 依赖。`@requires Pillow OpenImageIO?` 里的每一项。
struct ScriptRequirement
{
    QString name;
    bool    optional = false;   ///< 带 `?` 后缀：装不上只警告，不禁用脚本
};

/// 从脚本模块 docstring 顶部的声明块解析出的契约。
///
/// 解析策略刻意宽容：未知的 @key 记进 warnings 但不算失败，
/// 这样以后加新键不会让旧启动器崩。只有缺必填键才进 errors。
/// errors 非空的脚本仍然会出现在 Outliner 里（灰掉、不可执行、悬停给原因），
/// 而不是静默消失 —— 静默跳过会让人以为脚本没被发现，白折腾半天。
struct ScriptManifest
{
    enum Accepts { File, Dir, Both };
    enum Host     { Python, Blender, Exe };

    // ---- 必填 ----
    QString name;
    QString group;
    QString desc;
    Accepts accepts = File;

    // ---- 可选 ----
    QStringList extensions;              ///< 小写带点，如 ".exr"。空 = 不限
    bool        multi = true;
    QVector<ScriptRequirement> requires_; ///< 尾下划线：requires 是 C++20 关键字
    QString     destructiveReason;       ///< 非空即为破坏性操作，内容是给用户看的原因
    Host        host = Python;
    /// @host blender 用的主程序路径，来自 @blender 键（可含空格，取行内剩余全部）。
    /// Blender 自带 Python 环境（bpy 等依赖装在它自己的 site-packages），
    /// 不能拿启动器的 pythonCandidates 顶替，所以这个路径是 blender 场景
    /// 唯一的启动入口；为空时运行器明确报错，而不是猜一个安装路径。
    QString     blenderPath;
    /// @host exe 用的后端可执行文件名（不带扩展名，取 @exe 行内剩余全部内容）。
    /// 与 @blender 存完整路径不同，这里只存名字 —— 落到哪个路径由分发
    /// 布局决定（发行包 tools/<名字>/<名字>.exe，开发期 GO_DEV_BACKEND_DIR
    /// 注入的构建目录），启动器按固定顺序定位，找不到就预检失败，不猜。
    QString     exeName;

    QVector<ScriptParam> params;

    // ---- 解析结果 ----
    QString     filePath;
    QStringList errors;
    QStringList warnings;

    bool isValid() const { return errors.isEmpty(); }
    bool isDestructive() const { return !destructiveReason.isEmpty(); }

    /// 扩展名白名单为空时恒为 true。大小写不敏感，带点/不带点都接受。
    bool acceptsExtension(const QString &ext) const;

    /// 读文件并解析。读不到文件也返回一个 manifest，errors 里说明原因。
    static ScriptManifest fromFile(const QString &path);

    /// 解析已经拿到的源码文本。供单元测试直接调用，不必落地临时文件。
    static ScriptManifest fromSource(const QString &source, const QString &pathForReport);
};
