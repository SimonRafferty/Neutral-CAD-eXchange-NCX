# What NCX carries (SolidWorks add-in)

Every import reports, feature by feature, anything that did not carry exactly and why. This page summarises what the
SolidWorks add-in carries today, measured on a corpus of test models built through the SolidWorks API: each is exported
from SolidWorks 2026, imported (rebuilt with its history) into SolidWorks 2026 and 2018, checked by the import's own
verification (mass properties, face-by-face geometry, structure, parameter flex), then exported again and compared
with the source export.

## Test corpus

| Test model | What it exercises | 2026 → 2026 | 2026 → 2018 |
|---|---|---|---|
| P01_block | sketch, extrude | exact | exact |
| P02_block_features | face sketch, cut, fillet (tangent chain), chamfer | exact | exact |
| P03_revolve_shaft | revolve, revolve cut, centreline axis | exact | exact |
| P04_ref_plane | offset plane, sketch on it, reversed normal | exact | exact |
| P05_patterns_mirror | linear pattern (2 dirs), mirror | exact | exact |
| P06_circular | circular pattern | exact | exact |
| P07_shell | shell | exact | exact |
| P08_equations | global variables, equations | exact | exact |
| P09_draft | neutral-plane draft | exact | exact |
| P10_configs | 3 configurations: values + suppression | exact | exact |
| P11_hole_wizard | Hole Wizard counterbore, 2 positions | exact | exact |
| P12_sheet_metal | base flange (open profile), edge flange, cut, flat pattern | exact | exact |
| P13_sweep | sweep along line–arc–line | exact | exact |
| P14_loft | loft rectangle → circle | exact | exact |
| P15_multibody | non-merged boss, combine (add), separate body | exact | exact |
| P16_weldment | weldment, structural member (ISO square tube), derived configurations | exact | exact² |
| P17_properties | custom properties (file and configuration, linked values), materials | exact | exact |
| P18_appearances | colours on part/body/feature/face, a library texture | exact | exact |
| P19_extrude_options | thin extrude, start offset, up to vertex, selected contours | exact | exact |
| P20_ref_geometry | reference axes/points/coordinate system, thin revolve about an axis | exact | exact |
| P21_fillets | several radii, face fillet, vertex chamfer | exact | exact |
| P22_bodies | body pattern with a skipped instance, mirror body, delete body | exact | exact |
| P23_sketch_curves | splines (natural ends, handles), ellipse | exact | exact³ |
| P24_weldment_extras | weldment trim, end cap | exact | exact² |
| P25_lofted_bend | sheet-metal lofted bend | exact | exact |
| P26_surfaces | extruded and revolved surfaces | exact | exact |
| P27_rib_move | rib on a mid plane, move/copy body (copy by translation, move by rotation) | exact | exact |
| P28_sheet_extras | hem, break corner, sketched bend | exact | exact |
| P29_jog | jog | exact | exact |
| P30_variable_fillet | variable-radius fillet (per-vertex radii) | exact | exact |
| P31_guide_curves | sweep and loft with guide curves | exact | exact |
| P32_cosmetic_thread | cosmetic thread | exact | exact |
| P33_gusset | weldment gusset | exact | exact² |
| P34_closed_corner | sheet-metal closed corner | exact | exact |
| P35_miter_flange | sheet-metal miter flange round a corner | exact | exact |
| P36_multi_edge_flange | one edge flange on two edges | exact | exact |
| P37_dome | dome on a round post, a fillet after it | exact | exact |
| P38_split | split body by a plane, a chamfer on one half | exact | exact |
| P39_knit | two planar surfaces, knit | exact | exact |
| P40_wrap | sketch wrapped (embossed) round a cylinder | exact | exact |
| P41_unfold_fold | sheet metal unfolded, slot cut across the bend, folded | exact | exact |
| P42_hole_positions | Hole Wizard holes dimensioned from the origin in their position sketch | exact | exact |
| P43_flange_options | base flange extruded both ways, edge flange with a position offset | exact | exact |
| P44_cut_list | weldment cut-list items with user properties | exact | exact² |
| P45_sketch3d | 3D sketches (lines along x/y/z with dimensions, an arc), a member along a 3D path | exact | exact² |
| L01 | layout → Insert Part (derived plate) + pin → assembly, face mates | exact | exact |
| L02 | plane mates, nested sub-assembly, mates to a sub-assembly plane and to nested components | exact | exact |
| L03 | mates on axes, a reference point, a component origin, a sketch point, vertices; a limit mate | exact | exact |
| L04 | plane mates to the assembly's own planes; linear (4 × 2) and circular component patterns (about a reference axis and about a cylindrical face) | exact | exact |
| L05 | hinges, gear, screw, width mate | exact | exact⁴ |
| L06 | Mirror Components (same-part instance) and a Mirror Part opposite hand | exact | exact |
| L07 | slot and symmetric mates | exact | exact |
| L08 | an assembly reference plane and a mate to it | exact | exact |
| L09 | rack-and-pinion mate (pinion on a hinge, rack sliding) | exact | exact⁵ |
| L10 | assembly-level sketch and through-all assembly cut across two components | exact | exact |

"exact": every feature rebuilt, fingerprints match (volume/area 1e-5, centroid 0.001 mm, moments 1e-4), and the
re-export is structurally identical. ² Reported *approximate* by design: 2018 takes the profile from its own library file
(`…\SOLIDWORKS\lang\english\weldment profiles\iso\square tube\20 x 20 x 2.sldlfp`). ³ 2018 fits a natural-end
spline slightly differently, so spline parts are compared at the exchange tolerance across versions (noted).
⁴ 2018 has no hinge mate: each is rebuilt as a concentric plus a coincident mate and reported *approximate*; the
re-export compare accepts that pair as the hinge. ⁵ 2018's API cannot make a rack-and-pinion mate (no mate data
before 2019; `AddMate5` refuses it with every entity order, distance and mark tried): the rack and pinion arrive in
place, reported *approximate* "placed, not coupled"; the compare leaves out mates a replay reports that way.

## Known gaps

A feature the exporter cannot map is `opaque` with a reason. It carries its result (FORMAT-SPEC §4, "Feature
results"), so the part still rebuilds: that step is fixed geometry, reported *approximate*. A part falls back to the file's own B-rep
only when the result cannot be used either.
- **Not creatable through the SolidWorks API at all** (carried as results, or as placed components):
  - full-round fillets and thicken;
  - cam-follower mates;
  - assembly cuts limited to chosen components;
  - in 2018, rack-and-pinion mates.
- **Made with the default, reported:** a screw mate's pitch other than 1 mm per turn.
- **Not mapped:**
  - linear-coupler, path and universal-joint mates (exported opaque; the components arrive in place);
  - loft connectors (SolidWorks' default connection is used);
  - surface operations other than knit/planar/extruded/revolved, such as offset, fill, boundary and trim (carried
    as results);
  - curve features such as helix and composite curve (what is built on them carries its result);
  - weldment profiles located at a point other than the default, and mirrored profiles;
  - hinge angle limits in 2018 (it has no hinge);
  - opposite-hand Mirror Components copies (code present, unverified).
- **Not carried, by design:**
  - sketch pictures (images placed in sketches);
  - design tables as tables: the configurations they produce are carried.
