// *****************************************************************************
// ToolIcon — siehe .hpp
// *****************************************************************************

#include "ToolIcon.hpp"
#include "ToolIconData.hpp"
#include "NativeImage.hpp"
#include "NativeContext.hpp"

// Die eingebackenen Bilder liegen in doppelter Aufloesung vor (48x48 fuer eine
// 24-Punkt-Zelle). Damit sie mit der richtigen Groesse UND scharf gezeichnet
// werden, bekommt das Zielbild den Auflösungsfaktor 2 — den nimmt nur dieser
// Konstruktor entgegen, nicht der aus PNG-Daten. Deshalb der Umweg:
// PNG dekodieren, in ein Ziel mit Faktor 2 uebertragen.
static const double kFaktor = 2.0;

DG::Icon ToolIcon::Get (Id id, bool hover)
{
    if ((int) id < 0 || (int) id >= gToolIconCount)
        return DG::Icon ();

    const ToolIconBlob&  blob    = gToolIconBlobs[(int) id];
    const unsigned char* daten   = hover ? blob.hover     : blob.normal;
    unsigned int         groesse = hover ? blob.hoverSize : blob.normalSize;
    if (daten == nullptr || groesse == 0)
        return DG::Icon ();

    NewDisplay::NativeImage quelle ((const char*) daten, (UInt32) groesse,
                                    NewDisplay::NativeImage::PNG);
    if (quelle.GetWidth () == 0 || quelle.GetHeight () == 0)
        return DG::Icon ();

    NewDisplay::NativeImage ziel (quelle.GetWidth (), quelle.GetHeight (), quelle, kFaktor);
    NewDisplay::NativeContext ctx = ziel.GetContext ();
    ctx.DrawImage (quelle, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, true);
    ziel.ReleaseContext (ctx);

    return DG::Utils::CreateIconFromNativeImage (ziel);
}
