# Low-latency online voice translation

**Status:** parked.
**Source:** `ideas.md` line 168.
**Q-link:** `plans/open_questions.md` Q4.

## Scenario

> low-latency online voice translation

User speaks in language A; acva produces speech in language
B in near-realtime, optionally with the original audio
suppressed so a meeting participant only hears the
translation.

## Why interesting

- All the building blocks already exist: STT (Speaches —
  multilingual), translation (LLM + system prompt), TTS
  (Speaches — per-language voices). The gap is the
  latency budget: real translation feels reasonable at
  P50 < 1 s, P95 < 2 s. acva's P50 1.7 s is already too
  slow.
- Address detection (M9.P9.3) helps — only translate
  addressed speech, not background.
- Multi-context (M12.P12.5) helps for parallel "translate
  to A" and "translate to B" when there are two listeners.

## Why parked

- Latency isn't there. Even M9.P9.2 speculation only buys
  ~200 ms.
- Translation quality at low-budget LLMs is not great
  beyond English-Russian-Spanish; the user would expect
  GPT-4-grade translation.
- Real translators integrate with conferencing software
  (Zoom captions, Meet); acva has no integration there.

## Trigger to pick up

A user with a real translation use case + tolerance for
the quality / latency trade-offs. Or a meaningful jump
in local LLM translation quality.

## Investigation shape if picked up

Phase with stages:
1. End-to-end latency budget analysis — where does the
   1 s go?
2. Translation-only LLM (smaller, faster, fine-tuned for
   translation) vs general-purpose
3. Streaming translation with mid-utterance commits
4. Conferencing integration (Zoom / Meet captions API,
   Discord / Mumble TTS injection)
5. Dogfood with a multi-language meeting

Estimate: 2-3 months.
