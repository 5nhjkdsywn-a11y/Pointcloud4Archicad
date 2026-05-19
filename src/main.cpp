// E57-Import Add-On für ArchiCAD 26
// Punktwolken-Palette: Import, Dichte anpassen, Löschen

#include "APIEnvir.h"
#include "compat.h"

#include "DGDLLExport.h"
#include "DGFileDlg.hpp"
#include "DGDialog.hpp"
#include "DGButton.hpp"
#include "DGEditControl.hpp"
#include "DGStaticItem.hpp"
#include "DGListBox.hpp"
#include "DG.h"

#include "File.hpp"
#include "FileSystem.hpp"

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
#endif

#define ID_ADDON_INFO  32000
#define ID_ADDON_MENU  32500

#ifdef _WIN32
static const char* LOG_PATH = "C:\\ProgramData\\GRAPHISOFT\\punktwolke_addon\\pcd_debug.log";
#else
static const char* LOG_PATH = "/Applications/GRAPHISOFT/punktwolke_addon/pcd_debug.log";
#endif

static void Log(const char* msg)
{
    FILE* f = fopen(LOG_PATH, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ── UUID für GSM-MainGUID ─────────────────────────────────────────────────────
static void GenGUID(char out[37])
{
    uint32_t d[4];
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
    {
        CryptGenRandom(hProv, sizeof(d), (BYTE*)d);
        CryptReleaseContext(hProv, 0);
    }
    else
    {
        for (int i = 0; i < 4; i++) d[i] = (uint32_t)(GetTickCount64() ^ (uintptr_t)out ^ i);
    }
#else
    for (int i = 0; i < 4; i++) d[i] = arc4random();
#endif
    d[1] = (d[1] & 0xFFFF0FFF) | 0x00004000;
    d[2] = (d[2] & 0x3FFFFFFF) | 0x80000000;
    snprintf(out, 37, "%08X-%04X-%04X-%04X-%04X%08X",
        d[0],
        (d[1] >> 16), (d[1] & 0xFFFF),
        (d[2] >> 16), (d[2] & 0xFFFF),
        d[3]);
}

// ── Plattform-unabhängige Pfadtrenner-Suche ──────────────────────────────────
// IO::Location::ToPath() liefert auf Windows Backslash-Pfade, auf macOS Forward-Slash-Pfade.
// FindLast('/') schlägt auf Windows fehl → beide Trenner prüfen.
static UInt32 FindLastSep(const GS::UniString& path)
{
    UInt32 fwd  = path.FindLast('/');
    UInt32 back = path.FindLast('\\');
    if (fwd  == GS::MaxUIndex) return back;
    if (back == GS::MaxUIndex) return fwd;
    return std::max(fwd, back);
}

#ifdef _WIN32
static const char PATH_SEP = '\\';
#else
static const char PATH_SEP = '/';
#endif

// ── GSM via LP_XMLConverter erzeugen ─────────────────────────────────────────
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

static bool WriteGsmViaXml(
    int scanCount,
    const char* baseName,
    const char* outputDirPath,
    float origX, float origY, float origZ,   // Ausdehnung (bboxX/Y/Z)
    float offX,  float offY,  float offZ,    // Ursprungsversatz (xmin/ymin/zmin)
    int densityPercent,
    const char* e57PathStr,
    int64_t totalPoints,
    int64_t reducedPoints)
{
    char guid[37];
    GenGUID(guid);

#ifdef _WIN32
    char xmlTempDir[MAX_PATH] = "C:\\Temp";
    GetTempPathA(MAX_PATH, xmlTempDir);
    char xmlPath[MAX_PATH];
    snprintf(xmlPath, sizeof(xmlPath), "%spw_gsm_%s.xml", xmlTempDir, baseName);
#else
    char xmlPath[1024];
    snprintf(xmlPath, sizeof(xmlPath), "/tmp/pw_gsm_%s.xml", baseName);
#endif

    FILE* f = fopen(xmlPath, "w");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<Symbol IsArchivable=\"false\" IsPlaceable=\"true\" "
               "MainGUID=\"%s\" MigrationValue=\"Normal\" "
               "Owner=\"1196638531\" Signature=\"1196644685\" Version=\"44\">\n\n", guid);

    fprintf(f,
        "<Ancestry SectVersion=\"1\" SectionFlags=\"0\" SubIdent=\"0\" Template=\"false\">\n"
        "\t<MainGUID>F938E33A-329D-4A36-BE3E-85E126820996</MainGUID>\n"
        "\t<MainGUID>103E8D2C-8230-42E1-9597-46F84CCE28C0</MainGUID>\n"
        "\t<MainGUID>B298E56B-35FF-45C6-AA50-6D7F0B40BB1A</MainGUID>\n"
        "</Ancestry>\n\n"
        "<CalledMacros SectVersion=\"2\" SectionFlags=\"0\" SubIdent=\"0\">\n"
        "</CalledMacros>\n\n");

    // 3D-Script
    fprintf(f, "<Script_3D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf(f,
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
        "mul _scale3DModelX, _scale3DModelY, _scale3DModelZ\n\n");
    for (int sc = 0; sc < scanCount; ++sc) {
        char scanParam[32], scanFile[256];
        snprintf(scanParam, sizeof(scanParam), "bScan_%d", sc + 1);
        snprintf(scanFile,  sizeof(scanFile),  "%s_%d.gspc", baseName, sc + 1);
        fprintf(f, "if %s then\n\tPOINTCLOUD \"%s\"\nendif\n\n", scanParam, scanFile);
    }
    fprintf(f,
        "del 1\n\n"
        "if bProject2D then\n"
        "\tcutend\n"
        "\tcutend\n"
        "endif\n");
    fprintf(f, "]]>\n</Script_3D>\n\n");

    // 2D-Script
    fprintf(f, "<Script_2D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf(f,
        "project2{2} 3, 270, 3,\n"
        "\t\t\tparameters bProject2D = 1\n\n");
    fprintf(f, "]]>\n</Script_2D>\n\n");

    // Master-Script (1D)
    fprintf(f, "<Script_1D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf(f,
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
    fprintf(f, "]]>\n</Script_1D>\n\n");

    // UI-Script
    fprintf(f, "<Script_UI SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf(f,
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
        "yCurr = yStart\n\n"
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
        "ui_style 0, 0\n"
        "ui_outfield\t\t\"Importierte Dichte:\",\txPos1,\tyCurr,\toutFldWdt,\toutFldHgt\n");
    fprintf(f, "ui_outfield\t\tSTR(iDensityPercent, 1, 0) + \" %% (\" + STR(iReducedPoints, 1, 0) + \" von \" + STR(iTotalPoints, 1, 0) + \" Punkten)\",\n"
               "\t\t\t\txPos2,\tyCurr,\t444-xPos2,\toutFldHgt\n\n");
    fprintf(f, "yCurr = yCurr + diffY\n\n");
    fprintf(f, "ui_outfield\t\t\"Quelldatei:\",\txPos1,\tyCurr,\toutFldWdt,\toutFldHgt\n");
    fprintf(f, "ui_outfield\t\tsE57Path,\txPos2,\tyCurr,\t444-xPos2,\toutFldHgt\n\n");
    fprintf(f, "yCurr = yCurr + diffY\n\n");
    fprintf(f,
        "ui_separator\t0,\tyCurr,\t\t444,\tyCurr\n\n"
        "yCurr = yCurr + 9\n\n"
        "ui_style 0, 1\n"
        "ui_outfield\t\t\"Scan-Standorte:\",\txPos1,\tyCurr,\toutFldWdt,\toutFldHgt\n\n"
        "yCurr = yCurr + diffY\n\n"
        "ui_style 0, 0\n"
        "for sc = 1 to iScanCount\n"
        "\tscanLabel = \"Standort \" + str(sc, 1, 0)\n"
        "\tscanParam = \"bScan_\" + str(sc, 1, 0)\n"
        "\tui_infield{3} scanParam, xPos1, yCurr-4, 444-xPos1, inFldHgt,\n"
        "\t\t7, \"\", 1, 1,\n"
        "\t\t0, 0, 0, 0,\n"
        "\t\t1, scanLabel, 0,\n"
        "\t\t1, scanLabel, 1\n"
        "\tyCurr = yCurr + diffY\n"
        "endfor\n\n");
    fprintf(f, "]]>\n</Script_UI>\n\n");

    // VL-Script
    fprintf(f, "<Script_VL SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf(f,
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
        "lock \"iReducedPoints\"\n"
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
    fprintf(f, "]]>\n</Script_VL>\n\n");

    // Parameter-Sektion
    fprintf(f,
        "<ParamSection SectVersion=\"27\" SectionFlags=\"0\" SubIdent=\"0\">\n"
        "\t<ParamSectHeader>\n"
        "\t\t<AutoHotspots>false</AutoHotspots>\n"
        "\t\t<StatBits><STBit_UIDefault/><STBit_FixSize/></StatBits>\n"
        "\t\t<WDLeftFrame>0</WDLeftFrame><WDRightFrame>0</WDRightFrame>\n"
        "\t\t<WDTopFrame>0</WDTopFrame><WDBotFrame>0</WDBotFrame>\n"
        "\t\t<LayFlags>65535</LayFlags>\n"
        "\t\t<WDMirrorThickness>0</WDMirrorThickness>\n"
        "\t\t<WDWallInset>0</WDWallInset>\n"
        "\t</ParamSectHeader>\n"
        "\t<Parameters>\n");

    fprintf(f, "\t\t<Length Name=\"A\"><Description><![CDATA[\"Dimension 1\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origX);
    fprintf(f, "\t\t<Length Name=\"B\"><Description><![CDATA[\"Dimension 2\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origY);
    fprintf(f, "\t\t<Length Name=\"ZZYZX\"><Description><![CDATA[\"Height\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf(f, "\t\t<Boolean Name=\"AC_show2DHotspotsIn3D\"><Description><![CDATA[\"Show 2D Hotspots in 3D\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Boolean>\n");
    fprintf(f, "\t\t<Length Name=\"ac_bottomlevel\"><Description><![CDATA[\"Bottom Level\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"ac_toplevel\"><Description><![CDATA[\"Top Level\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf(f, "\t\t<Boolean Name=\"bProject2D\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Boolean>\n");
    fprintf(f, "\t\t<Title Name=\"tRep3D\"><Description><![CDATA[\"3D Representation\"]]></Description><Fix/></Title>\n");
    fprintf(f, "\t\t<Integer Name=\"iModelUnit\"><Description><![CDATA[\"Basic Model Unit\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></Integer>\n");
    fprintf(f, "\t\t<RealNum Name=\"offsetX\"><Description><![CDATA[\"X Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offX);
    fprintf(f, "\t\t<RealNum Name=\"offsetY\"><Description><![CDATA[\"Y Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offY);
    fprintf(f, "\t\t<RealNum Name=\"offsetZ\"><Description><![CDATA[\"Z Coordinate of The Point Cloud Origin\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>%.8g</Value></RealNum>\n", (double)offZ);
    fprintf(f, "\t\t<Length Name=\"scalingUnitX\"><Description><![CDATA[\"Scaling Unit Length X\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"scalingUnitY\"><Description><![CDATA[\"Scaling Unit Length Y\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"scalingUnitZ\"><Description><![CDATA[\"Scaling Unit Length Z\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></Length>\n");
    fprintf(f, "\t\t<Boolean Name=\"bLinkScale\"><Description><![CDATA[\"Link Scale\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></Boolean>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleX\"><Description><![CDATA[\"Scale of X Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleY\"><Description><![CDATA[\"Scale of Y Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleZ\"><Description><![CDATA[\"Scale of Z Coordinate\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleRatioXY\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleRatioXZ\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<RealNum Name=\"scaleRatioYZ\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1</Value></RealNum>\n");
    fprintf(f, "\t\t<Boolean Name=\"bOverrideCutplane\"><Description><![CDATA[\"Override Floor Plan Cut Plane Height\"]]></Description>"
               "<Fix/><Flags><ParFlg_BoldName/></Flags><Value>0</Value></Boolean>\n");
    fprintf(f, "\t\t<Length Name=\"upperPlaneHeight\"><Description><![CDATA[\"Upper Cutplane Height to Current Story\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1.1</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"tempUpperPlaneHeight\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>1.1</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"lowerPlaneHeight\"><Description><![CDATA[\"Lower Cutplane Height to Current Story\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>0</Value></Length>\n");
    fprintf(f, "\t\t<Length Name=\"tempLowerPlaneHeight\"><Description><![CDATA[\"\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/><ParFlg_Hidden/></Flags><Value>0</Value></Length>\n");
    fprintf(f, "\t\t<Integer Name=\"iDensityPercent\"><Description><![CDATA[\"Importierte Dichte (%%)\"]]></Description>"
               "<Fix/><Value>%d</Value></Integer>\n", densityPercent);
    fprintf(f, "\t\t<Integer Name=\"iTotalPoints\"><Description><![CDATA[\"Gesamtpunkte\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%lld</Value></Integer>\n", (long long)totalPoints);
    fprintf(f, "\t\t<Integer Name=\"iReducedPoints\"><Description><![CDATA[\"Importierte Punkte\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%lld</Value></Integer>\n", (long long)reducedPoints);
    fprintf(f, "\t\t<String Name=\"sE57Path\"><Description><![CDATA[\"E57 Quelldatei\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags>"
               "<Value><![CDATA[\"%s\"]]></Value></String>\n", e57PathStr ? e57PathStr : "");
    fprintf(f, "\t\t<Integer Name=\"iScanCount\"><Description><![CDATA[\"Anzahl Scan-Standorte\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%d</Value></Integer>\n", scanCount);
    for (int sc = 0; sc < scanCount; ++sc) {
        char pname[32], pdesc[64];
        snprintf(pname, sizeof(pname), "bScan_%d", sc + 1);
        snprintf(pdesc, sizeof(pdesc), "Standort %d anzeigen", sc + 1);
        fprintf(f, "\t\t<Boolean Name=\"%s\"><Description><![CDATA[\"%s\"]]></Description>"
                   "<Fix/><Value>1</Value></Boolean>\n", pname, pdesc);
    }

    fprintf(f, "\t</Parameters>\n</ParamSection>\n\n</Symbol>\n");
    fclose(f);

    char gsmPath[1024];
    snprintf(gsmPath, sizeof(gsmPath), "%s%c%s.gsm", outputDirPath, PATH_SEP, baseName);

    // Temporäre Ausgabedatei für LP_XMLConverter-Log
#ifdef _WIN32
    char tempDir[MAX_PATH] = "C:\\Temp";
    GetTempPathA(MAX_PATH, tempDir);
    char lpcOut[MAX_PATH];
    snprintf(lpcOut, sizeof(lpcOut), "%spw_lpc_%s.txt", tempDir, baseName);
#else
    char lpcOut[256];
    snprintf(lpcOut, sizeof(lpcOut), "/tmp/pw_lpc_%s.txt", baseName);
#endif

    bool ok = false;

#ifdef _WIN32
    // Kommandozeile zusammenbauen
    char cmdLine[4096];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" xml2libpart \"%s\" \"%s\"",
             LP_XMLCONVERTER, xmlPath, gsmPath);

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = CreateFileA(lpcOut,
        GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOA si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = hOut;
    si.hStdError   = hOut;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        ok = (exitCode == 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        char buf[256];
        snprintf(buf, sizeof(buf), "LP_XMLConverter exit=%lu xml=%s gsm=%s",
                 (unsigned long)exitCode, xmlPath, gsmPath);
        Log(buf);
    }
    else
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "CreateProcess fehlgeschlagen: %lu", (unsigned long)GetLastError());
        Log(buf);
    }
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, lpcOut,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    const char* argv[] = { LP_XMLCONVERTER, "xml2libpart", xmlPath, gsmPath, nullptr };
    extern char** environ;
    pid_t pid;
    int spawnRet = posix_spawn(&pid, LP_XMLCONVERTER, &fa, nullptr,
                               (char* const*)argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (spawnRet == 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        ok = (exitCode == 0);
        char buf[256];
        snprintf(buf, sizeof(buf), "LP_XMLConverter exit=%d xml=%s gsm=%s",
                 exitCode, xmlPath, gsmPath);
        Log(buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "posix_spawn fehlgeschlagen: %d", spawnRet);
        Log(buf);
    }
#endif

    // LP_XMLConverter-Log auslesen und in Debug-Log schreiben
    if (FILE* lf = fopen(lpcOut, "r")) {
        char line[512];
        while (fgets(line, sizeof(line), lf)) {
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
            Log(line);
        }
        fclose(lf);
    }

    remove(lpcOut);
    if (ok)
        remove(xmlPath);
    return ok;
}

// ── GSPC-Koordinaten normalisieren (Offset xmin/ymin/zmin subtrahieren) ──────
// GSPC enthält Weltkoordinaten; GDL-Script erwartet Ursprung bei (0,0,0).
// Format: 32 Bytes Header (4 magic + 4 version + 24 bbox), dann N×15 Bytes (xyz float32 + rgb uint8).
static void NormalizeGspcCoords(const char* path,
                                 float ox, float oy, float oz,
                                 float bboxX, float bboxY, float bboxZ)
{
    if (ox == 0.0f && oy == 0.0f && oz == 0.0f) return;

    FILE* f = fopen(path, "r+b");
    if (!f) { Log("NormalizeGspc: fopen fehlgeschlagen"); return; }

    // Bbox im Header auf normalisierte Werte setzen (Offset 8: xmin,xmax,ymin,ymax,zmin,zmax)
    fseek(f, 8, SEEK_SET);
    float bbox[6] = {0.0f, bboxX, 0.0f, bboxY, 0.0f, bboxZ};
    fwrite(bbox, sizeof(float), 6, f);

    // Punkte ab Byte 32 verschieben
    fseek(f, 32, SEEK_SET);
    float xyz[3];
    uint8_t rgb[3];
    while (true) {
        long pos = ftell(f);
        if (fread(xyz, sizeof(float), 3, f) != 3) break;
        if (fread(rgb, 1, 3, f) != 3) break;
        xyz[0] -= ox; xyz[1] -= oy; xyz[2] -= oz;
        fseek(f, pos, SEEK_SET);
        fwrite(xyz, sizeof(float), 3, f);
        fseek(f, 3, SEEK_CUR);  // RGB-Bytes überspringen
    }
    fclose(f);
}

// ── GSPC-Datei schreiben ──────────────────────────────────────────────────────
struct GspcWriter {
    FILE* fp      = nullptr;
    float xmin = 0, xmax = 0, ymin = 0, ymax = 0, zmin = 0, zmax = 0;
    bool  firstPt = true;

    bool Open(const char* path)
    {
        fp = fopen(path, "wb");
        if (!fp) return false;
        const char magic[4] = {'G','S','P','C'};
        uint32_t ver = 2;
        float    bbox[6] = {};
        fwrite(magic, 1, 4, fp);
        fwrite(&ver,  4, 1, fp);
        fwrite(bbox,  4, 6, fp);
        return true;
    }

    void Write(float x, float y, float z, uint8_t r, uint8_t g, uint8_t b)
    {
        if (firstPt) {
            xmin = xmax = x; ymin = ymax = y; zmin = zmax = z;
            firstPt = false;
        } else {
            if (x < xmin) xmin = x; if (x > xmax) xmax = x;
            if (y < ymin) ymin = y; if (y > ymax) ymax = y;
            if (z < zmin) zmin = z; if (z > zmax) zmax = z;
        }
        fwrite(&x, 4, 1, fp);
        fwrite(&y, 4, 1, fp);
        fwrite(&z, 4, 1, fp);
        fwrite(&r, 1, 1, fp);
        fwrite(&g, 1, 1, fp);
        fwrite(&b, 1, 1, fp);
    }

    bool Close()
    {
        if (!fp) return false;
        fseek(fp, 8, SEEK_SET);
        float bbox[6] = {xmin, xmax, ymin, ymax, zmin, zmax};
        fwrite(bbox, 4, 6, fp);
        fclose(fp);
        fp = nullptr;
        return true;
    }
};

// ── Quaternion-Pose auf Punkt anwenden ────────────────────────────────────────
static void ApplyPose(const e57::RigidBodyTransform& pose,
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

// ── PointCloud-Eintrag ────────────────────────────────────────────────────────
struct PointCloudEntry {
    GS::UniString name;
    int           densityPercent;
    GS::UniString e57Path;
    GS::UniString outputDir;
    int           scanCount;   // Anzahl Scan-Standorte; 0 = altes Format (_data.gspc)
    Int32         libIndex;

    GS::UniString GspcName(int scanIdx) const
    {
        if (scanCount == 0) return name + "_data.gspc";
        char buf[32];
        snprintf(buf, sizeof(buf), "_%d.gspc", scanIdx + 1);
        return name + GS::UniString(buf);
    }
};

static GS::Array<PointCloudEntry> CollectOurLibParts()
{
    GS::Array<PointCloudEntry> result;
    Int32 count = 0;
    if (ACAPI_LibPart_GetNum(&count) != NoError) { Log("CollectOurLibParts: GetNum fehlgeschlagen"); return result; }

    for (Int32 i = 1; i <= count; ++i) {
        API_LibPart lp = {};
        lp.index = i;
        if (ACAPI_LibPart_Get(&lp) != NoError) continue;

        double a = 0, b = 0;
        Int32 parNum = 0;
        API_AddParType** pars = nullptr;
        if (ACAPI_LibPart_GetParams(i, &a, &b, &parNum, &pars) != NoError) continue;

        GS::UniString e57Path;
        int density   = -1;
        int scanCount =  0;

        for (Int32 j = 0; j < parNum; ++j) {
            const API_AddParType& p = (*pars)[j];
            if (strcmp(p.name, "sE57Path") == 0 && p.typeID == APIParT_CString) {
                char val[API_UAddParStrLen + 1] = {};
                for (int k = 0; k < API_UAddParStrLen && p.value.uStr[k]; ++k)
                    val[k] = (char)(p.value.uStr[k] & 0xFF);
                e57Path = GS::UniString(val);
            }
            if (strcmp(p.name, "iDensityPercent") == 0)
                density = (int)(p.value.real + 0.5);
            if (strcmp(p.name, "iScanCount") == 0)
                scanCount = (int)(p.value.real + 0.5);
        }
        ACAPI_DisposeAddParHdl(&pars);

        if (e57Path.GetLength() == 0 || density <= 0) continue;

        PointCloudEntry entry;
        char nameBuf[API_UniLongNameLen + 1] = {};
        for (int k = 0; k < API_UniLongNameLen && lp.docu_UName[k]; ++k)
            nameBuf[k] = (char)(lp.docu_UName[k] & 0xFF);
        entry.name = GS::UniString(nameBuf);
        if (entry.name.GetLength() > 4) {
            GS::UniString ext = entry.name.GetSubstring(entry.name.GetLength() - 4, 4);
            ext.SetToLowerCase();
            if (ext == ".gsm")
                entry.name = entry.name.GetSubstring(0, entry.name.GetLength() - 4);
        }
        entry.densityPercent = density;
        entry.e57Path        = e57Path;
        entry.scanCount      = scanCount;
        entry.libIndex       = i;

        // Elternverzeichnis plattform-unabhängig (Windows: \, macOS: /)
        UInt32 sl = FindLastSep(e57Path);
        GS::UniString e57Dir = (sl != GS::MaxUIndex)
            ? e57Path.GetSubstring(0, sl) : e57Path;
        // outputDir via IO::Location aufbauen → korrekte native Trennzeichen
        IO::Location outputDirLoc(e57Dir);
        outputDirLoc.AppendToLocal(IO::Name(entry.name));
        outputDirLoc.ToPath(&entry.outputDir);
        result.Push(entry);
    }
    return result;
}

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

// Ausgabeordner in Bibliotheks-Manager eintragen (immer SetLibrariesID aufrufen → Reload)
static void RegisterLibraryDir(const IO::Location& outputDir)
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries(&allLibs, &embIdx);

    GS::UniString newPath;
    outputDir.ToPath(&newPath);

    GS::Array<API_LibraryInfo> userLibs;
    bool found = false;
    for (UInt32 i = 0; i < allLibs.GetSize(); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        GS::UniString p;
        allLibs[i].location.ToPath(&p);
        if (p == newPath) found = true;
        userLibs.Push(allLibs[i]);
    }
    if (!found) {
        API_LibraryInfo entry = {};
        entry.location    = outputDir;
        entry.libraryType = API_LocalLibrary;
        userLibs.Push(entry);
    }
    Compat_SetLibraries(&userLibs);
}

// Ausgabeordner aus Bibliotheks-Manager entfernen
static void UnregisterLibraryDir(const GS::UniString& outputDirPath)
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries(&allLibs, &embIdx);

    GS::Array<API_LibraryInfo> userLibs;
    for (UInt32 i = 0; i < allLibs.GetSize(); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        GS::UniString p;
        allLibs[i].location.ToPath(&p);
        if (p != outputDirPath) userLibs.Push(allLibs[i]);
    }
    Compat_SetLibraries(&userLibs);
}

// GSPC-Cache leeren (nach Dichte-Änderung nötig)
static void ReloadLibraries()
{
    GS::Array<API_LibraryInfo> allLibs;
    Int32 embIdx = 0;
    Compat_GetLibraries(&allLibs, &embIdx);
    GS::Array<API_LibraryInfo> userLibs;
    for (UInt32 i = 0; i < allLibs.GetSize(); ++i) {
        if (allLibs[i].libraryType == 3) continue;
        userLibs.Push(allLibs[i]);
    }
    Compat_SetLibraries(&userLibs);
    Compat_ReloadLibraries();
}

// Parameter in platzierten Instanzen aktualisieren (VOR ReloadLibraries aufrufen!)
static int UpdatePlacedParams(Int32 libIdx, int newDensity, int64_t writtenPoints)
{
    double lpA, lpB;
    Int32 parNum = 0;
    API_AddParType** lpPars = nullptr;
    ACAPI_LibPart_GetParams(libIdx, &lpA, &lpB, &parNum, &lpPars);
    ACAPI_DisposeAddParHdl(&lpPars);

    GS::Array<API_Guid> elemList;
    ACAPI_Element_GetElemList(API_ObjectID, &elemList);

    GS::Array<API_Guid> toUpdate;
    for (const auto& guid : elemList) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get(&elem) != NoError) continue;
        if (elem.object.libInd == libIdx)
            toUpdate.Push(guid);
    }

    int updCount = 0;
    if (toUpdate.GetSize() > 0) {
        ACAPI_CallUndoableCommand("Punktwolkendichte anpassen",
            [&]() -> GSErrCode {
                for (auto guid : toUpdate) {
                    API_ElementMemo memo = {};
                    if (ACAPI_Element_GetMemo(guid, &memo, APIMemoMask_AddPars) != NoError) {
                        ACAPI_DisposeElemMemoHdls(&memo);
                        continue;
                    }
                    if (memo.params != nullptr) {
                        for (Int32 j = 0; j < parNum; ++j) {
                            const char* pn = (*memo.params)[j].name;
                            if (strcmp(pn, "iDensityPercent") == 0)
                                (*memo.params)[j].value.real = (double)newDensity;
                            else if (strcmp(pn, "iReducedPoints") == 0)
                                (*memo.params)[j].value.real = (double)writtenPoints;
                        }
                    }
                    if (ACAPI_Element_ChangeMemo(guid, APIMemoMask_AddPars, &memo) == NoError)
                        ++updCount;
                    ACAPI_DisposeElemMemoHdls(&memo);
                }
                return NoError;
            });
    }
    return updCount;
}

// ── Auto-Placement eines LibParts ────────────────────────────────────────────
static void AutoPlaceEntry(const PointCloudEntry& entry)
{
    Int32 libIdx = entry.libIndex;
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "AutoPlaceEntry: libIdx=%d name=%s",
             libIdx, entry.name.ToCStr().Get());
    Log(logBuf);

    API_StoryInfo storyInfo = {};
    Compat_GetStorySettings(&storyInfo);
    short curFloor = storyInfo.actStory;

    API_Element     placeElem = {};
    API_ElementMemo placeMemo = {};
    placeElem.header.type = API_ObjectID;

    GSErrCode defErr = ACAPI_Element_GetDefaults(&placeElem, &placeMemo);
    if (defErr != NoError) {
        ACAPI_DisposeElemMemoHdls(&placeMemo);
        snprintf(logBuf, sizeof(logBuf), "GetDefaults: err=%d", defErr);
        Log(logBuf);
        return;
    }

    // LibPart-spezifische Parameter laden (ersetzt Generic-Defaults in memo)
    API_ParamOwnerType paramOwner = {};
    paramOwner.libInd = libIdx;
    if (Compat_OpenParameters(&paramOwner) == NoError) {
        API_GetParamsType getParams = {};
        if (Compat_GetActParameters(&getParams) == NoError) {
            ACAPI_DisposeAddParHdl(&placeMemo.params);
            placeMemo.params = getParams.params;
        }
        Compat_CloseParameters();
    }

    placeElem.header.floorInd = curFloor;
    placeElem.object.libInd   = libIdx;
    placeElem.object.pos.x    = 0.0;
    placeElem.object.pos.y    = 0.0;
    placeElem.object.angle    = 0.0;

    GSErrCode createErr = NoError;
    ACAPI_CallUndoableCommand("Punktwolke platzieren",
        [&]() -> GSErrCode {
            createErr = ACAPI_Element_Create(&placeElem, &placeMemo);
            return createErr;
        });
    snprintf(logBuf, sizeof(logBuf), "Element_Create: err=%d", createErr);
    Log(logBuf);
    ACAPI_DisposeElemMemoHdls(&placeMemo);
}

// ── Import (ohne Dichte-Dialog) ───────────────────────────────────────────────
// Gibt den baseName zurück (für Auto-Placement nach Refresh), oder "" bei Fehler/Abbruch.
static GS::UniString DoImport(int densityPct)
{
    Log("--- DoImport ---");
    const int STRIDE = std::max(1, 100 / densityPct);

    DGTypePopupItem filter;
    filter.Init();
    filter.text       = "E57-Punktwolke";
    filter.extensions = "e57";

    IO::Location e57Loc;
    if (!DGGetOpenFile(&e57Loc, 1, &filter, nullptr, GS::UniString("E57-Datei öffnen")))
        return GS::UniString();

    GS::UniString e57Path;
    e57Loc.ToPath(&e57Path);
    { char buf[512]; snprintf(buf, sizeof(buf), "E57: %s", e57Path.ToCStr().Get()); Log(buf); }

    IO::Name e57FileName;
    e57Loc.GetLastLocalName(&e57FileName);
    GS::UniString baseName = e57FileName.ToString();
    if (baseName.GetLength() > 4) {
        GS::UniString ext = baseName.GetSubstring(baseName.GetLength() - 4, 4);
        ext.SetToLowerCase();
        if (ext == ".e57")
            baseName = baseName.GetSubstring(0, baseName.GetLength() - 4);
    }

    // Elternverzeichnis plattform-unabhängig ermitteln (Windows: \, macOS: /)
    UInt32 lastSep = FindLastSep(e57Path);
    GS::UniString parentPathStr = (lastSep != GS::MaxUIndex)
        ? e57Path.GetSubstring(0, lastSep) : e57Path;

    IO::Location outputDir(parentPathStr);
    outputDir.AppendToLocal(IO::Name(baseName));
    IO::fileSystem.CreateFolder(outputDir);

    GS::UniString outputDirPath;
    outputDir.ToPath(&outputDirPath);

    // E57 → GSPC (pro Scan eine Datei)
    float   bboxX = 1, bboxY = 1, bboxZ = 1;
    float   offX  = 0, offY  = 0, offZ  = 0;
    int64_t totalOrigPoints = 0, totalWritten = 0;
    int64_t nScans = 0;
    bool    progressOpen = false;

    try {
        e57::Reader reader(std::string(e57Path.ToCStr().Get()), e57::ReaderOptions{});
        if (!reader.IsOpen()) {
            ACAPI_WriteReport("E57-Datei konnte nicht geöffnet werden.", true);
            return GS::UniString();
        }
        nScans = reader.GetData3DCount();
        if (nScans == 0) { ACAPI_WriteReport("E57-Datei enthält keine Scans.", true); return GS::UniString(); }

        int64_t totalExpected = 0;
        for (int64_t si2 = 0; si2 < nScans; ++si2) {
            e57::Data3D hdr2;
            if (reader.ReadData3D(si2, hdr2) && hdr2.pointCount > 0) {
                totalOrigPoints += hdr2.pointCount;
                totalExpected   += hdr2.pointCount / STRIDE;
            }
        }
        if (totalExpected <= 0) totalExpected = nScans * 500000LL;

        Int32 nPhases = 2;
        GS::UniString procTitle("E57 importieren...");
        Compat_InitProcessWindow(&procTitle, &nPhases);
        progressOpen = true;
        Int32 ptTotal    = (Int32)std::min(totalExpected, (int64_t)2000000000LL);
        Int32 updateStep = (Int32)std::max((int64_t)1, totalExpected / 200);
        GS::UniString phase1("Punktwolke konvertieren...");
        Compat_SetNextProcessPhase(&phase1, &ptTotal);

        const size_t CHUNK = 65536;
        std::vector<double>   x(CHUNK), y(CHUNK), z(CHUNK);
        std::vector<uint16_t> cr(CHUNK, 200), cg(CHUNK, 200), cb(CHUNK, 200);
        std::vector<int8_t>   colorInvalid(CHUNK, 0);
        bool cancelled = false;

        // Globale Bbox über alle Scans
        float gxmin =  1e30f, gxmax = -1e30f;
        float gymin =  1e30f, gymax = -1e30f;
        float gzmin =  1e30f, gzmax = -1e30f;

        for (int64_t si = 0; si < nScans && !cancelled; ++si) {
            e57::Data3D header;
            if (!reader.ReadData3D(si, header)) continue;

            // GSPC-Pfad für diesen Scan
            char scanSuffix[32];
            snprintf(scanSuffix, sizeof(scanSuffix), "_%lld.gspc", (long long)(si + 1));
            GS::UniString scanGspcName = baseName + GS::UniString(scanSuffix);
            IO::Location  scanGspcLoc  = outputDir;
            scanGspcLoc.AppendToLocal(IO::Name(scanGspcName));
            GS::UniString scanGspcPath;
            scanGspcLoc.ToPath(&scanGspcPath);

            GspcWriter gspc;
            if (!gspc.Open(scanGspcPath.ToCStr().Get())) {
                char buf[256]; snprintf(buf, sizeof(buf), "GSPC öffnen fehlgeschlagen: %s", scanGspcPath.ToCStr().Get());
                Log(buf);
                continue;
            }

            bool hasColor = header.pointFields.colorRedField
                         && header.pointFields.colorGreenField
                         && header.pointFields.colorBlueField;
            e57::Data3DPointsDouble buffers;
            buffers.cartesianX = x.data(); buffers.cartesianY = y.data(); buffers.cartesianZ = z.data();
            if (hasColor) {
                buffers.colorRed = cr.data(); buffers.colorGreen = cg.data();
                buffers.colorBlue = cb.data(); buffers.isColorInvalid = colorInvalid.data();
            }
            auto cvr = reader.SetUpData3DPointsData(si, CHUNK, buffers);
            uint64_t nRead;
            int64_t scanLocalIdx = 0;
            while ((nRead = cvr.read()) > 0) {
                for (size_t i = 0; i < (size_t)nRead; ++i) {
                    if (scanLocalIdx % STRIDE == 0) {
                        double wx, wy, wz;
                        ApplyPose(header.pose, x[i], y[i], z[i], wx, wy, wz);
                        uint8_t r = 200, g = 200, b = 200;
                        if (hasColor && !colorInvalid[i]) {
                            r = (uint8_t)std::min<int>((int)cr[i], 255);
                            g = (uint8_t)std::min<int>((int)cg[i], 255);
                            b = (uint8_t)std::min<int>((int)cb[i], 255);
                        }
                        gspc.Write((float)wx, (float)wy, (float)wz, r, g, b);
                        ++totalWritten;
                        if (totalWritten % updateStep == 0) {
                            Int32 prog = (Int32)std::min(totalWritten, (int64_t)ptTotal);
                            Compat_SetProcessValue(&prog);
                            if (Compat_IsProcessCanceled()) { cancelled = true; break; }
                        }
                    }
                    ++scanLocalIdx;
                }
            }
            cvr.close();
            gspc.Close();

            // Globale Bbox aktualisieren
            if (!gspc.firstPt) {
                if (gspc.xmin < gxmin) gxmin = gspc.xmin;
                if (gspc.xmax > gxmax) gxmax = gspc.xmax;
                if (gspc.ymin < gymin) gymin = gspc.ymin;
                if (gspc.ymax > gymax) gymax = gspc.ymax;
                if (gspc.zmin < gzmin) gzmin = gspc.zmin;
                if (gspc.zmax > gzmax) gzmax = gspc.zmax;
            }
        }
        reader.Close();

        if (cancelled) {
            Compat_CloseProcessWindow();
            ACAPI_WriteReport("Import abgebrochen.", false);
            return GS::UniString();
        }

        if (gxmin <= gxmax) {
            offX  = gxmin; offY  = gymin; offZ  = gzmin;
            bboxX = gxmax - gxmin;
            bboxY = gymax - gymin;
            bboxZ = gzmax - gzmin;
        }

    } catch (const std::exception& ex) {
        if (progressOpen) Compat_CloseProcessWindow();
        char buf[256]; snprintf(buf, sizeof(buf), "E57-Fehler: %s", ex.what()); Log(buf);
        ACAPI_WriteReport("E57-Konvertierung fehlgeschlagen.", true);
        return GS::UniString();
    }

    // GSM erstellen
    {
        Int32 gsmWork = 1;
        GS::UniString phase2("GSM erstellen...");
        Compat_SetNextProcessPhase(&phase2, &gsmWork);

        bool ok = WriteGsmViaXml(
            (int)nScans, baseName.ToCStr().Get(),
            outputDirPath.ToCStr().Get(),
            bboxX, bboxY, bboxZ,
            offX, offY, offZ,
            densityPct, e57Path.ToCStr().Get(),
            totalOrigPoints, totalWritten);

        Int32 done = 1;
        Compat_SetProcessValue(&done);
        Compat_CloseProcessWindow();

        if (!ok) {
            Log("WriteGsmViaXml fehlgeschlagen.");
            ACAPI_WriteReport("GSM konnte nicht erstellt werden.", true);
            return GS::UniString();
        }
    }

    RegisterLibraryDir(outputDir);
    Compat_ReloadLibraries();

    GS::UniString msg = GS::UniString("\"") + baseName
        + GS::UniString("\" importiert (")
        + GS::UniString(std::to_string(densityPct).c_str())
        + GS::UniString("% der Punkte).");
    ACAPI_WriteReport(msg.ToCStr().Get(), false);
    return baseName;  // Erfolg: baseName für Auto-Placement nach Refresh()
}

// ── Dichte anpassen ──────────────────────────────────────────────────────────
static bool DoAdjustDensity(const PointCloudEntry& sel, int newDensity)
{
    Log("--- DoAdjustDensity ---");
    const int STRIDE = std::max(1, 100 / newDensity);

    int64_t totalWritten = 0;
    bool    progressOpen = false;

    try {
        e57::Reader reader(std::string(sel.e57Path.ToCStr().Get()), e57::ReaderOptions{});
        if (!reader.IsOpen()) {
            ACAPI_WriteReport("E57-Datei konnte nicht geöffnet werden.", true);
            return false;
        }
        int64_t nScans = reader.GetData3DCount();
        if (nScans == 0) { ACAPI_WriteReport("E57-Datei enthält keine Scans.", true); return false; }

        IO::fileSystem.CreateFolder(IO::Location(sel.outputDir));

        int64_t totalExpected = 0;
        for (int64_t si2 = 0; si2 < nScans; ++si2) {
            e57::Data3D hdr2;
            if (reader.ReadData3D(si2, hdr2) && hdr2.pointCount > 0)
                totalExpected += hdr2.pointCount / STRIDE;
        }
        if (totalExpected <= 0) totalExpected = nScans * 500000LL;

        Int32 nPhases = 1;
        GS::UniString procTitle("Punktwolke reimportieren...");
        Compat_InitProcessWindow(&procTitle, &nPhases);
        progressOpen = true;
        Int32 ptTotal    = (Int32)std::min(totalExpected, (int64_t)2000000000LL);
        Int32 updateStep = (Int32)std::max((int64_t)1, totalExpected / 200);
        GS::UniString phase1("Punktwolke konvertieren...");
        Compat_SetNextProcessPhase(&phase1, &ptTotal);

        const size_t CHUNK = 65536;
        std::vector<double>   x(CHUNK), y(CHUNK), z(CHUNK);
        std::vector<uint16_t> cr(CHUNK, 200), cg(CHUNK, 200), cb(CHUNK, 200);
        std::vector<int8_t>   colorInvalid(CHUNK, 0);
        bool cancelled = false;

        for (int64_t si = 0; si < nScans && !cancelled; ++si) {
            e57::Data3D header;
            if (!reader.ReadData3D(si, header)) continue;

            // GSPC-Pfad für diesen Scan (neues Format: baseName_N.gspc)
            char scanSuffix[32];
            snprintf(scanSuffix, sizeof(scanSuffix), "_%lld.gspc", (long long)(si + 1));
            GS::UniString scanGspcName = sel.name + GS::UniString(scanSuffix);
            IO::Location  scanGspcLoc(sel.outputDir);
            scanGspcLoc.AppendToLocal(IO::Name(scanGspcName));
            GS::UniString scanGspcPath;
            scanGspcLoc.ToPath(&scanGspcPath);

            GspcWriter gspc;
            if (!gspc.Open(scanGspcPath.ToCStr().Get())) {
                char buf[256]; snprintf(buf, sizeof(buf), "GSPC öffnen fehlgeschlagen: %s", scanGspcPath.ToCStr().Get());
                Log(buf);
                continue;
            }

            bool hasColor = header.pointFields.colorRedField
                         && header.pointFields.colorGreenField
                         && header.pointFields.colorBlueField;
            e57::Data3DPointsDouble buffers;
            buffers.cartesianX = x.data(); buffers.cartesianY = y.data(); buffers.cartesianZ = z.data();
            if (hasColor) {
                buffers.colorRed = cr.data(); buffers.colorGreen = cg.data();
                buffers.colorBlue = cb.data(); buffers.isColorInvalid = colorInvalid.data();
            }
            auto cvr = reader.SetUpData3DPointsData(si, CHUNK, buffers);
            uint64_t nRead;
            int64_t scanLocalIdx = 0;
            while ((nRead = cvr.read()) > 0) {
                for (size_t i = 0; i < (size_t)nRead; ++i) {
                    if (scanLocalIdx % STRIDE == 0) {
                        double wx, wy, wz;
                        ApplyPose(header.pose, x[i], y[i], z[i], wx, wy, wz);
                        uint8_t r = 200, g = 200, b = 200;
                        if (hasColor && !colorInvalid[i]) {
                            r = (uint8_t)std::min<int>((int)cr[i], 255);
                            g = (uint8_t)std::min<int>((int)cg[i], 255);
                            b = (uint8_t)std::min<int>((int)cb[i], 255);
                        }
                        gspc.Write((float)wx, (float)wy, (float)wz, r, g, b);
                        ++totalWritten;
                        if (totalWritten % updateStep == 0) {
                            Int32 prog = (Int32)std::min(totalWritten, (int64_t)ptTotal);
                            Compat_SetProcessValue(&prog);
                            if (Compat_IsProcessCanceled()) { cancelled = true; break; }
                        }
                    }
                    ++scanLocalIdx;
                }
            }
            cvr.close();
            gspc.Close();
        }
        reader.Close();

        if (cancelled) {
            Compat_CloseProcessWindow();
            return false;
        }


    } catch (const std::exception& ex) {
        if (progressOpen) Compat_CloseProcessWindow();
        char buf[256]; snprintf(buf, sizeof(buf), "Exception: %s", ex.what()); Log(buf);
        ACAPI_WriteReport("Fehler beim Einlesen der E57-Datei.", true);
        return false;
    }

    Compat_CloseProcessWindow();

    // Parameter in platzierten Instanzen VOR Reload aktualisieren
    int updCount = UpdatePlacedParams(sel.libIndex, newDensity, totalWritten);
    { char buf[64]; snprintf(buf, sizeof(buf), "updCount=%d", updCount); Log(buf); }

    ReloadLibraries();

    GS::UniString msg = GS::UniString("\"") + sel.name
        + GS::UniString("\" neu importiert mit ")
        + GS::UniString(std::to_string(newDensity).c_str())
        + GS::UniString("% der Punkte");
    if (updCount > 0)
        msg += GS::UniString(" (") + GS::UniString(std::to_string(updCount).c_str())
             + GS::UniString(" Objekt(e) aktualisiert).");
    else
        msg += GS::UniString(".");
    ACAPI_WriteReport(msg.ToCStr().Get(), false);
    return true;
}

// ── Eintrag löschen (mit Bestätigung) ────────────────────────────────────────
static bool TryDeleteEntry(const PointCloudEntry& entry)
{
    // Platzierte Instanzen im Modell zählen
    GS::Array<API_Guid> elemList;
    ACAPI_Element_GetElemList(API_ObjectID, &elemList);
    GS::Array<API_Guid> toDelete;
    for (const auto& guid : elemList) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get(&elem) != NoError) continue;
        if (elem.object.libInd == entry.libIndex)
            toDelete.Push(guid);
    }

    // Bestätigungsdialog mit Hinweis auf Anzahl platzierter Objekte
    GS::UniString subText;
    if (toDelete.GetSize() > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "%d platziertes Objekt%s wird gelöscht. GSM + GSPC werden von der Festplatte entfernt.",
            (int)toDelete.GetSize(), toDelete.GetSize() == 1 ? "" : "e");
        subText = GS::UniString(buf);
    } else {
        subText = GS::UniString("GSM + GSPC-Dateien werden gelöscht. Die E57-Quelldatei bleibt erhalten.");
    }

    GS::UniString question = GS::UniString("\"") + entry.name + GS::UniString("\" löschen?");
    short result = DGAlert(DG_WARNING,
        GS::UniString("Punktwolke löschen"),
        question,
        subText,
        GS::UniString("Löschen"),
        GS::UniString("Abbrechen"));
    if (result != 1) return false;

    // 1. Platzierte Objekte in ArchiCAD löschen
    if (toDelete.GetSize() > 0) {
        ACAPI_CallUndoableCommand("Punktwolke löschen",
            [&]() -> GSErrCode {
                return ACAPI_Element_Delete(toDelete);
            });
    }

    // 2. Dateien + Ordner löschen (IO::Location für plattform-native Pfadtrenner)
    IO::Location dirLoc(entry.outputDir);
    IO::Location gsmLoc = dirLoc;
    gsmLoc.AppendToLocal(IO::Name(entry.name + ".gsm"));
    GS::UniString gsmPath;
    gsmLoc.ToPath(&gsmPath);
    remove(gsmPath.ToCStr().Get());

    int nToDelete = (entry.scanCount > 0) ? entry.scanCount : 1;
    for (int sc = 0; sc < nToDelete; ++sc) {
        IO::Location gspcLoc = dirLoc;
        gspcLoc.AppendToLocal(IO::Name(entry.GspcName(sc)));
        GS::UniString gspcPath;
        gspcLoc.ToPath(&gspcPath);
        remove(gspcPath.ToCStr().Get());
    }
    rmdir(entry.outputDir.ToCStr().Get());

    // 3. Aus Bibliotheks-Manager entfernen + Reload
    UnregisterLibraryDir(entry.outputDir);

    char buf[512];
    snprintf(buf, sizeof(buf), "Gelöscht: %s (%d Objekt%s entfernt)",
        entry.outputDir.ToCStr().Get(),
        (int)toDelete.GetSize(), toDelete.GetSize() == 1 ? "" : "e");
    Log(buf);
    return true;
}

// ── Punktwolken-Palette ───────────────────────────────────────────────────────

// Modul-globale Konstanten für Palette-Registrierung
static const GS::Guid kPaletteGuid("C7D4E5F6-1A2B-3C4D-8E9F-0A1B2C3D4E5F");
static const Int32    kPaletteRefId = 0x50574443;  // "PWDC"

static const int PAL_W  = 354;

// Item-IDs (entsprechen Reihenfolge im GDLG-Resource 32400)
enum PalItemId {
    kHeaderLabelId  = 1,   // "Punktwolken"-Beschriftung oben
    kAddBtnId       = 2,
    kDefDensEditId  = 3,
    kDefDensLabelId = 4,
    kDelBtnId       = 5,   // oben rechts
    kListBoxId      = 6,
    kRelBtnId       = 7,
    kSelDensEditId  = 8,
    kSelDensLabelId = 9
};

class PunktwolkenPalette
    : public DG::Palette
    , public DG::PanelObserver
    , public DG::ButtonItemObserver
    , public DG::ListBoxObserver
{
    // Items aus Resource (GetReference() + Item-ID)
    DG::LeftText         headerLabel;
    DG::Button           addBtn;
    DG::IntEdit          defDensEdit;
    DG::LeftText         defDensLabel;
    DG::IconButton       delBtn;        // oben rechts
    DG::SingleSelListBox listBox;
    DG::IconButton       relBtn;
    DG::IntEdit          selDensEdit;
    DG::LeftText         selDensLabel;

    GS::Array<PointCloudEntry> entries;
    int  defaultDensity = 10;
    bool importing      = false;  // Re-entrance-Guard: DGGetOpenFile kann ButtonClicked nochmal auslösen

    void UpdateFooter()
    {
        short sel   = listBox.GetSelectedItem();
        int   total = (int)entries.GetSize();

        if (sel > 0 && sel <= (short)total) {
            selDensEdit.SetValue(entries[sel - 1].densityPercent);
            relBtn.Enable();
            selDensEdit.Enable();
            delBtn.Enable();
        } else {
            relBtn.Disable();
            selDensEdit.Disable();
            delBtn.Disable();
        }
    }

public:
    PunktwolkenPalette()
        : DG::Palette(ACAPI_GetOwnResModule(), 32400, ACAPI_GetOwnResModule(), kPaletteGuid)
        , headerLabel (GetReference(), kHeaderLabelId)
        , addBtn      (GetReference(), kAddBtnId)
        , defDensEdit (GetReference(), kDefDensEditId)
        , defDensLabel(GetReference(), kDefDensLabelId)
        , delBtn      (GetReference(), kDelBtnId)
        , listBox     (GetReference(), kListBoxId)
        , relBtn      (GetReference(), kRelBtnId)
        , selDensEdit (GetReference(), kSelDensEditId)
        , selDensLabel(GetReference(), kSelDensLabelId)
    {
        Log("Palette-Konstruktor: Items initialisiert.");

        defDensEdit.SetMin(1); defDensEdit.SetMax(100);
        defDensEdit.SetValue(defaultDensity);

        listBox.SetTabFieldCount(1);
        listBox.SetTabFieldBeginEndPosition(1, 0, PAL_W - 15);

        selDensEdit.SetMin(1); selDensEdit.SetMax(100);
        selDensEdit.SetValue(25);

        SetMinClientSize(100, 150);

        this->Attach  (*this);
        addBtn.Attach (*this);
        relBtn.Attach (*this);
        delBtn.Attach (*this);
        listBox.Attach(*this);

        relBtn.Disable();
        selDensEdit.Disable();
        delBtn.Disable();

        Log("Palette-Konstruktor: rufe BeginEventProcessing...");
        BeginEventProcessing();
        Log("Palette-Konstruktor: fertig.");
    }

    ~PunktwolkenPalette()
    {
        EndEventProcessing();
        listBox.Detach(*this);
        this->Detach  (*this);
        addBtn.Detach (*this);
        relBtn.Detach (*this);
        delBtn.Detach (*this);
    }

    // Bibliothek neu einlesen + Liste neu aufbauen
    void Refresh()
    {
        entries = CollectOurLibParts();

        listBox.DeleteItem(DG::ListBox::AllItems);
        for (int i = 0; i < (int)entries.GetSize(); ++i) {
            GS::UniString text = entries[i].name
                + GS::UniString(" - ")
                + GS::UniString(std::to_string(entries[i].densityPercent).c_str())
                + GS::UniString("%");
            listBox.AppendItem();
            listBox.SetTabItemText((short)(i + 1), 1, text);
        }

        UpdateFooter();
    }

    // ── PanelObserver ─────────────────────────────────────────────────────────

    void PanelOpened(const DG::PanelOpenEvent&) override
    {
        Log("PanelOpened aufgerufen.");
        SetClientSize(GetOriginalClientWidth(), GetOriginalClientHeight());
        Refresh();
    }

    void PanelResized(const DG::PanelResizeEvent& ev) override
    {
        short dh = ev.GetHorizontalChange();
        short dv = ev.GetVerticalChange();
        BeginMoveResizeItems();
        delBtn.Move(dh, 0);                 // Lösch-Icon bleibt am rechten Rand
        listBox.Resize(dh, dv);             // Liste wächst in beide Richtungen
        relBtn.Move(0, dv);                 // Reload-Icon bleibt am unteren Rand
        selDensEdit.Move(0, dv);
        selDensLabel.Move(0, dv);
        EndMoveResizeItems();
        listBox.SetTabFieldBeginEndPosition(1, 0, listBox.GetItemWidth());
    }

    void PanelCloseRequested(const DG::PanelCloseRequestEvent&, bool* accepted) override
    {
        Log("PanelCloseRequested aufgerufen → Hide()");
        *accepted = false;
        Hide();
    }

    // ── ListBoxObserver ───────────────────────────────────────────────────────

    void ListBoxSelectionChanged(const DG::ListBoxSelectionEvent& ev) override
    {
        if (ev.GetSource() == &listBox)
            UpdateFooter();
    }

    // ── ButtonItemObserver ────────────────────────────────────────────────────

    void ButtonClicked(const DG::ButtonClickEvent& ev) override
    {
        Log("ButtonClicked aufgerufen");
        const DG::Item* src = ev.GetSource();
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "  src=%p  addBtn=%p  relBtn=%p  delBtn=%p",
                     (void*)src, (void*)&addBtn, (void*)&relBtn, (void*)&delBtn);
            Log(buf);
        }

        if (src == &addBtn) {
            if (importing) return;
            importing = true;
            defaultDensity = (int)defDensEdit.GetValue();
            GS::UniString newName = DoImport(defaultDensity);
            Refresh();  // Library ist jetzt geladen → CollectOurLibParts findet den neuen LibPart
            if (!newName.IsEmpty()) {
                for (UInt32 i = 0; i < entries.GetSize(); ++i) {
                    if (entries[i].name == newName) {
                        AutoPlaceEntry(entries[i]);
                        break;
                    }
                }
            }
            importing = false;
            return;
        }

        short sel = listBox.GetSelectedItem();  // 1-based
        int   idx = (int)sel - 1;               // 0-based

        if (src == &relBtn && sel > 0 && idx < (int)entries.GetSize()) {
            int newDens = (int)selDensEdit.GetValue();
            bool ok = DoAdjustDensity(entries[idx], newDens);
            if (ok) {
                entries[idx].densityPercent = newDens;
                GS::UniString text = entries[idx].name
                    + GS::UniString(" - ")
                    + GS::UniString(std::to_string(newDens).c_str())
                    + GS::UniString("%");
                listBox.SetTabItemText(sel, 1, text);
                UpdateFooter();
            }
            return;
        }

        if (src == &delBtn && sel > 0 && idx < (int)entries.GetSize()) {
            bool deleted = TryDeleteEntry(entries[idx]);
            if (deleted) Refresh();
            return;
        }
    }

    // ── Palette-Registrierung ─────────────────────────────────────────────────

    // Konstruktion NUR beim ersten Menüklick (nicht aus ControlCallback heraus)
    static PunktwolkenPalette* s_instance;

    static PunktwolkenPalette& GetInstance()
    {
        if (!s_instance)
            s_instance = new PunktwolkenPalette();
        return *s_instance;
    }

    static GSErrCode __ACENV_CALL ControlCallback(
        Int32 /*refId*/, API_PaletteMessageID msg, GS::IntPtr param)
    {
        // Palette noch nicht per Menüklick initialisiert → nur Sichtbarkeit melden
        if (!s_instance) {
            if (msg == APIPalMsg_IsPaletteVisible)
                *reinterpret_cast<bool*>(param) = false;
            return NoError;
        }
        PunktwolkenPalette& pal = *s_instance;
        switch (msg) {
            case APIPalMsg_IsPaletteVisible:   *reinterpret_cast<bool*>(param) = pal.IsVisible(); break;
            case APIPalMsg_OpenPalette:        pal.Show();             break;
            case APIPalMsg_ClosePalette:       pal.SendCloseRequest(); break;
            case APIPalMsg_HidePalette_Begin:  pal.Hide();             break;
            case APIPalMsg_HidePalette_End:    pal.Show();             break;
            case APIPalMsg_DisableItems_Begin: pal.DisableItems();     break;
            case APIPalMsg_DisableItems_End:   pal.EnableItems();      break;
            default:                                                    break;
        }
        return NoError;
    }
};

PunktwolkenPalette* PunktwolkenPalette::s_instance = nullptr;

// ── Add-On Einstiegspunkte ────────────────────────────────────────────────────

static GSErrCode MenuCommandHandler(const API_MenuParams* p)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "MenuCommandHandler resID=%d itemIndex=%d",
             p->menuItemRef.menuResID, p->menuItemRef.itemIndex);
    Log(buf);

    if (p->menuItemRef.menuResID == ID_ADDON_MENU && p->menuItemRef.itemIndex == 1) {
        PunktwolkenPalette& pal = PunktwolkenPalette::GetInstance();
        pal.Show();
        pal.Refresh();
        Log("MenuHandler: Show+Refresh done.");
    }
    return NoError;
}

API_AddonType CheckEnvironment(API_EnvirParams* envir)
{
    RSGetIndString(&envir->addOnInfo.name,        ID_ADDON_INFO, 1, ACAPI_GetOwnResModule());
    RSGetIndString(&envir->addOnInfo.description, ID_ADDON_INFO, 2, ACAPI_GetOwnResModule());
    return APIAddon_Preload;
}

GSErrCode RegisterInterface()
{
    ACAPI_Register_Menu(ID_ADDON_MENU, 32600, MenuCode_UserDef, MenuFlag_Default);
    return NoError;
}

GSErrCode Initialize()
{
    Log("Initialize() aufgerufen.");
    ACAPI_Install_MenuHandler(ID_ADDON_MENU, MenuCommandHandler);
    ACAPI_RegisterModelessWindow(
        kPaletteRefId,
        PunktwolkenPalette::ControlCallback,
        API_PalEnabled_FloorPlan | API_PalEnabled_Section | API_PalEnabled_Elevation |
        API_PalEnabled_InteriorElevation | API_PalEnabled_3D |
        API_PalEnabled_Detail | API_PalEnabled_Worksheet | API_PalEnabled_Layout |
        API_PalEnabled_DocumentFrom3D,
        GSGuid2APIGuid(kPaletteGuid));
    Log("ModelessWindow registriert.");
    return NoError;
}

GSErrCode FreeData()
{
    ACAPI_UnregisterModelessWindow(kPaletteRefId);
    return NoError;
}
