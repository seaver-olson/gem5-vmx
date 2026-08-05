# gem5 Workbench

A pygame-ce visual workbench for composing and running gem5 Standard Library
simulations without maintaining a separate configuration script.

## Current capabilities

- discovers Standard Library classes and factory functions from the local gem5
  source tree without importing gem5 into pygame;
- optionally probes the selected gem5 binary to confirm imports, ISAs, and
  protocols;
- shows unsupported discoveries as experimental instead of pretending they are
  runnable;
- edits schema-backed component parameters and creates explicit semantic
  port-to-port connections;
- saves rendering-independent project data and editor layout in one `.g5proj`;
- builds gem5 asynchronously and runs a validated project in a separate gem5
  process;
- retains project/run-spec snapshots, manifests, commands, logs, gem5 config
  output, and statistics for every run.

The first executable adapter is an x86 syscall-emulation system containing
`SimpleBoard`, `SimpleProcessor`, `SingleChannelDDR3_1600`, `NoCache`, and a
local-or-resource binary workload. Automatic discovery is intentionally broader
than executable support: Python signatures cannot reliably describe every gem5
compatibility or assembly rule.

## Launch

gem5 Workbench requires Python 3.10 or newer, independently of gem5's wider
repository baseline.

From the gem5 repository root:

```bash
python3 -m venv util/gem5-workbench/.venv
util/gem5-workbench/.venv/bin/pip install -r util/gem5-workbench/requirements.txt
util/gem5-workbench/.venv/bin/python util/gem5-workbench/run.py
```

The Workbench detects its containing gem5 checkout. Explicit locations may be
provided when needed:

```bash
util/gem5-workbench/.venv/bin/python util/gem5-workbench/run.py \
  --gem5-root /path/to/gem5 \
  --gem5-binary /path/to/gem5/build/X86/gem5.opt \
  --project /path/to/project.g5proj
```

Equivalent environment variables are
`GEM5_WORKBENCH_GEM5_ROOT`, `GEM5_WORKBENCH_GEM5_BINARY`, and
`GEM5_WORKBENCH_PROJECT`.

## Build and run

Use **Build** to explicitly launch:

```text
scons build/X86/gem5.opt -jN
```

`N` defaults to the smaller of eight and the host CPU count. Set
`GEM5_BUILD_JOBS` to override it. A successful build refreshes the runtime
catalog. **Run** becomes available only when project and environment validation
pass; **Stop** interrupts the active build or simulation without blocking the
pygame loop.

A tracked offline example is available at:

```text
util/gem5-workbench/projects/examples/x86-hello.g5proj
```

Launch it with `--project`; the document opens automatically and the background
catalog probe enables **Run** once the selected binary and project pass
validation. Its local workload is the bundled
`tests/test-progs/hello/bin/x86/linux/hello`, so the simulation does not require
a resource download. Selecting `source_kind=resource` uses gem5's resource
client instead.

Browse the palette by component category, expand only the groups you need, and
use search when you know a component's name. Selecting a type previews it in
the Inspector; choose a supported type, then click the canvas to place it. The
Inspector changes with the selection and groups the properties appropriate to
the project, component type, or connection instead of making every object share
one generic property list.

Drag nodes to move them, drag between compatible port handles to connect them,
use the mouse wheel to zoom, and middle-drag to pan. Connections are always
explicit; the Workbench never infers them from component order.

Keyboard shortcuts are `Ctrl+N`, `Ctrl+S`, `Ctrl+O`, `Ctrl+B`, `Ctrl+R`, and
`F5` for New, Save, Load, Build, Refresh Catalog, and Run respectively.

## Files and architecture

The default working document is the ignored
`projects/local/current.g5proj`. Run output is written beneath the ignored
`run-output/<project>/<run-id>/` tree. Catalog caches are machine-local under
the operating-system cache directory.

Each run executes a snapshotted bridge and records a versioned manifest with
the exact binary, workload, and bridge content identities plus the launch
directory and relevant environment. A zero process exit is considered
successful only when gem5 also produces nonempty statistics and configuration
artifacts.

A `.g5proj` has separate `project` and `layout` sections. Project components
retain stable IDs, JSON-compatible parameters, and explicit semantic
connections. Layout contains positions and viewport state and has no pygame
types. Generated catalog definitions and machine-specific binary paths are not
embedded in the project.

Unknown or removed component types survive loading and saving, then receive
validation diagnostics. Duplicate component or connection IDs make a document
invalid. Unknown structural fields are rejected rather than silently discarded,
and project text is limited to a generous 64 MiB before JSON decoding. Standard
Library composition ports are deliberately separate from raw SimObject ports;
low-level SimObject authoring remains future expert-mode work.

## Tests

```bash
cd util/gem5-workbench
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  .venv/bin/python -m unittest discover -s tests -v
```

With `build/X86/gem5.opt` available, enable the real offline integration test:

```bash
GEM5_WORKBENCH_RUN_E2E=1 \
  .venv/bin/python -m unittest tests.test_execution.RealGem5ExecutionTests -v
```
