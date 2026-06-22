# Working in this repository (Claude / agents)

Conventions for AI agents working in this repo.

> **This is a public, open-source repository.** Everything committed — code,
> comments, commit messages, and PR descriptions — is published. Keep it
> professional and self-contained: no secrets, internal notes, or references to
> private history; document the present design and flow, not past incidents.

## Worktrees

Do multi-step work in a dedicated git worktree rather than the root checkout, which
concurrent tooling may switch to other branches. Place worktrees under
`.claude/worktrees/<branch>` (gitignored):

```sh
git fetch origin
git worktree add .claude/worktrees/<name> -b <branch> origin/main
```

- Build inside the worktree (`build/` is gitignored).
- Remove the worktree when its branch merges
  (`git worktree remove --force .claude/worktrees/<name>` if a build dir is present).

## Build & test

Full build/run playbook: [`docs/build-and-run.md`](docs/build-and-run.md). It
covers terminal builds, the committed VS Code build/debug setup
([`.vscode/launch.json`](.vscode/launch.json), [`.vscode/tasks.json`](.vscode/tasks.json)),
tests, and iOS device install/launch.

macOS (Homebrew) defaults — adjust paths for your environment:

```sh
cmake -S <worktree> -B <worktree>/build/c -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=$HOME/Qt/6.10.1/macos \
  -DOLR_BUILD_TESTS=ON
cmake --build <worktree>/build/c
```

Use a fresh build directory when switching configurations. Ninja, ffmpeg/ffprobe,
and srt-live-transmit come from Homebrew (`/opt/homebrew`).

- Unit tests — run the full suite, since a worker/dispatcher change can affect
  sibling tests:

  ```sh
  ctest --test-dir <worktree>/build/c -L unit --output-on-failure
  ```

- Playback E2E — exercises the real playback worker against a recorded fixture:

  ```sh
  ctest --test-dir <worktree>/build/c -R e2e_play --output-on-failure
  # or the driver directly:
  tests/e2e/run_playback_e2e.sh <play_harness> <record_harness> <scenario> <views> <srtPort>
  ```

  Scenarios: `play1x seekplay reverse stepscrub sliderscrub liveedge seekflash
  farback armedcut armedcut-back armedcut-seekrace armedcut-rearm-seek`. Use a
  distinct SRT port per concurrent run.

## Formatting

CI's Lint job checks clang-format on changed lines only. Format changed lines only —
do not reformat whole files (several engine files use hand-written Allman style).
Use Homebrew LLVM (Xcode ships no `clang-format`):

```sh
CF=/opt/homebrew/opt/llvm/bin/clang-format
GCF=/opt/homebrew/opt/llvm/bin/git-clang-format
python3 "$GCF" --binary "$CF" --diff --commit origin/main -- '*.cpp' '*.h'   # check
python3 "$GCF" --binary "$CF"        --commit origin/main -- '*.cpp' '*.h'   # apply (stage first)
```

`.qml` files are checked by `qmllint` (errors only), not clang-format.

## Branches & pull requests

- One branch per logical change, off current `origin/main`. Open a separate PR per
  logical unit rather than appending unrelated commits to an open one.
- After a PR merges, fetch and confirm the commits are on `main`
  (`git merge-base --is-ancestor <sha> origin/main`). Don't push to a branch whose
  PR has already merged — open a new PR instead.
- GitHub CI is the comprehensive gate: parallel macOS + Linux + Windows builds,
  the unit suite, the playback e2e, and the clang ASan/UBSan/TSan sanitizer
  passes, all cached to stay within a ~6 min wall-clock (CI is free for this
  public repo, so the budget is wall-clock, not minutes).
- The pre-push hook (`.githooks/pre-push`) is a fast LOCAL pre-flight (~5 min): a
  single-platform `-Werror` build + the delivery matrix + clang-tidy + the fast
  unit label, so a push rarely fails CI on trivia. The slower e2e + sanitizer +
  iOS passes are opt-in (`OLR_PREPUSH_FULL=1`) to reproduce a CI failure locally.
- **Never** push with `--no-verify` — it bypasses the pre-flight entirely. Skip a
  gate that genuinely cannot run on the current platform/toolchain with its flag
  (`SKIP_UNIT=1`, `SKIP_TIDY=1`, or `OLR_PREPUSH_LIGHT=1` for delivery-only; under
  `OLR_PREPUSH_FULL` also `SKIP_E2E` / `SKIP_ASAN` / `SKIP_TSAN` / `SKIP_IOS_BUILD`);
  that keeps every other gate enforced. Authenticate via gh's git credential
  helper and let the hook run:

  ```sh
  git -c credential.helper= -c credential.helper='!gh auth git-credential' \
    push -u origin <branch>
  ```

- End commit messages with `Co-Authored-By: Claude <noreply@anthropic.com>` and PR
  bodies with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.

## Verification

Verify before reporting work done: build, run the relevant tests, and confirm merges
landed. For concurrency-sensitive changes — notably the playback worker's threading —
seek an independent review before merging.
