#include "go/manifest/catalog.hpp"

#include "json.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;

int failures = 0;

void check(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Json parse(std::string_view source, std::string_view id = "test.py")
{
    return Json::parse(go::manifest::parse_source_json(source, id));
}

const Json* find_script(const Json& scripts, const std::string& id)
{
    for (const Json& script : scripts) {
        if (script.value("id", std::string{}) == id)
            return &script;
    }
    return nullptr;
}

void test_repository_catalog()
{
    const Json scripts = Json::parse(go::manifest::catalog_json(GO_SCRIPT_MANIFEST_SCRIPTS_DIR));
    check(scripts.is_array(), "catalog output is a JSON array");
    check(!scripts.empty(), "catalog discovers Python scripts under the configured root");

    for (const Json& script : scripts) {
        for (const char* field : {"id", "name", "group", "description", "accepts", "extensions",
                                  "multi", "requires", "destructive", "host", "blenderPath",
                                  "exe", "params", "valid", "errors", "warnings"})
            check(script.contains(field), std::string("catalog item contains field ") + field);
        check(script.value("valid", false),
              "current script is valid: " + script.value("id", std::string("<missing id>")));
    }

    const Json* exr = find_script(scripts, "Image/Format Convert/img.EXR2PNG_Large.py");
    check(exr != nullptr, "catalog contains EXR2PNG_Large");
    if (exr) {
        check(exr->value("accepts", std::string{}) == "file", "EXR2PNG accepts files");
        check(exr->value("params", Json::array()).size() == 7, "EXR2PNG has all seven parameters");
        const Json requires = exr->value("requires", Json::array());
        bool retained_optional_suffix = false;
        for (const Json& requirement : requires) {
            if (requirement.is_string()) {
                const std::string value = requirement.get<std::string>();
                if (!value.empty() && value.back() == '?')
                    retained_optional_suffix = true;
            }
        }
        check(retained_optional_suffix, "optional @requires suffixes are preserved");
    }

    const Json* pmx = find_script(scripts, "Geometry/Format Convert/geo.pmx2fbx.py");
    check(pmx != nullptr, "catalog contains geo.pmx2fbx");
    if (pmx) {
        check(pmx->value("host", std::string{}) == "blender", "PMX2FBX uses the Blender host");
        check(pmx->value("blenderPath", std::string{}).find("Blender 5.2") != std::string::npos,
              "PMX2FBX Blender path with spaces is retained");
    }
}

void test_docstring_position_and_prefixes()
{
    const Json valid = parse(
        "#!/usr/bin/env python3\r\n"
        "# -*- coding: utf-8 -*-\r\n"
        "# comment before declaration\r\n"
        "\r\n"
        "  rb\"\"\"\r\n"
        "@name Prefixed\r\n"
        "@group tests\r\n"
        "@desc declaration after allowed preamble\r\n"
        "@accepts file\r\n"
        "\"\"\"\r\n"
        "value = 1\r\n");
    check(valid.value("valid", false), "shebang, encoding, comments, whitespace, and rb prefix are accepted");

    const Json code_first = parse(
        "value = 1\n"
        "\"\"\"\n"
        "@name Too late\n"
        "@group tests\n"
        "@desc code precedes module docstring\n"
        "@accepts file\n"
        "\"\"\"\n");
    check(!code_first.value("valid", true), "code before the module docstring is rejected");
    check(!code_first.value("errors", Json::array()).empty(), "rejected module declaration has an error");

    const Json assigned_string = parse(
        "value = \"\"\"\n"
        "@name Not a module docstring\n"
        "@group tests\n"
        "@desc assigned string\n"
        "@accepts file\n"
        "\"\"\"\n");
    check(!assigned_string.value("valid", true), "a triple-quoted assigned string is not a module docstring");
}

void test_required_fields_and_parameter_types()
{
    const Json missing = parse("\"\"\"\n@name Partial\n\"\"\"\n");
    check(!missing.value("valid", true), "missing required declarations invalidate the script");
    const Json missing_errors = missing.value("errors", Json::array());
    check(missing_errors.size() == 3, "each absent required field has an explicit error");

    const Json parameter = parse(
        "\"\"\"\n"
        "@name Range\n"
        "@group tests\n"
        "@desc range and presets\n"
        "@accepts file\n"
        "@param amount : int : 3 : Amount : 1..5 : presets 1|4\n"
        "@param ratio : float : 0.5 : Ratio : 0..1\n"
        "@param enabled : bool : true : Enabled\n"
        "@param mode : choice : fast : Mode : fast|safe\n"
        "@param label : str : hello : Label\n"
        "\"\"\"");
    check(parameter.value("valid", false), "int/float/bool/choice/str parameter declarations parse");
    const Json params = parameter.value("params", Json::array());
    check(params.size() == 5, "all supported parameter kinds are retained");
    if (params.size() >= 1) {
        check(params[0].value("hasRange", false), "int parameter has range metadata");
        check(params[0].value("minValue", 0.0) == 1.0 && params[0].value("maxValue", 0.0) == 5.0,
              "int parameter min/max are numeric");
        check(params[0].value("defaultValue", Json()) == Json(3), "int defaultValue is typed JSON number");
        check(params[0].value("presets", Json::array()) == Json::array({"1", "4"}),
              "presets are returned as strings");
        check(params[0].value("constraints", std::string{}) == "1..5", "raw range constraint is retained");
    }
    if (params.size() >= 4)
        check(params[3].value("choices", Json::array()) == Json::array({"fast", "safe"}),
              "choice options are returned");

    const Json bad_range = parse(
        "\"\"\"\n@name Bad range\n@group tests\n@desc bad constraint\n@accepts file\n"
        "@param value : int : 3 : Value : 1..oops\n\"\"\"");
    check(!bad_range.value("valid", true), "malformed parameter range invalidates the script");
    check(bad_range.value("params", Json::array()).empty(), "malformed range is omitted from parameters");
}

void test_executable_name_errors_and_unicode_id()
{
    const Json illegal = parse(
        "\"\"\"\n@name Executable\n@group tests\n@desc invalid name\n@accepts file\n"
        "@host exe\n@exe go pmx\n\"\"\"");
    check(!illegal.value("valid", true), "unsafe executable names invalidate the script");
    bool has_qt_error = false;
    bool has_cross_error = false;
    for (const Json& error : illegal.value("errors", Json::array())) {
        const std::string value = error.get<std::string>();
        has_qt_error = has_qt_error || value.find("@exe 名字含非法字符") != std::string::npos;
        has_cross_error = has_cross_error || value.find("@host exe 需要在脚本头用 @exe 声明后端可执行文件名") != std::string::npos;
    }
    check(has_qt_error, "invalid @exe preserves the Qt-facing diagnostic");
    check(!has_cross_error, "invalid @exe does not add a duplicate host/exe diagnostic");

    const Json unicode_id = parse(
        "\"\"\"\n@name Unicode\n@group tests\n@desc unicode id\n@accepts file\n\"\"\"",
        u8"模型/测试.py");
    check(unicode_id.value("id", std::string{}) == u8"模型/测试.py", "UTF-8 script ids are preserved");
}

} // namespace

int main()
{
    test_repository_catalog();
    test_docstring_position_and_prefixes();
    test_required_fields_and_parameter_types();
    test_executable_name_errors_and_unicode_id();
    if (failures != 0)
        return 1;
    std::cout << "script manifest tests passed\n";
    return 0;
}
