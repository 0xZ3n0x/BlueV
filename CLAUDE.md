# BlueV: rules for Claude Code

`src/ARCHITECTURE.md` explains how the program is built and what was learned about the console. Read the section you are touching before changing it, and keep it true: when code changes, change the text that describes it in the same change.

Everything in the repo is in English: code, identifiers, comments, tests, docs, log messages, commit messages and pull request text.

## Layout and layers

- Dependency rule: `app -> service -> core`, `platform -> core/ports`. `core` never includes `platform` or `service`.
- Everything that touches the console lives in `src/platform/ps5/`. `core` has no console header.
- C++20, no exceptions, no RTTI. Naming: classes `PascalCase`, methods `camelCase`, members `m_snake_case`, namespace `bluev`.
- Format with `clang-format` (`.clang-format`: Microsoft style, pointers left, braces always) on everything except `third_party`.

## Keep it simple

The codebase is being de-engineered step by step. Do not add layers.

- An interface exists only where a test fakes something it cannot run (the controller, the console, time, files). Judge a new one by "is there a real fake in a test?". Otherwise use a plain function, `std::function` or a concrete class.
- Prefer early returns. Code only `main.cpp` uses goes into `main.cpp` or a small file under `platform/ps5`.
- Do not add options, wrappers or helpers for a single caller, and do not add error handling for cases that cannot happen.
- No personal data in code, comments, tests, docs, logs or commit messages: no real names, e-mail addresses, home directory paths, user names, device serials, Bluetooth addresses of real devices or the addresses of someone's own network. Use made-up values in tests and examples (`192.168.1.20`, `AA:BB:CC:DD:EE:FF`). Only the copyright holder line in `README.md` and `LICENSE` names a person.
- Comments follow the surrounding code: short, plain English, explaining why or what the bytes mean, not narrating the code.

## Build and test

- Host tests: `BLUEV_BUILD_DIR=<scratch dir>/host sh tests/run.sh plain` (also `thread`, `address`; an optional gtest filter as the next argument).
- Console build: `make -j8` (SDK in `/opt/ps5-payload-sdk`), output `build/bluev.elf`.
- Host tests do not compile `src/platform/ps5` or `src/app/main.cpp`. Only the console `make` and the console itself check those, so say so when a change touches them and has not been run on the console.
- Only send the elf to the console (`nc --send-only <console-ip> 9021 < build/bluev.elf`) when asked. The web page is `http://<console-ip>:7733`.

## Git

- Commit messages are in English: an imperative subject line without a trailing period, then a short body saying what and why when it is not obvious.
- No `Co-Authored-By` line and no "Generated with Claude Code" line in commit messages or pull request descriptions. Claude's name does not appear in them.
- Do not commit or push unless asked. Never force-push without saying so first; use `--force-with-lease`.
- GitHub flow: `main` is always releasable. Every change is made on a short-lived branch started from an up-to-date `main`, named after the work (`fix/xm5-sdp-disconnect`, `ci/release-workflow`), and goes into `main` through a pull request. Do not commit to `main` directly unless the user asks for it.
- Pull requests go to `main`. Delete the branch once it is merged. A release is a version tag on `main`.
- Merging a pull request: squash is the usual choice. When a branch holds many changes that are worth telling apart in the history, keep its commits instead. Suggest one, the user decides.

## Reporting

- Say what was verified (host tests, console build, run on the console) and what was not. Do not claim a console behaviour that was not run.
