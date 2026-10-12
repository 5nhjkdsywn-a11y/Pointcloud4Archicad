// *****************************************************************************
// ToolIcon — Icon-Bitmaps fuer die Icon-Leiste der Palette
//
// Die Leiste ist eine DG::ListView (rahmenlos, eingebautes Hover, Click-through
// auf inaktive Paletten). Normal- und Hover-Variante werden als gleich grosse
// Bitmaps erzeugt, damit die ListView nichts skaliert; die Hover-Variante hat
// einen grauen abgerundeten Hintergrund (macOS: Core Graphics aus dem
// Ressourcen-TIF; Windows: nur das Ressourcen-Icon, kein Hover-Hintergrund).
// *****************************************************************************

#ifndef TOOLICON_HPP
#define TOOLICON_HPP

#include "APIEnvir.h"   // muss vor den DG-Headern stehen (Windows: WINDOWS/Typen fuer DGUtility.hpp)
#include "DGDLLExport.h"
#include "DGUtility.hpp"

namespace ToolIcon {
    // resModule/resId: Icon-Quelle (z. B. ACAPI_GetOwnResModule () + eigene GICN-ID)
    DG::Icon Make (GSResModule resModule, short resId, short w, short h, bool withBackground);
}

#endif // TOOLICON_HPP
