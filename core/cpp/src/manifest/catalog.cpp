#include "go/manifest/catalog.hpp"

#include "json.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace go::manifest {
namespace {

using Json = nlohmann::json;

bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && is_space(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && is_space(value.back()))
        value.remove_suffix(1);
    return value;
}

std::string lower_ascii(std::string_view value)
{
    std::string result(value);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return result;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::vector<std::string_view> split(std::string_view value, char delimiter)
{
    std::vector<std::string_view> pieces;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = value.find(delimiter, begin);
        pieces.push_back(trim(value.substr(begin, end == std::string_view::npos
                                                    ? value.size() - begin
                                                    : end - begin)));
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return pieces;
}

std::vector<std::string> split_whitespace(std::string_view value)
{
    std::vector<std::string> tokens;
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() && is_space(value[cursor]))
            ++cursor;
        const std::size_t begin = cursor;
        while (cursor < value.size() && !is_space(value[cursor]))
            ++cursor;
        if (begin != cursor)
            tokens.emplace_back(value.substr(begin, cursor - begin));
    }
    return tokens;
}

std::string normalize_newlines(std::string_view source)
{
    std::string normalized;
    normalized.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\r') {
            if (i + 1 < source.size() && source[i + 1] == '\n')
                ++i;
            normalized.push_back('\n');
        } else {
            normalized.push_back(source[i]);
        }
    }
    return normalized;
}

std::optional<std::string_view> module_docstring(const std::string& source)
{
    std::size_t line_begin = 0;
    if (starts_with(source, "\xEF\xBB\xBF"))
        line_begin = 3; // Python accepts a UTF-8 BOM before the first source line.

    while (line_begin <= source.size()) {
        const std::size_t line_end = source.find('\n', line_begin);
        const std::size_t actual_end = line_end == std::string::npos ? source.size() : line_end;
        const std::string_view line(source.data() + line_begin, actual_end - line_begin);
        const std::string_view left_trimmed = trim(line);

        if (left_trimmed.empty() || left_trimmed.front() == '#') {
            if (line_end == std::string::npos)
                break;
            line_begin = line_end + 1;
            continue;
        }

        std::size_t opening = line_begin;
        while (opening < actual_end && is_space(source[opening]))
            ++opening;

        static constexpr std::string_view triple_double = "\"\"\"";
        static constexpr std::string_view triple_single = "'''";
        std::string_view quote;
        std::size_t quote_pos = opening;
        if (source.compare(opening, triple_double.size(), triple_double) == 0) {
            quote = triple_double;
        } else if (source.compare(opening, triple_single.size(), triple_single) == 0) {
            quote = triple_single;
        } else {
            // A declaration may use Python's r/u/rb string prefixes. Restrict
            // the prefix to the supported alphabet and at most two characters
            // so code such as `value = """...` can never look like a module docstring.
            std::size_t prefix_end = opening;
            while (prefix_end < actual_end &&
                   ((source[prefix_end] >= 'a' && source[prefix_end] <= 'z') ||
                    (source[prefix_end] >= 'A' && source[prefix_end] <= 'Z')))
                ++prefix_end;
            const std::size_t prefix_size = prefix_end - opening;
            const std::string prefix = lower_ascii(
                std::string_view(source.data() + opening, prefix_size));
            const bool supported_prefix = prefix == "r" || prefix == "u" ||
                                          prefix == "rb" || prefix == "br";
            if (prefix_size == 0 || prefix_size > 2 || !supported_prefix)
                return std::nullopt;
            quote_pos = prefix_end;
            if (source.compare(quote_pos, triple_double.size(), triple_double) == 0) {
                quote = triple_double;
            } else if (source.compare(quote_pos, triple_single.size(), triple_single) == 0) {
                quote = triple_single;
            } else {
                return std::nullopt;
            }
        }

        const std::size_t content_begin = quote_pos + quote.size();
        std::size_t search = content_begin;
        while (search <= source.size()) {
            const std::size_t end = source.find(quote, search);
            if (end == std::string::npos)
                return std::nullopt;
            std::size_t slash_count = 0;
            for (std::size_t i = end; i > 0 && source[i - 1] == '\\'; --i)
                ++slash_count;
            if (slash_count % 2 == 0)
                return std::string_view(source.data() + content_begin, end - content_begin);
            search = end + quote.size();
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool parse_integer(std::string_view text, int& value)
{
    text = trim(text);
    if (text.empty())
        return false;
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(owned.c_str(), &end, 10);
    if (errno == ERANGE || end != owned.c_str() + owned.size() ||
        parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parse_double(std::string_view text, double& value)
{
    text = trim(text);
    if (text.empty())
        return false;
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(owned.c_str(), &end);
    if (errno == ERANGE || end != owned.c_str() + owned.size() || !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

std::string number_text(double value)
{
    std::ostringstream out;
    out.precision(15);
    out << value;
    return out.str();
}

std::optional<Json> parse_param(std::string_view raw, std::vector<std::string>& errors)
{
    const std::vector<std::string_view> fields = split(raw, ':');
    if (fields.size() < 4) {
        errors.emplace_back("@param 字段不足4段: " + std::string(raw));
        return std::nullopt;
    }

    const std::string name(fields[0]);
    const std::string type = lower_ascii(fields[1]);
    const std::string default_text(fields[2]);
    const std::string label(fields[3]);

    std::string constraint;
    std::vector<std::string> presets;
    for (std::size_t i = 4; i < fields.size(); ++i) {
        const std::string_view field = fields[i];
        if (starts_with(field, "presets")) {
            const std::string_view body = trim(field.substr(7));
            if (body.empty()) {
                errors.emplace_back("@param " + name + " presets 缺少候选值");
                continue;
            }
            for (const std::string_view token : split(body, '|')) {
                double value = 0.0;
                if (!parse_double(token, value)) {
                    errors.emplace_back("@param " + name + " presets 含非法数值: " +
                                        std::string(token));
                    continue;
                }
                presets.emplace_back(token);
            }
        } else {
            if (!constraint.empty())
                constraint.push_back(':');
            constraint.append(field.data(), field.size());
        }
    }

    if (type != "int" && type != "float" && type != "bool" && type != "choice" &&
        type != "str" && type != "path") {
        errors.emplace_back("@param " + name + " 类型未知: " + std::string(fields[1]));
        return std::nullopt;
    }

    bool has_range = false;
    double min_value = 0.0;
    double max_value = 0.0;
    if (type == "int" || type == "float") {
        if (!constraint.empty()) {
            const std::size_t separator = constraint.find("..");
            if (separator == std::string::npos) {
                errors.emplace_back("@param " + name + " " + type + " 约束格式错误: " + constraint);
                return std::nullopt;
            }
            bool ok_min = false;
            bool ok_max = false;
            if (type == "int") {
                int lo = 0;
                int hi = 0;
                ok_min = parse_integer(std::string_view(constraint).substr(0, separator), lo);
                ok_max = parse_integer(std::string_view(constraint).substr(separator + 2), hi);
                min_value = static_cast<double>(lo);
                max_value = static_cast<double>(hi);
            } else {
                ok_min = parse_double(std::string_view(constraint).substr(0, separator), min_value);
                ok_max = parse_double(std::string_view(constraint).substr(separator + 2), max_value);
            }
            if (!ok_min || !ok_max) {
                errors.emplace_back("@param " + name + " " + type + " 约束数值无法解析: " + constraint);
                return std::nullopt;
            }
            has_range = true;
        }
    }

    Json default_value;
    if (type == "int") {
        int value = 0;
        if (!parse_integer(default_text, value)) {
            errors.emplace_back("@param " + name + " 默认值不是合法 int: " + default_text);
            return std::nullopt;
        }
        if (has_range && (value < min_value || value > max_value)) {
            errors.emplace_back("@param " + name + " 默认值 " + std::to_string(value) +
                                " 超出范围 " + number_text(min_value) + ".." + number_text(max_value));
            return std::nullopt;
        }
        default_value = value;
    } else if (type == "float") {
        double value = 0.0;
        if (!parse_double(default_text, value)) {
            errors.emplace_back("@param " + name + " 默认值不是合法 float: " + default_text);
            return std::nullopt;
        }
        if (has_range && (value < min_value || value > max_value)) {
            errors.emplace_back("@param " + name + " 默认值 " + number_text(value) +
                                " 超出范围 " + number_text(min_value) + ".." + number_text(max_value));
            return std::nullopt;
        }
        default_value = value;
    } else if (type == "bool") {
        const std::string value = lower_ascii(default_text);
        if (value == "true") {
            default_value = true;
        } else if (value == "false") {
            default_value = false;
        } else {
            errors.emplace_back("@param " + name + " bool 默认值只认 true/false: " + default_text);
            return std::nullopt;
        }
    } else if (type == "choice") {
        if (constraint.empty()) {
            errors.emplace_back("@param " + name + " choice 缺少候选约束");
            return std::nullopt;
        }
        std::vector<std::string> choices;
        for (const std::string_view choice : split(constraint, '|'))
            choices.emplace_back(choice);
        const bool found = std::find(choices.begin(), choices.end(), default_text) != choices.end();
        if (!found) {
            std::string joined;
            for (std::size_t i = 0; i < choices.size(); ++i) {
                if (i != 0)
                    joined.push_back('|');
                joined += choices[i];
            }
            errors.emplace_back("@param " + name + " 默认值 " + default_text + " 不在候选 " +
                                joined + " 中");
            return std::nullopt;
        }
        default_value = default_text;
        Json choices_json = Json::array();
        for (const std::string& choice : choices)
            choices_json.push_back(choice);
        Json result = {
            {"name", name}, {"kind", type}, {"default", default_text}, {"label", label},
            {"constraints", constraint.empty() ? Json(nullptr) : Json(constraint)},
            {"presets", presets}, {"choices", std::move(choices_json)},
            {"hasRange", false}, {"minValue", 0.0}, {"maxValue", 0.0},
            {"defaultValue", std::move(default_value)}
        };
        return result;
    } else {
        default_value = default_text;
    }

    Json presets_json = Json::array();
    for (const std::string& preset : presets)
        presets_json.push_back(preset);
    return Json{
        {"name", name}, {"kind", type}, {"default", default_text}, {"label", label},
        {"constraints", constraint.empty() ? Json(nullptr) : Json(constraint)},
        {"presets", std::move(presets_json)}, {"choices", Json::array()},
        {"hasRange", has_range}, {"minValue", min_value}, {"maxValue", max_value},
        {"defaultValue", std::move(default_value)}
    };
}

bool valid_executable_name(std::string_view value)
{
    if (value.size() == 1) {
        const char ch = value.front();
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    }
    if (value.size() < 2)
        return false;
    const auto is_first_or_last = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    };
    const auto is_allowed = [&](char ch) {
        return is_first_or_last(ch) || ch == '.';
    };
    if (!is_first_or_last(value.front()) || !is_first_or_last(value.back()))
        return false;
    return std::all_of(value.begin(), value.end(), is_allowed);
}

#ifdef _WIN32
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

struct CatalogFile {
    NativePath path;
    std::string id;
};

#ifdef _WIN32
std::wstring widen_utf8(std::string_view value)
{
    if (value.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0)
        throw std::runtime_error("脚本目录不是合法 UTF-8 路径");
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), needed) != needed)
        throw std::runtime_error("脚本目录 UTF-8 转换失败");
    return result;
}

std::string narrow_utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        throw std::runtime_error("脚本路径不是合法 UTF-16");
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
                            static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr) != needed)
        throw std::runtime_error("脚本路径 UTF-8 转换失败");
    return result;
}

NativePath append_native(NativePath directory, std::wstring_view name)
{
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
        directory.push_back(L'\\');
    directory.append(name.data(), name.size());
    return directory;
}

bool is_python_file(std::string_view name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot != std::string_view::npos && lower_ascii(name.substr(dot)) == ".py";
}

void collect_files(const NativePath& directory, const std::string& relative_prefix,
                   std::vector<CatalogFile>& files)
{
    NativePath pattern = directory;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND)
            return;
        throw std::runtime_error("无法扫描脚本目录: " + narrow_utf8(directory));
    }

    std::vector<std::pair<std::wstring, DWORD>> children;
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;
        children.emplace_back(std::wstring(name), data.dwFileAttributes);
    } while (FindNextFileW(search, &data));
    const DWORD final_code = GetLastError();
    FindClose(search);
    if (final_code != ERROR_NO_MORE_FILES)
        throw std::runtime_error("扫描脚本目录时发生错误: " + narrow_utf8(directory));

    for (const auto& child : children) {
        const std::string child_name = narrow_utf8(child.first);
        const std::string id = relative_prefix.empty()
                                   ? child_name
                                   : relative_prefix + "/" + child_name;
        const NativePath path = append_native(directory, child.first);
        if ((child.second & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            collect_files(path, id, files);
        } else if (is_python_file(child_name)) {
            files.push_back({path, id});
        }
    }
}
#else
NativePath append_native(const NativePath& directory, std::string_view name)
{
    NativePath result = directory;
    if (!result.empty() && result.back() != '/')
        result.push_back('/');
    result.append(name.data(), name.size());
    return result;
}

bool is_python_file(std::string_view name)
{
    const std::size_t dot = name.find_last_of('.');
    return dot != std::string_view::npos && lower_ascii(name.substr(dot)) == ".py";
}

void collect_files(const NativePath& directory, const std::string& relative_prefix,
                   std::vector<CatalogFile>& files)
{
    DIR* stream = opendir(directory.c_str());
    if (!stream)
        throw std::runtime_error("无法扫描脚本目录: " + directory);
    std::vector<std::string> children;
    errno = 0;
    while (dirent* entry = readdir(stream)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..")
            children.push_back(name);
        errno = 0;
    }
    const int read_error = errno;
    closedir(stream);
    if (read_error != 0)
        throw std::runtime_error("扫描脚本目录时发生错误: " + directory);

    for (const std::string& name : children) {
        const NativePath path = append_native(directory, name);
        struct stat info {};
        if (lstat(path.c_str(), &info) != 0)
            throw std::runtime_error("无法检查脚本路径: " + path);
        const std::string id = relative_prefix.empty() ? name : relative_prefix + "/" + name;
        if (S_ISDIR(info.st_mode)) {
            collect_files(path, id, files);
        } else if (S_ISREG(info.st_mode) && is_python_file(name)) {
            files.push_back({path, id});
        }
    }
}
#endif

std::string read_file_utf8_path(const NativePath& path, std::string_view id)
{
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* file = std::fopen(path.c_str(), "rb");
#endif
    if (!file)
        throw std::runtime_error("无法打开脚本文件: " + std::string(id));

    std::string contents;
    char buffer[16 * 1024];
    for (;;) {
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        contents.append(buffer, read);
        if (read != sizeof(buffer)) {
            if (std::ferror(file)) {
                std::fclose(file);
                throw std::runtime_error("读取脚本文件失败: " + std::string(id));
            }
            break;
        }
    }
    std::fclose(file);
    return contents;
}

Json parse_manifest(std::string_view source, std::string_view id)
{
    std::string normalized = normalize_newlines(source);
    const std::string id_text(id);
    std::string normalized_id(id);
    std::replace(normalized_id.begin(), normalized_id.end(), '\\', '/');
    const std::size_t slash = normalized_id.find_last_of('/');
    const std::string fallback_group = slash == std::string::npos
                                           ? std::string{}
                                           : normalized_id.substr(0, slash);
    std::string fallback_name = slash == std::string::npos
                                    ? normalized_id
                                    : normalized_id.substr(slash + 1);
    const std::size_t extension = fallback_name.find_last_of('.');
    if (extension != std::string::npos)
        fallback_name.erase(extension);

    std::string name = fallback_name;
    std::string group = fallback_group;
    std::string description;
    std::string accepts = "file";
    std::vector<std::string> extensions;
    bool multi = true;
    std::vector<std::string> requirements;
    std::optional<std::string> destructive;
    std::string host = "python";
    std::optional<std::string> blender_path;
    std::optional<std::string> exe;
    std::vector<Json> params;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::set<std::string> seen_keys;
    bool saw_name = false;
    bool saw_group = false;
    bool saw_description = false;
    bool saw_accepts = false;
    bool exe_key_errored = false;

    const std::optional<std::string_view> doc = module_docstring(normalized);
    if (doc) {
        std::size_t line_begin = 0;
        while (line_begin <= doc->size()) {
            const std::size_t line_end = doc->find('\n', line_begin);
            const std::size_t actual_end = line_end == std::string_view::npos
                                               ? doc->size()
                                               : line_end;
            const std::string_view line = trim(doc->substr(line_begin, actual_end - line_begin));
            if (!line.empty() && line.front() == '@') {
                std::size_t key_end = 1;
                while (key_end < line.size()) {
                    const char ch = line[key_end];
                    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '_')
                        ++key_end;
                    else
                        break;
                }
                if (key_end > 1 && (key_end == line.size() || is_space(line[key_end]))) {
                    const std::string key(line.substr(1, key_end - 1));
                    const std::string value(trim(line.substr(key_end)));

                    if (key != "param") {
                        if (seen_keys.count(key) != 0)
                            warnings.emplace_back("重复的 @" + key + "，取最后一次出现的值");
                        seen_keys.insert(key);
                    }

                    if (key == "name") {
                        saw_name = true;
                        name = value;
                    } else if (key == "group") {
                        saw_group = true;
                        group = value;
                    } else if (key == "desc") {
                        saw_description = true;
                        description = value;
                    } else if (key == "accepts") {
                        saw_accepts = true;
                        const std::string parsed = lower_ascii(value);
                        if (parsed == "file" || parsed == "dir" || parsed == "both")
                            accepts = parsed;
                        else
                            errors.emplace_back("@accepts 值未知: " + value);
                    } else if (key == "ext") {
                        extensions.clear();
                        for (std::string extension : split_whitespace(value)) {
                            extension = lower_ascii(extension);
                            if (extension.empty() || extension.front() != '.')
                                extension.insert(extension.begin(), '.');
                            extensions.push_back(std::move(extension));
                        }
                    } else if (key == "multi") {
                        const std::string parsed = lower_ascii(value);
                        if (parsed == "true" || parsed == "1")
                            multi = true;
                        else if (parsed == "false" || parsed == "0")
                            multi = false;
                        else
                            warnings.emplace_back("@multi 值未知，保留默认值 true: " + value);
                    } else if (key == "requires") {
                        requirements = split_whitespace(value); // Keep the optional `?` suffix for both frontends.
                    } else if (key == "destructive") {
                        if (value.empty())
                            errors.emplace_back("@destructive 缺少原因说明");
                        else
                            destructive = value;
                    } else if (key == "host") {
                        const std::string parsed = lower_ascii(value);
                        if (parsed == "python" || parsed == "blender" || parsed == "exe")
                            host = parsed;
                        else
                            warnings.emplace_back("@host 值未知，保留默认值 python: " + value);
                    } else if (key == "blender") {
                        blender_path = value;
                    } else if (key == "exe") {
                        if (value.empty()) {
                            errors.emplace_back("@exe 缺少可执行文件名");
                            exe_key_errored = true;
                        } else if (valid_executable_name(value)) {
                            exe = value;
                        } else {
                            errors.emplace_back("@exe 名字含非法字符: " + value +
                                                "（只允许字母、数字、下划线、连字符和内部点号）");
                            exe_key_errored = true;
                        }
                    } else if (key == "param") {
                        if (std::optional<Json> param = parse_param(value, errors))
                            params.push_back(std::move(*param));
                    } else {
                        warnings.emplace_back("未知 @key: @" + key);
                    }
                }
            }

            if (line_end == std::string_view::npos)
                break;
            line_begin = line_end + 1;
        }
    }

    if (!saw_name || name.empty())
        errors.emplace_back("缺少必填键 @name");
    if (!saw_group || group.empty())
        errors.emplace_back("缺少必填键 @group");
    if (!saw_description || description.empty())
        errors.emplace_back("缺少必填键 @desc");
    if (!saw_accepts)
        errors.emplace_back("缺少必填键 @accepts");
    if (host == "exe" && !exe && !exe_key_errored)
        errors.emplace_back("@host exe 需要在脚本头用 @exe 声明后端可执行文件名");

    const bool valid = errors.empty();
    Json result = {
        {"id", id_text},
        {"name", name},
        {"group", group},
        {"description", description},
        {"accepts", accepts},
        {"extensions", extensions},
        {"multi", multi},
        {"requires", requirements},
        {"destructive", destructive ? Json(*destructive) : Json(nullptr)},
        {"host", host},
        {"blenderPath", blender_path && !blender_path->empty() ? Json(*blender_path) : Json(nullptr)},
        {"exe", exe ? Json(*exe) : Json(nullptr)},
        {"params", params},
        {"valid", valid},
        {"errors", errors},
        {"warnings", warnings}
    };
    return result;
}

} // namespace

std::string parse_source_json(std::string_view source, std::string_view id)
{
    return parse_manifest(source, id).dump();
}

std::string catalog_json(std::string_view root_utf8)
{
    if (root_utf8.empty())
        throw std::runtime_error("脚本目录不能为空");
    NativePath root;
#ifdef _WIN32
    root = widen_utf8(root_utf8);
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        throw std::runtime_error("脚本目录不存在或不是目录: " + std::string(root_utf8));
#else
    root.assign(root_utf8.data(), root_utf8.size());
    struct stat root_info {};
    if (stat(root.c_str(), &root_info) != 0 || !S_ISDIR(root_info.st_mode))
        throw std::runtime_error("脚本目录不存在或不是目录: " + std::string(root_utf8));
#endif

    std::vector<CatalogFile> paths;
    collect_files(root, {}, paths);
    std::sort(paths.begin(), paths.end(), [](const CatalogFile& left, const CatalogFile& right) {
        return left.id < right.id;
    });

    Json scripts = Json::array();
    for (const CatalogFile& entry : paths) {
        scripts.push_back(parse_manifest(read_file_utf8_path(entry.path, entry.id), entry.id));
    }
    return scripts.dump();
}

} // namespace go::manifest
