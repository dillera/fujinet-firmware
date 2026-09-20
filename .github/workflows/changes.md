# CI Trigger Changes: Path-Filtered Builds

## What changed

`autobuild.yml` and `build-fujinet-pc.yml` previously ran on **every push and
every pull request**, no matter what was in the commit. A typo fix in the
README kicked off a 10-target ESP32 PlatformIO matrix *and* a ~48-job
FujiNet-PC matrix (8 OS runners × 6 targets). Both workflows now carry `paths`
filters on their `push` and `pull_request` triggers so they only run when
files that actually feed their builds change.

`nightly.yml`, `release.yml`, and `pico-carts.yml` are untouched: the first
two are schedule/tag-driven and never fired on ordinary pushes, and
`pico-carts.yml` already had proper path filters.

## How the filters work

Both workflows use the "include everything, then negate" pattern:

```yaml
paths:
  - "**"            # start from everything...
  - "!**.md"        # ...then carve out what cannot affect the build
  - "!docs/**"
  - "!.github/**"   # blanket-exclude workflow config...
  - ".github/workflows/autobuild.yml"   # ...then re-include what DOES feed this build
```

This pattern is used instead of `paths-ignore` because later patterns
override earlier ones, which lets each workflow exclude `.github/**`
wholesale and then **re-include its own workflow file** (and, for autobuild,
the `platformio.release-*.ini` build configs). Editing a workflow still
tests that workflow.

The push and pull_request path lists inside each file are intentionally
duplicated rather than shared via YAML anchors — GitHub's anchor support in
workflows is recent, and a parse failure would silently disable all CI. Each
file carries a "keep in sync" comment marking the duplication.

## What each workflow now skips

**Both workflows skip:**

| Path | Why it can't affect the build |
|---|---|
| `**.md` | Documentation, wherever it lives |
| `docs/**`, `notes/**` | Prose only |
| `pico/**` | RP2040 cartridge trees, built by `pico-carts.yml` |
| `LICENSE` | Not compiled |
| `.gitignore`, `.clang-format`, `.dir-locals.el`, `.travis.yml` | Editor/lint/dead-CI config |
| `.github/**` (minus re-includes) | Other workflows' config |

**`autobuild.yml` (ESP32 firmware) additionally skips:**

- `components_pc/**` and `fujinet_pc.cmake` — these feed only the
  FujiNet-PC CMake build; the PlatformIO build never references them.

**`build-fujinet-pc.yml` additionally skips ESP32-only build inputs:**

- `boards/**`, `sdkconfig*`, `fujinet_partitions_*.csv`,
  `platformio-ini-files/**`

Note the asymmetry: the PC build does **not** skip `components/` because
`fujinet_pc.cmake` compiles gumbo, gumbo-query, and lz4 from that tree.

## Escape hatches and non-changes

- `autobuild.yml` gained a `workflow_dispatch` trigger, so a skipped run can
  always be launched manually from the Actions tab.
- `build-fujinet-pc.yml` already had `workflow_dispatch` and a nightly
  `schedule`; path filters do not apply to either, so the nightly
  FujiNet-PC release always builds regardless of what changed.

## Caveat for the future

`master` currently has no branch protection, so a path-skipped workflow
cannot strand a PR waiting on a required status check. If required checks
are ever added to these workflows, a docs-only PR would wait forever on a
check that never runs. The standard fix at that point is a no-op companion
workflow with the inverted path list that reports success for the same
check name.
