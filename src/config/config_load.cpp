// M8B Step 4 build-time split — this TU owns the Glaze YAML reader,
// which dominates config build time (~200 s on the dev box). Keeping
// it isolated lets `config.cpp` (validators + alias resolution)
// compile in parallel without re-paying the Glaze instantiation
// cost.
//
// Public entry points are `load_from_string` and `load_from_file`.
// They call into `acva::config::detail::resolve_aliases` (in
// `config.cpp`) and the public `validate` for non-Glaze work.

#include "config/config.hpp"

#include <glaze/yaml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace acva::config {

// Forward declaration — defined in `config.cpp`. Pulling the
// declaration in here keeps `config.hpp` free of a `detail::`
// namespace that's only meaningful to the two TUs in this module.
namespace detail {
void resolve_aliases(Config& cfg);
}

namespace {

std::string read_file(const std::filesystem::path& path, LoadError& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err.message = "config: cannot open file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

LoadResult load_from_string(std::string_view yaml) {
    Config cfg;
    auto ec = glz::read_yaml(cfg, yaml);
    if (ec) {
        return LoadError{"config: parse error: " + glz::format_error(ec, yaml)};
    }
    detail::resolve_aliases(cfg);
    if (auto verr = validate(cfg)) {
        return *verr;
    }
    return cfg;
}

LoadResult load_from_file(const std::filesystem::path& path) {
    LoadError err;
    auto text = read_file(path, err);
    if (!err.message.empty()) {
        return err;
    }
    return load_from_string(text);
}

} // namespace acva::config
