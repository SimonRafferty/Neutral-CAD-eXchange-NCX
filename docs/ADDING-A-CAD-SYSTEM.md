# Adding a CAD system to NCX: a plan for an AI coding agent

This file is written to be handed, as is, to an AI coding agent (it was developed with Claude Code) together with
this repository. It is the complete brief for building an NCX add-in for a CAD system that does not have one yet.
A human can follow it too.

> **To the agent:** read this whole file first, then `README.md`, then `FORMAT-SPEC.md` in full. Do not start
> writing code before you have read the spec. Everything you need is in this repository; you do not need, and
> must not assume, any other CAD system on the machine. Work as a user would who has received NCX files from
> someone else and has only the target CAD system installed.

## 1. What you are building

NCX carries a complete parametric CAD model between CAD systems and versions in **one** STEP file:

- any CAD program opens the file as plain geometry (STEP AP214 B-rep with assembly structure);
- the file also carries an embedded payload: a neutral, versioned log of how the model was built (sketches with
  relations and dimensions, features, parameters and equations, configurations, derived parts, assemblies and
  mates, properties, materials, appearances) plus fingerprints of the geometry;
- an NCX add-in **imports** by replaying that log natively, so the user gets a real parametric model with a
  feature history, and **exports** by writing its own models the same way.

Your add-in for the target CAD system needs two commands:

1. **Import NCX**: read the payload, rebuild the model with its history in the target system, verify it against
   the file's own geometry, and show a short report only when something was approximated or failed.
2. **Export NCX**: walk the target system's model and write the neutral log plus the STEP geometry.

Import is the priority: it is what lets someone who received an NCX file use it. Build import first, then export.

## 2. Where things are

| path | what it is |
|---|---|
| `FORMAT-SPEC.md` | **The specification.** Container, payload encoding, model schema, references, fingerprints, verification. Normative. |
| `schema/ncx-0.1.schema.json` | JSON Schema of the payload model |
| `core/` | `ncx_core`: C++17 library behind a plain C ABI (`core/include/ncx_core.h`). Container read/write, validation, fingerprint compare, the reference matcher, face-by-face compare, notice body. **Use it; do not reimplement it.** |
| `core/bindings/csharp/NcxCore.cs`, `python/ncx/core.py` | bindings to the core (C# P/Invoke, Python ctypes); a C++ host links the core directly |
| `conformance/` | test vectors: STEP files with their expected read status and payload |
| `python/ncx/occhost.py` | a complete geometry "host" written against OpenCASCADE: how to describe faces/edges and measure candidates for the matcher. A good model for your own host code. |
| `sw/` | the SolidWorks add-in (C#): the reference implementation of export and import. Read it to see how each spec section was realised. |
| `validator/` | `ncx_validate.py`: checks any NCX file against the format without a CAD system (container, schema, consistency, geometry). Run it on every file your exporter writes. |
| `docs/COVERAGE.md` | what the SolidWorks add-in carries, its test corpus and its known gaps |
| sample NCX files | exported from SolidWorks, one per feature area: your test inputs (ask the repository owner, or export your own with the SolidWorks add-in) |

## 3. Rules

- **The format is shared.** Never change the payload format, the schema or the core's behaviour on your own.
  If you find a gap or an error in the spec or the core, **raise a GitHub Issue** on the main repository (what is
  missing, the evidence, a proposal) and work around it locally until it is resolved. Changes to the format must
  keep every existing add-in working.
- **Fork or branch, then share.** Develop the new add-in in a fork (or a branch) of the main repository, in its own
  top-level folder (for example `fusion/`, `onshape/`, `freecad/`), so other people can find and use it. Open a pull
  request when it works; keep the format and core untouched in that pull request.
- **Functionally the same, not necessarily built the same.** Not every feature exists in every CAD system. When the
  target has no direct counterpart, build something that produces **the same geometry and behaves the same when
  its parameters change**, using the target's own tools, and report it as `approximate` with the reason. Examples:
  - *Weldments* (structural members along sketch segments): build each member as a swept or extruded body with
    the profile's cross-section, trimmed as the payload describes; keep the member names.
  - *Hole Wizard* holes: build the hole with the target's hole tool if it has one, else as revolve/extrude cuts of
    the same section.
  - *Sheet metal* without a sheet-metal environment: a solid of the same shape; say the bends are not live.
  - *Mates* in a system that positions components with joints: derive the joints from the mates (degrees of freedom
    removed), falling back to a rigid joint at the recorded placement.
  - *Configurations* in a system without them: the active configuration's values, and say so.
  - *Anything else with no counterpart* (a hem, a jog, a gusset, a dome, a wrap, or a feature the exporter left
    `opaque`): when the feature carries a `result` (spec §4, "Feature results"), import those bodies with the
    target's STEP reader in place of the bodies the feature changed, and carry on with the rest of the history.
    This is the universal "something simpler". Implement it early (Step 3), because it lets every later feature
    rebuild even where one step cannot.
  - *Mates that only couple motion* (gear, screw, rack and pinion, cam): if the target cannot make one, leave the
    components where the file places them and report "placed, not coupled".
  Never drop something silently: every payload item ends up as rebuilt, approximate (with the reason) or failed
  (with the reason).
- **Geometry wins.** The import is only right if the rebuilt geometry matches the payload's fingerprints (see §6 of
  the spec). If a feature cannot be rebuilt so that it matches, the importer falls back to the file's own plain
  geometry for that part and says why. A wrong parametric model is worse than an honest plain one.
- **References are geometric.** Faces, edges and vertices are identified by geometry, never by IDs (spec §5). Use the
  core's matcher (`ncx_reference_match`) with candidates you measure in the target system; only a `resolved` result
  may be used to build a feature.
- **Units.** The payload is in millimetres and degrees. Many CAD APIs use other units internally (Fusion: centimetres,
  radians); convert at the boundary and nowhere else.
- **Coordinate frames.** Sketches carry an explicit frame (origin, x, z). A target sketch's own axes may differ;
  map through the frame (see the SolidWorks `SketchFrame`).
- **The notice body.** Every NCX file carries a "READ ME" body (spec §2.6). The import must delete it from any geometry
  it imports and must never rebuild it.
- **No hidden dependencies.** The add-in must run on a machine with only the target CAD system installed.
- **The file's own geometry fills the gaps.** At import you have the file's B-rep as well as the payload. Where the
  payload lacks something your system needs (a size another system takes from its own tables, an orientation),
  measure it on that geometry and report the feature `approximate` with what you measured.
- **What the spec does not describe, `sw/Ncx.Sw/Export*.cs` does.** Several feature definitions are only written
  down by the SolidWorks exporter that writes them. Read the matching `Export*.cs` method rather than guessing
  from the samples, and raise an Issue so the spec gets the definition.

## 4. Plan

Work in this order. Keep notes as you go (what you tried, what the target's API did, what failed), and write them to
`<your-folder>/FINDINGS.md`: the next agent will thank you.

### Step 1: understand
1. Read `FORMAT-SPEC.md` completely, then the schema.
2. Read the SolidWorks importer (`sw/Ncx.Sw/Replayer.cs` and the `Replay*.cs` files) to see how each part of the spec
   is used, and `sw/Ncx.Sw/Host.cs` for how geometry facts are measured for the core.
3. Open the sample NCX files in the target CAD system as plain STEP, to see the geometry and the notice body.
4. Decide the add-in technology (the target's preferred, version-stable route: e.g. C++ for Fusion, as its
   bundled Python changes with updates) and how it will call `ncx_core` (link the C ABI directly from C++).

### Step 2: read a file
0. **Make the add-in drivable without clicks** before anything else: you will run it hundreds of times. If the
   target system has an out-of-process API, drive it from a script. If it does not (Fusion), make the add-in watch a
   job folder: a watcher thread notices a JSON job file, hands it to the CAD system's main thread (Fusion:
   `Application.fireCustomEvent`), runs it there and writes a result file. Load the real implementation from a copy
   of its DLL, so a rebuild is reloaded without restarting the CAD system. Loading a new add-in the first time
   usually needs a restart of the CAD system: check first that no document has unsaved changes.
1. Build the core (`core/build.ps1`, or CMake on other platforms) and call `ncx_payload_read` on every sample and on
   `conformance/vectors`; check the statuses match `conformance/vectors/vectors.json`.
2. Import the file's STEP geometry with the target's own importer as the **reference** (used for the stale check, the
   face check and the plain-geometry fallback); delete the notice body from it. Find it by name (spec §2.6), or,
   if your STEP importer drops body names (Fusion's does), by the header's `noticebox`: the solid with that box.
   Check how your system measures an imported body: some report zero mass properties for a body without a material
   (Fusion: give it one, any density, before measuring). Fingerprints cover solids only (§6).
3. Implement the **stale check** (spec §6): fingerprint the imported bodies (volume, area, centroid, principal moments,
   box, in mm) and compare with the payload's fingerprint through `ncx_fingerprint_compare_ex` with the `exchange`
   profile. A mismatch means: import the plain geometry and say why.

### Step 3: rebuild parts
Implement in this order, testing each on its sample before moving on:
1. sketches: lines, arcs, circles, splines, ellipses, points; relations; dimensions; the frame mapping; seeded
   coordinates (place every entity where the payload says, then add relations and dimensions);
2. extrude/cut (end conditions, thin, start conditions, selected contours), revolve;
3. reference planes, axes, points, coordinate systems;
4. fillets, chamfers; patterns, mirrors; shell, draft;
5. equations, parameters, configurations;
6. derived parts (`insertPart`), multibody features;
7. sweep, loft, sheet metal, hole wizard, weldments (functionally equivalent where needed);
8. properties, materials, appearances.
From step 1 on, any feature you have not implemented yet (or whose rebuild fails) takes its carried `result` when
it has one. Exporters attach results only to opaque features and to the feature types few systems share (spec §4),
so the core types above must still be rebuilt properly.
After each part is rebuilt, **verify** it (spec §6.1): mass properties (`native` tolerances), face by face
(`ncx_face_compare` with your measured candidates), structure (every payload feature present under its name), and
the flex probes when the file carries them (spec §6.4).

### Step 4: assemblies
Components with their placements and fixed state, nested sub-assemblies, mates (or joints derived from them).
- If your system's multi-document features (derived parts, external references) need documents saved somewhere
  first (Fusion's Derive needs saved cloud documents), building the whole file in one document is acceptable.
  Insert Part is then rebuilt as the source's sketches in the part, with their dimensions linked to the source's
  parameters: the layout still drives the part, and the flex probes prove it. Report it `approximate`.

### Step 5: the user's side
- A minimal UI: two commands (Import NCX, Export NCX) where the target system puts such commands. Show progress
  for long jobs with a cancel that really stops and leaves a defined state.
- The report: one line per feature (rebuilt / approximate / failed / fallback, with the reason). Show it only when
  something needs attention; otherwise a one-line status.

### Step 6: export
Walk the target's model and write the payload: the reverse of every step above, with descriptors measured in the
state each feature saw, fingerprints, and flex probes where the spec says. Export the STEP geometry with the target's
own exporter (AP214 when available) and let `ncx_payload_write` embed the payload and the notice body.
Round-trip test: export from the target, import in the target, and compare.
**Run `validator/ncx_validate.py` on every file you export** and fix every error before anything else: it checks
what other add-ins rely on (units, frames, seeds against dimensions, references, fingerprints against the geometry),
which your own importer may not notice because it shares your exporter's assumptions.

## 5. Testing

- Every sample must import with every check matching, or with an honest `approximate`/`fallback` report saying exactly
  what and why. Record the results per sample in `<your-folder>/FINDINGS.md`.
- Every file your exporter writes passes `validator/ncx_validate.py` (verdict PASS).
- Re-export what you imported and compare the payloads (feature names, types, sketch contents, parameters). A
  re-export keeps every feature under its name, with its type where your system has it; a feature it cannot map
  goes out `opaque` with its `result` carried (spec §4), never dropped. The re-import must verify.
- When the target system and SolidWorks disagree on a construction, the geometry decides.

## 6. Done means

- Import works on all samples (matching, or reported and justified).
- Export works and round-trips in the target system.
- `<your-folder>/README.md` says how to build and install the add-in; `<your-folder>/FINDINGS.md` records what was
  learned, including every feature translated by functional equivalence and why.
- Issues are raised for anything in the spec or core that had to be worked around.
- A pull request from your fork/branch adds the add-in folder only.

## 7. Traps by CAD system

Each add-in's `FINDINGS.md` records the traps its CAD system set. Read the one for your system if there is one, and
add yours. Ones that cost a debugging round each:

- **Fusion** (`fusion/FINDINGS.md`): no out-of-process API (job folder, above); STEP import drops body names (find
  the notice by `noticebox`); bodies without a material have zero mass properties; sketches on faces project the
  face's edges by themselves; a one-direction pattern defaults to 3 instances in its second direction; pattern
  instances cannot be suppressed through the API; moving an occurrence in a parametric design is only a pending
  position until captured; MeasureManager measures only entities in the root's context; parameter names that are
  unit symbols (`W`, `H`) are refused; centimetres and radians internally; a sketch on the XZ plane has its y axis
  along -Z (map everything through the payload's `frame`).
- **SolidWorks** (the comments in `sw/Ncx.Sw`): equation edits are refused unless the
  document is active; any rebuild replaces body objects; arrays of SolidWorks objects must be typed dispatch arrays
  out of process.
