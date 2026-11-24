# SolidCraft translator: findings

What was learned building the NCX translator for SolidCraft (0.47.0, design format solidcraft/65; the nightly of
2026-11-24 writes solidcraft/66). Research before the build: `results/solidcraft/FEASIBILITY.md` (plugins) and
`results/solidcraft/JSON-ROUTE.md` (files, CLI, live link, weldments).

## Route

- SolidCraft's plugins (WebAssembly) are limited to 10 minutes and get no file access, and an exporter plugin
  cannot get STEP. So the translator is a separate desktop program that drives SolidCraft's own command-line
  program in a private headless session: `solidcraft-cli mcp --headless --compact --allow-path <dirs>`, MCP
  (JSON-RPC 2.0) over stdin/stdout, tool `execute {command, params}` for every command. No time limit, full file
  access, SolidCraft's window untouched. Each command answers on its own, with the ids it made, so the importer
  reacts per feature. A failed command answers `isError: true` with SolidCraft's own reason (used in the report).
- Not a hand-written design file: a face-attached feature needs a persistent face anchor only SolidCraft makes
  (`a mounting plate requires a direct persistent planar face`), and the format number changes between releases.
- SolidCraft's look (theme, colours, icons, UI kit, name, logo) is under its Brand and Interface Licence: another
  program may not use it or look like SolidCraft without the maintainers' written permission. The app uses egui
  and IBM Plex (both open) with NCX's own colours, and its own icon (a stepped block between two chevrons, drawn by the
  user, assets/icon-source.jpg; not SolidCraft's logo). It is called "NCX
  SolidCraft" (user's choice, 2026-11-24; the licence asks for the maintainers' permission to use the name).

## SolidCraft behaviour that mattered

- **Sketches.** `sketch.create` takes an explicit plane `{origin, x_dir, y_dir}`: the payload's frame is used as
  is, so sketch coordinates and horizontal/vertical are the file's. A line's point argument can be another
  entity's point reference (`"l1.end"`): shared payload points are passed on that way, with `connect: false`, so
  points are never merged by position. Created ids: lines `lN` (`lN.start`, `lN.end`), circles `cN`
  (`cN.center`), arcs `aN`, points `pN`, constraints `kN`. A dimension makes a parameter `dN`.
- **Dimensions.** SolidCraft measures a distance to a circle to its edge; NCX means its centre (except between two
  concentric ones): the importer dimensions the centre point. A dimension the sketch already fixes is refused;
  `if_redundant: "driven"` keeps it driven. `scale_sketch: false`, or the first dimension scales the whole sketch.
- **Picks** are world points snapped to the nearest edge or face (`no edge at [..]: the nearest edge is 69 mm
  away`). A descriptor's middle sample is the pick.
- **Components.** The design's first part component is grounded when made and a grounded placement cannot move:
  make it `grounded: false`, build, turn, then ground it. `component.move_features` refuses a recorded body
  transfer (a carried result moved into the part), so each part is built inside its own component from the start,
  at the origin (file coordinates = world coordinates for every pick), and turned upright only at the end.
- **Upright.** SolidCraft is Z-up; a Y-up file's part component is turned +90° about X.
- **Results** (carried STEP): `file.insert_step` always makes a new component; `component.move_bodies` moves the
  bodies into the part (a BodyTransfer feature). The import's component stays (deleting it deletes the import the
  bodies came from), hidden.
- **Measuring.** `measure` gives per body volume, area, centre of mass, box, faces, edges; inertia only per
  placement (`assembly.mass_properties`, kg·mm² at the material's density). Per-body principal moments: between
  `edit.checkpoint` and `edit.restore_checkpoint`, remove the other bodies and measure; divide by density.
- **STEP export** is labelled AP242 (`AP242_MANAGED_MODEL_BASED_3D_ENGINEERING_MIM_LF`) with no option. Its entities
  are plain B-rep and product structure, AP214's too, so the exporter relabels the schema, application context and
  protocol as AP214 (SolidWorks 2018 hangs on AP242 assemblies; the validator's C-AP check).
- **Cuts.** A cut whose volume change is zero went the wrong way: undo and cut the other way.

## Packaging

- egui 0.37: `App::ui(&mut Ui)` (no `update`), panels take a `Ui` (`egui::Panel::bottom`), `set_style_of` per
  theme; set the dark theme explicitly or Windows' light setting gives light widgets.
- The C runtime is linked statically (`+crt-static`): the program needs only Windows' own DLLs.
- WiX 3: WixUI_Minimal sets ARPNOMODIFY itself; shortcut components need a per-user (HKCU) key path even in a
  per-machine MSI (ICE38/43/57); two unversioned files need two components for automatic GUIDs. light.exe leaves
  an MSI behind even when validation fails: check its exit code.

## Assemblies, mates, materials (0.1.2-0.1.3)

- Every placed part document is its own component, built at the origin and checked alone (bodies already in the
  design are left out of its checks: `measure` has no component filter). Parts that cannot be rebuilt are copied
  out of a one-time import of the file (`component.paste_new` makes them independent; a part placed twice needs
  `occurrence` + `path`), then the import is deleted. Placements: `occurrence.reparent` (first use) /
  `occurrence.copy`, then `occurrence.move` with the payload's row-major 3x4; `occurrence.ground` for `fixed`.
- SolidCraft feature names are unique across the whole design (a second part's "Sketch1" is refused): a clash
  becomes "Sketch1 (Part)". Global parameters too: a clash becomes "W_Part".
- A carried result's import makes a component; move exactly its `created_bodies` into the part (taking "every
  new body" moved another part's body away once).
- Mates: `assembly.constraint.create` (mate/flush/concentric/distance/angle/parallel/perpendicular/tangent) and
  `joint.create` relations (point_on_plane, line_on_plane, ball) and rigid; references `{occurrence, path, face|
  circle|edge|point: world point}` or `{occurrence, path, plane: "XY"}` (the occurrence's own origin plane) or a
  construction plane's name. SolidCraft's facing conventions differ from SolidWorks' alignment words, so each mate
  is made, checked (no occurrence moved, no conflict reported for it), else undone and the next variant tried.
  Between two fixed parts only the conflict shows a wrong facing. A hinge is concentric + coincident.
- Materials: `material.create` under the source's name (density g/cm3, colour) when SolidCraft has none of that
  name; `material.assign` by body. Colours: `appearance.assign` (bodies, faces).
- Inertia comes from the tessellation (curved bodies' moments ~1e-3 off): moments compared at 5e-3.

## Features added in 0.1.3

Shell, draft, linear/circular patterns (body patterns with skipped instances: the copies removed after), mirror,
holes (one `solid.hole` per centre), construction axes, sweep and loft (with rails), combine, delete, move bodies,
split, rib, sheet metal (rule from `sheetMetal`, base and contour flanges, edge flanges, convert, unfold/fold of
every bend), weldment members (`frame.member`, SolidWorks profiles mapped to SolidCraft's kinds), extruded,
revolved and planar surfaces, knit; extrudes starting at a face (`from`).
- Choices the file does not state (a base flange's thickness side, a draft's pull, a partial revolve's sense, an
  extrude from a face, a rotating move) are guessed and checked: against the next carried result's `replaces`
  (the part just before it) when one follows, else by the part's final check, which rebuilds the part with each
  guess (then pairs, then all) turned round. A feature with a carried result is checked against it; an edge flange
  that differs is tried the other way, else the result is used.
- Known gaps: face and full-round fillets, thin revolves, flange offsets along the face, 3D sketches, weldment
  corner overrides beyond SolidCraft's corner kinds, crossing contours (P49), skipped instances of feature
  patterns. SolidCraft joins disjoint pieces into one body where SolidWorks keeps two (P04, P31). SolidCraft's
  kernel refuses some shells (the Rear AC's Fan Cowl: "the offset is refused").

## Export with feature history (0.1.4)

- **What SolidCraft says about a design.** `timeline.inspect` gives each feature's definition (the fields
  `timeline.edit` takes: expressions, picks as points, the sketch, the operation) and its inputs, each a parameter
  (`d17`) whose value `parameters.list` has (the design's own parameter list leaves feature inputs out).
  `sketch.inspect` lists a sketch's points and curves; relations and dimensions name them by index into those two
  arrays (`p`, `q` points; `l`, `c`, `a`, `b` curves). A sketch is described only where the timeline has evaluated
  it (roll to the end first). `joint.inspect` gives each mate's rule and both ends (placement path, faces by
  SolidCraft's persistent names).
- **Frames.** Definitions are in their component's frame, but `body_topology`, `inspect.classify_points` and
  `measure` report world coordinates, and `inspect.curvature_comb` mixes the two (it converts the pick into one
  component's frame, then searches every body in its own). The exporter therefore moves each part's placement to
  identity in its private session (never saved): world is then the part's frame and every tool agrees. Mates are
  read and removed first (a mated part resists the move), and an imported STEP assembly's rigid group is unlocked
  (`component.unlock`). A design an NCX import turned upright (+90 degrees about X) has that turn undone, so the
  file goes out in its own frame again.
- **References** are built from `body_topology`: an edge's ends, middle and length, the faces it joins (in its
  name, `A|B`, where a name may hold `|` inside parentheses), and a face's area, a point on it, a plane's normal or
  a cylinder's axis. A circle is the one through an edge's ends and middle, accepted when a cylinder it bounds has
  that radius, or its arc is within 5 % of the reported length: SolidCraft measures curved edges on their
  tessellation (a 6 mm hole in a face that came from STEP came out 2.5 % short). Face samples: a grid over the
  plane or cylinder, kept where `inspect.classify_points` puts it on that face, the most interior first.
- **Moments.** SolidCraft's inertia is from a coarse tessellation: up to 4.8e-4 of the largest moment off (the
  exchange tolerance is 5e-4), which would fail SolidWorks' check on ordinary parts. The exporter integrates the
  body's "fine" STL mesh and scales by the exact volume over the mesh's (to the power 5/3): at most 8.4e-5 on the
  Rear AC bodies ("finest": 3.4e-5 at five times the time). Only where an importer compares moments (the parts'
  bodies, carried results' bodies).
- **STEP.** SolidCraft writes a part with several bodies as one representation per body; the exporter merges them
  into one (as SolidWorks writes it), where the core puts the notice body. The STEP is exported under the design's
  name (SolidCraft names the product after the file).
- **Written natively:** sketches, extrudes/cuts (contours from picked curve loops), revolves (origin axes or a
  sketch line), fillets, chamfers, boxes and cylinders (a frame-placed sketch and an extrude), holes, rectangular
  and circular patterns along origin axes, mirrors, shells, drafts, sweeps, ribs, splits, offset planes, combine,
  delete, move/copy. Features whose sense the file cannot pin down the same way everywhere carry their result too.
  Features SolidCraft warns about that change no body are written `noGeometry`. Not yet: sheet metal, weldment
  members, lofts, surfaces, regions bounded by parts of curves (the Rear AC lid's first extrude), linear-diameter
  dimensions, mates on edges or points and joints other than constraints.
- **Core fixes found on the way:** removing an earlier notice component never finished when a record it reached
  was shared (an NCX file opened as plain geometry and written again), and, once it did, took the whole
  sub-assembly holding the notice with it. It now stops at the definitions and shapes that are not the notice's
  own, and keeps whatever a record outside it still uses. The validator measured a 270-degree revolved ring 0.28 %
  high with OpenCASCADE's Gauss rule; where the Gauss and adaptive rules disagree it now takes the one nearer a
  fine triangulation.
- **Corpus round trip** (`results/solidcraft/run_export.py`): each sample imported into SolidCraft, exported,
  validated and imported back. Run export_10: 63 of 63 pass the validator, none with a part as plain geometry.
- **SolidWorks 2018** (add-in 0.6.8) importing the exports: P02, P05, the L13 assembly (with its mates) and a native
  SolidCraft test part (primitives, holes, patterns, mirror, shell, revolve, planes) rebuild with their history
  and match. Found on the way: SolidWorks' importer made an `equalDistance` chamfer of zero size (no second
  distance; the exporter writes equal chamfers as `angleDistance` at 45 degrees, as SolidWorks does, and the
  importer is fixed for the next add-in release); a picked loop's holes must be listed as contours; sketches on
  SolidCraft's custom planes are written on the face or origin plane they lie in. The user's Rear AC: 66 features
  rebuilt, the sheet-metal and carried features as their shapes; the fan falls back (a carried shape changes its
  body count), a few Half Shell mates do not resolve (its faces split differently in SolidWorks).

## Corpus

`results/solidcraft/run_corpus.py`, runs in `results/solidcraft/runs/`. Run 3 (2026-11-24): all 62
samples import and their designs open; 14 parts rebuild natively and match (P01, P02, P08, P10, P14, P17, P18,
P19, P25, P30, P37, P40, P47, P48: the loft, variable fillet, dome, wrap and lofted bend through their carried
results). The rest come in as plain geometry, each with its reason: unmapped feature types (patterns, mirror,
shell, draft, holes, sheet metal, weldments, sweeps, surfaces, splits, moves), and all assemblies and
layout-driven parts (not yet rebuilt).
