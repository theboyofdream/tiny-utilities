# PRINCIPLES

Inferred from the decisions recorded in `docs/JOURNEY.md` (append-only). Maintained by the author — that file is read-only history; this file is the editable digest. Agents: treat these as tie-breakers whenever a spec (PRD §1, tool docs) doesn't answer the question.

## 1. Machine efficiency is the product

Every tool exists because doing the thing manually — or with heavyweight alternatives — costs time or attention every day. A tool that doesn't pay for itself isn't built. Scope-creep reverts are normal and cheap (OCR v1, native Windows OCR API, the ip-send GUI rewrite): if a capability doesn't earn its keep, it goes.

## 2. Efficiency runs both directions

What a tool saves must exceed what it costs: RAM, resident processes, launch latency, and *attention* (knobs to configure, config files to maintain, flags to remember, cleanup after use). Idle footprint = zero. Exit = zero residue (0 processes, no hooks left behind, no temp files). A tool that is useless without a service is not accepted — exceptions are granted explicitly (PRD §21), never assumed.

## 3. Nothing works unless proven

Measured (KB / MiB / physical inches / ms / process counts) or smoke-tested (double-launch toggle → 0 processes; adversarial inputs: bad keys, bogus PIDs, vanished files). "Should work" = unfinished. Refactors must be behavior-identical and re-verified byte-for-byte; log lines that were byte-identical stay byte-identical. Unverifiable now (hardware, second machine, real phone) → recorded open item, never claimed done.

## 4. Lower cognitive load beats more features

Compile-time defaults with obvious names over flags (`kyper.local`, "no CLI surface added — keeps mental overhead at zero"). No config to maintain. No unsolicited UI (API-only verdicts, no HTML). Consistency is a contract, not a preference: same key = same behavior in every tool (`Ctrl±` zoom, the `capture --window` picker reused everywhere, one mDNS convention, one IPC toggle, one `--help`). When a convention appears a second time it becomes a shared header, not a copy. Fewer knobs wins: dead flags are deleted (`--group`, sort `mru`, `dim`, `tile`), duplicate aliases merged.

## 5. The user decides; evidence decides the details

No auto-action on the user's behalf without confirmation (no auto-select, no implicit sends, no focus stealing). Silent success, loud failure: exit code `0` on success with no ceremony, real errors on `stderr`/dialogs — "Sent successfully!" was hardcoded is a bug. Trade-offs are written into the log with every substantial decision. Experiments are preserved (`*.bak`, `*.rewrite`) and history is append-only — but anything kept must beat the incumbent in a measured way.
