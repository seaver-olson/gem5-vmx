# gem5 Workbench

A pygame-ce visual workbench for building and running gem5 simulations without
writing a new Python configuration script for every experiment.

## Milestone 1

The initial scaffold provides:

- a component palette based on gem5 Standard Library concepts;
- draggable board, processor, cache, memory, and workload nodes;
- a declarative project model;
- `.g5proj` JSON save/load;
- a basic inspector and persistent component identities.

Milestone 2 will add editable parameters, validation, a permanent gem5 runner,
and the first end-to-end syscall-emulation simulation.

## Run

From the gem5 repository root:

```bash
python3 -m pip install -r util/gem5-workbench/requirements.txt
python3 util/gem5-workbench/run.py
```

The current project is saved to:

```text
util/gem5-workbench/projects/current.g5proj
```

Use `Ctrl+N` for a new project, `Ctrl+S` to save, and `Ctrl+O` to load.

## Design rule

Users edit `.g5proj` files through the GUI. The workbench owns one permanent
runner that converts the project model into gem5 Standard Library components.
The user should not need to create or maintain per-experiment Python scripts.
