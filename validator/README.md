# NCX validator

Checks whether an NCX file meets the format (`FORMAT-SPEC.md`), so that any NCX add-in can import it, without any
CAD system. Use it on every file your exporter writes: it is how you know your export will work with add-ins you
don't have.

```
python validator/ncx_validate.py <file.ncx.step> [more files] [--json report.json] [--max-level N] [--quiet]
```

The exit code is 0 only when every file passes. `--json` writes one report per file, for an AI agent or a test to
read.

## Verdicts

- **PASS:** every level ran and nothing breaks the format.
- **FAIL:** at least one error. An error breaks a MUST of the spec, or would make an importer fall back to plain
  geometry or build something wrong.
- **INCOMPLETE:** no errors, but a level could not run (the core is not built, or OpenCASCADE is not installed).

Warnings are SHOULDs and likely mistakes; they never fail a file.

## Levels

Each level needs the one before it.

| level | checks | needs |
|---|---|---|
| 1 container | Part 21 structure; AP214 declared; no string over 8,000 characters; the payload on the root product, in the §2.1 shape; header length and keys; chunk names, sizes and order; the payload decodes (deflate, base64, SHA-256); the core and the independent reader agree; the header's `fp` is the payload fingerprint's hash; a notice body is declared | Python |
| 2 schema | the payload against `schema/ncx-0.1.schema.json` | `jsonschema` |
| 3 consistency | the core's structural rules (ids, document links, cycles); units; references by id resolve and point back in the history; sketch frames are perpendicular unit axes; **sketch seeds agree with their relations and dimensions** (catches unit and frame mistakes); geometric descriptors are well formed (samples on their own surface and in their own box, unit directions, no seams); component transforms are rigid; configurations, equations (the neutral grammar) and flex probes refer to things that exist; fingerprints are sane; the file fingerprint is the documents' bodies placed by the assembly; feature results are well formed | the built core |
| 4 geometry | the file's own B-rep measured with OpenCASCADE: the notice body is there and is one solid; the **stale check** (the payload's fingerprint against the geometry, `exchange` tolerance); every feature result's STEP reads and is what its fingerprints say; mate references (in their component's frame) and face appearances resolve with the shared matcher | the built core, `cadquery-ocp` |

Every finding has a stable `code` (for example `M-SEED-DIMENSION`), the spec section it enforces, the JSON path it
is about, and a message saying what is wrong.

## Setup

- Python 3.10 or later, with `pip install jsonschema cadquery-ocp` (the versions this was tested with are in
  `requirements.lock.txt` at the repository root, if present: jsonschema 4, cadquery-ocp 7.9).
- The core: `core/build.ps1` (Windows; CMake elsewhere). The validator finds `core/build/Release/ncx_core.dll`, or
  the library named by `NCX_CORE_DLL`.

## Limits

- It cannot prove that the design intent rebuilds: it checks that the file is consistent and that its geometry is
  what the payload says. A reference rebuild of the common features in OpenCASCADE (sketch, extrude, revolve, fillet,
  chamfer, patterns) is planned as level 5.
- References recorded in the state a feature saw (most part-feature references) are checked for form only; references
  measured in the finished model (mates, face appearances) are resolved in the file's geometry.
- **Freeform faces:** a STEP spline surface measures slightly differently in another kernel. The validator marks each
  solid with such a face `freeform`, so the stale check applies the spec's freeform tolerances (FORMAT-SPEC 6), as an
  importer would.
