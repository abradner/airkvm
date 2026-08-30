---
name: batch-review
description: Batch PR shipping workflow - fan out a body of work as a stack of small atomic single-commit PRs (interstitials, from base to cap), let CI and automated reviewers run immediately but treat all feedback as write-only until the whole batch is in, then synthesise the feedback in aggregate, ship all reactive work as ONE followup PR stacked on the cap, resolve interstitial comments as "fixed in the followup" or "not relevant", merge interstitials bottom-up and the followup last. Use this whenever the user wants to review open PRs holistically or in aggregate, process accumulated bot/agent review comments across several PRs, ship a "review feedback batch" or followup PR, or mentions their overnight/fired-off batch of work - even if they don't say "batch review" explicitly. ALSO use at fan-out time, when the user asks to build a planned body of work as small/carefully-factored/stacked PRs. This is a deliberately different, opt-in mode for a genuine multi-PR fan-out, not this repo's default single-PR flow.
---

# Batch review

The core bargain: **feedback is write-only until the whole batch is synthesised.** CI and the bots
run immediately on every PR and comments accumulate freely — but nothing responds to them and no
reviewed branch is rewritten until the synthesis pass. All reactive work ships as one followup PR.

Why: reacting piecemeal across several in-flight PRs creates real churn — branches rewritten under
open reviews, repeated rebase/conflict cycles, reviewer attention fragmented across many small
live threads instead of one synthesis pass. The default single-PR flow (react immediately, merge
when green) remains correct for one PR at a time; this skill exists for the multi-PR case only.

## Terminology

| Term | Meaning |
|---|---|
| Interstitial | A single-commit PR in the proactive stack |
| Base / Cap | The bottom-most / top-most interstitial |
| Followup | The one N-commit reactive PR stacked on the cap — the batch's only live-feedback surface and its release gate |
| Stack object | GitHub stacked-PR beta entity (stacked flavour only) |

## Repo specifics

**Validation commands** (pre-push gate, from a fresh shell):

```sh
mise run lint && mise run test && mise run build
```

`mise run test` is the only one that can fail for a behavioral bug; `build` proves compilation
only. Neither observes USB, HID delivery, or latency — no batch may claim hardware behavior was
validated by CI (AGENTS.md §4, last rule).

**Bot roster**: `copilot-pull-request-reviewer[bot]`. Discovered during the session-ownership
batch (2026-08), where this file still claimed no reviewer existed — it had reviewed all four
PRs within minutes of opening and nobody looked for ~two weeks. Correcting it here rather than
leaving the claim standing (AGENTS.md §2 principle 1).

- **Trigger**: automatic on PR open. No re-request needed; no configuration in this repo.
- **Cost**: free at time of writing.
- **Observed quality** on that batch: five findings, four accepted (a blocking `xSemaphoreTake`
  on the shared `esp_timer` task, a timer leaked on an error path, an off-by-one in a wraparound
  test's own arithmetic, one doc phrasing), one deferred to the tracker. Good at local,
  mechanical, single-file defects.
- **Its structural blind spot**: it reviews each PR against *its own base*, so in a stack it
  cannot see that a later interstitial already fixed what it is objecting to. One finding on the
  base PR had been resolved by the very next PR. Check the cap before actioning a finding raised
  low in a stack.
- **Read it via `gh api`, never the rendered summary.** It files real findings inside a collapsed
  `<details>Suppressed comments</details>` block, which is precisely the "no new comments"
  false-green in the Phase 6 table.

The self-review pass (Phase 2) and `independent-commit-review` still carry the aggregate and
cross-PR work: on that same batch the whole-stack pass found a design-level defect (a deadline
measuring the wrong thing entirely) that all four per-PR bot reviews missed.

**Merge strategy**: squash-merge. The template's `[MERGE-COMMIT]` rule variants were deleted at
init and the squash variants inlined, so the rules below read as unconditional — if this repo
ever switches strategy, they must be re-derived from the template, not edited in place.

**Deferral convention**: non-blocking findings become GitHub issues on `abradner/airkvm`,
labelled `deferred` and linked back to the review comment. Never a PR-body table — it goes stale
between rounds and vanishes on merge.

## Phase 1 — Flavour probe

Probe whether GitHub's stacked-PR beta (`gh stack`) is enrolled and working for this repo; pick
**stacked** or **manual** flavour accordingly and record the choice in the batch's tracking note.
A batch finishes in the flavour it started; the only mid-batch transition allowed is stacked →
manual (via unstacking), never the reverse. Re-probe every batch — enrollment and beta behavior
change. Treat any stacked-flavour tooling claims as recorded observation, not guarantee: sibling
repos observed `gh stack sync` reporting "✓ synced" without pushing, and stack commands silently
operating against a stale local main ref inside worktrees while printing success.

## Phase 2 — Self-review before fan-out

Before any PR opens, run one pointed self-review question per branch — "what did this change make
newly risky?" — delegated to a cheap review subagent, findings fixed in the commit itself.
Evidence for why this is worth the cost: the same finding surfaced before opening costs a
`git commit --amend`; the same finding after fan-out costs a reactive round, and rounds introduce
bugs (see Provenance).

## Phase 3 — Fan-out

- One commit per interstitial, opened ready-for-review immediately (trips auto-reviewers once,
  early, while reaction is still cheap to withhold).
- Every PR body carries a machine-scannable batch block, so the ground rules survive even when
  this skill doesn't trigger and AGENTS.md goes unread:

  ```
  ## Batch
  Batch: <name> | Flavour: manual|stacked | Position: N of M
  Stacked on: #<parent> | Feedback: write-only until synthesis — see followup PR
  Merge: bottom-up after followup approval, operator-gated
  ```

- Never guess PR numbers; never leave placeholder references live.
- **Neighbouring-PR overlap sweep**: intersect the batch's touched files against every other open
  PR (`gh pr list`, then `gh pr view <n> --json files`). Disposition per overlap, recorded:
  **comment/adapt-after** (default — tell the neighbour what changed and to adapt once the train
  lands; never absorb its work, never pre-emptively rebase its branch), **adapt-in** (only when the
  batch semantically breaks it and the fix is mechanical — lands in the followup, attributed), or
  **escalate** (irreversible collisions, e.g. competing migration timestamps, are operator
  decisions). Re-run the sweep at readiness — parallel work opens while a batch bakes — and name
  every neighbour that must adapt in the readiness report.

## Phase 4 — Bake

Interstitial CI red is tolerated (the followup fixes it). Exactly one showstopper bar justifies
touching an interstitial mid-bake: **an irreversible action on merge** — a destructive migration,
an unrecallable external side effect. Everything else waits.

- If main moves under the stack: restart affected branches from fresh main after their parents
  land; never stack new commits on pre-merge history.
- A showstopper fix injected low in the stack must be explicitly propagated into each child —
  squash does not preserve the ancestry that would reunify it.

**Driving phase transitions**: subscribe to PR/CI events and react when they arrive; pair the
subscription with a bounded fallback timer (order of ~30 min for a full stack's reviews, ~10 min
for a single re-solicited review) so a bot no-show can't stall an unattended batch. That fallback
is this workflow's single named exception to event-driven monitoring, not a license for scheduled
polling elsewhere.

## Phase 5 — Synthesis

- Harvest every comment on every PR via `gh api` — threads, reviews, and top-level comments.
- Filter agent-posted bookkeeping replies out of the review record: they post under the operator's
  credentials and masquerade as human reviews. Distinguish by `in_reply_to_id`, not author.
- Triage by verifying: reproduce claims, check citations, apply AGENTS.md's "a finding is a claim,
  not a verdict." Sort into: fix in followup / deferral to tracker / decline with stated reason.
- Add one aggregate pass across the full stack diff — cross-PR interactions are structurally
  invisible to per-PR reviewers. **When implementation was delegated to subagents, this pass must
  itself be delegated to one independent fresh-eyes reviewer agent**: the orchestrating session
  has read agent reports and bot comments, not the diff — its "own review" is process review
  wearing a code-review hat, and nobody has held the whole change. Brief the reviewer with the
  repo's trust-boundary questions and instruct it to *trace* claims, never trust comments, PR
  bodies, or the orchestrator's summary. Launch it in parallel with the comment harvest so its
  findings fold into the followup's first commit instead of burning a late reactive round; its
  findings count toward the round cap like anyone else's.
- The reviewer's diff command is `git fetch origin <default-branch> && git diff origin/<default-branch>...<followup-branch>` (or the cap branch
  before the followup exists) — spell out the left side and keep the command on one physical
  line: `git diff ...<branch>` is *valid* git that silently defaults the left side to HEAD, so a
  checkout not on main yields a quietly truncated diff, and a fresh-eyes verdict over a truncated
  diff is a confident "cleared" over code nobody read. (A wrapped command has the same failure:
  the first pasted line, bare `git diff`, is silently valid too.)

## Phase 6 — The followup PR

- Open as a **draft targeting main** so its diff is the whole stack — spend one budgeted
  aggregate bot review there — then **retarget to the cap** before marking ready, so per-PR
  reviewers see only the reactive delta.
- Maintain a `## Review focus` section in the body, restated each round.
- **Hard cap: three reactive rounds.** Past the cap, remaining findings are deferred to the
  tracker, not patched. If one small change draws three or more findings, revert it and ticket
  it — it needs design time, not another patch. (See Provenance for why the cap exists.)
- **Never trust an aggregate review signal.** Known false-green patterns, each observed for real:

  | Signal | How it lies | The check that catches it |
  |---|---|---|
  | "Zero unresolved threads" | Reads identically whether feedback was addressed or never solicited | Verify a review was actually requested and delivered for the current head commit |
  | "No new comments" | Suppressed/collapsed comment blocks hide real findings under a clean summary | Expand and read the raw review payload via `gh api`, not the summary state |
  | New test passing on first run | May be passing against unfixed code | Revert-and-confirm: watch it fail without the fix |
  | Green CI run | Path filtering may have skipped the half of the suite your change lives in | Check which jobs actually ran, not the rollup color |

## Phase 7 — Merge the stack

- **Operator gate: never start this phase without the operator explicitly saying to merge now.**
  A synthesis pass, a green followup, and an auto-mode session default are not that signal. If the
  stack is ready, say so and stop.
- **Pre-flight, first: account for the followup.** The write-only bargain is that feedback gets
  answered *somewhere*, and nothing in the mechanics enforces it — a stack merges perfectly well
  without a followup and the merge control does not ask. Merge without one and every accepted
  finding silently becomes debt on trunk with nothing tracking it.

  A followup is not mandatory; *accounting* for its absence is. If one exists, confirm it is in
  the train and go on to the second check. If none exists, establish which case this is:

  - **Synthesis has not happened.** This phase is premature. Say so and stop.
  - **Synthesis happened and produced nothing to action.** Legitimate — proceed. But confirm it
    rather than inferring it from quiet: "zero unresolved threads" reads identically to "never
    reviewed", so check that review was actually solicited and delivered for the current head.

  The failure this prevents is an agent finding no followup, assuming the second case because
  nothing looks wrong, and merging.
- **Pre-flight, second: confirm the followup's base is the cap, not trunk.** The Phase 6
  draft-first opening deliberately targets trunk for the aggregate review; if the retarget was
  missed, merging the followup collapses the whole stack into one commit.
- Merge bottom-up. Before deleting a merged branch, verify every child PR has already been
  retargeted — deleting a base branch races the platform's auto-retarget and has closed child PRs
  mid-train. Retarget first, confirm, then delete.
- Followup merges last, squash-merged.
- If a child PR does get closed by a race: reopen against the corrected base immediately; its
  commits are intact on the branch.

## Rules of thumb

One commit per interstitial. Open ready-for-review immediately. Nothing answers feedback until
synthesis. Showstopper bar = irreversible-on-merge only. One followup PR carries all reactive
work. Delegated implementation means delegated review — one fresh-eyes agent over the full,
spelled-out aggregate diff. Sweep neighbouring PRs at fan-out and readiness; comment, don't
absorb. Three reactive rounds, then defer. Verify aggregate signals; never trust the rollup.
Operator says "merge" — nothing else counts. Account for the followup before starting the train:
in it, or a confirmed nothing-to-action. Bottom-up, retarget before delete.

## Where these rules came from

Ported across a fleet of sibling repos and corrected against real batches in each. Keep this
section when adapting: add to it rather than replacing it — the value is in the accumulated
evidence, not in any single batch.

- The write-only rule exists because piecemeal feedback batches once rewrote main under a
  still-open stacked PR and killed it.
- The three-round cap exists because a measured batch ran seven reactive rounds with finding
  counts 6 → 2 → 1 → 3 → 2 → 5 — not convergence: four of the five later rounds were fixing
  defects a previous round's own fix had introduced. A separate repo independently recorded a
  "third instance in this batch of a fix introducing the next round's finding" the same week.
- The self-review-before-fan-out gate exists because of the same data: findings are an order of
  magnitude cheaper before the stack exists.
- The bot-budget asymmetry (unrationed cheap reviewer, budgeted expensive one) comes from a
  measured split: the cheap bot caught 6/6 of a mechanical bug class; every expensive-bot finding
  that mattered was cross-file. Spend the budget on aggregate diffs.
- The followup-exists pre-flight was added after a nine-PR stacked batch merged its entire train
  with no followup at all. Thirteen accepted findings landed on trunk untracked, including a
  warning that could never fire (production never set the logger it wrote to) and a
  nondeterminism trap in a package documented as pure — a replay hazard in a workflow engine.
  Nothing was red: CI was green, every thread had been triaged, and the merge control asked no
  questions. The findings were recovered only because the synthesis notes happened to still be in
  a live session. Every other rule here assumes the followup lands; this is the one that checks.
- The retarget-before-delete rule exists because `gh pr merge --delete-branch` closed a child PR
  in a live merge train when branch deletion raced GitHub's auto-retarget.
- The independent fresh-eyes pass exists because a sibling repo's nine-PR delegated batch went
  through per-PR bot review on every PR, one aggregate bot review, and two orchestrator-run
  reactive rounds — and a fresh-eyes agent over the full diff then found what all of that missed:
  a reactive-round fix had silently defanged a security regression test (now passing without
  executing the branch it pinned), an unaudited denial path, and a privilege grant whose sole
  written justification was never verified under the live role. Three bot rounds: zero of the
  three. The same pass also *cleared* the batch's security claims by tracing them, which is what
  made the readiness report worth trusting.
- The spelled-out diff command exists because the shorthand `...<branch>` doesn't fail when
  copied — it silently diffs from HEAD, truncating the review surface — and the wrapped two-line
  form fails the same way (bare `git diff` is valid). Both were caught in review of the skill text
  itself before either bit a real batch.
- The overlap sweep exists because the same batch shared one file with a neighbouring open PR;
  one comment telling it to adapt after the train cost one API call and prevented a surprise
  conflict — and a parallel session honouring the don't-push-to-others'-branches rule the same
  night is what kept two sessions from colliding on one branch twice in a day.

*This repo's own batch outcomes get appended here as they accumulate — none yet.*
