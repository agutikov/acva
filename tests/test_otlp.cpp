#include "config/config.hpp"
#include "event/bus.hpp"
#include "event/event.hpp"
#include "observability/otlp.hpp"
#include "observability/turn_span.hpp"

#include <doctest/doctest.h>

namespace cfg = acva::config;
namespace obs = acva::observability;

TEST_CASE("Tracer: disabled cfg leaves the tracer inactive") {
    obs::Tracer t;
    cfg::ObservabilityConfig oc;
    oc.otlp.enabled = false;
    CHECK_FALSE(t.init(oc));
    CHECK_FALSE(t.enabled());
    auto span = t.start_turn_span(/*turn*/ 42);
    CHECK_FALSE(span.active());
    // No-op methods don't crash.
    span.set_attribute("k", "v");
    span.set_attribute("n", 7);
    span.set_status_ok();
    span.set_status_error("oops");
    span.end();
    t.shutdown();
}

TEST_CASE("Tracer: shutdown is idempotent without init") {
    obs::Tracer t;
    t.shutdown();
    t.shutdown(); // second call also fine
}

#ifdef ACVA_HAVE_OTLP
TEST_CASE("Tracer: enabled-with-unreachable-endpoint init succeeds (queue-only)") {
    obs::Tracer t;
    cfg::ObservabilityConfig oc;
    oc.otlp.enabled = true;
    oc.otlp.endpoint = "http://127.0.0.1:1/v1/traces";  // nothing listens
    oc.otlp.service_name = "acva-test";

    // The OTLP/HTTP exporter doesn't synchronously connect on init —
    // it batches spans and posts them on flush. So init succeeds even
    // when the endpoint is unreachable; we only learn that on
    // shutdown's force_flush, which is best-effort.
    CHECK(t.init(oc));
    CHECK(t.enabled());

    auto span = t.start_turn_span(7);
    CHECK(span.active());
    span.set_attribute("test_attr", "value");
    span.set_status_ok();
    span.end();

    t.shutdown();  // flush attempts the unreachable endpoint, swallowed
}
#endif

TEST_CASE("install_turn_span_subscriber: opens + closes a per-turn span") {
    acva::event::EventBus bus;
    obs::Tracer tracer;
    cfg::ObservabilityConfig oc;
    oc.otlp.enabled = false;  // tracer stays no-op for unit speed
    tracer.init(oc);

    auto sub = obs::install_turn_span_subscriber(bus, tracer);

    // Drive a synthetic turn through the bus. With the tracer
    // disabled the calls are no-op, but we exercise the subscriber's
    // bookkeeping (open on LlmStarted, close on PlaybackFinished).
    bus.publish(acva::event::LlmStarted{ .turn = 11 });
    bus.publish(acva::event::PlaybackFinished{ .turn = 11, .seq = 0 });

    // UserInterrupted close path
    bus.publish(acva::event::LlmStarted{ .turn = 12 });
    bus.publish(acva::event::UserInterrupted{ .turn = 12, .ts = {} });

    // LlmFinished(cancelled) close path
    bus.publish(acva::event::LlmStarted{ .turn = 13 });
    bus.publish(acva::event::LlmFinished{ .turn = 13, .cancelled = true,
                                          .tokens_generated = 0 });

    // Untouched turn (no LlmStarted) — terminal events alone should
    // be no-op (no orphan close calls).
    bus.publish(acva::event::PlaybackFinished{ .turn = 99, .seq = 0 });

    bus.shutdown();
    tracer.shutdown();
}
