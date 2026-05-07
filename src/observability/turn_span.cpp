#include "observability/turn_span.hpp"

#include "event/event.hpp"

#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <variant>

namespace acva::observability {

namespace {

// Map of in-flight spans keyed by turn id. The bus subscriber owns
// the only reference to each Span — closing the span (via End() in
// the destructor) happens when the entry is erased on terminal
// events (PlaybackFinished / UserInterrupted / LlmFinished with
// cancellation or error).
struct SpanTable {
    std::mutex                                mu;
    std::unordered_map<event::TurnId, Tracer::Span> spans;
};

void open_span_if_absent(SpanTable& tbl, Tracer& tracer,
                          event::TurnId turn) {
    if (turn == event::kNoTurn) return;
    std::lock_guard lk(tbl.mu);
    if (tbl.spans.contains(turn)) return;
    auto span = tracer.start_turn_span(turn);
    if (!span.active()) return;
    tbl.spans.emplace(turn, std::move(span));
}

void close_span(SpanTable& tbl, event::TurnId turn,
                 bool ok, std::string_view error_msg = {}) {
    if (turn == event::kNoTurn) return;
    Tracer::Span span;
    {
        std::lock_guard lk(tbl.mu);
        auto it = tbl.spans.find(turn);
        if (it == tbl.spans.end()) return;
        span = std::move(it->second);
        tbl.spans.erase(it);
    }
    if (ok) span.set_status_ok();
    else    span.set_status_error(error_msg);
    span.end();
}

} // namespace

event::SubscriptionHandle
install_turn_span_subscriber(event::EventBus& bus, Tracer& tracer) {
    auto table = std::make_shared<SpanTable>();
    event::SubscribeOptions opts;
    opts.name = "otlp.turn_span";
    opts.queue_capacity = 256;
    opts.policy = event::OverflowPolicy::DropOldest;

    return bus.subscribe_all(opts,
        [table, &tracer](const event::Event& e) {
            std::visit([&]<class T>(const T& ev) {
                using Et = std::decay_t<T>;
                if constexpr (std::is_same_v<Et, event::LlmStarted>) {
                    // First event whose `turn` is the FSM-minted id.
                    open_span_if_absent(*table, tracer, ev.turn);
                } else if constexpr (std::is_same_v<Et, event::PlaybackFinished>) {
                    // Terminal when the assistant finishes speaking
                    // the last sentence of a turn.
                    close_span(*table, ev.turn, /*ok*/ true);
                } else if constexpr (std::is_same_v<Et, event::UserInterrupted>) {
                    close_span(*table, ev.turn, /*ok*/ false, "interrupted");
                } else if constexpr (std::is_same_v<Et, event::LlmFinished>) {
                    if (ev.cancelled) {
                        close_span(*table, ev.turn, /*ok*/ false, "llm_cancelled");
                    }
                    // The non-cancelled case keeps the span open
                    // until PlaybackFinished — the LLM finishes
                    // generating before TTS finishes speaking the
                    // last sentence, so closing here would truncate.
                }
            }, e);
        });
}

} // namespace acva::observability
