// *****************************************************************************
// E57Importer — E57-Punktwolken importieren, GSPC/GSM erzeugen
// Öffentliche Schnittstelle: PointCloudEntry, DoImport, DoAdjustDensity,
// TryDeleteEntry, AutoPlaceEntry, CollectOurLibParts, Log
// *****************************************************************************

#ifndef E57IMPORTER_HPP
#define E57IMPORTER_HPP

#include "APIEnvir.h"
#include "compat.h"

#include <vector>

// --- Version-String ----------------------------------------------------------

#ifdef _WIN32
#  define PW_PLATFORM_STR "Windows"
#elif defined(__arm64__) || defined(__aarch64__)
#  define PW_PLATFORM_STR "macosARM"
#else
#  define PW_PLATFORM_STR "macosIntel"
#endif

#ifndef PW_VERSION
#  define PW_VERSION "0.1.x"
#endif

#ifdef ServerMainVers_2900
extern const char* kVersionStr; // "v... · AC29 · ..."
#elif defined(ServerMainVers_2800)
extern const char* kVersionStr; // "v... · AC28 · ..."
#elif defined(ServerMainVers_2700)
extern const char* kVersionStr; // "v... · AC27 · ..."
#else
extern const char* kVersionStr; // "v... · AC26 · ..."
#endif

#undef PW_PLATFORM_STR

// --- Debug-Logging -----------------------------------------------------------

void Log (const char* msg);

// --- PointCloudEntry ---------------------------------------------------------

struct PointCloudEntry {
    GS::UniString       name;
    int                 densityPercent;
    GS::UniString       e57Path;
    GS::UniString       outputDir;
    int                 scanCount;        // 0 = altes Format (_data.gspc)
    Int32               libIndex;
};

// --- Öffentliche Operationen -------------------------------------------------

GS::Array<PointCloudEntry>  CollectOurLibParts  ();

GS::UniString               DoImport            (int densityPct,
                                                  const GS::UniString* forceE57Path = nullptr);
bool                        TryDeleteEntry      (const PointCloudEntry& entry);
void                        AutoPlaceEntry      (const PointCloudEntry& entry);
bool                        HasPlacedInstances  (Int32 libIndex);
API_Guid                    FirstPlacedInstance (Int32 libIndex);   // APINULLGuid wenn keine

// --- Sichtbarkeitszustand einer platzierten Punktwolke -----------------------
// Gelesen aus der ersten platzierten Instanz; Schreiben trifft alle Instanzen.

struct StoryEntry {
    short           index;      // Geschossnummer (wie im Navigator)
    double          level;      // Unterkante (Projekt-Nullpunkt)
    GS::UniString   name;
};

struct CloudState {
    int                 scanCount = 0;
    std::vector<double> zScan;      // Scanner-Hoehe je Standort (Weltkoordinaten)
    std::vector<bool>   bScan;      // Standort sichtbar
    std::vector<bool>   bGroup;     // [0] = unter Unterkante, [j] = Geschoss j (1-basiert aufsteigend)
};

std::vector<StoryEntry>     GetStoriesAscending ();
bool                        ReadCloudState      (Int32 libIndex, CloudState& out);
bool                        WriteCloudVisibility (Int32 libIndex, const CloudState& st);
int                         StoryIndexOfScan    (double zScan, const std::vector<StoryEntry>& stories); // 0 = unter Unterkante
bool                        OpenSettingsDialog  (Int32 libIndex);   // selektiert + oeffnet den Dialog aus dem Event-Loop

// Add-on-Befehl, ueber den der Dialog verzoegert geoeffnet wird (AddOnMain registriert ihn)
#define kCmdOpenSettings    'PCSD'
#define kCmdOpenSettingsVer 1
GSErrCode __ACENV_CALL      OpenSettingsDialogCommand (GSHandle params, GSPtr resultData, bool silentMode);

#endif // E57IMPORTER_HPP
