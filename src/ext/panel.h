// The dockable "WING Follow" window.
#pragma once

namespace wf {

void PanelSetInstance(void* hInstance);
void PanelShow(bool show);
bool PanelIsShown();
void PanelToggle();
void PanelDestroyForShutdown();  // closes without forgetting that it was open

}  // namespace wf
