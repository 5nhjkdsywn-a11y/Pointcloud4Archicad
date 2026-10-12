// *****************************************************************************
// ToolIconData — Zugriff auf die eingebackenen Leisten-Icons
//
// Daten in der erzeugten ToolIconData.cpp (tools/make-icon-data.py).
// Reihenfolge entspricht ToolIcon::Id.
// *****************************************************************************

#ifndef TOOLICONDATA_HPP
#define TOOLICONDATA_HPP

struct ToolIconBlob {
    const unsigned char*    normal;
    unsigned int            normalSize;
    const unsigned char*    hover;
    unsigned int            hoverSize;
};

extern const ToolIconBlob   gToolIconBlobs[];
extern const int            gToolIconCount;

#endif // TOOLICONDATA_HPP
