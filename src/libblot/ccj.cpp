#include "blot/ccj.hpp"

#include <clang-c/Index.h>
#include <fmt/std.h>

#include <boost/filesystem.hpp>
#include <boost/json.hpp>
#include <boost/system/system_error.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "auto.hpp"
#include "logger.hpp"
#include "utils.hpp"

namespace xpto::blot {

namespace fs = std::filesystem;
namespace json = boost::json;
namespace bs = boost::system;

// Helper function to parse compile_commands.json
json::array parse_ccj(const fs::path& compile_commands_path) {
  std::ifstream is{compile_commands_path.string()};
  if (!is) utils::throwf("Can't parse {}", compile_commands_path);
  return json::parse(is).as_array();
}

std::optional<fs::path> find_ccj() {
  auto probe = fs::current_path() / "compile_commands.json";
  if (fs::exists(probe)) return probe;
  return std::nullopt;
}

// Helper function to create a translation unit from a file and its compile
// command
CXTranslationUnit create_translation_unit(
    CXIndex index, const fs::path& file_path, const std::string& command) {
  std::vector<std::string> args;
  std::vector<const char*> argv;
  std::istringstream iss(command);
  std::string token;

  // Skip the compiler executable name
  iss >> token;

  // Collect arguments until we hit compilation output flags
  // FIXME: In theory, include directories can come after -c and -o flags,
  // but libclang doesn't seem to handle this correctly. We may need to parse
  // the entire command and reorder arguments appropriately in the future.
  while (iss >> token) {
    if (token == "-o" || token == "-c") {
      break;
    }
    args.push_back(token);
  }

  argv.reserve(args.size());
  for (const auto& arg : args) {
    argv.push_back(arg.data());
  }

  return clang_parseTranslationUnit(
      index, file_path.c_str(), argv.data(), static_cast<int>(argv.size()),
      nullptr, 0, CXTranslationUnit_None);
}

std::optional<compile_command> infer(
    const fs::path& compile_commands_path, const fs::path& source_file) {
  LOG_INFO(
      "Searching for includes of '{}' in '{}'", source_file,
      compile_commands_path);

  auto entries = parse_ccj(compile_commands_path);

  fs::path ccj_dir = compile_commands_path.parent_path();
  auto absolute_maybe = [&](const fs::path& p) -> fs::path {
    if (p.is_absolute()) return p;
    return fs::absolute(ccj_dir / p);
  };

  struct context_s {
    const fs::path* needle{};
    bool match{};
  } context{&source_file, false};

  // Create clang index
  CXIndex index = clang_createIndex(0, 0);
  AUTO(clang_disposeIndex(index));

  // Process each .cpp file
  for (auto&& entry : entries) {
    auto& obj = entry.as_object();

    // Parse command arguments and create translation unit
    std::string command{obj["command"].as_string().c_str()};

    fs::path file{obj["file"].as_string().c_str()};
    fs::path dir{obj["directory"].as_string().c_str()};
    fs::path full = dir / file;

    CXTranslationUnit unit = create_translation_unit(index, full, command);
    AUTO(clang_disposeTranslationUnit(unit));

    if (!unit) continue;  // what is this? failure to parse? ¯\_(ツ)_/¯
    LOG_DEBUG("OK: Examining entry for '{}'", obj["file"].as_string().c_str());
    clang_getInclusions(
        unit,
        [](CXFile included_file, CXSourceLocation*, unsigned,
           CXClientData cookie) {
          auto* context = static_cast<context_s*>(cookie);

          CXString filename = clang_getFileName(included_file);
          AUTO(clang_disposeString(filename));
          fs::path includee = clang_getCString(filename);
          LOG_DEBUG("   OK: Saw this includee '{}'", includee);

          // Check if this inclusion matches our target header
          if (includee == *context->needle ||
              includee.filename() == context->needle->filename()) {
            LOG_INFO("SUCCESS: Found includer of '{}'", *context->needle);
            context->match = true;
          }
        },
        &context);

    if (context.match) {
      return compile_command{
        .directory = absolute_maybe(dir),
        .command = command,
        .file = absolute_maybe(file),
      };
    }
  }
  return std::nullopt;
}

}  // namespace xpto::blot
