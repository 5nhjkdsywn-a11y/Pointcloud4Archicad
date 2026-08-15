// *****************************************************************************
// E57Importer — E57-Punktwolken importieren, GSPC/GSM erzeugen
// *****************************************************************************

#include "E57Importer.hpp"

#include "File.hpp"
#include "FileSystem.hpp"

#include "DGDLLExport.h"
#include "DGFileDlg.hpp"
#include "DGDialog.hpp"

#include "E57SimpleReader.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <string>
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#  include <shlobj.h>
#  include <direct.h>
#else
#  include <spawn.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <dirent.h>
#endif

// --- Version-String ----------------------------------------------------------

#ifdef _WIN32
#  define PW_PLATFORM_STR "Windows"
#elif defined(__arm64__) || defined(__aarch64__)
#  define PW_PLATFORM_STR "macosARM"
#else
#  define PW_PLATFORM_STR "macosIntel"
#endif

#ifdef ServerMainVers_2900
const char* kVersionStr = "v" PW_VERSION " \xC2\xB7 AC29 \xC2\xB7 " PW_PLATFORM_STR;
#elif defined(ServerMainVers_2800)
const char* kVersionStr = "v" PW_VERSION " \xC2\xB7 AC28 \xC2\xB7 " PW_PLATFORM_STR;
#elif defined(ServerMainVers_2700)
const char* kVersionStr = "v" PW_VERSION " \xC2\xB7 AC27 \xC2\xB7 " PW_PLATFORM_STR;
#else
const char* kVersionStr = "v" PW_VERSION " \xC2\xB7 AC26 \xC2\xB7 " PW_PLATFORM_STR;
#endif

#undef PW_PLATFORM_STR

// --- Log-Pfad ----------------------------------------------------------------

#ifdef _WIN32
static const char* LOG_PATH = "C:\\ProgramData\\GRAPHISOFT\\punktwolke_addon\\pcd_debug.log";
#else
static const char* LOG_PATH = "/Applications/GRAPHISOFT/punktwolke_addon/pcd_debug.log";
#endif

// =============================================================================
// Interne Hilfsfunktionen
// =============================================================================

// --- Logging -----------------------------------------------------------------

void Log (const char* msg)
{
    FILE* f = fopen (LOG_PATH, "a");
    if (f) { fprintf (f, "%s\n", msg); fclose (f); }
}

// --- UUID für GSM-MainGUID ---------------------------------------------------

static void GenGUID (char out[37])
{
    uint32_t d[4];

#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextA (&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        CryptGenRandom (hProv, sizeof (d), (BYTE*)d);
        CryptReleaseContext (hProv, 0);
    }
    else
    {
        for (int i = 0; i < 4; i++) d[i] = (uint32_t)(GetTickCount64 () ^ (uintptr_t)out ^ i);
    }
#else
    for (int i = 0; i < 4; i++) d[i] = arc4random ();
#endif

    d[1] = (d[1] & 0xFFFF0FFF) | 0x00004000;
    d[2] = (d[2] & 0x3FFFFFFF) | 0x80000000;
    snprintf (out, 37, "%08X-%04X-%04X-%04X-%04X%08X",
        d[0],
        (d[1] >> 16), (d[1] & 0xFFFF),
        (d[2] >> 16), (d[2] & 0xFFFF),
        d[3]);
}

// --- Plattform-unabhängige Pfadtrenner-Suche ---------------------------------

static UInt32 FindLastSep (const GS::UniString& path)
{
    UInt32 fwd  = path.FindLast ('/');
    UInt32 back = path.FindLast ('\\');
    if (fwd  == GS::MaxUIndex) return back;
    if (back == GS::MaxUIndex) return fwd;
    return std::max (fwd, back);
}

#ifdef _WIN32
static const char PATH_SEP = '\\';
#else
static const char PATH_SEP = '/';
#endif

// --- Stabile GUID: beim ersten Import erzeugen, danach aus .guid-Datei lesen --
// Verhindert, dass ArchiCAD platzierte Objekte als "fehlend" markiert, wenn
// das GSM bei einem Reload neu erzeugt wird.

static void GetOrCreateGuid (const char* outputDirPath,
                               const char* baseName,
                               char        out[37])
{
    char guidPath[1024];
    snprintf (guidPath, sizeof (guidPath), "%s%c%s.guid",
              outputDirPath, PATH_SEP, baseName);

    // Bereits vorhandene GUID lesen
    FILE* f = fopen (guidPath, "r");
    if (f) {
        char buf[40] = {};
        size_t n = fread (buf, 1, 36, f);
        fclose (f);
        if (n == 36) {
            memcpy (out, buf, 36);
            out[36] = '\0';
            return;
        }
    }

    // Neue GUID erzeugen und speichern
    GenGUID (out);
    f = fopen (guidPath, "w");
    if (f) {
        fwrite (out, 1, 36, f);
        fclose (f);
    }
}

// --- LP_XMLConverter-Pfad ----------------------------------------------------

#ifdef _WIN32
#  ifdef ServerMainVers_2900
static const char* LP_XMLCONVERTER =
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD 29\\LP_XMLConverter.exe";
#  elif defined(ServerMainVers_2800)
static const char* LP_XMLCONVERTER =
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD 28\\LP_XMLConverter.exe";
#  elif defined(ServerMainVers_2700)
static const char* LP_XMLCONVERTER =
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD 27\\LP_XMLConverter.exe";
#  else
static const char* LP_XMLCONVERTER =
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD26\\LP_XMLConverter.exe";
#  endif
#else
#  ifdef ServerMainVers_2900
static const char* LP_XMLCONVERTER =
    "/Applications/GRAPHISOFT/ARCHICAD29/Archicad 29.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter";
#  elif defined(ServerMainVers_2800)
static const char* LP_XMLCONVERTER =
    "/Applications/GRAPHISOFT/ARCHICAD28/Archicad 28.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter";
#  elif defined(ServerMainVers_2700)
static const char* LP_XMLCONVERTER =
    "/Applications/GRAPHISOFT/ARCHICAD27/Archicad 27.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter";
#  else
static const char* LP_XMLCONVERTER =
    "/Applications/GRAPHISOFT/Archicad 26/Archicad 26.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter";
#  endif
#endif

// --- PwMeta: Sidecar-Datei mit Import-Parametern (.pwmeta) -------------------
// Beim ersten Import geschrieben, beim Reload gelesen.
// Erlaubt GSM-Neugenerierung ohne erneutes E57-Parsing wenn GSPC schon vorhanden.

struct PwMeta {
    float    origX, origY, origZ;   // Bounding-Box-Ausmaße (Meter)
    float    offX,  offY,  offZ;    // Koordinaten-Offset (Weltkoordinaten)
    int32_t  scanCount;
    int64_t  totalPoints;
    int32_t  storyCount;
    // Danach im Stream:
    //   scanCount * double  scanTx
    //   scanCount * double  scanTy
    //   scanCount * double  scanTz
    //   int32_t e57PathLen + char e57Path[]
};

static void WritePwMeta (const char* outputDirPath, const char* baseName,
                          const PwMeta& m,
                          const std::vector<double>& tx,
                          const std::vector<double>& ty,
                          const std::vector<double>& tz,
                          const char* e57Path)
{
    char path[1024];
    snprintf (path, sizeof (path), "%s%c%s.pwmeta", outputDirPath, PATH_SEP, baseName);
    FILE* f = fopen (path, "wb");
    if (!f) return;
    const char magic[4] = {'P', 'W', 'M', 'F'};
    uint32_t ver = 1;
    fwrite (magic, 1, 4, f);
    fwrite (&ver,  4, 1, f);
    fwrite (&m,    sizeof (PwMeta), 1, f);
    fwrite (tx.data (), sizeof (double), (size_t)m.scanCount, f);
    fwrite (ty.data (), sizeof (double), (size_t)m.scanCount, f);
    fwrite (tz.data (), sizeof (double), (size_t)m.scanCount, f);
    int32_t plen = (int32_t)strlen (e57Path);
    fwrite (&plen,  4, 1, f);
    fwrite (e57Path, 1, (size_t)plen, f);
    fclose (f);
}

static bool ReadPwMeta (const char* outputDirPath, const char* baseName,
                         PwMeta& m,
                         std::vector<double>& tx,
                         std::vector<double>& ty,
                         std::vector<double>& tz,
                         std::string& e57Path)
{
    char path[1024];
    snprintf (path, sizeof (path), "%s%c%s.pwmeta", outputDirPath, PATH_SEP, baseName);
    FILE* f = fopen (path, "rb");
    if (!f) return false;
    char magic[4]; uint32_t ver;
    if (fread (magic, 1, 4, f) != 4 || memcmp (magic, "PWMF", 4) != 0
        || fread (&ver, 4, 1, f) != 1 || ver != 1
        || fread (&m, sizeof (PwMeta), 1, f) != 1) { fclose (f); return false; }
    tx.resize ((size_t)m.scanCount); ty.resize ((size_t)m.scanCount); tz.resize ((size_t)m.scanCount);
    if (fread (tx.data (), sizeof (double), (size_t)m.scanCount, f) != (size_t)m.scanCount
        || fread (ty.data (), sizeof (double), (size_t)m.scanCount, f) != (size_t)m.scanCount
        || fread (tz.data (), sizeof (double), (size_t)m.scanCount, f) != (size_t)m.scanCount)
        { fclose (f); return false; }
    int32_t plen = 0;
    if (fread (&plen, 4, 1, f) != 1 || plen < 0 || plen > 4096) { fclose (f); return false; }
    e57Path.resize ((size_t)plen);
    fread (&e57Path[0], 1, (size_t)plen, f);
    fclose (f);
    return true;
}

// --- Hilfsfunktion: Dichte-Prozent → dreistelliger Ordner-/Dateiname-Code -----

static GS::UniString DensityDirName (int densityPct)
{
    char buf[8];
    snprintf (buf, sizeof (buf), "%03d", densityPct);
    return GS::UniString (buf);
}


[[maybe_unused]] static void RecursiveDeleteDir (const char* dirPath)
{
#ifdef _WIN32
    char pattern[1024];
    snprintf (pattern, sizeof (pattern), "%s\\*", dirPath);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA (pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp (fd.cFileName, ".") == 0 || strcmp (fd.cFileName, "..") == 0) continue;
            char child[1024];
            snprintf (child, sizeof (child), "%s\\%s", dirPath, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RecursiveDeleteDir (child);
            else DeleteFileA (child);
        } while (FindNextFileA (h, &fd));
        FindClose (h);
    }
    RemoveDirectoryA (dirPath);
#else
    DIR* d = opendir (dirPath);
    if (d) {
        struct dirent* ent;
        while ((ent = readdir (d)) != nullptr) {
            if (strcmp (ent->d_name, ".") == 0 || strcmp (ent->d_name, "..") == 0) continue;
            char child[1024];
            snprintf (child, sizeof (child), "%s/%s", dirPath, ent->d_name);
            if (ent->d_type == DT_DIR) RecursiveDeleteDir (child);
            else remove (child);
        }
        closedir (d);
    }
    rmdir (dirPath);
#endif
}

// --- GSM via LP_XMLConverter erzeugen ----------------------------------------

static bool WriteGsmViaXml (
    int         scanCount,
    const char* baseName,
    const char* outputDirPath,
    float origX, float origY, float origZ,
    float offX,  float offY,  float offZ,
    int         densityPct,
    const char* e57PathStr,
    int64_t     totalPoints,
    const double* scanTx,
    const double* scanTy,
    const double* scanTz,
    int           storyCount)
{
    char guid[37];
    GetOrCreateGuid (outputDirPath, baseName, guid);

#ifdef _WIN32
    char xmlTempDir[MAX_PATH] = "C:\\Temp";
    GetTempPathA (MAX_PATH, xmlTempDir);
    char xmlPath[MAX_PATH];
    snprintf (xmlPath, sizeof (xmlPath), "%spw_gsm_%s.xml", xmlTempDir, baseName);
#else
    char xmlPath[1024];
    snprintf (xmlPath, sizeof (xmlPath), "/tmp/pw_gsm_%s.xml", baseName);
#endif

    FILE* f = fopen (xmlPath, "w");
    if (!f) return false;

    fprintf (f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf (f, "<Symbol IsArchivable=\"false\" IsPlaceable=\"true\" "
               "MainGUID=\"%s\" MigrationValue=\"Normal\" "
               "Owner=\"1196638531\" Signature=\"1196644685\" Version=\"44\">\n\n", guid);

    fprintf (f,
        "<Ancestry SectVersion=\"1\" SectionFlags=\"0\" SubIdent=\"0\" Template=\"false\">\n"
        "\t<MainGUID>F938E33A-329D-4A36-BE3E-85E126820996</MainGUID>\n"
        "\t<MainGUID>103E8D2C-8230-42E1-9597-46F84CCE28C0</MainGUID>\n"
        "\t<MainGUID>B298E56B-35FF-45C6-AA50-6D7F0B40BB1A</MainGUID>\n"
        "</Ancestry>\n\n"
        "<CalledMacros SectVersion=\"2\" SectionFlags=\"0\" SubIdent=\"0\">\n"
        "</CalledMacros>\n\n");

    // 3D-Script
    fprintf (f, "<Script_3D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "if bProject2D then\n"
        "\taddz _upperCutPlaneHeight\n"
        "\tcutplane{2} 0, 1\n"
        "\tdel 1\n\n"
        "\taddz _lowerCutPlaneHeight\n"
        "\trotx 180\n"
        "\tcutplane{2} 0, 1\n"
        "\tdel 2\n"
        "endif\n\n"
        "_scale3DModelX = scaleX * _scaleOriginalSize\n"
        "_scale3DModelY = scaleY * _scaleOriginalSize\n"
        "_scale3DModelZ = scaleZ * _scaleOriginalSize\n\n"
        "mul _scale3DModelX, _scale3DModelY, _scale3DModelZ\n\n"
        "if bVisible then\n\n");
    {
        char dirName[8];
        snprintf (dirName, sizeof (dirName), "%03d", densityPct);
        for (int sc = 0; sc < scanCount; ++sc)
            fprintf (f, "\tif _sv[%d] then POINTCLOUD \"%s_%s_%d.gspc\"\n",
                sc + 1, baseName, dirName, sc + 1);
        fprintf (f, "\n");
    }
    fprintf (f,
        "\tif bShowNumbers then\n"
        "\t\tDEFINE STYLE \"scannum\" \"Arial\", 300, 1, 2\n"
        "\t\tSET STYLE \"scannum\"\n\n"
        "\t\tfor _i = 1 to iScanCount\n"
        "\t\t\tif _sv[_i] then\n"
        "\t\t\t\tADD xScan[_i], yScan[_i], zScan[_i]\n"
        "\t\t\t\tTEXT 0.01, 0, STR(_i, 1, 0)\n"
        "\t\t\t\tDEL 1\n"
        "\t\t\tendif\n"
        "\t\tnext _i\n"
        "\tendif\n\n"
        "endif\n\n"
        "del 1\n\n"
        "if bProject2D then\n"
        "\tcutend\n"
        "\tcutend\n"
        "endif\n");
    fprintf (f, "]]>\n</Script_3D>\n\n");

    // 2D-Script
    fprintf (f, "<Script_2D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "project2{2} 3, 270, 3,\n"
        "\t\t\tparameters bProject2D = 1\n\n");
    fprintf (f, "]]>\n</Script_2D>\n\n");

    // Master-Script (1D)
    fprintf (f, "<Script_1D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "EPS = 0.00001\n\n"
        "MODEL_UNIT_M\t= 1\n"
        "MODEL_UNIT_CM\t= 2\n"
        "MODEL_UNIT_MM\t= 3\n"
        "MODEL_UNIT_FT\t= 4\n"
        "MODEL_UNIT_IN\t= 5\n\n"
        "_scaleOriginalSize = 1\n"
        "if iModelUnit = MODEL_UNIT_MM\tthen\t_scaleOriginalSize = 0.001\n"
        "if iModelUnit = MODEL_UNIT_CM\tthen\t_scaleOriginalSize = 0.01\n"
        "if iModelUnit = MODEL_UNIT_IN\tthen\t_scaleOriginalSize = 0.0254\n"
        "if iModelUnit = MODEL_UNIT_FT\tthen\t_scaleOriginalSize = 0.3048\n\n"
        "if GLOB_SCRIPT_TYPE <> 5 then\n"
        "\tif bOverrideCutplane then\n"
        "\t\t_upperCutPlaneHeight = GLOB_CSTORY_ELEV - GLOB_HSTORY_ELEV + upperPlaneHeight - GLOB_ELEVATION\n"
        "\t\t_lowerCutPlaneHeight = GLOB_CSTORY_ELEV - GLOB_HSTORY_ELEV + lowerPlaneHeight - GLOB_ELEVATION\n"
        "\telse\n"
        "\t\t_upperCutPlaneHeight = GLOB_CSTORY_ELEV - GLOB_HSTORY_ELEV + GLOB_CUTPLANES_INFO[1]\n"
        "\t\t_lowerCutPlaneHeight = GLOB_CSTORY_ELEV - GLOB_HSTORY_ELEV + GLOB_CUTPLANES_INFO[3]\n"
        "\tendif\n"
        "endif\n\n");

    if (scanCount > 0) {
        fprintf (f,
            "dim _sg[]\n"
            "dim _sv[]\n"
            "dim _stTab[]\n"
            "_nSt = 0\n"
            "_reqRes = REQUEST(\"STORY_INFO\", \"\", _nSt, _stTab)\n\n"
            "for _i = 1 to iScanCount\n"
            "\t_sg[_i] = 0\n"
            "\tfor _j = 1 to _nSt\n"
            "\t\tif zScan[_i] >= _stTab[4*(_j-1)+3] then _sg[_i] = _j\n"
            "\tnext _j\n"
            "next _i\n\n"
            "! bGroup/bGroupPrev erweitern falls neue Geschosse hinzugekommen sind\n"
            "_oldBGSz = VARDIM1 (bGroup)\n"
            "if _oldBGSz < _nSt + 1 then\n"
            "\tdim _newBG[]\n"
            "\tdim _newBGP[]\n"
            "\tfor _k = 1 to _oldBGSz\n"
            "\t\t_newBG[_k] = bGroup[_k]\n"
            "\t\t_newBGP[_k] = bGroupPrev[_k]\n"
            "\tnext _k\n"
            "\tfor _k = _oldBGSz + 1 to _nSt + 1\n"
            "\t\t_newBG[_k] = 1\n"
            "\t\t_newBGP[_k] = 1\n"
            "\tnext _k\n"
            "\tPARAMETERS bGroup = _newBG, bGroupPrev = _newBGP\n"
            "endif\n\n"
            "! bGroup-Aenderung erkennen und bScan synchronisieren\n"
            "dim _bScanNew[]\n"
            "dim _bGPrevNew[]\n"
            "_needSync = 0\n"
            "for _i = 1 to iScanCount\n"
            "\t_bScanNew[_i] = bScan[_i]\n"
            "next _i\n"
            "for _k = 1 to VARDIM1 (bGroup)\n"
            "\t_bGPrevNew[_k] = bGroupPrev[_k]\n"
            "next _k\n"
            "for _i = 1 to iScanCount\n"
            "\t_bGIdx = _sg[_i] + 1\n"
            "\tif _bGIdx > VARDIM1 (bGroup) then _bGIdx = VARDIM1 (bGroup)\n"
            "\tif bGroupPrev[_bGIdx] = 1 and bGroup[_bGIdx] = 0 then\n"
            "\t\t_bScanNew[_i] = 0\n"
            "\t\t_bGPrevNew[_bGIdx] = 0\n"
            "\t\t_needSync = 1\n"
            "\tendif\n"
            "\tif bGroupPrev[_bGIdx] = 0 and bGroup[_bGIdx] = 1 then\n"
            "\t\t_bScanNew[_i] = 1\n"
            "\t\t_bGPrevNew[_bGIdx] = 1\n"
            "\t\t_needSync = 1\n"
            "\tendif\n"
            "next _i\n"
            "if _needSync = 1 then\n"
            "\tPARAMETERS bScan = _bScanNew, bGroupPrev = _bGPrevNew\n"
            "endif\n\n"
            "for _i = 1 to iScanCount\n"
            "\t_bGIdx = _sg[_i] + 1\n"
            "\tif _bGIdx > VARDIM1 (bGroup) then _bGIdx = VARDIM1 (bGroup)\n"
            "\t_sv[_i] = bGroup[_bGIdx] AND bScan[_i]\n"
            "next _i\n\n");
    }

    fprintf (f, "]]>\n</Script_1D>\n\n");

    // UI-Script
    fprintf (f, "<Script_UI SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "dim\tstUIValues[]\n"
        "\tstUIValues[1]\t= `Einheit und Masstab Einstellungen`\n"
        "\tstUIValues[2]\t= `Modell Einheit`\n"
        "\tstUIValues[3]\t= `Modellmastab X`\n"
        "\tstUIValues[4]\t= `Modellmastab Y`\n"
        "\tstUIValues[5]\t= `Modellmastab Z`\n"
        "\tstUIValues[6]\t= `Datei Bezeichnung`\n"
        "\tstUIValues[7]\t= `Punktewolke Datei wurde nicht gefunden`\n"
        "\tstUIValues[8]\t= `Uberschreibe Grundrissdarstellungsauswahl`\n"
        "\tstUIValues[9]\t= `Hohe obere Ebene`\n"
        "\tstUIValues[10]\t= `Hohe untere Ebene`\n\n"
        "outFldWdt\t= 180\n"
        "inFldWdt\t= 108\n"
        "buttonWdt\t= 20\n\n"
        "xPos1\t\t= 5\n"
        "xPos2\t\t= xPos1 + outFldWdt + 5\n"
        "xPos3\t\t= xPos2 + inFldWdt + 5\n\n"
        "yStart\t\t= 5\n"
        "diffY\t\t= 25\n\n"
        "outFldHgt\t= 15\n"
        "inFldHgt\t= 19\n"
        "buttonHgt\t= inFldHgt + 2 * diffY\n\n"
        "ui_dialog `Punktwolke`\n\n"
        "ui_current_page gs_ui_current_page\n\n"
        "ui_page 1, -1, `Darstellung`\n\n"
        "yCurr = yStart\n\n"
        "ui_style 0, 1\n"
        "ui_infield{3}\t\"bVisible\",\txPos1,\tyCurr-4,\t444-xPos1,\tinFldHgt,\n"
        "\t7, \"\", 1, 1,\n"
        "\t0, 0, 0, 0,\n"
        "\t1, `Punktwolke anzeigen`,\t0,\n"
        "\t1, `Punktwolke anzeigen`,\t1\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_infield{3}\t\"bShowNumbers\",\txPos1,\tyCurr-4,\t444-xPos1,\tinFldHgt,\n"
        "\t7, \"\", 1, 1,\n"
        "\t0, 0, 0, 0,\n"
        "\t1, `Standortnummern anzeigen`,\t0,\n"
        "\t1, `Standortnummern anzeigen`,\t1\n"
        "yCurr = yCurr + diffY + 2\n\n"
        "ui_separator\t0,\tyCurr,\t\t444,\tyCurr\n\n"
        "yCurr = yCurr + 9\n\n"
        "ui_style 0, 1\n"
        "ui_outfield\t\tstUIValues[1],\txPos1,\tyCurr,\t\t444-xPos1,\toutFldHgt\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_style 0, 0\n"
        "ui_outfield\t\tstUIValues[2],\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n"
        "ui_infield{3}\t\"iModelUnit\",\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt,\n"
        "\t8, \"\", 1, 1,\n"
        "\t0, 0, 0, 0,\n"
        "\t\"\",\t`Millimeter`,\tMODEL_UNIT_MM,\n"
        "\t\"\",\t`Zentimeter`,\tMODEL_UNIT_CM,\n"
        "\t\"\",\t`Meter`, \t\tMODEL_UNIT_M,\n"
        "\t\"\",\t`Inch`,\t\t\tMODEL_UNIT_IN,\n"
        "\t\"\",\t`Fuss`,\t\t\tMODEL_UNIT_FT\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_outfield\t\tstUIValues[3],\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n"
        "ui_infield{3}\t\"scaleX\",\t\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt\n\n"
        "ui_infield{3}\t\"bLinkScale\",\txPos3,\tyCurr-4,\tbuttonWdt,\tbuttonHgt,\n"
        "\t6, 1, 2, 1,\n"
        "\t8, 26, 18, 54,\n"
        "\t1, \"\", 0,\n"
        "\t2, \"\", 1\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_outfield\t\tstUIValues[4],\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n"
        "ui_infield{3}\t\"scaleY\",\t\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_outfield\t\tstUIValues[5],\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n"
        "ui_infield{3}\t\"scaleZ\",\t\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt\n\n"
        "yCurr = yCurr + diffY + 2\n\n"
        "ui_separator\t0,\tyCurr,\t\t444,\tyCurr\n\n"
        "yCurr = yCurr + 9\n\n"
        "ui_style 0, 1\n"
        "ui_infield{3}\t\"bOverrideCutplane\",\txPos1,\tyCurr-4,\t444-xPos1,\tinFldHgt,\n"
        "\t7, \"\", 1, 1,\n"
        "\t0, 0, 0, 0,\n"
        "\t1, stUIValues[8],\t0,\n"
        "\t1, stUIValues[8],\t1\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_style 0, 0\n\n"
        "_upperPlaneHeight = GLOB_CUTPLANES_INFO[1] + GLOB_ELEVATION\n"
        "_lowerPlaneHeight = GLOB_CUTPLANES_INFO[3] + GLOB_ELEVATION\n\n"
        "result = REQUEST(\"Working_length_unit\", \"\", workingUnits)\n\n"
        "if result < 1 then\n"
        "\tworkingUnits = \"%%.0mm\"\n"
        "endif\n\n"
        "_stUpperPlaneHeight = STR(workingUnits, _upperPlaneHeight)\n"
        "_stLowerPlaneHeight = STR(workingUnits, _lowerPlaneHeight)\n\n"
        "ui_outfield\t\tstUIValues[9],\t\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n\n"
        "if bOverrideCutplane then\n"
        "\tui_infield{3}\t\"upperPlaneHeight\",\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt\n"
        "else\n"
        "\tui_outfield\t_stUpperPlaneHeight,\txPos2,\tyCurr,\tinFldWdt,\toutFldHgt\n"
        "endif\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_outfield\t\tstUIValues[10],\t\txPos1,\tyCurr,\t\toutFldWdt,\toutFldHgt\n\n"
        "if bOverrideCutplane then\n"
        "\tui_infield{3}\t\"lowerPlaneHeight\",\txPos2,\tyCurr-4,\tinFldWdt,\tinFldHgt\n"
        "else\n"
        "\tui_outfield\t_stLowerPlaneHeight,\txPos2,\tyCurr,\tinFldWdt,\toutFldHgt\n"
        "endif\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_separator\t0,\tyCurr,\t\t444,\tyCurr\n\n"
        "yCurr = yCurr + 9\n\n"
        "ui_style 0, 0\n");
    fprintf (f, "ui_outfield\t\t`Quelldatei:`,\txPos1,\tyCurr,\toutFldWdt,\toutFldHgt\n");
    fprintf (f, "ui_outfield\t\tsE57Path,\txPos2,\tyCurr,\t444-xPos2,\toutFldHgt\n\n");
    fprintf (f, "yCurr = yCurr + diffY\n\n");

    if (scanCount > 0) {
        fprintf (f,
            "\nui_page 2, -1, `Standorte`\n\n"
            "yCurr = yStart\n\n"
            "ui_listfield 1, 5, yCurr, 420, 240, 0, `Standort`, `Sichtbar`\n\n"
            "_itemID = 1\n\n"
            "! Aktuelle Geschoss-Daten frisch lesen (unabhaengig vom Master Script)\n"
            "dim _uiStTab[]\n"
            "_uiNSt = 0\n"
            "_reqUI = REQUEST(\"STORY_INFO\", \"\", _uiNSt, _uiStTab)\n\n"
            "! Scan-Geschoss-Zuordnung in UI-Scope neu berechnen\n"
            "dim _uiSg[]\n"
            "for _i = 1 to iScanCount\n"
            "\t_uiSg[_i] = 0\n"
            "\tfor _j = 1 to _uiNSt\n"
            "\t\tif zScan[_i] >= _uiStTab[4*(_j-1)+3] then _uiSg[_i] = _j\n"
            "\tnext _j\n"
            "next _i\n\n"
            "! Geschosse absteigend (oberstes zuerst)\n"
            "for _j = _uiNSt to 1 step -1\n"
            "\t_stNum = _uiStTab[4*(_j-1)+1]\n"
            "\t_stNam = _uiStTab[4*(_j-1)+2] + ``\n"
            "\tif STRLEN (_stNam) = 0 then\n"
            "\t\t_stNam = `Geschoss`\n"
            "\tendif\n"
            "\t_stLabel = STR(_stNum, 1, 0) + `  |  ` + _stNam\n"
            "\t_bGIdx = _j + 1\n"
            "\tif _bGIdx > VARDIM1 (bGroup) then _bGIdx = VARDIM1 (bGroup)\n"
            "\tui_listitem{2} _itemID, 1, bGroup[_bGIdx], 0, \"\", _stLabel\n"
            "\t_itemID = _itemID + 1\n"
            "\tfor _i = 1 to iScanCount\n"
            "\t\tif _uiSg[_i] = _j then\n"
            "\t\t\tui_listitem{2} _itemID, 1, bScan[_i], 1, \"\", `        Standort ` + STR(_i, 1, 0)\n"
            "\t\t\t_itemID = _itemID + 1\n"
            "\t\tendif\n"
            "\tnext _i\n"
            "next _j\n\n"
            "! Unter Unterkante\n"
            "ui_listitem{2} _itemID, 1, bGroup[1], 0, \"\", `Unter Unterkante`\n"
            "_itemID = _itemID + 1\n"
            "for _i = 1 to iScanCount\n"
            "\tif _uiSg[_i] = 0 then\n"
            "\t\tui_listitem{2} _itemID, 1, bScan[_i], 1, \"\", `        Standort ` + STR(_i, 1, 0)\n"
            "\t\t_itemID = _itemID + 1\n"
            "\tendif\n"
            "next _i\n\n");
    }

    fprintf (f, "]]>\n</Script_UI>\n\n");

    // VL-Script
    fprintf (f, "<Script_VL SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "hideparameter all \"A\", \"B\", \"ZZYZX\"\n\n"
        "_originalUnitX = %.8g\n"
        "_originalUnitY = %.8g\n"
        "_originalUnitZ = %.8g\n\n"
        "A\t\t= _scaleOriginalSize * _originalUnitX * scaleX\n"
        "B\t\t= _scaleOriginalSize * _originalUnitY * scaleY\n"
        "ZZYZX\t= _scaleOriginalSize * _originalUnitZ * scaleZ\n\n"
        "ac_bottomlevel\t= 0\n"
        "ac_toplevel\t\t= ZZYZX\n\n"
        "parameters\tA\t\t= A,\n"
        "\t\t\t\tB\t\t= B,\n"
        "\t\t\t\tZZYZX\t= ZZYZX,\n"
        "\t\t\t\tac_bottomlevel\t= ac_bottomlevel,\n"
        "\t\t\t\tac_toplevel\t\t= ac_toplevel\n\n"
        "lock \"A\", \"B\", \"ZZYZX\"\n"
        "lock \"iDensityPercent\"\n"
        "lock \"iTotalPoints\"\n"
        "lock \"iScanCount\"\n\n"
        "values \"lowerPlaneHeight\" range [ ,upperPlaneHeight)\n"
        "values \"upperPlaneHeight\" range [lowerPlaneHeight ,)\n\n"
        "if not(bOverrideCutplane) then\n"
        "\tlock \"upperPlaneHeight\", \"lowerPlaneHeight\"\n"
        "endif\n\n"
        "values \"scaleX\" range (0, ]\n"
        "values \"scaleY\" range (0, ]\n"
        "values \"scaleZ\" range (0, ]\n\n"
        "values{2} \"iModelUnit\"\tMODEL_UNIT_M,\t`Meter`,\n"
        "\t\t\t\t\t\tMODEL_UNIT_CM,\t`Zentimeter`,\n"
        "\t\t\t\t\t\tMODEL_UNIT_MM,\t`Millimeter`,\n"
        "\t\t\t\t\t\tMODEL_UNIT_FT,\t`Fuss`,\n"
        "\t\t\t\t\t\tMODEL_UNIT_IN,\t`Inch`\n\n"
        "if not(bLinkScale) then\n"
        "\tscaleRatioXY = scaleX / scaleY\n"
        "\tparameters\tscaleRatioXY = scaleRatioXY\n\n"
        "\tscaleRatioYZ = scaleY / scaleZ\n"
        "\tparameters\tscaleRatioYZ = scaleRatioYZ\n\n"
        "\tscaleRatioXZ = scaleX / scaleZ\n"
        "\tparameters\tscaleRatioXZ = scaleRatioXZ\n"
        "else\n"
        "\tif\tGLOB_MODPAR_NAME = \"scaleX\" |\\\n"
        "\t\tGLOB_MODPAR_NAME = \"scaleY\" |\\\n"
        "\t\tGLOB_MODPAR_NAME = \"scaleZ\" then\n"
        "\t\tif GLOB_MODPAR_NAME = \"scaleX\" then\n"
        "\t\t\tscaleY\t= scaleX / scaleRatioXY\n"
        "\t\t\tparameters\tscaleY = scaleY\n"
        "\t\t\tscaleZ\t= scaleX / scaleRatioXZ\n"
        "\t\t\tparameters\tscaleZ = scaleZ\n"
        "\t\telse\n"
        "\t\t\tif GLOB_MODPAR_NAME = \"scaleY\" then\n"
        "\t\t\t\tscaleX\t= scaleY * scaleRatioXY\n"
        "\t\t\t\tparameters\tscaleX = scaleX\n"
        "\t\t\t\tscaleZ\t= scaleY / scaleRatioYZ\n"
        "\t\t\t\tparameters\tscaleZ = scaleZ\n"
        "\t\t\telse\n"
        "\t\t\t\tscaleX\t= scaleZ * scaleRatioXZ\n"
        "\t\t\t\tparameters\tscaleX = scaleX\n"
        "\t\t\t\tscaleY\t= scaleZ * scaleRatioYZ\n"
        "\t\t\t\tparameters\tscaleY = scaleY\n"
        "\t\t\tendif\n"
        "\t\tendif\n"
        "\telse\n"
        "\t\tscaleRatioXY = scaleX / scaleY\n"
        "\t\tparameters\tscaleRatioXY = scaleRatioXY\n"
        "\t\tscaleRatioYZ = scaleY / scaleZ\n"
        "\t\tparameters\tscaleRatioYZ = scaleRatioYZ\n"
        "\t\tscaleRatioXZ = scaleX / scaleZ\n"
        "\t\tparameters\tscaleRatioXZ = scaleRatioXZ\n"
        "\tendif\n"
        "endif\n\n"
        "scalingUnitX\t= scaleX * _scaleOriginalSize\n"
        "scalingUnitY\t= scaleY * _scaleOriginalSize\n"
        "scalingUnitZ\t= scaleZ * _scaleOriginalSize\n\n"
        "parameters\tscalingUnitX\t= scalingUnitX,\n"
        "\t\t\t\tscalingUnitY\t= scalingUnitY,\n"
        "\t\t\t\tscalingUnitZ\t= scalingUnitZ\n",
        (double)origX, (double)origY, (double)origZ);
    fprintf (f, "]]>\n</Script_VL>\n\n");

    // Parameter-Sektion
    fprintf (f,
        "<ParamSection SectVersion=\"27\" SectionFlags=\"0\" SubIdent=\"0\">\n"
        "\t<ParamSectHeader>\n"
        "\t\t<AutoHotspots>false</AutoHotspots>\n"
        "\t\t<StatBits><STBit_UIDefault/><STBit_FixSize/><STBit_UIUseHierarchicalPages/></StatBits>\n"
        "\t\t<WDLeftFrame>0</WDLeftFrame><WDRightFrame>0</WDRightFrame>\n"
        "\t\t<WDTopFrame>0</WDTopFrame><WDBotFrame>0</WDBotFrame>\n"
        "\t\t<LayFlags>65535</LayFlags>\n"
        "\t\t<WDMirrorThickness>0</WDMirrorThickness>\n"
        "\t\t<WDWallInset>0</WDWallInset>\n"
        "\t</ParamSectHeader>\n"
        "\t<Parameters>\n");

    fprintf (f, "\t\t<Length Name=\"A\"><Description><![CDATA[\"Dimension 1\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origX);
    fprintf (f, "\t\t<Length Name=\"B\"><Description><![CDATA[\"Dimension 2\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origY);
    fprintf (f, "\t\t<Length Name=\"ZZYZX\"><Description><![CDATA[\"Height\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf (f, "\t\t<Boolean Name=\"AC_show2DHotspotsIn3D\"><Description><![CDATA[\"Show 2D Hotspots in 3D\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Length Name=\"ac_bottomlevel\"><Description><![CDATA[\"Bottom Level\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"ac_toplevel\"><Description><![CDATA[\"Top Level\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf (f, "\t\t<Boolean Name=\"bProject2D\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Title Name=\"tRep3D\"><Description><![CDATA[\"3D Representation\"]]></Description><Fix/></Title>\n");
    fprintf (f, "\t\t<Integer Name=\"iModelUnit\"><Description><![CDATA[\"Basic Model Unit\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></Integer>\n");
    fprintf (f, "\t\t<RealNum Name=\"offsetX\"><Description><![CDATA[\"X Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offX);
    fprintf (f, "\t\t<RealNum Name=\"offsetY\"><Description><![CDATA[\"Y Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offY);
    fprintf (f, "\t\t<RealNum Name=\"offsetZ\"><Description><![CDATA[\"Z Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offZ);
    fprintf (f, "\t\t<Length Name=\"scalingUnitX\"><Description><![CDATA[\"Scaling Unit Length X\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"scalingUnitY\"><Description><![CDATA[\"Scaling Unit Length Y\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"scalingUnitZ\"><Description><![CDATA[\"Scaling Unit Length Z\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf (f, "\t\t<Boolean Name=\"bLinkScale\"><Description><![CDATA[\"Link Scale\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></Boolean>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleX\"><Description><![CDATA[\"Scale of X Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleY\"><Description><![CDATA[\"Scale of Y Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleZ\"><Description><![CDATA[\"Scale of Z Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleRatioXY\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleRatioXZ\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<RealNum Name=\"scaleRatioYZ\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf (f, "\t\t<Boolean Name=\"bOverrideCutplane\"><Description><![CDATA[\"Override Floor Plan Cut Plane Height\"]]></Description>"
               "<Fix/><Flags><ParFlg_BoldName/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Length Name=\"upperPlaneHeight\"><Description><![CDATA[\"Upper Cutplane Height to Current Story\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1.1</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"tempUpperPlaneHeight\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1.1</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"lowerPlaneHeight\"><Description><![CDATA[\"Lower Cutplane Height to Current Story\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>0</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"tempLowerPlaneHeight\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>0</Value></Length>\n");
    fprintf (f, "\t\t<Boolean Name=\"bVisible\"><Description><![CDATA[\"Punktwolke anzeigen\"]]></Description>"
               "<Fix/><Value>1</Value></Boolean>\n");
    fprintf (f, "\t\t<Boolean Name=\"bShowNumbers\"><Description><![CDATA[\"Standortnummern anzeigen\"]]></Description>"
               "<Fix/><Value>%d</Value></Boolean>\n", scanCount > 0 ? 1 : 0);
    fprintf (f, "\t\t<Integer Name=\"iDensityPercent\"><Description><![CDATA[\"Importierte Dichte (%%)\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%d</Value></Integer>\n", densityPct);
    fprintf (f, "\t\t<Integer Name=\"iTotalPoints\"><Description><![CDATA[\"Gesamtpunkte\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%lld</Value></Integer>\n", (long long)totalPoints);
    fprintf (f, "\t\t<String Name=\"sE57Path\"><Description><![CDATA[\"E57 Quelldatei\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags>"
               "<Value><![CDATA[\"%s\"]]></Value></String>\n", e57PathStr ? e57PathStr : "");
    fprintf (f, "\t\t<Integer Name=\"iScanCount\"><Description><![CDATA[\"Anzahl Scan-Standorte\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%d</Value></Integer>\n", scanCount);
    fprintf (f, "\t\t<Integer Name=\"gs_ui_current_page\"><Description><![CDATA[\"Aktive Seite\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>1</Value></Integer>\n");

    if (scanCount > 0) {
        fprintf (f, "\t\t<Boolean Name=\"bScan\"><Description><![CDATA[\"Standorte anzeigen\"]]></Description>\n"
                   "\t\t\t<Fix/>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", scanCount);
        for (int sc = 0; sc < scanCount; ++sc)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">1</AVal>\n", sc + 1);
        fprintf (f, "\t\t\t</ArrayValues>\n"
                   "\t\t</Boolean>\n");

        fprintf (f, "\t\t<RealNum Name=\"xScan\"><Description><![CDATA[\"Scanner-Position X\"]]></Description>\n"
                   "\t\t\t<Fix/><Flags><ParFlg_Hidden/></Flags>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", scanCount);
        for (int sc = 0; sc < scanCount; ++sc)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">%.8g</AVal>\n", sc + 1, scanTx ? scanTx[sc] : 0.0);
        fprintf (f, "\t\t\t</ArrayValues>\n\t\t</RealNum>\n");

        fprintf (f, "\t\t<RealNum Name=\"yScan\"><Description><![CDATA[\"Scanner-Position Y\"]]></Description>\n"
                   "\t\t\t<Fix/><Flags><ParFlg_Hidden/></Flags>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", scanCount);
        for (int sc = 0; sc < scanCount; ++sc)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">%.8g</AVal>\n", sc + 1, scanTy ? scanTy[sc] : 0.0);
        fprintf (f, "\t\t\t</ArrayValues>\n\t\t</RealNum>\n");

        fprintf (f, "\t\t<RealNum Name=\"zScan\"><Description><![CDATA[\"Scanner-Position Z\"]]></Description>\n"
                   "\t\t\t<Fix/><Flags><ParFlg_Hidden/></Flags>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", scanCount);
        for (int sc = 0; sc < scanCount; ++sc)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">%.8g</AVal>\n", sc + 1, scanTz ? scanTz[sc] : 0.0);
        fprintf (f, "\t\t\t</ArrayValues>\n\t\t</RealNum>\n");

        int bGroupSize = storyCount + 1;
        fprintf (f, "\t\t<Boolean Name=\"bGroup\"><Description><![CDATA[\"Gruppe anzeigen\"]]></Description>\n"
                   "\t\t\t<Fix/>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", bGroupSize);
        for (int i = 1; i <= bGroupSize; ++i)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">1</AVal>\n", i);
        fprintf (f, "\t\t\t</ArrayValues>\n\t\t</Boolean>\n");

        fprintf (f, "\t\t<Boolean Name=\"bGroupPrev\"><Description><![CDATA[\"Gruppe Vorwert\"]]></Description>\n"
                   "\t\t\t<Fix/><Flags><ParFlg_Hidden/></Flags>\n"
                   "\t\t\t<ArrayValues FirstDimension=\"%d\" SecondDimension=\"0\">\n", bGroupSize);
        for (int i = 1; i <= bGroupSize; ++i)
            fprintf (f, "\t\t\t\t<AVal Row=\"%d\">1</AVal>\n", i);
        fprintf (f, "\t\t\t</ArrayValues>\n\t\t</Boolean>\n");
    }

    fprintf (f, "\t</Parameters>\n</ParamSection>\n\n</Symbol>\n");
    fclose (f);

    char gsmPath[1024];
    snprintf (gsmPath, sizeof (gsmPath), "%s%c%s.gsm", outputDirPath, PATH_SEP, baseName);

#ifdef _WIN32
    char tempDir[MAX_PATH] = "C:\\Temp";
    GetTempPathA (MAX_PATH, tempDir);
    char lpcOut[MAX_PATH];
    snprintf (lpcOut, sizeof (lpcOut), "%spw_lpc_%s.txt", tempDir, baseName);
#else
    char lpcOut[256];
    snprintf (lpcOut, sizeof (lpcOut), "/tmp/pw_lpc_%s.txt", baseName);
#endif

    bool ok = false;

#ifdef _WIN32
    char cmdLine[4096];
    snprintf (cmdLine, sizeof (cmdLine), "\"%s\" xml2libpart \"%s\" \"%s\"",
              LP_XMLCONVERTER, xmlPath, gsmPath);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = CreateFileA (lpcOut,
        GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si = {};
    si.cb         = sizeof (si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError  = hOut;
    si.hStdInput  = GetStdHandle (STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (CreateProcessA (nullptr, cmdLine, nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject (pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess (pi.hProcess, &exitCode);
        ok = (exitCode == 0);
        CloseHandle (pi.hProcess);
        CloseHandle (pi.hThread);
        char buf[256];
        snprintf (buf, sizeof (buf), "LP_XMLConverter exit=%lu xml=%s gsm=%s",
                  (unsigned long)exitCode, xmlPath, gsmPath);
        Log (buf);
    }
    else
    {
        char buf[256];
        snprintf (buf, sizeof (buf), "CreateProcess fehlgeschlagen: %lu", (unsigned long)GetLastError ());
        Log (buf);
    }
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle (hOut);
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init (&fa);
    posix_spawn_file_actions_addopen (&fa, STDOUT_FILENO, lpcOut,
                                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2 (&fa, STDOUT_FILENO, STDERR_FILENO);

    const char* argv[] = { LP_XMLCONVERTER, "xml2libpart", xmlPath, gsmPath, nullptr };
    extern char** environ;
    pid_t pid;
    int spawnRet = posix_spawn (&pid, LP_XMLCONVERTER, &fa, nullptr,
                                (char* const*)argv, environ);
    posix_spawn_file_actions_destroy (&fa);

    if (spawnRet == 0) {
        int status = 0;
        waitpid (pid, &status, 0);
        int exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
        ok = (exitCode == 0);
        char buf[256];
        snprintf (buf, sizeof (buf), "LP_XMLConverter exit=%d xml=%s gsm=%s",
                  exitCode, xmlPath, gsmPath);
        Log (buf);
    } else {
        char buf[256];
        snprintf (buf, sizeof (buf), "posix_spawn fehlgeschlagen: %d", spawnRet);
        Log (buf);
    }
#endif

    if (FILE* lf = fopen (lpcOut, "r")) {
        char line[512];
        while (fgets (line, sizeof (line), lf)) {
            size_t len = strlen (line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            Log (line);
        }
        fclose (lf);
    }

    remove (lpcOut);
    if (ok)
        remove (xmlPath);
    return ok;
}

// --- GSPC-Datei schreiben ----------------------------------------------------

struct GspcWriter {
    FILE*  fp      = nullptr;
    float  xmin = 0, xmax = 0, ymin = 0, ymax = 0, zmin = 0, zmax = 0;
    bool   firstPt = true;

    bool Open (const char* path)
    {
        fp = fopen (path, "wb");
        if (!fp) return false;
        const char magic[4] = {'G', 'S', 'P', 'C'};
        uint32_t ver  = 2;
        float    bbox[6] = {};
        fwrite (magic, 1, 4, fp);
        fwrite (&ver,  4, 1, fp);
        fwrite (bbox,  4, 6, fp);
        return true;
    }

    void Write (float x, float y, float z, uint8_t r, uint8_t g, uint8_t b)
    {
        if (firstPt) {
            xmin = xmax = x; ymin = ymax = y; zmin = zmax = z;
            firstPt = false;
        } else {
            if (x < xmin) xmin = x; if (x > xmax) xmax = x;
            if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            if (z < zmin) zmin = z; if (z > zmax) zmax = z;
        }
        fwrite (&x, 4, 1, fp);
        fwrite (&y, 4, 1, fp);
        fwrite (&z, 4, 1, fp);
        fwrite (&r, 1, 1, fp);
        fwrite (&g, 1, 1, fp);
        fwrite (&b, 1, 1, fp);
    }

    bool Close ()
    {
        if (!fp) return false;
        fseek (fp, 8, SEEK_SET);
        float bbox[6] = {xmin, xmax, ymin, ymax, zmin, zmax};
        fwrite (bbox, 4, 6, fp);
        fclose (fp);
        fp = nullptr;
        return true;
    }
};

// --- Quaternion-Pose auf Punkt anwenden --------------------------------------

static void ApplyPose (const e57::RigidBodyTransform& pose,
                       double lx, double ly, double lz,
                       double& wx, double& wy, double& wz)
{
    double qw = pose.rotation.w, qx = pose.rotation.x;
    double qy = pose.rotation.y, qz = pose.rotation.z;
    double cx  = qy*lz - qz*ly,  cy  = qz*lx - qx*lz,  cz  = qx*ly - qy*lx;
    double ccx = qy*cz - qz*cy,  ccy = qz*cx - qx*cz,  ccz = qx*cy - qy*cx;
    wx = lx + 2.0*(qw*cx + ccx) + pose.translation.x;
    wy = ly + 2.0*(qw*cy + ccy) + pose.translation.y;
    wz = lz + 2.0*(qw*cz + ccz) + pose.translation.z;
}


// =============================================================================
// Bibliotheks-Hilfsfunktionen
// =============================================================================

static void RegisterLibraryDir (const IO::Location& outputDir)
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries (&allLibs, &embIdx);

    GS::UniString newPath;
    outputDir.ToPath (&newPath);

    GS::Array<API_LibraryInfo> userLibs;
    bool found = false;
    for (UInt32 i = 0; i < allLibs.GetSize (); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        GS::UniString p;
        allLibs[i].location.ToPath (&p);
        if (p == newPath) found = true;
        userLibs.Push (allLibs[i]);
    }
    if (!found) {
        API_LibraryInfo entry = {};
        entry.location    = outputDir;
        entry.libraryType = API_LocalLibrary;
        userLibs.Push (entry);
    }
    Compat_SetLibraries (&userLibs);
}

static void UnregisterLibraryDir (const GS::UniString& outputDirPath)
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries (&allLibs, &embIdx);

    GS::Array<API_LibraryInfo> userLibs;
    for (UInt32 i = 0; i < allLibs.GetSize (); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        GS::UniString p;
        allLibs[i].location.ToPath (&p);
        if (p != outputDirPath) userLibs.Push (allLibs[i]);
    }
    Compat_SetLibraries (&userLibs);
}

static void ReloadLibraries ()
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries (&allLibs, &embIdx);

    GS::Array<API_LibraryInfo> userLibs;
    for (UInt32 i = 0; i < allLibs.GetSize (); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        userLibs.Push (allLibs[i]);
    }
    Compat_SetLibraries (&userLibs);
    Compat_ReloadLibraries ();
}

// --- Parameter in platzierten Instanzen aktualisieren ------------------------

static int UpdatePlacedParams (Int32 libIdx, int newDensity, int64_t writtenPoints)
{
    double lpA, lpB;
    Int32 parNum = 0;
    API_AddParType** lpPars = nullptr;
    ACAPI_LibPart_GetParams (libIdx, &lpA, &lpB, &parNum, &lpPars);
    ACAPI_DisposeAddParHdl (&lpPars);

    GS::Array<API_Guid> elemList;
    ACAPI_Element_GetElemList (API_ObjectID, &elemList);

    GS::Array<API_Guid> toUpdate;
    for (const auto& guid : elemList) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get (&elem) != NoError) continue;
        if (elem.object.libInd == libIdx)
            toUpdate.Push (guid);
    }

    int updCount = 0;
    if (toUpdate.GetSize () > 0) {
        ACAPI_CallUndoableCommand ("Punktwolkendichte anpassen",
            [&] () -> GSErrCode {
                for (auto guid : toUpdate) {
                    API_ElementMemo memo = {};
                    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) != NoError) {
                        ACAPI_DisposeElemMemoHdls (&memo);
                        continue;
                    }
                    if (memo.params != nullptr) {
                        for (Int32 j = 0; j < parNum; ++j) {
                            const char* pn = (*memo.params)[j].name;
                            if (strcmp (pn, "iDensityPercent") == 0)
                                (*memo.params)[j].value.real = (double)newDensity;
                        }
                    }
                    if (ACAPI_Element_ChangeMemo (guid, APIMemoMask_AddPars, &memo) == NoError)
                        ++updCount;
                    ACAPI_DisposeElemMemoHdls (&memo);
                }
                return NoError;
            });
    }
    return updCount;
}

// =============================================================================
// Öffentliche Operationen
// =============================================================================

// --- CollectOurLibParts ------------------------------------------------------

GS::Array<PointCloudEntry> CollectOurLibParts ()
{
    GS::Array<PointCloudEntry> result;
    Int32 count = 0;
    if (ACAPI_LibPart_GetNum (&count) != NoError) {
        Log ("CollectOurLibParts: GetNum fehlgeschlagen");
        return result;
    }

    for (Int32 i = 1; i <= count; ++i) {
        API_LibPart lp = {};
        lp.index = i;
        if (ACAPI_LibPart_Get (&lp) != NoError) continue;

        double a = 0, b = 0;
        Int32 parNum = 0;
        API_AddParType** pars = nullptr;
        if (ACAPI_LibPart_GetParams (i, &a, &b, &parNum, &pars) != NoError) continue;

        GS::UniString e57Path;
        int density   = -1;
        int scanCount =  0;

        for (Int32 j = 0; j < parNum; ++j) {
            const API_AddParType& p = (*pars)[j];
            if (strcmp (p.name, "sE57Path") == 0 && p.typeID == APIParT_CString) {
                char val[API_UAddParStrLen + 1] = {};
                for (int k = 0; k < API_UAddParStrLen && p.value.uStr[k]; ++k)
                    val[k] = (char)(p.value.uStr[k] & 0xFF);
                e57Path = GS::UniString (val);
            }
            if (strcmp (p.name, "iDensityPercent") == 0)
                density = (int)(p.value.real + 0.5);
            if (strcmp (p.name, "iScanCount") == 0)
                scanCount = (int)(p.value.real + 0.5);
        }
        ACAPI_DisposeAddParHdl (&pars);

        if (e57Path.GetLength () == 0 || density <= 0) continue;

        PointCloudEntry entry;
        char nameBuf[API_UniLongNameLen + 1] = {};
        for (int k = 0; k < API_UniLongNameLen && lp.docu_UName[k]; ++k)
            nameBuf[k] = (char)(lp.docu_UName[k] & 0xFF);
        entry.name = GS::UniString (nameBuf);
        if (entry.name.GetLength () > 4) {
            GS::UniString ext = entry.name.GetSubstring (entry.name.GetLength () - 4, 4);
            ext.SetToLowerCase ();
            if (ext == ".gsm")
                entry.name = entry.name.GetSubstring (0, entry.name.GetLength () - 4);
        }
        entry.densityPercent = density;
        entry.e57Path        = e57Path;
        entry.scanCount      = scanCount;
        entry.libIndex       = i;

        UInt32 sl = FindLastSep (e57Path);
        GS::UniString e57Dir = (sl != GS::MaxUIndex)
            ? e57Path.GetSubstring (0, sl) : e57Path;
        IO::Location outputDirLoc (e57Dir);
        outputDirLoc.AppendToLocal (IO::Name (entry.name));
        outputDirLoc.ToPath (&entry.outputDir);

        result.Push (entry);
    }
    return result;
}

// --- AutoPlaceEntry ----------------------------------------------------------

void AutoPlaceEntry (const PointCloudEntry& entry)
{
    Int32 libIdx = entry.libIndex;
    char logBuf[128];
    snprintf (logBuf, sizeof (logBuf), "AutoPlaceEntry: libIdx=%d name=%s",
              libIdx, entry.name.ToCStr ().Get ());
    Log (logBuf);

    API_StoryInfo storyInfo = {};
    Compat_GetStorySettings (&storyInfo);
    short curFloor = storyInfo.actStory;

    API_Element     placeElem = {};
    API_ElementMemo placeMemo = {};
    placeElem.header.type = API_ObjectID;

    GSErrCode defErr = ACAPI_Element_GetDefaults (&placeElem, &placeMemo);
    if (defErr != NoError) {
        ACAPI_DisposeElemMemoHdls (&placeMemo);
        snprintf (logBuf, sizeof (logBuf), "GetDefaults: err=%d", defErr);
        Log (logBuf);
        return;
    }

    API_ParamOwnerType paramOwner = {};
    paramOwner.libInd = libIdx;
    if (Compat_OpenParameters (&paramOwner) == NoError) {
        API_GetParamsType getParams = {};
        if (Compat_GetActParameters (&getParams) == NoError) {
            ACAPI_DisposeAddParHdl (&placeMemo.params);
            placeMemo.params = getParams.params;
        }
        Compat_CloseParameters ();
    }

    placeElem.header.floorInd = curFloor;
    placeElem.object.libInd   = libIdx;
    placeElem.object.pos.x    = 0.0;
    placeElem.object.pos.y    = 0.0;
    placeElem.object.angle    = 0.0;

    GSErrCode createErr = NoError;
    ACAPI_CallUndoableCommand ("Punktwolke platzieren",
        [&] () -> GSErrCode {
            createErr = ACAPI_Element_Create (&placeElem, &placeMemo);
            return createErr;
        });
    snprintf (logBuf, sizeof (logBuf), "Element_Create: err=%d", createErr);
    Log (logBuf);
    ACAPI_DisposeElemMemoHdls (&placeMemo);
}

// --- DoImport ----------------------------------------------------------------

GS::UniString DoImport (int densityPct, const GS::UniString* forceE57Path)
{
    Log ("--- DoImport ---");
    const int STRIDE = std::max (1, 100 / densityPct);

    GS::UniString e57Path;

    if (forceE57Path != nullptr) {
        e57Path = *forceE57Path;
    } else {
        DGTypePopupItem filter;
        filter.Init ();
        filter.text       = "E57-Punktwolke";
        filter.extensions = "e57";

        IO::Location e57Loc;
        if (!DGGetOpenFile (&e57Loc, 1, &filter, nullptr, GS::UniString ("E57-Datei öffnen")))
            return GS::UniString ();
        e57Loc.ToPath (&e57Path);
    }
    { char buf[512]; snprintf (buf, sizeof (buf), "E57: %s", e57Path.ToCStr ().Get ()); Log (buf); }

    // Dateiname aus Pfad extrahieren
    IO::Location e57LocForName (e57Path);
    IO::Name e57FileName;
    e57LocForName.GetLastLocalName (&e57FileName);
    GS::UniString baseName = e57FileName.ToString ();
    if (baseName.GetLength () > 4) {
        GS::UniString ext = baseName.GetSubstring (baseName.GetLength () - 4, 4);
        ext.SetToLowerCase ();
        if (ext == ".e57")
            baseName = baseName.GetSubstring (0, baseName.GetLength () - 4);
    }

    UInt32 lastSep = FindLastSep (e57Path);
    GS::UniString parentPathStr = (lastSep != GS::MaxUIndex)
        ? e57Path.GetSubstring (0, lastSep) : e57Path;

    IO::Location outputDir (parentPathStr);
    outputDir.AppendToLocal (IO::Name (baseName));
    IO::fileSystem.CreateFolder (outputDir);

    GS::UniString outputDirPath;
    outputDir.ToPath (&outputDirPath);

    GS::UniString dirCode = DensityDirName (densityPct);

    // GSPC dieser Auflösung bereits vorhanden?
    // → E57-Parsing überspringen, aber GSM mit gespeicherten Parametern neu erzeugen
    {
        char checkPath[1024];
        snprintf (checkPath, sizeof (checkPath), "%s%c%s_%s_1.gspc",
                  outputDirPath.ToCStr ().Get (), PATH_SEP,
                  baseName.ToCStr ().Get (), dirCode.ToCStr ().Get ());
        if (access (checkPath, F_OK) == 0) {
            Log ("GSPC vorhanden, lese PwMeta fuer GSM-Regenerierung.");
            PwMeta m = {};
            std::vector<double> tx, ty, tz;
            std::string metaE57;
            if (ReadPwMeta (outputDirPath.ToCStr ().Get (),
                             baseName.ToCStr ().Get (), m, tx, ty, tz, metaE57)) {
                bool ok = WriteGsmViaXml (
                    (int)m.scanCount, baseName.ToCStr ().Get (),
                    outputDirPath.ToCStr ().Get (),
                    m.origX, m.origY, m.origZ,
                    m.offX,  m.offY,  m.offZ,
                    densityPct,
                    metaE57.empty () ? e57Path.ToCStr ().Get () : metaE57.c_str (),
                    m.totalPoints,
                    tx.data (), ty.data (), tz.data (),
                    (int)m.storyCount);
                if (ok) {
                    RegisterLibraryDir (outputDir);
                    Compat_ReloadLibraries ();
                    GS::UniString msg = GS::UniString ("\"") + baseName;
                    msg += GS::UniString ("\" - Aufloesung ");
                    msg += GS::UniString (std::to_string (densityPct).c_str ());
                    msg += GS::UniString ("% geladen (GSPC bereits vorhanden).");
                    ACAPI_WriteReport (msg.ToCStr ().Get (), false);
                    return baseName;
                }
            }
            Log ("PwMeta nicht lesbar, starte vollen Import.");
        }
    }

    float   bboxX = 1, bboxY = 1, bboxZ = 1;
    float   offX  = 0, offY  = 0, offZ  = 0;
    int64_t totalOrigPoints = 0;
    int64_t nScans = 0;
    bool    progressOpen = false;
    std::vector<double> scanTx, scanTy, scanTz;

    struct StoryEntry { double level; char name[256]; };
    std::vector<StoryEntry> stories;
    {
        API_StoryInfo si = {};
        if (Compat_GetStorySettings (&si) == NoError && si.data != nullptr) {
            for (short i = si.firstStory; i <= si.lastStory; ++i) {
                const API_StoryType& s = (*si.data)[i - si.firstStory];
                StoryEntry e;
                e.level = s.level;
                memset (e.name, 0, sizeof (e.name));
                for (int k = 0; k < 255 && s.uName[k]; ++k)
                    e.name[k] = (char)(s.uName[k] & 0xFF);
                stories.push_back (e);
            }
            BMKillHandle ((GSHandle*)&si.data);
            std::sort (stories.begin (), stories.end (),
                [] (const StoryEntry& a, const StoryEntry& b) { return a.level < b.level; });
        }
    }

    try {
        e57::Reader reader (std::string (e57Path.ToCStr ().Get ()), e57::ReaderOptions{});
        if (!reader.IsOpen ()) {
            ACAPI_WriteReport ("E57-Datei konnte nicht geöffnet werden.", true);
            return GS::UniString ();
        }
        nScans = reader.GetData3DCount ();
        if (nScans == 0) {
            ACAPI_WriteReport ("E57-Datei enthält keine Scans.", true);
            return GS::UniString ();
        }

        int64_t totalExpected = 0;
        std::vector<int64_t> scanExpected (nScans, 500000LL);
        for (int64_t si2 = 0; si2 < nScans; ++si2) {
            e57::Data3D hdr2;
            if (reader.ReadData3D (si2, hdr2) && hdr2.pointCount > 0) {
                totalOrigPoints  += hdr2.pointCount;
                scanExpected[si2] = std::max<int64_t> (1, hdr2.pointCount / STRIDE);
                totalExpected    += scanExpected[si2];
            }
        }
        if (totalExpected <= 0) totalExpected = nScans * 500000LL;

        Int32 nPhases = (Int32)(nScans + 1);
        GS::UniString procTitle ("E57 Import");
        Compat_InitProcessWindow (&procTitle, &nPhases);
        progressOpen = true;

        const size_t CHUNK = 65536;
        std::vector<double>   x (CHUNK), y (CHUNK), z (CHUNK);
        std::vector<uint16_t> cr (CHUNK, 200), cg (CHUNK, 200), cb (CHUNK, 200);
        std::vector<int8_t>   colorInvalid (CHUNK, 0);
        bool cancelled = false;

        float gxmin =  1e30f, gxmax = -1e30f;
        float gymin =  1e30f, gymax = -1e30f;
        float gzmin =  1e30f, gzmax = -1e30f;

        for (int64_t si = 0; si < nScans && !cancelled; ++si) {
            {
                Int32 ptScan = (Int32)std::min (scanExpected[si], (int64_t)2000000000LL);
                char phaseStr[64];
                snprintf (phaseStr, sizeof (phaseStr), "Standort %lld / %lld",
                          (long long)(si + 1), (long long)nScans);
                GS::UniString phaseN (phaseStr);
                Compat_SetNextProcessPhase (&phaseN, &ptScan);
            }
            Int32   scanUpdateStep = (Int32)std::max ((int64_t)1, scanExpected[si] / 200);
            int64_t scanWritten    = 0;

            e57::Data3D header;
            if (!reader.ReadData3D (si, header)) continue;

            scanTx.push_back (header.pose.translation.x);
            scanTy.push_back (header.pose.translation.y);
            scanTz.push_back (header.pose.translation.z);

            // baseName_DDD_scanNr.gspc  direkt im outputDir (kein Unterordner)
            char scanSuffix[64];
            snprintf (scanSuffix, sizeof (scanSuffix), "_%s_%lld.gspc",
                      dirCode.ToCStr ().Get (), (long long)(si + 1));
            GS::UniString scanGspcName = baseName + GS::UniString (scanSuffix);
            IO::Location  scanGspcLoc  = outputDir;
            scanGspcLoc.AppendToLocal (IO::Name (scanGspcName));
            GS::UniString scanGspcPath;
            scanGspcLoc.ToPath (&scanGspcPath);

            GspcWriter gspc;
            if (!gspc.Open (scanGspcPath.ToCStr ().Get ())) {
                char buf[256];
                snprintf (buf, sizeof (buf), "GSPC öffnen fehlgeschlagen: %s",
                          scanGspcPath.ToCStr ().Get ());
                Log (buf);
                continue;
            }

            bool hasColor = header.pointFields.colorRedField
                         && header.pointFields.colorGreenField
                         && header.pointFields.colorBlueField;
            e57::Data3DPointsDouble buffers;
            buffers.cartesianX = x.data (); buffers.cartesianY = y.data ();
            buffers.cartesianZ = z.data ();
            if (hasColor) {
                buffers.colorRed   = cr.data (); buffers.colorGreen = cg.data ();
                buffers.colorBlue  = cb.data (); buffers.isColorInvalid = colorInvalid.data ();
            }
            auto cvr = reader.SetUpData3DPointsData (si, CHUNK, buffers);
            uint64_t nRead;
            int64_t  scanLocalIdx = 0;
            while ((nRead = cvr.read ()) > 0) {
                for (size_t i = 0; i < (size_t)nRead; ++i) {
                    if (scanLocalIdx % STRIDE == 0) {
                        double wx, wy, wz;
                        ApplyPose (header.pose, x[i], y[i], z[i], wx, wy, wz);
                        uint8_t r = 200, g = 200, b = 200;
                        if (hasColor && !colorInvalid[i]) {
                            r = (uint8_t)std::min<int> ((int)cr[i], 255);
                            g = (uint8_t)std::min<int> ((int)cg[i], 255);
                            b = (uint8_t)std::min<int> ((int)cb[i], 255);
                        }
                        gspc.Write ((float)wx, (float)wy, (float)wz, r, g, b);
                        ++scanWritten;
                        if (scanWritten % scanUpdateStep == 0) {
                            Int32 prog = (Int32)std::min (scanWritten, scanExpected[si]);
                            Compat_SetProcessValue (&prog);
                            if (Compat_IsProcessCanceled ()) { cancelled = true; break; }
                        }
                    }
                    ++scanLocalIdx;
                }
            }
            cvr.close ();
            gspc.Close ();

            if (!gspc.firstPt) {
                if (gspc.xmin < gxmin) gxmin = gspc.xmin;
                if (gspc.xmax > gxmax) gxmax = gspc.xmax;
                if (gspc.ymin < gymin) gymin = gspc.ymin;
                if (gspc.ymax > gymax) gymax = gspc.ymax;
                if (gspc.zmin < gzmin) gzmin = gspc.zmin;
                if (gspc.zmax > gzmax) gzmax = gspc.zmax;
            }
        }
        reader.Close ();

        if (cancelled) {
            Compat_CloseProcessWindow ();
            ACAPI_WriteReport ("Import abgebrochen.", false);
            return GS::UniString ();
        }

        if (gxmin <= gxmax) {
            offX  = gxmin; offY  = gymin; offZ  = gzmin;
            bboxX = gxmax - gxmin;
            bboxY = gymax - gymin;
            bboxZ = gzmax - gzmin;
        }

    } catch (const std::exception& ex) {
        if (progressOpen) Compat_CloseProcessWindow ();
        char buf[256];
        snprintf (buf, sizeof (buf), "E57-Fehler: %s", ex.what ());
        Log (buf);
        ACAPI_WriteReport ("E57-Konvertierung fehlgeschlagen.", true);
        return GS::UniString ();
    }

    {
        Int32 gsmWork = 1;
        GS::UniString phase2 ("GSM erstellen...");
        Compat_SetNextProcessPhase (&phase2, &gsmWork);

        bool ok = WriteGsmViaXml (
            (int)nScans, baseName.ToCStr ().Get (),
            outputDirPath.ToCStr ().Get (),
            bboxX, bboxY, bboxZ,
            offX,  offY,  offZ,
            densityPct,
            e57Path.ToCStr ().Get (),
            totalOrigPoints,
            scanTx.data (), scanTy.data (), scanTz.data (),
            (int)stories.size ());

        Int32 done = 1;
        Compat_SetProcessValue (&done);
        Compat_CloseProcessWindow ();

        if (!ok) {
            Log ("WriteGsmViaXml fehlgeschlagen.");
            ACAPI_WriteReport ("GSM konnte nicht erstellt werden.", true);
            return GS::UniString ();
        }
    }

    // PwMeta schreiben — ermoeglicht schnellen Reload (nur GSM neu, kein E57-Lesen)
    {
        PwMeta m = {};
        m.origX      = bboxX;  m.origY      = bboxY;  m.origZ      = bboxZ;
        m.offX       = offX;   m.offY       = offY;   m.offZ       = offZ;
        m.scanCount  = (int32_t)nScans;
        m.totalPoints = totalOrigPoints;
        m.storyCount = (int32_t)stories.size ();
        WritePwMeta (outputDirPath.ToCStr ().Get (), baseName.ToCStr ().Get (),
                     m, scanTx, scanTy, scanTz, e57Path.ToCStr ().Get ());
    }

    RegisterLibraryDir (outputDir);
    Compat_ReloadLibraries ();

    GS::UniString msg = GS::UniString ("\"") + baseName
        + GS::UniString ("\" importiert (")
        + GS::UniString (std::to_string (densityPct).c_str ())
        + GS::UniString ("% der Punkte).");
    ACAPI_WriteReport (msg.ToCStr ().Get (), false);
    return baseName;
}

// --- DoAdjustDensity (veraltet — nur noch intern referenziert) ----------------
// Wird nicht mehr über die Palette aufgerufen. relBtn ruft DoImport() direkt.

[[maybe_unused]] static bool DoAdjustDensity_Legacy (const PointCloudEntry& sel, int newDensity)
{
    Log ("--- DoAdjustDensity ---");
    const int STRIDE = std::max (1, 100 / newDensity);

    int64_t totalWritten = 0;
    bool    progressOpen = false;

    try {
        e57::Reader reader (std::string (sel.e57Path.ToCStr ().Get ()), e57::ReaderOptions{});
        if (!reader.IsOpen ()) {
            ACAPI_WriteReport ("E57-Datei konnte nicht geöffnet werden.", true);
            return false;
        }
        int64_t nScans = reader.GetData3DCount ();
        if (nScans == 0) {
            ACAPI_WriteReport ("E57-Datei enthält keine Scans.", true);
            return false;
        }

        IO::fileSystem.CreateFolder (IO::Location (sel.outputDir));

        int64_t totalExpected = 0;
        for (int64_t si2 = 0; si2 < nScans; ++si2) {
            e57::Data3D hdr2;
            if (reader.ReadData3D (si2, hdr2) && hdr2.pointCount > 0)
                totalExpected += hdr2.pointCount / STRIDE;
        }
        if (totalExpected <= 0) totalExpected = nScans * 500000LL;

        Int32 nPhases = 1;
        GS::UniString procTitle ("Dichte anpassen");
        Compat_InitProcessWindow (&procTitle, &nPhases);
        progressOpen = true;
        Int32 ptTotal    = (Int32)std::min (totalExpected, (int64_t)2000000000LL);
        Int32 updateStep = (Int32)std::max ((int64_t)1, totalExpected / 200);
        GS::UniString phase1 ("GSPC erzeugen...");
        Compat_SetNextProcessPhase (&phase1, &ptTotal);

        const size_t CHUNK = 65536;
        std::vector<double>   x (CHUNK), y (CHUNK), z (CHUNK);
        std::vector<uint16_t> cr (CHUNK, 200), cg (CHUNK, 200), cb (CHUNK, 200);
        std::vector<int8_t>   colorInvalid (CHUNK, 0);
        bool cancelled = false;

        for (int64_t si = 0; si < nScans && !cancelled; ++si) {
            e57::Data3D header;
            if (!reader.ReadData3D (si, header)) continue;

            char scanSuffix[32];
            snprintf (scanSuffix, sizeof (scanSuffix), "_%lld.gspc", (long long)(si + 1));
            GS::UniString scanGspcName = sel.name + GS::UniString (scanSuffix);
            IO::Location  scanGspcLoc (sel.outputDir);
            scanGspcLoc.AppendToLocal (IO::Name (scanGspcName));
            GS::UniString scanGspcPath;
            scanGspcLoc.ToPath (&scanGspcPath);

            GspcWriter gspc;
            if (!gspc.Open (scanGspcPath.ToCStr ().Get ())) {
                char buf[256];
                snprintf (buf, sizeof (buf), "GSPC öffnen fehlgeschlagen: %s",
                          scanGspcPath.ToCStr ().Get ());
                Log (buf);
                continue;
            }

            bool hasColor = header.pointFields.colorRedField
                         && header.pointFields.colorGreenField
                         && header.pointFields.colorBlueField;
            e57::Data3DPointsDouble buffers;
            buffers.cartesianX = x.data (); buffers.cartesianY = y.data ();
            buffers.cartesianZ = z.data ();
            if (hasColor) {
                buffers.colorRed   = cr.data (); buffers.colorGreen = cg.data ();
                buffers.colorBlue  = cb.data (); buffers.isColorInvalid = colorInvalid.data ();
            }
            auto cvr = reader.SetUpData3DPointsData (si, CHUNK, buffers);
            uint64_t nRead;
            int64_t  scanLocalIdx = 0;
            while ((nRead = cvr.read ()) > 0) {
                for (size_t i = 0; i < (size_t)nRead; ++i) {
                    if (scanLocalIdx % STRIDE == 0) {
                        double wx, wy, wz;
                        ApplyPose (header.pose, x[i], y[i], z[i], wx, wy, wz);
                        uint8_t r = 200, g = 200, b = 200;
                        if (hasColor && !colorInvalid[i]) {
                            r = (uint8_t)std::min<int> ((int)cr[i], 255);
                            g = (uint8_t)std::min<int> ((int)cg[i], 255);
                            b = (uint8_t)std::min<int> ((int)cb[i], 255);
                        }
                        gspc.Write ((float)wx, (float)wy, (float)wz, r, g, b);
                        ++totalWritten;
                        if (totalWritten % updateStep == 0) {
                            Int32 prog = (Int32)std::min (totalWritten, (int64_t)ptTotal);
                            Compat_SetProcessValue (&prog);
                            if (Compat_IsProcessCanceled ()) { cancelled = true; break; }
                        }
                    }
                    ++scanLocalIdx;
                }
            }
            cvr.close ();
            gspc.Close ();
        }
        reader.Close ();

        if (cancelled) {
            Compat_CloseProcessWindow ();
            return false;
        }

    } catch (const std::exception& ex) {
        if (progressOpen) Compat_CloseProcessWindow ();
        char buf[256];
        snprintf (buf, sizeof (buf), "Exception: %s", ex.what ());
        Log (buf);
        ACAPI_WriteReport ("Fehler beim Einlesen der E57-Datei.", true);
        return false;
    }

    Compat_CloseProcessWindow ();

    int updCount = UpdatePlacedParams (sel.libIndex, newDensity, totalWritten);
    { char buf[64]; snprintf (buf, sizeof (buf), "updCount=%d", updCount); Log (buf); }

    ReloadLibraries ();

    GS::UniString msg = GS::UniString ("\"") + sel.name
        + GS::UniString ("\" neu importiert mit ")
        + GS::UniString (std::to_string (newDensity).c_str ())
        + GS::UniString ("% der Punkte");
    if (updCount > 0)
        msg += GS::UniString (" (") + GS::UniString (std::to_string (updCount).c_str ())
             + GS::UniString (" Objekt(e) aktualisiert).");
    else
        msg += GS::UniString (".");
    ACAPI_WriteReport (msg.ToCStr ().Get (), false);
    return true;
}

// --- TryDeleteEntry ----------------------------------------------------------

bool TryDeleteEntry (const PointCloudEntry& entry)
{
    GS::Array<API_Guid> elemList;
    ACAPI_Element_GetElemList (API_ObjectID, &elemList);
    GS::Array<API_Guid> toDelete;
    for (const auto& guid : elemList) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get (&elem) != NoError) continue;
        if (elem.object.libInd == entry.libIndex)
            toDelete.Push (guid);
    }

    GS::UniString subText;
    if (toDelete.GetSize () > 0) {
        char buf[128];
        snprintf (buf, sizeof (buf),
            "%d platziertes Objekt%s wird gelöscht. GSM + GSPC werden von der Festplatte entfernt.",
            (int)toDelete.GetSize (), toDelete.GetSize () == 1 ? "" : "e");
        subText = GS::UniString (buf);
    } else {
        subText = GS::UniString ("GSM + GSPC-Dateien werden gelöscht. Die E57-Quelldatei bleibt erhalten.");
    }

    GS::UniString question = GS::UniString ("\"") + entry.name + GS::UniString ("\" löschen?");
    short result = DGAlert (DG_WARNING,
        GS::UniString ("Punktwolke löschen"),
        question,
        subText,
        GS::UniString ("Löschen"),
        GS::UniString ("Abbrechen"));
    if (result != 1) return false;

    if (toDelete.GetSize () > 0) {
        ACAPI_CallUndoableCommand ("Punktwolke löschen",
            [&] () -> GSErrCode {
                return ACAPI_Element_Delete (toDelete);
            });
    }

    // GSM + alle Auflösungsordner rekursiv löschen
    IO::Location dirLoc (entry.outputDir);
    IO::Location gsmLoc = dirLoc;
    gsmLoc.AppendToLocal (IO::Name (entry.name + ".gsm"));
    GS::UniString gsmPath;
    gsmLoc.ToPath (&gsmPath);
    remove (gsmPath.ToCStr ().Get ());

    // Alle GSPC-Dateien im outputDir löschen (liegen flach, kein Unterordner)
    {
        const char* outPath = entry.outputDir.ToCStr ().Get ();
#ifdef _WIN32
        char pattern[1024];
        snprintf (pattern, sizeof (pattern), "%s\\*.gspc", outPath);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA (pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char fp[1024];
                snprintf (fp, sizeof (fp), "%s\\%s", outPath, fd.cFileName);
                DeleteFileA (fp);
            } while (FindNextFileA (h, &fd));
            FindClose (h);
        }
#else
        DIR* d = opendir (outPath);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir (d)) != nullptr) {
                if (ent->d_type != DT_REG) continue;
                const char* n = ent->d_name;
                size_t nl = strlen (n);
                if (nl >= 5 && strcmp (n + nl - 5, ".gspc") == 0) {
                    char fp[1024];
                    snprintf (fp, sizeof (fp), "%s/%s", outPath, n);
                    remove (fp);
                }
            }
            closedir (d);
        }
#endif
    }
    rmdir (entry.outputDir.ToCStr ().Get ());

    UnregisterLibraryDir (entry.outputDir);

    char buf[512];
    snprintf (buf, sizeof (buf), "Gelöscht: %s (%d Objekt%s entfernt)",
              entry.outputDir.ToCStr ().Get (),
              (int)toDelete.GetSize (), toDelete.GetSize () == 1 ? "" : "e");
    Log (buf);
    return true;
}
