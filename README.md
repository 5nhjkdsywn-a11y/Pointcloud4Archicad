# Pointcloud4Archicad

Archicad-Add-on, das **E57-Punktwolken direkt in Archicad importiert** und als
platzierbares Bibliothekselement verwaltet — ohne Umweg über externe Konverter.

*Archicad add-on that imports E57 point clouds directly and manages them as
placeable library parts. The UI is German; the code and this README are the
documentation.*

## Was es tut

- **Import** einer `.e57`-Datei mit wählbarer Punktdichte (1–100 %, Feld oben
  links, dann ➕). Pro Scan-Standort entsteht eine eigene `.gspc`-Datei
  (Archicads natives Punktwolkenformat), dazu ein GDL-Objekt (`.gsm`), das
  automatisch als Bibliothek eingebunden und im aktuellen Geschoss platziert
  wird. Dieselbe E57 erneut wählen = Neuimport mit anderer Dichte; das Objekt
  behält seine GUID, platzierte Instanzen bleiben erhalten.
- **Palette**: Dropdown aller importierten Punktwolken („Name – Dichte"),
  darunter ein Baum mit den Geschossen des Projekts und den Scan-Standorten
  darin. Das Auge vor jedem Eintrag blendet Geschoss oder Standort ein/aus —
  in allen platzierten Instanzen, rückgängig-fähig. Die Zuordnung Standort →
  Geschoss wird live aus den Projektgeschossen berechnet.
- **Einstellungen**-Button öffnet den Objekt-Einstellungsdialog der Punktwolke;
  **✕** löscht Objekt, erzeugte Dateien und Bibliothekseintrag (E57 bleibt).
- **Standortnummern** als 3D-Text an den Scanner-Positionen.
- **Grundriss-Ausschnitt**: Die Wolke wird im Grundriss an der
  Geschoss-Schnittebene geschnitten; optional mit eigenen Höhen, optional auch
  im 3D-Fenster.

Bedienung über eine dockbare Palette: **Planung → Punktwolken-Palette…**
Alle Icons stammen aus Archicads eigenen Ressourcen.

## Dateien, die der Import erzeugt

```
Scans/Punktwolke.e57                    ← Quelle (bleibt unverändert)
Scans/Punktwolke/Punktwolke.gsm          ← GDL-Objekt
Scans/Punktwolke/Punktwolke_010_1.gspc   ← Standort 1 bei 10 % Dichte
Scans/Punktwolke/Punktwolke_010_2.gspc   ← Standort 2 …
Scans/Punktwolke/Punktwolke.guid         ← stabile Objekt-GUID
Scans/Punktwolke/Punktwolke.pwmeta       ← Import-Metadaten (Bounding-Box, Scanner-Positionen)
```

Der Ordner `Scans/Punktwolke/` wird als Bibliothek registriert. Der Projektpfad
zur E57 steht im Objekt (`sE57Path`) — daran erkennt die Palette ihre Objekte.

## Unterstützte Versionen

| Archicad | macOS (Intel) | Windows (x64) |
|---|---|---|
| 26, 27, 28, 29 | ✅ | ✅ |

Apple Silicon: das Add-on wird derzeit nur als x86_64 gebaut (Xerces-C aus
Homebrew ist Intel-only). Es läuft nur, wenn auch Archicad unter Rosetta
(Intel) gestartet wird — ein nativ laufendes Archicad lädt es nicht.

Das fertige Bundle hat keine Abhängigkeit zu Homebrew: Xerces-C ist statisch
eingebettet. **Bundles als ZIP weitergeben** (z. B. `ditto -c -k --keepParent`),
nicht als loser Ordner über Cloud-Sync — dabei gehen Execute-Bits verloren und
Archicad lädt das Add-on stillschweigend nicht.

Die GDL-Objekte selbst sind versionsunabhängig.

## Installation (fertiges Add-on)

1. ZIP entpacken und das Bundle (`Pointcloud4Archicad_x86_64_XX.bundle`) bzw.
   die `.apx` in einen Ordner deiner Wahl kopieren, z. B.
   `/Applications/GRAPHISOFT/Pointcloud4Archicad/`
2. Archicad → *Optionen → Add-On Manager → Hinzufügen* → Bundle wählen
3. Archicad neu starten
4. Der Befehl heißt **Punktwolken-Palette…**. Erscheint er in einer neuen
   Archicad-Version nicht im Menü, liegt er unter *Optionen → Arbeitsumgebung →
   Menüs* bereit und wird dort einmalig zugeordnet — das Add-on registriert ihn
   benutzerkonfigurierbar, die Platzierung kommt aus dem Profil.

**macOS:** Die Bundles sind mit einer Apple Developer ID signiert und von Apple
notarisiert, das Ticket ist angeheftet. Sie laufen damit ohne Gatekeeper-Warnung
und auch offline. Immer als **ZIP weitergeben** (`ditto -c -k --keepParent`) —
beim Kopieren über Cloud-Sync gehen Ausführungsrechte verloren und Archicad lädt
das Add-on stillschweigend nicht.

**Windows:** Die `.apx`-Dateien sind nicht signiert. Das ist für den Betrieb
nicht nötig; Windows blockiert keine unsignierten Add-on-DLLs (eine
Authenticode-Signatur wäre erst bei einem eigenen Installer oder unter
AppLocker-Richtlinien relevant).

## Selbst bauen

### Voraussetzungen

- CMake ≥ 3.16, Python 3 (für Graphisofts `CompileResources.py`)
- **Archicad API Development Kit** der Zielversion(en) — kostenlos bei
  Graphisoft, nicht Teil dieses Repos. Erwarteter Pfad (siehe `CMakePresets.json`):
  ```
  ../../api/archicad/macos/API.Development.Kit.MAC.26.7000
  ../../api/archicad/windows/API.Development.Kit.WIN.26.7000
  ```
  Andere Ablage: `AC_API_DEVKIT_DIR` beim Konfigurieren übergeben.
- **libE57Format** wird per CMake `FetchContent` automatisch geladen.
- **Xerces-C** (Abhängigkeit von libE57Format):
  - macOS: `brew install xerces-c`
  - Windows: `vcpkg install xerces-c:x64-windows-static-md`
    (Pfad `XercesC_ROOT` in den Presets anpassen)
- macOS: Xcode; DevKit nach dem Download einmalig entquarantänen:
  `xattr -cr <DevKit-Ordner>`
- Windows: Visual Studio 2022 mit Toolset **v142** für AC26–28 und **v143** für AC29
  (die DevKit-Header erzwingen das).

### Build

```bash
cmake --preset ac26-mac-release          # konfigurieren
cmake --build --preset ac26-mac-release  # bauen
# → build/ac26/Release/Pointcloud4Archicad_x86_64_26.bundle
```

Presets gibt es für `ac{26,27,28,29}-{mac,win}-{debug,release}`.
Die Versionsnummer wird über `-DPW_VERSION=x.y.z` gesetzt (Cache-Variable).

Nach Änderungen an `RINT/*.grc.in` oder `RFIX.mac/Info.plist.in` erneut
konfigurieren — die Ressourcen werden beim Konfigurieren generiert.

## Wenn du das Projekt forkst

Das Add-on ist bei Graphisoft mit einer **MDID** registriert
(`RFIX/Pointcloud4ArchicadFix.grc`). Archicad identifiziert Add-ons darüber.
Ein Fork, der unter derselben MDID gebaut wird, kollidiert mit diesem Add-on,
sobald beide installiert sind. Bitte eine eigene MDID registrieren
(Graphisoft Developer Program, kostenlos) und dort eintragen.

## Aufbau

```
src/AddOnMain.cpp           Registrierung, Menü, Lebenszyklus
src/PunktwolkenPalette.*    dockbare DG::Palette (Icon-Leiste als ListView, Dropdown, Geschoss-Baum)
src/ToolIcon.*              Icon-Bitmaps (normal/hover) aus Archicads Ressourcen
src/E57Importer.*           E57 → GSPC, GSM-Erzeugung (XML → LP_XMLConverter), Bibliotheksverwaltung
src/compat.h                API-Unterschiede AC26 ↔ AC27+ in einer Datei
RINT/, RFIX/, RFIX.mac/, RFIX.win/   Ressourcen (Dialog, Strings, MDID, Icons, plist)
```

Das GSM wird nicht über die API, sondern als XML geschrieben und mit Archicads
mitgeliefertem `LP_XMLConverter` kompiliert — `ACAPI_LibPart_Save` verweigert
das Schreiben in Ordner, die noch keine aktive Bibliothek sind.

## Lizenz

MIT — siehe [LICENSE](LICENSE). Abhängigkeiten: libE57Format (Boost Software
License), Xerces-C (Apache 2.0). Das Archicad-DevKit unterliegt Graphisofts
Lizenzbedingungen und wird nicht mitgeliefert.
