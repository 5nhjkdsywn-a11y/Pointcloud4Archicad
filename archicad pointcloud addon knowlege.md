# ArchiCAD Punktwolken-Dichte-Add-On — Wissensbasis

## Projektziel

Entwicklung eines ArchiCAD C++ Add-Ons, das die Dichte einer geladenen Punktwolke
über eine Prozentangabe (z.B. 10–100%) steuerbar macht — ohne zusätzliche
vorberechnete LOD-Dateien. ArchiCAD selbst bietet diese Funktion nativ nicht an.

-----

## Architektur-Überblick (aktuell implementiert)

```
Nutzer: Punktwolke selektieren → Menübefehl "Dynamische Punktwolke"
    ↓
[1] GSPC aus LCF lesen via IO::File (packed location)
    ↓
[2] Stride-Filter: jeden N-ten Punkt behalten → temp.gspc schreiben
    (gleicher Dateiname wie Original, z.B. "Punktwolke_Rampe_data.gspc")
    ↓
[3] Temp-Ordner (/tmp/PCDensityAddon/) an ERSTE Stelle der Bibliotheksliste
    via APIEnv_SetLibrariesID → ArchiCAD findet unsere Version zuerst
    ↓
[4] ReloadLibraries → gleiche Element-GUID, gleiche Position, dünnere Wolke
```

**Kein Eingriff in die LCF, keine Skript-Änderung nötig.**

-----

## ArchiCAD Punktwolken-Internals

### Dateiformat-Kette

- ArchiCAD importiert E57 und XYZ nativ
- Beim Import konvertiert der eingebaute Point Cloud Importer:
  - E57 → `.gspc` (GraphiSoft Point Cloud, proprietäres Binärformat)
  - → `.gsm` (GDL-Objekt)
  - Beide werden in einer `.lcf`-Datei verpackt
- Standard-Benennung: `Name.gsm` + `Name_data.gspc`
- GSM enthält im 3D-Script: `POINTCLOUD "Name_data.gspc"`
- ArchiCAD sucht GSPC im Library-Suchpfad (wie GSM-Dateien)

### GDL POINTCLOUD-Befehl

```gdl
POINTCLOUD "data_file_name"
```

- Dateiname wird im Library-Suchpfad gesucht — erste Übereinstimmung gewinnt
- Kein vollständiger Pfad möglich, nur Dateiname
- Damit: wer einen Ordner VORNE in die Bibliotheksliste setzt, gewinnt

-----

## LCF-Format (wichtig!)

**LCF ist KEIN ZIP-Archiv.**
Magic Bytes: `GLCF` (0x474c4346) — proprietäres Graphisoft-Format.
Struktur: `MAIN`, `FLDR`-Chunks — eigenes Container-Format.
→ Umbenennen zu .zip funktioniert NICHT.

Lesen aus LCF: über `IO::File` mit `IO::Location` (packed location):
```cpp
IO::File srcFile(gspcLoc);  // gspcLoc zeigt auf Datei innerhalb der LCF
srcFile.Open(IO::File::ReadMode);
srcFile.ReadBin(buf, n, &nRead);
srcFile.Close();
```

`lp.location` (aus `ACAPI_LibPart_Get`) ist auch für LCF-Einträge gesetzt.
`SetLastLocalName(IO::Name(gspcName))` ersetzt nur den Dateinamen, Container bleibt.

-----

## GSPC-Format (reverse-engineered aus BIMmTool 28)

BIMmTool 28 implementiert eigenen GSPC-Reader (`BIMm::GSPCReader`) und
Writer (`BIMm::GSPCWriter`) sowie `pdal::GspcWriter`. Aus dem Binary extrahiert:

```
Header (32 Bytes):
  [0-3]   Magic "GSPC"
  [4-7]   Version 0x00000002 (uint32 LE)
  [8-11]  xmin (float32)
  [12-15] xmax (float32)
  [16-19] ymin (float32)
  [20-23] ymax (float32)
  [24-27] zmin (float32)
  [28-31] zmax (float32)

Punkte (15 Bytes je, bis EOF):
  [0-3]   x (float32)
  [4-7]   y (float32)
  [8-11]  z (float32)
  [12]    R (uint8)
  [13]    G (uint8)
  [14]    B (uint8)
```

Kein gespeicherter Punktzähler im Header — Reader liest bis EOF.

-----

## BIMmTool-Erkenntnisse

BIMmTool 28 (`/Applications/BIMmTool 28/BIMmTool.bundle`) nutzt:
- `pdal` (Point Data Abstraction Library) mit eigenem E57-Reader
- `libE57Adapter` für E57-Dateien
- `BIMm::GSPCReader` / `BIMm::GSPCWriter` — eigene GSPC-Implementierung
- `pdal::GspcWriter` — PDAL-basierter GSPC-Writer
- Dichte-Varianten als separate GSPC-Dateien: `_05`, `_20`, `_25`, `_50` (5%, 20%, 25%, 50%)
- `PointcloudWriter::WritePointcloud` / `BuildStages` — Pipeline

MDID des eingebauten PointCloud_In-Importers (AC28):
- devID: 1198731108, localID: 797613762
- Parameter: `PCToImportLoc` (Quelldatei), `PCTargetLoc` (Zielverzeichnis)
- CommandCode: WEITERHIN UNBEKANNT (nicht aus Binary lesbar)

-----

## Bibliotheks-Management API

```cpp
// Lesen
GS::Array<API_LibraryInfo> libs;
Int32 embIdx = 0;
ACAPI_Environment(APIEnv_GetLibrariesID, &libs, &embIdx);

// Schreiben (setzt komplette Liste, löst Reload aus)
ACAPI_Environment(APIEnv_SetLibrariesID, &libs);

// API_LibraryInfo:
//   .location    IO::Location — Pfad des Ordners oder der LCF
//   .name        GS::UniString
//   .libraryType API_LocalLibrary / API_OtherObject / ...
//   .available   bool
//   .readOnly    bool
```

Ordner an ERSTER Stelle = hat Vorrang vor LCF mit gleichem Dateinamen.
`libs.Insert(0, newLib)` fügt an Position 0 ein.

-----

## Aktueller Build-Status (16.05.2026)

**Projektpfad:** `/Users/Shared/nextcloud/twA/Claude Code/claude_archicad/archicad-addon-dev/pw/`

**Build:**
```bash
cd pw/
cmake --build --preset ac26-mac-debug
```
Output: `build/ac26/Debug/PointCloudDensity26.bundle`
Installationspfad: `/Applications/GRAPHISOFT/punktwolke_addon/`

**Getestet (Stand 16.05.2026):**
- Addon lädt ✅ (ältere Version mit std::ifstream)
- GSPC-Filterung funktioniert ✅ (kein Lese/Schreib-Fehler)
- `ACAPI_LibPart_UpdateSection` auf LCF schlägt fehl ✅ bekannt, absichtlich vermieden
- Neueste Version (IO::File + EnsureLibraryFirst): lädt nicht ❌ — Ursache unklar
  - Kein Quarantine-Attribut
  - InputOutput.framework korrekt gelinkt
  - Wahrscheinlich: ArchiCAD-Neustart nötig nach Bundle-Wechsel

**Nächster Schritt:**
ArchiCAD 26 neu starten, neues Bundle laden, testen ob GSPC-Suche via Library-Order funktioniert.
Falls weiterhin Ladefehler: Diagnose mit ArchiCAD-Crash-Log oder schrittweiser Rückbau.

-----

## Offene Fragen

1. Lädt die neueste Bundle-Version nach ArchiCAD-Neustart?
2. Findet ArchiCAD die GSPC im Temp-Ordner wenn dieser vorne in der Bibliotheksliste steht?
3. Dichte-UI: Schieberegler oder Eingabedialog (aktuell hardcoded 25%)
