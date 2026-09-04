# Sunrise (T4tsuk3i personal build)

Personal fork of [stanuwu/Sunrise](https://github.com/stanuwu/Sunrise), `dev` branch, built and
run for my own offline single-player use. Not published anywhere — this README documents what
*this specific build* actually does, not the generic upstream pitch.

Base version: `0.3.2.0` (matches upstream `dev`/`master` at the time this fork was cloned).

## What works right now

Verified in-game on the live deployment (`C:\Users\Tatsuya\Pictures\Destiny2-Unvaulting`):

- **Load into orbit and any destination**, explore freely (fly/noclip/activity override).
- **Character select + inventory screens**: real equipped light and emblem art render correctly
  (native family-0/family-3 pipeline; this always worked).
- **Orbit Fireteam card + Roster panel**: shows your real display name (Steam persona,
  `settings.json` → `steam.user.persona_name`), equipped emblem, and equipment light/power. This
  did *not* work out of the box — see "What I fixed" below.
- **Collections**: pull any unlocked emblem, armor, weapon, or exotic; persists across restarts.
- **Tower vendors**: purchases (buy from a sale row), quests (grant + banner retirement), bounty
  rolls (Shaxx/Drifter confirmed, rules also authored for Banshee-44/Zavala/Eva/Prismatic
  Recaster/Saint-14), and recycling (Drifter synths, Rahool's 277 shaders) all actually grant/pay
  out now instead of being silently refused.

## What doesn't work / is parked

- **NPCs don't physically spawn.** Nothing does — no enemies, doors, plates, or vendor bodies.
  Root cause is known (the client's entity free-slot bitmap is never stocked under Sunrise's host
  role), a fix exists and installs cleanly, but it stalls the main loop on this account when
  entering a destination. Source is present (`client/diagnostics/entity_create_probe.*`) but
  **deliberately not wired into the boot sequence** until it can be debugged properly (needs an
  actual debugger, not more blind log-and-reboot cycles). This is why Tower vendors are only
  reachable indirectly right now, and why some encounters (confirmed: Prophecy's entry room) never
  open their first door.
- **Multiplayer fireteam** (seeing another real player's card, actual co-op). The peer-to-peer
  group-session protocol exists in the codebase as unexercised scaffolding
  (`middleware/gameplay/group`, `server/gameplay/group`) but nothing here has ever driven it
  end-to-end. Long-term, not a near-term goal.

See `C:\Users\Tatsuya\Pictures\d2\Sunrise\docs\feature-tracking.md` for the detailed session log,
including exactly which upstream PRs were ported in and how the roster power-field offset was
found.

## What I fixed to get here (from stock upstream `dev`)

- The orbit banner hook (`banner_bind.cpp`) existed fully implemented but nothing ever called
  `install()` on it — wired it into the boot sequence.
- A build-data cache the reader marks `invalid` (as opposed to merely `missing`/`stale`) aborted
  boot outright instead of rebuilding; now treated the same as a missing cache.
- Ported upstream PR #80 (family-two "social roster" object — Roster/Fireteam name + emblem).
- Found and wired in the roster panel's power field (member record offset +20, a 32-bit light
  value) — not covered by PR #80, no existing reference for it anywhere.
- Ported upstream PR #87 (vendor purchases/quests/bounties/recycling, vendor-by-hash catalog fix,
  pursuit hold/discard).
- Added reason logging to several previously-silent validation/preflight failure paths
  (`account_state.cpp`, `build_data_runtime.cpp`, `queuez_character_staging.cpp`) — these used to
  fail with no indication of why, which is most of why the above took as long as it did.

## Deploying a new build

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  "Sunrise.sln" /p:Configuration=Release /p:Platform=x64 /m
```

Output: `build\x64\Release\steam_api64.dll`. Copy over
`C:\Users\Tatsuya\Pictures\Destiny2-Unvaulting\bin\x64\steam_api64.dll` — **back up the existing
one first** (`steam_api64.dll.bak-<timestamp>`; a build confirmed working in-game gets
`-confirmed-working-<commit>` appended). The four vendor rule files
(`Sunrise/resources/vendor_rules/*.txt`) must also be present in
`Destiny2-Unvaulting\bin\x64\Sunrise\` alongside `settings.json` — they're data, not compiled in.

Toolchain: Visual Studio 2026, `v145` platform toolset, Windows SDK `10.0.26100.0`, C++20.

## Original upstream README

Kept below for reference (build/RE credits, legal disclaimers) — none of the "install this
yourself" framing applies since this build isn't distributed.

- [Install Instructions](https://github.com/stanuwu/Sunrise/wiki/Installing)
- [FAQ](https://github.com/stanuwu/Sunrise/wiki/FAQ)
- [Common Issues](https://github.com/stanuwu/Sunrise/wiki/Common-Issues)
- [Discord](https://discord.gg/22JS6et5k9)

### Credits

Dependencies: [imgui](https://github.com/ocornut/imgui),
[detours](https://github.com/microsoft/detours).

Artwork: [Solus](https://www.youtube.com/@Solus-yt).

Testing: [Ferr](https://x.com/light_fades_awy), [gage](https://x.com/_Quolu_),
[Jenka](https://youtube.com/@jenkad2oob?si=OQpCGeBCEJBS0zHx), [Katie](https://github.com/Confetti3),
[Kody Ivie](https://x.com/Kody_Ivie), [Solus](https://www.youtube.com/@Solus-yt), Breshi,
[Deltadog55](https://www.youtube.com/@deltadog55), Moosh,
[MoveableFormula](https://youtube.com/@movableformula), Z, The Cube17.

Inspiration/helpful repos: [tiger-pkg](https://github.com/v4nguard/tiger-pkg),
[alkahest](https://github.com/cohaereo/alkahest),
[tachyscope](https://codeberg.org/V4NGUARD/tachyscope),
[D2TagParser](https://github.com/MontagueM/D2TagParser),
[DestinyUnpackerCPP](https://github.com/MontagueM/DestinyUnpackerCPP),
[D2TextureRipper](https://github.com/nblockbuster/D2TextureRipper),
[tiger-parse](https://github.com/v4nguard/tiger-parse),
[demonware-cod4](https://github.com/Demonware-Custom-Server/demonware-cod4),
[demonware-companion](https://github.com/hosseinpourziyaie/demonware-companion),
[demonbugger](https://github.com/jordam/demonbugger),
[shield-development](https://github.com/project-bo4/shield-development),
[Charm](https://github.com/MontagueM/Charm), [quicktag](https://github.com/v4nguard/quicktag),
[D2StaticDocs](https://github.com/nblockbuster/D2StaticDocs),
[D2Maps](https://github.com/MontagueM/D2Maps),
[DestinyMapmining](https://github.com/MontagueM/DestinyMapmining),
[tachyscope](https://github.com/nblockbuster/tachyscope),
[destinydocs](https://github.com/cohaereo/destinydocs),
[DestinyUnpacker](https://github.com/MontagueM/DestinyUnpacker),
[bungie-lua-decompiler](https://github.com/nblockbuster/bungie-lua-decompiler).

Other: [Ginsor](https://x.com/GinsorKR) — useful pointers.

### Disclaimers

Sunrise is not a crack, a cheat, or a custom server. Everyone needs their own copy of the game; the
mod runs entirely locally and connects to no servers. Not affiliated with Bungie or Sony.

AI was used extensively in this fork's development — for RE, development, and documentation. Not
for art or creative writing. AI is a tool; the user is responsible for the results it produces.
