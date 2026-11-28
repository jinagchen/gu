"""Expose a local RecallFlow index through LangChain's StructuredTool API."""

import json
import subprocess
from pathlib import Path
from threading import Lock

from langchain_core.tools import StructuredTool, ToolException


def create_tools(binary: Path, index: Path) -> list[StructuredTool]:
    binary = binary.resolve(strict=True)
    index = index.resolve(strict=True)
    lock = Lock()

    def invoke(operation: str, **fields) -> dict:
        request = json.dumps({"operation": operation, **fields}, allow_nan=False)
        with lock:
            try:
                completed = subprocess.run(
                    [str(binary), str(index)], input=request + "\n", encoding="utf-8",
                    capture_output=True, check=True, timeout=30,
                )
                return json.loads(completed.stdout)
            except (subprocess.SubprocessError, OSError, ValueError) as exc:
                effect = " The write result is unknown." if operation != "search" else ""
                raise ToolException(f"RecallFlow command did not return a result.{effect}") from exc

    def search(vector: list[float], k: int = 10) -> dict:
        """Search local memory using a vector and return the closest IDs and versions."""
        return invoke("search", vector=vector, k=k)

    def insert(id: str, vector: list[float]) -> dict:
        """Insert a new memory using a decimal uint64 ID and return the durable receipt."""
        return invoke("insert", id=id, vector=vector)

    def upsert(id: str, vector: list[float]) -> dict:
        """Insert or replace a memory using a decimal uint64 ID and return the receipt."""
        return invoke("upsert", id=id, vector=vector)

    return [StructuredTool.from_function(function) for function in (search, insert, upsert)]
