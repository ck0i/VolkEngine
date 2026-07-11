# Contributing to VolkEngine

VolkEngine accepts focused changes that improve renderer or engine behavior without obscuring ownership, synchronization, lifetime, or runtime cost.

## Before coding

Open an issue or discussion for non-trivial work. Include:

- the problem and affected subsystem;
- proposed scope and exclusions;
- important API, ownership, and synchronization decisions;
- expected CPU, GPU, memory, build, or dependency cost;
- validation commands and likely failure modes.

Wait for approval or assignment before implementing large features or architectural changes. Roadmap entries describe direction; they are not claimable tasks.

Small bug fixes and documentation corrections may go directly to a pull request when their scope is obvious.

## Implementation rules

- Follow existing subsystem boundaries and naming.
- Keep Vulkan ownership and synchronization visible.
- Avoid per-frame allocation, repeated resource creation, unnecessary copies, and queue stalls.
- Update all affected callers and remove replaced code. Do not add compatibility layers before 1.0 unless requested.
- Keep unrelated formatting, renaming, and cleanup out of the change.
- Add dependencies only when their license, maintenance, build, binary-size, and runtime costs are justified.
- Do not submit generated or copied code that you cannot explain and maintain.

Prefer a small complete change over a broad partial one.

## Validation

Use the narrowest checks that cover the behavior. A typical Linux debug run is:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Windows uses the corresponding `windows-debug` preset.

Renderer changes also need a relevant live smoke run, for example:

```sh
./out/build/linux-debug/VolkEngineSandbox --frames 120 --resize-smoke
```

Add evidence appropriate to the change:

- validation-layer output for Vulkan lifetime or synchronization work;
- screenshots or image comparisons for visible changes;
- RenderDoc notes for pass, barrier, or attachment changes;
- CPU/GPU timing distributions and exact hardware for performance claims;
- release-build results when optimization or configuration affects release behavior;
- platform-specific results for platform code.

Do not claim a rendering or performance improvement without measurement.

## Pull requests

A pull request should contain:

1. a link to the approved issue or discussion when required;
2. a short explanation of the problem and solution;
3. the important files or contracts changed;
4. exact validation commands and results;
5. measurements, captures, or screenshots when relevant;
6. known limitations.

Keep each pull request to one coherent change. Review may require narrower scope, fewer abstractions, clearer ownership, stronger failure handling, or better evidence even when CI passes.

## Documentation

Update documentation only when a public contract, subsystem boundary, workflow, or roadmap status changes. Headers remain authoritative for signatures and field lists; docs should explain invariants and usage rather than mirror declarations.

Start with [ROADMAP.md](ROADMAP.md) and [docs/README.md](docs/README.md).

## AI-assisted work

Disclose material AI assistance in the pull request. The contributor remains responsible for every changed line, must be able to explain the design, and must provide the same tests and measurements as any other contribution. Remove invented APIs, placeholders, generic boilerplate, and explanations that do not match the implementation.

## Review priorities

When tradeoffs conflict, prefer:

1. correctness and debuggability;
2. explicit ownership and synchronization;
3. measured performance;
4. narrow changes;
5. maintainability.
