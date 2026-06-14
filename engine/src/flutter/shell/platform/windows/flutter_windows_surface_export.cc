// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/flutter_windows_surface_export.h"

#include <algorithm>
#include <iomanip>
#include <limits>

#include "flutter/fml/logging.h"

namespace flutter {

namespace {

constexpr uint64_t kProducerAcquireKey = 0;
constexpr uint64_t kConsumerAcquireKey = 1;

}  // namespace

FlutterWindowsSurfaceExport::Slot::~Slot() {
  if (egl_surface) {
    egl_surface->Destroy();
  }
  if (shared_handle != nullptr) {
    ::CloseHandle(shared_handle);
  }
}

FlutterWindowsSurfaceExport::FlutterWindowsSurfaceExport(egl::Manager* manager)
    : manager_(manager) {}

FlutterWindowsSurfaceExport::~FlutterWindowsSurfaceExport() {
  Shutdown();
}

void FlutterWindowsSurfaceExport::SetViewHandle(FlutterDesktopViewRef view) {
  std::scoped_lock lock(mutex_);
  view_ = view;
}

void FlutterWindowsSurfaceExport::SetMode(
    FlutterDesktopWindowsSurfaceExportMode mode) {
  std::scoped_lock lock(mutex_);
  if (shutdown_ || mode_ == mode) {
    return;
  }
  mode_ = mode;
}

FlutterDesktopWindowsSurfaceExportMode FlutterWindowsSurfaceExport::mode()
    const {
  std::scoped_lock lock(mutex_);
  return mode_;
}

void FlutterWindowsSurfaceExport::SetPublishedCallback(
    FlutterDesktopWindowsSurfacePublishedCallback callback,
    void* user_data) {
  std::scoped_lock lock(mutex_);
  published_callback_ = callback;
  published_callback_user_data_ = user_data;
}

std::shared_ptr<FlutterWindowsSurfaceExport::Ring>
FlutterWindowsSurfaceExport::CreateRing(size_t width, size_t height) {
  if (manager_ == nullptr || width == 0 || height == 0 ||
      width > std::numeric_limits<UINT>::max() ||
      height > std::numeric_limits<UINT>::max()) {
    return nullptr;
  }
  if (device_ == nullptr &&
      !manager_->GetDevice(device_.GetAddressOf())) {
    return nullptr;
  }

  auto ring = std::make_shared<Ring>();
  ring->generation = next_ring_generation_++;
  ring->width = width;
  ring->height = height;

  for (size_t i = 0; i < ring->slots.size(); ++i) {
    auto slot = std::make_unique<Slot>();
    slot->index = static_cast<uint32_t>(i);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                     D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT result =
        device_->CreateTexture2D(&desc, nullptr, slot->texture.GetAddressOf());
    if (FAILED(result)) {
      FML_LOG(ERROR) << "Windows surface export failed to create texture: 0x"
                     << std::hex << result;
      return nullptr;
    }

    Microsoft::WRL::ComPtr<IDXGIResource1> resource;
    result = slot->texture.As(&resource);
    if (FAILED(result)) {
      return nullptr;
    }
    result = resource->CreateSharedHandle(
        nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &slot->shared_handle);
    if (FAILED(result)) {
      FML_LOG(ERROR)
          << "Windows surface export failed to create shared handle: 0x"
          << std::hex << result;
      return nullptr;
    }
    result = slot->texture.As(&slot->keyed_mutex);
    if (FAILED(result)) {
      return nullptr;
    }

    EGLint attributes[] = {
        EGL_WIDTH,          static_cast<EGLint>(width),
        EGL_HEIGHT,         static_cast<EGLint>(height),
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_NONE};
    EGLSurface egl_surface = manager_->CreateSurfaceFromHandle(
        EGL_D3D_TEXTURE_ANGLE,
        reinterpret_cast<EGLClientBuffer>(slot->texture.Get()), attributes);
    if (egl_surface == EGL_NO_SURFACE) {
      FML_LOG(ERROR)
          << "Windows surface export failed to create ANGLE surface.";
      return nullptr;
    }
    slot->egl_surface = std::make_unique<egl::Surface>(
        manager_->egl_display(), manager_->render_context()->GetHandle(),
        egl_surface);
    ring->slots[i] = std::move(slot);
  }

  return ring;
}

std::optional<FlutterWindowsSurfaceExport::WritableSurface>
FlutterWindowsSurfaceExport::BeginFrame(size_t width, size_t height) {
  std::scoped_lock lock(mutex_);
  if (shutdown_ ||
      mode_ == kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    return std::nullopt;
  }

  if (!active_ring_ || active_ring_->width != width ||
      active_ring_->height != height) {
    RetireActiveRingLocked();
    active_ring_ = CreateRing(width, height);
    if (!active_ring_) {
      return std::nullopt;
    }
  }

  Slot* slot = FindWritableSlotLocked();
  if (slot == nullptr) {
    ++backpressure_count_;
    return std::nullopt;
  }

  HRESULT result = slot->keyed_mutex->AcquireSync(kProducerAcquireKey, 0);
  if (result != S_OK) {
    ++backpressure_count_;
    return std::nullopt;
  }
  slot->writing = true;
  WritableSurface writable;
  writable.surface = slot->egl_surface.get();
  writable.ring_generation = active_ring_->generation;
  writable.slot = slot->index;
  return writable;
}

bool FlutterWindowsSurfaceExport::PublishFrame(
    const WritableSurface& writable) {
  FlutterDesktopWindowsSurfacePublishedCallback callback = nullptr;
  void* callback_user_data = nullptr;
  FlutterDesktopViewRef view = nullptr;
  uint64_t frame_generation = 0;
  std::shared_ptr<Ring> published_ring;
  Slot* published_slot = nullptr;
  {
    std::scoped_lock lock(mutex_);
    Slot* slot = FindSlotLocked(writable);
    if (slot == nullptr || !slot->writing) {
      return false;
    }
    slot->writing = false;
    slot->frame_generation = next_frame_generation_++;
    frame_generation = slot->frame_generation;
    latest_ring_ = active_ring_;
    latest_slot_ = slot;
    published_ring = latest_ring_;
    published_slot = latest_slot_;
    callback = published_callback_;
    callback_user_data = published_callback_user_data_;
    view = view_;
    CollectRetiredRingsLocked();
  }

  HRESULT result =
      published_slot->keyed_mutex->ReleaseSync(kConsumerAcquireKey);
  if (FAILED(result)) {
    return false;
  }
  if (callback != nullptr) {
    callback(view, frame_generation, callback_user_data);
  }
  return true;
}

void FlutterWindowsSurfaceExport::CancelFrame(
    const WritableSurface& writable) {
  std::scoped_lock lock(mutex_);
  Slot* slot = FindSlotLocked(writable);
  if (slot == nullptr || !slot->writing) {
    return;
  }
  slot->writing = false;
  slot->keyed_mutex->ReleaseSync(kProducerAcquireKey);
}

bool FlutterWindowsSurfaceExport::AcquireLatest(
    FlutterDesktopWindowsSurface* surface_out) {
  if (surface_out == nullptr ||
      surface_out->struct_size < sizeof(FlutterDesktopWindowsSurface)) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  if (shutdown_ ||
      mode_ == kFlutterDesktopWindowsSurfaceExportModeDisabled ||
      latest_ring_ == nullptr || latest_slot_ == nullptr ||
      latest_slot_->writing) {
    return false;
  }

  Lease lease;
  lease.id = next_lease_id_++;
  lease.ring = latest_ring_;
  lease.slot = latest_slot_;
  ++lease.slot->lease_count;
  leases_.push_back(lease);

  surface_out->shared_texture_handle = lease.slot->shared_handle;
  surface_out->width = static_cast<uint32_t>(lease.ring->width);
  surface_out->height = static_cast<uint32_t>(lease.ring->height);
  surface_out->format = DXGI_FORMAT_B8G8R8A8_UNORM;
  surface_out->alpha_mode =
      kFlutterDesktopWindowsSurfaceAlphaModePremultiplied;
  surface_out->ring_generation = lease.ring->generation;
  surface_out->frame_generation = lease.slot->frame_generation;
  surface_out->slot = lease.slot->index;
  surface_out->consumer_acquire_key = kConsumerAcquireKey;
  surface_out->producer_release_key = kProducerAcquireKey;
  surface_out->lease_id = lease.id;
  return true;
}

bool FlutterWindowsSurfaceExport::Release(uint64_t lease_id) {
  std::scoped_lock lock(mutex_);
  auto lease = std::find_if(
      leases_.begin(), leases_.end(),
      [lease_id](const Lease& candidate) { return candidate.id == lease_id; });
  if (lease == leases_.end()) {
    return false;
  }
  FML_DCHECK(lease->slot->lease_count > 0);
  --lease->slot->lease_count;
  leases_.erase(lease);
  CollectRetiredRingsLocked();
  return true;
}

uint64_t FlutterWindowsSurfaceExport::backpressure_count() const {
  std::scoped_lock lock(mutex_);
  return backpressure_count_;
}

void FlutterWindowsSurfaceExport::Shutdown() {
  std::scoped_lock lock(mutex_);
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  published_callback_ = nullptr;
  published_callback_user_data_ = nullptr;
  view_ = nullptr;
  active_ring_.reset();
  latest_ring_.reset();
  latest_slot_ = nullptr;
  CollectRetiredRingsLocked();
}

FlutterWindowsSurfaceExport::Slot*
FlutterWindowsSurfaceExport::FindWritableSlotLocked() {
  if (!active_ring_) {
    return nullptr;
  }
  for (const auto& slot : active_ring_->slots) {
    if (!slot->writing && slot->lease_count == 0 &&
        (latest_ring_ != active_ring_ || latest_slot_ != slot.get())) {
      return slot.get();
    }
  }
  if (latest_ring_ != active_ring_) {
    for (const auto& slot : active_ring_->slots) {
      if (!slot->writing && slot->lease_count == 0) {
        return slot.get();
      }
    }
  }
  return nullptr;
}

FlutterWindowsSurfaceExport::Slot*
FlutterWindowsSurfaceExport::FindSlotLocked(
    const WritableSurface& writable) {
  if (!active_ring_ ||
      active_ring_->generation != writable.ring_generation ||
      writable.slot >= active_ring_->slots.size()) {
    return nullptr;
  }
  return active_ring_->slots[writable.slot].get();
}

void FlutterWindowsSurfaceExport::RetireActiveRingLocked() {
  if (active_ring_) {
    retired_rings_.push_back(std::move(active_ring_));
  }
}

void FlutterWindowsSurfaceExport::CollectRetiredRingsLocked() {
  retired_rings_.erase(
      std::remove_if(
          retired_rings_.begin(), retired_rings_.end(),
          [this](const std::shared_ptr<Ring>& ring) {
            if (ring == latest_ring_) {
              return false;
            }
            return std::none_of(
                ring->slots.begin(), ring->slots.end(),
                [](const std::unique_ptr<Slot>& slot) {
                  return slot->writing || slot->lease_count != 0;
                });
          }),
      retired_rings_.end());
}

}  // namespace flutter
