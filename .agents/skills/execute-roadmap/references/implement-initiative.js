// Workflow template driven by the `execute-roadmap` skill (step 3).
//
// Implements and independently reviews a batch of DISJOINT-write-scope roadmap
// initiatives. The main loop selects the batch (WIP cap 3, disjoint Files, advance
// slot) and keeps the side-effecting git/gh/state ops (label, merge_guard, state.py)
// to itself -- this workflow does the parallel cognitive work: implement + review.
//
// Invoke with the Workflow tool, passing the selected initiatives as `args`:
//   args = [{ id, title, size, phase, files:[...], slices:[{scope,proof,rollback,prereq}],
//             acceptance:[...], implPlanPath }]
// Adapt the prompts to the specific initiative before running; this is a starting
// point, not a fixed script. Only run in a build-capable, gh-authenticated environment.

export const meta = {
  name: 'implement-initiatives',
  description: 'Implement (test-first, isolated worktree) and independently review a batch of roadmap initiatives',
  phases: [
    { title: 'Implement', detail: 'One isolated-worktree agent per initiative, test-first per slice' },
    { title: 'Review', detail: 'Fresh-context adversarial review of each diff (never the implementer)' },
  ],
}

const IMPL_RESULT = {
  type: 'object', additionalProperties: false,
  properties: {
    id: { type: 'string' },
    branch: { type: 'string' },
    pr: { type: 'string', description: 'draft PR url (empty if not opened)' },
    filesTouched: { type: 'array', items: { type: 'string' } },
    slicesDone: { type: 'array', items: { type: 'string' } },
    localGate: { type: 'string', description: 'what built/tested and the result' },
    blocked: { type: 'string', description: 'empty if complete; else why it stopped' },
  },
  required: ['id', 'branch', 'filesTouched', 'localGate', 'blocked'],
}

const REVIEW_RESULT = {
  type: 'object', additionalProperties: false,
  properties: {
    id: { type: 'string' },
    verdict: { type: 'string', enum: ['approve', 'changes-requested'] },
    securityChecked: { type: 'boolean' },
    findings: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        properties: {
          severity: { type: 'string', enum: ['Critical', 'Important', 'Minor'] },
          area: { type: 'string' }, detail: { type: 'string' }, location: { type: 'string' },
        },
        required: ['severity', 'area', 'detail'],
      },
    },
    summary: { type: 'string' },
  },
  required: ['id', 'verdict', 'securityChecked', 'findings', 'summary'],
}

const items = Array.isArray(args) ? args : []
if (!items.length) return { error: 'no initiatives passed as args', results: [] }

const heavy = (it) => it.size === 'XL' || it.size === 'XXL'

const results = await pipeline(
  items,
  // Stage 1 -- implement in an isolated worktree, test-first, one commit per slice.
  (it) => agent(
    `Implement roadmap initiative \`${it.id}\` ("${it.title}", size ${it.size}) for OpenLiveReplay.\n` +
    `Follow the impl plan at ${it.implPlanPath} and docs/broadcast-plan/operating-loop.md.\n` +
    `RULES: work ONLY inside the declared Files: ${JSON.stringify(it.files || [])}. For each PR slice ` +
    `${JSON.stringify(it.slices || [])}, go RED (write the failing test named by the acceptance criteria: ` +
    `${JSON.stringify(it.acceptance || [])}) -> GREEN -> refactor, ONE commit per slice. Run the local gate ` +
    `for your scope (the .githooks/pre-push gate, or the roadmap audit for docs/tooling changes). Then push ` +
    `the branch (\`agent/<id>\`) with gh's credential helper and open a DRAFT PR to origin/main. Do NOT merge, ` +
    `do NOT touch program-state.json, do NOT edit files outside the declared list. If you cannot build/test ` +
    `here, stop and report it in 'blocked'. Return the branch, PR url, files touched, slices done, and the ` +
    `local-gate result.`,
    { label: `impl:${it.id}`, phase: 'Implement', isolation: 'worktree',
      model: heavy(it) ? 'opus' : undefined, effort: 'high', schema: IMPL_RESULT }
  ),
  // Stage 2 -- independent adversarial review in a fresh context (never the implementer).
  (impl, it) => {
    if (!impl || impl.blocked || !impl.pr) return impl ? { it, impl, review: null } : null
    return agent(
      `Independently review PR ${impl.pr} for initiative \`${it.id}\` in a FRESH context. You did NOT write ` +
      `it. Read the diff (\`gh pr diff ${impl.pr}\`). Verify: (a) it satisfies the acceptance criteria ` +
      `${JSON.stringify(it.acceptance || [])}; (b) the diff touches ONLY the declared Files ` +
      `${JSON.stringify(it.files || [])}; (c) tests are real (RED->GREEN, not tautological); (d) no ` +
      `correctness/concurrency/resource regressions. If the change touches auth, ingest/parsing, I/O or ` +
      `rendering surfaces, do a security-focused pass and set securityChecked=true. Return verdict=approve ` +
      `only with ZERO unresolved Critical/Important findings; else changes-requested with specifics.`,
      { label: `review:${it.id}`, phase: 'Review', model: 'opus', effort: 'high', schema: REVIEW_RESULT }
    ).then((review) => ({ it, impl, review }))
  }
)

const done = results.filter(Boolean)
return {
  results: done,
  // The main loop consumes these: label + merge_guard + state.py for approvals;
  // revert-first / re-plan for changes-requested or blocked.
  mergeable: done.filter(r => r.review && r.review.verdict === 'approve' && !r.impl.blocked)
    .map(r => ({ id: r.it.id, pr: r.impl.pr })),
  needsWork: done.filter(r => r.impl.blocked || (r.review && r.review.verdict === 'changes-requested'))
    .map(r => ({ id: r.it.id, why: r.impl.blocked || (r.review && r.review.summary) })),
}
