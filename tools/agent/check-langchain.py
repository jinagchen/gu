"""Build a disposable index and exercise the LangChain tools without a model."""

import argparse
import importlib.metadata
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from langchain_adapter import create_tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output-parent", type=Path, required=True)
    args = parser.parse_args()
    parent = args.output_parent.resolve()
    if not str(parent).isascii():
        parser.error("use an ASCII output path for the builder")
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="langchain-", dir=parent))
    fixture = run / "fixture"
    subprocess.run(
        [sys.executable, "-m", "evaluation.static_fixture", "--output", str(fixture),
         "--run-id", "langchain-smoke", "--vectors", "128", "--dimension", "16",
         "--duration-ms", "1000", "--ops-per-second", "100"],
        cwd=Path(__file__).resolve().parents[2], check=True,
    )
    subprocess.run(
        [str(args.build.resolve()), "--config", str(fixture / "build.json"),
         "--report", str(fixture / "build-report.json")],
        check=True,
    )
    config = json.loads((fixture / "run-config.json").read_text(encoding="utf-8"))
    index = fixture / config["engine"]["directory"]
    search, insert, upsert = create_tools(args.binary, index)
    first = [100.0] * 16
    updated = [125.0] * 16
    identifier = "9000001"
    write = insert.invoke({"id": identifier, "vector": first})
    assert write == {"status": "ok", "id": identifier, "sequence": "1"}, write
    read = search.invoke({"vector": first, "k": 1})
    assert read == {"status": "ok", "snapshot_sequence": "1", "neighbors": [
        {"id": identifier, "version": "1", "squared_distance": 0.0},
    ]}, read
    duplicate = insert.invoke({"id": identifier, "vector": first})
    assert duplicate["code"] == "already_exists" and duplicate["write_effect"] == "none", duplicate
    write = upsert.invoke({"id": identifier, "vector": updated})
    assert write == {"status": "ok", "id": identifier, "sequence": "2"}, write
    read = search.invoke({"vector": updated, "k": 1})
    assert read == {"status": "ok", "snapshot_sequence": "2", "neighbors": [
        {"id": identifier, "version": "2", "squared_distance": 0.0},
    ]}, read
    bad_query = search.invoke({"vector": [0.0], "k": 1})
    assert bad_query["code"] == "invalid_argument", bad_query
    print(json.dumps({"framework": "langchain-core",
                      "version": importlib.metadata.version("langchain-core"),
                      "cases_passed": 6, "directory": str(run)}))


if __name__ == "__main__":
    main()
