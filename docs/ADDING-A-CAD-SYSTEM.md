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
| sample NCX files | exported from SolidWorks, one per feature area: your test inputs. `NCX-samples-<version>.zip` on the latest SolidWorks add-in release (GitHub Releases); or export your own with the SolidWorks add-in |

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
  map through the frame (see the SolidWorks `SketchFrame`). The hard part is knowing your system's own frame for a
  sketch, especially on a face: before writing the sketch translator, measure it with one throwaway sketch each on
  every default plane, on a face in each axis direction and on an offset plane.
- **Origin planes by geometry, not by name.** `originPlane` names are defined by geometry in spec §5 (`front` = XY,
  `top` = XZ, `right` = YZ). Your system may give the same words to other planes (Onshape's *Front* is XZ).
- **Find the target's machine-readable feature definitions first.** Many CAD APIs publish every feature's parameter
  names, enum values and types (Onshape: `featurespecs` plus its OpenAPI file). Cache them and check every feature
  you create against them before sending it; it makes nearly every feature right first time.
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
   bundled Python changes with updates) and how it will call `ncx_core` (link the C ABI directly from C++). A
   cloud-only system may leave no choice: if its own scripting cannot read a local file or create features (Onshape's
   FeatureScript cannot), the importer is a client of its web API, and a real distribution is an app of that system
   (sign-in, a service running your code, a panel for the UI). Build the client and describe the distribution.
5. **Metered APIs (cloud systems): find the quotas before designing anything.** Many limit calls per account and per
   endpoint, often without publishing the per-endpoint numbers (Onshape, measured 2026-10, may change: FeatureScript
   evaluation 100 a day; reading a Part Studio's feature list 100 per window; `bodydetails` 500 per window;
   translations and STEP exports 10 per window, refilled within minutes; no header reports the per-key or annual
   total). Read the rate-limit headers (`X-Rate-Limit-Remaining`, `Retry-After`) on every response from the first
   request and log them per endpoint with a count of your own; never let the design depend on one quota-limited
   endpoint. Know what the
   measurements are *for*, so you can take each from its cheapest source: the matcher needs each candidate's type,
   parameters, size and distances to the samples; the mass check needs volume, area, centroid and principal moments.
   A B-rep description endpoint (Onshape's `bodydetails`) often has all of the matcher's facts.

### Step 2: read a file
0. **Make the add-in drivable without clicks** before anything else: you will run it hundreds of times. If the
   target system has an out-of-process API, drive it from a script. If it does not (Fusion), make the add-in watch a
   job folder: a watcher thread notices a JSON job file, hands it to the CAD system's main thread (Fusion:
   `Application.fireCustomEvent`), runs it there and writes a result file. Load the real implementation from a copy
   of its DLL, so a rebuild is reloaded without restarting the CAD system. Loading a new add-in the first time
   usually needs a restart of the CAD system: check first that no document has unsaved changes.
   With a metered API you cannot run it hundreds of times: make every run cheap and resumable instead (cache every
   response on disk, never upload the same file twice, delete your own earlier attempt, work out locally all you can).
   A cache must be order-aware: the same request made in another model state (a flex probe repeats a measurement)
   must be replayed in sequence, not matched by content, or a flexed measurement ends up in an export.
1. Build the core (`core/build.ps1`, or CMake on other platforms) and call `ncx_payload_read` on every sample and on
   `conformance/vectors`; check the statuses match `conformance/vectors/vectors.json`.
2. Import the file's STEP geometry with the target's own importer as the **reference** (used for the stale check, the
   face check and the plain-geometry fallback); delete the notice body from it. Find it by name (spec §2.6), or,
   if your STEP importer drops body names (Fusion's does), by the header's `noticebox`: the solid with that box.
   Check how your system measures an imported body: some report zero mass properties for a body without a material
   (Fusion: give it one, any density, before measuring). Fingerprints cover solids only (§6).
   Where importing into the target is expensive (a metered translation), read the reference locally instead: the
   repository's OpenCASCADE host (`python/ncx/occhost.py`) gives its face descriptors and fingerprint for nothing.
   Import it into the target only when it is needed as the plain-geometry fallback.
3. Implement the **stale check** (spec §6): fingerprint the imported bodies (volume, area, centroid, principal moments,
   box, in mm) and compare with the payload's fingerprint through `ncx_fingerprint_compare_ex` with the `exchange`
   profile. A mismatch means: import the plain geometry and say why.

### Step 3: rebuild parts
Implement in this order, testing each on its sample before moving on:
1. sketches: lines, arcs, circles, splines, ellipses, points; relations; dimensions; the frame mapping; seeded
   coordinates (place every entity where the payload says, then add relations and dimensions). Keep every payload
   point a separate entity, even where two share a position (spec §4, sketches): finding points again by position
   after drawing ties a relation to the wrong one. Relations to another sketch's entities (`sketchEntity`) may
   point off the sketch's plane (a layout point below a sheet face): relate to its projection. A fully defined
   source sketch must arrive fully defined: compare your solver's status with the payload's `constrained`;
   samples P47 (slots), P48 (polygons) and L11 (layout points into sheet metal) exercise these. P48's second
   hexagon has only an inscribed circle, and the relations carried cannot hold it regular: it arrives
   under-defined in every system (SolidWorks too). Report it; do not invent relations to make it fully defined;
2. extrude/cut (end conditions, thin, start conditions, selected contours), revolve;
3. reference planes, axes, points, coordinate systems;
4. fillets, chamfers; patterns, mirrors; shell, draft;
5. equations, parameters, configurations;
6. derived parts (`insertPart`), multibody features;
7. sweep, loft, sheet metal, hole wizard, weldments (functionally equivalent where needed);
8. properties, materials, appearances, cut lists: these are not features and are easy to forget. Report each of
   them per document (carried, approximate or dropped) even when you carry nothing, or a part with lost appearances
   reports "exact".
From step 1 on, any feature you have not implemented yet (or whose rebuild fails) takes its carried `result` when
it has one. Exporters attach results only to opaque features and to the feature types few systems share (spec §4),
so the core types above must still be rebuilt properly. A mapped feature can carry a `result` too (a sheet-metal
corner, a loft between unlike sections): rebuild it natively first and use the result only if your rebuild differs.
Sheet metal is where most of the per-system work goes: start from `bendGeometry` and the definitions in spec §4,
not from the source's option words.
**Use a carried result to check your own rebuild.** Where the payload does not pin down one of your system's
settings (which side a flange's bend goes, which datum its length is from, which way a loft lines up), build each
candidate and keep the one whose bodies equal the feature's `result` (fingerprint compare, `exchange` tolerance).
If none matches, use the result itself and report the feature `approximate`. Check *before* the result too: its
`replaces` fingerprint is the part as it was just before the feature, so it tells whether your earlier steps were
right (a thickness side, an extrude sense) at the moment the result is used; flip the earlier undetermined senses
until it matches. Measured placement fields
(`bendGeometry`, loft `connectors`) give the same check where no result is carried.
After each part is rebuilt, **verify** it (spec §6.1): mass properties (`native` tolerances), face by face
(`ncx_face_compare` with your measured candidates), structure (every payload feature present under its name), and
the flex probes when the file carries them (spec §6.4). If your system cannot give exact mass properties without
changing the user's model (a material it would have to assign, a centroid only from a mesh), spec §6.1 says which
quantities must still be exact and how to report the rest.
A carried result is cheap on a desktop system but may not be elsewhere: on Onshape a STEP enters a Part Studio only
through a translation job, which costs several requests and leaves an extra Part Studio in the document.

### Step 4: assemblies
Components with their placements and fixed state, nested sub-assemblies, mates (or joints derived from them).
- If your system's multi-document features (derived parts, external references) need documents saved somewhere
  first (Fusion's Derive needs saved cloud documents), building the whole file in one document is acceptable.
  Insert Part is then rebuilt as the source's sketches in the part, with their dimensions linked to the source's
  parameters: the layout still drives the part, and the flex probes prove it. Report it `approximate`.
- **Frame-based mate systems** (Onshape mate connectors, Fusion joints) join two coordinate frames, while the payload's
  mates carry faces, edges and planes. Use the mate's `frame` (spec §5, "Mates"): put a connector at that frame on
  each mated component (the frame taken into the component's own frame through its `transform`); both sides then
  coincide at the solved placement, with no axis sense to guess. A concentric + coincident pair is one revolute at
  the concentric mate's axis through the coincident mate's origin. Files without `frame` (SolidWorks add-in before
  0.5.2): put the connector at the centre of the circular edge where the cylinder meets the mated plane, build,
  read the placement back and flip the axis sense if the component moved (a revolute's free turn is not a move).
- **Suppressed mates** (`suppressed`, spec §5): make the mate and suppress it, or leave it out and say so. Either
  way every component stays at its `transform`: a suppressed mate may not hold in the placement (sample L11's
  `Distance-Lift`). Mates whose source lost their geometry are not in the file.
- **Assembly cuts** in a system whose assemblies cannot cut: cutting the part itself is right only when the cut
  changes every instance the same way. Otherwise report the cut failed and keep the instances uncut (each part still
  matches its own fingerprint).

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
A feature your importer built from a carried result has no native counterpart to export: write it `opaque` with
that same result again (you know its provenance), so the file stays rebuildable everywhere.
**Run `validator/ncx_validate.py` on every file you export** (it needs the core: build it with `core/build.ps1`, or
point `NCX_CORE_DLL` at your add-in's copy; a verdict of INCOMPLETE is not a pass) and fix every error before
anything else: it checks
what other add-ins rely on (units, frames, seeds against dimensions, references, fingerprints against the geometry),
which your own importer may not notice because it shares your exporter's assumptions.

### Step 7: package
Make the add-in installable by someone without build tools: the version set in one place; a build from a clean
tree (delete the build output first) that works exactly as your README says; the built files checked for runtime
dependencies a user would not have (link the C runtime statically, or ship it); a script that writes a zip of the
ready-to-install add-in folder; and install-from-zip instructions in your README, tested on a machine by installing
from the zip and importing a sample. The zip is published as a release asset, not committed. For a web-API client
the package is its scripts plus the core library; describe in the README what a real distribution (the system's own
app store, sign-in, a server) would need, and where progress and cancel would live in its UI.

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
- The add-in installs from its zip (Step 7).
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
  along -Z (map everything through the payload's `frame`). Fusion has a sheet-metal API (flanges, hems, unfold); a
  one-sided through-all cut on a sheet-metal face fails (a two-sided one works); its sheet-metal chamfer takes
  plain millimetre numbers, not centimetres; a split's body list shows only one piece; lofts merge tangent arcs
  unless told not to; a face's plane normal ignores the face's orientation. Changing a sheet-metal rule invalidates
  every edge already found (find them again after); a new edge flange that meets an earlier flange re-shapes the
  earlier flange's bend (SolidWorks leaves it whole); the lofted flange refuses open profiles; a variable fillet's
  intermediate-radius list is not in position order; `windows.h` defines `near` (a name clash in C++). A bend
  radius override given as a plain number is read in millimetres; two-offset edge flange offsets run outward from
  the edge's ends (holding a flange back needs a negative offset); a fold with two bend lines keeps the strip
  between them and folds both sides (a jog is two folds, the far line first). Sheet-metal features build their
  bodies in the active component: activate the part's occurrence while rebuilding it. A rule's thickness cannot
  appear in a feature expression and a rule refuses a user parameter, so a depth that follows the thickness
  (`linkToThickness`) is an extent to the sheet's other face.
- **Onshape** (from a trial add-in, not yet in this repository): cloud-only, so a web-API client (or an Onshape app's server), not a desktop
  add-in. Use a versioned path (`/api/v17`) and `application/json;charset=UTF-8; qs=0.09`: unversioned paths use a
  legacy JSON form and every feature post fails with a bare "Error processing json". Quotas: FeatureScript
  evaluation 100 a day (then refused for 23 h), feature-list reads 100 per window, translations and STEP exports
  10 per window; honour `Retry-After`. A feature in `WARNING` state is built: keep it.
  Default planes: Top = XY, Front = XZ, Right = YZ; a sketch on a face uses a canonical frame (origin = the world
  origin projected onto the plane), not the face's own, and imprints the face's edges into the sketch. Thin extrudes
  take the curves as `wallShape`; "through all" both ways is `symmetric`. REST mass properties need a material (zero
  centroid and inertia without one); `bodydetails` gives exact analytic parameters, areas, lengths and loops, but a
  plane's `normal` must be flipped when the face's `orientation` is false, and a cone gives a point on the axis with
  the radius there, not its apex. A STEP enters a Part Studio only through a translation (then reference its Import
  feature's `foreignId`); a raw STEP blob is refused. Assembly mates on implicit mate connectors lose their entity ids
  and fail without a message: create explicit mate connector features and mate those. A part of an active
  sheet-metal model cannot be deleted: finish the model (`sheetMetalEnd`) before a carried result replaces it.
  Plain plates are refused as sheet metal by both thicken and convert, without a message; the sheet-metal extrude
  ignores its second direction and start offset. Mate relations (gear, rack) need "mate query data" the API does not
  describe. Assemblies have no planes, sketches or cuts.
- **SolidWorks** (the comments in `sw/Ncx.Sw`): equation edits are refused unless the
  document is active; any rebuild replaces body objects; arrays of SolidWorks objects must be typed dispatch arrays
  out of process.
