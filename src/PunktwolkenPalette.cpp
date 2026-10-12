// *****************************************************************************
// PunktwolkenPalette — dockbare Palette zur Punktwolken-Verwaltung
// *****************************************************************************

#include "PunktwolkenPalette.hpp"
#include "RS.hpp"
#include <string>

static const GS::Guid paletteGuid ("{C7D4E5F6-1A2B-3C4D-8E9F-0A1B2C3D4E5F}");

// Icon-IDs = eigene GICN-Ressourcen (RFIX/Pointcloud4ArchicadFix.grc, Bilder in RFIX/Images).
// Die Bilder sind einmalig aus Archicad 26 (macOS) kopiert; in Klammern die dortige ID.
// Frueher wurden sie zur Laufzeit aus Archicads eigenem Ressourcenmodul geladen —
// diese IDs gelten aber nur im Mac-Bundle, unter Windows blieb die Palette ohne Icons.
enum PwIconId {
    kIcoAdd       = 32401,   // (29531) Plus im Kreis (Vorgabe Torben)
    kIcoSettings  = 32402,   // (12382) Einstellungen-Liste (Vorgabe Torben)
    kIcoDelete    = 32403,   // (28346)
    kIcoEyeOpen   = 32404,   // (10015)
    kIcoEyeClosed = 32405,   // (10016)
    kIcoStory     = 32406,   // (12750)
    kIcoScan      = 32407,   // (12411) Vermessungspunkt (grau) — Scanner-Standort
    kIcoCloud     = 32408,   // ( 9900)

    // Hover-Fassungen der Werkzeugleiste (erzeugt von tools/make-hover-svg.py)
    kIcoAddHover      = 32409,
    kIcoSettingsHover = 32410,
    kIcoDeleteHover   = 32411
};

GS::Ref<PunktwolkenPalette> PunktwolkenPalette::instance;

// Reagiert auf APINotify_Quit: Instanz freigeben
static GSErrCode NotificationHandler (API_NotifyEventID notifID, Int32 /*param*/)
{
    if (notifID == APINotify_Quit)
        PunktwolkenPalette::DestroyInstance ();
    return NoError;
}

static GS::UniString CloudLabel (const PointCloudEntry& e)
{
    return e.name + GS::UniString (" - ")
         + GS::UniString (std::to_string (e.densityPercent).c_str ())
         + GS::UniString ("%");
}

// --- Konstruktor / Destruktor -------------------------------------------------

PunktwolkenPalette::PunktwolkenPalette () :
    DG::Palette (ACAPI_GetOwnResModule (), 32400, ACAPI_GetOwnResModule (), paletteGuid),
    densEdit     (GetReference (), kDensEditId),
    densLabel    (GetReference (), kDensLabelId),
    toolbar      (GetReference (), DG::Rect (86, 5, 190, 33), DG::ListView::BottomText,
                  DG::ListView::HideScrollBar, DG::ListView::NoFrame),
    cloudPopUp   (GetReference (), kCloudPopUpId),
    tree         (GetReference (), kTreeId),
    versionLabel (GetReference (), kVersionLabelId),
    iconEyeOpen   (ACAPI_GetOwnResModule (), kIcoEyeOpen),
    iconEyeClosed (ACAPI_GetOwnResModule (), kIcoEyeClosed),
    iconStory     (ACAPI_GetOwnResModule (), kIcoStory),
    iconScan      (ACAPI_GetOwnResModule (), kIcoScan),
    iconCloud     (ACAPI_GetOwnResModule (), kIcoCloud)
{
    ACAPI_ProjectOperation_CatchProjectEvent (APINotify_Quit, NotificationHandler);

    // Icon-Leiste aufbauen
    {
        const short ids[4]      = { 0, kIcoAdd,      kIcoSettings,      kIcoDelete };
        const short hoverIds[4] = { 0, kIcoAddHover, kIcoSettingsHover, kIcoDeleteHover };
        toolbar.SetItemSize (32, 28);
        toolbar.SetImageSize (24, 24);
        for (int i = 1; i <= 3; ++i) {
            // Beide Fassungen sind eigene Ressourcen (24x24, SVG) — nichts wird
            // zur Laufzeit gemalt, also auf macOS und Windows gleich.
            toolIcons[i]      = DG::Icon (ACAPI_GetOwnResModule (), ids[i]);
            toolHoverIcons[i] = DG::Icon (ACAPI_GetOwnResModule (), hoverIds[i]);
            toolbar.AppendItem ();
            toolbar.SetItemText ((short)i, GS::UniString ());
            toolbar.SetItemImage ((short)i, DG::ListView::Icon, toolIcons[i]);
        }
        // Hintergrund: ListView kennt nur Weiss / ButtonFace, eine eigene Farbe wird ignoriert
        // (auch DGSetItemBackgroundColor). ButtonFace ist etwas heller als die Palette — akzeptiert.
        toolbar.SetBackground (DG::ListView::BGButtonFace);
        toolbar.EnableHoverEvent ();
        toolbar.SetHoverInWaitMillisecs (0);
        toolbar.Show ();
    }

    densEdit.SetMin (1); densEdit.SetMax (100);
    densEdit.SetValue (defaultDensity);


    versionLabel.SetText (GS::UniString (kVersionStr));

    SetMinClientSize (200, 200);

    Attach            (*this);
    toolbar.Attach    (*this);
    cloudPopUp.Attach (*this);
    tree.Attach       (*this);

    BeginEventProcessing ();
}

PunktwolkenPalette::~PunktwolkenPalette ()
{
    EndEventProcessing ();
    tree.Detach       (*this);
    toolbar.Detach    (*this);
    cloudPopUp.Detach (*this);
    Detach            (*this);
}

// --- Singleton-Verwaltung ----------------------------------------------------

bool PunktwolkenPalette::HasInstance ()
{
    return instance != nullptr;
}

void PunktwolkenPalette::CreateInstance ()
{
    DBASSERT (!HasInstance ());
    instance = new PunktwolkenPalette ();
    ACAPI_KeepInMemory (true);
}

PunktwolkenPalette& PunktwolkenPalette::GetInstance ()
{
    DBASSERT (HasInstance ());
    return *instance;
}

void PunktwolkenPalette::DestroyInstance ()
{
    instance = nullptr;
}

// --- Refresh: Liste der Punktwolken neu lesen, Auswahl erhalten ---------------

void PunktwolkenPalette::Refresh ()
{
    GS::UniString keepName;
    int sel = SelectedCloud ();
    if (sel >= 0) keepName = entries[sel].name;

    entries = CollectOurLibParts ();

    cloudPopUp.DeleteItem (DG::PopUp::AllItems);
    short selectItem = 0;
    for (UInt32 i = 0; i < entries.GetSize (); ++i) {
        cloudPopUp.AppendItem ();
        cloudPopUp.SetItemText ((short)(i + 1), CloudLabel (entries[i]));
        if (entries[i].name == keepName) selectItem = (short)(i + 1);
    }
    if (selectItem == 0 && entries.GetSize () > 0) selectItem = 1;
    if (selectItem > 0) cloudPopUp.SelectItem (selectItem);

    bool any = entries.GetSize () > 0;
    if (any) cloudPopUp.Enable (); else cloudPopUp.Disable ();
    SetToolEnabled (kToolSettings, any);
    SetToolEnabled (kToolDelete, any);

    RebuildTree ();
}

int PunktwolkenPalette::SelectedCloud () const
{
    short s = cloudPopUp.GetSelectedItem ();
    if (s < 1 || s > (short)entries.GetSize ()) return -1;
    return (int)s - 1;
}

const PunktwolkenPalette::Node* PunktwolkenPalette::FindNode (Int32 tvItem) const
{
    for (const Node& n : nodes)
        if (n.tvItem == tvItem) return &n;
    return nullptr;
}

// --- Baum aufbauen -----------------------------------------------------------

void PunktwolkenPalette::RebuildTree ()
{
    tree.DisableDraw ();
    tree.DeleteItem (DG::TreeView::AllItems);
    nodes.clear ();

    int sel = SelectedCloud ();
    if (sel < 0) { tree.EnableDraw (); return; }
    const PointCloudEntry& e = entries[sel];

    stories = GetStoriesAscending ();
    bool hasState = ReadCloudState (e.libIndex, state);

    // bGroup ggf. auf Geschossanzahl + 1 auffuellen (GDL macht das beim naechsten Lauf ebenso)
    while (hasState && state.bGroup.size () < stories.size () + 1)
        state.bGroup.push_back (true);
    while (hasState && state.bScan.size () < (size_t)state.scanCount)
        state.bScan.push_back (true);

    Int32 rootItem = tree.InsertItem (DG::TreeView::RootItem, DG::TreeView::BottomItem);
    tree.SetItemText (rootItem, CloudLabel (e));
    tree.SetItemNormalIcon (rootItem, iconCloud);
    nodes.push_back ({rootItem, kNodeCloud, sel});

    if (!hasState) {
        Int32 hint = tree.InsertItem (rootItem, DG::TreeView::BottomItem);
        tree.SetItemText (hint, GS::UniString ("(nicht platziert)"));
        tree.ExpandItem (rootItem);
        tree.EnableDraw ();
        return;
    }

    Int32 folder = tree.InsertItem (rootItem, DG::TreeView::BottomItem);
    tree.SetItemText (folder, GS::UniString ("Geschosse"));
    tree.SetItemNormalIcon (folder, iconStory);
    nodes.push_back ({folder, kNodeStoriesFolder, -1});

    // Gruppe "alle": Auge offen, wenn irgendein Geschoss sichtbar
    bool anyVisible = false;
    for (bool b : state.bGroup) anyVisible = anyVisible || b;
    tree.SetItemStateIcon (folder, anyVisible ? iconEyeOpen : iconEyeClosed);

    // Geschosse absteigend (oberstes zuerst), danach "Unter Unterkante"
    auto addStory = [&] (int bgIdx, const GS::UniString& label, bool bold) {
        Int32 it = tree.InsertItem (folder, DG::TreeView::BottomItem);
        tree.SetItemText (it, label);
        tree.SetItemNormalIcon (it, iconStory);
        tree.SetItemStateIcon (it, state.bGroup[bgIdx] ? iconEyeOpen : iconEyeClosed);
        if (bold) tree.SetItemFontStyle (it, DG::Font::Bold);
        nodes.push_back ({it, kNodeStory, bgIdx});

        for (int i = 0; i < state.scanCount; ++i) {
            double z = i < (int)state.zScan.size () ? state.zScan[i] : 0.0;
            if (StoryIndexOfScan (z, stories) != bgIdx) continue;
            Int32 sc = tree.InsertItem (it, DG::TreeView::BottomItem);
            tree.SetItemText (sc, GS::UniString ("Standort ") + GS::UniString (std::to_string (i + 1).c_str ()));
            tree.SetItemNormalIcon (sc, iconScan);
            tree.SetItemStateIcon (sc, state.bScan[i] ? iconEyeOpen : iconEyeClosed);
            nodes.push_back ({sc, kNodeScan, i});
        }
    };

    for (int j = (int)stories.size (); j >= 1; --j) {
        const StoryEntry& st = stories[j - 1];
        GS::UniString label = GS::UniString (std::to_string (st.index).c_str ()) + GS::UniString (". ")
                            + (st.name.IsEmpty () ? GS::UniString ("Geschoss") : st.name);
        addStory (j, label, false);
    }
    {
        short below = stories.empty () ? -1 : (short)(stories.front ().index - 1);
        GS::UniString label = GS::UniString (std::to_string (below).c_str ()) + GS::UniString (". Unter Unterkante");
        addStory (0, label, true);
    }

    tree.ExpandItem (rootItem);
    tree.ExpandItem (folder);
    tree.EnableDraw ();
}

// --- Auge umschalten ---------------------------------------------------------

void PunktwolkenPalette::SetEye (Int32 tvItem, bool visible)
{
    const Node* n = FindNode (tvItem);
    if (n == nullptr) return;
    int sel = SelectedCloud ();
    if (sel < 0) return;

    switch (n->kind) {
        case kNodeStoriesFolder:
            for (size_t k = 0; k < state.bGroup.size (); ++k) state.bGroup[k] = visible;
            for (size_t i = 0; i < state.bScan.size (); ++i) state.bScan[i] = visible;
            break;
        case kNodeStory:
            state.bGroup[n->index] = visible;
            for (int i = 0; i < state.scanCount; ++i) {
                double z = i < (int)state.zScan.size () ? state.zScan[i] : 0.0;
                if (StoryIndexOfScan (z, stories) == n->index) state.bScan[i] = visible;
            }
            break;
        case kNodeScan:
            state.bScan[n->index] = visible;
            break;
        default:
            return;
    }

    WriteCloudVisibility (entries[sel].libIndex, state);
    UpdateEyes ();
}

void PunktwolkenPalette::UpdateEyes ()
{
    for (const Node& n : nodes) {
        bool vis;
        switch (n.kind) {
            case kNodeStory: vis = n.index < (int)state.bGroup.size () && state.bGroup[n.index]; break;
            case kNodeScan:  vis = n.index < (int)state.bScan.size ()  && state.bScan[n.index];  break;
            case kNodeStoriesFolder: {
                vis = false;
                for (bool b : state.bGroup) vis = vis || b;
                break;
            }
            default: continue;
        }
        tree.SetItemStateIcon (n.tvItem, vis ? iconEyeOpen : iconEyeClosed);
    }
}

// --- PanelObserver -----------------------------------------------------------

void PunktwolkenPalette::PanelOpened (const DG::PanelOpenEvent& /*ev*/)
{
    for (short i = 1; i <= 3; ++i)      // Icons beim ersten Oeffnen sonst nicht sichtbar
        toolbar.SetItemImage (i, DG::ListView::Icon, toolIcons[i]);
    toolbar.Redraw ();
    Refresh ();
}

// --- Icon-Leiste -------------------------------------------------------------

void PunktwolkenPalette::SetToolEnabled (ToolItem item, bool enabled)
{
    toolEnabled[item] = enabled;
    toolbar.SetItemStatus ((short)item, enabled);
}

bool PunktwolkenPalette::ListViewSelectionChangeRequested (const DG::ListViewMouseDownEvent& ev)
{
    return ev.GetSource () != &toolbar;     // Leiste: nie selektieren
}

void PunktwolkenPalette::ListViewMouseDown (const DG::ListViewMouseDownEvent& ev, bool* processed)
{
    if (ev.GetSource () != &toolbar) return;
    *processed = true;
    short item = ev.GetListItem ();
    if (item < 1 || item > 3 || !toolEnabled[item]) return;
    switch (item) {
        case kToolAdd:      OnAdd ();      break;
        case kToolSettings: OnSettings (); break;
        case kToolDelete:   OnDelete ();   break;
    }
}

void PunktwolkenPalette::ListViewHoverStarted (const DG::ListViewHoverEvent& ev)
{
    if (ev.GetSource () != &toolbar) return;
    short item = ev.GetListItem ();
    if (item >= 1 && item <= 3 && toolEnabled[item] && toolHoverIcons[item].IsValid ())
        toolbar.SetItemImage (item, DG::ListView::Icon, toolHoverIcons[item]);
}

void PunktwolkenPalette::ListViewHoverEnded (const DG::ListViewHoverEvent& ev)
{
    if (ev.GetSource () != &toolbar) return;
    short item = ev.GetListItem ();
    if (item >= 1 && item <= 3)
        toolbar.SetItemImage (item, DG::ListView::Icon, toolIcons[item]);
}

void PunktwolkenPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    short dh = ev.GetHorizontalChange ();
    short dv = ev.GetVerticalChange ();
    BeginMoveResizeItems ();
    cloudPopUp.Resize (dh, 0);
    tree.Resize (dh, dv);
    versionLabel.Move (dh, dv);
    EndMoveResizeItems ();
}

void PunktwolkenPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent& /*ev*/,
                                               bool* accepted)
{
    *accepted = false;
    Hide ();
}

// --- PopUpObserver -----------------------------------------------------------

void PunktwolkenPalette::PopUpChanged (const DG::PopUpChangeEvent& ev)
{
    if (ev.GetSource () == &cloudPopUp)
        RebuildTree ();
}

// --- TreeViewObserver: Klick aufs Auge ---------------------------------------

void PunktwolkenPalette::TreeViewItemClicked (const DG::TreeViewItemClickEvent& ev,
                                               bool* denySelectionChange)
{
    if (ev.GetSource () != &tree) return;
    if (ev.GetPartId () != DG::TreeViewItemClickEvent::StateIcon) return;

    Int32 item = ev.GetTreeItem ();
    const Node* n = FindNode (item);
    if (n == nullptr || n->kind == kNodeCloud) return;

    bool nowVisible;
    switch (n->kind) {
        case kNodeStory: nowVisible = state.bGroup[n->index]; break;
        case kNodeScan:  nowVisible = state.bScan[n->index];  break;
        default: {
            nowVisible = false;
            for (bool b : state.bGroup) nowVisible = nowVisible || b;
        }
    }
    *denySelectionChange = true;
    SetEye (item, !nowVisible);
}

// --- Button-Aktionen ---------------------------------------------------------

void PunktwolkenPalette::OnAdd ()
{
    if (importing) return;
    importing = true;
    defaultDensity = (int)densEdit.GetValue ();
    GS::UniString newName = DoImport (defaultDensity);
    Refresh ();
    if (!newName.IsEmpty ()) {
        for (UInt32 i = 0; i < entries.GetSize (); ++i) {
            if (entries[i].name == newName) {
                // Erneuter Import derselben E57 (GSM-Neuerzeugung): kein zweites Objekt
                if (!HasPlacedInstances (entries[i].libIndex))
                    AutoPlaceEntry (entries[i]);
                cloudPopUp.SelectItem ((short)(i + 1));
                RebuildTree ();
                break;
            }
        }
    }
    importing = false;
}

void PunktwolkenPalette::OnSettings ()
{
    int sel = SelectedCloud ();
    if (sel < 0) return;
    if (!OpenSettingsDialog (entries[sel].libIndex))
        ACAPI_WriteReport ("Punktwolke ist nicht platziert - kein Einstellungsdialog.", false);
    RebuildTree ();
}

void PunktwolkenPalette::OnDelete ()
{
    int sel = SelectedCloud ();
    if (sel < 0) return;
    if (TryDeleteEntry (entries[sel])) Refresh ();
}

// --- PaletteControlCallBack --------------------------------------------------

GSErrCode PunktwolkenPalette::PaletteControlCallBack (Int32          /*refId*/,
                                                       API_PaletteMessageID msg,
                                                       GS::IntPtr     param)
{
    switch (msg) {
        case APIPalMsg_OpenPalette:
            if (!HasInstance ())
                CreateInstance ();
            GetInstance ().Show ();
            break;

        case APIPalMsg_ClosePalette:
            if (HasInstance ())
                GetInstance ().Hide ();
            break;

        case APIPalMsg_HidePalette_Begin:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().Hide ();
            break;

        case APIPalMsg_HidePalette_End:
            if (HasInstance () && !GetInstance ().IsVisible ())
                GetInstance ().Show ();
            break;

        case APIPalMsg_DisableItems_Begin:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().DisableItems ();
            break;

        case APIPalMsg_DisableItems_End:
            if (HasInstance () && GetInstance ().IsVisible ())
                GetInstance ().EnableItems ();
            break;

        case APIPalMsg_IsPaletteVisible:
            *(reinterpret_cast<bool*> (param)) = HasInstance () && GetInstance ().IsVisible ();
            break;

        default:
            break;
    }
    return NoError;
}

// --- RegisterPaletteControlCallBack ------------------------------------------

GSErrCode PunktwolkenPalette::RegisterPaletteControlCallBack ()
{
    return ACAPI_RegisterModelessWindow (
        GS::CalculateHashValue (paletteGuid),
        PaletteControlCallBack,
        API_PalEnabled_FloorPlan       + API_PalEnabled_Section    + API_PalEnabled_Elevation +
        API_PalEnabled_InteriorElevation + API_PalEnabled_3D       + API_PalEnabled_Detail    +
        API_PalEnabled_Worksheet       + API_PalEnabled_Layout     + API_PalEnabled_DocumentFrom3D,
        GSGuid2APIGuid (paletteGuid));
}
