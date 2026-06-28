// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <vector>

#include <d3d11_1.h>
#include <wrl/client.h>

#include "flutter/impeller/renderer/backend/gles/gles.h"
#include "flutter/shell/platform/windows/compositor_opengl.h"
#include "flutter/shell/platform/windows/egl/manager.h"
#include "flutter/shell/platform/windows/flutter_windows_view.h"
#include "flutter/shell/platform/windows/testing/egl/mock_context.h"
#include "flutter/shell/platform/windows/testing/egl/mock_manager.h"
#include "flutter/shell/platform/windows/testing/egl/mock_window_surface.h"
#include "flutter/shell/platform/windows/testing/engine_modifier.h"
#include "flutter/shell/platform/windows/testing/flutter_windows_engine_builder.h"
#include "flutter/shell/platform/windows/testing/mock_window_binding_handler.h"
#include "flutter/shell/platform/windows/testing/view_modifier.h"
#include "flutter/shell/platform/windows/testing/windows_test.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

namespace {
using ::testing::AnyNumber;
using ::testing::Return;

void MockGetIntegerv(GLenum name, int* value) {
  if (name == GL_NUM_EXTENSIONS) {
    *value = 1;
  } else {
    *value = 0;
  }
}

const unsigned char* MockGetString(GLenum name) {
  switch (name) {
    case GL_VERSION:
    case GL_SHADING_LANGUAGE_VERSION:
      return reinterpret_cast<const unsigned char*>("3.0");
    default:
      return reinterpret_cast<const unsigned char*>("");
  }
}

const unsigned char* MockGetStringi(GLenum name, int index) {
  if (name == GL_EXTENSIONS) {
    return reinterpret_cast<const unsigned char*>("GL_ANGLE_framebuffer_blit");
  } else {
    return reinterpret_cast<const unsigned char*>("");
  }
}

GLenum MockGetError() {
  return GL_NO_ERROR;
}

void DoNothing() {}

const impeller::ProcTableGLES::Resolver kMockResolver = [](const char* name) {
  std::string function_name{name};

  if (function_name == "glGetString") {
    return reinterpret_cast<void*>(&MockGetString);
  } else if (function_name == "glGetStringi") {
    return reinterpret_cast<void*>(&MockGetStringi);
  } else if (function_name == "glGetIntegerv") {
    return reinterpret_cast<void*>(&MockGetIntegerv);
  } else if (function_name == "glGetError") {
    return reinterpret_cast<void*>(&MockGetError);
  } else {
    return reinterpret_cast<void*>(&DoNothing);
  }
};

class CompositorOpenGLTest : public WindowsTest {
 public:
  CompositorOpenGLTest() = default;
  virtual ~CompositorOpenGLTest() = default;

 protected:
  FlutterWindowsEngine* engine() { return engine_.get(); }
  FlutterWindowsView* view() { return view_.get(); }
  egl::MockManager* egl_manager() { return egl_manager_; }
  egl::MockContext* render_context() { return render_context_.get(); }
  egl::MockWindowSurface* surface() { return surface_; }

  void UseHeadlessEngine() {
    auto egl_manager = std::make_unique<egl::MockManager>();
    render_context_ = std::make_unique<egl::MockContext>();
    egl_manager_ = egl_manager.get();

    EXPECT_CALL(*egl_manager_, render_context)
        .Times(AnyNumber())
        .WillRepeatedly(Return(render_context_.get()));

    FlutterWindowsEngineBuilder builder{GetContext()};

    engine_ = builder.Build();
    EngineModifier modifier{engine_.get()};
    modifier.SetEGLManager(std::move(egl_manager));
  }

  void UseEngineWithView(bool add_surface = true) {
    UseHeadlessEngine();

    auto window = std::make_unique<MockWindowBindingHandler>();
    EXPECT_CALL(*window.get(), SetView).Times(1);
    EXPECT_CALL(*window.get(), GetWindowHandle).WillRepeatedly(Return(nullptr));

    view_ = std::make_unique<FlutterWindowsView>(kImplicitViewId, engine_.get(),
                                                 std::move(window), false,
                                                 BoxConstraints());

    if (add_surface) {
      auto surface = std::make_unique<egl::MockWindowSurface>();
      surface_ = surface.get();

      EXPECT_CALL(*surface_, Destroy).Times(AnyNumber());

      ViewModifier modifier{view_.get()};
      modifier.SetSurface(std::move(surface));
    }
  }

 private:
  std::unique_ptr<FlutterWindowsEngine> engine_;
  std::unique_ptr<FlutterWindowsView> view_;
  std::unique_ptr<egl::MockContext> render_context_;
  egl::MockWindowSurface* surface_;
  egl::MockManager* egl_manager_;

  FML_DISALLOW_COPY_AND_ASSIGN(CompositorOpenGLTest);
};

}  // namespace

TEST_F(CompositorOpenGLTest, CreateBackingStore) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, CreateBackingStoreImpeller) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/true};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));
  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, InitializationFailure) {
  UseHeadlessEngine();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(false));
  EXPECT_FALSE(compositor.CreateBackingStore(config, &backing_store));
}

TEST_F(CompositorOpenGLTest, InitializationRequiresBlit) {
  UseHeadlessEngine();

  const impeller::ProcTableGLES::Resolver resolver = [](const char* name) {
    std::string function_name{name};

    if (function_name == "glBlitFramebuffer" ||
        function_name == "glBlitFramebufferANGLE") {
      return (void*)nullptr;
    }

    return kMockResolver(name);
  };

  auto compositor =
      CompositorOpenGL{engine(), resolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_FALSE(compositor.CreateBackingStore(config, &backing_store));
}

TEST_F(CompositorOpenGLTest, Present) {
  UseEngineWithView();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, ExportFailurePreservesWindowPresentation) {
  UseEngineWithView();
  ASSERT_NE(view()->surface_export(), nullptr);
  view()->surface_export()->SetMode(
      kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);
  view()->surface_export()->RecordFrameRequest();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};
  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};
  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest,
       CompositorOwnedAttemptsFlutterFrameExportWithoutFramePumpToken) {
  UseEngineWithView();
  ASSERT_NE(view()->surface_export(), nullptr);
  view()->surface_export()->SetMode(
      kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};
  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};
  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));

  FlutterDesktopWindowsSurfaceExportState state = {};
  state.struct_size = sizeof(state);
  ASSERT_TRUE(view()->surface_export()->GetState(&state));
  EXPECT_EQ(state.present_count, 1u);
  EXPECT_EQ(state.export_begin_fail_count, 1u);
  EXPECT_EQ(state.publish_count, 0u);
  EXPECT_EQ(state.pending_frame_pump_frames, 0u);

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest,
       CompositorOwnedAttemptsEmptyFrameExportWithoutFramePumpToken) {
  UseEngineWithView();
  ASSERT_NE(view()->surface_export(), nullptr);
  view()->surface_export()->SetMode(
      kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), nullptr, 0));

  FlutterDesktopWindowsSurfaceExportState state = {};
  state.struct_size = sizeof(state);
  ASSERT_TRUE(view()->surface_export()->GetState(&state));
  EXPECT_EQ(state.present_count, 1u);
  EXPECT_EQ(state.export_begin_fail_count, 1u);
  EXPECT_EQ(state.publish_count, 0u);
  EXPECT_EQ(state.pending_frame_pump_frames, 0u);
}

TEST_F(CompositorOpenGLTest, SurfaceExportModeSwitchesAreStable) {
  UseEngineWithView();
  auto* surface_export = view()->surface_export();
  ASSERT_NE(surface_export, nullptr);
  EXPECT_EQ(surface_export->mode(),
            kFlutterDesktopWindowsSurfaceExportModeDisabled);

  surface_export->SetMode(kFlutterDesktopWindowsSurfaceExportModeMirror);
  EXPECT_EQ(surface_export->mode(),
            kFlutterDesktopWindowsSurfaceExportModeMirror);
  surface_export->SetMode(
      kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);
  EXPECT_EQ(surface_export->mode(),
            kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);
  surface_export->SetMode(
      kFlutterDesktopWindowsSurfaceExportModeDisabled);
  EXPECT_EQ(surface_export->mode(),
            kFlutterDesktopWindowsSurfaceExportModeDisabled);
}

TEST(FlutterWindowsSurfaceExportTest,
     PreservesAlphaLeasesBackpressureAndResizeGenerations) {
  auto manager =
      flutter::egl::Manager::Create(flutter::egl::GpuPreference::NoPreference);
  ASSERT_NE(manager, nullptr);
  FlutterWindowsSurfaceExport surface_export(manager.get());
  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeMirror);

  Microsoft::WRL::ComPtr<ID3D11Device> device;
  ASSERT_TRUE(manager->GetDevice(device.GetAddressOf()));
  Microsoft::WRL::ComPtr<ID3D11Device1> device1;
  ASSERT_TRUE(SUCCEEDED(device.As(&device1)));

  uint64_t callback_generation = 0;
  surface_export.SetPublishedCallback(
      [](FlutterDesktopViewRef, uint64_t generation, void* user_data) {
        *static_cast<uint64_t*>(user_data) = generation;
      },
      &callback_generation);

  const auto publish = [&](size_t width, size_t height) {
    auto writable = surface_export.BeginFrame(width, height);
    EXPECT_TRUE(writable.has_value());
    if (!writable.has_value()) {
      return FlutterDesktopWindowsSurface{};
    }
    EXPECT_TRUE(surface_export.PublishFrame(*writable));

    FlutterDesktopWindowsSurface surface = {};
    surface.struct_size = sizeof(surface);
    EXPECT_TRUE(surface_export.AcquireLatest(&surface));
    return surface;
  };
  const auto consume = [&](const FlutterDesktopWindowsSurface& surface,
                           bool verify_alpha) {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(device1->OpenSharedResource1(
        surface.shared_texture_handle, IID_PPV_ARGS(&texture))));
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed_mutex;
    EXPECT_TRUE(SUCCEEDED(texture.As(&keyed_mutex)));
    EXPECT_EQ(keyed_mutex->AcquireSync(surface.consumer_acquire_key, 100),
              S_OK);
    if (verify_alpha) {
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
      ASSERT_TRUE(SUCCEEDED(
          device->CreateRenderTargetView(texture.Get(), nullptr, &rtv)));
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
      device->GetImmediateContext(context.GetAddressOf());
      const FLOAT premultiplied_rgba[] = {0.25f, 0.125f, 0.0625f, 0.5f};
      context->ClearRenderTargetView(rtv.Get(), premultiplied_rgba);
      context->Flush();

      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
      ASSERT_TRUE(SUCCEEDED(
          device->CreateTexture2D(&desc, nullptr, &staging)));
      context->CopyResource(staging.Get(), texture.Get());
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      ASSERT_TRUE(SUCCEEDED(
          context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
      const auto* bgra = static_cast<const uint8_t*>(mapped.pData);
      EXPECT_NEAR(bgra[0] / 255.0, 0.0625, 0.01);
      EXPECT_NEAR(bgra[1] / 255.0, 0.125, 0.01);
      EXPECT_NEAR(bgra[2] / 255.0, 0.25, 0.01);
      EXPECT_NEAR(bgra[3] / 255.0, 0.5, 0.01);
      context->Unmap(staging.Get(), 0);
    }
    EXPECT_TRUE(SUCCEEDED(
        keyed_mutex->ReleaseSync(surface.producer_release_key)));
  };

  auto first = publish(5, 3);
  ASSERT_NE(first.shared_texture_handle, nullptr);
  EXPECT_EQ(first.format, DXGI_FORMAT_B8G8R8A8_UNORM);
  EXPECT_EQ(first.alpha_mode,
            kFlutterDesktopWindowsSurfaceAlphaModePremultiplied);
  EXPECT_EQ(callback_generation, first.frame_generation);
  consume(first, true);

  auto second = publish(5, 3);
  auto third = publish(5, 3);
  consume(second, false);
  consume(third, false);
  EXPECT_NE(first.slot, second.slot);
  EXPECT_NE(second.slot, third.slot);
  EXPECT_FALSE(surface_export.BeginFrame(5, 3).has_value());
  EXPECT_EQ(surface_export.backpressure_count(), 1u);

  EXPECT_TRUE(surface_export.Release(first.lease_id));
  auto writable = surface_export.BeginFrame(5, 3);
  ASSERT_TRUE(writable.has_value());
  surface_export.CancelFrame(*writable);

  auto resized = publish(7, 5);
  consume(resized, false);
  EXPECT_GT(resized.ring_generation, second.ring_generation);
  EXPECT_EQ(resized.width, 7u);
  EXPECT_EQ(resized.height, 5u);

  EXPECT_TRUE(surface_export.Release(second.lease_id));
  EXPECT_TRUE(surface_export.Release(third.lease_id));
  EXPECT_TRUE(surface_export.Release(resized.lease_id));
  surface_export.Shutdown();
  FlutterDesktopWindowsSurface after_shutdown = {};
  after_shutdown.struct_size = sizeof(after_shutdown);
  EXPECT_FALSE(surface_export.AcquireLatest(&after_shutdown));
  EXPECT_FALSE(surface_export.BeginFrame(5, 3).has_value());
}

TEST(FlutterWindowsSurfaceExportTest, AcquireLatestV2ReportsBackend) {
  auto manager =
      flutter::egl::Manager::Create(flutter::egl::GpuPreference::NoPreference);
  ASSERT_NE(manager, nullptr);
  FlutterWindowsSurfaceExport surface_export(manager.get());
  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeMirror);

  auto writable = surface_export.BeginFrame(5, 3);
  ASSERT_TRUE(writable.has_value());
  ASSERT_TRUE(surface_export.PublishFrame(*writable));

  FlutterDesktopWindowsSurfaceAcquireOptions d3d12_options = {};
  d3d12_options.struct_size = sizeof(d3d12_options);
  d3d12_options.requested_backend =
      kFlutterDesktopWindowsSurfaceBackendD3D12;
  FlutterDesktopWindowsSurfaceV2 d3d12_surface = {};
  d3d12_surface.struct_size = sizeof(d3d12_surface);
  ASSERT_TRUE(
      surface_export.AcquireLatestV2(&d3d12_options, &d3d12_surface));
  EXPECT_EQ(d3d12_surface.backend, kFlutterDesktopWindowsSurfaceBackendD3D12);
  EXPECT_EQ(d3d12_surface.sync, kFlutterDesktopWindowsSurfaceSyncKeyedMutex);
  EXPECT_NE(d3d12_surface.texture_handle, nullptr);
  EXPECT_EQ(d3d12_surface.fence_handle, nullptr);
  EXPECT_EQ(d3d12_surface.fence_value, 0u);
  EXPECT_EQ(d3d12_surface.width, 5u);
  EXPECT_EQ(d3d12_surface.height, 3u);
  EXPECT_EQ(d3d12_surface.format, DXGI_FORMAT_B8G8R8A8_UNORM);
  EXPECT_TRUE(surface_export.Release(d3d12_surface.lease_id));

  FlutterDesktopWindowsSurfaceAcquireOptions d3d11_options = {};
  d3d11_options.struct_size = sizeof(d3d11_options);
  d3d11_options.requested_backend =
      kFlutterDesktopWindowsSurfaceBackendD3D11;
  FlutterDesktopWindowsSurfaceV2 surface = {};
  surface.struct_size = sizeof(surface);
  EXPECT_FALSE(surface_export.AcquireLatestV2(&d3d11_options, &surface));
}

TEST(FlutterWindowsSurfaceExportTest, CancelledEmptyFrameIsNotPublished) {
  auto manager =
      flutter::egl::Manager::Create(flutter::egl::GpuPreference::NoPreference);
  ASSERT_NE(manager, nullptr);
  FlutterWindowsSurfaceExport surface_export(manager.get());
  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeMirror);
  auto writable = surface_export.BeginFrame(3, 3);
  ASSERT_TRUE(writable.has_value());
  surface_export.CancelFrame(*writable);

  FlutterDesktopWindowsSurface surface = {};
  surface.struct_size = sizeof(surface);
  EXPECT_FALSE(surface_export.AcquireLatest(&surface));
}

TEST(FlutterWindowsSurfaceExportTest, StateTracksRequestsAndPublishedFrames) {
  auto manager =
      flutter::egl::Manager::Create(flutter::egl::GpuPreference::NoPreference);
  ASSERT_NE(manager, nullptr);
  FlutterWindowsSurfaceExport surface_export(manager.get());

  FlutterDesktopWindowsSurfaceExportState state = {};
  state.struct_size = sizeof(state);
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.mode, kFlutterDesktopWindowsSurfaceExportModeDisabled);
  EXPECT_FALSE(state.latest_available);

  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeMirror);
  surface_export.RecordFrameRequest();
  surface_export.RecordExportGpuSync(false);
  auto writable = surface_export.BeginFrame(3, 3);
  ASSERT_TRUE(writable.has_value());
  ASSERT_TRUE(surface_export.PublishFrame(*writable));

  state = {};
  state.struct_size = sizeof(state);
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.mode, kFlutterDesktopWindowsSurfaceExportModeMirror);
  EXPECT_EQ(state.request_count, 1u);
  EXPECT_EQ(state.publish_count, 1u);
  EXPECT_EQ(state.export_flush_count, 1u);
  EXPECT_EQ(state.export_finish_count, 0u);
  EXPECT_EQ(state.frame_generation, 1u);
  EXPECT_EQ(state.ring_generation, 1u);
  EXPECT_EQ(state.width, 3u);
  EXPECT_EQ(state.height, 3u);
  EXPECT_TRUE(state.latest_available);
  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);
  surface_export.RecordFrameRequest();
  state = {};
  state.struct_size = sizeof(state);
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.request_count, 2u);
  EXPECT_GT(state.pending_frame_pump_frames, 0u);
}

TEST(FlutterWindowsSurfaceExportTest, FrameRequestsArmBoundedCompositorPump) {
  FlutterWindowsSurfaceExport surface_export(nullptr);
  FlutterDesktopWindowsSurfaceExportState state = {};
  state.struct_size = sizeof(state);

  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeCompositorOwned);
  surface_export.RecordFrameRequest();
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.request_count, 1u);
  ASSERT_GT(state.pending_frame_pump_frames, 0u);

  const uint64_t initial_pending = state.pending_frame_pump_frames;
  EXPECT_TRUE(surface_export.ConsumeFramePumpToken());
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.pending_frame_pump_frames, initial_pending - 1);

  uint64_t consumed_count = 1;
  while (surface_export.ConsumeFramePumpToken()) {
    ++consumed_count;
  }
  EXPECT_EQ(consumed_count, initial_pending);
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.pending_frame_pump_frames, 0u);
  EXPECT_FALSE(surface_export.ConsumeFramePumpToken());

  surface_export.RecordFrameRequest();
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_GT(state.pending_frame_pump_frames, 0u);
  surface_export.SetMode(kFlutterDesktopWindowsSurfaceExportModeDisabled);
  ASSERT_TRUE(surface_export.GetState(&state));
  EXPECT_EQ(state.pending_frame_pump_frames, 0u);
}

TEST_F(CompositorOpenGLTest, PresentEmpty) {
  UseEngineWithView();

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  // The context will be bound twice: first to initialize the compositor, second
  // to clear the surface.
  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), nullptr, 0));
}

TEST_F(CompositorOpenGLTest, NoSurfaceIgnored) {
  UseEngineWithView(/*add_surface = */ false);

  auto compositor =
      CompositorOpenGL{engine(), kMockResolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_FALSE(compositor.Present(view(), &layer_ptr, 1));

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

TEST_F(CompositorOpenGLTest, PresentUsingANGLEBlitExtension) {
  UseEngineWithView();

  bool resolved_ANGLE_blit = false;
  const impeller::ProcTableGLES::Resolver resolver =
      [&resolved_ANGLE_blit](const char* name) {
        std::string function_name{name};

        if (function_name == "glBlitFramebuffer") {
          return (void*)nullptr;
        } else if (function_name == "glBlitFramebufferANGLE") {
          resolved_ANGLE_blit = true;
          return reinterpret_cast<void*>(&DoNothing);
        }

        return kMockResolver(name);
      };

  auto compositor =
      CompositorOpenGL{engine(), resolver, /*enable_impeller=*/false};

  FlutterBackingStoreConfig config = {};
  FlutterBackingStore backing_store = {};

  EXPECT_CALL(*render_context(), MakeCurrent).WillOnce(Return(true));
  ASSERT_TRUE(compositor.CreateBackingStore(config, &backing_store));

  FlutterLayer layer = {};
  layer.type = kFlutterLayerContentTypeBackingStore;
  layer.backing_store = &backing_store;
  const FlutterLayer* layer_ptr = &layer;

  EXPECT_CALL(*surface(), IsValid).WillRepeatedly(Return(true));
  EXPECT_CALL(*surface(), MakeCurrent).WillOnce(Return(true));
  EXPECT_CALL(*surface(), SwapBuffers).WillOnce(Return(true));
  EXPECT_TRUE(compositor.Present(view(), &layer_ptr, 1));
  EXPECT_TRUE(resolved_ANGLE_blit);

  ASSERT_TRUE(compositor.CollectBackingStore(&backing_store));
}

}  // namespace testing
}  // namespace flutter
