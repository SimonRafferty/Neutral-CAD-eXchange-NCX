# Adding a CAD system to NCX: a plan for an AI coding agent

This file is written to be handed, as is, to an AI coding agent (it was developed with Claude Code) together with
this repository. It is the complete brief for building an NCX add-in for a CAD system that does not have one yet.
A human can follow it too.

> **To the agent:** read this whole file first, then `README.md`, then `FORMAT-SPEC.md` in full. Do not start
> writing code before you have read the spec. Everything you need is in this repository and its releases; you do not
> need, and must not assume, any other CAD system on the machine. Work as a user would who has received NCX files from
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

**What "works" means to the user.** The user opens the model and uses it: edits a sketch, changes a dimension, drags
a mechanism. A model whose every check passes but whose sketches move when edited, whose assembly comes apart when
dragged, or that lies on its side, does not work. Section 4 says how to check each of these.

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
| `fusion/` | the Autodesk Fusion add-in (C++), written by an AI agent from this repository alone: a second implementation, with its `FINDINGS.md` (what worked, what Fusion did) and `ISSUES.md` (the format questions it raised) |
| `validator/` | `ncx_validate.py`: checks any NCX file against the format without a CAD system (container, schema, consistency, geometry). Run it on every file your exporter writes. It needs the core for its levels 3-4. |
| `docs/COVERAGE.md` | what the SolidWorks add-in carries, its test corpus and its known gaps |
| sample NCX files | exported from SolidWorks, one per feature area: your test inputs. `NCX-samples-<version>.zip` on the latest SolidWorks add-in release (GitHub Releases); or export your own with the SolidWorks add-in |

The samples: `P01`-`P49` are parts, one feature area each; `L01`-`L12` are layout-driven parts and assemblies.
Some carry the cases a real assembly showed and earlier samples did not: `P47` slots, `P48` polygons, `P49`
nested and crossing contours, picked regions with their boundary and a reversed start offset, `L11` layout points
into sheet metal with a suppressed mate, `L12` a four-bar mechanism of sub-assemblies mated at the top level, one of
them flexible.

## 3. Rules

- **The format is shared.** Never change the payload format, the schema or the core's behaviour on your own.
  If you find a gap or an error in the spec or the core, **raise a GitHub Issue** on the main repository (what is
  missing, the evidence, a proposal) and work around it locally until it is resolved. Changes to the format must
  keep every existing add-in working.
- **Fork or branch, then share.** Develop the new add-in in a fork (or a branch) of the main repository, in its own
  top-level folder (for example `onshape/`, `freecad/`), so other people can find and use it. Open a pull request
  when it works; keep the format and core untouched in that pull request.
- **Functionally the same, not necessarily built the same.** Not every feature exists in every CAD system. When the
  target has no direct counterpart, build something that produces **the same geometry and behaves the same when
  its parameters change**, using the target's own tools, and report it as `approximate` with the reason. Examples:
  - *Weldments* (structural members along sketch segments): build each member as a swept or extruded body with
    the profile's cross-section, trimmed as the payload describes; keep the member names.
  - *Hole Wizard* holes: build the hole with the target's hole tool if it has one, else as revolve/extrude cuts of
    the same section.
  - *Sheet metal* without a sheet-metal environment: a solid of the same shape; say the bends are not live.
  - *Mates* in a system that positions components with joints: derive the joints from the mates (Step 4).
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
  every default plane, on a face in each axis direction and on an offset plane. In an assembly, every component's
  geometry is in its own document's frame and every mate's `frame` in the assembly's: take a face into the design
  through the component's **full path** (every nested transform), never through only its last parent.
- **Origin planes by geometry, not by name.** `originPlane` names are defined by geometry in spec §5 (`front` = XY,
  `top` = XZ, `right` = YZ). Your system may give the same words to other planes (Onshape's *Front* is XZ; Fusion
  set to Z-up calls XY its top).
- **Which way is up.** The file's `orientation.up` (spec §4, top level; absent: `y`) says which axis the source shows
  as up. If your system's convention differs, place the model so it is upright in your system's own frame (its
  Top view looks down the file's up axis), and say so in a note. Turning only the camera is not enough: the user's
  Home view, view cube and standard views still show the model lying on its side. Keep every coordinate, reference
  and check in the file's frame (one transform between the two), and export in that frame.
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
- **Leave the user's model clean.** Anything the import makes for its own use (the reference geometry, partial
  rebuilds kept for comparison, the layout's sketch copies) goes out of the user's way: hidden, in one hidden
  container, or deleted. Never leave loose copies where the user can grab them: a copy at the same origin as the real
  part is what the user will pick and drag.

## 4. Plan

Work in this order. Keep notes as you go (what you tried, what the target's API did, what failed), and write them to
`<your-folder>/FINDINGS.md`: the next agent will thank you.

### Step 1: understand
1. Read `FORMAT-SPEC.md` completely, then the schema.
2. Read the SolidWorks importer (`sw/Ncx.Sw/Replayer.cs` and the `Replay*.cs` files) to see how each part of the spec
   is used, and `sw/Ncx.Sw/Host.cs` for how geometry facts are measured for the core. `fusion/src/impl` is a second
   importer, for a system that positions components with joints.
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
   endpoint. Know what the measurements are *for*, so you can take each from its cheapest source: the matcher needs
   each candidate's type, parameters, size and distances to the samples; the mass check needs volume, area, centroid
   and principal moments. A B-rep description endpoint (Onshape's `bodydetails`) often has all of the matcher's facts.

### Step 2: read a file
0. **Make the add-in drivable without clicks** before anything else: you will run it hundreds of times. If the
   target system has an out-of-process API, drive it from a script. If it does not (Fusion), make the add-in watch a
   job folder: a watcher thread notices a JSON job file, hands it to the CAD system's main thread (Fusion:
   `Application.fireCustomEvent`), runs it there and writes a result file. Load the real implementation from a copy
   of its DLL, so a rebuild is reloaded without restarting the CAD system. Loading a new add-in the first time
   usually needs a restart of the CAD system: check first that no document has unsaved changes. Give the job route a
   developer option to rebuild only named documents of a file: iterating on one sub-assembly of a large model then
   takes seconds instead of minutes.
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
1. **Sketches**: lines, arcs, circles, splines, ellipses, points; relations; dimensions; the frame mapping; seeded
   coordinates (place every entity where the payload says, then add relations and dimensions).
   - Keep every payload point a separate entity, even where two share a position (spec §4, sketches): finding points
     again by position after drawing ties a relation to the wrong one.
   - Relations to another sketch's entities (`sketchEntity`) may point off the sketch's plane (a layout point below
     a sheet face): relate to its projection. Where you rebuild the inserted sketch yourself, find its entity by `id`.
   - Slots and polygons arrive as plain segments with the relations that hold them; make no slot or polygon objects.
   - A fully defined source sketch must arrive fully defined: compare your solver's status with the payload's
     `constrained` and report a difference. P48's second hexagon has only an inscribed circle, and the relations
     carried cannot hold it regular: it arrives under-defined in every system (SolidWorks too). Report it; do not
     invent relations to make it fully defined.
   - A sketch marked `hidden` is hidden after the import, and so is the copy of a transferred (Insert Part) sketch
     whose item is `hidden`.
2. **Extrude/cut** (end conditions, thin, start conditions, selected contours and regions), **revolve**.
   - Several `contours` combine by the area each encloses; a contour inside another is a hole; crossing ones unite
     (spec §4). A curve your own system adds to a sketch (a projected edge) must not split or close a profile.
   - A picked region: build it from its `boundary` where present, then check its point and area; a region may be
     bounded by the face's own edges, not only by sketch curves. If your profiles come only from sketch curves,
     project those face edges into the sketch, or draw them as fixed curves where projection is refused.
   - `start` offsets run along the sketch normal, against it with `reverse`, whichever way the feature runs.
3. **Reference planes, axes, points, coordinate systems.**
4. **Fillets, chamfers; patterns, mirrors; shell, draft.**
5. **Equations, parameters, configurations.**
6. **Derived parts** (`insertPart`), **multibody features.**
7. **Sweep, loft, sheet metal, hole wizard, weldments** (functionally equivalent where needed).
8. **Properties, materials, appearances, cut lists**: these are not features and are easy to forget. Report each of
   them per document (carried, approximate or dropped) even when you carry nothing, or a part with lost appearances
   reports "exact".

From step 1 on, any feature you have not implemented yet (or whose rebuild fails) takes its carried `result` when
it has one. Exporters attach results only to opaque features and to the feature types few systems share (spec §4),
so the core types above must still be rebuilt properly. A mapped feature can carry a `result` too (a sheet-metal
corner, a loft between unlike sections, a Convert to Sheet Metal): rebuild it natively first and use the result only
if your rebuild differs. Sheet metal is where most of the per-system work goes: start from `bendGeometry` and the
definitions in spec §4, not from the source's option words.

**Use a carried result to check your own rebuild.** Where the payload does not pin down one of your system's
settings (which side a flange's bend goes, which datum its length is from, which way a loft lines up), build each
candidate and keep the one whose bodies equal the feature's `result` (fingerprint compare, `exchange` tolerance).
If none matches, use the result itself and report the feature `approximate`. Check *before* the result too: its
`replaces` fingerprint is the part as it was just before the feature, so it tells whether your earlier steps were
right (a thickness side, an extrude sense) at the moment the result is used; flip the earlier undetermined senses
until it matches. Measured placement fields (`bendGeometry`, loft `connectors`) give the same check where no result
is carried.

After each part is rebuilt, **verify** it (spec §6.1): mass properties (`native` tolerances), face by face
(`ncx_face_compare` with your measured candidates), structure (every payload feature present under its name), and
the flex probes when the file carries them (spec §6.4). If your system cannot give exact mass properties without
changing the user's model (a material it would have to assign, a centroid only from a mesh), spec §6.1 says which
quantities must still be exact and how to report the rest.
A carried result is cheap on a desktop system but may not be elsewhere: on Onshape a STEP enters a Part Studio only
through a translation job, which costs several requests and leaves an extra Part Studio in the document.

### Step 4: assemblies
Components with their placements and fixed state, nested sub-assemblies, mates (or joints derived from them).
Placement is the easy half: every component arrives where its `transform` puts it. Motion is the hard half, and the
one the user will try first.

- **Multi-document features.** If your system's derived parts or external references need documents saved somewhere
  first (Fusion's Derive needs saved cloud documents), building the whole file in one document is acceptable.
  Insert Part is then rebuilt as the source's sketches in the part, with their dimensions linked to the source's
  parameters: the layout still drives the part, and the flex probes prove it. Report it `approximate`.
- **Fixed and free.** Only components with `fixed: true` are fixed in space, and only at the level they belong to.
  Check that your system did not fix anything else by itself (Fusion gave "ground to parent" to the first part
  component it created, and its copies inherited it, so seven bolts were fixed in space).
- **Sub-assemblies: rigid or flexible** (spec §5, components). A sub-assembly's own mates always hold its parts. A
  rigid one (the default) moves as one piece in its parent: also hold each of its parts to it (a rigid group, or each
  part grounded to its parent). A `flexible: true` one keeps only its mates, so they can move with the parent's
  motion. Make the sub-assembly's mates inside it, against its own parts. A system that shares one sub-assembly's
  joints and grounding across all its occurrences (Fusion) needs one component per use when the same sub-assembly
  is placed both rigid and flexible.
- **Mates between parts of different sub-assemblies** belong to the top level: make them there, against the parts as
  they sit in the design (proxies, assembly context), measured through each part's full path.
- **Frame-based mate systems** (Onshape mate connectors, Fusion joints) join two coordinate frames, while the payload's
  mates carry faces, edges and planes. Use the mate's `frame` (spec §5, "Mates"): put a connector at that frame on
  each mated component (the frame taken into the component's own frame through its `transform`); both sides then
  coincide at the solved placement, with no axis sense to guess. A concentric + coincident pair is one revolute at
  the concentric mate's axis through the coincident mate's origin. Files without `frame` (SolidWorks add-in before
  0.5.2): put the connector at the centre of the circular edge where the cylinder meets the mated plane, build,
  read the placement back and flip the axis sense if the component moved (a revolute's free turn is not a move).
- **One joint per pair of mated components, not per component.** A mechanism is often a closed loop (a four-bar:
  frame, two links, coupler): joining each component only to its "main" partner leaves the loop open, and the parts
  come apart when dragged. Close the loop; if your solver needs it, make the closing joint the less constrained kind
  (Fusion: cylindrical). A part held on two parallel pins cannot turn: join it rigidly.
- **Fasteners**: a bolt or nut mated to two parts is joined to one of them only (a part much smaller than every part
  it is mated to); joining it to both, and to the fasteners it touches, can lock the whole solver.
- **Check every joint by numbers.** Read back each joint's origin and axis in the design's frame and compare with its
  mate's `frame` and with the pin's axis: a joint at the wrong place turns the parts about a point far away. A joint
  your code had to place at a default (a component origin) because it found no face is a failure to report.
- **Suppressed mates** (`suppressed`, spec §5): make the mate and suppress it, or leave it out and say so. Either
  way every component stays at its `transform`: a suppressed mate may not hold in the placement (sample L11's
  `Distance-Lift`). Mates whose source lost their geometry are not in the file.
- **Assembly cuts** in a system whose assemblies cannot cut: cutting the part itself is right only when the cut
  changes every instance the same way. Otherwise report the cut failed and keep the instances uncut (each part still
  matches its own fingerprint).

### Step 5: the user's side
- A minimal UI: two commands (Import NCX, Export NCX) where the target system puts such commands.
- **Progress, always.** A large import takes minutes; with no sign of life the user decides the CAD system has
  crashed. Show a progress dialog for every import and export, however started (also when your test scripts start
  it), naming the document and the feature or phase. Update it and let the CAD system process its events a few
  times a second (about every 0.25 s: more often visibly slows the import, never is worse), including inside long
  phases (reading the file's geometry, verification, placement, joints, the fallback). Measure the longest gap
  between two updates on a large file; the ones left should be single calls into the CAD system.
- **Cancel** really stops, within a second, and leaves a defined state: an import closes everything it created
  (scratch documents too); an export finishes the feature it is reading, puts the model back as it was and writes
  no file.
- The report: one line per feature (rebuilt / approximate / failed / fallback, with the reason). Show it only when
  something needs attention; otherwise a one-line status.
- The model opens upright (`orientation`, §3), with the sketches hidden that the source hides, and nothing of the
  import's own scaffolding in the way.

### Step 6: export
Walk the target's model and write the payload: the reverse of every step above, with descriptors measured in the
state each feature saw, fingerprints, and flex probes where the spec says. Export the STEP geometry with the target's
own exporter (AP214 when available) and let `ncx_payload_write` embed the payload and the notice body. Write the
model in the frame it was imported in, and `orientation` with your system's up axis for models made in it.
Round-trip test: export from the target, import in the target, and compare.
A feature your importer built from a carried result has no native counterpart to export: write it `opaque` with
that same result again (you know its provenance), so the file stays rebuildable everywhere.

**Record provenance on import, and use it on export.** Most users send a model to your system and back. Your
importer builds some features as functional equivalents (Insert Part as copied sketches, a weldment member as
extrusions, a convert to sheet metal as a body plus a conversion), and your exporter cannot turn those back into the
source's features. So, as you import, tag what you build. Attributes on the CAD objects are the place, since they are
saved with the model.
- **Every feature:** the payload feature it came from, its definition, and the part's shape right after it.
- **Every sketch entity:** its payload id.
- **Every document's component:** the whole source document, and the component's state as imported (solids' volume,
  area, face and edge counts, parameter values, how many features, occurrences and joints).
- **Every placed occurrence:** its component entry's id.

On export, use those tags in this order:
- **Whole documents first.** A document whose state is unchanged goes out as the source's own document. An assembly
  goes out with its components at their current positions, which keeps the mates, limits and sub-assemblies.
- **Then single features.** In a document the user changed, write natively whatever your system has. Where your
  system built the feature as an equivalent, or as several objects, or cannot write it natively, write the source's
  definition while the part's shape right after it is unchanged.
- **Keep the source's ids** for documents, features and sketch entities, so the source's references stay valid.
- **Keep it switchable.** Offer an option to switch passthrough off, so your native export can still be tested.

Measured on the Fusion add-in: the folding step went SolidWorks → Fusion → SolidWorks with every part rebuilt and all
87 mates, against 8 parts as plain geometry and 30 mates without it.

Write the rest of what users see:
- **Materials:** under the source's name when the import mapped it (Fusion's "Stainless Steel AISI 304" goes back as
  "AISI 304"); none when the source had none and your default stands in for it.
- **Appearances**, with the source's exact values when you recorded them.
- **Custom properties.**
- **Equations and global variables** in the neutral grammar, under the source's names (Fusion renames `W` to `W_`;
  write `W`). An importer uses `native` text only from its own system's files.

A few writing rules:
- **Profiles:** whenever a sketch has more than one profile, write the selection explicitly as `contours` or
  `regions`. Your system's "every profile" is not the odd-even whole sketch: adjacent regions, or a revolve axis on
  the boundary, make SolidWorks refuse it.
- **Cuts:** never write `merge` on a cut.
- **Document order:** dependencies first (a layout before the parts inserting it). An importer should not rely on
  that either; order the documents itself.
**Run `validator/ncx_validate.py` on every file you export** (it needs the core: build it with `core/build.ps1`, or
point `NCX_CORE_DLL` at your add-in's copy; a verdict of INCOMPLETE is not a pass) and fix every error before
anything else: it checks what other add-ins rely on (units, frames, seeds against dimensions, references,
fingerprints against the geometry), which your own importer may not notice because it shares your exporter's
assumptions.

### Step 7: package
Make the add-in installable by someone without build tools: the version set in one place; a build from a clean
tree (delete the build output first) that works exactly as your README says; the built files checked for runtime
dependencies a user would not have (link the C runtime statically, or ship it); an installer (or, at least, a script
that writes a zip of the ready-to-install add-in folder) with install instructions in your README, tested on a
machine by installing it and importing a sample. Users of these add-ins are often not computer literate: one
download and one click to install. The installer is published as a release asset, not committed. For a web-API
client the package is its scripts plus the core library; describe in the README what a real distribution (the
system's own app store, sign-in, a server) would need, and where progress and cancel would live in its UI.

## 5. Testing

- Every sample must import with every check matching, or with an honest `approximate`/`fallback` report saying exactly
  what and why. Record the results per sample in `<your-folder>/FINDINGS.md`.
- **The samples are not enough.** In the Fusion add-in's rounds, all the samples passed while a real 19-part
  assembly fell back in 11 of its 13 parts, came apart when dragged and opened lying down. Before calling the import
  done, import the largest real model you have been given (or ask for one), time it, and look at it as a user would.
- **A person tests the motion.** No CAD API we know reproduces an interactive drag: setting an occurrence's position
  skips the joint solver, driving one joint's value does not solve a closed loop. So check what the API can show
  (every joint healthy, at the right place by numbers, the right components fixed), then ask the person you work
  for to drag the mechanism, and tell them exactly what to drag and what should happen. Use the sample L12
  (a four-bar) to explain it.
- Every file your exporter writes passes `validator/ncx_validate.py` (verdict PASS).
- Re-export what you imported and compare the payloads (feature names, types, sketch contents, parameters). A
  re-export keeps every feature under its name, with its type where your system has it; a feature it cannot map
  goes out `opaque` with its `result` carried (spec §4), never dropped. The re-import must verify.
- When the target system and SolidWorks disagree on a construction, the geometry decides.
- Iterate on something small: reproduce a problem of a large model on a sample or on one sub-assembly of it
  (the developer option of Step 2), not by importing the whole model again and again; a person watching sees only
  the same long import repeating.

## 6. Done means

- Import works on all samples (matching, or reported and justified), and on a real model: it opens upright, its
  sketches behave when edited, its mechanism moves when a person drags it.
- Export works and round-trips in the target system.
- The add-in installs from its installer (Step 7).
- `<your-folder>/README.md` says how to build and install the add-in; `<your-folder>/FINDINGS.md` records what was
  learned, including every feature translated by functional equivalence and why.
- Issues are raised for anything in the spec or core that had to be worked around.
- A pull request from your fork/branch adds the add-in folder only.

## 7. Traps by CAD system

Each add-in's `FINDINGS.md` records the traps its CAD system set. Read the one for your system if there is one, and
add yours. Ones that cost a debugging round each:

- **Fusion** (`fusion/FINDINGS.md`): no out-of-process API (job folder, above); STEP import drops body names (find
  the notice by `noticebox`); bodies without a material have zero mass properties; sketches on faces project the
  face's edges by themselves, and a projected curve can split or close a profile; profile-loop curves do not come
  in loop order; the profile geometry of a sketch on an XZ-parallel plane comes back in sketch space; a
  one-direction pattern defaults to 3 instances in its second direction; pattern instances cannot be suppressed
  through the API; moving an occurrence in a parametric design is only a pending position until captured, and
  `Design.computeAll()` throws away every uncaptured position; MeasureManager measures only entities in the root's
  context; parameter names that are unit symbols (`W`, `H`) are refused; centimetres and radians internally; a
  sketch on the XZ plane has its y axis along -Z (map everything through the payload's `frame`).
  - Sheet metal: Fusion has a sheet-metal API (flanges, hems, unfold); a one-sided through-all cut on a sheet-metal
    face fails (a two-sided one works), and some through-all cuts are refused outright (a blind cut past every body
    does the same); its sheet-metal chamfer takes plain millimetre numbers, not centimetres; changing a sheet-metal
    rule invalidates every edge already found (find them again after); a new edge flange that meets an earlier
    flange re-shapes the earlier flange's bend (SolidWorks leaves it whole); the lofted flange refuses open profiles;
    a bend radius override given as a plain number is read in millimetres; two-offset edge flange offsets run
    outward from the edge's ends (holding a flange back needs a negative offset); a fold with two bend lines keeps
    the strip between them and folds both sides (a jog is two folds, the far line first); sheet-metal features build
    their bodies in the active component (activate the part's occurrence while rebuilding it); a rule's thickness
    cannot appear in a feature expression and a rule refuses a user parameter, so a depth that follows the thickness
    (`linkToThickness`) is an extent to the sheet's other face; convert-to-sheet-metal only converts a body that is
    already a sheet (make it from a Convert to Sheet Metal's carried result).
  - Other: a split's body list shows only one piece; lofts merge tangent arcs unless told not to; a face's plane
    normal ignores the face's orientation; a variable fillet's intermediate-radius list is not in position order;
    `windows.h` defines `near` (a name clash in C++); a single sketch-constraint solve can take 4-9 s.
  - Assemblies: the old Ground is now **Pin** (`Occurrence.isGrounded`, fixes in the design's space); **Ground to
    Parent** (`isGroundToParent`, anchor icon) ties a component to its parent component; parts of a sub-assembly are
    not rigid in the parent unless ground to parent, a rigid group or rigid joints hold them; pinning a nested
    occurrence does not tie it to its parent. A joint reports its axis in the coordinates of the occurrence it was
    built on and its origin in the design's; a sub-assembly's own joints cannot be read back afterwards (no
    occurrences, no geometry): record each joint's frame as you build it. Fusion's model orientation
    (`GeneralPreferences.defaultModelingOrientation`) is often Z-up. A cut made without naming its participant
    bodies also cuts other parts' bodies built at the same origin (and `participantBodies()` then reads empty).
  - Geometry: `measureMinimumDistance` measures a planar face as its whole infinite plane (measure to its edges when
    the nearest point is off the face); projecting an edge that a later feature of the same sketch made is refused as
    circular.
  - Responsiveness: Fusion pumps no events during an add-in's command unless the add-in calls `adsk::doEvents()`;
    a progress dialog that is never updated looks exactly like a hang.
  - Upright wrapper (the model inside one turned component): a part is built with its occurrence active, and Fusion
    then makes no `Path` of the native sketch curves (sweeps, thin extrudes, surface revolves): use the curves of the
    sketch's proxy in that occurrence. On export read native objects: a proxy body or face is measured in the root's
    turned frame (deleted bodies went out turned by 90° and were not found). `Feature` has no `nativeObject`: find
    the native feature by name in its parent component.
  - Export: a vertex descriptor needs its point as its one sample; Fusion's "All" extent (`AllExtentDefinition`) is
    through all; a fixed spline has only its two ends as points, so write its exact `nurbs` (importers build another
    system's spline from `nurbs`).
  - Sheet-metal export: write positions by the spec's geometric meaning, not by the word.
    - Fusion's fold line marks the start, centre or end of the flat bend zone: write the line at the outer virtual
      sharp (`materialInside`), in a sketch of its own on the sheet's face.
    - Fusion's "inside" flange at an angle is `bendSharp`.
    - Measure a flange's web only on faces that rise clear of the sheet.
- **Onshape** (from a trial add-in, not yet in this repository): cloud-only, so a web-API client (or an Onshape app's
  server), not a desktop add-in. Use a versioned path (`/api/v17`) and `application/json;charset=UTF-8; qs=0.09`:
  unversioned paths use a legacy JSON form and every feature post fails with a bare "Error processing json". Quotas:
  FeatureScript evaluation 100 a day (then refused for 23 h), feature-list reads 100 per window, translations and STEP
  exports 10 per window; honour `Retry-After`. A feature in `WARNING` state is built: keep it.
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
- **SolidWorks** (the comments in `sw/Ncx.Sw`): equation edits are refused unless the document is active; any rebuild
  replaces body objects; arrays of SolidWorks objects must be typed dispatch arrays out of process; a sub-assembly's
  "solve as flexible" is set through `CompConfigProperties5` on the selected component (`Solving` is read-only); a
  file opened read-only stays locked after it is closed, until SolidWorks exits; Through All - Both cannot be made
  through `FeatureCut4` (build through all in both directions).
  - Sheet metal:
    - Give the K-factor and relief to the base flange that makes the Sheet-Metal feature (a CustomBendAllowance).
      Changed later, they only take with SetOverrideDefaultParameter2 and a rebuild.
    - Never change an edge flange's length datum after it is made: SolidWorks drops the flange.
    - A sketched bend needs its sketch on a face of the sheet.
    - SolidWorks refuses a hem whose edge meets another hem at a corner.
    - Once a sheet-metal feature is carried as fixed geometry, later sheet-metal features cannot be built.
