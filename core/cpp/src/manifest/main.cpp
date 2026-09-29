#include "go/manifest/catalog.hpp"

#include "json.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;

#ifdef _WIN32
std::string wide_to_utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    constexpr DWORD kRejectInvalidUtf16 = 0x00000080; // WC_ERR_INVALID_CHARS (Vista+)
    const int needed = WideCharToMultiByte(CP_UTF8, kRejectInvalidUtf16, value.data(),
                                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        throw std::runtime_error("command line contains invalid UTF-16");
    std::string result(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, kRejectInvalidUtf16, value.data(),
                            static_cast<int>(value.size()), result.data(), needed, nullptr, nullptr) != needed)
        throw std::runtime_error("command line UTF-8 conversion failed");
    return result;
}
#endif

int run(const std::vector<std::string>& args)
{
    bool check_only = false;
    std::string root;
    bool saw_root = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--check") {
            if (check_only) {
                std::cerr << "--check may only be specified once\n";
                return 2;
            }
            check_only = true;
        } else if (args[i] == "--root") {
            if (saw_root || i + 1 >= args.size() || args[i + 1].empty()) {
                std::cerr << "--root requires one scripts directory\n";
                return 2;
            }
            root = args[++i];
            saw_root = true;
        } else {
            std::cerr << "unknown argument: " << args[i] << '\n';
            return 2;
        }
    }
    if (!saw_root) {
        std::cerr << "usage: go_script_catalog [--check] --root <scripts-dir>\n";
        return 2;
    }

    try {
        const std::string output = go::manifest::catalog_json(root);
        if (!check_only) {
            std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
            std::cout.put('\n');
            return std::cout ? 0 : 1;
        }

        const Json scripts = Json::parse(output);
        bool invalid_found = false;
        for (const Json& script : scripts) {
            if (script.value("valid", false))
                continue;
            invalid_found = true;
            std::cerr << script.value("id", std::string{}) << ':';
            const Json errors = script.value("errors", Json::array());
            for (std::size_t i = 0; i < errors.size(); ++i)
                std::cerr << (i == 0 ? " " : "; ") << errors[i].get<std::string>();
            std::cerr << '\n';
        }
        return invalid_found ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    try {
        for (int i = 1; i < argc; ++i)
            args.push_back(wide_to_utf8(argv[i]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    return run(args);
}
#else
int main(int argc, char* argv[])
{
    std::vector<std::string> args;
    args.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return run(args);
}
#endif
