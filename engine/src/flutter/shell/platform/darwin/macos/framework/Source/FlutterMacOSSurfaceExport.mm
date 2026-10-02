// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterMacOSSurfaceExport.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>

#include <algorithm>
#include <chrono>
#include <limits>

#include "flutter/fml/logging.h"

NSNotificationName const FlutterMacOSSurfaceExportPublishedNotification =
    @"VoidPlayerMacOSFlutterSurfaceExportPublished";

namespace {

constexpr NSUInteger kRingSize = 3;
constexpr uint64_t kFrameStreamIdleTimeoutNs = 150ull * 1000ull * 1000ull;
constexpr uint64_t kFrameStreamSafetyFrameLimit = 600;

uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

NSString* FlutterMacOSSurfaceExportPixelFormatString(MTLPixelFormat pixelFormat) {
  switch (pixelFormat) {
    case MTLPixelFormatBGRA8Unorm:
      return @"MTLPixelFormatBGRA8Unorm";
    case MTLPixelFormatBGRA10_XR:
      return @"MTLPixelFormatBGRA10_XR";
    default:
      return [NSString stringWithFormat:@"MTLPixelFormat(%lu)", (unsigned long)pixelFormat];
  }
}

}  // namespace

@interface FlutterMacOSSurfaceExportSlot : NSObject

- (nullable instancetype)initWithIndex:(uint32_t)index
                                 width:(NSUInteger)width
                                height:(NSUInteger)height
                                device:(nonnull id<MTLDevice>)device;

@property(readonly, nonatomic) uint32_t index;
@property(readonly, nonatomic) IOSurfaceRef ioSurface;
@property(readonly, nonatomic, nonnull) id<MTLTexture> texture;
@property(readwrite, nonatomic) uint32_t leaseCount;
@property(readwrite, nonatomic) BOOL writing;
@property(readwrite, nonatomic) uint64_t frameGeneration;

@end

@implementation FlutterMacOSSurfaceExportSlot {
  IOSurfaceRef _ioSurface;
  id<MTLTexture> _texture;
  uint32_t _index;
}

- (instancetype)initWithIndex:(uint32_t)index
                        width:(NSUInteger)width
                       height:(NSUInteger)height
                       device:(id<MTLDevice>)device {
  if (width == 0 || height == 0 || width > std::numeric_limits<int32_t>::max() ||
      height > std::numeric_limits<int32_t>::max()) {
    return nil;
  }
  self = [super init];
  if (!self) {
    return nil;
  }
  _index = index;
  const size_t bytesPerElement = 4;
  const size_t bytesPerRow = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * bytesPerElement);
  const size_t totalBytes = IOSurfaceAlignProperty(kIOSurfaceAllocSize, height * bytesPerRow);
  NSDictionary* options = @{
    (id)kIOSurfaceWidth : @(width),
    (id)kIOSurfaceHeight : @(height),
    (id)kIOSurfacePixelFormat : @(kCVPixelFormatType_32BGRA),
    (id)kIOSurfaceBytesPerElement : @(bytesPerElement),
    (id)kIOSurfaceBytesPerRow : @(bytesPerRow),
    (id)kIOSurfaceAllocSize : @(totalBytes),
  };
  _ioSurface = IOSurfaceCreate((CFDictionaryRef)options);
  if (!_ioSurface) {
    return nil;
  }
  IOSurfaceSetValue(_ioSurface, kIOSurfaceColorSpace, kCGColorSpaceSRGB);

  MTLTextureDescriptor* textureDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  textureDescriptor.usage =
      MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
  _texture = [device newTextureWithDescriptor:textureDescriptor iosurface:_ioSurface plane:0];
  if (!_texture) {
    CFRelease(_ioSurface);
    _ioSurface = nullptr;
    return nil;
  }
  return self;
}

- (void)dealloc {
  if (_ioSurface) {
    CFRelease(_ioSurface);
    _ioSurface = nullptr;
  }
}

- (uint32_t)index {
  return _index;
}

- (IOSurfaceRef)ioSurface {
  return _ioSurface;
}

- (id<MTLTexture>)texture {
  return _texture;
}

@end

@interface FlutterMacOSSurfaceExportRing : NSObject

- (nullable instancetype)initWithGeneration:(uint64_t)generation
                                      width:(NSUInteger)width
                                     height:(NSUInteger)height
                                     device:(nonnull id<MTLDevice>)device;

@property(readonly, nonatomic) uint64_t generation;
@property(readonly, nonatomic) NSUInteger width;
@property(readonly, nonatomic) NSUInteger height;
@property(readonly, nonatomic, nonnull) NSArray<FlutterMacOSSurfaceExportSlot*>* slots;

@end

@implementation FlutterMacOSSurfaceExportRing

- (instancetype)initWithGeneration:(uint64_t)generation
                             width:(NSUInteger)width
                            height:(NSUInteger)height
                            device:(id<MTLDevice>)device {
  self = [super init];
  if (!self) {
    return nil;
  }
  NSMutableArray<FlutterMacOSSurfaceExportSlot*>* slots =
      [NSMutableArray arrayWithCapacity:kRingSize];
  for (NSUInteger index = 0; index < kRingSize; ++index) {
    FlutterMacOSSurfaceExportSlot* slot =
        [[FlutterMacOSSurfaceExportSlot alloc] initWithIndex:static_cast<uint32_t>(index)
                                                       width:width
                                                      height:height
                                                      device:device];
    if (!slot) {
      return nil;
    }
    [slots addObject:slot];
  }
  _generation = generation;
  _width = width;
  _height = height;
  _slots = [slots copy];
  return self;
}

@end

@interface FlutterMacOSSurfaceExportLease : NSObject

@property(readwrite, nonatomic) uint64_t leaseId;
@property(readwrite, strong, nonatomic, nonnull) FlutterMacOSSurfaceExportRing* ring;
@property(readwrite, strong, nonatomic, nonnull) FlutterMacOSSurfaceExportSlot* slot;

@end

@implementation FlutterMacOSSurfaceExportLease
@end

@implementation FlutterMacOSSurfaceExport {
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLRenderPipelineState> _copyPipelineState;
  id<MTLSamplerState> _copySamplerState;

  FlutterMacOSSurfaceExportRing* _activeRing;
  NSMutableArray<FlutterMacOSSurfaceExportRing*>* _retiredRings;
  FlutterMacOSSurfaceExportRing* _latestRing;
  FlutterMacOSSurfaceExportSlot* _latestSlot;
  NSMutableArray<FlutterMacOSSurfaceExportLease*>* _leases;

  BOOL _enabled;
  uint64_t _nextRingGeneration;
  uint64_t _nextFrameGeneration;
  uint64_t _nextLeaseId;
  uint64_t _publishCount;
  uint64_t _requestCount;
  uint64_t _requestDispatchCount;
  uint64_t _scheduleFrameCount;
  uint64_t _presentCount;
  uint64_t _exportBeginCount;
  uint64_t _exportBeginFailCount;
  uint64_t _exportPublishFailCount;
  uint64_t _backpressureCount;
  uint64_t _acquireCount;
  uint64_t _releaseCount;
  uint64_t _autoPublishCount;
  uint64_t _autoPublishSkippedNoAcquireCount;
  uint64_t _wakeupRequestCount;
  uint64_t _frameStreamArmCount;
  uint64_t _frameStreamIdleDisarmCount;
  uint64_t _frameStreamSafetyDisarmCount;
  uint64_t _frameStreamFramesSinceRequest;
  uint64_t _lastPresentNs;
  uint64_t _previousPresentNs;
  uint32_t _pendingFramePumpFrames;
  BOOL _frameStreamArmed;
  BOOL _latestAcquiredSincePublish;
  NSString* _lastError;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device
                  commandQueue:(id<MTLCommandQueue>)commandQueue {
  self = [super init];
  if (self) {
    _device = device;
    _commandQueue = commandQueue;
    _retiredRings = [NSMutableArray array];
    _leases = [NSMutableArray array];
    _nextRingGeneration = 1;
    _nextFrameGeneration = 1;
    _nextLeaseId = 1;
    _latestAcquiredSincePublish = YES;
    _lastError = @"none";
  }
  return self;
}

- (BOOL)isEnabled {
  @synchronized(self) {
    return _enabled;
  }
}

- (void)setEnabled:(BOOL)enabled {
  @synchronized(self) {
    if (_enabled == enabled) {
      return;
    }
    _enabled = enabled;
    _pendingFramePumpFrames = 0;
    _frameStreamArmed = NO;
    _frameStreamFramesSinceRequest = 0;
    _latestAcquiredSincePublish = YES;
    if (!enabled) {
      _activeRing = nil;
      _latestRing = nil;
      _latestSlot = nil;
      [_retiredRings removeAllObjects];
      [_leases removeAllObjects];
      _lastError = @"disabled";
    } else {
      _lastError = @"none";
    }
  }
}

- (BOOL)requestFrame {
  @synchronized(self) {
    if (!_enabled) {
      _lastError = @"disabled";
      return NO;
    }
    ++_requestCount;
    ++_wakeupRequestCount;
    _lastError = @"none";
    return YES;
  }
}

- (void)recordRequestDispatch {
  @synchronized(self) {
    if (_enabled) {
      ++_requestDispatchCount;
    }
  }
}

- (void)recordScheduleFrame {
  @synchronized(self) {
    if (_enabled) {
      ++_scheduleFrameCount;
    }
  }
}

- (void)recordPresent {
  @synchronized(self) {
    if (_enabled) {
      ++_presentCount;
      _previousPresentNs = _lastPresentNs;
      _lastPresentNs = NowNs();
    }
  }
}

- (void)publishSurfaceIfNeeded:(FlutterSurface*)surface {
  if (!surface || !surface.texture) {
    return;
  }
  id<MTLTexture> sourceTexture = surface.texture;
  const NSUInteger width = sourceTexture.width;
  const NSUInteger height = sourceTexture.height;
  if (width == 0 || height == 0) {
    return;
  }

  FlutterMacOSSurfaceExportRing* ring = nil;
  FlutterMacOSSurfaceExportSlot* slot = nil;
  uint64_t ringGeneration = 0;
  @synchronized(self) {
    if (![self shouldPublishForCurrentPresentLocked]) {
      return;
    }
    if (_latestSlot && !_latestAcquiredSincePublish) {
      ++_autoPublishSkippedNoAcquireCount;
      _lastError = @"latest-not-acquired";
      return;
    }
    if (!_activeRing || _activeRing.width != width || _activeRing.height != height) {
      [self retireActiveRingLocked];
      _activeRing = [[FlutterMacOSSurfaceExportRing alloc] initWithGeneration:_nextRingGeneration++
                                                                        width:width
                                                                       height:height
                                                                       device:_device];
      if (!_activeRing) {
        ++_exportBeginFailCount;
        _lastError = @"ring-create-failed";
        return;
      }
    }
    slot = [self findWritableSlotLocked];
    if (!slot) {
      ++_backpressureCount;
      ++_exportBeginFailCount;
      _lastError = @"backpressure";
      return;
    }
    slot.writing = YES;
    _pendingFramePumpFrames = 0;
    ++_autoPublishCount;
    ++_exportBeginCount;
    ring = _activeRing;
    ringGeneration = ring.generation;
  }

  NSError* pipelineError = nil;
  if (![self ensureCopyPipelineWithError:&pipelineError]) {
    FML_LOG(ERROR) << "macOS surface export failed to create copy pipeline: "
                   << (pipelineError.localizedDescription.UTF8String ?: "unknown");
    [self cancelSlot:slot ringGeneration:ringGeneration reason:@"pipeline-failed"];
    return;
  }

  MTLRenderPassDescriptor* passDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  passDescriptor.colorAttachments[0].texture = slot.texture;
  passDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
  passDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  passDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  if (!commandBuffer) {
    [self cancelSlot:slot ringGeneration:ringGeneration reason:@"command-buffer-unavailable"];
    return;
  }
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:passDescriptor];
  if (!encoder) {
    [self cancelSlot:slot ringGeneration:ringGeneration reason:@"encoder-unavailable"];
    return;
  }
  [encoder setRenderPipelineState:_copyPipelineState];
  [encoder setFragmentTexture:sourceTexture atIndex:0];
  [encoder setFragmentSamplerState:_copySamplerState atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];

  __weak FlutterMacOSSurfaceExport* weakSelf = self;
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
    FlutterMacOSSurfaceExport* strongSelf = weakSelf;
    if (!strongSelf) {
      return;
    }
    if (completedBuffer.status == MTLCommandBufferStatusCompleted) {
      [strongSelf completePublishForSlot:slot ring:ring ringGeneration:ringGeneration];
    } else {
      [strongSelf cancelSlot:slot ringGeneration:ringGeneration reason:@"copy-failed"];
    }
  }];
  [commandBuffer commit];
}

- (NSDictionary<NSString*, id>*)acquireLatestSurface {
  @synchronized(self) {
    if (!_enabled || !_latestRing || !_latestSlot || _latestSlot.writing) {
      return nil;
    }
    FlutterMacOSSurfaceExportLease* lease = [[FlutterMacOSSurfaceExportLease alloc] init];
    lease.leaseId = _nextLeaseId++;
    lease.ring = _latestRing;
    lease.slot = _latestSlot;
    ++lease.slot.leaseCount;
    [_leases addObject:lease];
    ++_acquireCount;
    _latestAcquiredSincePublish = YES;

    IOSurfaceRef ioSurface = lease.slot.ioSurface;
    NSMutableDictionary<NSString*, id>* info = [NSMutableDictionary dictionary];
    info[@"texture"] = lease.slot.texture;
    info[@"ioSurface"] = (__bridge id)ioSurface;
    info[@"ioSurfaceId"] = @(IOSurfaceGetID(ioSurface));
    info[@"texturePointer"] = @((uint64_t)(uintptr_t)(__bridge void*)lease.slot.texture);
    info[@"texturePixelFormat"] =
        FlutterMacOSSurfaceExportPixelFormatString(lease.slot.texture.pixelFormat);
    info[@"textureWidth"] = @(lease.slot.texture.width);
    info[@"textureHeight"] = @(lease.slot.texture.height);
    info[@"width"] = @(lease.ring.width);
    info[@"height"] = @(lease.ring.height);
    info[@"pixelFormat"] = @(lease.slot.texture.pixelFormat);
    info[@"ringGeneration"] = @(lease.ring.generation);
    info[@"frameGeneration"] = @(lease.slot.frameGeneration);
    info[@"slot"] = @(lease.slot.index);
    info[@"leaseId"] = @(lease.leaseId);
    info[@"alphaMode"] = @"premultiplied";
    info[@"format"] = @"bgra8";
    return info;
  }
}

- (BOOL)releaseLease:(uint64_t)leaseId {
  @synchronized(self) {
    NSUInteger index = NSNotFound;
    for (NSUInteger i = 0; i < _leases.count; ++i) {
      if (_leases[i].leaseId == leaseId) {
        index = i;
        break;
      }
    }
    if (index == NSNotFound) {
      _lastError = @"lease-not-found";
      return NO;
    }
    FlutterMacOSSurfaceExportLease* lease = _leases[index];
    if (lease.slot.leaseCount > 0) {
      --lease.slot.leaseCount;
    }
    [_leases removeObjectAtIndex:index];
    ++_releaseCount;
    [self collectRetiredRingsLocked];
    _lastError = @"none";
    return YES;
  }
}

- (NSDictionary<NSString*, id>*)stateDictionary {
  @synchronized(self) {
    uint64_t retainedLeaseCount = 0;
    for (FlutterMacOSSurfaceExportLease* lease in _leases) {
      if (lease.leaseId != 0) {
        ++retainedLeaseCount;
      }
    }
    return @{
      @"mode" : _enabled ? @"export-ring-auto" : @"disabled",
      @"enabled" : @(_enabled),
      @"ringGeneration" : @(_latestRing ? _latestRing.generation : 0),
      @"frameGeneration" : @(_latestSlot ? _latestSlot.frameGeneration : 0),
      @"publishCount" : @(_publishCount),
      @"autoPublishEnabled" : @(_enabled),
      @"autoPublishCount" : @(_autoPublishCount),
      @"autoPublishSkippedNoAcquireCount" : @(_autoPublishSkippedNoAcquireCount),
      @"wakeupRequestCount" : @(_wakeupRequestCount),
      @"requestCount" : @(_requestCount),
      @"requestDispatchCount" : @(_requestDispatchCount),
      @"scheduleFrameCount" : @(_scheduleFrameCount),
      @"presentCount" : @(_presentCount),
      @"exportBeginCount" : @(_exportBeginCount),
      @"exportBeginFailCount" : @(_exportBeginFailCount),
      @"exportPublishFailCount" : @(_exportPublishFailCount),
      @"backpressureCount" : @(_backpressureCount),
      @"acquireCount" : @(_acquireCount),
      @"releaseCount" : @(_releaseCount),
      @"pendingFramePumpFrames" : @(_pendingFramePumpFrames),
      @"frameStreamArmed" : @(_frameStreamArmed),
      @"frameStreamArmCount" : @(_frameStreamArmCount),
      @"frameStreamFramesSinceRequest" : @(_frameStreamFramesSinceRequest),
      @"frameStreamIdleDisarmCount" : @(_frameStreamIdleDisarmCount),
      @"frameStreamSafetyDisarmCount" : @(_frameStreamSafetyDisarmCount),
      @"frameStreamIdleTimeoutMs" : @(kFrameStreamIdleTimeoutNs / 1000000ull),
      @"frameStreamSafetyFrameLimit" : @(kFrameStreamSafetyFrameLimit),
      @"width" : @(_latestRing ? _latestRing.width : 0),
      @"height" : @(_latestRing ? _latestRing.height : 0),
      @"latestSlot" : @(_latestSlot ? _latestSlot.index : 0),
      @"latestAvailable" : @(_latestRing != nil && _latestSlot != nil && !_latestSlot.writing),
      @"leaseCount" : @(retainedLeaseCount),
      @"lastError" : _lastError ?: @"none",
    };
  }
}

- (BOOL)ensureCopyPipelineWithError:(NSError**)error {
  if (_copyPipelineState && _copySamplerState) {
    return YES;
  }
  static NSString* const shaderSource =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "struct VertexOut { float4 position [[position]]; float2 uv; };\n"
       "vertex VertexOut voidplayer_surface_export_vertex(uint vertex_id [[vertex_id]]) {\n"
       "  float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
       "  float2 uvs[3] = { float2(0.0, 1.0), float2(2.0, 1.0), float2(0.0, -1.0) };\n"
       "  VertexOut out;\n"
       "  out.position = float4(positions[vertex_id], 0.0, 1.0);\n"
       "  out.uv = uvs[vertex_id];\n"
       "  return out;\n"
       "}\n"
       "fragment half4 voidplayer_surface_export_fragment(VertexOut in [[stage_in]],\n"
       "    texture2d<float> source [[texture(0)]], sampler sourceSampler [[sampler(0)]]) {\n"
       "  return half4(source.sample(sourceSampler, in.uv));\n"
       "}\n";
  NSError* libraryError = nil;
  id<MTLLibrary> library = [_device newLibraryWithSource:shaderSource
                                                 options:nil
                                                   error:&libraryError];
  if (!library) {
    if (error) {
      *error = libraryError;
    }
    return NO;
  }
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = [library newFunctionWithName:@"voidplayer_surface_export_vertex"];
  descriptor.fragmentFunction = [library newFunctionWithName:@"voidplayer_surface_export_fragment"];
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  NSError* pipelineError = nil;
  _copyPipelineState = [_device newRenderPipelineStateWithDescriptor:descriptor
                                                               error:&pipelineError];
  if (!_copyPipelineState) {
    if (error) {
      *error = pipelineError;
    }
    return NO;
  }

  MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
  samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
  samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
  samplerDescriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
  samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  _copySamplerState = [_device newSamplerStateWithDescriptor:samplerDescriptor];
  if (!_copySamplerState) {
    if (error) {
      *error = [NSError errorWithDomain:@"FlutterMacOSSurfaceExport"
                                   code:1
                               userInfo:@{NSLocalizedDescriptionKey : @"failed to create sampler"}];
    }
    return NO;
  }
  return YES;
}

- (BOOL)shouldPublishForCurrentPresentLocked {
  if (!_enabled) {
    return NO;
  }
  return YES;
}

- (FlutterMacOSSurfaceExportSlot*)findWritableSlotLocked {
  if (!_activeRing) {
    return nil;
  }
  for (FlutterMacOSSurfaceExportSlot* slot in _activeRing.slots) {
    if (!slot.writing && slot.leaseCount == 0) {
      return slot;
    }
  }
  return nil;
}

- (void)retireActiveRingLocked {
  if (_activeRing) {
    [_retiredRings addObject:_activeRing];
    _activeRing = nil;
  }
}

- (BOOL)ringCanRetireLocked:(FlutterMacOSSurfaceExportRing*)ring {
  if (ring == _latestRing) {
    return NO;
  }
  for (FlutterMacOSSurfaceExportSlot* slot in ring.slots) {
    if (slot.writing || slot.leaseCount != 0) {
      return NO;
    }
  }
  return YES;
}

- (void)collectRetiredRingsLocked {
  for (NSInteger i = static_cast<NSInteger>(_retiredRings.count) - 1; i >= 0; --i) {
    if ([self ringCanRetireLocked:_retiredRings[static_cast<NSUInteger>(i)]]) {
      [_retiredRings removeObjectAtIndex:static_cast<NSUInteger>(i)];
    }
  }
}

- (void)completePublishForSlot:(FlutterMacOSSurfaceExportSlot*)slot
                          ring:(FlutterMacOSSurfaceExportRing*)ring
                ringGeneration:(uint64_t)ringGeneration {
  uint64_t frameGeneration = 0;
  @synchronized(self) {
    if (!_enabled || !slot.writing || ring.generation != ringGeneration) {
      if (slot.writing) {
        slot.writing = NO;
      }
      return;
    }
    slot.writing = NO;
    slot.frameGeneration = _nextFrameGeneration++;
    _latestRing = ring;
    _latestSlot = slot;
    _latestAcquiredSincePublish = NO;
    ++_publishCount;
    if (_frameStreamArmed) {
      ++_frameStreamFramesSinceRequest;
    }
    frameGeneration = slot.frameGeneration;
    [self collectRetiredRingsLocked];
    _lastError = @"none";
  }

  [[NSNotificationCenter defaultCenter]
      postNotificationName:FlutterMacOSSurfaceExportPublishedNotification
                    object:nil
                  userInfo:@{
                    @"frameGeneration" : @(frameGeneration),
                    @"ringGeneration" : @(ringGeneration),
                    @"slot" : @(slot.index),
                  }];
}

- (void)cancelSlot:(FlutterMacOSSurfaceExportSlot*)slot
    ringGeneration:(uint64_t)ringGeneration
            reason:(NSString*)reason {
  (void)ringGeneration;
  @synchronized(self) {
    if (slot.writing) {
      slot.writing = NO;
    }
    ++_exportPublishFailCount;
    _lastError = reason ?: @"cancelled";
    [self collectRetiredRingsLocked];
  }
}

@end
