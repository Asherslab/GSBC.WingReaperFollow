// REAPER extension entry point: registers actions (with toolbar toggle states), menu entries,
// the main-thread timer and the project-load hook.
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "panel.h"
#include "reaper_api.h"

namespace wf {
namespace {

reaper_plugin_info_t* g_rec = nullptr;

struct Action {
  const char* id;    // stable command identifier, e.g. for toolbars: _WINGFOLLOW_MASTER
  const char* name;  // as shown in the action list
  void (*run)();
  int (*state)();    // toggle state (1 on / 0 off) or nullptr
  int cmd = 0;       // assigned by REAPER
};

Engine& E() { return Engine::Get(); }

Action g_actions[] = {
    {"_WINGFOLLOW_WINDOW", "WING Follow: Show/hide window", [] { PanelToggle(); },
     [] { return PanelIsShown() ? 1 : 0; }},
    {"_WINGFOLLOW_MASTER", "WING Follow: Toggle following the WING (master switch)",
     [] { E().SetMaster(!E().GetSettings().sw.master); }, [] { return E().GetSettings().sw.master ? 1 : 0; }},
    {"_WINGFOLLOW_MASTER_ON", "WING Follow: Start following the WING", [] { E().SetMaster(true); }, nullptr},
    {"_WINGFOLLOW_MASTER_OFF", "WING Follow: Stop following the WING", [] { E().SetMaster(false); }, nullptr},
    {"_WINGFOLLOW_MUTES", "WING Follow: Toggle following mutes (all mappings)",
     [] { E().SetFollowMute(!E().GetSettings().sw.mute); }, [] { return E().GetSettings().sw.mute ? 1 : 0; }},
    {"_WINGFOLLOW_FADERS", "WING Follow: Toggle following faders (all mappings)",
     [] { E().SetFollowFader(!E().GetSettings().sw.fader); }, [] { return E().GetSettings().sw.fader ? 1 : 0; }},
    {"_WINGFOLLOW_CONNECT", "WING Follow: Toggle connection to the WING",
     [] { E().SetConnect(!E().GetSettings().connect); }, [] { return E().GetSettings().connect ? 1 : 0; }},
    {"_WINGFOLLOW_ENABLE_ALL", "WING Follow: Enable all mappings", [] { E().SetAllEnabled(true); }, nullptr},
    {"_WINGFOLLOW_DISABLE_ALL", "WING Follow: Disable all mappings", [] { E().SetAllEnabled(false); }, nullptr},
    {"_WINGFOLLOW_RESYNC", "WING Follow: Re-sync from the WING now", [] { E().Resync(); }, nullptr},
    {"_WINGFOLLOW_SEL_TOGGLE", "WING Follow: Toggle mappings on selected tracks",
     [] { E().ToggleSelectedTracks(); }, nullptr},
    {"_WINGFOLLOW_SEL_MAP", "WING Follow: Map selected tracks by track number (window's \"New\" kind/group)",
     [] {
       const Settings& s = E().GetSettings();
       FollowTarget t;
       t.kind = s.addKind;
       t.strip.type = s.addStripType;
       t.io.group = s.addGroup;
       E().AddSelectedByTrackNumber(t);
     },
     nullptr},
    {"_WINGFOLLOW_SEL_ADDFX", "WING Follow: Add mixer-strip FX for selected tracks' mappings",
     [] { E().AddStripFx(E().SelectedTrackBindingIds()); }, nullptr},
};

Action* FindAction(int cmd) {
  for (Action& a : g_actions) {
    if (a.cmd && a.cmd == cmd) return &a;
  }
  return nullptr;
}

Action* FindActionById(const char* id) {
  for (Action& a : g_actions) {
    if (!strcmp(a.id, id)) return &a;
  }
  return nullptr;
}

bool OnAction(KbdSectionInfo* sec, int command, int val, int val2, int relmode, HWND hwnd) {
  (void)val, (void)val2, (void)relmode, (void)hwnd;
  if (sec && sec->uniqueID != 0) return false;
  Action* a = FindAction(command);
  if (!a) return false;
  a->run();
  return true;
}

int ToggleState(int command) {
  Action* a = FindAction(command);
  if (!a) return -1;
  return a->state ? a->state() : -1;
}

void RefreshToggles() {
  for (Action& a : g_actions) {
    if (a.state && a.cmd) RefreshToolbar2(0, a.cmd);
  }
}

void AddMenuItem(HMENU m, const char* text, int cmd) {
  MENUITEMINFO mi = {};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_TYPE | MIIM_ID;
  mi.fType = text ? MFT_STRING : MFT_SEPARATOR;
  mi.wID = cmd;
  mi.dwTypeData = const_cast<char*>(text ? text : "");
  InsertMenuItem(m, GetMenuItemCount(m), TRUE, &mi);
}

void AddActionItem(HMENU m, const char* id, const char* label) {
  if (Action* a = FindActionById(id)) AddMenuItem(m, label, a->cmd);
}

void AddSubMenu(HMENU parent, const char* label, HMENU sub) {
  MENUITEMINFO mi = {};
  mi.cbSize = sizeof(mi);
  mi.fMask = MIIM_TYPE | MIIM_SUBMENU;
  mi.fType = MFT_STRING;
  mi.hSubMenu = sub;
  mi.dwTypeData = const_cast<char*>(label);
  InsertMenuItem(parent, GetMenuItemCount(parent), TRUE, &mi);
}

void MenuHook(const char* menuidstr, HMENU menu, int flag) {
  if (flag != 0 || !menuidstr) return;  // REAPER shows toggle-action check marks by itself
  if (!strcmp(menuidstr, "Main extensions")) {
    HMENU sub = CreatePopupMenu();
    AddActionItem(sub, "_WINGFOLLOW_WINDOW", "Show window");
    AddMenuItem(sub, nullptr, 0);
    AddActionItem(sub, "_WINGFOLLOW_MASTER", "Follow the WING");
    AddActionItem(sub, "_WINGFOLLOW_MUTES", "Follow mutes");
    AddActionItem(sub, "_WINGFOLLOW_FADERS", "Follow faders");
    AddActionItem(sub, "_WINGFOLLOW_CONNECT", "Connected to WING");
    AddMenuItem(sub, nullptr, 0);
    AddActionItem(sub, "_WINGFOLLOW_ENABLE_ALL", "Enable all mappings");
    AddActionItem(sub, "_WINGFOLLOW_DISABLE_ALL", "Disable all mappings");
    AddActionItem(sub, "_WINGFOLLOW_RESYNC", "Re-sync from WING now");
    AddSubMenu(menu, "WING Follow", sub);
  } else if (!strcmp(menuidstr, "Track control panel context") ||
             !strcmp(menuidstr, "Mixer control panel context")) {
    HMENU sub = CreatePopupMenu();
    AddActionItem(sub, "_WINGFOLLOW_SEL_MAP", "Map selected tracks by track number");
    AddActionItem(sub, "_WINGFOLLOW_SEL_TOGGLE", "Toggle following for selected tracks");
    AddActionItem(sub, "_WINGFOLLOW_SEL_ADDFX", "Add mixer-strip FX");
    AddMenuItem(sub, nullptr, 0);
    AddActionItem(sub, "_WINGFOLLOW_MASTER", "Follow the WING (all tracks)");
    AddActionItem(sub, "_WINGFOLLOW_WINDOW", "Show WING Follow window");
    AddMenuItem(menu, nullptr, 0);
    AddSubMenu(menu, "WING Follow", sub);
  }
}

bool g_firstTick = true;

void OnTimer() {
  if (g_firstTick) {
    g_firstTick = false;
    if (E().GetSettings().windowOpen) PanelShow(true);  // restore the docked window
  }
  E().OnTimer();
}

void BeginLoadProjectState(bool isUndo, project_config_extension_t*) {
  if (!isUndo) E().OnProjectLoad();
}
bool ProcessExtensionLine(const char*, ProjectStateContext*, bool, project_config_extension_t*) { return false; }
void SaveExtensionConfig(ProjectStateContext*, bool, project_config_extension_t*) {}

project_config_extension_t g_projectConfig = {ProcessExtensionLine, SaveExtensionConfig, BeginLoadProjectState,
                                              nullptr};

bool Register() {
  for (Action& a : g_actions) {
    custom_action_register_t reg = {0, a.id, a.name, nullptr};
    a.cmd = g_rec->Register("custom_action", &reg);
  }
  g_rec->Register("hookcommand2", (void*)OnAction);
  g_rec->Register("toggleaction", (void*)ToggleState);
  g_rec->Register("hookcustommenu", (void*)MenuHook);
  g_rec->Register("timer", (void*)OnTimer);
  g_rec->Register("projectconfig", &g_projectConfig);
  AddExtensionsMainMenu();
  return true;
}

void Unregister() {
  g_rec->Register("-timer", (void*)OnTimer);
  g_rec->Register("-hookcommand2", (void*)OnAction);
  g_rec->Register("-toggleaction", (void*)ToggleState);
  g_rec->Register("-hookcustommenu", (void*)MenuHook);
  g_rec->Register("-projectconfig", &g_projectConfig);
}

}  // namespace
}  // namespace wf

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t* rec) {
  using namespace wf;
  if (!rec) {
    if (g_rec) {
      PanelDestroyForShutdown();
      Unregister();
      Engine::Get().Shutdown();
      g_rec = nullptr;
    }
    return 0;
  }
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc) return 0;
  if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;  // REAPER too old for a function we need

  g_rec = rec;
  PanelSetInstance(hInstance);
  Engine::Get().onToggleStateChanged = RefreshToggles;
  Engine::Get().Init();
  Register();
  return 1;
}
