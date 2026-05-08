




move existing diagrams and project desing into docs; review, update if anything changed

archive (move) all plans docs (exept postpone) into docs/history/MVP

remove ALL milestones mentioning from code and comments
existing features now not belong to milestones
if need to refer to some implementation step - all of them were implemented in MVP

--------------------------------------------------------------------------------

BUG: I don't see any metrics in grafana and prometheus

BUG: wake-word has unexpectedly long latency and threshold

BUG: does model-controller works? how I can see it?

--------------------------------------------------------------------------------

from now on use stadard planning schema: Milestone -> Phase -> Stage -> Task: M1 -> P1.2 -> S1.2.1 -> T1.2.1.3
    Milestones are >2 month
    Phases are 2-4 weeks
    Stages are days to weeks
    Tasks are up to 2 days
rework M9 and M10 plans into Phases or Stages in next plans, combine with new plans
start from M1
also use open questions doc to track questions

---

## Random Ideas


move config or some it's parts into db


convert plans and docs history into single-file design with history of acva creation
process it and load into memory + add memory read instructions into context
so agent can answer questions about itself


memory-personality relations - everything is optional: n-n relation


Dialog:
    - SILENCE!!!! SHUTUP!!!
    - silence is important, remove annoying "How can I help you" after every response
    - verify and eliminate hidden prompts in GGUF
    - chat template
    - logits bias
        - increase EOS probability
    - grammar constraints
        - GBNF ???
    - stop token
    - --verbose-prompt llama.cpp option
    - develop GBNF for our conversational agent
        - actions:
            - think
            - wait
            - speak
            - ignore
            - what else?
    - Clarify location and configuration of:
        - chat template
        - special tokenizer control tokens
        - GBNF and output formatting - XML,JSON
    



Can we split internal explicit LLM thinking and speaking?
Optionally enable verbal thinking



Goals:
    - ready to use and higly customizable
    - modular experimental platform
    - local-first (privacy, connection)
    - Voice UX


Modular:
    - after obvious modules - separate services (tts, stt, llm), what else?
        - audio processing
        - memory
        - tools
        - what else?
            can FSM be a module for example?
            can AEC be a module?
            can pattern builder be a module?
            what are pros/cons separating component to a module?
    - orchestrator loads configurable runtime plugins - modules implementation
    - do not mixup modularity and configurability


Sound filters cheep way to make voice more natural

SSML

artistic speaking with expression

sarcastic

compare:
    - min default config: no humanization, plain direct question-answer, default computer voice
    - max human-like config: voice, drift, background thinking, etc...


Followups:
- tools integration
    - first tool - own config modification and restart
    - knowledge graph
    - diagrams and presentations
    - quiz
    - web search
- web ui - web_ui_architecture.md
    - builtin monitoring with charts.js
- docs, readme for advanced users plus arch overview, internals in docs
- MacOS Metal
- Windows

Other potential improvements ???

all-in-one C++ app (llama.cpp + whisper.cpp + others, libs instead of servers) ???

?? Interleaving with players (music, browser, etc)

multi-user conversation
    - detect users by voice and remember
    - remember main user by voice

generate copy of user personality

multi-context with single model
multi-model runtime


multi-user conversation -> interleaved STT - solution???



game NPC character design
    - user switch talking between character, admin, other characters
    - ask characters talk to each other


different database, vector database, graph database

llama.cpp vs ollama

speeches systemd service ???


reasoning
temperature
person state from memory to promt
style post processing
drift policy
2-stage generation




Installations:
- docker
- native packages - systemd service with deps, deb, rpm, ...
- native all-in-one package - snap/flatpack/appimage with/wo servers
- ??? single all-in-one executable, run as single process

UX:
- background service + tray icon with quick small menu + web-ui
- desktop app with electron-based UI, loads server web-ui ???



CI/CD, release

gh pages


Compare with other tools
analyze target audiences, usage patterns, required functions
can:
    - user - Voice UX only
        - offline Siri/Alexa, desktop Linux
        - try new open/free models
    - advanced user - edit config via UI
        - ollama is a docker for LLMs
        - speeches is ollama for STT/TTS
        - 
    - context engineering - read session, history, dialogs, memory; edit
    - agent engineering - tools, memories, integrations
    - runtime engineering - sandbox, FSM, pipeline, modules
    - what else?


tg on-call assistent

low-latency online voice translation

