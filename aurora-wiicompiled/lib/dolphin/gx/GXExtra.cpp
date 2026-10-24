#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"
#include <SDL3/SDL_init.h>
#include <mutex>
#include <vector>

namespace {
struct Destruction {
  u16 command;
  u64 resource;
};

std::mutex sDestructionMutex;
std::vector<Destruction> sDestructions;
thread_local bool sGxProducerThread = false;

void emitDestruction(const Destruction& entry) {
  GX_WRITE_AURORA(entry.command);

  if (entry.command == GX_AURORA_DESTROY_COPY_TEX) {
    GX_WRITE_U64(entry.resource);
  } else {
    GX_WRITE_U32(static_cast<u32>(entry.resource));
  }
}

void destroyResource(u16 command, u64 resource) {
  if (resource == 0) {
    return;
  }

  const Destruction entry{command, resource};

  if (SDL_IsMainThread() || sGxProducerThread) {
    emitDestruction(entry);
  } else {
    // Loading/heap cleanup threads must not write to the main thread's GX FIFO.
    // Keep only the resource ID: the original GX object may already be freed.
    std::lock_guard lock{sDestructionMutex};
    sDestructions.push_back(entry);
  }
}
} // namespace

void aurora::gx::flush_texture_destructions() {
  std::vector<Destruction> pending;
  {
    std::lock_guard lock{sDestructionMutex};
    pending.swap(sDestructions);
  }

  for (const auto& entry : pending) {
    emitDestruction(entry);
  }
}

extern "C" {
void AuroraSetGxProducerThread() { sGxProducerThread = true; }

void GXDestroyTexObj(GXTexObj* obj_) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  destroyResource(GX_AURORA_DESTROY_TEXOBJ, obj->texObjId);
  obj->texObjId = 0;
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  destroyResource(GX_AURORA_DESTROY_TLUT, obj->tlutObjId);
  obj->tlutObjId = 0;
}

void GXDestroyCopyTex(void* dest) { destroyResource(GX_AURORA_DESTROY_COPY_TEX, reinterpret_cast<u64>(dest)); }
}
