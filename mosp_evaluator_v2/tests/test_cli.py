#!/usr/bin/env python3
"""CLI orientation, index conversion and failure checks; no third-party modules."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
checks = 0
with tempfile.TemporaryDirectory(prefix="mosp-cli-") as tmp:
    root = Path(tmp)
    matrix = root / "matrix.txt"
    sequence = root / "solution.txt"
    matrix.write_text("3 2\n1 0\n1 0\n0 1\n")
    sequence.write_text("0 2 1\n")
    common = [binary, "--matrix", str(matrix), "--solution", str(sequence),
              "--axis", "rows", "--presence", "one", "--profile"]

    def run(args=(), *, good=True, command=None):
        global checks
        p = subprocess.run(common + list(args) if command is None else command,
                           text=True, capture_output=True)
        if good:
            assert p.returncode == 0, p.stderr
            checks += 1
            return json.loads(p.stdout)
        assert p.returncode == 2 and not p.stdout and p.stderr, (p.returncode, p.stdout, p.stderr)
        checks += 1

    result=run()
    assert {k:result[k] for k in ['objective','patterns','items','profile']} == {"objective": 2, "patterns": 3, "items": 2, "profile": [1, 2, 1]}
    assert result['total_open']==4 and result['energy']==2
    for backend in ['full','incremental','auto']:
        score=run(['--evaluator',backend,'--energy','mmosp'])
        assert score['objective']==2 and score['total_open']==4 and abs(score['energy']-8/3)<1e-14
    run(['--evaluator','invalid'],good=False)
    run(['--energy','invalid'],good=False)
    sequence.write_text('1 0\n')
    decoded=run(['--sequence','items'])
    assert decoded['objective']==1 and decoded['total_open']==3
    sequence.write_text('0 2 1\n')
    sequence.write_text("1 3 2\n")
    assert run(["--index-base", "1"])["objective"] == 2
    sequence.write_text("0 1 2\n")
    assert run()["objective"] == 1
    transposed = root / "transpose.txt"
    transposed.write_text("2 3\n1 1 0\n0 0 1\n")
    cols = [binary, "--matrix", str(transposed), "--solution", str(sequence),
            "--axis", "columns", "--presence", "positive", "--profile"]
    assert run(command=cols)["profile"] == [1, 1, 1]
    sequence.write_text("0 0 2\n")
    run(good=False)
    sequence.write_text("0 1\n")
    run(good=False)
    sequence.write_text("0 1 2 junk\n")
    run(good=False)
    sequence.write_text("0 1 2\n")
    run(["--axis", "columns"], good=False)  # Duplicate flags are rejected.
    run(["--index-base", "2"], good=False)
    run(["--index-base", "1"], good=False)
    matrix.write_text("-1 2\n")
    run(good=False)
    matrix.write_text("3 2\n1 0 1 0 0\n")
    run(good=False)
    matrix.write_text("3 2\n1 0 1 0 0 1 extra\n")
    run(good=False)
    matrix.write_text("0 0\n")
    sequence.write_text("")
    assert run()["objective"] == 0
    matrix.write_text("3 2\n2 0 2 0 0 1\n")
    sequence.write_text("0 2 1\n")
    assert run()["objective"] == 1
    positive = common.copy()
    positive[positive.index("one")] = "positive"
    assert run(command=positive)["objective"] == 2
print(json.dumps({"cli_status": "PASS", "cli_cases": checks}))
