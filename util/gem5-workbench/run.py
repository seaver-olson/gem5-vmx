import argparse
import sys

MINIMUM_PYTHON = (3, 10)


def _parse_args(argv=None):
    parser = argparse.ArgumentParser(description="Launch gem5 Workbench")
    parser.add_argument(
        "--project", help="path to the current .g5proj document"
    )
    parser.add_argument(
        "--gem5-root", help="path to a local gem5 source checkout"
    )
    parser.add_argument(
        "--gem5-binary", help="path to gem5.opt for probing and execution"
    )
    return parser.parse_args(argv)


def main(argv=None):
    if sys.version_info < MINIMUM_PYTHON:
        required = ".".join(str(part) for part in MINIMUM_PYTHON)
        current = f"{sys.version_info.major}.{sys.version_info.minor}"
        raise SystemExit(
            f"gem5 Workbench requires Python {required}+; found {current}"
        )

    from workbench.app import WorkbenchApp

    arguments = _parse_args(argv)
    app = WorkbenchApp(
        project_path=arguments.project,
        gem5_root=arguments.gem5_root,
        gem5_binary=arguments.gem5_binary,
    )
    app.run()


if __name__ == "__main__":
    main()
