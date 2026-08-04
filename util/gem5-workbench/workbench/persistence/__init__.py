"""Serialization and file I/O for ``.g5proj`` documents."""

from workbench.persistence.project_file import (
    ProjectFileError,
    document_from_data,
    document_to_data,
    dumps_project_document,
    load_project_document,
    loads_project_document,
    save_project_document,
)

__all__ = [
    "ProjectFileError",
    "document_from_data",
    "document_to_data",
    "dumps_project_document",
    "load_project_document",
    "loads_project_document",
    "save_project_document",
]
