// *****************************************************************************
// PointCloudDensity Add-On
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
    return ACAPI_MenuItem_RegisterMenu (kMenuResId, 32600, MenuCode_UserDef, MenuFlag_Default);
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

    return NoError;
}

// FreeData: Instanz freigeben
GSErrCode FreeData (void)
{
    PunktwolkenPalette::DestroyInstance ();
    return NoError;
}
