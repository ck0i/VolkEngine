# Contributing to VolkEngine

VolkEngine is a C++23 Vulkan game-engine foundation for high-fidelity real-time rendering with explicit performance constraints. Contributions must protect that goal: graphical fidelity without compromising performance.

We want contributions that solve real problems and can be maintained after they merge. Please avoid unreviewed code drops, speculative rewrites, broad cleanups, or AI-assisted patches you cannot fully explain, validate, and support through review.

## Contribution gate

Before opening a non-trivial pull request, open an issue or discussion proposal and wait for maintainer approval or assignment.

A useful proposal includes:

1. **Problem** — the concrete engine, renderer, tooling, asset, or documentation problem being solved.
2. **Why it matters** — how the work improves fidelity, performance, correctness, debuggability, portability, or contributor onboarding.
3. **Scope** — the files/subsystems expected to change and what is explicitly out of scope.
4. **Design** — the intended approach, important API/lifetime/synchronization choices, and known alternatives.
5. **Performance impact** — expected CPU/GPU/memory/latency impact, including why added complexity is justified.
6. **Validation plan** — exact build, test, smoke, benchmark, profiler, or RenderDoc evidence you will provide.
7. **Risks** — likely failure modes, platform assumptions, and compatibility concerns.

Large work should wait for acceptance; unapproved PRs may be redirected back to proposal or discussion even when the code builds.

## What is valuable here

Good contributions usually do at least one of these:

- Fix a real correctness bug with a minimal, source-level fix.
- Improve Vulkan renderer behavior without hiding synchronization, lifetime, or performance costs.
- Add measured performance work that removes hot-path allocation, redundant work, excessive synchronization, avoidable queue stalls, or unnecessary CPU/GPU traffic.
- Improve diagnostics that make renderer state, GPU capabilities, resource lifetime, or timing easier to understand.
- Extend engine architecture only where an existing seam already needs it.
- Add tests or smoke coverage that defend real behavior and regressions.
- Improve documentation that helps contributors make correct engine changes.

Good contributions are narrow, explainable, tested, and consistent with the existing architecture.

## Common reasons for revision or rejection

Maintainers may ask you to narrow, justify, test, or rework a PR when it does not yet fit the project. Common reasons include:

- Non-trivial work opened without prior approval.
- Formatting-only, rename-only, style-only, or churn-heavy changes.
- Generic "cleanup" that does not remove a real maintenance or correctness problem.
- Premature abstraction layers, engine-wide rewrites, or API generalization without a concrete current need.
- Renderer changes that make synchronization, ownership, or lifetime less explicit.
- Performance claims without measurements or reproducible evidence.
- Visual/fidelity changes without screenshots, rationale, and regression notes.
- New dependencies without a clear maintenance, build, license, and performance argument.
- Generated code, copied code, or AI-assisted code the contributor cannot explain and defend.
- PRs that ignore the documented architecture, renderer behavior, or performance model.

A technically valid PR may still need revision if the value, scope, or fit is unclear.

## Required process

### 1. Read the current docs

Before proposing code, read the current project documentation relevant to the area you want to change. The docs may move as the engine evolves, so use the repository's current documentation index and nearby subsystem notes instead of relying on a fixed file list.

Your proposal and PR should use the same vocabulary and constraints as the current docs.

### 2. Open a proposal before implementation

Use the contribution-gate checklist above. Keep the scope small enough to review. If the issue is exploratory, say what evidence you will gather before changing code.

Wait for one of these maintainer responses:

- **Approved** — you may implement the agreed scope.
- **Assigned** — you are the expected owner for that scope.
- **Needs design changes** — revise the proposal before coding.
- **Rejected / not now** — do not open a PR for that work.

### 3. Implement the agreed scope carefully

During implementation:

- Stay inside the approved scope. If the design needs to change, update the proposal instead of silently expanding the PR.
- Use straightforward code by default. If the accepted solution needs a less obvious approach, explain why it is safe, maintainable, and worth the complexity.
- Keep Vulkan ownership, synchronization, and lifetime visible.
- Avoid hot-path heap allocation, repeated resource creation, avoidable copies, and hidden work.
- Preserve existing command-line flags, diagnostics, and smoke paths unless the approved proposal says otherwise.
- Update every affected callsite; do not leave compatibility shims or dead paths unless explicitly requested.
- Delete obsolete code when the new path fully replaces it.
- Keep unrelated formatting and cleanup out of the PR.

### 4. Validate before opening the PR

Every PR needs evidence. Use the narrowest commands that cover the change, and include exact commands plus relevant output in the PR description. Validate on the platform you are using with the configured preset that matches your OS and build type; Linux and Windows validation are both acceptable when they cover the changed path.

Example Linux debug validation:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Example Windows debug validation:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

For runtime renderer changes, include an appropriate sandbox smoke run, for example:

```sh
./out/build/linux-debug/VolkEngineSandbox --frames 120 --resize-smoke
```

```powershell
.\out\build\windows-debug\VolkEngineSandbox.exe --frames 120 --resize-smoke
```

Depending on the change, maintainers may require one or more of:

- Release build/test evidence for the relevant platform, such as `linux-release` or `windows-release`.
- `--validation` smoke evidence.
- `--no-imgui` latency-path smoke evidence.
- `--depth-prepass` and `--no-depth-prepass` coverage.
- `--indirect-draws` and `--no-indirect-draws` coverage.
- GPU timestamp, CPU timing, draw-count, triangle-count, or resource-count comparisons.
- RenderDoc capture notes for synchronization, barriers, attachments, or pass structure.
- Screenshots for visible rendering changes.
- Platform-specific notes for Windows or non-NVIDIA hardware when relevant.

Do not claim a performance or rendering improvement unless you measured it.

### 5. Open a focused pull request

A good PR description includes:

- Link to the approved proposal or assigned issue.
- Short summary of the change.
- List of important files/subsystems touched.
- Design notes for non-obvious choices.
- Performance/fidelity/correctness impact.
- Exact validation commands and results.
- Screenshots, captures, or benchmark data when relevant.
- Known limitations and intentionally deferred work.

A PR should be reviewable as one coherent change. Split unrelated work into separate approved proposals.

## AI-assisted contributions

AI tools can be useful for drafting, exploration, and mechanical assistance, but they are only assistants. Contributors still need to understand and own the result.

If you use AI to help with a contribution, you must:

- Disclose that AI assistance was used.
- Describe what the tool helped with.
- Read, understand, and take responsibility for every changed line.
- Explain the design during review without outsourcing the answer to the tool.
- Remove hallucinated APIs, fake fallbacks, placeholders, dead code, and generic boilerplate.
- Provide the same build, test, runtime, and measurement evidence required from any other contributor.

Submit AI-assisted patches only when you can reason about them, keep generated code focused, and make sure any generated explanation matches the actual code.

Maintainers may ask for revision or decline AI-assisted PRs that are broad, unreviewable, untested, misleading, or detached from the approved proposal.

## Review expectations

Review is about project fit, not just passing CI. Maintainers may ask for:

- Smaller scope.
- Fewer abstractions.
- More explicit ownership or synchronization.
- Better names matching existing renderer concepts.
- Benchmarks or smoke tests that exercise the changed path.
- Documentation updates when behavior or contributor expectations changed.
- Removal of unrelated cleanup.

Respond with evidence and concrete changes. Do not argue from preference when the project has an existing convention.

## Maintainer priorities

When tradeoffs conflict, the project prefers:

1. Correctness and debuggability before cleverness.
2. Explicit Vulkan behavior before abstraction.
3. Measured performance before theoretical optimization.
4. Narrow changes before broad rewrites.
5. Real engine value before cosmetic improvement.
6. Maintainable code before impressive-looking code.

If your contribution follows that order, it is much more likely to be accepted.
