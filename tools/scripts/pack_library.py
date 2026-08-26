import argparse
import json
from pathlib import Path


def pack_library(lib_dir: Path, output_js: Path) -> int:
    library_data = []

    library_data.append({
        "category": "System",
        "name": "Silence",
        "author": "OmniBox",
        "mode": 0,
        "speed": 1.0,
        "code": "0"
    })

    for filepath in sorted(lib_dir.rglob("*.txt")):
        category = filepath.parent.name
        content = filepath.read_text(encoding="utf-8")

        name = "Unknown"
        author = "Unknown"
        sample_rate = 8000
        mode_str = "bytebeat"

        for line in content.splitlines():
            if line.startswith("// Name:"):
                name = line.split(":", 1)[1].strip()
            elif line.startswith("// Author:"):
                author = line.split(":", 1)[1].strip()
            elif line.startswith("// Sample Rate:"):
                try:
                    sample_rate = int(line.split(":", 1)[1].strip())
                except ValueError:
                    pass
            elif line.startswith("// Mode:"):
                mode_str = line.split(":", 1)[1].strip().lower()

        mode_int = 2 if "signed bytebeat" in mode_str else 0
        if "floatbeat" in mode_str or "funcbeat" in mode_str:
            mode_int = 1

        library_data.append({
            "category": category,
            "name": name,
            "author": author,
            "mode": mode_int,
            "speed": round(sample_rate / 48000.0, 4),
            "code": content
        })

    output_js.write_text(
        "const LIBRARY = " + json.dumps(library_data, indent=2, ensure_ascii=False) + ";\n",
        encoding="utf-8"
    )
    return len(library_data)


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack the Bytebeat text library into a JavaScript data file.")
    default_library = Path(__file__).resolve().parents[2] / "examples" / "web_daw" / "tools" / "dollchan_library"
    parser.add_argument("library_dir", nargs="?", type=Path, default=default_library)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    library_dir = args.library_dir.resolve()
    output_js = (args.output or library_dir / "library_data.js").resolve()
    output_js.parent.mkdir(parents=True, exist_ok=True)
    count = pack_library(library_dir, output_js)
    print(f"Packed {count} songs into {output_js}")


if __name__ == "__main__":
    main()
