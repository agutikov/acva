#pragma once

#include "event/bus.hpp"
#include "observability/otlp.hpp"

namespace acva::observability {

// M8B Step 3 — bus subscriber that brackets each user turn with one
// `voice.turn` OTLP span. Subscribes to:
//
//   * SpeechStarted     → start_turn_span(turn) and stash the
//                         handle keyed by (turn, the FSM-minted id
//                         that flows through the rest of the
//                         pipeline).
//   * PlaybackFinished  → set_status_ok + close span.
//   * UserInterrupted   → set_status_error("interrupted") + close.
//   * LlmFinished(error) → set_status_error(error_message) + close.
//
// Returns the subscription handle so main.cpp can keep it alive
// for the run. When the Tracer is disabled, the subscriber still
// installs (cheap) but every span operation is a no-op.

[[nodiscard]] event::SubscriptionHandle
install_turn_span_subscriber(event::EventBus& bus, Tracer& tracer);

} // namespace acva::observability
