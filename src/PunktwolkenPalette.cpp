// *****************************************************************************
// PunktwolkenPalette — dockbare Palette zur Punktwolken-Verwaltung
// *****************************************************************************

#include "PunktwolkenPalette.hpp"

static const GS::Guid paletteGuid ("{C7D4E5F6-1A2B-3C4D-8E9F-0A1B2C3D4E5F}");
static const int      PAL_W = 354;

GS::Ref<PunktwolkenPalette> PunktwolkenPalette::instance;

// Reagiert auf APINotify_Quit: Instanz freigeben
static GSErrCode NotificationHandler (API_NotifyEventID notifID, Int32 /*param*/)
{
    if (notifID == APINotify_Quit)
        PunktwolkenPalette::DestroyInstance ();
    return NoError;
}

// --- Konstruktor / Destruktor -------------------------------------------------

PunktwolkenPalette::PunktwolkenPalette () :
    DG::Palette (ACAPI_GetOwnResModule (), 32400, ACAPI_GetOwnResModule (), paletteGuid),
    headerLabel  (GetReference (), kHeaderLabelId),
    addBtn       (GetReference (), kAddBtnId),
    defDensEdit  (GetReference (), kDefDensEditId),
    defDensLabel (GetReference (), kDefDensLabelId),
    delBtn       (GetReference (), kDelBtnId),
    listBox      (GetReference (), kListBoxId),
    relBtn       (GetReference (), kRelBtnId),
    selDensEdit  (GetReference (), kSelDensEditId),
    selDensLabel (GetReference (), kSelDensLabelId),
    versionLabel (GetReference (), kVersionLabelId)
{
    ACAPI_ProjectOperation_CatchProjectEvent (APINotify_Quit, NotificationHandler);

    defDensEdit.SetMin (1); defDensEdit.SetMax (100);
    defDensEdit.SetValue (defaultDensity);

    listBox.SetTabFieldCount (1);
    listBox.SetTabFieldBeginEndPosition (1, 0, PAL_W - 15);

    selDensEdit.SetMin (1); selDensEdit.SetMax (100);
    selDensEdit.SetValue (25);

    versionLabel.SetText (GS::UniString (kVersionStr));

    SetMinClientSize (100, 150);

    Attach      (*this);
    addBtn.Attach  (*this);
    relBtn.Attach  (*this);
    delBtn.Attach  (*this);
    listBox.Attach (*this);

    relBtn.Disable ();
    selDensEdit.Disable ();
    delBtn.Disable ();

    BeginEventProcessing ();
}

PunktwolkenPalette::~PunktwolkenPalette ()
{
    EndEventProcessing ();
    listBox.Detach (*this);
    Detach         (*this);
    addBtn.Detach  (*this);
    relBtn.Detach  (*this);
    delBtn.Detach  (*this);
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

// --- Refresh -----------------------------------------------------------------

void PunktwolkenPalette::Refresh ()
{
    entries = CollectOurLibParts ();

    listBox.DeleteItem (DG::ListBox::AllItems);
    for (int i = 0; i < (int)entries.GetSize (); ++i) {
        GS::UniString text = entries[i].name
            + GS::UniString (" - ")
            + GS::UniString (std::to_string (entries[i].densityPercent).c_str ())
            + GS::UniString ("%");
        listBox.AppendItem ();
        listBox.SetTabItemText ((short)(i + 1), 1, text);
    }

    UpdateFooter ();
}

// --- UpdateFooter ------------------------------------------------------------

void PunktwolkenPalette::UpdateFooter ()
{
    short sel   = listBox.GetSelectedItem ();
    int   total = (int)entries.GetSize ();

    if (sel > 0 && sel <= (short)total) {
        selDensEdit.SetValue (entries[sel - 1].densityPercent);
        relBtn.Enable ();
        selDensEdit.Enable ();
        delBtn.Enable ();
    } else {
        relBtn.Disable ();
        selDensEdit.Disable ();
        delBtn.Disable ();
    }
}

// --- PanelObserver -----------------------------------------------------------

void PunktwolkenPalette::PanelOpened (const DG::PanelOpenEvent& /*ev*/)
{
    SetClientSize (GetOriginalClientWidth (), GetOriginalClientHeight ());
    Refresh ();
}

void PunktwolkenPalette::PanelResized (const DG::PanelResizeEvent& ev)
{
    short dh = ev.GetHorizontalChange ();
    short dv = ev.GetVerticalChange ();
    BeginMoveResizeItems ();
    delBtn.Move (dh, 0);
    listBox.Resize (dh, dv);
    relBtn.Move (0, dv);
    selDensEdit.Move (0, dv);
    selDensLabel.Move (0, dv);
    versionLabel.Move (dh, dv);
    EndMoveResizeItems ();
    listBox.SetTabFieldBeginEndPosition (1, 0, listBox.GetItemWidth ());
}

void PunktwolkenPalette::PanelCloseRequested (const DG::PanelCloseRequestEvent& /*ev*/,
                                               bool* accepted)
{
    *accepted = false;
    Hide ();
}

// --- ListBoxObserver ---------------------------------------------------------

void PunktwolkenPalette::ListBoxSelectionChanged (const DG::ListBoxSelectionEvent& ev)
{
    if (ev.GetSource () == &listBox)
        UpdateFooter ();
}

// --- ButtonItemObserver ------------------------------------------------------

void PunktwolkenPalette::ButtonClicked (const DG::ButtonClickEvent& ev)
{
    const DG::Item* src = ev.GetSource ();

    if (src == &addBtn) {
        if (importing) return;
        importing = true;
        defaultDensity = (int)defDensEdit.GetValue ();
        GS::UniString newName = DoImport (defaultDensity);
        Refresh ();
        if (!newName.IsEmpty ()) {
            for (UInt32 i = 0; i < entries.GetSize (); ++i) {
                if (entries[i].name == newName) {
                    AutoPlaceEntry (entries[i]);
                    break;
                }
            }
        }
        importing = false;
        return;
    }

    short sel = listBox.GetSelectedItem ();
    int   idx = (int)sel - 1;

    if (src == &relBtn && sel > 0 && idx < (int)entries.GetSize ()) {
        int  newDens = (int)selDensEdit.GetValue ();
        bool ok      = DoAdjustDensity (entries[idx], newDens);
        if (ok) {
            entries[idx].densityPercent = newDens;
            GS::UniString text = entries[idx].name
                + GS::UniString (" - ")
                + GS::UniString (std::to_string (newDens).c_str ())
                + GS::UniString ("%");
            listBox.SetTabItemText (sel, 1, text);
            UpdateFooter ();
        }
        return;
    }

    if (src == &delBtn && sel > 0 && idx < (int)entries.GetSize ()) {
        bool deleted = TryDeleteEntry (entries[idx]);
        if (deleted) Refresh ();
        return;
    }
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
