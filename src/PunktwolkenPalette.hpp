// *****************************************************************************
// PunktwolkenPalette — dockbare Palette zur Punktwolken-Verwaltung
//
//   [ 10 % ] (+) (Einstellungen) (x)
//   [ Dropdown: Punktwolke – 10% ▾ ]
//   Baum: Punktwolke → Geschosse → Geschoss (Auge) → Standorte (Auge)
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
#include "DGPopUp.hpp"
#include "DGTreeView.hpp"
#include "DGListView.hpp"

#include <vector>

// --- Ressource-IDs -----------------------------------------------------------

#define kAddonInfoResId     32000
#define kMenuResId          32500

// --- Item-IDs (entsprechen GDLG 32400) --------------------------------------

enum PalItemId {
    kDensEditId     = 1,
    kDensLabelId    = 2,
    kCloudPopUpId   = 3,
    kTreeId         = 4,
    kVersionLabelId = 5
    // Die drei Icon-Buttons werden programmatisch erzeugt (flach, ohne Rahmen)
};

// --- PunktwolkenPalette ------------------------------------------------------

class PunktwolkenPalette final
    : public DG::Palette
    , public DG::PanelObserver
    , public DG::PopUpObserver
    , public DG::TreeViewObserver
    , public DG::ListViewObserver
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

    enum ToolItem { kToolAdd = 1, kToolSettings = 2, kToolDelete = 3 };

    // Baum-Knoten: was steckt hinter einem Tree-Item
    enum NodeKind { kNodeCloud, kNodeStoriesFolder, kNodeStory, kNodeScan };
    struct Node {
        Int32       tvItem;
        NodeKind    kind;
        int         index;      // Story: bGroup-Index (0 = unter Unterkante); Scan: 0-basiert
    };

    void    RebuildTree   ();
    void    UpdateEyes    ();   // nur Status-Icons nachfuehren, Aufklapp-Zustand bleibt
    void    SetEye        (Int32 tvItem, bool visible);
    const Node* FindNode  (Int32 tvItem) const;
    int     SelectedCloud () const;    // Index in entries, -1 = keine

    // DG::PanelObserver
    virtual void PanelOpened             (const DG::PanelOpenEvent& ev)                              override;
    virtual void PanelResized            (const DG::PanelResizeEvent& ev)                            override;
    // DG::ListViewObserver (Icon-Leiste)
    virtual bool ListViewSelectionChangeRequested (const DG::ListViewMouseDownEvent& ev)         override;
    virtual void ListViewMouseDown       (const DG::ListViewMouseDownEvent& ev, bool* processed)  override;
    virtual void ListViewHoverStarted    (const DG::ListViewHoverEvent& ev)                       override;
    virtual void ListViewHoverEnded      (const DG::ListViewHoverEvent& ev)                       override;
    void    SetToolEnabled (ToolItem item, bool enabled);
    virtual void PanelCloseRequested     (const DG::PanelCloseRequestEvent& ev, bool* accepted)      override;

    void    OnAdd ();
    void    OnSettings ();
    void    OnDelete ();

    // DG::PopUpObserver
    virtual void PopUpChanged            (const DG::PopUpChangeEvent& ev)                            override;

    // DG::TreeViewObserver
    virtual void TreeViewItemClicked     (const DG::TreeViewItemClickEvent& ev, bool* denySelectionChange) override;

    // Palette-Callback
    static GSErrCode __ACENV_CALL PaletteControlCallBack (Int32, API_PaletteMessageID, GS::IntPtr);

    // Items
    DG::IntEdit             densEdit;
    DG::LeftText            densLabel;
    // Icon-Leiste wie in ArchiCADs Paletten: ListView mit drei Icon-Items
    // (rahmenlos, eingebautes Hover, Click-through auf inaktive Palette)
    DG::SingleSelListView   toolbar;
    DG::Icon                toolIcons[4], toolHoverIcons[4];
    bool                    toolEnabled[4] = { true, true, true, true };
    DG::PopUp               cloudPopUp;
    DG::SingleSelTreeView   tree;
    DG::RightText           versionLabel;

    // Icons aus ArchiCADs eigenen Ressourcen
    DG::Icon                iconEyeOpen, iconEyeClosed, iconStory, iconScan, iconCloud;

    GS::Array<PointCloudEntry>  entries;
    std::vector<Node>           nodes;
    CloudState                  state;
    std::vector<StoryEntry>     stories;
    int                         defaultDensity  = 10;
    bool                        importing       = false;

    static GS::Ref<PunktwolkenPalette>  instance;
};

#endif // PUNKTWOLKENPALETTE_HPP
