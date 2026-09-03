# LSP Faithful Port — scope.md

Append-only. Do not edit or delete existing entries — add a new dated entry if
something changes.

## 2026-09-04 — why this file exists

Root cause found tonight, with hard evidence (not guessed): the current
`lsp_wrap.cpp` (Gate/Compressor/Limiter) is a hand-rolled wrapper around the
bare `lsp::dspu::Gate`/`Compressor`/`Limiter` DSP-primitive classes, not the
real LSP plugins. This has now happened **five times** — every previous round
asked for "the plugins ported faithfully" and got a hand-wrap instead, because
the actual task spec handed to the execution stage (`lsp-gate/lsp-compressor/
lsp-limiter/lsp-mb-compressor/AGENT_INSTRUCTIONS.md`, written 2026-08-05
23:20) literally said "read the real plugin for reference, then write a new
minimal wrapper" — not "port it." A session on 2026-08-07 found this exact
same bug, correctly diagnosed it, and asked whether to redo it properly — that
correction never actually happened, and the same hand-wrap has shipped
unchanged in every OTA since.

**This file supersedes those four `AGENT_INSTRUCTIONS.md` files. Do not follow
them — they contain the exact mistake being corrected here.**

## Hard rule for every module below

**Port the real algorithm. Do not write a new implementation from scratch,
even one that "does the same thing."** Concretely:
- `update_settings()` and `process()` (and every private method they call)
  from the real `plug/<module>.cpp` are the actual DSP logic. Carry that logic
  over close to verbatim — same variable names where reasonable, same order
  of operations, same envelope/filter/gain-stage math. This is a **transliteration
  into JamesDSP's calling convention**, not a rewrite from understanding.
- The **only** things to strip are the parts that exist purely because LSP
  plugins run inside a full plugin host: `plug::IPort*` member fields, the
  `pXxx->value()` reads (replace with JamesDSP's own config struct fields),
  `plug::Factory`/metadata registration, `IDisplay`/mesh/graph-visualization
  code (`vCurve`, `pIDisplay`, oscilloscope-graph buffers), and UI-only
  metering ports. If you're not sure whether something is host-plumbing or
  real DSP, it's real DSP — keep it and ask rather than guessing it away.
- Acceptance test (mechanical, not a self-report): after porting, `grep` the
  new implementation file for the real plugin's actual internal field/method
  names (e.g. Gate's `sPremix`, `vChannels`, `update_premix`; Limiter's
  `sOver`, `sScOver`, `get_oversampling_mode`). If those names aren't present,
  it's not a port — it's another wrapper, and that's a failure, not "close
  enough."

## Explicit scope decision for tonight — mono/stereo only, no external sidechain UI

JamesDSP's current AIDL command surface (`cmd 10100`/`10101`, `jamesdsp.c`)
only ever sends a stereo pair with no mode selector and no external-sidechain
routing. Rather than block on wiring a whole new UI for LR/MS modes and
external sidechain sourcing that nothing currently uses:

- **Port stereo mode only** (Gate's `GM_STEREO`, Compressor's `CM_STEREO`).
  Mono/LR/MS variants are real code paths in the reference plugins — leave
  them out of the initial port rather than guessing at them, don't delete
  the reference source.
- **Internal sidechain only** (the module's own input feeds its own
  detector — this already matches current JamesDSP behavior). Skip the
  `bSidechain`/external-sidechain-source plumbing and Gate's premix matrix
  (`sPremix`) — that's real host-routing capability current JamesDSP has no
  UI for, not something to fake.
- **Limiter's oversampler (`sOver`/`sScOver`) stays in scope** — that's real
  DSP quality (true-peak limiting), not a host feature, and JamesDSP's own
  standing goal is pristine audio quality. Port it.
- If any of these scope calls turn out wrong once you're reading the real
  code, stop and flag it rather than silently deciding differently — this is
  exactly the kind of thing that's supposed to get caught before code ships,
  not after.

## Per-module starting points

**Gate** — `app/src/main/cpp/lsp-gate/src/main/plug/gate.cpp` (1404 lines).
Core: `update_settings()` (:630, calls `update_premix()` at :620 — premix
itself out of scope per above, but read it to understand what stays),
`process()` (:870–end). Header: `include/private/plugins/gate.h`.
Current hand-wrap being replaced: `lsp_wrap.cpp`'s `GateHandle`/
`lsp_gate_*` functions (~lines 254–360), called from `jdspController.c`'s
`w_gate_lsp()` and `jamesdsp.c`'s cmd-10100 paths — keep those call sites'
signatures identical (`lsp_gate_create/destroy/configure/process`,
`(void*, float open_db, float close_db, float attack_ms, float release_ms,
float hold_ms, float reduction_db)` / `(void*, float*, float*, unsigned,
int enabled)`), just make the implementation behind them the real port.

**Compressor** — `app/src/main/cpp/lsp-compressor/src/main/plug/
compressor.cpp` (1515 lines). Core: `update_settings()` (:605),
`process()` (:898–end). Same replacement target: `lsp_wrap.cpp`'s
`CompHandle`/`lsp_comp_*` functions, same call-site signatures to preserve
in `jdspController.c`'s `w_mb_dyn()` and `jamesdsp.c`'s cmd-10101 paths.

**Limiter** — `app/src/main/cpp/lsp-limiter/src/main/plug/limiter.cpp`
(1242 lines). Core: `update_settings()` (:570, includes oversampler setup
at :617/:622/:646), `process()` (:779–end). Replacement target:
`lsp_wrap.cpp`'s `LimiterHandle`/`lsp_limiter_*`, call sites in
`jdspController.c` (`lsp_limiter_process(jdsp->limiter_handle, ...)`, called
from the tail/last-line-of-defense position, not `chain_register`) and
`jamesdsp.c`'s cmd-1500 path.

**MB-Compressor** — explicitly **out of scope tonight**. It doesn't exist as
a real feature yet (was scoped 2026-08-06 as a future opt-in, separate from
single-band, never built) — this isn't a hand-wrap-vs-port question for that
one, it's genuinely unstarted work. Don't touch `lsp-mb-compressor/` tonight.

## 2026-09-04 (same night) — file isolation for parallel execution

Dispatched three subagents in parallel instead of sequentially, per the
user's suggestion: each owns its own file so there's no shared-file conflict
and no need to wait on each other. Gate edits `lsp_wrap.cpp`'s existing
Gate section in place (it started first, before this decision, and was
already scoped to touch only its own section). Compressor and Limiter each
write into a brand-new self-contained file (`lsp_wrap_compressor.cpp`,
`lsp_wrap_limiter.cpp`) instead of touching the shared file at all. Neither
subagent wires its own file into `CMakeLists.txt` or touches
`jdspController.c`/`jamesdsp.c` — that assembly step happens after, done
directly (not by another agent), once each port is mechanically verified
against the real reference source. Gate's code will likely get relocated
out of the shared `lsp_wrap.cpp` into its own file too, for consistency,
as part of that same assembly pass.

## Build integration

`lsp-dsp-units` (the primitive classes: `Sidechain`, `Bypass`, `Delay`,
`Oversampler`, etc.) is already cloned and already in `CMakeLists.txt` — the
real plugins' `process()`/`update_settings()` bodies call into these same
classes, just with real orchestration logic around them instead of a thin
wrapper's guess at that orchestration. `lsp-gate`/`lsp-compressor`/
`lsp-limiter` themselves are NOT yet in `CMakeLists.txt` — wiring the ported
`.cpp` (not the whole plugin file — the extracted/transliterated core) into
the build is part of this task, matching the general shape (not the literal
wrapper instruction) of the old `AGENT_INSTRUCTIONS.md` step 2.
