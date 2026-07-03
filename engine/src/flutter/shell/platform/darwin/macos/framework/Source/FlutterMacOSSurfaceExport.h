// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERMACOSSURFACEEXPORT_H_
#define FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERMACOSSURFACEEXPORT_H_

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#import "flutter/shell/platform/darwin/macos/framework/Source/FlutterSurface.h"

FOUNDATION_EXPORT NSNotificationName _Nonnull const FlutterMacOSSurfaceExportPublishedNotification;

@interface FlutterMacOSSurfaceExport : NSObject

- (nonnull instancetype)initWithDevice:(nonnull id<MTLDevice>)device
                          commandQueue:(nonnull id<MTLCommandQueue>)commandQueue
    NS_DESIGNATED_INITIALIZER;

- (nonnull instancetype)init NS_UNAVAILABLE;

@property(readonly, nonatomic, getter=isEnabled) BOOL enabled;

- (void)setEnabled:(BOOL)enabled;
- (BOOL)requestFrame;
- (void)recordRequestDispatch;
- (void)recordScheduleFrame;
- (void)recordPresent;

- (void)publishSurfaceIfNeeded:(nonnull FlutterSurface*)surface;
- (nullable NSDictionary<NSString*, id>*)acquireLatestSurface;
- (BOOL)releaseLease:(uint64_t)leaseId;
- (nonnull NSDictionary<NSString*, id>*)stateDictionary;

@end

#endif  // FLUTTER_SHELL_PLATFORM_DARWIN_MACOS_FRAMEWORK_SOURCE_FLUTTERMACOSSURFACEEXPORT_H_
