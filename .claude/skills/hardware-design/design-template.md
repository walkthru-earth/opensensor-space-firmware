<!-- SPDX-License-Identifier: CC-BY-4.0. Copyright Walkthru.Earth. -->

# <Parts> <use>, design

Status is draft for owner approval, <YYYY-MM-DD>. Nothing is modelled yet.

## Intent

What the Thing is, where it lives, what this printed part does for it and
why now. Printer, nozzle and material for the first print.

## Goals

- What it must do, each one checkable.

## Non-goals

- What this version deliberately leaves out.

## Anti-goals

- What would make it a failure even if it prints and fits, for example a
  biased sensor reading or a latch that loosens.

## Constraints and source numbers

Link each part sheet in `hardware/parts/` and copy only the numbers this
design rests on, each with its source. State the coordinate frame.

| Item | Value | Source |
| --- | --- | --- |

## Approaches considered

Two or three ways to build it, what each trades off, which one the owner
picked and why.

## Design

One subsection per printed part, with its print orientation, how it holds
each other part, how it opens, and how it meets the rules in
`.claude/skills/hardware-design/SKILL.md`. A small text sketch helps.

### Test plate

Every fit the full print depends on, as small labelled samples.

## Assembly and maintenance

Numbered steps to assemble, and what the owner does to reach each
serviceable part.

## Checks the builder runs

Asserts, interference against vendor bodies, dfm, snapshots and sections.

## Decided by the owner

## Calls made in this design (owner can change)

## Left to the builder

## Open items

Caliper checks, values no source gives, and anything the test plate must
settle.
