#include "supervisor/startup_check.hpp"

#include "log/log.hpp"
#include "stt/openai_stt_client.hpp"

#include <httplib.h>
#include <portaudio.h>

#include <fmt/format.h>

#include <chrono>
#include <string>
#include <vector>

namespace acva::supervisor {

namespace {

std::string authority_of(const std::string& base_url) {
    auto pos = base_url.find("://");
    auto start = (pos == std::string::npos) ? 0 : pos + 3;
    auto path = base_url.find('/', start);
    if (path == std::string::npos) return base_url;
    return base_url.substr(0, path);
}

// 1-token chat completion → forces the LLM weights + KV cache into
// VRAM. The actual reply text is discarded.
void check_llm(const config::Config& cfg, std::vector<StartupFailure>& out) {
    if (cfg.llm.base_url.empty()) return; // backend not configured

    httplib::Client cli(authority_of(cfg.llm.base_url));
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);

    const auto body = fmt::format(
        R"({{"model":"{}","messages":[{{"role":"user","content":"hi"}}],)"
        R"("max_tokens":1,"stream":false}})",
        cfg.llm.model);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Post("/v1/chat/completions", body, "application/json");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (!res) {
        out.push_back(StartupFailure{
            .component = "llm",
            .error = fmt::format("POST /v1/chat/completions: no response ({}; {} ms)",
                                  httplib::to_string(res.error()),
                                  static_cast<long long>(ms)),
            .remediation =
                "verify llama-server is running and reachable at "
                + cfg.llm.base_url
                + " — `docker compose -f packaging/compose/docker-compose.yml ps llama` "
                  "should show it healthy",
        });
        return;
    }
    if (res->status != 200) {
        out.push_back(StartupFailure{
            .component = "llm",
            .error = fmt::format("HTTP {} after {} ms: {}",
                                  res->status, static_cast<long long>(ms),
                                  res->body),
            .remediation =
                "check the model id `cfg.llm.model` matches what llama is "
                "serving (`curl " + cfg.llm.base_url + "/models`); "
                "missing GGUF → `tools/acva-models install <alias>` "
                "and restart",
        });
        return;
    }
    log::info("startup_check", fmt::format(
        "llm force-load ok in {} ms", static_cast<long long>(ms)));
}

// Reuse the existing silent-WAV warmup. It already issues a
// multipart POST to /v1/audio/transcriptions and returns a structured
// result.
void check_stt(const config::Config& cfg, std::vector<StartupFailure>& out) {
    if (cfg.stt.base_url.empty()) return;
    const auto r = stt::warmup(cfg.stt);
    if (r.ok) {
        log::info("startup_check", fmt::format(
            "stt force-load ok in {} ms", r.ms));
        return;
    }
    out.push_back(StartupFailure{
        .component = "stt",
        .error = fmt::format("warmup failed in {} ms: {}", r.ms, r.error),
        .remediation =
            "verify Speaches is running, the configured model id "
            "(" + cfg.stt.model + ") is downloadable, and that VRAM is "
            "available — `tools/acva-models install <stt-alias>` if missing",
    });
}

// 5-character "test" against the configured English voice. We hit
// /v1/audio/speech directly with libcurl-equivalent (httplib here).
// Don't require the response body to look any particular way — a 200
// is sufficient evidence Speaches loaded the voice.
void check_tts(const config::Config& cfg, std::vector<StartupFailure>& out) {
    if (cfg.tts.base_url.empty()) return;
    if (cfg.tts.voices_resolved.empty()) {
        // Voices weren't configured — nothing to load.
        return;
    }
    // Pick the fallback (English by default) voice.
    const auto& fallback = cfg.tts.voices_resolved.find(cfg.tts.fallback_lang);
    if (fallback == cfg.tts.voices_resolved.end()) {
        // No matching voice for fallback_lang; skip rather than fail
        // — the operator may have intentionally omitted the
        // fallback in a single-language config.
        return;
    }
    const auto& voice = fallback->second;

    httplib::Client cli(authority_of(cfg.tts.base_url));
    cli.set_connection_timeout(5);
    cli.set_read_timeout(30);

    const auto body = fmt::format(
        R"({{"model":"{}","voice":"{}","input":"hello","response_format":"pcm"}})",
        voice.model_id, voice.voice_id);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = cli.Post("/v1/audio/speech", body, "application/json");
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (!res) {
        out.push_back(StartupFailure{
            .component = "tts",
            .error = fmt::format("POST /v1/audio/speech: no response ({}; {} ms)",
                                  httplib::to_string(res.error()),
                                  static_cast<long long>(ms)),
            .remediation =
                "verify Speaches is reachable at " + cfg.tts.base_url
                + " and the voice (" + voice.model_id + "/" + voice.voice_id
                + ") is downloadable",
        });
        return;
    }
    if (res->status != 200) {
        out.push_back(StartupFailure{
            .component = "tts",
            .error = fmt::format("HTTP {} after {} ms", res->status,
                                  static_cast<long long>(ms)),
            .remediation =
                "if the response says \"voice not found\", install via "
                "`tools/acva-models install <tts-alias>` and restart",
        });
        return;
    }
    log::info("startup_check", fmt::format(
        "tts force-load ok in {} ms", static_cast<long long>(ms)));
}

// Open the configured PortAudio input device for ~100 ms and close
// it. If PortAudio falls back to a no-op host API (e.g. the user has
// no microphone configured but capture_enabled is true), the open
// itself returns paNoDevice or similar — we surface that.
void check_capture(const config::Config& cfg, std::vector<StartupFailure>& out) {
    if (!cfg.audio.capture_enabled) return;

    if (auto err = Pa_Initialize(); err != paNoError) {
        out.push_back(StartupFailure{
            .component = "capture",
            .error = fmt::format("Pa_Initialize: {}", Pa_GetErrorText(err)),
            .remediation =
                "verify PortAudio finds the audio host (PipeWire / Pulse / "
                "ALSA) and the configured input device exists",
        });
        return;
    }

    PaStreamParameters in{};
    in.device = Pa_GetDefaultInputDevice();
    if (in.device == paNoDevice) {
        Pa_Terminate();
        out.push_back(StartupFailure{
            .component = "capture",
            .error = "Pa_GetDefaultInputDevice returned paNoDevice",
            .remediation =
                "no input device available — is the microphone plugged in "
                "and not held by another process?",
        });
        return;
    }
    in.channelCount = 1;
    in.sampleFormat = paInt16;
    in.suggestedLatency =
        Pa_GetDeviceInfo(in.device)->defaultLowInputLatency;
    in.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    auto err = Pa_OpenStream(
        &stream, &in, nullptr, /*sample_rate*/ 48000.0,
        /*frames_per_buffer*/ 480, paClipOff,
        /*callback*/ nullptr, /*userdata*/ nullptr);
    if (err != paNoError) {
        Pa_Terminate();
        out.push_back(StartupFailure{
            .component = "capture",
            .error = fmt::format("Pa_OpenStream: {}", Pa_GetErrorText(err)),
            .remediation =
                "another app may have exclusive access to the mic; close it "
                "or pick a different device via cfg.audio.input_device",
        });
        return;
    }

    // Open + close is enough to verify the device responds; we don't
    // need to actually start the stream (which would block 100 ms
    // and wake a real audio thread).
    Pa_CloseStream(stream);
    Pa_Terminate();
    log::info("startup_check", "capture-readiness ok");
}

} // namespace

std::vector<StartupFailure> run_startup_checks(const config::Config& cfg) {
    std::vector<StartupFailure> failures;
    if (!cfg.supervisor.startup_force_load) {
        // Operator has not opted in. Capture-readiness is also
        // gated on this so a "tolerant boot" makes no startup
        // demands beyond the supervisor's own /health probes.
        return failures;
    }

    log::info("startup_check", "force-load gate running");
    check_llm(cfg, failures);
    check_stt(cfg, failures);
    check_tts(cfg, failures);
    check_capture(cfg, failures);
    return failures;
}

} // namespace acva::supervisor
