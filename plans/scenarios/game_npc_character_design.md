# Game NPC character design

**Status:** parked.
**Source:** `ideas.md` lines 111-113.
**Q-link:** `plans/open_questions.md` Q4.

## Scenario

> game NPC character design
>     - user switch talking between character, admin, other
>       characters
>     - ask characters talk to each other

acva drives multiple NPC characters in a game-like setting:

- The user can switch which NPC they're addressing
  ("hey guard, …").
- An "admin" mode lets the user step out of character
  ("pause: change the dragon's mood to angry").
- NPCs can be asked to talk to each other ("guard, ask
  the merchant about the stolen ring").

## Why interesting

- It exercises everything: voice ID (M12.P12.1), persona
  switching (M12.P12.4), multi-context (M12.P12.5),
  address detection (M9.P9.3 — extended to multiple
  addresses), TTS expression (M9.P9.5 mood mapping).
- Differentiates acva from any of the smart-speaker
  competitors.

## Why parked

- No demand. Speculative.
- Multi-NPC concurrent dialog needs M12.P12.5 multi-context
  to land first.
- Game integration (engine plugin? OBS? Roll20?) is its
  own large surface.

## Trigger to pick up

Real game-design dogfood — operator running an RPG session
or a streamer with an audience asking for it.

## Investigation shape if picked up

Would become a phase under a new milestone (M16 +) with
stages:
1. NPC personality registry (extends M12 personality)
2. Address detection multi-target — "guard" vs "merchant"
   vs ambient
3. NPC-to-NPC dialog turn-taking
4. Admin mode (out-of-character commands)
5. Engine integration adapter (Foundry / Roll20 / custom)
6. Dogfood + UX iteration with a real game

Estimate: 2-3 months. Full new milestone.
