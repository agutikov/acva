#include "observability/speaches_wedge.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace acva::observability {

std::vector<ComputeApp>
parse_compute_apps(std::string_view csv_output) {
    std::vector<ComputeApp> rows;
    std::size_t start = 0;
    while (start < csv_output.size()) {
        const auto eol = csv_output.find('\n', start);
        const auto line_end = (eol == std::string_view::npos)
                                ? csv_output.size() : eol;
        std::string_view line = csv_output.substr(start, line_end - start);
        start = (eol == std::string_view::npos)
                  ? csv_output.size() : eol + 1;
        // Skip empty / whitespace-only lines.
        std::size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size()) continue;
        const auto comma = line.find(',', s);
        if (comma == std::string_view::npos) continue;
        // pid up to comma.
        std::string pid_str(line.substr(s, comma - s));
        std::string mib_str(line.substr(comma + 1));
        // Strip trailing whitespace from both.
        auto rstrip = [](std::string& v) {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t'
                                  || v.back() == '\r')) {
                v.pop_back();
            }
        };
        auto lstrip = [](std::string& v) {
            std::size_t i = 0;
            while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
            v.erase(0, i);
        };
        rstrip(pid_str); lstrip(pid_str);
        rstrip(mib_str); lstrip(mib_str);
        if (pid_str.empty() || mib_str.empty()) continue;

        ComputeApp app;
        try {
            app.pid      = static_cast<int>(std::stoi(pid_str));
            app.used_mib = std::stol(mib_str);
        } catch (...) {
            continue;
        }
        rows.push_back(app);
    }
    return rows;
}

SpeachesProbe
classify_speaches(std::span<const ComputeApp> apps,
                   const CmdlineLookup& lookup,
                   long threshold_mib) {
    SpeachesProbe out;
    for (const auto& app : apps) {
        const std::string cmdline = lookup(app.pid);
        if (cmdline.find("speaches") == std::string::npos) continue;
        out.pid      = app.pid;
        out.used_mib = app.used_mib;
        out.wedged   = (app.used_mib >= threshold_mib);
        return out;
    }
    return out;
}

std::string run_nvidia_smi_compute_apps() {
    std::FILE* p = ::popen(
        "nvidia-smi --query-compute-apps=pid,used_memory "
        "--format=csv,noheader,nounits 2>/dev/null", "r");
    if (!p) return {};
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
        out.append(buf.data(), n);
    }
    ::pclose(p);
    return out;
}

std::string read_proc_cmdline(int pid) {
    if (pid <= 0) return {};
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();
    // /proc/<pid>/cmdline uses NUL separators between argv entries
    // and a single trailing NUL. Replace separators with spaces so a
    // simple `find("speaches")` matches across the whole command line.
    for (auto& c : raw) if (c == '\0') c = ' ';
    return raw;
}

} // namespace acva::observability
