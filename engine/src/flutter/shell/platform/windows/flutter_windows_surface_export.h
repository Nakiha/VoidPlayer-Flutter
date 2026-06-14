// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_SURFACE_EXPORT_H_
#define FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_SURFACE_EXPORT_H_

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "flutter/fml/macros.h"
#include "flutter/shell/platform/windows/egl/manager.h"
#include "flutter/shell/platform/windows/egl/surface.h"
#include "flutter/shell/platform/windows/public/flutter_windows.h"

namespace flutter {

// Owns the exported final-frame texture rings for one Flutter view.
//
// The producer runs on Flutter's raster thread. Acquire/release and mode
// changes may run on the platform or compositor thread.
class FlutterWindowsSurfaceExport {
 public:
  static constexpr size_t kRingSize = 3;

  struct WritableSurface {
    egl::Surface* surface = nullptr;
    uint64_t ring_generation = 0;
    uint32_t slot = 0;
  };

  explicit FlutterWindowsSurfaceExport(egl::Manager* manager);
  virtual ~FlutterWindowsSurfaceExport();

  void SetViewHandle(FlutterDesktopViewRef view);
  void SetMode(FlutterDesktopWindowsSurfaceExportMode mode);
  FlutterDesktopWindowsSurfaceExportMode mode() const;

  void SetPublishedCallback(
      FlutterDesktopWindowsSurfacePublishedCallback callback,
      void* user_data);

  // Returns a keyed-mutex-owned surface for the producer to draw into.
  // Returns null when export is disabled or every non-current slot is leased.
  std::optional<WritableSurface> BeginFrame(size_t width, size_t height);

  // Completes or abandons a BeginFrame call.
  bool PublishFrame(const WritableSurface& writable);
  void CancelFrame(const WritableSurface& writable);

  bool AcquireLatest(FlutterDesktopWindowsSurface* surface_out);
  bool Release(uint64_t lease_id);

  uint64_t backpressure_count() const;
  void Shutdown();

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
    HANDLE shared_handle = nullptr;
    std::unique_ptr<egl::Surface> egl_surface;
    uint32_t index = 0;
    uint32_t lease_count = 0;
    bool writing = false;
    uint64_t frame_generation = 0;

    ~Slot();
  };

  struct Ring {
    uint64_t generation = 0;
    size_t width = 0;
    size_t height = 0;
    std::array<std::unique_ptr<Slot>, kRingSize> slots;
  };

  struct Lease {
    uint64_t id = 0;
    std::shared_ptr<Ring> ring;
    Slot* slot = nullptr;
  };

  std::shared_ptr<Ring> CreateRing(size_t width, size_t height);
  Slot* FindWritableSlotLocked();
  Slot* FindSlotLocked(const WritableSurface& writable);
  void RetireActiveRingLocked();
  void CollectRetiredRingsLocked();

  egl::Manager* manager_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;

  mutable std::mutex mutex_;
  FlutterDesktopViewRef view_ = nullptr;
  FlutterDesktopWindowsSurfaceExportMode mode_ =
      kFlutterDesktopWindowsSurfaceExportModeDisabled;
  FlutterDesktopWindowsSurfacePublishedCallback published_callback_ = nullptr;
  void* published_callback_user_data_ = nullptr;
  std::shared_ptr<Ring> active_ring_;
  std::vector<std::shared_ptr<Ring>> retired_rings_;
  Slot* latest_slot_ = nullptr;
  std::shared_ptr<Ring> latest_ring_;
  std::vector<Lease> leases_;
  uint64_t next_ring_generation_ = 1;
  uint64_t next_frame_generation_ = 1;
  uint64_t next_lease_id_ = 1;
  uint64_t backpressure_count_ = 0;
  bool shutdown_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(FlutterWindowsSurfaceExport);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_WINDOWS_FLUTTER_WINDOWS_SURFACE_EXPORT_H_
