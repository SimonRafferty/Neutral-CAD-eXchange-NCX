# NCX add-in for Autodesk Fusion

Version **0.2.0**. Imports and exports NCX files (Neutral CAD eXchange, see `../README.md` and `../FORMAT-SPEC.md`)
in Autodesk Fusion: an NCX file opens as a parametric Fusion design with its history, checked against the file's own
geometry, and a Fusion design can be written as an NCX file.

Written by AI coding agents (Claude Code) from `../docs/ADDING-A-CAD-SYSTEM.md`, with Fusion only (no SolidWorks),
against the 61 sample files of NCX 0.6.0 (exported with SolidWorks add-in 0.6.0). Tested with **Autodesk Fusion 2706.1.14 on Windows 10 x64**.
Windows only (the add-in is a set of Windows DLLs; Fusion on macOS is not supported).

On the 61 samples: every file imports with its history and passes every check (mass, face by face, structure,
parameter flex where recorded); 28 are rebuilt exactly, 33 approximately (geometry identical, but something is built
as a functional equivalent or taken from the geometry the file carries, each reported with its reason), none falls
back to plain geometry. Every design re-exported from Fusion passes the format validator (one re-export, P46, falls
back to plain geometry when imported again). A real 19-document assembly (a folding step) imports with 12 of its 13
parts rebuilt. Details, per sample, and what is approximate and why: `FINDINGS.md` (sections 5, 8 and 14). Problems
with the format or the core: `ISSUES.md`.

## Installing

1. Download the installer:
   **[NCX-Fusion-Setup-0.2.0.exe](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/download/fusion-v0.2.0/NCX-Fusion-Setup-0.2.0.exe)**
   ([release page](https://github.com/SimonRafferty/Neutral-CAD-eXchange-NCX/releases/tag/fusion-v0.2.0)).
2. Double-click it. If Windows says "Windows protected your PC", click **More info**, then **Run anyway** (the
   installer is not signed by a paid certificate).
3. Click **Install**, then **Finish**. If Fusion is open, the installer asks you to close it first.
4. Start Fusion. **Import NCX...** and **Export NCX...** are in Fusion's **File** menu.

No administrator rights are needed: it installs for your Windows user only. Run the same installer again to update.
To remove it: Windows **Settings** > **Apps** > **NCX for Autodesk Fusion** > **Uninstall**.

If the commands are missing after starting Fusion: **Utilities** tab > **Add-Ins** (Shift+S) > **Add-Ins** tab >
**NCX** > **Run**, and tick **Run on Startup**.

Nothing else is installed: the DLLs carry their own C runtime. The add-in writes a log to
`%LOCALAPPDATA%\NcxFusion\ncx.log` and temporary files to `%LOCALAPPDATA%\NcxFusion` (the uninstaller removes them).

## Using it

The commands are in Fusion's **File** menu (and in the Design workspace, Solid tab, Insert panel):

- **Import NCX...** opens an NCX (STEP) file as a new, unsaved design. Every feature is rebuilt natively where Fusion
  has a counterpart, built as its functional equivalent where it has not (reported *approximate* with the reason),
  or taken from the geometry the file carries for it. The result is checked against the file's own geometry (mass
  properties, face by face, feature names, and parameter flex where the file records probes); a part that does not
  match is replaced by the file's plain geometry, with the reason. A progress dialog with **Cancel** shows long
  imports; cancelling closes everything the import made. A one-line message when everything carried, else the list
  of what needs attention.
- **Export NCX...** writes the active design: STEP geometry (Fusion's exporter) with the NCX payload embedded;
  an assembly's joints go out as the mates that leave the same freedom. Every file it writes passes
  `../validator/ncx_validate.py`.

A file without a payload, or whose geometry no longer matches its payload, opens as plain geometry with a message.

What Fusion builds differently (all reported *approximate* on import, with the reason): weldments (extrusions
with the corner treatments), Insert Part (the layout's sketches rebuilt in the part), configurations (the active
one only), cut lists and custom properties (attributes), mates (joints by their freedoms), component patterns
(placed instances), jogs (two folds), edge flanges with a position offset (an extension plus the flange). Taken
from the file's geometry: miter flanges and closed corners (their corner's bend region is the source system's own),
an edge flange that meets an earlier flange, lofted bends, a loft between unlike sections, dome, wrap, gusset, end
cap, weldment trim, mirror part. The full list: `FINDINGS.md` section 5.

## Building from source

Needs:
- Visual Studio 2019 or 2022 (the free Build Tools are enough) with the C++ x64 build tools and "C++ CMake tools for
  Windows";
- Fusion installed: its C++ SDK is the copy Fusion keeps at `%APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP`
  (another location: configure with `-DFUSION_SDK=<dir>`).

From the repository root:

```
powershell -ExecutionPolicy Bypass -File fusion\build.ps1            # build and deploy
powershell -ExecutionPolicy Bypass -File fusion\build.ps1 -Clean     # the same from an empty build folder
powershell -ExecutionPolicy Bypass -File fusion\package.ps1          # build, then write the installer to fusion\out
```

`build.ps1` builds the shared core (`../core`, unchanged), `NCX.dll` and `NcxFusionImpl.dll` into `fusion\build`,
and deploys them with `NCX\NCX.manifest` to `%APPDATA%\Autodesk\Autodesk Fusion 360\API\AddIns\NCX` (`-NoDeploy`
to skip). A build from scratch takes about two minutes. Restart Fusion once and the add-in starts with it. While
Fusion runs, `NCX.dll` is locked: the script says so, and the new stub is deployed by rerunning it with Fusion
closed (the implementation DLLs are always replaced).

`package.ps1` builds from an empty build folder and writes `fusion\out\NCX-Fusion-Setup-<version>.exe` (from `installer\NCX-Fusion.iss`; needs Inno Setup 6:
`winget install JRSoftware.InnoSetup`), which installs the add-in folder (`NCX\` with the manifest, the three DLLs, this
README and the licence) as described above. The version is set once, in `CMakeLists.txt` (`project(... VERSION ...)`): CMake writes
Fusion's manifest from `NCX\NCX.manifest.in` into `build\NCX.manifest`, and the installer takes its version from it.

Layout of the add-in folder:

| file | what |
|---|---|
| `NCX.dll` | what Fusion loads: the menu commands and a loader for the implementation |
| `NcxFusionImpl.dll` | import, verification, export |
| `ncx_core.dll` | the shared NCX core |
| `NCX.manifest` | Fusion's add-in manifest |

## Developing

`NCX.dll` loads `NcxFusionImpl.dll` from a private copy, so a rebuilt implementation can replace the running one
without restarting Fusion. When the folder `%LOCALAPPDATA%\NcxFusion\jobs` exists, the add-in also runs JSON jobs
dropped there (Fusion has no out-of-process API; this is how the add-in is tested without clicking). The folder
does not exist on a user's machine, so the watcher does nothing there.

The tools are Python 3 scripts (standard library only, except the validator); run them from the repository root.

| tool | what |
|---|---|
| `tools/rebuild.sh` | build, deploy, reload the implementation in the running Fusion (`sh`, e.g. Git Bash; `$PYTHON` names the Python, default `python`) |
| `tools/fjob.py` | run one job: `ping`, `reload`, `docs`, `import <file>`, `closeDocs`, or any JSON request |
| `tools/run_samples.py [names]` | import every sample in `../samples`, verify, print a table (results in `results/`) |
| `tools/roundtrip.py [names]` | import, export, import again (re-exports in `results/rt`) |
| `tools/results_table.py` | the per-sample Markdown table of `FINDINGS.md` from those results |
| `tools/shot.py <file> <png>` | import, screenshot, list the timeline |
| `tools/read_all.py` | the core's reader over the conformance vectors and the samples (set `NCX_CORE_DLL` to `fusion\build\core\Release\ncx_core.dll`) |
| `tools/tree.py` | the open design's components, bodies (volume, box) and occurrence offsets |
| `tools/frames_check.py <reply> <payload>` | joints by numbers: each joint the import made against its mate's `frame` |
| `../validator/ncx_validate.py` | the format validator, run on every re-export; set up with a venv in the clone: `pip install jsonschema "cadquery-ocp<8"` (8.0 breaks its level 4); it needs the core for levels 3-4: set `NCX_CORE_DLL` to `fusion\build\core\Release\ncx_core.dll` (without it the verdict is INCOMPLETE) |

Jobs: `{"cmd": "import", "file": ..., "close": true, "trace": true, "faces": true, "keepPartial": false, "only": [part names], "onlyAssemblies": [sub-assembly names]}`,
`{"cmd": "export", "file": ...}`, `{"cmd": "uiImport", "file": ..., "quiet": true, "cancelAfter": n}`,
`{"cmd": "probe", "what": "timeline" | "sketches" | "params" | "stepdoc"}`, `{"cmd": "screenshot", "file": ..., "view": "iso"}`.
A folder `%LOCALAPPDATA%\NcxFusion\debug_faces` makes rejected sheet-metal variants log their faces.

Source (`src/impl`): `importer.cpp` (the import and its checks), `reference.cpp` (the file's own geometry), `replay.cpp`
(the feature loop, carried results), `sketch.cpp`, `features*.cpp`, `sheetmetal.cpp`, `weldment.cpp`, `params.cpp`,
`props.cpp`, `assembly.cpp`, `joints.cpp`, `exporter.cpp`, `probe.cpp` (developer jobs), `neutral.h` (enumerated
words, and the SolidWorks numbers older files carry), `host.cpp` (descriptors, fingerprints and measurements for
the core's matcher). `src/addin/NCX.cpp` is the stub; `src/jobapi.h` the interface between the two DLLs.
