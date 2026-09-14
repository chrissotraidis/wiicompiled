#include "wup028_adapter.h"
#include <dolphin/pad.h>

namespace Wup028Adapter {

void Initialize() {}
void Shutdown() {}
AdapterInfo GetInfo() { return {}; }
bool Read(std::array<PADStatus, 4>& statuses) {
  for (PADStatus& status : statuses) status = {};
  return false;
}
void SetPortAssignment(std::uint32_t, int) {}
int GetPortAssignment(std::uint32_t) { return -1; }
bool SetRumble(std::uint32_t, bool) { return false; }

}  // namespace Wup028Adapter
