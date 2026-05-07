#include "observability/otlp.hpp"

#include "log/log.hpp"

#include <fmt/format.h>

#ifdef ACVA_HAVE_OTLP
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/tracer_provider.h>

namespace otel = opentelemetry;
namespace otel_sdk = opentelemetry::sdk;
namespace otel_trace = opentelemetry::trace;
namespace otel_otlp  = opentelemetry::exporter::otlp;
#endif

namespace acva::observability {

#ifdef ACVA_HAVE_OTLP

namespace {

constexpr const char* kInstrumentationName    = "acva";
constexpr const char* kInstrumentationVersion = "0.0.1";

// otel uses its own `nostd::string_view` (a byte-for-byte clone
// for ABI stability across C++ standards). Adapt at the boundary.
otel::nostd::string_view as_otel(std::string_view s) {
    return {s.data(), s.size()};
}

// Get the `acva` tracer from the globally-installed provider. Returns
// a nopTracer when init() never ran or shutdown() finalised it.
otel::nostd::shared_ptr<otel_trace::Tracer> get_tracer() {
    auto provider = otel_trace::Provider::GetTracerProvider();
    return provider->GetTracer(kInstrumentationName, kInstrumentationVersion);
}

// The shared_ptr erased into Span::impl_ holds an
// otel::nostd::shared_ptr<otel_trace::Span> wrapped in a deleter
// that calls End() at destruction time. The wrapper avoids leaking
// otel types through observability/otlp.hpp.
struct SpanHolder {
    otel::nostd::shared_ptr<otel_trace::Span> span;
};

}  // namespace

Tracer::Tracer()  = default;

bool Tracer::init(const config::ObservabilityConfig& cfg) {
    if (!cfg.otlp.enabled) {
        enabled_ = false;
        return false;
    }

    otel_otlp::OtlpHttpExporterOptions opts;
    opts.url = cfg.otlp.endpoint;
    auto exporter = otel_otlp::OtlpHttpExporterFactory::Create(opts);

    otel_sdk::trace::BatchSpanProcessorOptions bp_opts{};
    auto processor = otel_sdk::trace::BatchSpanProcessorFactory::Create(
        std::move(exporter), bp_opts);

    auto resource = otel_sdk::resource::Resource::Create({
        {"service.name",    cfg.otlp.service_name},
        {"service.version", kInstrumentationVersion},
    });

    auto provider = otel_sdk::trace::TracerProviderFactory::Create(
        std::move(processor), resource);

    // The global Provider takes a nostd::shared_ptr; the SDK
    // factory returns std::unique_ptr — convert via release.
    otel::nostd::shared_ptr<otel_trace::TracerProvider> provider_p(
        provider.release());
    otel_trace::Provider::SetTracerProvider(provider_p);

    enabled_ = true;
    log::info("otlp", fmt::format(
        "OTLP/HTTP traces enabled — endpoint={} service={}",
        cfg.otlp.endpoint, cfg.otlp.service_name));
    return true;
}

void Tracer::shutdown() {
    if (!enabled_) return;
    auto provider = otel_trace::Provider::GetTracerProvider();
    if (auto* p = dynamic_cast<otel_sdk::trace::TracerProvider*>(provider.get())) {
        p->ForceFlush(std::chrono::milliseconds{2000});
    }
    // Replace with a no-op provider so any late spans drop cleanly.
    otel::nostd::shared_ptr<otel_trace::TracerProvider> noop(
        new otel_trace::NoopTracerProvider{});
    otel_trace::Provider::SetTracerProvider(noop);
    enabled_ = false;
    log::info("otlp", "OTLP/HTTP traces shut down");
}

Tracer::~Tracer() { shutdown(); }

Tracer::Span Tracer::start_turn_span(event::TurnId turn) {
    if (!enabled_) return Span{};
    auto tracer = get_tracer();
    auto span = tracer->StartSpan("voice.turn", {
        {"turn_id", static_cast<int64_t>(turn)},
    });
    auto holder = std::make_shared<SpanHolder>();
    holder->span = std::move(span);
    return Span(std::shared_ptr<void>(std::move(holder)));
}

Tracer::Span::~Span() {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    if (holder->span) holder->span->End();
}

void Tracer::Span::set_attribute(std::string_view key, std::string_view value) {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    holder->span->SetAttribute(as_otel(key), as_otel(value));
}

void Tracer::Span::set_attribute(std::string_view key, std::int64_t value) {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    holder->span->SetAttribute(as_otel(key), value);
}

void Tracer::Span::set_status_ok() {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    holder->span->SetStatus(otel_trace::StatusCode::kOk);
}

void Tracer::Span::set_status_error(std::string_view message) {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    holder->span->SetStatus(otel_trace::StatusCode::kError, as_otel(message));
}

void Tracer::Span::end() {
    if (!impl_) return;
    auto* holder = static_cast<SpanHolder*>(impl_.get());
    if (holder->span) {
        holder->span->End();
        holder->span = nullptr;
    }
}

#else  // !ACVA_HAVE_OTLP — stub path

Tracer::Tracer()  = default;
Tracer::~Tracer() = default;

bool Tracer::init(const config::ObservabilityConfig& cfg) {
    if (cfg.otlp.enabled) {
        log::warn("otlp",
            "cfg.observability.otlp.enabled=true but acva was built "
            "without ACVA_HAVE_OTLP — install opentelemetry-cpp and "
            "rebuild to record traces");
    }
    enabled_ = false;
    return false;
}

void Tracer::shutdown() {}

Tracer::Span Tracer::start_turn_span(event::TurnId /*turn*/) { return Span{}; }

Tracer::Span::~Span() = default;
void Tracer::Span::set_attribute(std::string_view, std::string_view) {}
void Tracer::Span::set_attribute(std::string_view, std::int64_t) {}
void Tracer::Span::set_status_ok() {}
void Tracer::Span::set_status_error(std::string_view) {}
void Tracer::Span::end() {}

#endif

} // namespace acva::observability
