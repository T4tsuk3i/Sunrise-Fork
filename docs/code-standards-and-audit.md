# Sunrise: upstream code standards + audit of commit `83a5f01`

## Context

We want our fork's code to be upstream-quality — not because we're PRing today, but because
work that isn't PR-able tends to be work that rots. Two questions prompted this:

1. Why does stanuwu reject hardcoded RVAs, and what does he actually consider good code?
2. Was the "Armor 3.0" work in commit `83a5f01` good?

This is a reference, not a proposal — it records the standard so it's knowable without
re-deriving it, and records which parts of `83a5f01` stay PR-able later. No source files were
changed to produce it.

---

## Part A — The standard

### A1. The authoritative rules (upstream `README.md`, Contributing section)

- **No Copyrighted Data** — all game data extracted at runtime
- **Code Formatting** — stick to the provided clang-format and clang-tidy configs
- **Clean Code** — readable high-quality code, follow the project's existing style
- **Provide Documentation** — explain what you changed, why you changed it
- **One Feature** — do not put multiple features into one PR
- **Complete Implementations** — do not PR features that are not completed
- **Server Focus** — for server features, don't abuse client patches

Also: *"Issues are for bug reports only. PRs are for pull requests only."*

### A2. Mechanically enforced

| Control | Effect |
|---|---|
| `.clang-format` | LLVM base, C++20, 100 col, 4 spaces, no tabs, attach braces, `NamespaceIndentation: None`, `PointerAlignment: Left`, `BinPackArguments/Parameters: false`, `SortIncludes: CaseSensitive`, `IncludeBlocks: Regroup` — `<Windows.h>` priority 1, other `<>` 2, quoted 3 |
| `.clang-tidy` | `WarningsAsErrors: '*'` — every finding is an error |
| `.clang-tidy` `ExtraArgsBefore: -Wdocumentation` | Doxygen tags must match real parameters. Docs are compiler-checked. |
| `.clang-tidy` `portability-restrict-system-includes` | Explicit header allowlist. A new `#include <...>` not on it is an error. **No `<string>`, `<vector>`, `<map>`, `<functional>`, `<iostream>`** — fixed storage only. |
| `Sunrise.vcxproj` | v145, `/W4 /WX /permissive- /utf-8`, `stdcpp20` |
| CI (`.github/workflows/build.yml`) | Build only. **No lint job, no format check, no tests.** |

### A3. House idioms (observed across the tree)

- **Naming**: constants `kPascalCase`; functions `snake_case`; types `PascalCase`; enumerators
  `camelCase`; struct fields `camelCase`; file-scope globals `g_camelCase`; namespaces
  `snake_case` mirroring the directory path exactly.
- **Every non-void function** is `[[nodiscard]]` and `noexcept`. Deliberate discards are
  `(void)expr`.
- **Errors are `bool` + out-param.** Zero `throw`, zero `catch` anywhere in the codebase. Richer
  failure detail goes in a separate `enum class Failure` + accessor, not the return value.
- **Most `.cpp` files** wrap internals in an anonymous namespace; file-scope `static` is not used.
- **Doxygen on everything** including individual constants and enumerators. The voice explains
  *why*, in present tense, and states the consequence of the alternative.
- **Logging is the test suite.** Format: `ev=<subsystem> stage=<step> [reason=<why>]
  result=<outcome> [key=value ...]`. Log volume is **always** capped — a `std::atomic_bool`
  latch, a `kMaxReports` constant, or a re-arm interval.
- **Magic numbers**: every literal that isn't 0/1/an index gets a named `constexpr` *and* a
  doc comment saying **where the number came from** (which instruction, which dump). Derived
  values are computed from the base constant and guarded with `static_assert`.
- **No tests exist and none are expected.** `AGENTS.md`: *"No test suite. Verification is build
  + deploy + user runs the game."* Compile-time checking substitutes: `consteval` signature
  compilation traps a malformed pattern as a link error.
- **Settings** are a 5-step ritual: field with brace default in `definition.h` → named bounds
  constants → parser arm with a `has*` latch validating into a `candidate` copy → `snake_case`
  JSON key ↔ `camelCase` field → add to `resources/default_settings.json`. A changed value shape
  needs a row in `settings_upgrade.cpp`'s `kReplacedMembers`.
- **Re-declaration over shared headers, in places.** Small self-contained constants used in a few
  spots (FNV-1a's offset basis/prime, a `uint32` bound check) are frequently redeclared locally
  in each file rather than pulled from one shared header — a real, observed pattern in this
  codebase, not an oversight to "fix" on sight.

---

## Part B — Why hardcoded RVAs get rejected

An RVA is a position-dependent fact about one binary. Any recompile, patch or relocation moves
it, and the code then reads **wrong memory silently** — no failure, just corruption. A byte
signature matches the instruction stream, survives address changes, and *reports* when it stops
matching.

The project has infrastructure for exactly this, and a stated rule for choosing between its two
halves:

- **Shared registry sweep** — `client/patterns/registry.{h,cpp}`, `game.h`, `game_signatures.cpp`.
  For targets a whole subsystem depends on.
- **Local one-shot scan** — `client/patterns/image_scan.h`, which states it verbatim:
  *"A single-hook target belongs here, not in the shared sweep: one miss there kills the group."*

Addresses are never stored — RIP-relative operands are decoded at runtime
(`client/targets/game/relative.h`), with the canonical comment in `fade_release.cpp`: *"The
address comes from that instruction's own operand, not from a stored offset."*

**The clinching evidence** — two PRs implemented the same seven functions two ways:

| PR #46 — rejected | PR #58 — patterns |
|---|---|
| `kPlacementInitializeRva = 0x4B25F0;` | `kPlacementInitializeText = "89 54 24 10 53 48 83 EC 20 …"` |
| 7 function RVAs, no signature scanning at all | 6 signatures via the shared `patterns/` infra |

stanuwu on #46: *"2 Major Issues for Merge: Hardcoded RVAs instead of patterns / Extraction does
not fit in with the existing stack"*, then after revision: *"Why did you add more RVAs?? Use
patterns."*

**Calibration:** across the whole tree there are exactly **2 hardcoded RVA constants, in 1 file**
(`entity_create_probe.cpp`). And that file is the exception that proves the rule — it resolves
its *function* target by signature, falls back to RVAs only for two data pointers no signature
reaches, validates its expected record stride at runtime *"because a wrong one would read foreign
memory,"* documents the disassembly provenance, and is deliberately parked out of the build path.
Treat the rule as **"signatures, always."**

**Worth knowing:** most upstream rejections are *scope*, not quality — several PRs were closed
"not planned" outright. One contributor writing a style guide got *"I dont think its relevant for
someone who has not made a meaningful contribution to write a contribution guide."* Quality
critique is rarer than direction critique.

---

## Part C — Audit of `83a5f01`

`feat: Armor 3.0 build-data stat layer, item armor meta threading, and stat diagnostics`
— 24 files, +2160/−11.

**Verdict: high craft, largely unshippable content.** The prose and documentation discipline
match the house style closely. What fails is *what was built*, not *how it reads* — and because
the scaffolding is written in the same confident, fully-documented voice as production code, it
is harder to spot in review, not easier.

### C1. Genuinely good — keep

- Doxygen coverage near-total with real rationale; constants named; settings block is the
  best-documented part of the change.
- `inventory_parser.cpp` armor keys follow the file's existing pattern exactly, backward
  compatible. No issues found.
- `loadout_item_resolver.cpp` — the "no definition" sentinel translates to a single canonical
  value so "none" has one meaning downstream. Correct and well commented.
- `state_persistence.cpp` — armor meta read/write round-trips; the writer emits keys only when
  non-default, so old saves stay byte-identical.
- Selected-character gating on the probes (fixes diagnostics that silently reported the wrong
  character).
- `stat_block_scan.cpp` resolves the module base at runtime and reports offsets *relative* to it
  — the opposite of the RVA sin.

### C2. Fails the stated rules

| Rule | Failure |
|---|---|
| **One Feature** | Armor 3.0 layer + armor-meta threading + 4 diagnostic probes + a memory scanner, in one commit |
| **Complete Implementations** | The armor definition table (archetypes, gear tiers, masterwork) has **zero call sites**. A tuned per-point damage/scaling struct is never read. The armor-meta struct is **write-only**: its only reader is a log line. A large share of the "stat layer" is inert. |
| **Clean Code** | `investment_dump.cpp` hardcodes a list of six definition indices captured from one session on one machine, comment says "tonight" |
| **Clean Code** | `stat_block_scan.cpp` reimplements a module-range helper that already exists in the same directory with two existing callers |
| **Clean Code** | `inventory_state.h` named its bounds constants `kArmor*None` while the comment says zero is unset and the constant is actually the highest valid one-based number — read as the opposite of its meaning. **Fixed** (renamed to `kArmor*Maximum`) as part of the easy-cleanup pass following this audit. |
| **Code Formatting** | `stat_block_scan.cpp` was the only file writing `<windows.h>` not `<Windows.h>` — breaks the case-sensitive include-priority regex |
| **Code Formatting** | `stat_block_scan.cpp` includes `<psapi.h>`, not on the clang-tidy allowlist → error under `WarningsAsErrors: '*'` |

### C3. Smaller findings

- `character_appearance_banks.cpp` had a signed/unsigned comparison (`int` accumulator compared
  against `.size()`) at two sites. **Fixed** as part of the easy-cleanup pass.
- `count_hashes_filled` written, then the loop immediately below reimplements it inline.
- A bounds check on `armor_set_hash` was spelled `0xFFFFFFFFULL` in `state_persistence.cpp`
  while the identical check in `inventory_parser.cpp` used
  `(std::numeric_limits<std::uint32_t>::max)()` — same check, two spellings. **Fixed** (matched
  to the `numeric_limits` spelling) as part of the easy-cleanup pass.
- 1 MB function-local `static std::array` in BSS for a disabled feature.
- Several new settings keys were absent from `resources/default_settings.json` (step 5 of the
  ritual). **Fixed** as part of the easy-cleanup pass — the client settings block now enumerates
  every field the struct carries, including the diagnostic-only ones, with their code-level
  defaults spelled out.
- `investment_dump` had **no off switch** and ran on every investment refresh for every user.
  **Fixed**: gated behind a new `investment_dump` setting, off by default.
- Missing `@param` tags on several probes — would fail `-Wdocumentation`.
- Datagen consistency: two touched files in the same commit gained logging while the datagen
  layer's established style elsewhere is silent, pure encoding.
- A row-index mapping is documented candidly as *"data to adjust, not a hardcoded assumption"* —
  an unverified guess with no test and no bounds assertion. Honest, but load-bearing.

### C4. What stays PR-able later

These are the clean single-feature candidates, each extractable without the scaffolding:

1. **ArmorMeta persistence** (`state_persistence.cpp` reader + writer) — self-contained, fixes a
   real "parseable but not persistable" gap. Needs the armor-meta consumer to exist first,
   otherwise it fails *Complete Implementations*.
2. **The sentinel translation** (`loadout_item_resolver.cpp`) — smallest, cleanest, genuinely a
   bug fix.
3. **Selected-character gating** — only meaningful alongside diagnostics, which upstream would
   not take. Probably fork-only.

Anything shipping the armor definition table, the unread stat-scaling struct, `stat_block_scan.*`
or `investment_dump.*` would need those either given a consumer or removed first.
