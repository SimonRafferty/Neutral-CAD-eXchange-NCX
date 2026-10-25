# Issues for the main repository (format, schema, core, SolidWorks exporter, validator)

The brief says to raise these as GitHub Issues; this agent may not touch GitHub, so they are written here. Each has
what is missing, the evidence, and a proposal. None of them was changed in this branch: the Fusion add-in works
around each one locally.

Round 4 (the owner's answers to 21-26, spec only) closes 21-26; nothing new for the format was found (section
"Round 4" at the end).

Round 1 (issues 1-10) was raised against the first sample set; round 5 of the format (core 0.2.0, merged as 0c2eac9)
answered most of them. Round 2 (issues 11-20) comes from rerunning the 56 re-exported samples. Round 3 (issues
21-26) comes from the samples re-exported with the owner's answers to 11-20 (bend geometry, edge flange length datum,
loft connectors, variable fillet transition, unfold bend faces, hole measured diameters).

## Status

| # | issue | status |
|---|---|---|
| 1 | notice body not recognisable without names | **closed**: header `noticebox`; the add-in finds the notice solid by its box (all part samples) |
| 2 | splines carry fit points only | **closed**: `nurbs`; built as a Fusion fixed spline, P23 now exact |
| 3 | Hole Wizard sizes leave the drilled diameter 0 | **closed** with issue 11 (round 3) |
| 4 | SolidWorks enum numbers in definitions | **closed**: words, read through `fusion/src/impl/neutral.h` (numbers still accepted); one number left (issue 14) |
| 5 | direction conventions undefined | **closed** for extrude, thin wall, base flange, revolve, patterns (spec §4 "Directions and orientation", all matched what the add-in had found); the weldment corner direction contradicts the samples (issue 12) |
| 6 | cone `semiAngle` in radians | **closed**: spec §4 units exceptions |
| 7 | `ncx_reference_match` header comment wrong | **closed**: comment corrected in core 0.2.0 |
| 8 | fingerprints cover solids only; surface parts unchecked | **closed** as documented (spec §6); a surface fingerprint is still not proposed in the spec, so P26-style parts stay unverifiable |
| 9 | parameters / equations undocumented | **closed**: `expression` in the schema, quoting and units stated, `targetRef` used to find equation targets |
| 10 | definitions only in the SolidWorks source | **closed** for plane, axis, point, hole, fillet, chamfer, structural member, transferred references; spec now says `sw/Ncx.Sw/Export*.cs` is the reference for the rest |
| 11 | hole sizes still 0, no measuredDiameters in the samples | **closed** (round 3): P11/P42 carry `measuredDiameters` [6.6, 11] and a filled `holeDiameter`; both now import **exact** with no measurement of the file's B-rep |
| 12 | weldment corners: segment direction is the chain's, not the entity's | **closed** (round 3): the spec now states the chain direction, which is what the add-in does; P46's corners match (P46 stays approximate only because weldments are a functional equivalent in Fusion) |
| 13 | component `configuration`: id or name? | **closed** (round 3): a name; the schema agrees; the exporter writes the name and every assembly re-export passes the validator |
| 14 | `miterFlange.reliefType: -1` is a number | **closed** (round 3): P35 carries `default`; the validator warns on numbers (M-ENUM-NUMBER) |
| 15 | validator setup: `cadquery-ocp` 8 breaks level 4 | **closed** (round 3): README pins `>=7.9,<8` |
| 16 | unfold/fold bends named by SolidWorks bend features | **closed** (round 3): `bends[].face` resolves; the unfold now unfolds the named bend by its face, and P41 (unfold, flat cut, fold) rebuilds natively: fallback -> approximate (only the cut list is approximate) |
| 17 | edge flange height datum and bend position not defined | **closed** (round 3): `length {distance, datum}` maps to Fusion's height datum; P12, P36, P41 flanges are built at the first try. Two flanges still carried for Fusion reasons, not format ones: P34's second (issue 21), P43's offset (issue 22) |
| 18 | sketched bend line position not defined | **closed** (round 3): P28's sketched bend is native: the Fusion fold's line is the source's line moved to the bend start taken from `bendGeometry` ((r+t)·tan(θ/2) = 1.657 mm, confirming the spec's outer-virtual-sharp definition) |
| 19 | loft section alignment not carried | **closed** as far as the format can go (round 3): `connectors` and the loft's `result` are carried; P14 is no longer a fallback (fallback -> approximate, the loft from its result). Fusion's loft has no alignment control, and the connectors (at the corners' own directions) match what Fusion already does there: the difference is between the connectors (issue 24) |
| 20 | variable fillet radius law not defined | **closed** (round 3): `transition: smooth`; Fusion's variable fillet with 9 intermediate radii on the cubic r = r0 + (r1 - r0)(3t^2 - 2t^3) matches the carried result at native tolerance: P30 **exact** (issue 23 asks the spec to name that law) |
| 21 | an edge flange meeting an earlier one: Fusion trims the earlier bend | **closed** for the format (round 4): now a Fusion trap in the brief. Still a Fusion limitation: P34 `Flange-Left` takes its carried result |
| 22 | edge flange `positionOffset`: no Fusion counterpart | **closed** (round 4): spec §4 defines it as the flange placed by `position`, moved `distance` outward, with the sheet extended to its bend. Built as Fusion offset faces (the sheet's edge face moved out by the distance) plus the edge flange on the moved edge: P43 `Edge-Offset` matches the source at **native** tolerance (carried -> functional equivalent) |
| 23 | variable fillet: state the smooth law | **closed** (round 4): spec §4 states r0 + (r1 - r0)(3s^2 - 2s^3), the law P30 already uses (exact) |
| 24 | loft connectors do not pin down a mixed-segment loft | **closed** (information; no change needed): P14's loft takes its carried result |
| 25 | miter flange `gap` and closed corner `gap` / `overlapRatio`: where measured | **closed** for the format (round 4): both defined in spec §4. Both rebuilt to the definitions and both still differ from the source only in the bend region at the corner, which the spec leaves to the source system ("the result has it"); the carried results stay. Evidence below (round 4) |
| 26 | jog: no counterpart outside SolidWorks; `fixProjectedLength` undefined | **closed** (round 4): spec §4 defines the offset per `dimensionPosition` and `fixProjectedLength`. Built as the sheet beyond the jog extended (projected length kept) plus two Fusion folds on `bendGeometry`: P29 `Jog-Step` matches the source at **native** tolerance (carried -> functional equivalent) |

---

## Round 1, still open in part

### 3. Hole Wizard sizes leave the drilled diameter at 0

See issue 11 for what remains. The add-in measures the drilled diameter on the file's own geometry and reports the hole
approximate (P11, P42).

### 8. Surface-only parts cannot be checked

Spec §6 now says fingerprints cover solids only and a surface-only part has `bodies: []`. That is honest, but such a
part (P26) can be neither stale-checked nor verified. **Proposal:** an optional per-sheet fingerprint (area, centroid,
box) under `fingerprint.sheets`.

---

## Round 2

### 11. Hole sizes are still 0 and `measuredDiameters` is missing in the samples

**Evidence.** Spec §4 `hole` says a size the source's tables supply and its API reports as 0 "is filled from them",
and `sizes.measuredDiameters` lists the hole's cylinder diameters. P11 and P42 (`CBORE-M6`) still have
`holeDiameter: 0`, `holeDepth: 0`, and no `measuredDiameters`. `sw/Ncx.Sw/ExportFeatures2.cs` `ExportHoleWizard` fills
both from `HoleDiameters(f)`, so that call must return nothing for these holes (perhaps the hole's faces are not
reachable from the feature while the definition's selections are accessed).

**Workaround.** The add-in reads `measuredDiameters` when present (smallest = drilled hole, largest = counterbore), and
otherwise still measures the coaxial cylinders on the file's own B-rep (6.6 mm), reporting the hole approximate.

**Proposal.** Fix `HoleDiameters` (or call it outside `AccessSelections`), and let the validator warn when a hole has
a zero `holeDiameter` and no `measuredDiameters`.

### 12. Weldment corners: the chain's direction decides, not the sketch entity's

**Evidence.** Spec §4 `structuralMember`: "Segment direction is the sketch entity's start to end", and `butt1` lets the
member whose segment *ends* at the corner run to the outside. P46 `Tube-Butt1` (segments `s5, s8, s7, s6`, gap 1):
`s7` runs from (250,100) to (450,100) and `s6` from (250,0) to (250,100) as entities, yet the result has `s7`'s member
running to x = 240 (outside) at (250,100) and stopping at x = 439 at (450,100): exactly what `butt1` gives when each
segment is taken in the direction the group's chain runs (s5 -> s8 -> s7 reversed -> s6 reversed). With entity
directions two corners have both segments starting (or both ending) there and the rule says nothing. `Tube-Butt2`
(coped) confirms the same.

**Workaround.** The add-in orders each group's segments as a chain (each runs towards the point it shares with the
next) and applies `butt1`/`butt2` to that direction; P46 matches.

**Proposal.** Spec: "segment direction is the direction of travel along the group's segments in the order listed".

Also worth stating: with `connectedSegments: copedCut`, the stopping member runs to the outside of the corner and the
other member's whole outer section is cut out of it; pieces left beyond the other member's rounded edges are dropped
(P46 `Tube-Butt2`).

### 13. Component `configuration`: id or name?

**Evidence.** `schema/ncx-0.1.schema.json` gives a component's `configuration` as `#/$defs/id`; the SolidWorks
exporter writes the configuration's *name* (`"Default"`); the validator (`M-CONFIG`) checks it against the document's
configuration *names*. The Fusion exporter first wrote the id (`cfg:Default`) and every assembly failed the validator.

**Proposal.** Pick one. Names match what the SolidWorks exporter writes; ids match the rest of the model (documents'
`activeConfiguration`, `suppressed` keys). Whichever: say it in the spec and make the schema description agree.

### 14. A numeric enumerated value is left: `miterFlange.reliefType: -1`

**Evidence.** P35 `Miter-Upstand` carries `reliefType: -1`: `Neutral.Word("relief", d.ReliefType)` has no word for
-1 (SolidWorks' "use the document default"?), so the number goes out. The validator does not flag it.

**Proposal.** A word for it (`default`), and a validator check that enumerated fields hold one of the table's words.

### 15. Validator setup: `pip install cadquery-ocp` installs 8.0, which breaks level 4

**Evidence.** Following `validator/README.md` (`pip install jsonschema cadquery-ocp`) installed cadquery-ocp 8.0.1;
level 4 then reports "OpenCASCADE not available (cannot import name 'TopTools_IndexedDataMapOfShapeListOfShape' from
'OCP.TopTools')" and every file is INCOMPLETE. `cadquery-ocp<8` (7.9.3.1) works: all 56 samples PASS.

**Proposal.** Pin `cadquery-ocp>=7.9,<8` in the README (and the `requirements.lock.txt` it mentions, which is not in
the repository).

### 16. Unfold / fold name their bends by the source's bend features

**Evidence.** P41 `Unfold-Flange` and `Fold-Flange`: `bends: [{feature: "f:5", sub: 1, name: "EdgeBend1"}]`, a
SolidWorks sub-feature of the edge flange. Another system has no such sub-features; it cannot pick those bends.

**Workaround.** All bends are unfolded (P41 has one).

**Proposal.** Carry each bend as a face descriptor of its bend face (in the state the unfold saw), like other face
references.

**Related (Fusion, no format change needed):** Fusion's refold takes no fixed face; after a cut in the flat state
that crosses the unfold's fixed face, it keeps the other side fixed (P41 refolds the plate up instead of the flange),
so P41 still falls back. Recorded in FINDINGS.

### 17. Edge flange: height datum and bend position are not defined in neutral terms

**Evidence.** `edgeFlange` gives a profile sketch (height and width), `angle`, `position` (`materialInside` ...). It
does not say from where the profile's height is measured (SolidWorks' default "outer virtual sharp"?), and the
`position` words are SolidWorks' own. Fusion's edge flange takes a height datum (inner faces, outer faces, tangent to
bend) and a bend position (inside, outside, start edge, tangent to side) that do not map one-to-one.

**Workaround.** The add-in builds the flange in each combination (the source's word first) and keeps the one whose
body is the feature's carried result: P12, P34 (front), P36 and P41 rebuild natively that way ("inside" with "inner
faces" for `materialInside`). P34's second flange matches no combination (its corner with the first flange differs),
and P43's `positionOffset` has no Fusion counterpart; both keep their carried results.

**Proposal.** Define, per `position` word, where the bend sits relative to the edge and the face, and from where the
height is measured; write the flange's resulting outer face or its sharp line so a system can check.

### 18. Sketched bend: where the bend lies relative to the line

**Evidence.** P28 `Bend-Line` (`position: materialInside`, 45 deg): Fusion's fold puts the bend line at the start,
centre or end of the bend; the source's result lies between "centre" and "end" (y extent 74.82 against 74.96 / 74.61),
so none matches and the carried result is used.

**Proposal.** Define each `position` word for a sketched bend (which face of the bent sheet touches the line, or the
offset of the bend zone from the line) so another system can offset its own bend line.

### 19. Loft section alignment is not carried

**Evidence.** P14 (rectangle at y = 0 to circle at y = 50): Fusion's loft has 73913.87 mm^3 where the source has
73745.80 (0.23 %), outside even the freeform tolerance. Which point of the circle each rectangle corner runs to is the
loft's alignment (SolidWorks' connectors); the payload has no field for it. Computing ruled lofts outside Fusion,
SolidWorks' volume is close to (1e-4) a correspondence by arc length starting at the rectangle corner's 45-degree
direction; Fusion's equals a per-edge correspondence at the corners' own directions. Splitting the circle into arcs
in Fusion did not change its alignment.

**Proposal.** Carry each section's start point (or the connectors) in `loft`, or the exact B-spline of the lateral
faces, so a system can at least verify or import the surface.

### 20. Variable fillet radius law

**Evidence.** P30 `Fillet-Variable`: radii 2 and 4 at the edge's ends. Fusion's variable-radius fillet with the same
end radii removes 80.05 mm^3 where the source removes 81.47; the face check fails. The law between the end radii
(linear in arc length? smooth?) is not stated.

**Workaround.** The carried result is kept (geometry decides).

**Proposal.** State the law (SolidWorks' "smooth transition" or "straight transition") in the definition.

---

## Round 3

Issues 11-20 are answered (status table). What the new data did not settle, from rerunning the 56 samples. None of
these blocks an import: each affected feature takes its carried result and is reported approximate.

### 21. An edge flange that meets an earlier flange: Fusion trims the earlier flange's bend (Fusion, no format change)

**Evidence.** P34 `Flange-Left` (materialInside, 22 mm from the outer virtual sharp, full length of the plate's
left edge, y 4..60) meets `Flange-Front`'s bend zone (x 0..100, y 0..4) at the corner. SolidWorks leaves the front
bend whole: the left flange adds exactly 37.42 mm^3 per mm of edge (2095.8 mm^3 for 56 mm). Fusion's edge flange
with the same length and datum treats the meeting as a two-bend corner and cuts the front bend's 4 mm at x 0..4
(-37.7 mm^3; face list measured). Not changed by any `TwoBendCornerOverride` shape, a near-zero rule (miter) gap,
mitred corners, or holding the flange back 0.0002 mm from that end.

**Workaround.** The carried result (unchanged from round 2).

**For the brief.** Worth a line in the per-CAD traps: Fusion re-shapes the earlier flange where a new flange meets
it; SolidWorks does not. A system may need the earlier flange's result again after building a later one.

### 22. Edge flange `positionOffset` has no Fusion counterpart (Fusion, a proposal)

**Evidence.** P43 `Edge-Offset` (`positionOffset {blind, 5}`, materialInside): the flange's outer face is 5 mm
beyond the edge (x = 85 for an edge at x = 80); `bendGeometry` confirms (axis at x = 81, outer radius 4). Fusion's
edge flange places its bend relative to the edge only by its four bend positions (outer face at 80, 82, 84 or as
inside); none gives 85, and Fusion has no offset.

**Proposal (optional).** None needed for correctness (the result is carried). If the format wants a system without
an offset to rebuild this natively, it could say in the spec that `positionOffset` is equivalent to extending the
sheet along the face by `distance` before the flange (Fusion could then build a flat extension plus a flange, as a
functional equivalent).

### 23. Variable fillet: name the smooth law

**Evidence.** P30 (`transition: smooth`, radii 2 and 4): a Fusion variable fillet with intermediate radii placed on
r(t) = r0 + (r1 - r0)(3t^2 - 2t^3), t the fraction of the edge's length, 9 per edge, matches the carried result at
**native** tolerance (volume, area, centroid, moments and every face). Fusion's own law between two end radii is
close to straight (80.05 mm^3 removed against the source's 81.47; a straight law predicts 80.16).

**Proposal.** The spec says `smooth` is "SolidWorks' smooth law, which leaves and reaches each vertex radius without
a sudden change of slope". State it as the cubic above (for a single edge between two vertex radii; with more than
two radii, presumably the same cubic between each pair), so other systems can follow it with their own intermediate
radii, as Fusion now does.

### 24. Loft `connectors` do not pin down a loft between sections of different segment counts (information)

**Evidence.** P14 (rectangle to circle): the connectors join each rectangle corner to the circle point in the
corner's own direction (12.48, 8.32), which is where Fusion's loft already joins them. Fusion's loft still differs by
0.23 % in volume: the correspondence between the corners (SolidWorks parametrises the circle by arc length from
those points, Fusion per rectangle edge) is not in the connectors, and Fusion offers no control over it. The spec
already makes such lofts carry their `result`, which is what the add-in now uses (P14 fallback -> approximate).

**Proposal.** None required. If a native rebuild is wanted, the lateral faces' exact B-spline (as for sketch
splines' `nurbs`) would let a system import the surface and verify it; connectors alone cannot.

### 25. Miter flange `gap`, closed corner `gap` / `overlapRatio`: where are they measured?

**Evidence.** P35 `Miter-Upstand` (two edges at 90 deg, gap 1): built as a Fusion edge flange on both edges with
mitred corners and the rule's miter gap. Fusion's volume is linear in its gap (gap 1: -15.3 mm^3 against the
source; 0.7071: -0.37 mm^3; 0.7: the volume matches but area and centroid z still differ), so the two systems shape
the mitred corner differently, not only measure the gap differently. P34 `Corner-Closed` (overlap, gap 0.2, ratio 1):
Fusion's corner closure (built on the carried flange) extends the end face by the thickness less the gap (+64.8
mm^3; the overlap ratio, transition and width alignment change nothing), SolidWorks' adds 54.4 mm^3.

**Proposal.** Define, for `miterFlange.gap` and `closedCorner.gap`, between which faces the gap is measured (across
the miter plane, or along the edge), and what `overlapRatio` is a ratio of. As with `bendGeometry`, a measured
placement would let a system check itself: for example the corner's two end faces after the feature (descriptors).

### 26. Jog: no counterpart outside SolidWorks; `fixProjectedLength` is not defined

**Evidence.** P29 `Jog-Step`: `bendGeometry` gives both bends, so a system without a jog could build two folds.
But `fixProjectedLength: true` means the sheet beyond the jog keeps its projected length (material is added); a pair
of folds consumes flat length instead, so the far edge moves. The spec lists the field without saying what it does.

**Proposal.** One sentence in the spec for `fixProjectedLength` (and `dimensionPosition` / `offset`: to which faces
the offset is measured), so a system can at least build the jog as a functional equivalent and know what to
compensate.

---

## Round 4

The owner answered 21-26 in FORMAT-SPEC §4 (no sample, schema or core change). What each answer did:

- **22, positionOffset.** Built exactly as the spec words it: the sheet's edge face at the flange edge is moved out by
  `distance` (Fusion `OffsetFacesFeature`; an extrusion of the face, joined, is the fallback), then the same edge
  flange (same position word, same `length` datum) is built on the moved edge. P43: native-tolerance match at the
  first try. Reported approximate (two Fusion features for one source feature).
- **26, jog.** The bends are placed from `bendGeometry`: the first bend starts at its axis projected onto the sheet;
  the second starts one flat bend length (θ(ri + k·t)) plus the straight between them further on (the straight from
  the axes' distance: both axes lie ri + t/2 from its mid-surface). `fixProjectedLength: true`: the sheet's edge faces
  beyond the jog are first moved out by (start of bend 2 + its flat length) - (where the moved sheet starts), 7.425 mm
  on P29, so the far edge stays at y 60. The offset that `dimensionPosition` gives (P29: outsideOffset 10) is checked
  against the one the bends give (10): they agree. Fusion details: a fold with **both** lines keeps the strip between
  them and folds both sides (it keeps the region of the face holding the face's centre), so the jog is two folds,
  the far line first; and a bend radius override given as a real `ValueInput` is read in **mm** (0.2 gave a 0.2 mm
  bend), so it is passed as text with its unit. P29: native-tolerance match. Reported approximate.
- **25, miter flange gap.** Built as the spec defines it: one Fusion edge flange on both edges, no mitring, each
  flange ending square gap/√2 = 0.707 mm back from the other flange's inner face (found from the other bend's axis
  and inner radius). Fusion's two-offset edge definitions run **outward** from the edge ends (+2.707 made the flange
  2.707 longer), so a held-back end is a negative offset. The best variant (bend position inside, height 20 from the
  inner faces) has the right box and is 15.28 mm^3 short of the source (17749.65 against 17764.93, 0.09 %): the same
  shortfall as round 3's mitred variant, i.e. the difference is the bend region at the corner, which the spec leaves
  to the source system. The carried result stays.
- **25, closed corner.** The source's result against the body before it (OpenCASCADE boolean difference of the two
  carried results): SolidWorks extends the left flange from y 4 to y 2.2 (0.2 short of the front flange's inner face)
  over its whole height **including its bend** (+70.58 mm^3, x 0..4, y 2.2..4, z -2..20) and re-shapes the two bends'
  meeting (-16.15 mm^3, x 0..4, y 0..4, z -2..2): net +54.43. Fusion's corner closure (on the carried left flange)
  extends only the flat part of the flange (1.8 x 2 x 18 = +64.8) and leaves the bends alone. The spec's flat-part
  definition matches what Fusion does; the bend region is the source's own, so the carried result stays.
- **21.** Now in the brief's Fusion traps. P34 `Flange-Left` still takes its carried result.

Nothing here needs a format change. If the owner wants P34/P35 native in a system like Fusion, the only missing
piece is the corner's bend-region shape, which only a result (as now) can carry.

## Round 5

Against the 59 samples of NCX 0.5.3 (P47, P48, L11 new). The brief and spec took this feedback in NCX 0.5.4.

### 27. Inscribed polygons carry one tangent (exporter; P48)

P48's `SK_Hex` is `constrained: "full"`, but its inscribed hexagon carries only `tangent` (s9, circle s2), the
`patterned` pairs and one fixed side: no importer can make it fully defined from that (Fusion: the circle and five
sides under-defined). Proposal: the exporter writes, for a polygon whose construction circle is inscribed, every side
`tangent` to the circle and every side's midpoint `coincident` with it (equal sides tangent to one circle do not fix a
hexagon; with their midpoints on it they do), as it writes every corner on the circle for a circumscribed one. No
format change.

### 28. Normal cut across a bend: carry the result (proposal)

`normalCut: true` has no counterpart in Fusion (nor, as far as is known, in Onshape). A straight cut is the same only
where the profile lies on a flat region; across a bend it removes different material, by less than the native mass
tolerance on typical parts (the spec's own 0.33 mm³). Proposal: the exporter attaches the feature's `result` when a
normal cut crosses a bend (it knows: the cut's faces are not all planar-normal to one flat face), so an importer
without the option can check its straight cut against it and use the result when they differ. L11's cut lies on a
flat face: Fusion's straight cut removes exactly profile area x thickness and the part matches.

### 29. Suppressed mate key in assemblies (spec wording)

Mates use `{config: bool}` "as for features", but assemblies have no configurations in schema 0.1 and L11 writes
`{"cfg:active": true}`. Proposal: the spec names `cfg:active` as the key of an assembly without configurations (the
Fusion exporter now writes suppressed joints this way too). The Fusion importer accepts the active configuration's
id, `cfg:active`, or a single key.
