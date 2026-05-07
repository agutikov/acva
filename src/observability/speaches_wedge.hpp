#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace acva::observability {

// M8B Step 1 — Speaches CUDA-OOM wedge detection.
//
// Pure helpers split out from the VramMonitor so they can be unit-
// tested without a real /proc + nvidia-smi. The live monitor wires
// these together with a popen() of nvidia-smi and a `cat
// /proc/<pid>/cmdline` lookup; tests inject canned strings + maps.

// One row from `nvidia-smi --query-compute-apps=pid,used_memory
// --format=csv,noheader,nounits`.
struct ComputeApp {
    int  pid       = -1;
    long used_mib  = 0;
};

// Returns the parsed rows. Empty input or unparseable lines yield an
// empty vector — the live monitor treats that as "no compute apps
// reported, nothing to classify".
[[nodiscard]] std::vector<ComputeApp>
parse_compute_apps(std::string_view csv_output);

// Resolve a host PID to its full cmdline (NUL-separated argv joined
// with a space). Used by `classify_speaches` to identify which
// compute app is the speaches container's Python process. The live
// implementation reads `/proc/<pid>/cmdline`; tests inject a map.
using CmdlineLookup = std::function<std::string(int pid)>;

struct SpeachesProbe {
    int  pid       = -1;     // -1 when no speaches process found
    long used_mib  = 0;
    bool wedged    = false;
};

// Walk `apps`, look up each PID's cmdline, return the first whose
// cmdline contains "speaches". Sets `wedged=true` when the matched
// app's used_mib >= threshold_mib. Returns a default-constructed
// SpeachesProbe (pid=-1) when no match is found.
[[nodiscard]] SpeachesProbe
classify_speaches(std::span<const ComputeApp> apps,
                   const CmdlineLookup& lookup,
                   long threshold_mib);

// Production-side helpers — defined in the .cpp; the .hpp surfaces
// them so the live VramMonitor can use them without re-rolling.

// `nvidia-smi --query-compute-apps=pid,used_memory --format=csv,
//  noheader,nounits 2>/dev/null` — popen + read. Returns empty when
// nvidia-smi is unavailable.
[[nodiscard]] std::string run_nvidia_smi_compute_apps();

// Reads the NUL-separated cmdline at /proc/<pid>/cmdline and joins
// argv with spaces. Returns empty on permission error / missing PID.
[[nodiscard]] std::string read_proc_cmdline(int pid);

} // namespace acva::observability
