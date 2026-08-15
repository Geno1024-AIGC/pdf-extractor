# pdf-extractor

A small C++/Qt tool that extracts streams from PDF files. No bundled runtime or
virtual machines — it links against the system Qt (5.x) and zlib only.

## Build

Requires CMake, a C++17 compiler, Qt5 Widgets headers and zlib.

```sh
cmake -B build && cmake --build build
```

The helper `build.sh` additionally supports building against a self-contained
Qt5 + zlib dev tree when system dev packages are unavailable (sets `-DMY_SDK`).

## CLI usage

```sh
build/pdfx list <file.pdf>                 # list all parsed objects
build/pdfx info <file.pdf>                 # show xref table / trailer info
build/pdfx extract <file.pdf>              # extract every stream into CWD
build/pdfx extract <file.pdf> 3 5          # extract objects 3 and 5 only
```

`extract` writes decoded stream content as `obj_<number>.<ext>`. The extension
is guessed from `/Subtype`, filter names and content magic bytes (png/jpg/pdf/...);
fallbacks are `txt` for printable text and `raw` when decoding fails.

## GUI usage

```sh
build/pdfx                # open the GUI
build/pdfx <file.pdf>     # open the GUI with a file
```

- **Open PDF** loads a file; the object table lists every parsed object (id,
  type, subtype, filter, raw size, offset).
- Select a row to see the decoded stream in the **Preview** tab: text streams
  render with line numbers, image streams (JPEG/PNG from DCTDecode/FlateDecode)
  are rendered as pictures, and other binary data is shown as a hexdump.
- The **Info** tab shows file structure: the classic xref table, the trailer
  dictionary (`/Size`, `/Root`, `/Info`, `/Prev`) and how many scanned objects
  are confirmed by the xref.
- **Extract Selected** writes the selected stream to a chosen directory.
- The **command line** at the bottom runs the same commands as the CLI
  (`list`, `info`, `preview <n>`, `extract <n>`, `extractall`, `out <dir>`,
  `open <f>`).

## Supported filters

- FlateDecode
- ASCIIHexDecode
- ASCII85Decode
- RunLengthDecode

Unknown filters are reported and the raw stream is exported with a `.raw`
extension. Indirect `/Length` references are resolved.

## Tests

`tools/make_test_pdf.py` generates a small PDF exercising dicts, FlateDecode
streams (direct and indirect `/Length`). `tests/selftest.cpp` is a standalone
parser smoke test:

```sh
g++ -std=c++17 -Isrc src/pdfparser.cpp src/filters.cpp tests/selftest.cpp -o selftest -lz
./selftest test.pdf
```