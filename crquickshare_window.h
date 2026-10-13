// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license.
#ifndef CRQUICKSHARE_CRQUICKSHARE_WINDOW_H_
#define CRQUICKSHARE_CRQUICKSHARE_WINDOW_H_
#include "base/functional/callback.h"
namespace crquickshare {
void ShowCrQuickShareWindow(base::OnceClosure quit);
void DestroyCrQuickShareWindow();
}
#endif
