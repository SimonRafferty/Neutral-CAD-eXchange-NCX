# NCX add-in for Autodesk Fusion: findings

Working notes of the agents that built this add-in from `docs/ADDING-A-CAD-SYSTEM.md`, with only Fusion (2706.1.14,
Windows 10 x64) and Visual Studio 2019 Build Tools on the machine, and sample NCX files exported beforehand from
SolidWorks 2026. No SolidWorks was used.

- Round 1: the first agent, 55 samples (core 0.1.0 files).
- Round 2 (2026-10-23): a second agent, continuing from round 1's state, against the 56 samples re-exported with
  format round 5 (core 0.2.0: enumerated words, spline NURBS, hole type and end, vertex chamfer legs, equation
  `targetRef`, header `noticebox`, freeform fingerprints) plus P46 (weldment corners). Section 9 says what changed.
- Round 3 (2026-10-23): a third agent, against the 56 samples re-exported with the owner's answers to round 2's
  issues 11-20 (`bendGeometry`, edge flange `length` datum, sketched bend position, unfold/fold bend faces, loft
  `connectors` and results, variable fillet `transition`, hole `measuredDiameters`). Section 10 says what changed.
- Round 4 (2026-10-23): a fourth agent, against the same 56 samples, with the owner's answers to issues 21-26 (spec
  only: miter flange and closed corner gaps, jog offset and `fixProjectedLength`, edge flange `positionOffset`, the
  variable fillet's smooth law), and the add-in made ready to publish (version 0.1.0, clean build, release zip).
  Section 11 says what changed.
- Round 5 (2026-10-25): a fifth agent, from a fresh clone (NCX 0.5.3), against the 59 samples of
  `NCX-samples-0.5.3.zip` (the 56 re-exported with SolidWorks add-in 0.5.3, plus P47 slots, P48 polygons and the L11
  assembly). Section 12 says what changed; brief and spec feedback folded into `FORMAT-SPEC.md` and `docs/ADDING-A-CAD-SYSTEM.md` in NCX 0.5.4; issues 27-29.
- Round 6 (2026-10-25): a sixth agent, from a fresh clone (NCX 0.5.4), on progress and responsiveness, and on a real
  assembly (a folding step, 19 documents, exported with SolidWorks add-in 0.5.4; not published). Section 13; issues
  30-36; its brief and spec feedback folded into NCX 0.6.0.

Tested with Autodesk Fusion 2706.1.14 (Windows 10 x64); built with Visual Studio 2019 Build Tools (v142).

The per-sample results table is in section 8; it is kept current.

## 1. Building

- **Toolset:** the Fusion SDK samples name the v143 toolset (VS 2022); the default add-in template even says v110.
  **v142 (VS 2019) builds and runs fine**: the SDK is header-only wrappers plus `core.lib`/`fusion.lib` import
  libraries, C++17, no v143-only feature. Built with CMake ("Visual Studio 16 2019", x64, static CRT), the same
  route as `core/build.ps1`.
- **SDK location:** `%APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP\{include,lib}` is a stable copy of the SDK
  that Fusion keeps current; the webdeploy folder (`%LOCALAPPDATA%\Autodesk\webdeploy\production\<hash>\CPP`)
  changes hash with every update. `fusion/CMakeLists.txt` uses the stable one.
- **The core** is built unchanged from `../core` (CMake `add_subdirectory`) into `ncx_core.dll`, which sits beside
  the add-in's DLLs. Conformance: `fusion/tools/read_all.py` runs `ncx_payload_read` on every conformance vector
  (24/24 pass) and every sample (55/55 `OK`).

## 2. Running and testing inside Fusion without anyone clicking

Fusion has no out-of-process API (no COM, no command line). What worked:

1. **Add-in layout.** `NCX.dll` (what Fusion loads: `run`/`stop`) is a thin stub. All the work is in
   `NcxFusionImpl.dll`, which the stub loads with `LoadLibraryEx` **from a private copy**
   (`%LOCALAPPDATA%\NcxFusion\impl\<pid>_<n>\`). The files in the add-in folder are therefore never locked, a rebuild
   can overwrite them, and a `reload` job swaps the running implementation **without restarting Fusion**. Fusion only
   has to be restarted when the stub itself changes (rare).
2. **Developer job folder.** When `%LOCALAPPDATA%\NcxFusion\jobs` exists, a background thread in the stub watches it.
   A file `<name>.job.json` (`{"cmd": "import", "file": ...}`) is renamed `.running`, handed to Fusion's main thread
   with `Application.fireCustomEvent`, run there, and answered with `<name>.result.json`. `fusion/tools/fjob.py`
   writes a job and waits for the answer; `fusion/tools/run_samples.py` imports every sample and tabulates the
   results; `fusion/tools/rebuild.sh` builds, deploys and reloads. The folder does not exist on a user's machine, so
   the watcher does nothing there.
3. **First load.** The manifest has `"runOnStartup": true`; a new add-in folder is picked up the next time Fusion
   starts. Fusion was closed gracefully (`CloseMainWindow`, never killed) after checking that no unsaved document
   was open (no crash-recovery files under `%LOCALAPPDATA%\Autodesk\Autodesk Fusion 360\<user>\CrashRecovery`, and
   the session log showed no document opened besides the start-up one), then restarted through
   `FusionLauncher.exe`. From then on the add-in starts with Fusion and the job folder answers within seconds.
4. **Work never runs inside a command event.** `ImportManager.importToTarget` refuses to run inside Fusion command
   events (documented in the header). The menu commands therefore only fire a custom event; the work runs in its
   handler, exactly as jobs do.
5. **Documents.** Every document the implementation creates (scratch imports, rebuilt designs) is registered; the
   `closeDocs` job closes only those, unsaved. Tests run with `"close": true` so nothing is left open. Nothing is
   ever saved.
6. **Seeing the result.** A `screenshot` job saves the active viewport as a PNG (`Viewport.saveAsImageFile`). A
   `trace` option records every body's volume and centroid after each feature, which pinpoints the feature where a
   rebuild departs from the source (that is how the thin-wall side and the face-sketch problem below were found).
7. Fusion's own MCP server (its log shows `MCP:` handlers, including an `Execute` handler for `script`) might be an
   alternative route, but it was not enabled here and enabling it means changing the user's preferences.

## 3. Fusion facts that mattered (measured)

- **STEP import drops solid names.** Every body arrives as `Body1`, `Body2`...: `MANIFOLD_SOLID_BREP` names are
  ignored. Component (product) names are kept. So the notice body of a part file cannot be found by name (spec 2.6).
  Files from core 0.2.0 carry the notice's box in the header (`noticebox`): the add-in takes the solid whose box
  matches within 0.01 mm (Fusion's `boundingBox` of the notice block is exact). Older files fall back to round 1's
  heuristic: the one solid that matches no payload body, has only planar faces and lies wholly below the lowest y of
  every other body. In assembly files the notice is a component and its name works.
- **Bodies without a material have no mass properties.** `getPhysicalProperties` returns centre of mass (0,0,0) and
  zero moments for a body with no material, which is every body Fusion's STEP reader makes. The add-in assigns a
  library material before measuring; moments are divided by the density to get the spec's density-1 values (mm^5).
- **Fingerprints cover solid bodies only.** The payload's `fingerprint` and `bodies[].fingerprint` list solids only
  (P26, a surface-only part, has `bodies: []`), so the stale check and mass check use Fusion's solid bodies only.
- **Sketches on faces auto-project the face's edges** (Fusion's "auto project" preference). Those reference curves
  make extra profiles (a hole sketch on a face then cut the whole face away). They are deleted right after the
  sketch is created.
- **Profiles.** A source extrude of a whole sketch uses its closed contours by the odd-even rule (nested contours
  make holes). Fusion offers every region separately; the add-in selects the regions inside an even number of other
  regions' outer loops.
- **Cut direction.** The payload's `reverse` is SolidWorks' `ReverseDirection`; for a **cut** SolidWorks' default
  runs *against* the sketch normal (into the material), for a boss *along* it. Not stated in the spec.
- **Thin features on open profiles** need a `Path` of the sketch curves (`Path.create(curves,
  connectedChainedCurves)`); SolidWorks' one-direction wall is on Fusion's `Side2` for the same path and normal
  (P19).
- **Redundant relations.** Fusion refuses a relation the others already imply (`VCS_SKETCH_OVER_CONSTRAINTS`);
  SolidWorks keeps it (P03: a horizontal line between two points already on a horizontal centreline). Such a refusal
  loses nothing and is reported as information, not as an approximation.
- **Parameter names.** Fusion parameter names are identifiers, so `D1@SK_Base` becomes `D1_SK_Base`; the native name
  is kept in the parameter's comment.
- **Splines.** Fusion's fitted spline through a spline's fit points is not SolidWorks' curve: on P23 the region
  area differs by 3.7 mm^2 in 1310 (0.3 %), more than the exchange tolerance; natural cubics with uniform, centripetal
  or chord-length parameters do not reproduce it either. Files from core 0.2.0 carry the exact B-spline (`nurbs`):
  `NurbsCurve3D::createNonRational` (control points mapped through the sketch frame into Fusion's sketch space,
  knots as given) and `SketchFixedSplines::addByNurbsCurve` reproduce it exactly (P23 exact). A fixed spline cannot
  be reshaped: its fit points are kept as points on it.

- **Patterns: the second direction defaults to 3 instances.** A `RectangularPatternFeatureInput` made with one
  direction keeps the UI default of 3 instances in direction two at distance 0: every instance is made three times
  over (12 bodies from a 4-instance body pattern, measured). `quantityTwo(1)` fixes it.
- **Pattern instances cannot be suppressed through the API.** `PatternElement.isSuppressed(true)` fails with "Didn't
  roll editing feature back", or, with the timeline rolled to the pattern, returns true with "API Function not yet
  implemented" and changes nothing. Skipped instances are therefore made by removing their bodies (body patterns)
  or by re-spacing the pattern when the kept instances are still evenly spaced (feature patterns); both reported.
- **Failed attempts can leave features.** `add()` can return null and still leave a feature (in an error state) in
  the timeline: every retry deletes what the failed attempt left.
- **A feature can be created with a failed compute.** An extrude can come back non-null with health "Compute
  Failed" ("No target body"): such a feature is treated as failed and deleted.
- **Moving an occurrence in a parametric design is only a pending position.** `transform2` on an existing occurrence
  is reverted by the next occurrence added, a snapshot capture put it back to the origin, and while pending it
  offsets new occurrences of the same component (measured: 9 cm asked, 3 cm got). Every instance is therefore a new
  occurrence made at its place with `addExistingComponent(component, transform)`; the occurrence a part was built
  under is deleted afterwards (or hidden, for a layout no assembly uses).
- **MeasureManager only measures entities in the root's context.** For a native face of a component under an
  occurrence `measureMinimumDistance` returns nothing; measurements go through `createForAssemblyContext` proxies and
  points are moved through the occurrence's transform (`host.cpp`, `setMeasureContext`).
- **Parameter names that are unit symbols are refused** (`W`, `H`: "param name is not valid"); such a global variable
  becomes `W_`.
- **Sheet metal.** Fusion's base flange (closed profile) and contour flange (`createOpenProfile`) reproduce
  SolidWorks' base flanges exactly with a design rule of the payload's thickness, bend radius and K-factor (P28, P29,
  P34, P35, P36, P43). A cut in a sheet-metal component after a body brought by a base feature (a carried edge-flange
  result) finds no body in either direction ("No target body", "body not found to extrude through"), even after
  `convertToSheetMetal`: P12 and P41 fall back to plain geometry for that reason.
- **Fusion's sketch on a face may report no reference plane** (`Sketch.referencePlane` null) at export: the exporter
  finds the face lying in the sketch plane instead.
- **Extrude direction at export.** `ThroughAllExtentDefinition.isPositiveDirection` reads true for a cut made in the
  negative direction; the exporter decides the side from where the feature's side faces lie (with the timeline
  rolled to just after it).
- **STEP export of a component** (`ExportManager.createSTEPExportOptions(file, component)`) works with the timeline
  rolled back, which is how the exporter writes a carried result for every feature it cannot map.

Round 2 (measured):

- **Fusion 2706 has a sheet-metal API** (marked "preview"): `flangeFeatures().createEdgeFlangeInput`,
  `hemFeatures()`, `foldFeatures()`, `unfoldFeatures()` / `refoldFeatures()`, `sheetMetalChamferFeatures()` /
  `sheetMetalFilletFeatures()`, `cornerClosureFeatures()`, `loftedFlangeFeatures()`. Round 1 carried all of these as
  results; edge flanges, hems, break corners, unfold (and refold, when it works) are now native.
- **A cut on a sheet-metal body.** A one-sided through-all cut from a sketch on the sheet's face finds no body in
  either direction ("No target body" / "body not found to extrude through"), and naming the sheet-metal body as a
  participant makes it worse; the same cut two-sided through all works (P12; reported approximate). Round 1 put the
  failure down to the carried flange; it is the same with a native flange.
- **`SheetMetalChamferEdgeSetInputs.add(edges, distance)`** reads a real `ValueInput` in the design's units (mm), not
  centimetres like every other API value: 0.5 gave a 0.5 mm chamfer. Pass a string with its unit.
- **Edge flange variants.** The payload's profile and `position` do not say which of Fusion's height datums and bend
  positions match (ISSUES 17). The add-in builds each combination (inside / outside / start edge / tangent, inner /
  outer faces / tangent to bend, flipped, mitered corners) and keeps the one whose body is the carried result;
  `materialInside` came out as bend position *inside* with height from the *inner faces*. Separate flanges must not
  be mitered (`isMiteredCorners(false)`), as SolidWorks does not miter flanges made one after another.
- **Edge flange profile sketches** are replayed *before* the flange (their vertex references exist only then) on the
  plane their frame gives, and the flange's height parameter is set to the sketch dimension of the same value, so
  editing the dimension changes the flange as in the source.
- **Hems.** Not every bend position is accepted for every hem type ("Invalid bendPositionType argument"): skipped.
  A SolidWorks closed hem is Fusion's open hem with the source's small gap (P28 matches).
- **Fold (sketched bend).** Fusion puts the bend line at the start, centre or end of the bend; P28's `materialInside`
  lies between centre and end (ISSUES 18), so its carried result stays.
- **Refold takes no fixed face.** It keeps the unfold's; after a flat-state cut that crosses that face it keeps the
  other side fixed (P41 refolds the plate instead of the flange). The add-in checks the fold's fixed face after
  refolding and reports the failure; P41 falls back.
- **Lofted flange** from open profiles (`Component.createOpenProfile`, one arc each) is refused at `add` with "Failed
  to resolve profile path" (P25, carried result kept).
- **Rib** (`ribFeatures().createInput(curves, thickness, body)`, symmetric, to next) reproduces SolidWorks' rib (P27
  exact).
- **Variable fillet** (`addVariableRadiusEdgeSet` with the end radii) varies the radius by another law than
  SolidWorks: P30 removes 80.05 mm^3 instead of 81.47 and fails the face check; the carried result stays (ISSUES 20).
- **Loft alignment.** P14 (rectangle to circle) is 0.23 % off in volume. Splitting the circle into four arcs in a
  helper sketch (with `isTangentEdgesMerged(false)`, else Fusion merges the arcs back) at the corners' directions, on
  the sketch axes, or by arc length gave the same 73913.87 mm^3 as the plain loft; only a split half-way between the
  corners changed it (73967.90). SolidWorks' 73745.80 is close (1e-4) to a ruled loft with arc-length correspondence
  computed outside Fusion. Not reproducible through Fusion's loft: P14 still falls back (ISSUES 19).
- **Weldment corners.** Members are extruded with a start offset and length that extend or shorten them at each
  corner (direction and offset sign checked on the body made), miters are a split by the bisecting plane and removal of
  the outer piece, copes a combine-cut by the other member's whole outer section. A split's own `bodies` list holds
  only one piece: the pieces are found by the old body's box. A cope can leave slivers beyond the other member's
  rounded edges: only the largest piece is kept. The drawn ISO section (outer radius 2t, inner t) is 0.0024 mm^2 off
  SolidWorks' library section (1.8e-5 in volume), so weldment parts are verified at exchange tolerance and body
  references resolve at 2e-4.
- **Joints at export.** `Joint.geometryOrOriginOne` / `AsBuiltJoint.geometry` give the joint origin in the assembly's
  frame; occurrence face proxies give geometry there too. A plane's `normal` ignores the face's orientation: the
  outward normal comes from the face evaluator. A screw motion link names its one cylindrical joint as both
  `jointOne` and `jointTwo`.

Round 3 (measured):

- **Fold line position.** Fusion's fold with the line at the *start* of the bend puts the bend's tangent line (on the
  stationary side) on the line. SolidWorks' `materialInside` sketched bend has its line at the outer virtual sharp,
  (r + t)·tan(θ/2) further on (P28: 1.657 mm for r 2, t 2, 45°). The line is therefore moved to the bend axis
  projected onto the sheet, taken from `bendGeometry`, in a helper sketch (`<sketch> (Fusion bend line)`, hidden) whose
  line is parallel to the source's line projected into it, at a dimensioned offset.
- **Edge flange height datums.** `length.datum` maps one to one: `outerVirtualSharp` -> outer faces, `innerVirtualSharp`
  -> inner faces, `bendTangent` -> tangent to bend, at `length.distance` (P12, P36, P41: first try). The flange's
  height parameter is linked to the profile sketch's dimension plus the datum's offset (`D1_SK_Flange + 2 mm`).
- **A flange that meets an earlier one.** Fusion treats the meeting as a two-bend corner and cuts the earlier flange's
  bend there (P34: the front flange's bend lost 4 mm at the left corner, 37.7 mm^3); SolidWorks leaves it whole.
  `TwoBendCornerOverride` (every shape), a near-zero rule gap and holding the flange back 0.0002 mm changed nothing.
- **Changing a sheet-metal rule invalidates edges.** Setting the rule's gap recomputes the body: every `BRepEdge`
  found before is invalid ("failed to resolve edge for flange definition"). Find edges after any rule change.
- **The rule's gap is the miter gap** ("Miter Gap set failed: ... < 1e-05 min"); it is in centimetres, minimum 1e-5.
- **Mitred edge flange vs SolidWorks' miter flange** (P35): Fusion's volume is linear in its miter gap (gap 1: -15.3
  mm^3; 0.7: the volume matches but area and centroid z do not): the corner is shaped differently.
- **Corner closure** works on a carried flange converted to sheet metal (P34): `createInput(dominant, submissive)`
  reports `TwoBendCornerClosureType` for edges of the two flange ends; overlap extends the end face by the thickness
  less the gap (+64.8 mm^3) whatever the overlap ratio, transition or width alignment; SolidWorks adds 54.4 mm^3.
- **Variable fillet laws.** Fusion's variable fillet with only end radii is close to straight; intermediate radii
  (`VariableRadiusFilletEdgeSetInput.setMidRadii(radii, positions)`, positions as fractions 0..1) on the cubic
  r0 + (r1 - r0)(3t^2 - 2t^3) reproduce SolidWorks' smooth law at native tolerance with 9 per edge (P30). The
  created feature's `midRadii()` list is **not** in position order (read `midPositions()` for each); each mid radius's
  expression is set from the start and end radius parameters, so editing an end radius keeps the law.
- **Unfold by bend faces.** `UnfoldFeatureInput.bendFaces` with `isUnfoldAllBends(false)` takes the inner cylindrical
  face the payload's `bends[].face` resolves to; Fusion's refold of that unfold, after P41's flat cut, keeps the
  source's fixed face where the source has it. (If it did not, the add-in moves the unfold's stationary face to
  another face of the flat region, with the timeline rolled back, and refolds again; not needed on the samples.)
- **Lofted flange** still refuses open profiles ("Failed to resolve profile path"), with `createOpenProfile` of the
  single arc chained or not.
- `windows.h` defines `near` and `far`: a C++ variable named `near` does not compile in the add-in.

Round 4 (measured):

- **A bend radius override reads a real `ValueInput` in mm** (`BendRadiusOverride.setOverride`): 0.2 (meant as
  2 mm in centimetres) gave a 0.2 mm bend. Like the sheet-metal chamfer; every override is now passed as text with
  its unit.
- **Two-offset edge flange definitions run outward** (`addTwoOffsetsEdge`): +2.707 made the flange 2.707 mm longer
  than its edge; a flange held back from an end takes a negative offset.
- **A fold keeps the region of its face that holds the face's centre.** With two bend lines in one fold, the strip
  between them stayed and both sides folded. A jog is therefore two folds, the far line first (each keeps the fixed
  side, which then holds the centre of what is left of the face).
- **Offset faces work on a sheet-metal body**: moving the sheet's edge face (thickness wide) out by d lengthens the
  sheet by d, and the body stays sheet metal (later flanges and folds accept it).
- **A surface body has no mass properties** (centre of mass at the origin): a surface's fingerprint in a result
  uses the area-weighted centre of its faces and zero moments.
- **A suppressed feature can be unsuppressed and suppressed again** through `TimelineObject.isSuppressed`; the
  exporter does that to read a suppressed feature's definition (Fusion does not compute a suppressed feature).
- **Windows PowerShell 5.1's `ConvertFrom-Json` refuses the manifest** (its `description` has an empty `""` key):
  the build scripts read the version by pattern.

## 4. Design decisions

- **The carried result decides the variant (round 2).** Where the payload does not pin down a Fusion setting (an edge
  flange's height datum, a hem's bend position, a fold's line position, a rib's side), the add-in builds each variant
  and keeps the one whose bodies are the feature's carried result at native tolerance (`matchesCarriedResult`:
  the bodies the result lists plus the earlier bodies it did not replace). If none matches, the carried result is
  used, so a wrong guess can never get into the model.

- **Mapped features that carry a result are checked against it (round 3).** Besides the features that pick their
  variant by the result, every mapped feature that carries a `result` (the spec does this where systems' defaults
  differ, e.g. a loft between sections of different segment counts) is compared with it at exchange tolerance after
  the native rebuild; if it differs, the native feature is deleted and the result is used (P14).

- One NCX file becomes one unsaved Fusion design. A part is rebuilt in the root component.
- Features are named as in the payload; sketches too. Bodies get the payload's body names (matched by fingerprint).
- Configurations: Fusion's configured designs are not built (yet); the active configuration is, features it
  suppresses are suppressed, and the report says which configurations were not created.
- Verification follows spec 6.1: mass (native; exchange where a carried result or a spline is involved), structure
  (every non-opaque feature present under its name), face by face (`ncx_face_compare`). A mass mismatch replaces the
  part by the file's own geometry (`keepPartial` keeps the partial rebuild in a component, for diagnosis).

## 5. What is carried, approximated, or not (import)

Rebuilt natively: sketches (lines, arcs, circles, ellipses, points, fitted splines; coincident, horizontal,
vertical, parallel, perpendicular, tangent, equal, concentric, midpoint, symmetric, collinear, fix; distance,
horizontal, vertical, diameter, radius, angle, offset dimensions, driving and driven, named after the source),
3D sketches, extrude/cut (blind, through all, to vertex/face, mid-plane, two directions, start offset, selected
contours, thin), revolve/revolve cut, constant and multi-radius fillets, chamfers (equal distance, two distances,
distance-angle, with flips), offset planes, axes (face, edge, two planes, two points), points (vertex, centre),
linear and circular patterns (feature and body), mirrors (feature and body), combine, delete body, move/copy body,
shell, draft, sweep (with one guide rail), loft, extruded/revolved/planar surfaces, knit (stitch), split, Hole Wizard
holes (Fusion holes), sheet-metal rule and base/contour flanges, global variables and equations (user parameters and
expressions), suppression in the active configuration, custom properties, materials, appearances; assemblies with
nested sub-assemblies, grounded components, placement, assembly planes, sketches and cuts.

Functional equivalents (reported *approximate*, with the reason):

| source feature | built as | why |
|---|---|---|
| Insert Part (derived part) | the source's sketches rebuilt in the part, each driving dimension's expression = the source's dimension | Fusion's Derive needs saved cloud documents; the layout still drives the part (L01's flex probe passes) |
| weldment structural member | an extrusion per path segment of a drawn profile (tube: outer radius 2t, inner t; pipe) on a plane on the path, extended or shortened at each corner; miters as split and removal, copes as a combine cut | Fusion has no weldments; corner treatments none, miter, butt1, butt2, coped (P46) |
| face fillet | a constant fillet on the edges where its two face sets meet | Fusion has no face fillet |
| vertex chamfer | a split by the plane through its three points and removal of the corner | Fusion has no vertex chamfer |
| thin revolve | a revolve of the wall's outline drawn in the sketch | Fusion's revolve has no thin option |
| point at a face's centre | a point on a sketch on that face, at the recorded position | no face-centre construction point |
| coordinate system | a joint origin at the same point and x axis | Fusion has no coordinate system feature |
| configurations | the active configuration (values, suppressed features) | Fusion's configured designs are not built |
| custom properties | part number, description, and every property as a component attribute (`NCX.properties`) | Fusion has no other custom properties |
| cut lists | attributes on the member bodies (`NCX.cutList`) | Fusion has no cut lists |
| mates | as-built joints (revolute, cylindrical, slider, planar, rigid / grounded) by the freedoms the mates leave; gear, rack-and-pinion and screw mates as motion links | Fusion positions with joints; point, limit, width, symmetric and slot mates are kept as the placement |
| component patterns, mirror components | the instances placed where the file lists them | not live Fusion patterns |
| skipped pattern instances | bodies removed, or the pattern re-spaced | the API cannot suppress instances |

Rebuilt natively in round 2: edge flanges (variant chosen by the carried result; P12, P34 front, P36, P41), hems
(P28), break corners (Fusion sheet-metal chamfer, P28), ribs (P27), unfold (P41), weldment corner treatments (P46),
splines from NURBS (P23).

Rebuilt in round 4, as functional equivalents that match the source at native tolerance: edge flange position
offset (P43: Fusion offset faces moving the sheet's edge face out by the offset, then the flange on the moved edge),
jog (P29: the sheet beyond the jog extended to keep its projected length, then two Fusion folds placed on
`bendGeometry`).

Rebuilt natively in round 3: sketched bend (P28, a Fusion fold placed by `bendGeometry`), unfold by its bend faces and
the fold after a flat-state cut (P41), variable fillet with the smooth law (P30), edge flanges from their `length`
datum (P12, P36, P41, P34 front); Hole Wizard holes from `measuredDiameters` (P11, P42, no longer measured on the
file's geometry).

From the carried result (fixed geometry, reported *approximate*): an edge flange that meets an earlier one (P34 left,
ISSUES 21), closed corner and miter flange (rebuilt to the spec's gap definitions as a Fusion corner closure and as
square-ended edge flanges; both differ from the source only in the corner's bend region, which the spec leaves to
the source system, ISSUES 25), lofted bend (P25: Fusion's lofted flange
refuses open profiles), the loft P14 (Fusion's loft has no alignment control, ISSUES 24), dome, wrap, gusset, end
cap, weldment trim, mirror part. Not shown (no geometry): cosmetic threads; a flat pattern suppressed in the active
configuration is a note (Fusion makes one on demand).

Known gaps (the part falls back to the file's geometry): none on the samples since round 3.

## 6. Export

`Export NCX...` writes sketches, extrude/cut (blind, through all, to face/vertex, symmetric, two sides, start offset,
selected regions), extruded and revolved surfaces from open curves, planar surfaces (patch), knit (stitch), revolve, fillet (one constant set), chamfer (all three kinds), offset planes, remove body, user
parameters and dimension expressions (as equations), body names and fingerprints, and an assembly's components
(document, transform, grounded). Every other timeline feature is written *opaque* with its carried result (the
component's bodies right after it, as STEP; surface bodies too, with `sheet: true`), so any NCX importer can rebuild
the rest of the history. A suppressed feature is unsuppressed for the moment, read like any other (mapped, or opaque
with its result) and written suppressed (round 4). Not written yet: configurations, appearances, materials, properties,
sheet metal, patterns, mirrors, sweeps, lofts. The round-trip check (`tools/roundtrip.py`) imports a sample, exports
the rebuilt design and imports that file again; results below.

Round 2:
- **Joints as mates.** Each joint (and as-built joint) goes out as the mates that leave the same freedom, made from
  faces the two components share: rigid -> `lock`; revolute -> `concentric` (coaxial cylinders on the joint axis) +
  `coincident` (a face pair across the axis); cylindrical -> `concentric`; slider -> two `coincident` face pairs
  whose normals are across the slide; planar -> one `coincident` pair along its normal. A joint to the assembly
  itself uses the assembly's origin planes. Ball and pin-slot joints, and joints without such faces, go out `opaque`
  with the reason (the components still arrive in place). Motion links go out as `gear` (two turning joints,
  `ratio` = the link's values), `screw` (one cylindrical joint, `pitch`) and `rackPinion` (a turning and a sliding
  joint, `travelPerTurn`). L03, L05, L07 round-trip exact with them.
- **Format round 5 on export:** a plane's constraint is the word `distance` (with `flip` for a negative offset);
  fingerprints mark `freeform` bodies; a component's `configuration` is the configuration's name (ISSUES 13).
- **Validator.** Every re-export is checked with `validator/ncx_validate.py` (levels 1-4); results in section 8.

## 7. The NCX test project

The owner created a Fusion project `NCX test` during the work, where test documents may be saved. Nothing needed
saving: every test imports into an unsaved design and closes it. It would allow trying Fusion's real Derive for
Insert Part (which needs saved documents) as a later improvement.

## 8. Per-sample results

Round 5, on the 59 samples of NCX 0.5.3 (the 56 re-exported with SolidWorks add-in 0.5.3, plus P47, P48, L11).
Round 4's text and table are in git history (9ad89b8). Import (`tools/run_samples.py`, every check in spec 6.1;
"exact" = everything rebuilt natively and every check matched; "approximate" = geometry matches, but something was
built as a functional equivalent, taken from a carried result, or reported; "fallback" = the part was replaced by the
file's own geometry, with the reason):

| | exact | approximate | fallback | failed | not tried |
|---|---|---|---|---|---|
| round 1 (55 old samples) | 19 | 31 | 5 | 0 | 0 |
| round 2 (56 samples, core 0.2.0) | 22 | 32 | 2 | 0 | 0 |
| round 3 (placement data) | 25 | 31 | 0 | 0 | 0 |
| round 4 | 25 | 31 | 0 | 0 | 0 |
| **round 5 (59 samples, NCX 0.5.3)** | **26** | **33** | **0** | **0** | **0** |

The 56 older samples give exactly round 4's results with the 0.5.3 re-exports (the same class sample by sample, and the same
first approximations in the table). P47 is exact; P48 approximate (its inscribed hexagon arrives under-defined, as it does in
SolidWorks: section 12); L11 approximate (Insert Part as sketch copies, cut list). Before this round's fixes L11 fell
back (its bracket's sheet-metal body was built in the wrong component).

Round trip (`tools/roundtrip.py`: import, Export NCX, import the exported file): **21 exact, 38 approximate, 0
failed, 0 not tried** (round 4: 19 / 37 of 56; P47 and P48 exact, L11 approximate: the exporter still writes no sheet
metal, so the base flange goes out opaque with its result). Export still writes no sheet metal, holes, lofts,
patterns or variable fillets as mapped features: those go out opaque with their carried results, so their re-imports
are approximate.

Validator (`validator/ncx_validate.py`, levels 1-4, cadquery-ocp 7.9.3, `NCX_CORE_DLL` set to the build's core) on
every re-export: **59 of 59 PASS**; the only warnings are C-NOTICE on the two surface-only parts (P26, P39), as before.

| sample | import | mass | faces | structure | flex | approximations / fallbacks (first ones) | round trip | validator (re-export) |
|---|---|---|---|---|---|---|---|---|
| L01 | approximate | match | match | match | match | L01_Layout (approximate) | exact (11 mapped, 0 as results) | PASS |
| L02 | exact | match | match | match | - |  | exact (4 mapped, 0 as results) | PASS |
| L03 | approximate | match | match | match | - | Point-FrontCentre (approximate); Coincident-Axes (approximate); Limit-PinHeight (approximate); Coincident-Origin (approximate); +2 | exact (8 mapped, 0 as results) | PASS |
| L04 | approximate | match | match | match | - | Pattern-Pegs (approximate); Pattern-Tabs (approximate); Pattern-TabsOnFace (approximate) | exact (8 mapped, 0 as results) | PASS |
| L05 | approximate | match | match | match | - | Width-Tab (approximate) | exact (10 mapped, 0 as results) | PASS |
| L06 | approximate | match | match | match | - | L06_Bracket_Mirrored (approximate); Mirror-Right (approximate) | approximate (8 mapped, 1 as results) | PASS |
| L07 | approximate | match | match | match | - | Slot-Slider (approximate); Symmetric-Pins (approximate) | exact (6 mapped, 0 as results) | PASS |
| L08 | exact | match | match | match | - |  | exact (4 mapped, 0 as results) | PASS |
| L09 | exact | match | match | match | - |  | approximate (8 mapped, 0 as results) | PASS |
| L10 | exact | match | skipped | match | - |  | approximate (2 mapped, 1 as results) | PASS |
| L11 | approximate | match | match | match | - | L11_Layout (approximate); cut list Sheet<1> (approximate) | approximate (7 mapped, 1 as results) | PASS |
| P01_block | exact | match | match | match | - |  | exact (2 mapped, 0 as results) | PASS |
| P02_block_features | exact | match | match | match | - |  | exact (6 mapped, 0 as results) | PASS |
| P03_revolve_shaft | exact | match | match | match | - |  | exact (5 mapped, 0 as results) | PASS |
| P04_ref_plane | exact | match | match | match | - |  | exact (5 mapped, 0 as results) | PASS |
| P05_patterns_mirror | exact | match | match | match | - |  | approximate (7 mapped, 2 as results) | PASS |
| P06_circular | exact | match | match | match | - |  | approximate (4 mapped, 1 as results) | PASS |
| P07_shell | exact | match | match | match | - |  | approximate (3 mapped, 1 as results) | PASS |
| P08_equations | exact | match | match | match | match |  | exact (2 mapped, 0 as results) | PASS |
| P09_draft | exact | match | match | match | - |  | approximate (2 mapped, 1 as results) | PASS |
| P10_configs | approximate | match | match | match | - | configurations (approximate) | exact (4 mapped, 0 as results) | PASS |
| P11_hole_wizard | exact | match | match | match | - |  | approximate (3 mapped, 1 as results) | PASS |
| P12_sheet_metal | approximate | match | match | match | - | Cut-Hole (approximate); cut list Sheet<1> (approximate) | approximate (4 mapped, 2 as results) | PASS |
| P13_sweep | exact | match | match | match | - |  | approximate (2 mapped, 1 as results) | PASS |
| P14_loft | approximate | match | match | match | - | Loft-Transition (approximate) | approximate (3 mapped, 1 as results) | PASS |
| P15_multibody | exact | match | match | match | - |  | approximate (6 mapped, 1 as results) | PASS |
| P16_weldment | approximate | match | match | match | - | Weldment (approximate); Tube-Frame (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<1> (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<2> (approximate); +1 | approximate (13 mapped, 0 as results) | PASS |
| P17_properties | approximate | match | match | match | - | custom properties (approximate); material 6061 Alloy (approximate); configurations (approximate) | exact (2 mapped, 0 as results) | PASS |
| P18_appearances | approximate | match | match | match | - | appearance on body (approximate) | exact (6 mapped, 0 as results) | PASS |
| P19_extrude_options | exact | match | match | match | - |  | approximate (9 mapped, 1 as results) | PASS |
| P20_ref_geometry | approximate | match | match | match | - | Point-TopCentre (approximate); CS-Corner (approximate); Rev-Thin (approximate) | approximate (4 mapped, 1 as results) | PASS |
| P21_fillets | approximate | match | match | match | - | Fillet-Faces (approximate); Chamfer-Vertex (approximate) | approximate (8 mapped, 2 as results) | PASS |
| P22_bodies | approximate | match | match | match | - | Pattern-Posts (approximate); Cut-Hole (approximate); Pattern-Holes (approximate) | approximate (8 mapped, 3 as results) | PASS |
| P23_sketch_curves | exact | match | match | match | - |  | exact (4 mapped, 0 as results) | PASS |
| P24_weldment_extras | approximate | match | match | match | - | Weldment (approximate); Tube-Frame (approximate); Tube-Brace (approximate); Tube-Post (approximate); +8 | approximate (20 mapped, 2 as results) | PASS |
| P25_lofted_bend | approximate | match | match | match | - | Loft-Transition (approximate); cut list Sheet<1> (approximate) | approximate (3 mapped, 1 as results) | PASS |
| P26_surfaces | exact | match | match | match | - |  | exact (4 mapped, 0 as results) | PASS |
| P27_rib_move | exact | match | match | match | - |  | approximate (8 mapped, 4 as results) | PASS |
| P28_sheet_extras | approximate | match | match | match | - | cut list Sheet<1> (approximate) | approximate (3 mapped, 4 as results) | PASS |
| P29_jog | approximate | match | match | match | - | Jog-Step (approximate); cut list Sheet<1> (approximate) | approximate (3 mapped, 4 as results) | PASS |
| P30_variable_fillet | exact | match | match | match | - |  | approximate (4 mapped, 1 as results) | PASS |
| P31_guide_curves | exact | count_only | match | match | - |  | approximate (8 mapped, 2 as results) | PASS |
| P32_cosmetic_thread | approximate | match | match | match | - | Thread-M10 (approximate) | exact (2 mapped, 0 as results) | PASS |
| P33_gusset | approximate | match | match | match | - | Weldment (approximate); Tube-L (approximate); Gusset-Corner (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<1> (approximate); +3 | approximate (7 mapped, 1 as results) | PASS |
| P34_closed_corner | approximate | match | match | match | - | Flange-Left (approximate); Corner-Closed (approximate); cut list Sheet<1> (approximate) | approximate (5 mapped, 6 as results) | PASS |
| P35_miter_flange | approximate | match | match | match | - | Miter-Upstand (approximate); cut list Sheet<1> (approximate) | approximate (3 mapped, 3 as results) | PASS |
| P36_multi_edge_flange | approximate | match | match | match | - | cut list Sheet<1> (approximate) | approximate (3 mapped, 2 as results) | PASS |
| P37_dome | approximate | match | match | match | - | Dome-Top (approximate) | approximate (6 mapped, 1 as results) | PASS |
| P38_split | exact | match | match | match | - |  | approximate (4 mapped, 1 as results) | PASS |
| P39_knit | exact | match | match | match | - |  | exact (5 mapped, 0 as results) | PASS |
| P40_wrap | approximate | match | match | match | - | Wrap-Label (approximate) | approximate (5 mapped, 1 as results) | PASS |
| P41_unfold_fold | approximate | match | match | match | - | cut list Sheet<1> (approximate) | approximate (4 mapped, 4 as results) | PASS |
| P42_hole_positions | exact | match | match | match | - |  | approximate (3 mapped, 1 as results) | PASS |
| P43_flange_options | approximate | match | match | match | - | Edge-Offset (approximate); cut list Sheet<1> (approximate) | approximate (2 mapped, 3 as results) | PASS |
| P44_cut_list | approximate | match | match | match | - | Weldment (approximate); Tube-Frame (approximate); cut list Long rail<1> (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<2> (approximate); +1 | approximate (13 mapped, 0 as results) | PASS |
| P45_sketch3d | approximate | match | match | match | - | Weldment (approximate); Tube-Path (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<1> (approximate); cut list TUBE, SQUARE 20.00 X 20.00 X 2.00<2> (approximate); +2 | approximate (11 mapped, 0 as results) | PASS |
| P46_weldment_corners | approximate | match | match | match | - | Weldment (approximate); Tube-Miter (approximate); Tube-Butt1 (approximate); Tube-Butt2 (approximate); +7 | approximate (73 mapped, 12 as results) | PASS |
| P47_slots | exact | match | match | match | - |  | exact (4 mapped, 0 as results) | PASS |
| P48_polygons | approximate | match | match | match | - | SK_Hex (approximate) | exact (4 mapped, 0 as results) | PASS |

Import totals: approximate 33, exact 26

## 9. Round 2: what changed

Format round 5 (each used, and whether it helped):

| change | used for | effect |
|---|---|---|
| enumerated words | plane constraint, hole type and end, structural member corners and coping (`neutral.h`, numbers still read) | needed: without it every reference plane failed |
| `noticebox` | the notice solid found by its box | replaced the geometric guess; exact on every part file |
| spline `nurbs` | fixed splines from the exact curve; also exported | P23 fallback -> exact |
| hole `holeType`, `end` | the kind and the end condition | used; `measuredDiameters` is not in the samples (ISSUES 11), the diameter is still measured on the file's B-rep |
| vertex chamfer `legs` | which distance goes along which edge | removes the permutation search (P21) |
| equation `targetRef` | the target parameter by sketch/dimension id or feature field | used for P08, L01 (flex probes match) |
| `freeform` fingerprints | exchange tolerance for spline-faced bodies; written by the exporter | P31 now verifies (exact); P23's body too |
| `cornerTreatment`, `connectedSegments` | weldment corners (P46) | P46 approximate (geometry matches), not a fallback; the spec's direction rule had to be corrected (ISSUES 12) |
| directions section | confirmation | matched round 1's findings |
| validator | every re-export | found two exporter bugs |

Beyond the format:
- Native sheet metal where the carried result decides the variant: edge flange (P12, P34 front, P36, P41), hem
  (P28), break corner (P28), rib (P27 now exact), unfold (P41), with refold, fold, lofted flange and variable fillet
  tried and rejected by the result (FINDINGS 3).
- Cuts on sheet metal retried without participants and two-sided (P12 fallback -> approximate).
- A plane with no references the importer can use is built at its recorded position (three points).
- Export: joints and motion links as mates, splines with NURBS, non-planar sketches as `sketch3d`, enumerated
  words, configuration names.
- Tooling: `tools/results_table.py` has a validator column (`results/validate_rt.json`).

## 10. Round 3: what changed

The owner's answers to issues 11-20, each used, and what it did:

| change | used for | effect |
|---|---|---|
| hole `measuredDiameters`, filled `holeDiameter` | the drilled and counterbore diameters | P11, P42 approximate -> **exact** (no measuring of the file's B-rep) |
| weldment segment direction (spec) | confirmation of the chain direction the add-in used | P46 unchanged (corners match; approximate as a functional equivalent) |
| component `configuration` is a name | confirmation; the exporter writes names | every assembly re-export passes |
| `reliefType: default` | P35 reads a word | no number left in the samples |
| validator pins `cadquery-ocp<8` | setup | level 4 runs from the README's instructions |
| unfold/fold `bends[].face` | the unfold unfolds the named bend by its face; the fold is paired with its unfold by the bends' source ids (the faces differ between the two) | P41 fallback -> approximate (native unfold, flat cut, fold) |
| edge flange `length {distance, datum}` | Fusion's height datum and height directly; the height parameter linked to the profile dimension plus the datum offset | P12, P36, P41, P34 front at the first try (were found by a search); P34 left still carried (Fusion trims the earlier flange, ISSUES 21) |
| `bendGeometry` | the sketched bend's Fusion line moved to the bend start (helper sketch, dimensioned offset from the source's line) | P28 `Bend-Line` carried -> **native** |
| sketched bend `materialInside` definition | confirmed: the offset is (r + t)·tan(θ/2) | as above |
| edge flange `positionOffset` definition | none: Fusion has no offset (ISSUES 22) | P43 still carried |
| loft `connectors` + `result` | the loft's result checks the native loft and replaces it when it differs (any mapped feature with a result is now checked) | P14 fallback -> approximate; Fusion cannot follow the connectors (ISSUES 24) |
| variable fillet `transition` | intermediate radii on the smooth (cubic) law, linked to the end radii | P30 approximate -> **exact** (native tolerance) |
| validator M-BEND-GEOMETRY, M-LOFT-CONNECTOR, M-ENUM-NUMBER, M-HOLE-SIZE | every re-export | 56/56 PASS (the Fusion exporter writes none of these features as mapped yet) |

Also tried with the new data, and kept from the carried result (each reported approximate with the reason): closed
corner as a Fusion corner closure (P34; Fusion's overlap differs, ISSUES 25), miter flange as a mitred edge flange
with the rule's miter gap (P35; the corner differs, ISSUES 25), lofted bend (P25; Fusion's lofted flange refuses
open profiles), jog (P29; no counterpart, ISSUES 26).

Tooling: a `params` probe job (every parameter's name, expression and value) to check parameter links.

## 11. Round 4: what changed

The owner's answers to issues 21-26 (FORMAT-SPEC §4 only), each used:

| answer | used for | effect |
|---|---|---|
| edge flange `positionOffset` defined (flange moved out, sheet extended to its bend) | Fusion offset faces on the sheet's edge face by the offset, then the edge flange on the moved edge | P43 `Edge-Offset` carried -> rebuilt (native-tolerance match, reported approximate) |
| jog: offset per `dimensionPosition`, `fixProjectedLength`, two bends on `bendGeometry` | the sheet beyond the jog extended by what the bends use (7.425 mm on P29), then two folds placed on the bends (the far one first); the word offset is checked against the bends' (10 = 10) | P29 `Jog-Step` carried -> rebuilt (native-tolerance match, reported approximate) |
| miter flange `gap`: square ends, inner end edges gap apart | one Fusion edge flange on both edges, each end held back 0.707 from the other flange's inner face | the box matches; 15.28 mm^3 short (0.09 %): the corner's bend region, the source's own; P35 keeps its result |
| closed corner overlap / underlap / butt with `gap`, `overlapRatio` | confirmation: Fusion's corner closure extends the flat part as defined (+64.8) | the source also extends the flange's bend and re-shapes the corner (+70.58 / -16.15, measured on the results); P34 keeps its result |
| variable fillet smooth law | confirmation (the law P30 already uses) | P30 exact, unchanged |
| issue 21 in the brief's traps | none needed | P34 `Flange-Left` keeps its result |

Exporter (validator warnings of round 3): a suppressed feature is read unsuppressed and written mapped and suppressed
(P10); results carry surface bodies (`sheet: true`, a surface fingerprint); extruded and revolved surfaces from open
curves, planar surfaces and knits are mapped (P26, P39 round trip exact). The importer finds the surfaces a result
replaces with the same surface fingerprint.

Ready to publish:
- version 0.1.0, set once in `CMakeLists.txt` (`NCX_FUSION_VERSION` for both DLLs); `build.ps1` checks the manifest;
- `build.ps1` finds Visual Studio 2019 or 2022 (generator from the installed version) and has `-Clean`; a build from an
  empty build folder with the README's command took 98 s and deployed;
- the DLLs depend only on Windows and Fusion's own DLLs (`dumpbin /dependents`: XInterface10, FusionXInterface10,
  KERNEL32, SHELL32, ole32; ncx_core: KERNEL32 only): no C runtime to install;
- `package.ps1` writes `fusion\out\NCX-Fusion-0.1.0.zip` (`NCX/` with the manifest, the three DLLs, the README,
  the licence). Tested: Fusion closed gracefully (no document open, no autosaved document), the developer add-in
  folder moved aside, the zip extracted into `%APPDATA%\Autodesk\Autodesk Fusion 360\API\AddIns`, Fusion
  started: the add-in started with it (log: "NCX add-in 0.1.0 starting in Fusion 2706.1.14") and imported P01 (exact),
  P29 and P43 through the job folder. The developer install (build.ps1) was then put back. (2026-10-24: the zip was
  replaced by a per-user installer, `installer\NCX-Fusion.iss`, which `package.ps1` now builds instead: installed
  silently, the add-in started with Fusion 2706.1.14, and the uninstaller removed it; users are not expected to copy
  files into AppData by hand);
- tracked files hold no path of this machine (`rebuild.sh` takes `$PYTHON`); `tools/__pycache__` is untracked;
  `fusion/.gitattributes` keeps `*.sh` with LF line endings.

## 12. Round 5: what changed

The spec's 0.5.3 additions, each met in a sample:

| spec item | sample | what happened first | now |
|---|---|---|---|
| points sharing a position are separate entities | P47 (p9 on the slot's centreline, p15 the construction line's end, `coincident`) | exact at the first import: the importer already made standalone points separately and merges only by payload id | exact |
| slots as plain lines/arcs with `equal`, `tangent`, `midpoint` | P47 | exact at the first import | exact |
| `patterned` (polygon sides) | P48 | "relation patterned has no Fusion counterpart here" x12 | each pair `equal`; `parallel` only where the two lines are parallel at their seeds (a polygon's sides are not: spec §4, `patterned`). The circumscribed hexagon is fully defined; the inscribed one (one `tangent` only) leaves its circle and five sides under-defined (Fusion's own per-curve check), reported "the source sketch is fully defined, the rebuilt one is not", as SolidWorks reports it (ISSUES 27) |
| `sketchEntity` of another document, off the sketch plane | L11 `SK_Holes` (layout points at z 0, the sketch on the sheet's face at z -2) | "sketch f:1 was not rebuilt": references were looked up among the part's own sketches only | looked up in the Insert Part copy (`T:<document>|<feature>`, same entity ids) first; `Sketch.project` gives the projected point; the sketch arrives fully defined |
| cut `linkToThickness` | L11 `Cut-Pivots` | blind 2 mm | Fusion's expressions cannot name the rule's thickness ("Expression is invalid"), and the rule's thickness refuses a user parameter ("Invalid expression"), measured; so the cut runs **up to the sheet's opposite face** (the planar face parallel to the sketch plane at the depth, under the profile), which follows the thickness. Reported in the feature's info |
| cut `normalCut` | L11 `Cut-Pivots` | ignored | a straight cut; checked by the removed volume: equal to profile area x depth (1e-6) means the profile lies on a flat region, where a straight and a normal cut are the same (info); otherwise reported approximate |
| mate `suppressed` | L11 `Distance-Lift` (`{"cfg:active": true}`) | folded into the pin's revolute joint and reported "rebuilt" (the key is not a configuration id, so it read as active) | left out of the joints and reported as a note; the components stay at their transforms. Key: the active configuration's id, `cfg:active`, or a single key (ISSUES 29) |

Fusion facts found (measured):

- **Sheet-metal features build in the active component.** A base flange made with `comp.features().flangeFeatures()`
  of a component under an occurrence put its body into the **root** component (L11: the bracket had no body, its hole
  sketch found no face, the part fell back and the assembly counted 3 bodies for 2). The assembly import now
  activates each part's build occurrence while the part is rebuilt (`assembly.cpp`), then the root again. The 10
  older assemblies are unchanged by it.
- **Rule thickness and expressions.** See the table; neither direction of link works, so "follows the thickness" is
  an extent to the opposite face.
- **Projected entities at export.** `SketchEntity.referencedEntity()` of a point projected from another sketch of a
  part under an occurrence is an assembly-context proxy: its `entityToken` is not the native entity's. Taking the
  token of `nativeObject()` finds the source; the relation goes out as a `sketchEntity` reference (a projected
  component origin as `origin`). The end points of projected curves are not marked `isReference` themselves: they
  were written as free points, which left re-imported sketches under-defined (L06 `SK_Post`, L11 `SK_Holes`); they
  are now skipped. A fixed origin is no longer written as a `fix` relation.
- **Suppressed joints at export** go out as suppressed mates (`cfg:active`) instead of being skipped silently.

Tools: `tools/tree.py` (probe `tree`: components, bodies with volume and box, occurrence offsets); the `sketches`
probe reports each sketch's and curve's `isFullyConstrained`. `tools/rebuild.sh` prints BUILD FAILED and stops when
the compile fails (it went on to reload the old DLL before).

Not done this round: the version is still 0.1.0 (no installer was built or released); export of sheet metal,
holes, lofts and patterns as mapped features is unchanged (opaque with results).

## 13. Round 6: progress, responsiveness, a real assembly

### Progress and Cancel

Before: `progressStep` pumped Fusion's events only when the dialog existed, and job-driven imports showed none, so a
job import gave no sign of life at all (the real assembly: 213 s); steps were per feature only, so in the menu import
the longest gaps were 94 s (the reference: Fusion's STEP import plus face descriptors) and 39 s (one sketch).

Now (`common.cpp`, `importer.cpp`, `reference.cpp`, `assembly.cpp`, `host.cpp`, `joints.cpp`, `sketch.cpp`,
`exporter.cpp`):
- the dialog shows for every import and export, job-driven ones too (`"progress": false` hides it); the title names
  the file, the message the document and the feature or phase;
- updates and event pumping are throttled to one per 0.25 s (the owner found per-step updates visibly slower); a step
  inside that window only counts; Cancel is checked at every pump;
- ticks inside long phases: the reference (per occurrence, per face), every geometric measurement (`closest`), sketch
  relations and dimensions, face checks, placement, joints, the whole-model check, flex, the fallback, export's walk;
- Cancel closes every document the import created (scratch STEP documents too); in export a feature unsuppressed or
  rolled back for reading is finished first (`ProgressNoCancel`), the timeline goes back to its end, no file is
  written;
- face descriptors of the file's geometry are ~8x faster: the distance of candidate sample points to the face's
  boundary is estimated on edge polylines and measured exactly only for the chosen point; planar facet centroids are
  not projected (the reference phase of the real assembly: 94 s -> 12 s);
- the reply carries `progress`: steps, updates, `doEvents` calls, phase seconds, the 15 longest gaps.

| import | before (round 5 code) | now |
|---|---|---|
| L04 | 3.4 s, longest gap 2.5 s | 2.0 s, 4 updates, longest gap 0.9 s |
| L06 | 2.4 s, longest gap 1.2 s | 2.3 s, 4 updates, longest gap 0.8 s |
| folding step | 213 s (job: no update at all; menu: gaps 94 s, 39 s) | 221 s, 503 updates, longest gap 8.6 s |

The folding step now rebuilds far more (3 parts fall back instead of 11), which costs replay time (54 s -> 180 s; the
Step Plate alone 57 s). The gaps over 2 s are single Fusion calls: one sketch constraint in the Step Plate's
`Sketch7` (a sketch on a face with 37 symmetric relations, several to model edges: 4-9 s each), an extrusion
(`Boss-Extrude17`, 3.7 s), a fillet (3.5 s). Fusion's STEP import of the 3.4 MB file is ~1 s.

Cancel, tested as a user would (`tools/cancel_test.py`: Escape on the dialog during the folding step's import, at
12 s and at 30 s): the import stopped within 0.4 s ("cancelled by the user at Left_Top Hanger Cheek: Fillet12;
everything this import made was closed"), and no document was left open.

Regression: the 59 samples give exactly section 8's classes (26 exact, 33 approximate, 0 fallback; every sample the
same class).

### The folding step (real assembly)

| document | round 5 code | now |
|---|---|---|
| M8x20 Dome Head + Washer | fallback | rebuilt (approximate); mass match, 1 face ambiguous |
| Chassis Attachment Plate | fallback | rebuilt (approximate); mass, faces, structure match |
| Top Hanger Cheek | fallback | fallback (12 features fail; see below) |
| Top Hanger Cheek Opposite | fallback | fallback (2 fail) |
| Long Link | fallback | rebuilt; mass count_only; 3 small faces differ |
| IGUS WFM-0810-04 | exact | exact |
| M8 Nyloc + Washer | fallback | every feature rebuilt, volume 5.6e-5 high: fallback |
| Short Link | fallback | rebuilt; mass, faces match |
| Step Rung | fallback | rebuilt; mass, faces match |
| Step Plate | fallback | rebuilt; mass count_only; 2 small faces ambiguous |
| Nissan Window Regulator | fallback | rebuilt; mass count_only; 1 face ambiguous |
| M8x20 Dome Head | fallback | rebuilt; mass, faces match |
| layouts (2) | rebuilt | rebuilt (Insert Part copies) |
| 5 sub-assemblies, top | placed | placed; whole model count_only (40 bodies, mass properties match) |

Fixes (general, each found on this model):
- `regions` were ignored (every region-selected extrude took the whole sketch): now the profile containing the point,
  checked by area; when Fusion splits the source's region with curves the add-in projected, those are made
  construction and it is tried again; when the region is larger, the model edges in the sketch plane are projected
  (ISSUES 31);
- several `contours`, or a contour another curve crosses (a hexagon in a circle): the profiles inside the contours, a
  contour inside another a hole, crossing ones united (ISSUES 30);
- profile loops of sketches on XZ-parallel planes: their geometry came back in sketch space and was converted again,
  so nested circles no longer nested and rings were extruded as discs;
- whole-sketch profiles: crossing parity per curve (curves need not run in loop order) and only the profiles bounded
  by the payload's own curves (a projected bend edge had closed an extra region that a cut took);
- `convertToSheetMetal`: the result, made a Fusion sheet-metal body from the fixed face with the rule's thickness and
  bend radius (later edge flanges then build natively: the rung's `Edge-Flange1`);
- start offset through-all against the normal (Fusion refuses it) and refused through-all cuts: blind past every body;
- `noGeometry` opaque features: a note, not a failure;
- a failing cut names the profile's and the bodies' boxes; a part that falls back reports which faces differ.

Open on this model: Top Hanger Cheek's `Boss-Extrude7` and `Cut-Extrude11` regions (the model-edge projection finds a
region 1.6 % too large, or none), and after them fillets and sketches on faces that differ; the Opposite's
`Cut-Extrude10` region (0.75 % too large); the nut's 5.6e-5 volume difference (every feature rebuilt; one coplanar face
ambiguous; not found); Long Link's 3 faces (the exporter notes "1 selected contour not readable through SolidWorks'
API" on its `Cut-Extrude1`, and the normal cut is made straight).

### Joints of the folding step (round 6, second task)

The user dragged `Left_Step Assy` and it came away from the links. Probe `{"cmd":"probe","what":"joints"}` on that import:
every joint healthy and made at the right level (top-level mates between parts of sub-assemblies became as-built joints
between the top-level sub-assembly occurrences; mates inside a sub-assembly, joints in that component); only
`Left_Top Hanger Assy` grounded. The cause was the rule "one joint per moving component, to the partner it shares most
mates with": a closed chain stays open. `Left_Step Assy` was joined only to `Left_Long Link Assy`, `Left_Short Link Assy`
only to the hanger, and every bolt to one part, so the four-bar linkage (hanger, long link, step, short link) had no
joint between the step and the short link. Now every pair of mated components gets a joint (`Concentric9`, step to
short link, cylindrical). Fasteners are joined to one part only: a part under a tenth of the volume of every part it is
mated to. With every fastener joined to both parts it passes through, and to the fasteners it touches, Fusion's solver
locked and no joint could be driven.

Measured with the probes:
- `jointmove` drives a joint's value. Driving `Distance8` (long link to hanger) moves the long link and its nuts, but
  the step and the short link stay put: driving a value does not solve the closed loop.
- `occdrag` moves an occurrence with `transform2`. That does not call the joint solver at all.
- `Design.computeAll()` throws away every position not captured in a snapshot (it reverted every probe move until it
  was removed from the probes).

So whether Fusion's interactive drag carries the closed four-bar loop cannot be checked through the API. The user has
to try it in Fusion. The L01-L11 assemblies keep their classes.

### Grounding, Pin and joints in Fusion 2706 (round 6, third task)

What the user saw in the browser (screenshot `fusion/results/fusion_window.png`, not committed): `Left_Top Hanger
Assy:2` (the same occurrence the API reported, no :1/:2 mismatch) without a pin icon, and the seven `Left_M8x20 Dome
Head + Washer` instances with an anchor icon. The probe, extended with `Occurrence.isGroundToParent`, showed those seven
grounded to their parent (the root, so fixed in space) although the add-in never set it: their build occurrence
(`addNewComponent`, the first part document) came out grounded to parent, and the instances made from it with
`addExistingComponent` inherited it. Inside each sub-assembly nothing held the parts to the sub-assembly: the
sub-assembly's fixed part had been given `isGrounded` (Pin) on a native occurrence inside a component, which does not
fix it to its parent. So dragging a part moved it inside its sub-assembly, away from the top-level joints, which act on
the sub-assembly occurrences.

Fusion's current model (sources below):
- the old **Ground** command is now **Pin** (fixes an occurrence in the design's space; API `Occurrence.isGrounded`,
  "whether this occurrence is pinned"); **Ground to Parent** (2024, API `Occurrence.isGroundToParent`, anchor icon)
  fixes a component to its parent component, so it moves with it;
- components of a sub-assembly are not rigid in the parent by design; ground to parent, rigid groups or as-built rigid
  joints hold them;
- pinning a nested occurrence through the API was described by Autodesk as a bug (2017): proxies are the way to address
  nested occurrences;
- four-bar linkages with joints are dragged in Fusion; the closing joint is usually made cylindrical so it can seat
  (the folding step's closing joint, `Concentric9`, is cylindrical).

Fix (ec7c4af): at the top level a fixed component is pinned and every other one explicitly not pinned and not grounded
to parent; in a sub-assembly the fixed component is grounded to parent and the others are neither. Layouts and partial
rebuilds are moved into one hidden component, "NCX source parts (layouts, partial rebuilds)", pinned and hidden. On the
folding step the probe then shows: the hanger assembly pinned; `Left_Top Hanger Cheek`, `Left_Long Link`,
`Left_Short Link`, `Left_Step Plate` grounded to their sub-assemblies; no fastener grounded; the top level holds only
the assembly's components and the hidden one. L02 (a sub-assembly) and L11 show the same pattern. One import of the
folding step during this work failed with "unexpected structured exception in the implementation" after `Left_Step
Rung`; the next, identical one did not (not reproduced).

Upright view: Fusion here is Z-up (`GeneralPreferences.defaultModelingOrientation`); a SolidWorks file (Y-up) now
opens with the camera turned so Y is up on screen, reported as a note; coordinates, checks and export unchanged
(ISSUES 34). The ground grid stays Fusion's XY.

Sources: [Ground to Parent (Autodesk blog)](https://autodesk.com/products/fusion-360/blog/introducing-ground-to-parent-a-new-assembly-tool-in-autodesk-fusion),
[Occurrence.isGrounded](https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/Occurrence_isGrounded.htm),
[Occurrence.isGroundToParent](https://help.autodesk.com/cloudhelp/ENU/Fusion-360-API/files/Occurrence_isGroundToParent.htm),
[isGrounded on nested occurrences (forum)](https://forums.autodesk.com/t5/fusion-360-api-and-scripts/i-am-isgrounded/m-p/7169797),
[Ground component? (forum)](https://forums.autodesk.com/t5/fusion-support-forum/design-ground-component/td-p/12953410),
[joints broken / sub-assemblies not rigid by design (forum)](https://forums.autodesk.com/t5/fusion-support-forum/my-assembly-joints-are-broken-after-i-updated-fusion-360-now/m-p/8889452),
[Four bar linkage (forum)](https://forums.autodesk.com/t5/fusion-design-validate-document/four-bar-linkage/m-p/6764318),
[Motion link 4-bar tutorial](https://cad.grabcad.com/tutorials/motion-link-4-bar-mechanism-using-autodesk-fusion-360).

### Joint frames and rigid sub-assemblies (round 6, fourth task)

The user dragged the open import: links turned about a point a metre away, and the Step Plate came away from
`Left_Step Assy`. The causes, measured:
- **Joint frames.** A mate's face is described in the referenced part's own frame (checked: for every mate, the
  descriptor's axis, placed by the part's transforms, lies on the mate's `frame` axis), and Fusion measures a
  component's faces where an occurrence puts them. The joint builder measured through the leaf's native occurrence,
  which is the part's place inside its sub-assembly. Nested references did not resolve, and their joints fell back
  to the component origin with its Z axis. Now the faces are measured through the leaf's full path: through the
  sub-assembly's occurrence in the root for a sub-assembly's own mates. A joint that still falls back to an origin is
  reported.
- **Reading a joint back.** `JointGeometry.primaryAxisVector` of a joint made on a face of a placed part comes back in
  that part's own frame. A bolt turned 90 deg read its own -Y for the assembly's X; the origin comes back in the
  assembly's frame. The check turns the axis by the part's placement. A sub-assembly's own as-built joints read from
  its component (or through `createForAssemblyContext`) have no occurrences and no geometry. So the import records
  every joint's frame as it is made (reply `jointFrames`).
- **Result on the folding step** (`fusion/results/frames_table.py`, against the axial mate of each pair, assembly
  frames): every revolute and cylindrical joint's axis within 1e-15 of its pin, and its origin on the pin's axis
  (0.0000 mm). The origin sits along the axis at the middle of the cylinder face instead of the mate's face, by
  0.7 to 7 mm, which does not change the motion. Pairs held on two parallel pins (the regulator, the hanger's
  cheeks, the step plates) are now rigid joints, not revolute joints about one of the pins.
- **Rigid sub-assemblies.** SolidWorks sub-assemblies are rigid unless set flexible, and their mates hold their parts
  either way. The importer keeps every sub-assembly mate as a joint in the sub-assembly and grounds every component of
  a rigid sub-assembly to it (Ground to Parent). The grounding is set before the joints: set after, Fusion refused it
  on one step plate already joined. A rigid group was tried first and replaced by this, because ground to parent shows
  in the browser and can be read back. ISSUES 35 proposes `flexible: true` on a component entry.
- **Developer option.** `"onlyAssemblies": [names]` builds those sub-assemblies (and their parts) without the rest.
