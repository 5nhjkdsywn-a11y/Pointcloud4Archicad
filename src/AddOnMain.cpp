// *****************************************************************************
// Pointcloud4Archicad Add-On
// E57-Punktwolken importieren und als platzierbares ArchiCAD-Objekt verwalten
// *****************************************************************************

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "compat.h"
#include "PunktwolkenPalette.hpp"

// --- Menü-Befehl: Palette öffnen / anzeigen ----------------------------------

static void ShowPunktwolkenPalette ()
{
    if (!PunktwolkenPalette::HasInstance ())
        PunktwolkenPalette::CreateInstance ();
    PunktwolkenPalette::GetInstance ().Show ();
    PunktwolkenPalette::GetInstance ().Refresh ();
}

// --- MenuCommandHandler ------------------------------------------------------

static GSErrCode MenuCommandHandler (const API_MenuParams* menuParams)
{
    switch (menuParams->menuItemRef.menuResID) {
        case kMenuResId:
            switch (menuParams->menuItemRef.itemIndex) {
                case 1:
                    ShowPunktwolkenPalette ();
                    break;
            }
            break;
    }
    return NoError;
}

// =============================================================================
// Pflicht-Funktionen des Add-Ons
// =============================================================================

// CheckEnvironment: APIAddon_Preload ist Pflicht für Paletten!
API_AddonType CheckEnvironment (API_EnvirParams* envir)
{
    RSGetIndString (&envir->addOnInfo.name,        kAddonInfoResId, 1, ACAPI_GetOwnResModule ());
    RSGetIndString (&envir->addOnInfo.description, kAddonInfoResId, 2, ACAPI_GetOwnResModule ());
    return APIAddon_Preload;
}

// RegisterInterface: Menüeintrag registrieren
GSErrCode RegisterInterface (void)
{
    GSErrCode menuErr = ACAPI_MenuItem_RegisterMenu (kMenuResId, 32600, MenuCode_UserDef, MenuFlag_Default);

    // Der Add-on-Befehl fuer den verzoegerten Einstellungsdialog ist Beiwerk:
    // sein Fehler darf NICHT zurueckgegeben werden, sonst verwirft ArchiCAD die
    // gesamte Registrierung inklusive Menueeintrag (das Add-on gilt trotzdem als geladen).
    Compat_RegisterSupportedService (kCmdOpenSettings, kCmdOpenSettingsVer);

    // MenuCode_UserDef heisst "vom Anwender konfigurierbar": der Befehl wird registriert,
    // die Platzierung kommt aber aus der Arbeitsumgebung. Die "Planung"-Kopfzeile der
    // STR#-Ressource ist nur der Vorschlag. Kennt ein Profil den Befehl noch nicht
    // (neue AC-Version, neuer Rechner), erscheint kein Menuepunkt — dann einmalig unter
    // Optionen > Arbeitsumgebung > Menues zuordnen.
    return menuErr;
}

// Initialize: Handler installieren, Palette-Callback registrieren
GSErrCode Initialize (void)
{
    GSErrCode err = ACAPI_MenuItem_InstallMenuHandler (kMenuResId, MenuCommandHandler);
    if (DBERROR (err != NoError))
        return err;

    err = PunktwolkenPalette::RegisterPaletteControlCallBack ();
    if (DBERROR (err != NoError))
        return err;

    err = Compat_InstallModulCommandHandler (kCmdOpenSettings, kCmdOpenSettingsVer, OpenSettingsDialogCommand);
    if (DBERROR (err != NoError))
        return err;

    return NoError;
}

// FreeData: Instanz freigeben
GSErrCode FreeData (void)
{
    PunktwolkenPalette::DestroyInstance ();
    return NoError;
}
