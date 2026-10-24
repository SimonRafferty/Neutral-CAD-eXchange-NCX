# NCX: Neutral CAD eXchange

**Move a complete parametric CAD model between CAD systems, and between versions of the same system, in one file.**

An NCX file is an ordinary STEP file. Any CAD program opens it as plain geometry. But it also carries, inside the same
file, a neutral record of *how the model was built*: sketches with their relations and dimensions, features,
parameters and equations, configurations, derived parts, assemblies and mates, custom properties, materials and
appearances. A CAD system with an NCX add-in rebuilds the model from that record, natively, with its feature history,
and checks the result against the file's own geometry.

What it is for:

- **Back-saving:** open a current SolidWorks model in an older SolidWorks version (tested back to 2018), as a real parametric model (SolidWorks itself
  cannot save to an older version).
- **Moving between CAD systems:** SolidWorks to Fusion, Onshape, or any other system someone writes an add-in for - any CAD, any version to any other.
- **One file, always:** the geometry and the intent travel together and can never get separated.

If you have received an NCX file and your CAD system has no NCX add-in, you can still open it as plain geometry. The
"READ ME" plate beside the model is the notice NCX adds for you; delete it and you have the plain model.

## Status

| CAD system | add-in | download | state |
|---|---|---|---|
| SolidWorks | `sw/` | [0.5.1](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/tag/v0.5.1) | import and export; a 56-model test corpus rebuilds exactly in 2018 and 2026 |
| Autodesk Fusion | `fusion/` | [0.1.0](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/tag/fusion-v0.1.0) | import and export, written by an AI agent from this repo alone (no SolidWorks): of the 56 test files, 25 rebuild exactly and 31 approximately (sheet-metal cut lists, jogs and other features Fusion lacks are built as functional equivalents), none fall back to plain geometry |
| Onshape | | | planned |
| *your CAD system* | | | see "Adding a CAD system" below |

What carries across, what is approximated and what is not yet carried is listed in
[`docs/COVERAGE.md`](docs/COVERAGE.md).
Every import reports, feature by feature, anything that did not carry exactly and why.

## How it works

- **The container** ([`FORMAT-SPEC.md`](FORMAT-SPEC.md) §1-2): STEP AP214 geometry, plus one STEP property holding the
  payload (compressed, base64, in chunks small enough for every reader tested). A "READ ME" notice body tells readers
  without an add-in what the file is.
- **The payload** (§4, [`schema/`](schema/ncx-0.1.schema.json)): a versioned JSON log of the model.
- **References are geometric** (§5): a fillet's edges, a mate's faces and so on are described by their geometry, not
  by internal IDs, and found again in the rebuilt model by the shared matcher. This is what makes the record portable
  between kernels.
- **Verification** (§6): every import compares the rebuilt model with the file's geometry (mass properties, face by
  face, structure) and, where the source recorded them, checks that the parameters still drive the model the same
  way (parameter flex). A part that cannot be rebuilt faithfully is replaced by its plain geometry, with the reason.
- **The shared core** ([`core/`](core/)): one C++ library with a C interface does the container, validation,
  fingerprints, reference matching and verification for every add-in, so they cannot drift apart.

## Installing the SolidWorks add-in

1. Download the installer: **[NCX-Translator-Setup-0.5.1.exe](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/download/v0.5.1/NCX-Translator-Setup-0.5.1.exe)**
   ([release page](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/tag/v0.5.1)).
2. Close SolidWorks, run the downloaded file and follow the steps. It works with SolidWorks 2018 and every later
   version, and asks for administrator permission once.
   If Windows says "Windows protected your PC", click **More info**, then **Run anyway** (the installer is not
   signed with a paid certificate).
3. Start SolidWorks. The commands are under **Tools > NCX Translator**: *Export Neutral (NCX)...* and
   *Import Neutral (NCX)...*.

To remove it, uninstall **NCX Translator for SOLIDWORKS** from Windows' *Installed apps* like any other program.

**Building it yourself** (developers): `installer\build.bat` builds the add-in and the installer. It needs the Visual
Studio Build Tools, a SolidWorks 2018 install (for its interop libraries), the core built first, and Inno Setup 6.

## Installing the Fusion add-in

1. Download the installer: **[NCX-Fusion-Setup-0.1.0.exe](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/download/fusion-v0.1.0/NCX-Fusion-Setup-0.1.0.exe)**
   ([release page](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/tag/fusion-v0.1.0)).
2. Run it and click **Install**, then **Finish**. If Fusion is open, it asks you to close it first. No administrator
   permission is needed.
   If Windows says "Windows protected your PC", click **More info**, then **Run anyway** (the installer is not
   signed with a paid certificate).
3. Start Fusion. The commands are in the **File** menu: *Import NCX...* and *Export NCX...*.

To remove it, uninstall **NCX for Autodesk Fusion** from Windows' *Installed apps*.

Details, building it yourself and what it carries: [`fusion/README.md`](fusion/README.md).

## Adding a CAD system

NCX is meant to grow by people adding add-ins for the CAD systems they use, and the repository is organised so that an
AI coding agent can do most of that work:

1. **Fork this repository** (or create a branch) and give the new add-in its own top-level folder.
2. **Hand [`docs/ADDING-A-CAD-SYSTEM.md`](docs/ADDING-A-CAD-SYSTEM.md) to your coding agent** (it was written for
   Claude Code) together with the repository. It is the complete brief: what to build, in what order, how to test it
   against sample NCX files, and the rules.
3. **Where a feature has no counterpart** in your CAD system, the add-in builds something functionally the same
   (same geometry, same behaviour when parameters change) and reports it as approximate. Weldments in Fusion, for
   example, become swept bodies.
4. **If the format or the core needs to change**, raise an Issue on this repository rather than changing them in
   your fork: every add-in depends on them.
5. **Open a pull request** with your add-in folder so others can use it.

## Repository

| path | contents |
|---|---|
| `FORMAT-SPEC.md` | the format specification (normative) |
| `schema/` | JSON Schema of the payload |
| `core/` | the shared core library (C++17, C ABI) and its bindings |
| `conformance/` | conformance vectors for container readers |
| `validator/` | checks any NCX file against the format, without a CAD system: use it on every export |
| `sw/` | the SolidWorks add-in (C#, .NET Framework 4.8) and its development driver |
| `docs/` | how to add a CAD system; what the SolidWorks add-in carries |
| `python/` | the core's Python binding and an OpenCASCADE geometry host (a reference for writing host code) |

The whole source is included, so that anyone (or any AI agent) extending NCX can see how each part of the
specification was realised.

## How this was made

NCX was designed and written by **Claude Code** (Anthropic's AI coding agent), working with a human engineer who set
the goals, the modelling practice it must support, and the decisions. It was deliberately written so that another
AI agent can extend it: the specification is complete and normative, the core is shared, the SolidWorks add-in is a
reference implementation, and adding a CAD system is a brief an agent can follow.

## Licence

MIT: see [`LICENSE`](LICENSE).
