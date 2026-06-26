// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/flutter_windows_surface_export.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>

#include "flutter/fml/logging.h"

namespace flutter {

namespace {

constexpr uint64_t kProducerAcquireKey = 0;
constexpr uint64_t kConsumerAcquireKey = 1;
constexpr uint32_t kFramePumpFramesPerRequest = 18;

uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

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
  pending_frame_pump_frames_ = 0;
  mode_ = mode;
}

FlutterDesktopWindowsSurfaceExportMode FlutterWindowsSurfaceExport::mode()
    const {
  std::scoped_lock lock(mutex_);
  return mode_;
}

void FlutterWindowsSurfaceExport::RecordFrameRequest() {
  std::scoped_lock lock(mutex_);
  if (!shutdown_ &&
      mode_ != kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    ++request_count_;
    pending_frame_pump_frames_ =
        std::max(pending_frame_pump_frames_, kFramePumpFramesPerRequest);
    last_request_time_us_ = NowMicros();
  }
}

void FlutterWindowsSurfaceExport::RecordFrameRequestDispatch() {
  std::scoped_lock lock(mutex_);
  if (!shutdown_ &&
      mode_ != kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    ++request_dispatch_count_;
    last_request_dispatch_time_us_ = NowMicros();
  }
}

void FlutterWindowsSurfaceExport::RecordScheduleFrame() {
  std::scoped_lock lock(mutex_);
  if (!shutdown_ &&
      mode_ != kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    ++schedule_frame_count_;
    last_schedule_frame_time_us_ = NowMicros();
  }
}

void FlutterWindowsSurfaceExport::RecordVsync() {
  std::scoped_lock lock(mutex_);
  if (!shutdown_ &&
      mode_ != kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    ++vsync_count_;
    last_vsync_time_us_ = NowMicros();
  }
}

void FlutterWindowsSurfaceExport::RecordPresent() {
  std::scoped_lock lock(mutex_);
  if (!shutdown_ &&
      mode_ != kFlutterDesktopWindowsSurfaceExportModeDisabled) {
    ++present_count_;
    last_present_time_us_ = NowMicros();
  }
}

void FlutterWindowsSurfaceExport::RecordExportGpuSync(
    bool waited_for_completion) {
  std::scoped_lock lock(mutex_);
  if (waited_for_completion) {
    ++export_finish_count_;
  } else {
    ++export_flush_count_;
  }
  last_export_sync_time_us_ = NowMicros();
}

void FlutterWindowsSurfaceExport::RecordExportMakeCurrentFail() {
  std::scoped_lock lock(mutex_);
  ++export_make_current_fail_count_;
}

void FlutterWindowsSurfaceExport::RecordExportPublishFail() {
  std::scoped_lock lock(mutex_);
  ++export_publish_fail_count_;
}

bool FlutterWindowsSurfaceExport::ConsumeFramePumpToken() {
  std::scoped_lock lock(mutex_);
  if (shutdown_ ||
      mode_ != kFlutterDesktopWindowsSurfaceExportModeCompositorOwned ||
      pending_frame_pump_frames_ == 0) {
    return false;
  }
  --pending_frame_pump_frames_;
  return true;
}

bool FlutterWindowsSurfaceExport::GetState(
    FlutterDesktopWindowsSurfaceExportState* state_out) const {
  if (state_out == nullptr ||
      state_out->struct_size <
          sizeof(FlutterDesktopWindowsSurfaceExportState)) {
    return false;
  }
  std::scoped_lock lock(mutex_);
  state_out->mode = mode_;
  state_out->ring_generation = latest_ring_ ? latest_ring_->generation : 0;
  state_out->frame_generation =
      latest_slot_ ? latest_slot_->frame_generation : 0;
  state_out->publish_count = publish_count_;
  state_out->request_count = request_count_;
  state_out->request_dispatch_count = request_dispatch_count_;
  state_out->schedule_frame_count = schedule_frame_count_;
  state_out->vsync_count = vsync_count_;
  state_out->present_count = present_count_;
  state_out->export_begin_count = export_begin_count_;
  state_out->export_begin_fail_count = export_begin_fail_count_;
  state_out->export_make_current_fail_count =
      export_make_current_fail_count_;
  state_out->export_publish_fail_count = export_publish_fail_count_;
  state_out->export_flush_count = export_flush_count_;
  state_out->export_finish_count = export_finish_count_;
  state_out->backpressure_count = backpressure_count_;
  state_out->pending_frame_pump_frames = pending_frame_pump_frames_;
  state_out->width =
      latest_ring_ ? static_cast<uint32_t>(latest_ring_->width) : 0;
  state_out->height =
      latest_ring_ ? static_cast<uint32_t>(latest_ring_->height) : 0;
  state_out->latest_slot = latest_slot_ ? latest_slot_->index : 0;
  state_out->latest_available = latest_ring_ != nullptr &&
                                latest_slot_ != nullptr &&
                                !latest_slot_->writing;
  state_out->shutdown = shutdown_;
  state_out->last_request_time_us = last_request_time_us_;
  state_out->last_request_dispatch_time_us =
      last_request_dispatch_time_us_;
  state_out->last_schedule_frame_time_us = last_schedule_frame_time_us_;
  state_out->last_vsync_time_us = last_vsync_time_us_;
  state_out->last_present_time_us = last_present_time_us_;
  state_out->last_begin_time_us = last_begin_time_us_;
  state_out->last_begin_fail_time_us = last_begin_fail_time_us_;
  state_out->last_backpressure_time_us = last_backpressure_time_us_;
  state_out->last_publish_time_us = last_publish_time_us_;
  state_out->last_export_sync_time_us = last_export_sync_time_us_;
  state_out->last_acquire_time_us = last_acquire_time_us_;
  state_out->last_release_time_us = last_release_time_us_;
  state_out->active_lease_count =
      static_cast<uint32_t>(std::min<size_t>(
          leases_.size(), std::numeric_limits<uint32_t>::max()));
  uint32_t writing_slot_count = 0;
  uint32_t leased_slot_count = 0;
  if (active_ring_) {
    for (const auto& slot : active_ring_->slots) {
      if (!slot) {
        continue;
      }
      if (slot->writing) {
        ++writing_slot_count;
      }
      if (slot->lease_count != 0) {
        ++leased_slot_count;
      }
    }
  }
  state_out->writing_slot_count = writing_slot_count;
  state_out->leased_slot_count = leased_slot_count;
  state_out->retired_ring_count =
      static_cast<uint32_t>(std::min<size_t>(
          retired_rings_.size(), std::numeric_limits<uint32_t>::max()));
  state_out->latest_slot_lease_count =
      latest_slot_ ? latest_slot_->lease_count : 0;
  state_out->acquire_count = acquire_count_;
  state_out->release_count = release_count_;
  return true;
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
    ++export_begin_fail_count_;
    last_begin_fail_time_us_ = NowMicros();
    return std::nullopt;
  }

  if (!active_ring_ || active_ring_->width != width ||
      active_ring_->height != height) {
    RetireActiveRingLocked();
    active_ring_ = CreateRing(width, height);
    if (!active_ring_) {
      ++export_begin_fail_count_;
      last_begin_fail_time_us_ = NowMicros();
      return std::nullopt;
    }
  }

  Slot* slot = FindWritableSlotLocked();
  if (slot == nullptr) {
    ++backpressure_count_;
    ++export_begin_fail_count_;
    last_begin_fail_time_us_ = NowMicros();
    last_backpressure_time_us_ = last_begin_fail_time_us_;
    return std::nullopt;
  }

  HRESULT result = slot->keyed_mutex->AcquireSync(kProducerAcquireKey, 0);
  if (result != S_OK && slot != latest_slot_) {
    // A slot can contain an older published frame that was superseded before
    // the native compositor acquired it. In that case its keyed mutex is still
    // waiting on the consumer key, but no consumer can legally observe it now
    // because it is no longer latest and has no outstanding lease.
    result = slot->keyed_mutex->AcquireSync(kConsumerAcquireKey, 0);
  }
  if (result != S_OK) {
    ++backpressure_count_;
    ++export_begin_fail_count_;
    last_begin_fail_time_us_ = NowMicros();
    last_backpressure_time_us_ = last_begin_fail_time_us_;
    return std::nullopt;
  }
  ++export_begin_count_;
  last_begin_time_us_ = NowMicros();
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
    ++publish_count_;
    last_publish_time_us_ = NowMicros();
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
    std::scoped_lock lock(mutex_);
    if (latest_slot_ == published_slot) {
      latest_slot_ = nullptr;
      latest_ring_.reset();
    }
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
  ++acquire_count_;
  last_acquire_time_us_ = NowMicros();

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

bool FlutterWindowsSurfaceExport::AcquireLatestV2(
    const FlutterDesktopWindowsSurfaceAcquireOptions* options,
    FlutterDesktopWindowsSurfaceV2* surface_out) {
  if (surface_out == nullptr ||
      surface_out->struct_size < sizeof(FlutterDesktopWindowsSurfaceV2)) {
    return false;
  }

  FlutterDesktopWindowsSurfaceBackend requested_backend =
      kFlutterDesktopWindowsSurfaceBackendUnknown;
  if (options != nullptr &&
      options->struct_size >=
          sizeof(FlutterDesktopWindowsSurfaceAcquireOptions)) {
    requested_backend = options->requested_backend;
  }

  if (requested_backend == kFlutterDesktopWindowsSurfaceBackendD3D12) {
    return false;
  }
  if (requested_backend != kFlutterDesktopWindowsSurfaceBackendUnknown &&
      requested_backend != kFlutterDesktopWindowsSurfaceBackendD3D11) {
    return false;
  }

  FlutterDesktopWindowsSurface surface = {};
  surface.struct_size = sizeof(FlutterDesktopWindowsSurface);
  if (!AcquireLatest(&surface)) {
    return false;
  }

  surface_out->backend = kFlutterDesktopWindowsSurfaceBackendD3D11;
  surface_out->sync = kFlutterDesktopWindowsSurfaceSyncKeyedMutex;
  surface_out->texture_handle = surface.shared_texture_handle;
  surface_out->fence_handle = nullptr;
  surface_out->fence_value = 0;
  surface_out->width = surface.width;
  surface_out->height = surface.height;
  surface_out->format = surface.format;
  surface_out->alpha_mode = surface.alpha_mode;
  surface_out->ring_generation = surface.ring_generation;
  surface_out->frame_generation = surface.frame_generation;
  surface_out->slot = surface.slot;
  surface_out->consumer_acquire_key = surface.consumer_acquire_key;
  surface_out->producer_release_key = surface.producer_release_key;
  surface_out->lease_id = surface.lease_id;
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
  ++release_count_;
  last_release_time_us_ = NowMicros();
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
