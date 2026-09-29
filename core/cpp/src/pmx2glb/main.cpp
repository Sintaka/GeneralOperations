// go_pmx2glb —— 启动器 @host exe 契约的独立命令行入口。
#include <go/pmx2glb/convert.hpp>
#include <go/pmx2glb/pmx.hpp>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "path_io.hpp"
#include "utf.hpp"

using go::pmx2glb::ConvertOptions;
using go::pmx2glb::PmxModel;

namespace {

void print_line(FILE* stream, const std::string& text) {
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

bool parse_scale(const std::string& text, float* value) {
    errno = 0;
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed) || parsed <= 0.0f) {
        return false;
    }
    *value = parsed;
    return true;
}

bool read_bytes(const std::wstring& path, std::vector<uint8_t>* bytes, std::string* error) {
    if (!go::pmx2glb::path_io::read_bytes(path, bytes)) {
        *error = "cannot read input file";
        return false;
    }
    return true;
}

int run(const std::vector<std::string>& args) {
    ConvertOptions options;
    std::vector<std::string> files;
    bool separator = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (!separator && args[i] == "--") {
            separator = true;
        } else if (!separator && args[i] == "--scale") {
            if (++i >= args.size() || !parse_scale(args[i], &options.scale)) {
                print_line(stderr, "error: --scale requires a finite positive number");
                return 2;
            }
        } else if (!separator && (args[i] == "--help" || args[i] == "-h")) {
            print_line(stdout, "usage: go_pmx2glb [--scale NUMBER] -- <file.pmx> [...]");
            return 0;
        } else if (!separator) {
            print_line(stderr, "error: unknown option before --: " + args[i]);
            return 2;
        } else {
            files.push_back(args[i]);
        }
    }
    if (!separator || files.empty()) {
        print_line(stderr, "error: expected -- followed by at least one .pmx file");
        return 2;
    }

    bool failed = false;
    for (const std::string& file : files) {
        const std::wstring input = go::pmx2glb::utf8_to_wide(file);
        std::wstring ext = go::pmx2glb::path_io::extension(input);
        for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
        if ((input.empty() && !file.empty()) || ext != L".pmx" ||
            !go::pmx2glb::path_io::exists(input, true)) {
            print_line(stderr, "error: input is not a .pmx file: " + file);
            failed = true;
            continue;
        }

        print_line(stdout, "[1/6] Parse PMX: " + file);
        std::vector<uint8_t> bytes;
        std::string error;
        if (!read_bytes(input, &bytes, &error)) {
            print_line(stderr, "error: " + error + ": " + file);
            failed = true;
            continue;
        }
        PmxModel model;
        if (!go::pmx2glb::parse_pmx(bytes.data(), bytes.size(), &model, &error)) {
            print_line(stderr, "error: " + file + ": " + error);
            failed = true;
            continue;
        }

        const std::wstring output_w = go::pmx2glb::path_io::replace_extension(input, L".glb");
        const std::string output = go::pmx2glb::wide_to_utf8(
            output_w.c_str(), static_cast<int>(output_w.size()));
        const std::wstring parent_w = go::pmx2glb::path_io::parent(input);
        const std::string parent = go::pmx2glb::wide_to_utf8(
            parent_w.c_str(), static_cast<int>(parent_w.size()));
        if (!go::pmx2glb::convert_pmx_to_glb(
                model, parent, output, options,
                [](const std::string& line) { print_line(stdout, line); }, &error)) {
            print_line(stderr, "error: " + file + ": " + error);
            failed = true;
            continue;
        }
        print_line(stdout, "DONE: " + output);
    }
    return failed ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        args.push_back(go::pmx2glb::wide_to_utf8(argv[i], static_cast<int>(std::wcslen(argv[i]))));
    return run(args);
}
