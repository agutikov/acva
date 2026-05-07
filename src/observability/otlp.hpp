#pragma once

#include "config/config.hpp"
#include "event/event.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

// M8B Step 3 — OTLP/HTTP traces, opt-in.
//
// This header exposes a small, otel-free surface so every TU that
// records spans compiles without pulling in `opentelemetry/...`
// headers. The .cpp gates the real implementation on
// `ACVA_HAVE_OTLP`; when the build was made without
// opentelemetry-cpp present, every method is a no-op and the
// `cfg.observability.otlp.enabled: true` flag emits a one-line warn
// at startup.
//
// Usage from main.cpp:
//
//   acva::observability::Tracer tracer;
//   tracer.init(cfg.observability);   // no-op if cfg.otlp.enabled = false
//   ...
//   tracer.shutdown();                // before bus.shutdown()
//
// Per-turn root spans live for the duration of a turn — the bus
// subscriber that wires them owns the Span object on a per-turn
// map keyed by turn_id (see src/observability/turn_span_subscriber.cpp).

namespace acva::observability {

class Tracer {
public:
    // RAII span handle. Move-only; closing happens in the destructor
    // OR via end() (whichever comes first). The underlying type is
    // erased — the destructor calls a private hook that knows the
    // real `opentelemetry::trace::Span*` shape.
    class Span {
    public:
        Span() = default;
        explicit Span(std::shared_ptr<void> impl) : impl_(std::move(impl)) {}
        ~Span();

        Span(const Span&)            = delete;
        Span& operator=(const Span&) = delete;
        Span(Span&& other) noexcept            = default;
        Span& operator=(Span&& other) noexcept = default;

        void set_attribute(std::string_view key, std::string_view value);
        void set_attribute(std::string_view key, std::int64_t value);
        void set_status_ok();
        void set_status_error(std::string_view message);
        void end();
        [[nodiscard]] bool active() const noexcept { return static_cast<bool>(impl_); }

    private:
        // shared_ptr so a Span can be stashed in a map keyed by turn id
        // without forcing the holder to template on otel types.
        std::shared_ptr<void> impl_;
    };

    Tracer();
    ~Tracer();

    Tracer(const Tracer&)            = delete;
    Tracer& operator=(const Tracer&) = delete;
    Tracer(Tracer&&)                 = delete;
    Tracer& operator=(Tracer&&)      = delete;

    // Init reads cfg.observability.otlp; on success the global otel
    // tracer provider is set and start_turn_span() returns real
    // spans. On any failure (otel disabled, build without OTLP,
    // exporter ctor threw) returns false and start_turn_span()
    // returns inactive Span handles thereafter.
    bool init(const config::ObservabilityConfig& cfg);

    // Flushes pending spans + tears down the tracer provider.
    // Idempotent. Call before EventBus::shutdown so any subscriber
    // that still owns Spans can drain them first.
    void shutdown();

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // Mint the root `voice.turn` span for `turn`. The handle stays
    // open until the caller drops it (or calls `Span::end()`); the
    // bus subscriber pairs SpeechStarted with PlaybackFinished /
    // UserInterrupted to bracket a turn.
    [[nodiscard]] Span start_turn_span(event::TurnId turn);

private:
    bool enabled_ = false;
};

} // namespace acva::observability
