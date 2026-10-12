// *****************************************************************************
// ToolIcon — Icons der Werkzeugleiste
//
// Die DG::ListView zeichnet keine Ressourcen-Icons: im Baum (DG::TreeView)
// funktionieren GICN-Icons, in der Leiste bleiben die Zellen leer bzw. grau —
// auf macOS wie auf Windows (gemessen 12.10.2026). Was dort funktioniert, ist
// ein Icon aus einem NewDisplay::NativeImage. Die drei Leisten-Icons stecken
// deshalb als PNG-Daten im Programm (ToolIconData.cpp, erzeugt von
// tools/make-icon-data.py aus den SVGs) und werden zur Laufzeit umgewandelt.
//
// Die Baum-Icons bleiben gewoehnliche GICN-Ressourcen.
// *****************************************************************************

#ifndef TOOLICON_HPP
#define TOOLICON_HPP

#include "APIEnvir.h"   // muss vor den DG-Headern stehen (Windows: WINDOWS/Typen fuer DGUtility.hpp)
#include "DGDLLExport.h"
#include "DGUtility.hpp"

namespace ToolIcon {

    // Reihenfolge muss zu gToolIconBlobs in ToolIconData.cpp passen
    enum Id { Add = 0, Settings, Delete };

    DG::Icon Get (Id id, bool hover);
}

#endif // TOOLICON_HPP
