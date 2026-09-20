"""Strip Private-only experiment/cache bits from the current worktree. Not shipped."""

from __future__ import annotations

import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]


def strip_cmakelists(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    text = re.sub(
        r"\noption\(\s*\n\s*SKIP_RUNTIME_DOWNLOADS\n"
        r'\s*"Skip redistributable runtime downloads for DLL-only development builds\."\n'
        r"\s*OFF\n\)\n",
        "\n",
        text,
        count=1,
    )
    text = text.replace(
        'message("\\tSkip runtime downloads: ${SKIP_RUNTIME_DOWNLOADS}")\n',
        "",
        1,
    )
    text = re.sub(
        r"if\(NOT SKIP_RUNTIME_DOWNLOADS\)\n((?:    .*\n)*)endif\(\)\n",
        lambda m: "".join(
            line[4:] if line.startswith("    ") else line
            for line in m.group(1).splitlines(True)
        ),
        text,
        count=1,
    )
    path.write_text(text, encoding="utf-8", newline="\n")


def strip_integration(path: pathlib.Path) -> None:
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    text = text.replace('#include "ExperimentCapture.h"\n', "")
    text = re.sub(r"^[ \t]*bool captureStarted = false;\n", "", text, flags=re.M)
    text = re.sub(
        r"^[ \t]*if \(captureStarted\)\n[ \t]*ExperimentCapture::Complete\([^;]*\);\n",
        "",
        text,
        flags=re.M,
    )
    text = re.sub(
        r"^[ \t]*(?:const bool |bool )?captureStarted = ExperimentCapture::BeginIfRequested\([^;]*\);\n",
        "",
        text,
        flags=re.M,
    )
    text = re.sub(
        r"^[ \t]*if \(captureStarted && !succeeded\)\n[ \t]*ExperimentCapture::Complete\([^;]*\);\n",
        "",
        text,
        flags=re.M,
    )
    text = re.sub(r"^[ \t]*ExperimentCapture::Complete\([^;]*\);\n", "", text, flags=re.M)
    text = re.sub(r"^[ \t]*ExperimentCapture::Reset\(\);\n", "", text, flags=re.M)
    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    strip_cmakelists(ROOT / "CMakeLists.txt")
    strip_integration(ROOT / "src/Features/Upscaling/NeuralRendering/Integration.cpp")


if __name__ == "__main__":
    main()
