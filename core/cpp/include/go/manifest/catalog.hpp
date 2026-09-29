#pragma once

#include <string>
#include <string_view>

namespace go::manifest {

/// Parse one Python source file's module docstring declaration into a JSON object.
/// `id` is the caller-provided script identifier (catalog uses a UTF-8 relative path).
std::string parse_source_json(std::string_view source, std::string_view id);

/// Recursively read Python scripts under `root`, sorted by UTF-8 relative path,
/// and return their declarations as a UTF-8 JSON array.
std::string catalog_json(std::string_view root_utf8);

} // namespace go::manifest
