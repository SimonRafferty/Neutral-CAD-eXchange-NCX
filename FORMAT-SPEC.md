# NCX single-file neutral CAD format — specification (draft 0.1)

Status: draft, 2026-10-21. Normative words: MUST, SHOULD, MAY. The limits in §1-2 were measured on SolidWorks
2018/2025/2026 and OpenCASCADE readers.

An NCX file is an ordinary STEP file. Any CAD application opens it as plain B-rep geometry with its
assembly structure. A companion add-in also finds an embedded **payload** (the neutral modelling
history, §4) and rebuilds the full parametric model from it.

## 1. Container

1. The file MUST be an ISO 10303-21 exchange structure whose `FILE_SCHEMA` is AP214
   (`AUTOMOTIVE_DESIGN`).
   - Writers MUST NOT declare AP242 for assemblies, because SolidWorks 2018 never returns from
     importing an AP242 assembly.
   - Readers MUST accept AP203, AP214 and AP242.
2. The B-rep and product structure SHOULD be produced by the source CAD system's own STEP exporter.
   The payload is then added by text insertion (§2.4); no other byte of the export changes.
3. Every STEP string literal the writer produces MUST be at most **8,000 characters**, because
   OpenCASCADE (and so FreeCAD) rejects a whole file containing any string longer than 16,380.

## 2. Payload placement

### 2.1 Entities

The payload is one property of the **root** PRODUCT_DEFINITION, which is the one never used as
the related (component) side of a NEXT_ASSEMBLY_USAGE_OCCURRENCE:

```
#c  = REPRESENTATION_CONTEXT('','');
#g  = GENERAL_PROPERTY('NCX','NCX','');
#p  = PROPERTY_DEFINITION('NCX','',#<root PRODUCT_DEFINITION>);
#a  = GENERAL_PROPERTY_ASSOCIATION('','',#g,#p);
#h  = DESCRIPTIVE_REPRESENTATION_ITEM('NCX','<header>');
#k0 = DESCRIPTIVE_REPRESENTATION_ITEM('NCX.0000','<chunk 0>');
...
#r  = REPRESENTATION('NCX',(#h,#k0,#k1,...),#c);
#d  = PROPERTY_DEFINITION_REPRESENTATION(#p,#r);
```

- Chunk items are named `NCX.` followed by a decimal index, zero-padded to at least 4 digits and
  counting from 0.
- Readers MUST order chunks by that index, not by file position or item order.
- Readers MUST ignore line breaks inside string literals (Part 21), so a payload re-wrapped by
  another writer still reads.

Why this shape:
- SolidWorks maps a PROPERTY_DEFINITION to one custom property named after it, holding the
  *first* item's value. A user without the add-in therefore sees one property, `NCX`, holding the
  header: in 2025/2026 always, and in 2018 with 3D Interconnect on.
- `GENERAL_PROPERTY` lets OpenCASCADE's metadata reader see the property.
- The bulk chunks cost readers that ignore them nothing.

### 2.2 Header

The header is one line of at most **1,000 characters** (SolidWorks 2018 silently drops longer
property values). It starts with the container marker `NCX/1`, followed by `;key=value` pairs:

| key | meaning |
|---|---|
| `schema` | payload schema version `MAJOR.MINOR` |
| `enc` | `deflate-b64` (the only encoding defined) |
| `chunks` | number of chunk items |
| `b64len` | total base64 length |
| `jsonlen` | length of the payload JSON in bytes |
| `sha256` | lower-case hex SHA-256 of the payload JSON bytes |
| `fp` | 16-hex fingerprint hash (§6), or `none` |
| `core` | ncx_core version that wrote it |
| `gen` | free text naming the exporter |
| `notice` | the name of the notice body (§2.6), if the file has one |
| `noticebox` | the notice body's bounding box in the file's frame, mm: `xmin,ymin,zmin,xmax,ymax,zmax` |

Readers MUST ignore unknown keys. Values never contain `;`, `'` or `\` (writers replace them).

### 2.3 Encoding

```
payload JSON (UTF-8) --raw DEFLATE, RFC 1951--> --base64, RFC 4648 standard alphabet, padded, no line breaks-->
split into consecutive chunks of <= 8000 characters
```

### 2.4 Writing

1. Remove any existing NCX payload first: the PROPERTY_DEFINITION named `NCX` and every entity
   that belongs only to it. A file never carries two.
2. Insert the new records immediately before the DATA section's `ENDSEC;`, with entity ids above
   the file's current maximum.
3. Keep the file's line-ending style.
4. Each part's STEP PRODUCT should carry the name of its payload document (`name`), so a reader can find a
   document's plain geometry when its rebuild fails. Readers MUST NOT rely on it: some systems write their own
   product names (Fusion writes time stamps), so a reader that finds no product by name matches the document's
   body fingerprints against the products' geometry instead.

### 2.5 Reading

Readers report exactly one status:

| status | condition |
|---|---|
| `OK` | payload found and intact |
| `NO_PAYLOAD` | no `PROPERTY_DEFINITION` named `NCX` whose definition is a PRODUCT_DEFINITION |
| `HEADER_UNREADABLE` | no item named `NCX`, or its value does not start with `NCX/1`, or required keys missing |
| `CHUNK_MISSING` | fewer chunk items than `chunks` (the missing indices are reported) |
| `DIGEST_MISMATCH` | lengths or SHA-256 of the reassembled payload don't match |
| `SCHEMA_TOO_NEW` | schema MAJOR greater than the reader's; the JSON MAY still be returned for display |

- If several NCX properties exist, the one on the root product wins and a warning is reported.
- `NO_PAYLOAD`, a failed check or a stale fingerprint (§6) all mean the same to the user: **import
  the plain geometry and say why.**

NCX is a transport format. A file that a CAD program has re-saved, or that a user has modified, is
plain STEP from then on: nothing in the format tries to survive that. The stale check (§6) makes it
import as plain geometry.

### 2.6 Notice body

A reader without an NCX add-in sees only geometry. To tell that reader what the file is, the writer
adds a **notice body**: a plain solid plate beside the model whose name is a short message. The message
points at the NCX project, not at any one vendor's add-in. The user can delete the body and keep the
plain geometry.

- **Shape:** a plate in front of the model (below its lowest y, from its lowest x and z), half the
  model's width long. The message is raised on its top face in a 5×7 pixel font, word-wrapped so the
  text block is about five times wider than tall, and the plate is sized to fit it.
  - **One solid:** plate and letters are a single closed polyhedral `MANIFOLD_SOLID_BREP`, with planar
    faces and straight edges only. Diagonal-only pixel contacts are bridged, so the boundary is
    2-manifold.
  - **Why the lettering:** some importers replace body names (SolidWorks 2018 with 3D Interconnect
    on), so the words must also be in the geometry.
- **Part files:** the notice is another solid in the root part's B-rep representation, so it never
  merges with the model.
- **Assembly files:** the notice is a new part component of the root assembly at the origin; its
  geometry is already in the assembly frame. It is named with the message, with path characters
  replaced by `_`. Its records go first in the DATA section, so importers that list components in file
  order show it at the top of the tree.
- **Naming:** the body's name is the message. The header's `notice` key repeats it in header form
  (`;` `'` `\` and non-printing characters as `_`), so a reader can identify the body without relying on
  the wording.
- **Readers that lose names:** some STEP readers drop solid names (Fusion names every body `Body1`, `Body2`...).
  Such a reader finds the notice by `noticebox`: the solid whose bounding box matches it within 0.01 mm. In files
  without `noticebox` (core 0.1.0), the notice is the solid that matches no payload body.
- **Matching names:** compare in header form, with path characters `/\:*?"<>|` also as `_` (as an
  assembly's component name has them) and spaces dropped. SolidWorks 2018 loses the space after a
  quoted `'` when it reads a STEP name.
- **Not part of the model:** the notice is never in the payload, so it is not in any fingerprint (§6).
  An NCX importer deletes it from anything it imports as geometry (the reference used for the stale
  check, face matching, plain-geometry fallback). Some importers shorten long names, so a body whose
  name is the start of the header's name, at least 7 characters long, also counts.
- **Rewriting:** writing a payload again replaces an earlier notice together with all its records,
  never doubling it. The earlier notice is the body or component named by the earlier header's `notice`
  (or, for files written before that existed, one whose name starts with `READ ME`).
- **Default text:** `READ ME - NCX file: Search 'Neutral CAD eXchange' on Github to import.` It points at
  the project, not at any vendor's add-in.
- **Importing an assembly:** an NCX importer leaves the notice component out (it is not in the payload)
  and deletes it from the plain-geometry reference.

`ncx_payload_write` adds the notice by default. Its options take `"notice": false` (none) or
`"notice": "<text>"` (the message).

## 3. Conformance

`conformance/vectors/vectors.json` lists STEP files with their expected read status and, for
`OK`, the exact payload bytes.
- The fixtures are real SolidWorks 2018/2026 AP214 exports (part and assembly) and an
  OpenCASCADE AP242 export.
- A conforming reader MUST return the expected status for every vector.
- The C++ core (`ncx_core`) and the independent Python reader (`python/ncx/specread.py`) both
  pass.
- Writers: `validator/ncx_validate.py` checks a whole file against this specification (container, schema,
  the model's consistency, and its geometry against the payload). A conforming writer's files pass it.

## 4. Payload model (schema 0.1)

The JSON Schema is `schema/ncx-0.1.schema.json`. A worked example in the user's modelling style is
`schema/examples/frame.ncx.json`, embedded in `frame.step`.

**Units:** lengths in **mm** and angles in **degrees**, always. Hosts convert at their edge
(SolidWorks: metres and radians; Fusion: centimetres and radians). Two exceptions, both outside feature
definitions: geometric descriptors (§5) give a cone's `semiAngle` in **radians** (the matcher compares them
directly), and spline knot values (`nurbs.knots`) have no unit. Global variables (`parameters`) are plain numbers;
where one drives a length or an angle, its value is taken in mm or degrees.

**Enumerated values** are words, listed in the table below. Files written by the SolidWorks add-in 0.4.0 and
earlier carry SolidWorks enumeration numbers instead; importers should accept both (`sw/Ncx.Sw/Neutral.cs` maps
each word to its number). Blocks named `native`, and a hole's `wizard` block, hold the source system's own data:
other systems may use them where they understand them, and otherwise ignore them.

| field (feature) | words |
|---|---|
| `references[].constraint` (plane) | `parallel` `perpendicular` `coincident` `distance` `angle` `tangent` `project` `midPlane` `parallelToScreen`; `options` may add `originOnCurve` `projectToNearest` `projectAlongSketchNormal` `referenceFlip`; `flip` is a separate flag |
| `axisType` (axis) | `line` `twoPlanes` `twoPoints` `cylindricalFace` `pointAndPlane` |
| `pointType` (point) | `alongCurve` `arcCenter` `faceCenter` `projection` `intersection` `sketchPoint`; `alongCurve` (its distance mode): `distance` `percentage` `evenlyDistributed` |
| `holeType` (hole) | `simple` `counterbore` `countersink` `tapped` `pipeTap` `slot` `counterboreSlot` `countersinkSlot` `legacy` |
| `end.type` (hole) | `blind` `throughAll` `throughNext` `upToVertex` `upToSurface` `offsetFromSurface` `midPlane` `upToBody` `throughAllBoth` `upToSelection` `upToNext` |
| `endCondition` / `diameterType` (cosmeticThread) | `blind` `upToNext` `throughFeature` / `conicalOffset` `major` `minor` |
| `hemType` / `position` (hem) | `open` `closed` `teardrop` `rolled` `double` / `inside` `outside` |
| `breakType` (breakCorner) | `fillet` `chamfer` |
| `cornerType` (closedCorner) | `butt` `overlap` `underlap` |
| `cornerType` (weldmentTrim) | `none` `miter` `butt1` `butt2` `endTrim` |
| `reliefType` (sheetMetal, edgeFlange, miterFlange) | `rectangular` `tear` `obround` `none` `tearBend`; `default` = the sheet metal's own relief |
| `bendAllowanceType` (sheetMetal) | `bendTable` `kFactor` `bendAllowance` `bendDeduction` `bendCalculationTable` `gaugeTable` |
| `position` (edgeFlange, miterFlange, sketchedBend) | `materialInside` `materialOutside` `bendOutside` `bendCenterline` `bendSharp` `bendTangent` |
| `length.datum` (edgeFlange) | `outerVirtualSharp` `innerVirtualSharp` `bendTangent`; `length.type` as `positionOffset.type` |
| `transition` (variable fillet) | `smooth` `straight` |
| `positionOffset.type` (edgeFlange) | `blind` `upToVertex` `upToSurface` `offsetFromSurface` `midPlane` `upToEdgeAndMerge` |
| `bendPosition` / `dimensionPosition` (jog) | `bendCenterline` `materialInside` `materialOutside` `bendOutside` / `insideOffset` `outsideOffset` `overall` |
| `profileType` / `thicknessType` / `location` (gusset) | `polygon` `triangle` / `inner` `bothSides` `outer` / `start` `center` `end` |
| `extrusion` / `ribType` (rib) | `parallelToSketch` `normalToSketch` / `linear` `natural` |
| `cornerTreatment` / `connectedSegments` (structuralMember) | `none` `miter` `butt1` `butt2` / `simpleCut` `copedCut` |
| `params.constraint` (width mate) | `centered` `free` `distance` `percent` |
| `params.constraint` (slot mate) | `free` `centered` `distance` `percent` |

**Top level:**
- `format: "ncx"`, `schema`, `generator`, `units`;
- `orientation` (optional): `{up}`, the axis of the model frame the source system shows as up (`x`, `y` or `z`,
  optionally signed: `-z`). Informative: an importer places the model under one transform so that this axis is up
  in its own convention (turning only the view leaves its standard views and home view showing the model on its
  side), but the model's coordinates, references and checks stay in the file's
  frame, and an exporter writing the model back keeps that frame. SolidWorks is Y-up (its Top plane is XZ); Fusion
  and Onshape are usually Z-up. Without it, assume `y`;
- `root` (a document id);
- `documents`;
- `fingerprint`.

**Documents:** one per source file.
- `kind` is `part` or `assembly`.
- Each document has `name` and `sourceFile`, `configurations` / `activeConfiguration`,
  `parameters`, `equations`, an optional `designTable`, ordered `features`, `bodies`
  (with per-body fingerprints) and `properties`.
- Assemblies add `components` (document, configuration, row-major 4×4 `transform` in mm, `fixed`)
  and `mates`.
- A component that places a sub-assembly may say `flexible: true`: the sub-assembly is solved as flexible in this
  assembly, so its own mates move with this assembly's motion. Without it the sub-assembly is rigid here: its parts
  keep their places relative to each other and move as one piece. Either way the sub-assembly's own mates define
  where its parts are: an importer keeps them (as joints, constraints or its system's equivalent) and, for a rigid
  sub-assembly, also holds its parts to it (a rigid group, or each part grounded to its parent). In a flexible
  sub-assembly its `fixed` parts stay fixed to it, and a parent mate that names a part inside it (by its `path`)
  acts on that part, not on the sub-assembly as a whole.
- Every document reachable from `root` MUST be in the same payload. The layout part, its derived
  parts and the assembly travel together: one file.

**Features:** `{id, type, name, native{app,type}, suppressed{config: bool}, status, note?, reason?, def}`.
- `status` is `mapped`, `approximate` (with `note`) or `opaque` (with `reason`).
- An unmappable feature is kept as `opaque`, so the history and the report stay complete.
- `noGeometry: true` on an `opaque` feature: it adds nothing to the model's bodies (another add-in's reference data,
  such as a 3D scan the part is modelled over). An importer skips it and reports a note; it has no `result`, and
  features after it do not depend on it geometrically.
- Type-specific definitions in schema 0.1: `sketch`, `insertPart`, `extrude`/`cut`, `fillet`/`chamfer`.
  The other known types are `plane axis point coordinateSystem revolve revolveCut hole
  linearPattern circularPattern sketchPattern mirror shell draft sweep loft combine split
  deleteBody moveBody thicken sheetMetal baseFlange edgeFlange flatPattern weldment structuralMember
  sweepCut loftCut importedBody mirrorBody weldmentTrim endCap loftedBend extrudedSurface
  revolvedSurface rib hem breakCorner sketchedBend jog closedCorner miterFlange mirrorPart cosmeticThread gusset
  dome planarSurface knit wrap unfold fold sketch3d`.
  Their `def` is free-form in 0.1 and gets fixed per type as Stage 3 maps them (see "Feature
  definitions" below for the ones whose meaning is not obvious from their names).
- Sheet metal: `sheetMetal` carries the part's sheet-metal defaults (thickness, bend radius,
  K-factor, relief); `baseFlange` and `edgeFlange` are the flanges; `flatPattern` is a marker for
  the flat pattern a base flange creates.
- `convertToSheetMetal`: a sheet of `thickness` wrapped over faces of the solid before it, starting from
  `fixedFace` (a face of that solid) and bent with inner radius `bendRadius` at each of `bendEdges` (edges of that
  solid, as descriptors in the state before the feature). By default (`reverse` false) the sheet lies inside the
  solid, its outer faces on the solid's faces; `reverse` puts it outside. `keepBody` keeps the solid as well. The
  part's `sheetMetal` defaults (K-factor, relief) follow it. Exporters attach the feature's `result`; an importer
  builds it natively (SolidWorks: Convert to Sheet Metal; elsewhere: the equivalent sheet with its bends, or the
  sheet-metal model made from the result) and uses the result to check its rebuild and the thickness sense.

**Feature definitions** (all lengths mm, angles deg, directions in the model frame). A definition, or a field, not
described here is as the SolidWorks exporter writes it: `sw/Ncx.Sw/Export*.cs` is the reference for it. An importer
that meets a field it does not understand reports the feature `approximate` rather than guessing.
- `plane`: `references` (up to three, each `{ref, constraint, options?, value, flip}`; `value` is mm, or degrees
  for `angle`), and the resulting `origin` and `normal`, so a system can also build the plane directly.
  `references` is empty when they could not be carried (a plane placed on another add-in's reference data, such as
  a 3D scan): the plane is built at `origin`/`normal` and reported `approximate`. A sketch whose `plane` is
  `{kind: "frame"}` is placed the same way, on a plane at its frame.
- `axis`: `axisType`, `references`, and the resulting `line` `{start, end}`.
- `point`: `pointType`, `references`, `alongCurve` and `distance` (mm, or a fraction for `percentage`), and the
  resulting `point`.
- `hole`: `face` (the face it starts on), `points` (the hole centres, model frame), `holeType`, `end`
  `{type, depth}`, `sizes` (mm and deg: `holeDiameter`, `holeDepth`, `counterBoreDiameter`, `counterBoreDepth`,
  `counterSinkDiameter`, `counterSinkAngle`, `tapDrillDiameter`, `threadDepth`, `drillAngle`, and
  `measuredDiameters`, the distinct diameters of the hole's cylindrical faces, ascending; a size the source's tables
  supply and its API reports as 0 is filled from them), `positionSketch` (the sketch whose points place the holes,
  with its relations and dimensions), and `wizard` (the source's own hole-table data).
- `fillet`: `filletType` `constant` (`radius`, `edges`, `radii` (one per edge) when they differ, `propagate`),
  `face` (`faces1`, `faces2`, `radius`), `fullRound` (`faces1` one side, `faces2` the centre, `faces3` the other
  side) or `variable` (below).
- `chamfer`: `chamferType` `equalDistance` (`distance`), `distanceDistance` (`distance`, `distance2`) or
  `angleDistance` (`distance`, `angle`), each with `edges` and `flipped` (the edges whose two distances are
  swapped); or `vertex` (`vertex`, `distances`, and `legs`: one `{direction, distance}` per edge at the vertex, the
  direction a unit vector from the vertex along that edge).
- `extrude` / `cut` with selected contours: `contours`, a list of contours, each the ids of the profile sketch's
  segments around it. Without `contours` (and `regions`) the whole sketch is the profile.
  - **`regions`**: areas of the sketch picked by clicking inside them (bounded by parts of crossing segments, or
    with islands): each `{point, area, clearance}`, a point well inside the region in sketch coordinates (mm),
    the region's area (mm²) and the point's distance from the region's boundary (mm). An importer selects the
    region of its rebuilt sketch that contains the point, and checks that its area matches (relative 1e-3).
    `contours` and `regions` may both be present; the profile is all of them. Revolves use them the same way.
  - Several **`contours`** combine by the area each encloses: other curves of the sketch crossing a contour do not
    split it; a contour wholly inside another listed contour is a hole in it (an island inside that hole is material
    again); contours that cross are united.
  - A region is bounded by the sketch's curves and, for a sketch on a face, by that face's edges lying in the sketch
    plane. A region may carry its **`boundary`**: per loop of the region, in loop order, the ids of the sketch
    segments it runs along (each once for consecutive edges along one segment, so a segment that runs into the
    region and stops - a slit, bounding it on both sides - is listed once), and for a boundary piece on no sketch
    segment (an edge of the face) its curve in sketch coordinates, `{line: [start, end]}` with its ends in loop
    order or `{arc: [centre, radius]}` (mm). A piece lying on a sketch segment is always written as that segment's id.
    An importer that can build the region from its boundary does so (the face's edges projected or drawn into the
    sketch), then checks the point and area.
  - `start` (optional; absent: the sketch plane): `{type: "offset", distance, reverse}` starts the feature
    `distance` mm along the sketch plane's normal (`frame.z`), or against it with `reverse`, whichever way the
    feature itself runs; `{type: "surface" | "vertex", target}` starts it at that face or vertex.
  - One sketch used by several features (each with its own contours or regions) is referenced by every one of
    them: SolidWorks lists it under each as `Sketch1<2>`, `Sketch1<3>`...; the payload names the sketch once.
- `cut` in a sheet-metal body: `normalCut` (true: the cut runs normal to the sheet's faces wherever it crosses
  the sheet, bends included, as a hole punched or lasered in the flat; false or absent: straight along the sketch
  normal). `cut` and `extrude` in a sheet-metal body: `linkToThickness` (true: the depth is the sheet's thickness
  and follows it). A straight cut through
  a bend removes slightly different material (measured: 0.33 mm³ on four 2 mm holes across a 3 mm bend), below the
  mass tolerance, so an importer cannot tell its own straight cut from the normal one by mass; on a flat face the two
  are the same. Where a system has no normal cut, it reports the cut `approximate` when the cut crosses a bend.
- `rib`: `profile` (an open sketch), `twoSided`, `thickness`, `reverseThickness` (the side of a
  one-sided rib), `flipMaterial` (which way from the sketch it fills), `extrusion` (0 parallel to the
  sketch plane, 1 normal to it), optional `draft {angle, outward, fromWall}`.
- `moveBody`: `bodies`, `copy`, `copies`, and either `translate [dx, dy, dz]` or `rotate {origin,
  angles [about x, y, z]}`. With more than one angle the order is not defined: the feature is
  `approximate` and checked by geometry.
- `hem`: `edges`, `hemType` (open, closed, teardrop, rolled), `position`, `reverse`, `length`, `gap`,
  `angle`, `radius`, `miterGap`.
- `breakCorner`: `entities` (corner edges or vertices), `breakType` (chamfer, fillet), `distance`.
- `sketchedBend`: `line` (the bend-line sketch), `fixedFace` (a face descriptor), `angle`,
  `reverse`, `position`, `useDefaultRadius`, `radius`, and `bendGeometry` (below). `position` is where the bend
  lies relative to the line. `materialInside`: the line, projected through the sheet, is the bend's outer virtual
  sharp (where the outer faces of the fixed side and of the bent side, extended, meet). The other words are
  SolidWorks' placements; for them, place the bend so it matches `bendGeometry`.
- **`bendGeometry`** (edgeFlange, miterFlange, sketchedBend, hem, jog): each bend the feature made, measured just
  after it: `axis` (`point`, at the middle of the bend along the axis, and unit `direction`), `innerRadius` and
  `outerRadius`. It fixes where the bend is whatever the source system's position words mean: a system whose own
  bend placements differ builds its bend so that its bend faces are these cylinders, and checks that they are.
- `miterFlange`: `profile` (a sketch on a plane normal to the first edge, at its end), `edges` (the
  chain it runs along), `useDefaultRadius`, `bendRadius`, `position`, `trimSideBends`, `gap`,
  `startOffset`, `endOffset`, and relief (`useDefaultRelief`, `reliefType`, `useReliefRatio`,
  `reliefRatio`, `reliefWidth`, `reliefDepth`).
  - `gap`: at a corner between two of its flanges, each flange ends **square** (its end face normal to its own
    edge, not cut on the mitre plane), and the two ends sit symmetrically so that the flanges' inner end edges are
    `gap` apart. P35 (90° corner, gap 1, thickness 2): each end face lies 0.707 (gap/√2) beyond the other flange's
    inner face. A system whose corners are mitred builds the nearest it can and checks against the `result`.
- `edgeFlange`: `edge` (the edge it is built on), `profile` (the flange's outline, a sketch on a plane through
  the edge at `angle` to the face), `angle`, `useDefaultRadius`, `bendRadius`, `position`, relief (as for
  `miterFlange`), `positionOffset` (`{type, distance, reverse}`, a shift of the flange along the face), `length`
  and `bendGeometry`. `positionOffset`: the flange is placed as `position` says for the edge, then moved `distance`
  outward along the face, and the sheet is extended along the face to meet its bend (P43, materialInside, offset
  5 from an edge at x 80: the flange's outer face is at 85, the bend axis at 81, the base sheet now runs to 81). A
  system without an offset builds the same thing as an extension of the face plus a flange. Along several edges in one feature it adds `edges` and `profiles` (one profile sketch per
  edge, in the same order) to the single `edge` and `profile` (the first of each).
  - `length`: `{type, distance, datum, reverse}`: how far the flange runs (`type` as `positionOffset.type`), and
    the line it is measured from: `outerVirtualSharp` (where the outer faces of the sheet and of the flange,
    extended, meet), `innerVirtualSharp` (the same for the inner faces) or `bendTangent` (the line where the bend
    ends and the flange's flat part begins, on the outer face). The profile sketch shows the flange at this length.
  - `position`: where the flange sits relative to the edge. `materialInside`: the flange's outer face is flush
    with the edge (the part's outer size is kept); `materialOutside`: the flange's inner face is flush with the
    edge (the outer size grows by the thickness); `bendOutside`: the bend starts at the edge. Check the result
    against `bendGeometry`.
- `closedCorner`: `faces` (the flange end faces to extend), `cornerType` (butt, overlap, underlap),
  `gap`, `overlapRatio`, `openBendRegion`. With t the thickness, "the listed flange" the one whose end face is in
  `faces` and "the other flange" the one it meets (measured on P34, gap 0.2, t 2):
  - `overlap`: the listed flange is extended until its end is `gap` short of the other flange's inner face; the
    other flange runs across the listed flange's end for `overlapRatio` × t, measured from the listed flange's
    inner face (ratio 1: across its whole thickness; 0.5: half way);
  - `underlap`: the reverse: the other flange stops `gap` short of the listed flange's inner face, and the listed
    flange runs across its end for `overlapRatio` × t;
  - `butt`: both flanges end square, symmetrically, with their inner end edges `gap` apart (as `miterFlange.gap`).
  The bend region at the corner (`openBendRegion: false`) is closed in the source system's own way; the feature's
  `result` has its exact shape.
- `jog`: `line`, `fixedFace`, `angle`, `offset`, `reverse`, `dimensionPosition`, `bendPosition`,
  `fixProjectedLength`, `radius`, and `bendGeometry` (its two bends). A jog is discontinuous at 90°: importers
  should round the angle before use (a radian round trip gives 90.0000000000002). With t the thickness, the
  sheet beyond the line moves by `offset` + t for `dimensionPosition: insideOffset` (`offset` is the clear gap
  between the fixed and the moved sheet), by `offset` for `outsideOffset` (between like faces of the two) and by
  `offset` − t for `overall` (from the outer face of one to the far face of the other) (measured on P29).
  `fixProjectedLength: true`: the moved part keeps its extent (its far edge stays where it was; the jog adds
  material); `false`: the developed length is kept, so the far edge moves towards the line by what the jog uses.
  A system without a jog builds it as two bends placed on `bendGeometry`.

- `cosmeticThread`: `edge` (the circular edge it starts from), `diameter` (the minor diameter of an
  external thread, major of an internal one), `depth`, `endCondition`, `callout` (e.g. `M10x1.5`),
  `standard`, `size`. It changes no geometry; it is annotation an importer can show or drop.
- `structuralMember` (weldments): `profile` (`{standard, type, size}`, e.g. `iso` / `square tube` / `20 x 20 x 2`:
  an importer finds it in its own profile library, or builds the section), `connectedSegments` (`simpleCut` or
  `copedCut`: how members of one group meet where their segments join), `allowProtrusion`, and `groups`. Each group has
  `segments` (sketch-entity references, the member paths), `angle` (the profile's rotation about the path),
  `gapWithinGroup` and `gapForOtherGroups` (weld gaps, mm), `mergeArcSegmentBodies`, `miterMergeCondition`, and
  `cornerTreatment` for the corners where two of its segments meet:
  - `none`: each member ends where its segment ends (the members overlap at the corner);
  - `miter`: both members run to the outside of the corner and are cut on the plane bisecting it;
  - `butt1`: the member whose segment *ends* at the corner runs to the outside of the corner; the member whose
    segment *starts* there stops against it;
  - `butt2`: the reverse of `butt1`.

  Segment direction is the direction of travel along the group's segments in the order listed (each segment runs
  towards the point it shares with the next), not the sketch entity's own start to end. With `connectedSegments:
  copedCut`, the stopping member is cut to the other member's whole outer section. Treatments apply only between
  members of one group. Per-
  corner overrides (a different treatment, trim or order at one corner) are not in schema 0.1: an exporter carries
  such a member's `result` and reports it `approximate`. Files written by the SolidWorks add-in 0.4.0 and earlier
  carry `cornerTreatment` and `connectedSegments` as SolidWorks enumeration numbers (1 miter, 2 butt1, 3 butt2;
  1 simpleCut, 2 copedCut); importers should accept both.
- `gusset` (weldments): `faces` (the two supporting faces, which meet at an edge), `profileType`
  (triangular, polygonal), `d1`..`d4`, `angle`, `useAngle`, `thicknessType` (both sides, one side,
  the other), `thickness`, `location` (end or mid point), `offset`, `offsetDistance`.
- `variable` fillets (`filletType: "variable"`): `edges`, `vertexRadii` (`{vertex, radius}`), `radius`
  (the default, may be 0), and `transition`, how the radius changes between vertices: `straight` (in proportion
  to the distance along the edge) or `smooth`: between two vertex radii r0 and r1,
  r(s) = r0 + (r1 − r0)(3s² − 2s³), s the fraction of the edge's length from the r0 vertex (it leaves and reaches
  each radius with zero slope). Measured on P30: a Fusion fillet with intermediate radii placed on this law (9 per
  edge) matches the source at native tolerance. A system without a variable law builds it from intermediate radii
  on this curve; the feature's `result` (always carried for variable fillets) has the exact faces.
- `loft` / `loftCut`: `sections` (the section sketches, in order), `guides`, `centerline`, `closed`, the start
  and end tangency (`startTangency`, `endTangency`, `startTangentLength`, `endTangentLength`,
  `reverseStartTangent`, `reverseEndTangent`), `maintainTangency`, `merge`, and `connectors`.
  - `connectors`: the loft's alignment, measured on the loft: each edge of the loft that runs from one section to
    another, as `{from, to, points}`: the two sections' indices and the points it joins (model frame, mm). A
    system lines its sections up so that these points join. Where the sections have different numbers of
    segments (a rectangle to a circle), systems' default alignments differ, so such a loft also carries its
    `result`.
- `sketch3d`: a 3D sketch, defined as for `sketch` with these differences:
  - It has `space: "3d"` and no `plane` or `frame`.
  - Every coordinate is `[x, y, z]` in the model frame (mm), and dimension text positions are too.
  - An arc adds `normal` (its plane), and `ccw` is about that normal.
  - Its relations add `alongX`, `alongY` and `alongZ` (a line along a model axis).

  Weldment members and sweeps refer to its entities as `sketchEntity`, as for 2D sketches.
- `dome`: `faces`, `height`, `reverse`, `elliptical`.
- `split`: `tools` (planes, surfaces or sketches) and `consume`. Every body the tools cut out is kept.
- `planarSurface`: `profile`, a closed sketch.
- `knit`: `surfaces` (bodies or faces), `formSolid`, `mergeEntities`, `gapControl`, `tolerance`, `maxGap`.
- `wrap`: `sketch`, `face`, `wrapType` (`emboss`, `deboss`, `scribe`), `thickness`, `reverse`.
- `unfold` / `fold` (sheet metal): `fixedFace`, and `bends` as `{feature, sub, name, face}`. `face` is a face
  descriptor of the bend in the state the feature saw: its inner cylindrical face when bent (before an unfold), or
  the flat strip's larger face when flat (before a fold). `feature` (the feature the bend belongs to), `sub` (the
  bend's index among that feature's sub-features) and `name` are the source's own; a system without bend
  sub-features finds the bend by `face`.
- `mirrorPart`: the document is the mirror image of another part: `source` (a document id), `plane` (one
  of the source's origin planes, `{kind: "originPlane", plane}`), `transfer` (as for `insertPart`). It is
  the first feature of its document; an importer creates the document by mirroring the source.

**Assembly features** (`features` of an assembly document):
- `componentPattern`: `def.kind` is `linear` (`direction {ref, count, spacing, reverse}`,
  optional `direction2`) or `circular` (`axis`, `count`, `angle` between neighbours, `reverse`).
  `seeds` are component ids, `skipped` lists skipped instance indices. A direction or axis is an
  assembly reference (as for mates: `{component, path?, ref}` or the assembly's own `{ref}`).
- The components a pattern makes are listed like any other, with `pattern: <feature id>`. An
  importer that rebuilds the pattern does not add them itself; it matches what the pattern makes to
  them by transform. Which way an edge or axis runs can differ between systems, so a pattern whose
  instances land elsewhere is rebuilt with the other direction(s). An importer that cannot pattern
  components places them as listed.
- `mirrorComponents`: `def.plane` (an assembly reference), `instances` (`{seed, orientation}`: copies of
  the same part, mirrored again at import and matched by transform; they carry `pattern: <feature id>`
  like pattern instances) and `oppositeHand` (seed component ids whose copies are mirrored parts: those
  copies are plain components of their own `mirrorPart` documents, placed as listed, and the feature is
  `approximate`).
- `plane`: a reference plane of the assembly, as for parts (`references` with `constraint`, `value`,
  `flip`, and the resulting `origin` and `normal`), but each reference is an assembly reference. Mates,
  patterns and other planes refer to it as `{ref: {kind: "feature", id}}` (no `component`).
- `sketch` and `cut` (assembly level): written as in parts. Their plane and profile references are the
  assembly's own (`originPlane`, or `{kind: "feature", id}` for its planes and sketches). A cut's
  `def.components` lists the components it changes, and `autoSelect` says whether the source let it
  cut everything it passes through.
  - The file's B-rep holds a part as the assembly shows it, so parts an assembly cut changes skip the
    part-level face check, with the reason given. The assembly's own fingerprint checks the cut.
  - An importer that cannot cut exactly those components reports the cut. It must not make a cut that
    changes other components.
- Any other assembly-level feature is kept as `opaque` with a `reason`.

**Feature results** (`result`, optional on any part feature): the feature's effect as geometry, for an
importer that cannot rebuild it. This applies when the exporter could not map the feature, when the
importer's CAD system has no such feature (a hem, a jog or a gusset in a system without them), or when its
replay fails. Instead of giving up on the whole part, the importer swaps in these bodies and carries on
with the rest of the history.
- `replaces`: the fingerprints (§6) of the bodies, just before the feature, that it changed or removed.
- `bodies`: `{name, sheet, fingerprint}` of the bodies just after it that it changed or made, in the
  order of `data`.
- `data`: the text of an ISO 10303-21 file (AP214, mm) holding exactly those bodies. It is empty if the
  feature only removed bodies.
- `format: "step"`.
- **Importing:**
  1. For each `replaces` fingerprint, find the nearest body in the part as rebuilt so far (volume,
     area, centroid). If none is close, earlier steps differ: the result can't be used.
  2. Delete those bodies.
  3. Import `data` with the system's own STEP reader and add its bodies.
  4. Give the new bodies their `name`s, matched by fingerprint.

  Later features then resolve their references (§5) on the new bodies as usual.
- **What the importer gets:** the geometry exactly as it was, but fixed: it does not follow
  parameter changes made before the feature.
  - The importer reports the feature as `approximate`, giving the reason it was not rebuilt.
  - The part is verified at exchange tolerance, because the bodies went through STEP.
  - Parameter-flex probes that move such a part are skipped, with a reason.
- **What exporters attach it to:** to every `opaque` feature that changes geometry (where producing it has a real
  cost, such as a quota-limited STEP export of an intermediate state, an exporter may leave it out: the validator
  warns, `M-RESULT-MISSING`, and an importer that cannot rebuild the feature falls back to plain geometry), and by
  default to the feature types few systems share. The SolidWorks exporter uses: the sheet-metal
  extras (`hem`, `jog`, `miterFlange`, `closedCorner`, `breakCorner`, `sketchedBend`, `loftedBend`,
  `edgeFlange`), `gusset`, `endCap`, `weldmentTrim`, `rib`, variable fillets, dome, wrap and
  `mirrorPart`.
  - An importer that can rebuild the feature ignores `result`, but keeps it as the fallback if the
    rebuild fails.
  - Features that change no body (sketches, reference geometry, cosmetic threads) never carry one.
- **Something simpler, where a counterpart is missing.** Every importer has a STEP reader, so a carried
  result works in any CAD system. An importer still prefers a near-equivalent feature where one
  exists. It reports the substitution as `approximate` with a reason, and checks it by fingerprint
  like any rebuild; if the fingerprint does not match, it falls back to the result. Suggested
  equivalents:
  - a `hem` as an edge flange plus a second flange;
  - a `gusset` as a sketched extrude on the plane through its two faces' normals;
  - a weldment `endCap` as an extruded plate on the member end face;
  - a dome as a revolve or loft;
  - a mechanical mate with no counterpart as the components placed where the file lists them
    (they arrive in position).

**Sketch:**
- `plane` (a reference) plus an explicit `frame` (origin, x, z in model mm).
- Where the plane cannot be referenced, `plane` is `{kind: "frame"}` (the frame alone places the
  sketch) or `{kind: "generated", by: <feature name>}`: a plane the named feature creates (an edge
  flange's profile plane), and that feature replays the sketch.
- `entities` carry **solved** 2-D coordinates, which act as seeds so under-constrained sketches
  solve to the same configuration.
- A `spline` carries its fit `points` (the editable intent) and `nurbs`, the exact curve: `degree`, `knots` (the
  full knot vector), `controlPoints` (sketch coordinates, mm) and, for a rational spline, `weights`. A system whose
  spline through the same fit points comes out differently builds the curve from `nurbs`. Fit points are the writer's
  own fit: an importer builds another system's spline from `nurbs` (a spline with fewer than three `points`, such as
  a fixed spline whose only points are its ends, always).
- `constraints` use a closed list of types; others keep their native name with `supported: false`.
- `hidden` (optional, default false): the sketch is hidden in the source; an importer hides it too.
- **`converted`** (Convert Entities / Project / Use): `entities: [own, source]`, the sketch's own segment and the
  entity it was made from: another sketch's segment (often an inserted layout sketch's, see §5) or a model edge.
  The own segment follows its source. An importer makes a real link where its system has one (SolidWorks' Convert
  Entities, Fusion's Project, Onshape's Use); otherwise it keeps the segment on its source with relations that
  have the same effect: collinear (a line) or coradial (an arc or circle) with the source, and each end point
  coincident with the source's nearest end. A flex probe on the source's sketch shows the link works.
- **`offset`** (Offset Entities): `entities: [a, b]`, an offset segment and its source in the same sketch. The two
  stay concentric (arcs, circles) or parallel (lines); the offset distance is one of the sketch's `dimensions`.
- **`patterned`** (a sketch pattern's copies): `entities`, two or more segments of the same sketch that are copies of
  one another; they stay equal when the seed changes. Copies of a linear pattern (lines parallel in their seeded
  positions) stay parallel too; copies turned by a circular pattern, such as a polygon's sides, are only equal.
- A **sketch entity of another document** (`{kind: "sketchEntity", by: "id", document, feature, id}`) is an
  entity of a sketch an `insertPart` brought in: `feature` and `id` name the entity in the source document. The
  derived part's copy of the sketch has the same geometry, so an importer finds the entity there by its
  coordinates (the source entity's coordinates through the source sketch's frame). An importer that rebuilds the
  inserted sketch itself (a copy keyed by the source sketch and its entity ids) finds the entity there by its `id`,
  and by coordinates only where its copy has other ids: two points may share a position (see below). This is how a derived part's
  sketches relate to, dimension from and convert entities of the layout sketch they are drawn on.
  The referenced entity may lie off the sketch's plane (a layout point on the mid plane, the sketch on a sheet's
  face): the relation then holds its projection onto the sketch plane, as a point projected along the sketch's
  normal (Fusion: project it into the sketch; Onshape: use it).
- Two points of one sketch at the same position are two entities when the payload lists them so (a standalone
  point on a line's end, tied by a `coincident` relation): an importer keeps them apart and must not merge points
  by position. Shapes some systems draw as one tool (slots, polygons) arrive as their plain segments and the
  relations that hold them (equal caps, sides tangent to them, corners on the polygon's circle).
- `dimensions` keep the native full name (`D1@SK_Outline`), so spreadsheet-driven dimension
  writes keep working, and a `param` link where a parameter drives them.
- A `distance` between two circles or arcs is between their centres, except between two concentric ones, where
  it is the gap between them (the difference of their radii): a ring's width, a slot's offset from its pivot.
- Sketch names are kept verbatim.

**Equations:** `{target, expression}` in a small neutral grammar (`+ - * / ^ ( )`,
`sin cos tan sqrt abs min max if`, names). Anything else is kept in `native` with
`status: approximate`.
- Names in expressions are in double quotes (`"W" * 0.5`): a parameter's name, or a dimension's full name.
- `target` is the source's name for what the equation drives (`D1@SK_Base`, or `D1@Boss-Base` for a feature's own
  dimension, numbered the source's way). `targetRef` says the same in neutral form: `{kind: "dimension", feature,
  id}` for a sketch dimension, or `{kind: "featureField", feature, field}` for a field of a feature's definition
  (`end.depth`). An exporter leaves `targetRef` out where it cannot tell which field it is.
- Parameters (global variables) carry `value` and, as the user wrote it, `expression`.

**Inserted / derived parts:** `insertPart` with `source` (a document id), `sourceConfiguration`,
`transfer` (what carries across), `transform`, and `items` (source item → derived name, e.g.
`SK_Outline-Frame-Layout`; an item's `hidden` says the derived part hides that transferred sketch, and an importer
hides the copy too). Features of the derived part refer to a transferred item as
`{kind: "transferred", by: "id", document, feature | plane, name, stock}`: `document` and `feature` (or an origin
`plane`) identify the source item, `name` is its name in the derived part, and `stock` names the `insertPart`
feature. A system without derived parts can rebuild the source's sketches in the part, with their dimensions
linked to the source's parameters: the source then still drives the part, and the flex probes (§6.4) prove it.

**Directions and orientation** (as the SolidWorks exporter writes them; `frame` is the sketch's frame):
- A `reverse` on a direction or axis given by a geometric reference (a pattern's `dir1`/`dir2` edge, a circular
  pattern's `axis`) is against the `direction` or `axis` that reference's descriptor records, not against the
  importer's own idea of the entity's direction (systems orient the same edge either way).
- `extrude`: along the sketch normal (`frame.z`); `cut`: against it. `reverse` flips either. `end2` runs the
  other way.
- A one-direction thin wall lies to the right of an open profile's direction (start to end), looking down
  `frame.z`; `reverse` puts it on the left.
- `baseFlange` thickness: to the right of an open profile, as for a thin wall; against `frame.z` for a closed one.
- `revolve`: an unreversed revolve turns clockwise about its axis's direction as recorded (start to end), that is
  against the right-hand rule; `reverse` turns it the other way. `revolveCut` and `revolvedSurface` the same.
  `revolvedSurface` carries `axis` like `revolve` (the profile sketch's centreline). Files from the SolidWorks
  add-in before 0.5.2 lack it, and so does one whose profile sketch has several centrelines: the axis is then the
  sketch's only construction line, else undetermined (report the feature `approximate` and check by geometry).
- Patterns: along the direction reference's own direction (an edge descriptor's `params.direction`, an axis's
  `line` from start to end); `reverse` flips it. `skipped` instance indices count from 0, the seed being 0.
- Where a system's own convention differs, build by these rules and check by fingerprint; if that fails, try the
  other sense, and report it if neither matches.

**Custom properties** (`customProperties`, parts and assemblies): what a BOM is built from.
- Each property is `{name, type (text|number|yesno|date), value, resolved, configuration?, native?}`.
- `value` is what the user typed. A linked value keeps its expression, e.g.
  `"SW-Mass@@Default@P1.SLDPRT"`, whose file name means "this document".
- `resolved` is what it evaluated to, for importers that cannot evaluate the expression.
- Without `configuration`, a property is file-wide.

**Cut lists** (`cutLists`, weldment parts): `[{name, bodies, properties}]`.
- `bodies` are body references (§5).
- `properties` are as for custom properties; linked values name their own item
  (`"LENGTH@@@<item name>@<file>"`).
- An importer whose system groups members itself matches each group to an entry by its bodies. It gives the group
  the entry's name, so the links resolve, and sets the properties. A system without cut lists keeps them as
  per-body properties, or reports them.

**Materials** (`materials`, parts): `[{configuration, name, database}]`. An importer looks the name up in
its own material library. If it is missing, the part is still rebuilt, and the report says which
material was not found.

**Appearances** (`appearances`, parts and assemblies): what the user sees. Each is `{target, kind,
color, optical?, file?, mapping?}`.
- `target` is one of:
  - `{kind: "document"}`
  - a body reference (§5)
  - a feature reference
  - a face descriptor (§5, resolved in the finished model)
  - in an assembly, `{kind: "component", id}`
- `kind: "color"` is a plain colour: `color` is RGB 0–1, and `optical` holds ambient, diffuse,
  specular, shininess, transparency and emission.
- `kind: "appearance"` is a library appearance, usually textured:
  - `file` is its path inside the appearance library (`metal/aluminum/brushed aluminum.p2m`). An
    importer finds it in its own library by that path, else by file name.
  - `mapping` is type, width/height/x/y in mm, rotation in rad, and fixed aspect.
  - If the importer has no such appearance, it is **skipped**, with a note, never a failure.
  - The appearance's tint comes from the importer's library copy (SolidWorks 2018 ignores a set
    colour on a textured appearance).
- **Not carried:**
  - a material's own appearance (it follows the material);
  - appearances in display states other than the active one;
  - appearances on nested assembly components.

**Structural rules** (beyond the JSON Schema; checked by `ncx_model_validate`):
- ids are unique (documents globally; features, components and sketch entities within their
  scope);
- every reference resolves (`root`, `insertPart.source` must be a part, `component.document`,
  sketch entity ids in constraints and dimensions);
- there are no cycles in the document dependency graph. Writers list documents in dependency order (a document after
  every document it names: an `insertPart` source, a component's document); importers order them themselves all
  the same;
- `opaque` features need a `reason`;
- units are mm and deg.

**Versioning:** `MAJOR.MINOR`. A reader refuses a newer MAJOR (`SCHEMA_TOO_NEW`), accepts a newer
MINOR, and ignores unknown fields and feature types, reporting them as warnings.

## 5. References

**Rule 1, by id:** anything replay creates itself is referenced by id. That covers origin planes,
the origin, features, sketch entities and transferred items, for example
`{kind:"sketchEntity", by:"id", feature, id}`. Ids are stable because replay records the native
handle it creates for each id.

**Origin planes** are named by geometry, in the model's own frame (the names happen to be SolidWorks'):

| `plane` | plane | normal |
|---|---|---|
| `front` | XY | +Z |
| `top` | XZ | +Y |
| `right` | YZ | +X |

An importer maps each name to its own plane with that geometry, never to its own plane of the same name (Onshape's
*Front* is XZ). A sketch's `frame` is authoritative for the sketch's axes; the plane only says which plane it lies on.

**Rule 2, by geometry:** B-rep topology (faces, edges, vertices picked by fillets, chamfers,
shells, drafts, face-based sketches and mates) is referenced by a **descriptor**. It is recorded
in the state the referencing feature saw, which means rolled back to that feature:

```
{kind: "face"|"edge", by: "geom", geom: {
   type,                 plane cylinder cone sphere torus bspline ... | line circle ellipse bspline ...
   params,               analytic parameters: normal+point, axis+origin+radius, apex+axis+semiAngle, ...
   samples: [[x,y,z]],   5 interior points (9 for freeform faces); edges: at 0.1 0.3 0.5 0.7 0.9 of the range
   area | length,
   bbox: [xmin,ymin,zmin,xmax,ymax,zmax],
   clearance, weak?,     the samples' distance from the entity boundary; weak if < 0.01 mm
   seam?, degenerate?    never referenceable; exporters MUST NOT emit references to them
}}
```

**Sample choice for faces:** take interior points from the face's triangulation, keep the most
interior one, then spread the others out. UV-grid sampling is not robust: it fails on thin rims
and odd parametrisations (measured).

**Resolution protocol** (`ncx_reference_match`):

1. **The host measures.** For each candidate entity of the target (same kind, prefiltered to
   those whose bounding box meets the reference's `bbox` enlarged by 1 mm), the host provides:
   - its type and analytic parameters;
   - its area or length;
   - its distance to every reference sample;
   - optionally, its own sample points and its neighbour ids (faces sharing an edge, edges
     sharing a vertex).
2. **The core decides.** It applies, in order:
   1. *Type compatibility:* analytic types must match exactly; freeform matches any type.
   2. *Backward check:* a candidate's own points must lie on the reference's analytic geometry
      **and** inside its `bbox`.
   3. *Single exact cover:* one candidate covers every sample.
   4. *Split:* the backward-checked pieces of one type, or else the union of the minimal sample
      covers of one type and parameter set.
   5. *Completion:* add connected, backward-checked pieces until the reference size is reached.
   6. *Size check.*
   7. *Retry:* only if nothing resolved, retry once at 10× tolerance and say so.
3. **Results:**

| result | meaning | host action |
|---|---|---|
| `resolved` | the ids are the target entity (or all pieces of it); `size_ratio` within 1 % (or larger, if the target merged it) | build the feature |
| `partial` | the pieces found are correct but short of the reference size | report; never build on part of an entity |
| `ambiguous` | more than one answer fits (alternatives listed) | report and fall back |
| `unresolved` | nothing fits (nearest miss given) | report and fall back |

Default tolerance: 0.001 mm, with the automatic retry at 0.01 mm.

**Vertices** are `{kind: "vertex", by: "geom", geom: {type: "point", params: {point}, samples,
bbox}}`. A point needs no scoring, so the host resolves it: the one vertex within the tolerance
(0.001 mm, then once at 0.01 mm); several coincident vertices are `ambiguous`.

**Bodies** (multibody features such as `combine`) are referenced as
`{kind: "body", by: "geom", geom: {volume, centroid, bbox}}`, measured in the state the feature
saw. A body resolves when exactly one solid body has a volume within 1e-5 relative and a centroid
within 0.01 mm.

**Mates** reference `{component, path?, ref}`:
- `component` is a component id of the assembly; `path` lists nested component ids below it,
  each in its parent's document (`Sub-1/Pin-1` → `component: "c:Sub-1", path: ["c:Pin-1"]`).
- `ref` is a face/edge descriptor in the component's own frame, an `originPlane`, or a
  `feature` reference `{kind: "feature", document, id}` into the component's document.
- Mated datum planes are identified geometrically against the component's plane features; origin
  planes win a tie.
- Without `component`, the reference is the assembly's own `originPlane` or `origin`. Layout-style
  assemblies mate every component's base planes to the assembly's planes this way.
- `suppressed` (optional) `{config: bool}`, as for features: a mate suppressed in the source holds nothing
  there. An assembly that lists no configurations uses the key `cfg:active` for its one (active) state. An importer makes it and suppresses it (or leaves it out and says so); the components stay at
  their `transform`.
- A mate whose source has an end with no geometry (its part deleted) is left out of the file by the
  exporter, with a note: it holds nothing.
- Mechanical mates:
  - `gear`: two refs, `params {ratio: [a, b], reverse}`.
  - `screw`: two refs, `params {pitch (mm per turn), reverse}`.
  - `width`: refs tagged `role: "width"` (the two outer faces) and `role: "tab"` (the two inner
    ones), `params {constraint (centred, free, distance, percent), distance, percent, flip}`.
  - `symmetric`: refs tagged `role: "entity"` (two) and `role: "plane"` (the symmetry plane),
    `params {alignment}`.
  - `slot`: two refs (the slot face, the entity riding in it), `params {constraint (free, centred,
    distance, percent), distance, percent, flip}`.
  - `hinge`: refs tagged `role: "concentric"` and `role: "coincident"`, optional angle limits
    `params {angle, min, max}`. Where a system has no hinge, it is built as its functional
    equivalent, a concentric plus a coincident mate, and reported `approximate`.
  - `rackPinion`: the rack's edge (or face) and the pinion's cylindrical face, `params {pitchDiameter,
    travelPerTurn (= π × pitchDiameter), byTravel (which of the two the user set), reverse}`.
  - `cam`: the cam's faces (a tangent chain) and the follower's face or point.
- **`frame`** (optional; SolidWorks add-in 0.5.2 on): `{origin, x, z}` in the assembly frame at the solved
  placement (mm), for systems whose mates join two coordinate frames (Onshape mate connectors, Fusion joints).
  - `z` is the direction of the mate's first axial entity (cylinder, cone, circle, line or axis), else the normal
    of its first plane.
  - `origin` is where that axis meets the mate's first plane not parallel to it (a hinge: the pin axis on the
    coincident face), else a point of the entity itself (any point on a concentric mate's axis).
  - `x` is the assembly's X made square to `z` (its Y when `z` is within about 25° of X); it carries no intent.
  - A connector at `frame`, expressed in each mated component's own frame (through its `transform`), puts both
    sides at the same frame in the solved placement, so the joint is satisfied there with no sense to choose.
    A concentric + coincident pair becomes one revolute at the concentric mate's axis through the coincident
    mate's origin. The refs stay the intent; a dimensioned mate's value still lies between its refs.
  SolidWorks 2018 cannot read a screw's pitch or a gear's direction).
- **Mates with no counterpart.** Every component arrives where the file places it (`transform`), so
  an importer that cannot make a mate loses only that mate's motion, never a position.
  - A mate that only couples motion (`gear`, `screw`, `rackPinion`, `cam`, linear coupler, path) is
    then reported `approximate`: "components placed, not coupled".
  - A positioning mate is replaced by its functional equivalent where one exists (a `hinge` as
    concentric plus coincident), else reported the same way.
  - Mate types the exporter does not map (linear coupler, path, universal joint in the SolidWorks
    exporter) are `opaque`; their components still arrive in place.

## 6. Fingerprint

**Per body:** `volume, area, centroid[3], moments[3]` (principal, sorted), `bbox[6]`, and
`faces` / `edges` (informational), all in mm. `freeform: true` when any face is not a plane, cylinder, cone,
sphere or torus (omitted otherwise).

**File fingerprint:** `{bodies: [...]}`. Fingerprints cover **solid** bodies only: surface (sheet) bodies are not
fingerprinted (a surface-only part has `bodies: []` and cannot be stale-checked), and a surface-only part file has
no notice body, because the notice goes into the part's solid B-rep representation.

**Header `fp`:** the first 16 hex digits of SHA-256 over the sorted lines
`V=%.5e;A=%.5e;C=%.2f,%.2f,%.2f\n`, one per body (centroid rounded to 0.01 mm, never `-0.00`).

**Comparison:** `ncx_fingerprint_compare_ex` pairs the bodies (by volume, then centroid) and
applies one of two tolerance profiles:

| quantity | `native` (default) | `exchange` |
|---|---|---|
| volume | 1e-5 relative | 2e-4 relative |
| area | 1e-5 relative | 2e-4 relative |
| centroid | 0.001 mm | 0.01 mm |
| moments | 1e-4 of the largest | 5e-4 of the largest |
| bbox | 0.5 mm (warning only) | 0.5 mm (warning only) |
| freeform body: volume, area | as above | 6e-4 relative |
| freeform body: centroid | as above | 0.05 mm |
| freeform body: moments | as above | 3e-3 of the largest |

- `native` is for a model compared with its rebuild in the same kernel (replay verification).
- `exchange` is for B-rep that went through STEP or another kernel. Freeform faces (lofts,
  sweeps) re-imported by SolidWorks 2018/2026 measured up to 3.4e-5 relative in volume, which
  fails `native`. A real edit, such as 0.1 mm on a 100 mm part (~1e-3), still fails `exchange`.
- **Freeform bodies.** A spline surface written to STEP and read by another kernel measures differently: a
  SolidWorks guide-curve sweep read by OpenCASCADE differed by 3.0e-4 in volume, 3.9e-4 in area, 0.033 mm in
  centroid and 1.7e-3 in moments (SolidWorks re-reading its own STEP: 3.4e-5). Under `exchange`, a pair of bodies
  where either is marked `freeform` (by the writer, or by the reader from its own measurement) gets the freeform
  tolerances. A 0.1 mm edit on a 100 mm part (1e-3 in volume) still fails. `native` is never loosened.
- Individual tolerances can be overridden in the options (`volume_rel`, `area_rel`, `centroid_abs`, `moment_rel`,
  `bbox_abs`, and the `freeform_` variants, which are never tighter than the analytic ones).

**Stale check at import:** the host fingerprints the B-rep it imported from the same file and
compares it with the payload's fingerprint, using `exchange`.

Face and edge counts may differ between kernels; they are reported as `count_only`, which is not
a failure. A `mismatch` means the payload describes different geometry: fall back to plain
geometry.

### 6.1 Verification at import

An importer verifies each part it rebuilt, and reports every check:

| check | compares | result |
|---|---|---|
| mass | per-body fingerprint of the rebuild vs the payload (`native`) | `match` / `count_only` / `mismatch` |
| structure (6.2) | the rebuilt tree vs the payload's features | `match` / `mismatch` |
| flex (6.4) | each probe applied to the rebuild vs the probe's fingerprint (`native`) | `match` / `mismatch` / `failed` |
| faces (6.3) | the rebuild vs the file's own B-rep, face by face | `match` / `topology` / `mismatch` |

A mass `mismatch` replaces the part by its reference B-rep (the other checks are then not run).
The others are reported, not acted on.

**When the target cannot measure exactly.** Two kernels measure the same body differently (measured: 9e-5 relative
in volume for one spline extrusion, SolidWorks against Onshape; 5.6e-5 for a fillet-and-torus nut with no spline at
all, SolidWorks against Fusion), which fails `native` even for a perfect rebuild. For any body, the mass check also
passes when the rebuild matches the importer's own measurement of the file's B-rep (the stale check's reference) at
`native` *and* the payload at `exchange`; the report says so (the part is `approximate`, not `exact`). Some
systems give exact volume and area but centroid and moments only from a mesh, or only after assigning a material that
would change the user's model. Volume and area must then still meet `native`. Centroid and moments may be compared at
`exchange` instead, and the report says so (for example `mass: match (centroid, moments at exchange: mesh)`).
A mesh of a curved body lies inside it and measures small: scale mesh moments to the exact volume,
`moments × (V / V_mesh)^(5/3)`, and name the correction in the report (measured on a radius-40 gear: 6.6e-4 low
uncorrected, 1.2e-4 corrected).

The comparisons here and in 6.3 and 6.4 are **per body**: bodies are paired as `ncx_fingerprint_compare_ex` pairs
them.

### 6.2 Structure

Every payload feature that is not opaque must exist in the rebuild under its name, and, when the
importer is the same application as the exporter, with the same native type.

Each sketch carries `constrained` (`full` / `under` / `over` / `invalid` / `unknown`, as the source
reported it). A sketch exported `full` must be fully defined in the rebuild. Its shape would be right
either way (seeded coordinates), so this is what shows a lost relation or dimension, or a reference
dimension where a driving one was. It costs nothing and touches nothing in the source. An `under`
sketch is reported to the user as a note: it arrives with exactly the source's shape, but when edited
each CAD system's solver decides what moves.
The definition-state check applies where the target reports a sketch's definition state (Onshape's API does not).
An importer that cannot read it reports `structure: match (definition state not checkable)`; flex probes (6.4), where
the file carries them, test the same behaviour.

### 6.3 Face by face

`ncx_face_compare` takes every face of the reference B-rep, described as for a reference (§5) and
measured by the host against the rebuild's faces, plus the area of every rebuilt face. Each
reference face is resolved as by `ncx_reference_match`; then:
- `unmatched`: reference faces with no rebuilt geometry; `extra`: rebuilt faces no reference face
  claimed;
- `split` / `merged`: one reference face covered by several rebuilt faces, or the reverse;
- `area`: groups of connected reference and rebuilt faces whose total areas differ by more than
  1e-3 relative (and 0.01 mm²).

`match` = one for one with equal areas; `topology` = the same surfaces and areas cut into faces
differently (not a failure); anything unmatched, extra or of different area = `mismatch`.

In a part with several bodies, run the comparison once per pair of bodies (paired as in 6.1), with only that body's
faces as candidates. Faces of two bodies can coincide (a block split by a plane): compared together they would be
`ambiguous`.

### 6.4 Parameter flex

A document may carry `flex`: probes measured in the source. Each changes one parameter of that
document to `value` in `configuration`:
- a driving sketch dimension (`kind` `length` / `angle`, `parameter` = `<dimension>@<sketch>`,
  value in mm / deg), or
- a global variable whose value is a plain number (`kind` `variable`).

The exporter then rebuilds every part of the model in dependency order, records `fingerprints`
(document id → fingerprint) for every part the change moved, and puts the value back. A layout's
dimension is thereby tested through the derived parts that use it.

The importer applies the same change to its rebuild of the whole model, compares every part
(`native`): those listed must match their fingerprint, and all others must not move. It then
restores the value. This shows that the parameters still drive the model as in the source, which
mass properties alone cannot: an under-constrained or wrongly related sketch, a lost equation link,
or a derived part cut off from its layout all rebuild to the same shape but flex differently.

Probes are written automatically, but only where structure cannot prove behaviour:
- a document whose geometry is driven by equations (global variables), and
- a document other parts are derived from (a layout).

Such a document gets up to 2 probes, global variables first; plain parts get none, and their export
never touches the model. (`FlexProbes`: -1 automatic, the default; 0 none; N probes on every part.)
Only dimensions of fully defined sketches are probed: how an under-defined sketch moves is up to each
CAD system's solver, not a translation fault. Parameters are chosen that are not equation-driven,
spread over the document, and changed by 5% (at least 0.5 mm or 1°), shrinking if growing breaks the
rebuild. Changes with no geometric effect are skipped. Every
part is checked to return exactly to its fingerprint afterwards, and feature names SolidWorks resets
on a rebuild (weldment members) are restored.

SolidWorks notes: an equation-driven dimension reports itself as *driven*; it must be exported and
replayed as a driving dimension (a reference dimension cannot be driven by an equation). An equation
edit is refused (silently, status -1) unless its document is the active one.

## 7. The core (`ncx_core`)

A C++17 library with a C ABI (`core/include/ncx_core.h`), statically linked CRT, and no
CAD-kernel dependency:

| function | job |
|---|---|
| `ncx_payload_write` / `ncx_payload_read` | the container (§2) |
| `ncx_model_validate` | structural rules (§4) |
| `ncx_fingerprint_compare` / `ncx_fingerprint_hash` | §6 |
| `ncx_reference_match` | §5 |
| `ncx_face_compare` | §6.3 |

Bindings:
- C#: `core/bindings/csharp/NcxCore.cs` (P/Invoke, .NET Framework 4.8 x64);
- Python: `python/ncx/core.py` (ctypes);
- a C++ host links directly.
