// *****************************************************************************
// E57Importer — E57-Punktwolken importieren, GSPC/GSM erzeugen
// *****************************************************************************

#include "E57Importer.hpp"

#include "File.hpp"
#include "FileSystem.hpp"
#include "HashTable.hpp"

#include "DGDLLExport.h"
#include "DGFileDlg.hpp"
#include "DGFileDialog.hpp"
#include "DGDialog.hpp"

#include "E57SimpleReader.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
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
#  include <mach-o/dyld.h>
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

// Debug-Log im Benutzerkontext, nicht im Installationsordner:
//   macOS:   ~/Library/Logs/Pointcloud4Archicad.log
//   Windows: %TEMP%\Pointcloud4Archicad.log

static const char* GetLogPath ()
{
    static char path[1024] = {};
    if (path[0] != '\0')
        return path;
#ifdef _WIN32
    char tmp[MAX_PATH] = "C:\\Temp\\";
    GetTempPathA (MAX_PATH, tmp);
    snprintf (path, sizeof (path), "%sPointcloud4Archicad.log", tmp);
#else
    const char* home = getenv ("HOME");
    snprintf (path, sizeof (path), "%s/Library/Logs/Pointcloud4Archicad.log", home ? home : "/tmp");
#endif
    return path;
}

// =============================================================================
// Interne Hilfsfunktionen
// =============================================================================

// --- Logging -----------------------------------------------------------------

void Log (const char* msg)
{
    FILE* f = fopen (GetLogPath (), "a");
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
// Ohne LP_XMLConverter entsteht kein GSM. Suchreihenfolge:
//   1. vom Benutzer gewaehlter Pfad (Add-on-Preferences, gilt ueber Sitzungen hinweg)
//   2. neben der laufenden Archicad-Programmdatei:
//        Windows: <Installationsordner>\LP_XMLConverter.exe
//        macOS:   <Archicad.app>/Contents/MacOS/LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter
//   3. feste Standardpfade (beide Schreibweisen des Ordners)
// Findet sich nichts, fragt EnsureLpXmlConverter () per Ordnerdialog nach dem
// Archicad-Programmordner — und zwar VOR dem Import, nicht erst nach dem E57-Lesen.
// (Frueher stand fuer Windows/AC26 "ARCHICAD26" ohne Leerzeichen fest im Code —
// der Converter wurde nie gefunden, es entstand kein GSM.)

#ifdef _WIN32
#  ifdef ServerMainVers_2900
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "C:\\Program Files\\GRAPHISOFT\\Archicad 29\\LP_XMLConverter.exe",
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD29\\LP_XMLConverter.exe" };
#  elif defined(ServerMainVers_2800)
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "C:\\Program Files\\GRAPHISOFT\\Archicad 28\\LP_XMLConverter.exe",
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD28\\LP_XMLConverter.exe" };
#  elif defined(ServerMainVers_2700)
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "C:\\Program Files\\GRAPHISOFT\\Archicad 27\\LP_XMLConverter.exe",
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD27\\LP_XMLConverter.exe" };
#  else
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "C:\\Program Files\\GRAPHISOFT\\Archicad 26\\LP_XMLConverter.exe",
    "C:\\Program Files\\GRAPHISOFT\\ARCHICAD26\\LP_XMLConverter.exe" };
#  endif
#else
#  ifdef ServerMainVers_2900
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "/Applications/GRAPHISOFT/ARCHICAD29/Archicad 29.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter" };
#  elif defined(ServerMainVers_2800)
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "/Applications/GRAPHISOFT/ARCHICAD28/Archicad 28.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter" };
#  elif defined(ServerMainVers_2700)
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "/Applications/GRAPHISOFT/ARCHICAD27/Archicad 27.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter" };
#  else
static const char* LP_XMLCONVERTER_FALLBACK [] = {
    "/Applications/GRAPHISOFT/Archicad 26/Archicad 26.app/Contents/MacOS/"
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter" };
#  endif
#endif

#ifdef _WIN32
static std::wstring ToWide (const GS::UniString& s)
{
    // UniChar ist unter Windows UTF-16 wie wchar_t
    return std::wstring (reinterpret_cast<const wchar_t*> (s.ToUStr ().Get ()));
}

static bool ConverterExists (const GS::UniString& path)
{
    if (path.IsEmpty ()) return false;
    DWORD a = GetFileAttributesW (ToWide (path).c_str ());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// char-Pfade kommen aus GS::UniString::ToCStr () und sind damit in der
// ANSI-Codepage (CC_Default) — nicht UTF-8.
static std::wstring AnsiToWide (const char* s)
{
    int n = MultiByteToWideChar (CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return std::wstring ();
    std::wstring w ((size_t)n, L'\0');
    MultiByteToWideChar (CP_ACP, 0, s, -1, &w[0], n);
    w.resize ((size_t)n - 1);
    return w;
}

// Fuer Text in der als UTF-8 deklarierten XML-Datei: ANSI → UTF-8.
// Ohne diese Umwandlung bricht LP_XMLConverter bei Umlauten im
// Datei- oder Ordnernamen mit einem XML-Fehler ab.
static std::string AnsiToUtf8 (const char* s)
{
    std::wstring w = AnsiToWide (s);
    int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str (), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string ();
    std::string u ((size_t)n, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w.c_str (), -1, &u[0], n, nullptr, nullptr);
    u.resize ((size_t)n - 1);
    return u;
}
#else
static const char* LP_XMLCONVERTER_SUBPATH_MAC =
    "LP_XMLConverter.app/Contents/MacOS/LP_XMLConverter";

static std::string ToUtf8 (const GS::UniString& s)
{
    return std::string (s.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

static bool ConverterExists (const GS::UniString& path)
{
    return !path.IsEmpty () && access (ToUtf8 (path).c_str (), X_OK) == 0;
}
#endif

static GS::UniString JoinPath (const GS::UniString& dir, const char* name)
{
    GS::UniString p = dir;
    if (!p.IsEmpty () && !p.EndsWith (GS::UniString ("/")) && !p.EndsWith (GS::UniString ("\\")))
        p += GS::UniString (PATH_SEP == '\\' ? "\\" : "/");
    p += GS::UniString (name);
    return p;
}

// Converter in einem vom Benutzer gewaehlten Ordner suchen. Akzeptiert werden
// der Archicad-Installationsordner und — auf dem Mac — auch das Programmpaket
// selbst oder dessen Contents/MacOS.
static GS::UniString ConverterInFolder (const GS::UniString& folder)
{
#ifdef _WIN32
    GS::UniString p = JoinPath (folder, "LP_XMLConverter.exe");
    return ConverterExists (p) ? p : GS::UniString ();
#else
    const char* subs[] = { "", "Contents/MacOS/" };
    for (const char* sub : subs) {
        GS::UniString p = JoinPath (folder, (std::string (sub) + LP_XMLCONVERTER_SUBPATH_MAC).c_str ());
        if (ConverterExists (p)) return p;
    }
    // Installationsordner: darin liegt "Archicad NN.app"
    if (DIR* d = opendir (ToUtf8 (folder).c_str ())) {
        GS::UniString found;
        while (struct dirent* ent = readdir (d)) {
            std::string n (ent->d_name);
            if (n.size () < 5 || n.compare (n.size () - 4, 4, ".app") != 0) continue;
            GS::UniString p = JoinPath (JoinPath (folder, n.c_str ()),
                                        (std::string ("Contents/MacOS/") + LP_XMLCONVERTER_SUBPATH_MAC).c_str ());
            if (ConverterExists (p)) { found = p; break; }
        }
        closedir (d);
        return found;
    }
    return GS::UniString ();
#endif
}

static GS::UniString AutoFindConverter ()
{
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW (nullptr, exe, (DWORD)(sizeof (exe) / sizeof (exe[0])));
    if (n > 0 && n < sizeof (exe) / sizeof (exe[0])) {
        GS::UniString p (reinterpret_cast<const GS::UniChar::Layout*> (exe), n);
        UInt32 sep = FindLastSep (p);
        if (sep != GS::MaxUIndex) {
            GS::UniString c = ConverterInFolder (p.GetSubstring (0, sep));
            if (!c.IsEmpty ()) return c;
        }
    }
#else
    char exe[2048];
    uint32_t size = sizeof (exe);
    if (_NSGetExecutablePath (exe, &size) == 0) {
        GS::UniString p (exe, CC_UTF8);
        UInt32 sep = FindLastSep (p);
        if (sep != GS::MaxUIndex) {
            GS::UniString c = ConverterInFolder (p.GetSubstring (0, sep));
            if (!c.IsEmpty ()) return c;
        }
    }
#endif
    for (const char* f : LP_XMLCONVERTER_FALLBACK) {
        GS::UniString p (f);
        if (ConverterExists (p)) return p;
    }
    return GS::UniString ();
}

// --- Gespeicherter Converter-Pfad (ACAPI-Preferences, programmweit) ----------

struct PwPrefs {
    Int32 version;
    char  converterPath[2048];   // UTF-8, leer = automatisch suchen
};
static const Int32 kPwPrefsVersion = 1;

static GS::UniString LoadSavedConverterPath ()
{
    Int32  version = 0;
    GSSize nBytes  = 0;
    if (ACAPI_GetPreferences (&version, &nBytes, nullptr) != NoError
        || version != kPwPrefsVersion || nBytes != (GSSize)sizeof (PwPrefs))
        return GS::UniString ();
    PwPrefs prefs = {};
    if (ACAPI_GetPreferences (&version, &nBytes, &prefs) != NoError)
        return GS::UniString ();
    prefs.converterPath[sizeof (prefs.converterPath) - 1] = '\0';
    return GS::UniString (prefs.converterPath, CC_UTF8);
}

static void SaveConverterPath (const GS::UniString& path)
{
    PwPrefs prefs = {};
    prefs.version = kPwPrefsVersion;
    snprintf (prefs.converterPath, sizeof (prefs.converterPath), "%s",
              path.ToCStr (0, MaxUSize, CC_UTF8).Get ());
    ACAPI_SetPreferences (kPwPrefsVersion, sizeof (PwPrefs), &prefs);
}

static GS::UniString gConverterPath;   // in dieser Sitzung gefunden/gewaehlt

// Prueft, ob der Converter auffindbar ist; fragt sonst den Benutzer.
// false = abgebrochen, Import nicht starten.
static bool EnsureLpXmlConverter ()
{
    if (ConverterExists (gConverterPath))
        return true;

    GS::UniString saved = LoadSavedConverterPath ();
    if (ConverterExists (saved)) {
        gConverterPath = saved;
        return true;
    }

    gConverterPath = AutoFindConverter ();
    if (!gConverterPath.IsEmpty ()) {
        Log (GS::UniString (GS::UniString ("LP_XMLConverter: ") + gConverterPath).ToCStr ().Get ());
        return true;
    }

    Log ("LP_XMLConverter nicht gefunden — frage Benutzer nach dem Archicad-Ordner.");
#ifdef _WIN32
    const GS::UniString hinweis ("Bitte den Programmordner dieser Archicad-Version waehlen "
                                 "(enthaelt LP_XMLConverter.exe), z. B. C:\\Program Files\\GRAPHISOFT\\Archicad 26.");
#else
    const GS::UniString hinweis ("Bitte den Programmordner dieser Archicad-Version waehlen, "
                                 "z. B. /Applications/GRAPHISOFT/Archicad 26.");
#endif
    short antwort = DGAlert (DG_WARNING,
        GS::UniString ("LP_XMLConverter nicht gefunden"),
        GS::UniString ("Ohne LP_XMLConverter kann das Punktwolken-Objekt nicht erzeugt werden."),
        hinweis,
        GS::UniString ("Ordner waehlen..."),
        GS::UniString ("Abbrechen"));
    if (antwort != 1)
        return false;

    while (true) {
        DG::FileDialog dlg (DG::FileDialog::OpenFolder);
        dlg.SetTitle (GS::UniString ("Archicad-Programmordner waehlen"));
        if (!dlg.Invoke ())
            return false;

        GS::UniString folder;
        dlg.GetSelectedFolder ().ToPath (&folder);
        GS::UniString found = ConverterInFolder (folder);
        if (!found.IsEmpty ()) {
            gConverterPath = found;
            SaveConverterPath (found);
            Log (GS::UniString (GS::UniString ("LP_XMLConverter vom Benutzer gewaehlt: ") + found).ToCStr ().Get ());
            return true;
        }

        antwort = DGAlert (DG_ERROR,
            GS::UniString ("LP_XMLConverter nicht gefunden"),
            GS::UniString ("Im gewaehlten Ordner liegt kein LP_XMLConverter:"),
            folder,
            GS::UniString ("Anderen Ordner..."),
            GS::UniString ("Abbrechen"));
        if (antwort != 1)
            return false;
    }
}

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

// --- Existiert die Datei? (portabel, ohne access/F_OK) ----------------------

static bool FileExists (const char* path)
{
    FILE* f = fopen (path, "rb");
    if (f == nullptr) return false;
    fclose (f);
    return true;
}

// Pfad einer Scan-GSPC zusammensetzen: <dir>/<base>_<DDD>_<nr>.gspc
static void GspcPathOf (char* out, size_t outSize, const char* dir, const char* base,
                         int densityPct, int scanNr1)
{
    snprintf (out, outSize, "%s%c%s_%03d_%d.gspc", dir, PATH_SEP, base, densityPct, scanNr1);
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

    // Text IN der XML-Datei muss UTF-8 sein (so deklariert). Unter Windows liefern
    // die Aufrufer ANSI — fuer Dateioperationen richtig, fuer das XML nicht.
#ifdef _WIN32
    const std::string baseNameXml = AnsiToUtf8 (baseName);
    const std::string e57PathXml  = AnsiToUtf8 (e57PathStr ? e57PathStr : "");
#else
    const std::string baseNameXml = baseName;
    const std::string e57PathXml  = e57PathStr ? e57PathStr : "";
#endif

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

    // Owner/Signature sind FourCC-Codes (APIdefs_LibraryParts.h), NICHT die Add-on-MDID:
    //   Owner     = 'GSAC' = APILibOwner_ArchiCAD — jeder andere Wert macht das GSM zum
    //               ESYM (externes Symbol), das einen ESYM-Handler im Add-on voraussetzt
    //   Signature = 'GSYM' = normales Objekt (wie beim Graphisoft-Punktwolken-Objekt)
    fprintf (f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf (f, "<Symbol IsArchivable=\"false\" IsPlaceable=\"true\" "
               "MainGUID=\"%s\" MigrationValue=\"Normal\" "
               "Owner=\"1196638531\" Signature=\"1196644685\" Version=\"44\">\n\n", guid);   // 'GSAC' / 'GSYM'

    fprintf (f,
        "<Ancestry SectVersion=\"1\" SectionFlags=\"0\" SubIdent=\"0\" Template=\"false\">\n"
        "\t<MainGUID>F938E33A-329D-4A36-BE3E-85E126820996</MainGUID>\n"
        "\t<MainGUID>103E8D2C-8230-42E1-9597-46F84CCE28C0</MainGUID>\n"
        "\t<MainGUID>B298E56B-35FF-45C6-AA50-6D7F0B40BB1A</MainGUID>\n"
        "</Ancestry>\n\n"
        "<CalledMacros SectVersion=\"2\" SectionFlags=\"0\" SubIdent=\"0\">\n"
        "</CalledMacros>\n\n");

    // -------------------------------------------------------------------------
    // GDL-Scripts — eigene Implementierung (Pointcloud4Archicad)
    //
    // Grundriss: das 3D-Modell wird per project2{2} mit bProject2D=1 projiziert;
    // im 3D-Script begrenzen zwei cutplane{2}-Ebenen den Ausschnitt auf den
    // Bereich zwischen _cutBottom und _cutTop (Geschoss-Schnittebene oder
    // per bOverrideCutplane eigene Hoehen). Alle Hoehen relativ zum Objektursprung.
    // -------------------------------------------------------------------------

    // Master-Script (1D): Schnittebenen-Hoehen berechnen; im VL-Script (Typ 5)
    // sind die GLOB_*-Geschossvariablen nicht verfuegbar.
    fprintf (f, "<Script_1D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "! Pointcloud4Archicad - Master Script\n\n"
        "_cutTop    = 0\n"
        "_cutBottom = 0\n"
        "if GLOB_SCRIPT_TYPE <> 5 then\n"
        "\t_storyShift = GLOB_CSTORY_ELEV - GLOB_HSTORY_ELEV\n"
        "\tif bOverrideCutplane then\n"
        "\t\t_cutTop    = _storyShift + upperPlaneHeight - GLOB_ELEVATION\n"
        "\t\t_cutBottom = _storyShift + lowerPlaneHeight - GLOB_ELEVATION\n"
        "\telse\n"
        "\t\t_cutTop    = _storyShift + GLOB_CUTPLANES_INFO[1]\n"
        "\t\t_cutBottom = _storyShift + GLOB_CUTPLANES_INFO[3]\n"
        "\tendif\n"
        "endif\n\n");

    if (scanCount > 0) {
        fprintf (f,
            "! Scan-Standorte den Geschossen zuordnen (live per REQUEST, kein statischer Stand)\n"
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

    // 2D-Script: Projektion des geschnittenen 3D-Modells + Griffpunkte an der Bounding-Box
    fprintf (f, "<Script_2D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "! Pointcloud4Archicad - 2D Script\n\n"
        "hotspot2 0, 0, 1\n"
        "hotspot2 A, 0, 2\n"
        "hotspot2 A, B, 3\n"
        "hotspot2 0, B, 4\n\n"
        "project2{2} 3, 270, 3, parameters bProject2D = 1\n");
    fprintf (f, "]]>\n</Script_2D>\n\n");

    // 3D-Script: Schnittebenen (nur bei Grundriss-Projektion), Punktwolken, Standortnummern
    fprintf (f, "<Script_3D SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "! Pointcloud4Archicad - 3D Script\n\n"
        "! Schnittebenen: immer fuer die Grundriss-Projektion, im 3D-Fenster nur auf Wunsch\n"
        "_doCut = bProject2D or bCutIn3D\n\n"
        "if _doCut then\n"
        "\t! alles oberhalb _cutTop entfernen\n"
        "\taddz _cutTop\n"
        "\tcutplane{2} 0, 1\n"
        "\tdel 1\n"
        "\t! alles unterhalb _cutBottom entfernen (Ebene um 180 Grad gedreht)\n"
        "\taddz _cutBottom\n"
        "\trotx 180\n"
        "\tcutplane{2} 0, 1\n"
        "\tdel 2\n"
        "endif\n\n"
        "if bVisible then\n\n");
    {
        // Nur Standorte referenzieren, deren GSPC auch wirklich auf der Platte liegt.
        // Eine fehlende Datei wuerde sonst beim Zeichnen "Kann Bilddatei nicht finden"
        // ausloesen und je nach AC-Version das ganze 2D-Symbol ungueltig machen.
        char dirName[8];
        snprintf (dirName, sizeof (dirName), "%03d", densityPct);
        int fehlend = 0;
        for (int sc = 0; sc < scanCount; ++sc) {
            char pfad[1024];
            GspcPathOf (pfad, sizeof (pfad), outputDirPath, baseName, densityPct, sc + 1);
            if (!FileExists (pfad)) {
                ++fehlend;
                fprintf (f, "\t! Standort %d uebersprungen - %s_%s_%d.gspc fehlt\n",
                         sc + 1, baseNameXml.c_str (), dirName, sc + 1);
                continue;
            }
            fprintf (f, "\tif _sv[%d] then POINTCLOUD \"%s_%s_%d.gspc\"\n",
                sc + 1, baseNameXml.c_str (), dirName, sc + 1);
        }
        fprintf (f, "\n");
        if (fehlend > 0) {
            char buf[128];
            snprintf (buf, sizeof (buf), "WARNUNG: %d von %d GSPC-Dateien fehlen (Dichte %d%%)",
                      fehlend, scanCount, densityPct);
            Log (buf);
        }
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
        "if _doCut then\n"
        "\tcutend\n"
        "\tcutend\n"
        "endif\n");
    fprintf (f, "]]>\n</Script_3D>\n\n");

    // UI-Script: Seite 1 "Darstellung", Seite 2 "Standorte" (nur bei Scan-Standorten)
    fprintf (f, "<Script_UI SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "! Pointcloud4Archicad - UI Script\n\n"
        "_x1 = 8\n"
        "_x2 = 200\n"
        "_wFull = 444 - _x1\n"
        "_wLbl  = _x2 - _x1\n"
        "_wVal  = 444 - _x2 - 8\n"
        "_hRow  = 20\n"
        "_dy    = 26\n\n"
        "ui_dialog `Punktwolke`\n\n"
        "ui_current_page gs_ui_current_page\n\n"
        "ui_page 1, -1, `Darstellung`\n\n"
        "yStart = 8\n"
        "yCurr  = yStart\n\n"
        "ui_infield{3} \"bVisible\", _x1, yCurr, _wFull, _hRow, 7, \"\", 1, 1, 0, 0, 0, 0, 1, `Punktwolke anzeigen`, 0, 1, `Punktwolke anzeigen`, 1\n"
        "yCurr = yCurr + _dy\n"
        "ui_infield{3} \"bShowNumbers\", _x1, yCurr, _wFull, _hRow, 7, \"\", 1, 1, 0, 0, 0, 0, 1, `Standortnummern anzeigen`, 0, 1, `Standortnummern anzeigen`, 1\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_separator _x1, yCurr, 444 - _x1, yCurr\n"
        "yCurr = yCurr + 10\n\n"
        "ui_style 0, 1\n"
        "ui_outfield `Grundriss-Ausschnitt`, _x1, yCurr, _wFull, _hRow\n"
        "ui_style 0, 0\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_infield{3} \"bOverrideCutplane\", _x1, yCurr, _wFull, _hRow, 7, \"\", 1, 1, 0, 0, 0, 0, 1, `Eigene Schnittebenen im Grundriss`, 0, 1, `Eigene Schnittebenen im Grundriss`, 1\n"
        "yCurr = yCurr + _dy\n"
        "ui_infield{3} \"bCutIn3D\", _x1, yCurr, _wFull, _hRow, 7, \"\", 1, 1, 0, 0, 0, 0, 1, `Ausschnitt auch im 3D-Fenster`, 0, 1, `Ausschnitt auch im 3D-Fenster`, 1\n"
        "yCurr = yCurr + _dy\n\n"
        "! Anzeige der wirksamen Hoehen in der Arbeitseinheit des Projekts\n"
        "_unitRes = REQUEST(\"Working_length_unit\", \"\", _unitFmt)\n"
        "if _unitRes < 1 then _unitFmt = \"%%.2m\"\n\n"
        "ui_outfield `Obere Schnittebene`, _x1, yCurr + 3, _wLbl, _hRow\n"
        "if bOverrideCutplane then\n"
        "\tui_infield \"upperPlaneHeight\", _x2, yCurr, _wVal, _hRow\n"
        "else\n"
        "\tui_outfield STR(_unitFmt, GLOB_CUTPLANES_INFO[1] + GLOB_ELEVATION), _x2, yCurr + 3, _wVal, _hRow\n"
        "endif\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_outfield `Untere Schnittebene`, _x1, yCurr + 3, _wLbl, _hRow\n"
        "if bOverrideCutplane then\n"
        "\tui_infield \"lowerPlaneHeight\", _x2, yCurr, _wVal, _hRow\n"
        "else\n"
        "\tui_outfield STR(_unitFmt, GLOB_CUTPLANES_INFO[3] + GLOB_ELEVATION), _x2, yCurr + 3, _wVal, _hRow\n"
        "endif\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_separator _x1, yCurr, 444 - _x1, yCurr\n"
        "yCurr = yCurr + 10\n\n"
        "ui_style 0, 1\n"
        "ui_outfield `Import`, _x1, yCurr, _wFull, _hRow\n"
        "ui_style 0, 0\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_outfield `Quelldatei`, _x1, yCurr, _wLbl, _hRow\n"
        "ui_outfield sE57Path, _x2, yCurr, _wVal, _hRow\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_outfield `Dichte`, _x1, yCurr, _wLbl, _hRow\n"
        "ui_outfield STR(iDensityPercent, 1, 0) + ` %% von ` + STR(iTotalPoints, 1, 0) + ` Punkten`, _x2, yCurr, _wVal, _hRow\n"
        "yCurr = yCurr + _dy\n\n"
        "ui_outfield `Scan-Standorte`, _x1, yCurr, _wLbl, _hRow\n"
        "ui_outfield STR(iScanCount, 1, 0), _x2, yCurr, _wVal, _hRow\n"
        "yCurr = yCurr + _dy\n\n");

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

    // VL-Script: Ausdehnung aus der Bounding-Box fest setzen, Wertebereiche
    fprintf (f, "<Script_VL SectVersion=\"20\" SectionFlags=\"0\" SubIdent=\"0\">\n<![CDATA[\n");
    fprintf (f,
        "! Pointcloud4Archicad - Parameter Script\n\n"
        "! Ausdehnung der Punktwolke (Meter, aus dem Import) - nicht editierbar\n"
        "A     = %.8g\n"
        "B     = %.8g\n"
        "ZZYZX = %.8g\n"
        "parameters A = A, B = B, ZZYZX = ZZYZX,\n"
        "           ac_bottomlevel = 0, ac_toplevel = ZZYZX\n"
        "lock \"A\", \"B\", \"ZZYZX\"\n"
        "lock \"iDensityPercent\", \"iTotalPoints\", \"iScanCount\"\n\n"
        "! Schnittebenen: obere immer ueber der unteren (kein values-range,\n"
        "! das erzeugt im Dialog stoerende Bereichs-Popups neben den Feldern)\n"
        "if GLOB_MODPAR_NAME = \"upperPlaneHeight\" and upperPlaneHeight <= lowerPlaneHeight then\n"
        "\tparameters lowerPlaneHeight = upperPlaneHeight - 0.01\n"
        "endif\n"
        "if GLOB_MODPAR_NAME = \"lowerPlaneHeight\" and lowerPlaneHeight >= upperPlaneHeight then\n"
        "\tparameters upperPlaneHeight = lowerPlaneHeight + 0.01\n"
        "endif\n"
        "if not (bOverrideCutplane) then\n"
        "\tlock \"upperPlaneHeight\", \"lowerPlaneHeight\"\n"
        "endif\n",
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

    fprintf (f, "\t\t<Length Name=\"A\"><Description><![CDATA[\"Ausdehnung X\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origX);
    fprintf (f, "\t\t<Length Name=\"B\"><Description><![CDATA[\"Ausdehnung Y\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origY);
    fprintf (f, "\t\t<Length Name=\"ZZYZX\"><Description><![CDATA[\"Ausdehnung Z\"]]></Description>"
               "<Fix/><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf (f, "\t\t<Length Name=\"ac_bottomlevel\"><Description><![CDATA[\"Unterkante\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"ac_toplevel\"><Description><![CDATA[\"Oberkante\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>%.8g</Value></Length>\n", (double)origZ);
    fprintf (f, "\t\t<Boolean Name=\"bProject2D\"><Description><![CDATA[\"Grundriss-Projektion aktiv\"]]></Description>"
               "<Fix/><Flags><ParFlg_Hidden/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Boolean Name=\"bOverrideCutplane\"><Description><![CDATA[\"Eigene Schnittebenen im Grundriss\"]]></Description>"
               "<Fix/><Flags><ParFlg_BoldName/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Boolean Name=\"bCutIn3D\"><Description><![CDATA[\"Ausschnitt auch im 3D-Fenster\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>0</Value></Boolean>\n");
    fprintf (f, "\t\t<Length Name=\"upperPlaneHeight\"><Description><![CDATA[\"Obere Schnittebene (zum Geschoss)\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>1.1</Value></Length>\n");
    fprintf (f, "\t\t<Length Name=\"lowerPlaneHeight\"><Description><![CDATA[\"Untere Schnittebene (zum Geschoss)\"]]></Description>"
               "<Fix/><Flags><ParFlg_Child/></Flags><Value>0</Value></Length>\n");
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
               "<Value><![CDATA[\"%s\"]]></Value></String>\n", e57PathXml.c_str ());
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
    if (!ConverterExists (gConverterPath)) {
        Log ("LP_XMLConverter nicht verfuegbar (EnsureLpXmlConverter vor dem Import nicht erfolgreich).");
        return false;
    }

    // CreateProcessW darf die Kommandozeile veraendern, daher ein beschreibbarer Puffer
    std::wstring cmd = L"\"" + ToWide (gConverterPath) + L"\" xml2libpart \""
                     + AnsiToWide (xmlPath) + L"\" \"" + AnsiToWide (gsmPath) + L"\"";
    std::vector<wchar_t> cmdLine (cmd.begin (), cmd.end ());
    cmdLine.push_back (L'\0');

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;

    HANDLE hOut = CreateFileA (lpcOut,
        GENERIC_WRITE, FILE_SHARE_READ, &sa,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si = {};
    si.cb         = sizeof (si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError  = hOut;
    si.hStdInput  = GetStdHandle (STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (CreateProcessW (nullptr, cmdLine.data (), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject (pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess (pi.hProcess, &exitCode);
        ok = (exitCode == 0);
        CloseHandle (pi.hProcess);
        CloseHandle (pi.hThread);
        char buf[2048];
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

    const std::string converter = ToUtf8 (gConverterPath);
    if (!ConverterExists (gConverterPath)) {
        posix_spawn_file_actions_destroy (&fa);
        Log ("LP_XMLConverter nicht verfuegbar (EnsureLpXmlConverter vor dem Import nicht erfolgreich).");
        return false;
    }

    const char* argv[] = { converter.c_str (), "xml2libpart", xmlPath, gsmPath, nullptr };
    extern char** environ;
    pid_t pid;
    int spawnRet = posix_spawn (&pid, converter.c_str (), &fa, nullptr,
                                (char* const*)argv, environ);
    posix_spawn_file_actions_destroy (&fa);

    if (spawnRet == 0) {
        int status = 0;
        waitpid (pid, &status, 0);
        int exitCode = WIFEXITED (status) ? WEXITSTATUS (status) : -1;
        ok = (exitCode == 0);
        char buf[2048];
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

// --- Platzierte Instanzen eines Bibliothekselements ---------------------------
// Vergleich ueber die stabile GUID (ownUnID), nicht ueber den LibPart-Index:
// der Index aendert sich beim Bibliotheks-Reload, die platzierten Objekte
// behalten voruebergehend den alten.

static GS::UniString LibPartUnID (Int32 libIndex)
{
    API_LibPart lp = {};
    lp.index = libIndex;
    if (libIndex <= 0 || ACAPI_LibPart_Get (&lp) != NoError) return GS::UniString ();
    return GS::UniString (lp.ownUnID);
}

static GS::Array<API_Guid> PlacedInstances (Int32 libIndex)
{
    GS::Array<API_Guid> result;
    GS::UniString wanted = LibPartUnID (libIndex);
    if (wanted.IsEmpty ()) return result;

    GS::Array<API_Guid> elemList;
    ACAPI_Element_GetElemList (API_ObjectID, &elemList);
    GS::HashTable<Int32, bool> matchByIndex;    // Cache: libInd -> gehoert zu uns?
    for (const auto& guid : elemList) {
        API_Element elem = {};
        elem.header.guid = guid;
        if (ACAPI_Element_Get (&elem) != NoError) continue;
        Int32 li = elem.object.libInd;
        if (!matchByIndex.ContainsKey (li))
            matchByIndex.Add (li, li == libIndex || LibPartUnID (li) == wanted);
        if (matchByIndex[li])
            result.Push (guid);
    }
    return result;
}

// --- Parameter in platzierten Instanzen aktualisieren ------------------------

static int UpdatePlacedParams (Int32 libIdx, int newDensity, int64_t writtenPoints)
{
    double lpA, lpB;
    Int32 parNum = 0;
    API_AddParType** lpPars = nullptr;
    ACAPI_LibPart_GetParams (libIdx, &lpA, &lpB, &parNum, &lpPars);
    ACAPI_DisposeAddParHdl (&lpPars);

    GS::Array<API_Guid> toUpdate = PlacedInstances (libIdx);

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

// --- HasPlacedInstances ------------------------------------------------------
// true, wenn mindestens ein Objekt dieses Bibliothekselements im Projekt liegt

bool HasPlacedInstances (Int32 libIndex)
{
    return !PlacedInstances (libIndex).IsEmpty ();
}

API_Guid FirstPlacedInstance (Int32 libIndex)
{
    GS::Array<API_Guid> inst = PlacedInstances (libIndex);
    return inst.IsEmpty () ? APINULLGuid : inst[0];
}

// --- Geschosse ---------------------------------------------------------------

std::vector<StoryEntry> GetStoriesAscending ()
{
    std::vector<StoryEntry> stories;
    API_StoryInfo si = {};
    if (Compat_GetStorySettings (&si) != NoError || si.data == nullptr)
        return stories;
    for (short i = si.firstStory; i <= si.lastStory; ++i) {
        const API_StoryType& s = (*si.data)[i - si.firstStory];
        StoryEntry e;
        e.index = s.index;
        e.level = s.level;
        e.name  = GS::UniString (s.uName);
        stories.push_back (e);
    }
    BMKillHandle ((GSHandle*)&si.data);
    std::sort (stories.begin (), stories.end (),
        [] (const StoryEntry& a, const StoryEntry& b) { return a.level < b.level; });
    return stories;
}

// Gleiche Regel wie im GDL-Master-Script: hoechstes Geschoss, dessen Unterkante <= zScan
int StoryIndexOfScan (double zScan, const std::vector<StoryEntry>& stories)
{
    int sg = 0;
    for (size_t j = 0; j < stories.size (); ++j)
        if (zScan >= stories[j].level) sg = (int)j + 1;
    return sg;
}

// --- Zustand lesen / schreiben -----------------------------------------------

static Int32 ArrayLen (const API_AddParType& p)
{
    Int32 n = p.dim1 * (p.dim2 > 0 ? p.dim2 : 1);
    return (p.value.array != nullptr && n > 0) ? n : 0;
}

bool ReadCloudState (Int32 libIndex, CloudState& out)
{
    out = CloudState ();
    API_Guid guid = FirstPlacedInstance (libIndex);
    if (guid == APINULLGuid) return false;

    API_ElementMemo memo = {};
    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) != NoError || memo.params == nullptr) {
        ACAPI_DisposeElemMemoHdls (&memo);
        return false;
    }
    Int32 parNum = (Int32)(BMGetHandleSize ((GSHandle)memo.params) / sizeof (API_AddParType));
    for (Int32 j = 0; j < parNum; ++j) {
        const API_AddParType& p = (*memo.params)[j];
        if (strcmp (p.name, "iScanCount") == 0) {
            out.scanCount = (int)(p.value.real + 0.5);
        } else if (strcmp (p.name, "zScan") == 0) {
            Int32 n = ArrayLen (p);
            for (Int32 k = 0; k < n; ++k) out.zScan.push_back (((double*)*p.value.array)[k]);
        } else if (strcmp (p.name, "bScan") == 0) {
            Int32 n = ArrayLen (p);
            for (Int32 k = 0; k < n; ++k) out.bScan.push_back (((double*)*p.value.array)[k] > 0.5);
        } else if (strcmp (p.name, "bGroup") == 0) {
            Int32 n = ArrayLen (p);
            for (Int32 k = 0; k < n; ++k) out.bGroup.push_back (((double*)*p.value.array)[k] > 0.5);
        }
    }
    ACAPI_DisposeElemMemoHdls (&memo);
    return out.scanCount > 0;
}

// Schreibt ein numerisches 1D-Array in einen Parameter (Groesse darf wachsen)
static void SetNumArray (API_AddParType& p, const std::vector<double>& vals)
{
    Int32 n = (Int32)vals.size ();
    if (p.value.array != nullptr) BMKillHandle (&p.value.array);
    p.value.array = BMAllocateHandle (n * sizeof (double), ALLOCATE_CLEAR, 0);
    p.dim1 = n;
    p.dim2 = 1;
    for (Int32 k = 0; k < n; ++k) ((double*)*p.value.array)[k] = vals[k];
}

bool WriteCloudVisibility (Int32 libIndex, const CloudState& st)
{
    GS::Array<API_Guid> targets = PlacedInstances (libIndex);
    if (targets.IsEmpty ()) return false;

    std::vector<double> g (st.bGroup.begin (), st.bGroup.end ());
    std::vector<double> sc (st.bScan.begin (),  st.bScan.end ());

    int updCount = 0;
    ACAPI_CallUndoableCommand ("Punktwolke: Sichtbarkeit",
        [&] () -> GSErrCode {
            for (auto guid : targets) {
                API_ElementMemo memo = {};
                if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) != NoError || memo.params == nullptr) {
                    ACAPI_DisposeElemMemoHdls (&memo);
                    continue;
                }
                Int32 parNum = (Int32)(BMGetHandleSize ((GSHandle)memo.params) / sizeof (API_AddParType));
                for (Int32 j = 0; j < parNum; ++j) {
                    API_AddParType& p = (*memo.params)[j];
                    if (strcmp (p.name, "bGroup") == 0 || strcmp (p.name, "bGroupPrev") == 0)
                        SetNumArray (p, g);       // Prev mitziehen, sonst synchronisiert das Master-Script zurueck
                    else if (strcmp (p.name, "bScan") == 0)
                        SetNumArray (p, sc);
                }
                if (ACAPI_Element_ChangeMemo (guid, APIMemoMask_AddPars, &memo) == NoError)
                    ++updCount;
                ACAPI_DisposeElemMemoHdls (&memo);
            }
            return NoError;
        });
    return updCount > 0;
}

// --- Einstellungsdialog der platzierten Punktwolke oeffnen -------------------
// Der Dialog arbeitet auf der Selektion, und die wird erst im naechsten
// Event-Loop-Durchlauf wirksam. Deshalb: hier selektieren, den Dialog dann
// ueber einen eigenen Add-on-Befehl aus dem Event-Loop oeffnen.

static const API_ModulID kOwnMdid = { 709315661, 1905536138 };

bool OpenSettingsDialog (Int32 libIndex)
{
    API_Guid guid = FirstPlacedInstance (libIndex);
    if (guid == APINULLGuid) return false;

    Compat_DeselectAll ();
    API_Neig neig = {};
    neig.neigID = APINeig_Symb;
    neig.guid   = guid;
    GS::Array<API_Neig> neigs;
    neigs.Push (neig);
    GSErrCode e2 = Compat_Select (neigs, true);
    GSErrCode e3 = Compat_CallFromEventLoop (&kOwnMdid, kCmdOpenSettings, kCmdOpenSettingsVer);

    return e2 == NoError && e3 == NoError;
}

GSErrCode __ACENV_CALL OpenSettingsDialogCommand (GSHandle /*params*/, GSPtr /*resultData*/, bool /*silentMode*/)
{
    API_ToolBoxItem tbi = {};
    tbi.type = API_ObjectID;
#ifdef ServerMainVers_2700
    GSErrCode err = ACAPI_Dialog_SettingsDialog (&tbi);
#else
    GSErrCode err = ACAPI_Interface (APIIo_SettingsDialogID, &tbi, nullptr);
#endif
    return err;      // APIERR_CANCEL, wenn der Dialog abgebrochen wurde
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

    // Converter VOR dem (langen) E57-Lesen pruefen, ggf. Benutzer fragen
    if (!EnsureLpXmlConverter ())
        return GS::UniString ();
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
        // Abkuerzung (GSM neu erzeugen statt E57 neu zu lesen) nur, wenn ALLE
        // Standorte dieser Dichte vorliegen. Frueher wurde nur Standort 1 geprueft —
        // fehlte ein spaeterer, verwies das neue GSM auf eine nicht existierende Datei.
        PwMeta m = {};
        std::vector<double> tx, ty, tz;
        std::string metaE57;
        bool metaOk = ReadPwMeta (outputDirPath.ToCStr ().Get (),
                                   baseName.ToCStr ().Get (), m, tx, ty, tz, metaE57);
        int fehlendeGspc = 0;
        if (metaOk) {
            for (int sc = 0; sc < (int)m.scanCount; ++sc) {
                char pfad[1024];
                GspcPathOf (pfad, sizeof (pfad), outputDirPath.ToCStr ().Get (),
                            baseName.ToCStr ().Get (), densityPct, sc + 1);
                if (!FileExists (pfad)) ++fehlendeGspc;
            }
        }
        if (metaOk && fehlendeGspc > 0) {
            char buf[160];
            snprintf (buf, sizeof (buf),
                      "%d von %d GSPC-Dateien fehlen - vollstaendiger Neuimport statt Abkuerzung.",
                      fehlendeGspc, (int)m.scanCount);
            Log (buf);
        }
        {
            if (metaOk && fehlendeGspc == 0) {
                Log ("Alle GSPC vorhanden, erzeuge GSM aus PwMeta neu.");
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

            // Die Positions-Arrays muessen fuer JEDEN Standort einen Eintrag haben,
            // sonst verschieben sich die Indizes gegen die Standortnummern — und
            // WriteGsmViaXml liest ueber das Ende des Vektors hinaus.
            e57::Data3D header;
            bool kopfOk = reader.ReadData3D (si, header);
            scanTx.push_back (kopfOk ? header.pose.translation.x : 0.0);
            scanTy.push_back (kopfOk ? header.pose.translation.y : 0.0);
            scanTz.push_back (kopfOk ? header.pose.translation.z : 0.0);
            if (!kopfOk) {
                char buf[128];
                snprintf (buf, sizeof (buf), "Standort %lld: Kopfdaten nicht lesbar, uebersprungen.",
                          (long long)(si + 1));
                Log (buf);
                continue;
            }

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
                snprintf (buf, sizeof (buf), "FEHLER Standort %lld: GSPC nicht anlegbar: %s",
                          (long long)(si + 1), scanGspcPath.ToCStr ().Get ());
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
            // Ein Lesefehler mitten im Scan darf den Lauf nicht abbrechen: Datei
            // schliessen, Standort vermerken, mit dem naechsten weitermachen.
            try {
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
            } catch (const std::exception& ex) {
                char buf[256];
                snprintf (buf, sizeof (buf), "FEHLER Standort %lld: %s", (long long)(si + 1), ex.what ());
                Log (buf);
            }
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
            char msg[1200];
            snprintf (msg, sizeof (msg), "GSM konnte nicht erstellt werden. Details im Log: %s", GetLogPath ());
            ACAPI_WriteReport (msg, true);
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
    GS::Array<API_Guid> toDelete = PlacedInstances (entry.libIndex);

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
