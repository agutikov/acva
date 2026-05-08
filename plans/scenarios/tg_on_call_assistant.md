# Telegram on-call assistant

**Status:** parked.
**Source:** `ideas.md` line 166.
**Q-link:** `plans/open_questions.md` Q4.

## Scenario

> tg on-call assistent

acva integrates with Telegram so an on-call engineer can
voice-chat with it from their phone (Telegram voice
messages → acva → Telegram voice replies). Use case: away
from desk, pager fires, "what was the last alert? when did
that service start failing? what runbook applies?"

## Why interesting

- Telegram has a stable Bot API with voice message support;
  no app development needed.
- M11 tools (especially KG + web search) make acva useful
  for runbook lookups.
- Voice replies on phone solve the "I'm walking my dog and
  paged" problem.

## Why parked

- Bridges acva from "local-first" to "cloud-touching" —
  Telegram's servers see the audio. Pillar tension with
  the project's privacy stance.
- Auth: Telegram bot tokens are infinite-lived; if leaked,
  attacker has full access. acva has no per-bot auth model.
- M11 tools have to actually be useful first; without
  knowledge graph + runbook integration, this is just a
  Telegram chat-with-LLM bot.

## Trigger to pick up

An on-call team with a runbook surface (Confluence, Notion,
local markdown) that wants a hands-free voice-driven query
interface. Plus M11 tools.

## Investigation shape if picked up

Phase with stages:
1. Telegram bot adapter (`src/integrations/telegram/`)
2. Voice-message round-trip (download → STT → reply →
   TTS upload)
3. Auth + per-user binding (Telegram user → acva
   `user_id`)
4. On-call-specific tools (paging API, runbook search,
   metrics query)
5. Privacy + data-handling write-up

Estimate: 1-2 months. Could fit as an M11 phase if the
tools are already there.
