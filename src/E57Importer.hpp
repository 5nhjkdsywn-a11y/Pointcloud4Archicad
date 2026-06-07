// *****************************************************************************
// E57Importer — E57-Punktwolken importieren, GSPC/GSM erzeugen
// Öffentliche Schnittstelle: PointCloudEntry, DoImport, DoAdjustDensity,
// TryDeleteEntry, AutoPlaceEntry, CollectOurLibParts, Log
// *****************************************************************************

#ifndef E57IMPORTER_HPP
#define E57IMPORTER_HPP

#include "APIEnvir.h"
#include "compat.h"

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

// --- ResolutionInfo ----------------------------------------------------------

struct ResolutionInfo {
    int            densityPercent;
    GS::UniString  dirName;    // z.B. "010"
    GS::UniString  label;      // z.B. "10 %"
};

// --- PointCloudEntry ---------------------------------------------------------

struct PointCloudEntry {
    GS::UniString   name;
    int             densityPercent;   // aktive Auflösung (für Palette)
    GS::UniString   e57Path;
    GS::UniString   outputDir;
    int             scanCount;        // 0 = altes Format (_data.gspc)
    Int32           libIndex;
    int             resCount;         // Anzahl Auflösungen (>1 = Multi-Res)
};

// --- Öffentliche Operationen -------------------------------------------------

GS::Array<PointCloudEntry>  CollectOurLibParts  ();

GS::UniString               DoImport            (int densityPct,
                                                  const GS::UniString* forceE57Path = nullptr);
bool                        TryDeleteEntry      (const PointCloudEntry& entry);
void                        AutoPlaceEntry      (const PointCloudEntry& entry);

#endif // E57IMPORTER_HPP
