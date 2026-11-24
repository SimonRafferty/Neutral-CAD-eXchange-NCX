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
  and IBM Plex (both open) with NCX's own colours, and is called "NCX Translator".

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

## Corpus

`results/solidcraft/run_corpus.py`, runs in `results/solidcraft/runs/`. Run 3 (2026-11-24): all 62
samples import and their designs open; 14 parts rebuild natively and match (P01, P02, P08, P10, P14, P17, P18,
P19, P25, P30, P37, P40, P47, P48: the loft, variable fillet, dome, wrap and lofted bend through their carried
results). The rest come in as plain geometry, each with its reason: unmapped feature types (patterns, mirror,
shell, draft, holes, sheet metal, weldments, sweeps, surfaces, splits, moves), and all assemblies and
layout-driven parts (not yet rebuilt).
