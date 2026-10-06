# Coherence analysis: where `flush` and `invalidate` go

Source: [`src/pass_coherence.cpp`](../src/pass_coherence.cpp). Theory: a **forward
data-flow analysis over the control-flow graph** (Aho, Lam, Sethi, Ullman, 2nd ed.,
section 9.2) -- the same iterative framework as reaching definitions, with a different
lattice and transfer function.

## The problem

A DPU reads and writes DRAM directly. It bypasses the host's cache, so the two can
disagree about the contents of an array:

* The host writes `A`. The newest values may sit in **dirty cache lines**, invisible
  to a DPU that is about to read `A` from DRAM. The host must **`flush(A)`** first
  (write the dirty lines back).
* A DPU writes `C`. The host's cache may still hold **stale lines** of `C`. Before the
  host reads `C` it must **`invalidate(C)`** (drop those lines so the next access
  misses and fetches the new values).

Placing these everywhere would be correct but slow (each costs `COHERENCE_OP_CYCLES`
plus the lines it touches). Placing them nowhere is wrong. The analysis finds the
minimum that is still safe.

## The lattice

For one program point the state is a pair of **sets of array names**:

| component | meaning |
|---|---|
| `HostDirty` | arrays written by host code and not yet flushed |
| `PimDirty`  | arrays written by a PIM kernel and not yet invalidated in the host cache |

States are ordered by inclusion (componentwise). This is a **may** analysis: a name in
a set means "possibly dirty on some path". The top of the lattice is "everything
dirty"; the analysis starts from the empty state at the entry (nothing is dirty before
the program runs) and only grows.

**Meet = union**, per component, over the predecessors of a block. This is where the
`if`/`else` (and the loop header) matter: an array that is dirty on *one* incoming path
must be treated as dirty after the paths join.

## gen / kill and the transfer function

Every host statement and every nest of an inlined `call` updates the state `(H, P)`:

| event | operations emitted first | effect on the state |
|---|---|---|
| host **store** to `A` | `invalidate(A)` if `A ∈ P` | `P -= {A}`;  `H += {A}` (gen) |
| host **load** of `A`  | `invalidate(A)` if `A ∈ P` | `P -= {A}` (kill) |
| PIM nest touching `A` (read or write), `A ∈ H` | `flush(A)` | `H -= {A}` (kill) |
| PIM nest **writes** `A` | -- | `P += {A}` (gen) |
| nest of a kernel kept on the host | as the host loads/stores it contains | |

(A PIM nest that only *writes* `A` also flushes it first: otherwise a stale dirty line
could be written back after the DPUs finish and overwrite their result. This extends the
course's "flush before a kernel *reads* A".)

Arrays a fused nest keeps entirely inside PIM (`local_only`) never touch DRAM, so they
take no part in any of this.

The transfer function of a basic block is the composition of these updates over its
statements in order; `call k()` is expanded in place, nest by nest, using the plan's
verdict for each nest.

## Worked example: [`tests/coherence_demo.nmc`](../tests/coherence_demo.nmc)

```c
kernel scale() { for (i...) { C[i] = A[i] * 2.0; } }      // offloaded to PIM

host main() {
    alpha = 1.0;
    for (i...) { A[i] = i * 0.5; }      // host writes A
    call scale();                       // PIM reads A, writes C
    if (alpha > 0.0) { print C[0]; }    // host reads C, in one branch only
    print C[5];                         // host reads C after the branches merge
}
```

Control-flow graph of `main` (`./nemoc tests/coherence_demo.nmc --emit-dot cfg`):

```
B0 entry -> B1 for.i.header -> B2 for.i.body -> B1        (back edge)
                           \-> B3 for.i.exit  ---> B4 if.then ---> B5 if.merge
                                               \----------------->/   (else: skip)
```

Output of `./nemoc tests/coherence_demo.nmc --coherence` (H = HostDirty, P = PimDirty):

```
  round 1
    B0  entry            IN  H={}    P={}    OUT H={}    P={}
    B1  for.i.header     IN  H={}    P={}    OUT H={}    P={}
    B2  for.i.body       IN  H={}    P={}    OUT H={A}   P={}        host stores A: gen H
    B3  for.i.exit       IN  H={}    P={}    OUT H={}    P={C}       call: flush A, PIM writes C
    B4  if.then          IN  H={}    P={C}   OUT H={}    P={}        host load of C: invalidate
    B5  if.merge         IN  H={}    P={C}   OUT H={}    P={}        <- meet of B4 and B3
  round 2
    B1  for.i.header     IN  H={A}  P={}    OUT H={A}   P={}        back edge brings H={A} into the header
    B2  for.i.body       IN  H={A}  P={}    OUT H={A}   P={}
    B3  for.i.exit       IN  H={A}  P={}    OUT H={}    P={C}       H={A} reaches the call: flush(A) needed
    B4  if.then          IN  H={}   P={C}   OUT H={}    P={}
    B5  if.merge         IN  H={}   P={C}   OUT H={}    P={}
  round 3                 (no block changes: fixed point)
```

Reading the table:

* **Round 1** cannot know that `A` is dirty when the loop *header* is entered a second
  time; the facts only flow forward once.
* **Round 2**: the back edge `B2 -> B1` carries `H={A}` into the header (meet of the
  entry path `{}` and the back-edge path `{A}` is `{A}`), and from there to the exit
  block `B3`. Now the call is known to see a HostDirty `A`.
* **Round 3** changes nothing: the analysis has converged.
* **`B5` is the point of the example.** Its predecessors are `B4` (which invalidated
  `C`: `P={}`) and `B3` (the `if` was skipped: `P={C}`). The meet is the union, so
  `P={C}` at the merge. The read `print C[5]` therefore needs *its own* `invalidate(C)`
  even though the then-branch already had one.

Operations inserted (`--coherence` also prints the IR with them in place):

```
kernel scale():   flush(A)                  before the offloaded nest
host main():      invalidate(C)             inside the then-branch, before  print C[0]
                  invalidate(C)             after the merge,       before  print C[5]
```

An invalidate needed inside a loop that contains no kernel call is hoisted in front of
the loop: the array cannot become PimDirty again inside it, so one invalidate suffices.

## How it is validated

The static analysis is checked dynamically. During simulation a **coherence checker**
tracks the same two sets from the actual execution: a host access to an array that is
PimDirty, or a PIM nest touching an array that is HostDirty, is recorded as a violation.
`tests/test_equiv.cpp` requires **zero violations** on every benchmark and on this
example, and the verdict line of `--simulate` shows them if they occur.
