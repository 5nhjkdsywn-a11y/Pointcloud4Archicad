// *****************************************************************************
// PunktwolkenPalette — dockbare Palette zur Punktwolken-Verwaltung
// *****************************************************************************

#ifndef PUNKTWOLKENPALETTE_HPP
#define PUNKTWOLKENPALETTE_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "compat.h"
#include "E57Importer.hpp"

#include "DGDLLExport.h"
#include "DGDialog.hpp"
#include "DGButton.hpp"
#include "DGEditControl.hpp"
#include "DGStaticItem.hpp"
#include "DGListBox.hpp"

// --- Ressource-IDs -----------------------------------------------------------

#define kAddonInfoResId     32000
#define kMenuResId          32500

// --- Item-IDs (entsprechen GDLG 32400) --------------------------------------

enum PalItemId {
    kHeaderLabelId  = 1,
    kAddBtnId       = 2,
    kDefDensEditId  = 3,
    kDefDensLabelId = 4,
    kDelBtnId       = 5,
    kListBoxId      = 6,
    kRelBtnId       = 7,
    kSelDensEditId  = 8,
    kSelDensLabelId = 9,
    kVersionLabelId = 10
};

// --- PunktwolkenPalette ------------------------------------------------------

class PunktwolkenPalette final
    : public DG::Palette
    , public DG::PanelObserver
    , public DG::ButtonItemObserver
    , public DG::ListBoxObserver
{
public:
    static bool                     HasInstance     ();
    static void                     CreateInstance  ();
    static PunktwolkenPalette&      GetInstance     ();
    static void                     DestroyInstance ();

    static GSErrCode                RegisterPaletteControlCallBack ();

    void    Refresh ();

    virtual ~PunktwolkenPalette () override;

private:
    PunktwolkenPalette ();

    void    UpdateFooter ();

    // DG::PanelObserver
    virtual void PanelOpened             (const DG::PanelOpenEvent& ev)                              override;
    virtual void PanelResized            (const DG::PanelResizeEvent& ev)                            override;
    virtual void PanelCloseRequested     (const DG::PanelCloseRequestEvent& ev, bool* accepted)      override;

    // DG::ListBoxObserver
    virtual void ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& ev)                       override;

    // DG::ButtonItemObserver
    virtual void ButtonClicked           (const DG::ButtonClickEvent& ev)                            override;

    // Palette-Callback
    static GSErrCode __ACENV_CALL PaletteControlCallBack (Int32, API_PaletteMessageID, GS::IntPtr);

    // Items
    DG::LeftText            headerLabel;
    DG::Button              addBtn;
    DG::IntEdit             defDensEdit;
    DG::LeftText            defDensLabel;
    DG::IconButton          delBtn;
    DG::SingleSelListBox    listBox;
    DG::IconButton          relBtn;
    DG::IntEdit             selDensEdit;
    DG::LeftText            selDensLabel;
    DG::RightText           versionLabel;

    GS::Array<PointCloudEntry>  entries;
    int                         defaultDensity  = 10;
    bool                        importing       = false;

    static GS::Ref<PunktwolkenPalette>  instance;
};

#endif // PUNKTWOLKENPALETTE_HPP
