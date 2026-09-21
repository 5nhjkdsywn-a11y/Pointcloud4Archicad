// *****************************************************************************
// ToolIcon — Icon-Bitmaps fuer die Icon-Leiste (siehe .hpp)
// *****************************************************************************

#include "ToolIcon.hpp"
#include "RS.hpp"
#include "E57Importer.hpp"   // Log
#include <cstdio>

#ifdef macintosh
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreGraphics/CoreGraphics.h>
#  include <ImageIO/ImageIO.h>
#  include "NativeImage.hpp"
#endif

// --- Hover-Icon: graues abgerundetes Rechteck + Icon (macOS) -----------------

DG::Icon ToolIcon::Make (GSResModule resModule, short resId, short w, short h, bool withBackground)
{
#ifdef macintosh
    // Icon-Bitmap aus dem Ressourcenordner des Moduls laden: <Resources>/<id>.tif
    // Fuer ArchiCADs eigene Icons ist das das Hauptbundle; fuer unsere das Add-on-Bundle.
    CFBundleRef bundle = (resModule == RSGetApplicationResModule ())
                       ? CFBundleGetMainBundle ()
                       : CFBundleGetBundleWithIdentifier (CFSTR ("com.torben.Pointcloud4Archicad"));
    if (bundle == nullptr) { Log ("MakeHoverIcon: kein Bundle"); return DG::Icon (); }

    char idStr[16];
    snprintf (idStr, sizeof (idStr), "%d", (int)resId);
    CFStringRef name = CFStringCreateWithCString (nullptr, idStr, kCFStringEncodingASCII);
    CFURLRef url = CFBundleCopyResourceURL (bundle, name, CFSTR ("tif"), nullptr);
    CFRelease (name);
    if (url == nullptr) { Log ("MakeHoverIcon: Ressource nicht gefunden"); return DG::Icon (); }

    CGImageSourceRef src = CGImageSourceCreateWithURL (url, nullptr);
    CFRelease (url);
    if (src == nullptr) return DG::Icon ();
    CGImageRef img = CGImageSourceCreateImageAtIndex (src, 0, nullptr);
    CFRelease (src);
    if (img == nullptr) { Log ("MakeHoverIcon: TIF nicht dekodierbar"); return DG::Icon (); }

    size_t iW = CGImageGetWidth (img), iH = CGImageGetHeight (img);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB ();
    CGContextRef ctx = CGBitmapContextCreate (nullptr, w, h, 8, w * 4, cs,
                                              kCGBitmapByteOrderDefault | kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease (cs);
    if (ctx == nullptr) { CGImageRelease (img); return DG::Icon (); }

    if (withBackground) {
        CGContextSetRGBFillColor (ctx, 0.80f, 0.80f, 0.80f, 1.0f);
        CGPathRef bg = CGPathCreateWithRoundedRect (CGRectMake (0, 0, w, h), 4.0, 4.0, nullptr);
        CGContextAddPath (ctx, bg);
        CGContextFillPath (ctx);
        CGPathRelease (bg);
    }

    CGFloat ix = (w - (CGFloat)iW) / 2.0f;
    CGFloat iy = (h - (CGFloat)iH) / 2.0f;
    CGContextDrawImage (ctx, CGRectMake (ix, iy, iW, iH), img);
    CGImageRelease (img);

    CGImageRef out = CGBitmapContextCreateImage (ctx);
    CGContextRelease (ctx);
    if (out == nullptr) return DG::Icon ();
    NewDisplay::NativeImage ni (out, true);
    CGImageRelease (out);
    return DG::Utils::CreateIconFromNativeImage (ni);
#else
    (void)w; (void)h;
    return withBackground ? DG::Icon () : DG::Icon (resModule, resId);   // Windows: kein Hover-Hintergrund
#endif
}
