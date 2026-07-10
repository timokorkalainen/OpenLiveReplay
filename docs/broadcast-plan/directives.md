# Directives — human steering channel

Append-only. This is how the human steers the autonomous loop **by exception**.
Agents read this file **first** every iteration, obey the newest applicable
directive, and acknowledge each open directive by appending a `Resolution` line.
Silence means continue.

## Rules

- **Append only.** Never edit or delete a prior entry. Superseding a directive is
  itself a new entry that references the old one.
- **Agents never author directives** — only `Resolution` lines under an existing
  human entry.
- A directive with **Stop** halts new work in its scope until a later entry lifts
  it; a directive with **Revert** authorises (and requests) reverting the named
  slice/PR — reverting is always in-policy.

## Entry format

```
### YYYY-MM-DD — <scope> — <one-line directive>
**Directive:** what to do / not do.
**Stop:** <scope>            (optional — halts new work in scope)
**Revert:** <pr/slice>       (optional — revert this)
**Resolution (agent, YYYY-MM-DD):** what was done, with links.   ← appended by agents
```

`<scope>` is `global`, a phase (`phase-3`), an initiative id (`bvp-hq-scaling`),
or a subsystem (`ingest`).

---

## 2026-07-10 — global — Seed assumptions (ratify or correct)

**Directive:** These three inputs to the execution model were inferred from the
repository and the prior engineering review + broadcast plan, not supplied
explicitly. They are in force until a later directive changes them. **Correct any
that are wrong by appending a new entry.**

- **Product goal / users / non-goals:** as stated in [`README.md`](./README.md)
  (Goal + Non-goals) — a certifiable professional broadcast replay engine for
  replay operators and technical directors on event-safety, sports and
  small-to-mid live productions; explicitly *not* an NLE, a cloud SaaS, or a
  vendor-hardware product.
- **Hard constraints:** none beyond what the repo shows — preserve Qt6 / C++17 /
  QML, native-only ingest, the delivered frame-sync program, the GPU pipeline,
  the FFmpeg-in-record-layer firewall, and the existing CI gates.
- **Human owner:** **Timo Korkalainen** (solo maintainer) steers here and owns the
  external evidence gates — LSM-operator usability studies, real-hardware /
  interop-lab validation (DeckLink / ST 2110 / genlock), production deploys, and
  any spending.

**Resolution (agent, 2026-07-10):** Seed acknowledged and encoded into the index
governance, the quality gates, and the operating loop's "never without a
directive" list. Loop will proceed under these assumptions until superseded.
