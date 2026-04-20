# PR CI pipeline restructure

Plan for cleaning up the PR CI pipeline so it can grow a real test
suite without becoming a single monolithic workflow. Delete this file
when the streams have all landed.

---

## Context

`integration-pipeline.yml` currently does too much in one file: base
image build, build, artifact publish, local deploy verify, and
distributed deploy verify — all chained sequentially. Bolting unit /
integration / e2e test tiers on top of this as-is would make it
worse. We want to split, but not explode into a dozen workflows.

---

## Target shape

Two parallel tracks that share a build foundation, with test tiers
living as **jobs** inside each track (not separate workflow files):

1. **`build.yml`** — base image + Debug build + artifact/image publish.
   Single source of truth for the binaries every downstream tier
   consumes.
2. **`local.yml`** — local deployment verify + `unit` / `integration` /
   `e2e` jobs running against the artifact in a single container.
3. **`distributed.yml`** — distributed deployment verify + `unit` /
   `integration` / `e2e` jobs running in the multi-container compose
   setup. Symmetric internal structure to `local.yml`.
4. **`check-format.yml`** — unchanged.

Principle: **jobs, not files**, for parallel work that shares a
trigger. Split files only when triggers, environments, or lifecycles
genuinely differ (local vs. distributed is the clearest such split).

---

## Incremental plan

### Step 1 — switch build to Debug

**Status:** tracked in issue #599.

**Scope:** pass `-t Debug` to the `local_single_user_deploy.sh -b` and
`-i` calls in `integration-pipeline.yml`, and rename the uploaded
artifact to reflect Debug. This enables `CHRONOLOG_INSTALL_TESTS=ON`
so test binaries ship in the artifact, ready for later tiers to
consume. No test execution yet.

**Blocks:** step 3 (tests need Debug binaries).
**Blocked by:** nothing.

---

### Step 2 — split local vs. distributed

**Status:** not started.

**Scope:** first structural cut. Extract the `distributed-deployment`
job into its own `distributed.yml`. The rest (base image + build +
local deploy) stays in one file — renamed to `local.yml` or kept as
`integration-pipeline.yml` for now. Cleans up the PR check graph
before we start adding test jobs. Local and distributed will each
grow their own test tiers with symmetric internal structure.

**Open question:** share the base-image build via reusable workflow
(`workflow_call`) or duplicate it across `local.yml` and
`distributed.yml` initially? Lean: duplicate first, factor out in
step 4 once we see the real shape.

**Blocks:** steps 3 and 4.
**Blocked by:** step 1 (want the Debug artifact in place first so the
split lands with test-capable binaries).

---

### Step 3 — add test tiers as jobs

**Status:** not started.

**Scope:** inside `local.yml` and `distributed.yml`, add a `unit` job
first, then `integration` and `e2e` jobs as those suites come online.
Each job consumes the Debug artifact/image from step 1 and runs
`ctest` with the appropriate filter. Tiers run in parallel per
workflow.

**Rollout:**
- Start with `unit` in `local.yml`.
- Mirror into `distributed.yml` once the local version is green.
- Add `integration` and `e2e` incrementally as suites stabilize.

**Blocks:** nothing.
**Blocked by:** steps 1 and 2.

---

### Step 4 — factor base image into shared `build.yml`

**Status:** not started. May be skipped if duplication proves cheap
enough.

**Scope:** if after step 2 the base-image build is duplicated and
painful to maintain, extract it into a reusable `build.yml` that both
`local.yml` and `distributed.yml` call via `workflow_call`. Only do
this once there's evidence the duplication hurts.

**Blocks:** nothing.
**Blocked by:** step 2 (need to see the real duplication first).

---

## Suggested landing order

1. **Step 1** — issue #599, small and mechanical.
2. **Step 2** — split local vs. distributed. Pure restructure, no
   behavior change.
3. **Step 3** — add `unit` job first, grow tiers from there.
4. **Step 4** — only if duplication actually bites.

---

## Open audit items

- What consumes the current Release artifact today? (Distributed
  deployment, image publishing, anything external?) Drives whether
  step 1 can flip to Debug-only cleanly.
- Current `integration-pipeline` runtime baseline — for budgeting
  step 3.
- Confirm distributed deployment works off a Debug-built image.
