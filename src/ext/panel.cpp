#include "panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "engine.h"
#include "reaper_api.h"
#include "resource.h"

#ifdef _WIN32
#include "WDL/win32_utf8.h"
#else
#include "swell/swell-dlggen.h"
#include "wingfollow.rc_mac_dlg"
#endif
#include "WDL/wingui/wndsize.h"

namespace wf {

namespace {

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
bool g_shuttingDown = false;
WDL_WndSizer g_sizer;
bool g_discoveryPending = false;
uint32_t g_seenBindings = 0, g_seenValues = 0;
bool g_refreshNeeded = true;
std::string g_note;  // transient message shown in the status line

enum Col { kColTrack, kColFollows, kColRoute, kColName, kColOn, kColMute, kColFader, kColOffset,
           kColValue, kColStatus, kColFx, kColCount };
const struct {
  const char* name;
  int width;
} kCols[kColCount] = {
    {"REAPER track", 130}, {"Follows", 80}, {"Route", 150}, {"WING name", 90}, {"On", 34},
    {"M", 26}, {"F", 26}, {"Offset", 46}, {"WING now", 90}, {"Status", 80}, {"Strip FX", 54},
};

struct Row {
  uint32_t id = 0;
  std::string cells[kColCount];
};
std::vector<Row> g_rows;

const char* StatusText(BindingStatus s) {
  switch (s) {
    case BindingStatus::Following: return "following";
    case BindingStatus::Paused: return "paused";
    case BindingStatus::NoConsole: return "no console";
    case BindingStatus::Unresolved: return "not patched";
    case BindingStatus::Off: return "off";
    default: return "";
  }
}

HWND List() { return GetDlgItem(g_hwnd, IDC_LIST); }

std::string GetText(int id) {
  char buf[512] = "";
  GetDlgItemText(g_hwnd, id, buf, sizeof(buf));
  return buf;
}

void AddMenuItem(HMENU m, const char* text, int id, bool checked = false, bool enabled = true) {
  MENUITEMINFO mi = {};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_TYPE | MIIM_ID | MIIM_STATE;
  mi.fType = text ? MFT_STRING : MFT_SEPARATOR;
  mi.fState = (checked ? MFS_CHECKED : 0) | (enabled ? 0 : MFS_GRAYED);
  mi.wID = id;
  mi.dwTypeData = const_cast<char*>(text ? text : "");
  InsertMenuItem(m, GetMenuItemCount(m), TRUE, &mi);
}

// ---- "New mapping" combos -----------------------------------------------------------------------

BindKind SelectedKind() {
  int k = static_cast<int>(SendDlgItemMessage(g_hwnd, IDC_KIND, CB_GETCURSEL, 0, 0));
  return k == 1 ? BindKind::Input : k == 2 ? BindKind::Strip : BindKind::Output;
}

void FillGroups() {
  HWND combo = GetDlgItem(g_hwnd, IDC_GROUP);
  SendMessage(combo, CB_RESETCONTENT, 0, 0);
  const Settings& s = Engine::Get().GetSettings();
  BindKind kind = SelectedKind();
  int sel = 0;
  if (kind == BindKind::Strip) {
    for (int t = 0; t < static_cast<int>(SrcType::Count); t++) {
      const SrcTypeInfo& info = GetSrcTypeInfo(static_cast<SrcType>(t));
      int idx = static_cast<int>(SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)info.label));
      SendMessage(combo, CB_SETITEMDATA, idx, t);
      if (info.type == s.addStripType) sel = idx;
    }
  } else {
    const auto& groups = IoGroups();
    for (size_t g = 0; g < groups.size(); g++) {
      int count = kind == BindKind::Input ? groups[g].inputs : groups[g].outputs;
      if (!count) continue;
      std::string label = std::string(groups[g].key) + " (" + groups[g].label + ")";
      int idx = static_cast<int>(SendMessage(combo, CB_ADDSTRING, 0, (LPARAM)label.c_str()));
      SendMessage(combo, CB_SETITEMDATA, idx, static_cast<LPARAM>(g));
      if (s.addGroup == groups[g].key) sel = idx;
    }
  }
  SendMessage(combo, CB_SETCURSEL, sel, 0);
}

// The target described by the "New" row, numbered `num`.
FollowTarget NewTarget(int num) {
  FollowTarget t;
  t.kind = SelectedKind();
  int sel = static_cast<int>(SendDlgItemMessage(g_hwnd, IDC_GROUP, CB_GETCURSEL, 0, 0));
  int data = sel >= 0 ? static_cast<int>(SendDlgItemMessage(g_hwnd, IDC_GROUP, CB_GETITEMDATA, sel, 0)) : 0;
  if (t.kind == BindKind::Strip) {
    t.strip = Source{static_cast<SrcType>(data), num};
  } else {
    t.io = IoRef{IoGroups()[std::max(0, std::min(data, static_cast<int>(IoGroups().size()) - 1))].key, num};
  }
  return t;
}

void SaveNewDefaults() {
  FollowTarget t = NewTarget(1);
  const Settings& s = Engine::Get().GetSettings();
  Engine::Get().SetAddDefaults(t.kind, t.kind == BindKind::Strip ? s.addGroup : t.io.group,
                               t.kind == BindKind::Strip ? t.strip.type : s.addStripType);
}

// ---- list ---------------------------------------------------------------------------------------

std::vector<uint32_t> SelectedIds() {
  std::vector<uint32_t> ids;
  HWND list = List();
  int n = ListView_GetItemCount(list);
  for (int i = 0; i < n && i < static_cast<int>(g_rows.size()); i++) {
    if (ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED) ids.push_back(g_rows[i].id);
  }
  return ids;
}

std::string FormatDb(double db) {
  if (db <= kWingMinusInfDb + 0.001) return "-inf dB";
  char buf[32];
  snprintf(buf, sizeof(buf), "%+.1f dB", db);
  return buf;
}

std::vector<Row> BuildRows() {
  Engine& e = Engine::Get();
  struct TrackInfo {
    int number;
    std::string name;
  };
  std::map<std::string, TrackInfo> tracks;
  ReaProject* proj = EnumProjects(-1, nullptr, 0);
  int nt = CountTracks(proj);
  for (int i = 0; i < nt; i++) {
    MediaTrack* tr = GetTrack(proj, i);
    char name[256] = "";
    GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
    tracks[e.TrackGuid(tr)] = TrackInfo{i + 1, name};
  }

  struct Sortable {
    int trackNumber;
    Row row;
  };
  std::vector<Sortable> rows;
  const WingState& st = e.State();
  for (const Binding& b : e.Bindings()) {
    Sortable s;
    Row& r = s.row;
    r.id = b.id;
    auto t = tracks.find(b.trackGuid);
    s.trackNumber = t != tracks.end() ? t->second.number : 1 << 30;
    char buf[320];
    if (t != tracks.end()) {
      snprintf(buf, sizeof(buf), "%d: %s", t->second.number, t->second.name.c_str());
      r.cells[kColTrack] = buf;
    } else {
      r.cells[kColTrack] = "(track missing)";
    }
    r.cells[kColFollows] = b.target.Label();
    Resolution res = e.ResolveBinding(b);
    r.cells[kColRoute] = b.target.kind == BindKind::Strip ? "" : res.detail;
    if (res.ok) {
      r.cells[kColName] = st.GetName(res.strip);
      std::string v;
      double db;
      bool muted;
      if (st.GetFaderDb(res.strip, &db)) v = FormatDb(db);
      if (st.GetMute(res.strip, &muted) && muted) v += v.empty() ? "MUTED" : "  MUTED";
      r.cells[kColValue] = v;
    }
    r.cells[kColOn] = b.enabled ? "on" : "off";
    r.cells[kColMute] = b.followMute ? "M" : "-";
    r.cells[kColFader] = b.followFader ? "F" : "-";
    snprintf(buf, sizeof(buf), "%+.1f", b.OffsetDb());
    r.cells[kColOffset] = b.offsetCentiDb ? buf : "";
    r.cells[kColStatus] = StatusText(e.StatusOf(b));
    r.cells[kColFx] = b.fxGuid.empty() ? "" : "yes";
    rows.push_back(s);
  }
  std::stable_sort(rows.begin(), rows.end(), [](const Sortable& a, const Sortable& b) {
    return a.trackNumber < b.trackNumber;
  });
  std::vector<Row> out;
  for (auto& s : rows) out.push_back(s.row);
  return out;
}

void RefreshList() {
  HWND list = List();
  std::vector<Row> rows = BuildRows();
  bool sameShape = rows.size() == g_rows.size();
  for (size_t i = 0; sameShape && i < rows.size(); i++) sameShape = rows[i].id == g_rows[i].id;

  if (!sameShape) {
    std::vector<uint32_t> sel = SelectedIds();
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < rows.size(); i++) {
      LVITEM it = {};
      it.mask = LVIF_TEXT | LVIF_PARAM;
      it.iItem = static_cast<int>(i);
      it.pszText = const_cast<char*>(rows[i].cells[0].c_str());
      it.lParam = static_cast<LPARAM>(rows[i].id);
      ListView_InsertItem(list, &it);
      for (int c = 1; c < kColCount; c++) {
        ListView_SetItemText(list, static_cast<int>(i), c, const_cast<char*>(rows[i].cells[c].c_str()));
      }
      if (std::find(sel.begin(), sel.end(), rows[i].id) != sel.end()) {
        ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
      }
    }
  } else {
    // Only touch cells that changed, to avoid flicker while faders move.
    for (size_t i = 0; i < rows.size(); i++) {
      for (int c = 0; c < kColCount; c++) {
        if (rows[i].cells[c] != g_rows[i].cells[c]) {
          ListView_SetItemText(list, static_cast<int>(i), c, const_cast<char*>(rows[i].cells[c].c_str()));
        }
      }
    }
  }
  g_rows.swap(rows);
}

void RefreshControls() {
  Engine& e = Engine::Get();
  const Settings& s = e.GetSettings();
  const ClientStatus& cs = e.ConsoleStatus();
  CheckDlgButton(g_hwnd, IDC_MASTER, s.sw.master ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(g_hwnd, IDC_GMUTE, s.sw.mute ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(g_hwnd, IDC_GFADER, s.sw.fader ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(g_hwnd, IDC_SUBSCRIBE, s.subscribe ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(g_hwnd, IDC_LOGOSC, s.logOsc ? BST_CHECKED : BST_UNCHECKED);
  SetDlgItemText(g_hwnd, IDC_CONNECT, s.connect ? "Disconnect" : "Connect");
  EnableWindow(GetDlgItem(g_hwnd, IDC_HOST), !s.connect);

  std::string status;
  if (!g_note.empty()) {
    status = g_note;
  } else if (!s.connect) {
    status = "Disconnected  (WING Follow " WF_VERSION ")";
  } else if (!cs.error.empty()) {
    status = cs.error;
  } else if (cs.responding) {
    DiscoveredConsole c;
    status = "Connected";
    if (ParseConsoleInfo(cs.consoleInfo, &c)) status += " to " + c.name + " (" + c.model + ", fw " + c.firmware + ")";
    if (!s.sw.master) status += "  -  following is OFF";
  } else {
    status = "Waiting for WING at " + s.host + "...";
  }
  SetDlgItemText(g_hwnd, IDC_STATUS, status.c_str());
}

void Refresh(bool force) {
  if (!g_hwnd) return;
  Engine& e = Engine::Get();
  if (force || g_refreshNeeded || e.BindingsSerial() != g_seenBindings || e.ValuesSerial() != g_seenValues) {
    g_seenBindings = e.BindingsSerial();
    g_seenValues = e.ValuesSerial();
    g_refreshNeeded = false;
    RefreshList();
    RefreshControls();
  }
}

// ---- actions on selected rows -------------------------------------------------------------------

template <typename F>
void ForSelected(F f) {
  Engine& e = Engine::Get();
  for (uint32_t id : SelectedIds()) {
    const Binding* b = e.FindBinding(id);
    if (!b) continue;
    Binding copy = *b;
    f(copy);
    e.UpdateBinding(copy);
  }
}

void ToggleColumn(uint32_t id, int col) {
  Engine& e = Engine::Get();
  const Binding* b = e.FindBinding(id);
  if (!b) return;
  Binding c = *b;
  if (col == kColOn) c.enabled = !c.enabled;
  else if (col == kColMute) c.followMute = !c.followMute;
  else if (col == kColFader) c.followFader = !c.followFader;
  else return;
  e.UpdateBinding(c);
}

void SelectTrackOf(uint32_t id) {
  Engine& e = Engine::Get();
  const Binding* b = e.FindBinding(id);
  MediaTrack* tr = b ? e.FindTrack(b->trackGuid) : nullptr;
  if (!tr) return;
  SetOnlyTrackSelected(tr);
  Main_OnCommand(40913, 0);  // Track: Vertical scroll selected tracks into view
}

void ShowDiscoveryResults() {
  std::vector<DiscoveredConsole> found = Engine::Get().Client()->GetDiscovered();
  if (found.empty()) {
    g_note = "No WING found on the network (you can still type its IP)";
    return;
  }
  g_note.clear();
  if (found.size() == 1) {
    SetDlgItemText(g_hwnd, IDC_HOST, found[0].ip.c_str());
    return;
  }
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < found.size(); i++) {
    std::string label = found[i].name + "  (" + found[i].ip + ", " + found[i].model + ")";
    AddMenuItem(m, label.c_str(), static_cast<int>(i + 1));
  }
  RECT r;
  GetWindowRect(GetDlgItem(g_hwnd, IDC_FIND), &r);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, r.left, r.bottom, 0, g_hwnd, nullptr);
  DestroyMenu(m);
  if (cmd > 0 && cmd <= static_cast<int>(found.size())) SetDlgItemText(g_hwnd, IDC_HOST, found[cmd - 1].ip.c_str());
}

void ShowListMenu() {
  bool any = !SelectedIds().empty();
  HMENU m = CreatePopupMenu();
  AddMenuItem(m, "Toggle on/off", IDC_TOG_EN, false, any);
  AddMenuItem(m, "Toggle follow mute", IDC_TOG_MUTE, false, any);
  AddMenuItem(m, "Toggle follow fader", IDC_TOG_FADER, false, any);
  AddMenuItem(m, nullptr, 0);
  AddMenuItem(m, "Retarget to the \"New\" settings", IDC_RETARGET, false, any);
  AddMenuItem(m, "Add mixer-strip FX", IDC_ADDFX, false, any);
  AddMenuItem(m, nullptr, 0);
  AddMenuItem(m, "Remove", IDC_REMOVE, false, any);
  POINT p;
  GetCursorPos(&p);
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, g_hwnd, nullptr);
  DestroyMenu(m);
  if (cmd) SendMessage(g_hwnd, WM_COMMAND, cmd, 0);
}

void OnCommand(int id, int code) {
  Engine& e = Engine::Get();
  g_note.clear();
  switch (id) {
    case IDC_CONNECT:
      if (!e.GetSettings().connect) e.SetHost(GetText(IDC_HOST));
      e.SetConnect(!e.GetSettings().connect);
      break;
    case IDC_FIND:
      e.Client()->StartDiscovery();
      g_discoveryPending = true;
      g_note = "Looking for WING consoles...";
      break;
    case IDC_MASTER: e.SetMaster(IsDlgButtonChecked(g_hwnd, IDC_MASTER) == BST_CHECKED); break;
    case IDC_GMUTE: e.SetFollowMute(IsDlgButtonChecked(g_hwnd, IDC_GMUTE) == BST_CHECKED); break;
    case IDC_GFADER: e.SetFollowFader(IsDlgButtonChecked(g_hwnd, IDC_GFADER) == BST_CHECKED); break;
    case IDC_SUBSCRIBE: e.SetSubscribe(IsDlgButtonChecked(g_hwnd, IDC_SUBSCRIBE) == BST_CHECKED); break;
    case IDC_LOGOSC: e.SetLogOsc(IsDlgButtonChecked(g_hwnd, IDC_LOGOSC) == BST_CHECKED); break;
    case IDC_POLLMS:
      if (code == EN_CHANGE) {
        int ms = atoi(GetText(IDC_POLLMS).c_str());
        if (ms >= 20) e.SetPollMs(ms);
      }
      return;
    case IDC_ENABLE_ALL: e.SetAllEnabled(true); break;
    case IDC_DISABLE_ALL: e.SetAllEnabled(false); break;
    case IDC_RESYNC: e.Resync(); break;
    case IDC_KIND:
      if (code == CBN_SELCHANGE) {
        FillGroups();
        SaveNewDefaults();
      }
      return;
    case IDC_GROUP:
      if (code == CBN_SELCHANGE) SaveNewDefaults();
      return;
    case IDC_ADD: {
      int num = std::max(1, atoi(GetText(IDC_NUM).c_str()));
      int n = e.AddForSelectedTracks(NewTarget(num));
      if (!n) g_note = "Select REAPER tracks first (numbers must fit the chosen group)";
      break;
    }
    case IDC_ADD_BYNUM: {
      int n = e.AddSelectedByTrackNumber(NewTarget(1));
      if (!n) g_note = "Select REAPER tracks first (track numbers must fit the chosen group)";
      break;
    }
    case IDC_RETARGET: {
      int num = std::max(1, atoi(GetText(IDC_NUM).c_str()));
      ForSelected([&](Binding& b) {
        FollowTarget t = NewTarget(num++);
        if (t.IsValid()) b.target = t;
      });
      break;
    }
    case IDC_TOG_EN: ForSelected([](Binding& b) { b.enabled = !b.enabled; }); break;
    case IDC_TOG_MUTE: ForSelected([](Binding& b) { b.followMute = !b.followMute; }); break;
    case IDC_TOG_FADER: ForSelected([](Binding& b) { b.followFader = !b.followFader; }); break;
    case IDC_SET_OFS: {
      std::string t = GetText(IDC_OFFSET);
      std::replace(t.begin(), t.end(), ',', '.');
      double db = atof(t.c_str());
      db = std::max(-24.0, std::min(24.0, db));
      ForSelected([&](Binding& b) { b.offsetCentiDb = static_cast<int>(std::lround(db * 100)); });
      break;
    }
    case IDC_ADDFX: e.AddStripFx(SelectedIds()); break;
    case IDC_REMOVE: e.RemoveBindings(SelectedIds()); break;
    default: return;
  }
  Refresh(true);
}

void OnInit(HWND hwnd) {
  g_hwnd = hwnd;
  g_rows.clear();
  Engine& e = Engine::Get();
  const Settings& s = e.GetSettings();

  HWND list = List();
#ifdef _WIN32
  WDL_UTF8_HookListView(list);
  WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_KIND));
  WDL_UTF8_HookComboBox(GetDlgItem(hwnd, IDC_GROUP));
#endif
  ListView_SetExtendedListViewStyleEx(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  for (int c = 0; c < kColCount; c++) {
    LVCOLUMN col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = kCols[c].width;
    col.pszText = const_cast<char*>(kCols[c].name);
    ListView_InsertColumn(list, c, &col);
  }

  SetDlgItemText(hwnd, IDC_HOST, s.host.c_str());
  SetDlgItemText(hwnd, IDC_POLLMS, std::to_string(s.pollMs).c_str());
  SetDlgItemText(hwnd, IDC_NUM, "1");
  SetDlgItemText(hwnd, IDC_OFFSET, "0");

  HWND kind = GetDlgItem(hwnd, IDC_KIND);
  SendMessage(kind, CB_ADDSTRING, 0, (LPARAM) "Output");
  SendMessage(kind, CB_ADDSTRING, 0, (LPARAM) "Input source");
  SendMessage(kind, CB_ADDSTRING, 0, (LPARAM) "Strip");
  SendMessage(kind, CB_SETCURSEL, s.addKind == BindKind::Input ? 1 : s.addKind == BindKind::Strip ? 2 : 0, 0);
  FillGroups();

  g_sizer.init(hwnd);
  g_sizer.init_item(IDC_STATUS, 0, 0, 1, 0);
  g_sizer.init_item(IDC_LIST, 0, 0, 1, 1);
  const int bottom[] = {IDC_LBL_NEW, IDC_KIND, IDC_GROUP, IDC_NUM, IDC_ADD, IDC_ADD_BYNUM, IDC_RETARGET,
                        IDC_LBL_SEL, IDC_TOG_EN, IDC_TOG_MUTE, IDC_TOG_FADER, IDC_LBL_OFS, IDC_OFFSET,
                        IDC_SET_OFS, IDC_ADDFX, IDC_REMOVE};
  for (int id : bottom) g_sizer.init_item(id, 0, 1, 0, 1);

  SetTimer(hwnd, 1, 100, nullptr);
  Refresh(true);
}

WDL_DLGRET DlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_INITDIALOG:
      OnInit(hwnd);
      return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) g_sizer.onResize();
      return 0;
    case WM_TIMER:
      if (g_discoveryPending && !Engine::Get().Client()->IsDiscovering()) {
        g_discoveryPending = false;
        ShowDiscoveryResults();
        g_refreshNeeded = true;
      }
      Refresh(false);
      return 0;
    case WM_COMMAND:
      OnCommand(LOWORD(wParam), HIWORD(wParam));
      return 0;
    case WM_NOTIFY: {
      NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
      if (hdr->idFrom != IDC_LIST) break;
      if (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK) {
        LVHITTESTINFO ht = {};
        GetCursorPos(&ht.pt);
        ScreenToClient(List(), &ht.pt);
        int row = ListView_SubItemHitTest(List(), &ht);
        if (row < 0 || row >= static_cast<int>(g_rows.size())) break;
        uint32_t id = g_rows[row].id;
        if (hdr->code == NM_DBLCLK) {
          SelectTrackOf(id);
        } else if (ht.iSubItem == kColOn || ht.iSubItem == kColMute || ht.iSubItem == kColFader) {
          ToggleColumn(id, ht.iSubItem);
          Refresh(true);
        }
      } else if (hdr->code == NM_RCLICK) {
        ShowListMenu();
      }
      break;
    }
    case WM_CLOSE:
      PanelShow(false);
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, 1);
      g_hwnd = nullptr;
      if (!g_shuttingDown) Engine::Get().SetWindowOpen(false);
      return 0;
  }
  return 0;
}

}  // namespace

void PanelSetInstance(void* hInstance) { g_inst = static_cast<HINSTANCE>(hInstance); }

bool PanelIsShown() { return g_hwnd != nullptr; }

void PanelShow(bool show) {
  if (show) {
    if (!g_hwnd) {
      CreateDialog(g_inst, MAKEINTRESOURCE(IDD_MAIN), GetMainHwnd(), DlgProc);
      if (!g_hwnd) return;
      DockWindowAddEx(g_hwnd, "WING Follow", "wingfollow_main", true);
    }
    DockWindowActivate(g_hwnd);
    Engine::Get().SetWindowOpen(true);
  } else if (g_hwnd) {
    HWND h = g_hwnd;
    DockWindowRemove(h);
    DestroyWindow(h);
  }
}

void PanelToggle() { PanelShow(!PanelIsShown()); }

void PanelDestroyForShutdown() {
  g_shuttingDown = true;
  PanelShow(false);
}

}  // namespace wf
