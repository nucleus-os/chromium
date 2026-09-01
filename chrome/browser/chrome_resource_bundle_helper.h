// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROME_RESOURCE_BUNDLE_HELPER_H_
#define CHROME_BROWSER_CHROME_RESOURCE_BUNDLE_HELPER_H_

#include <string>

#include "ui/base/resource/resource_bundle.h"

class ChromeFeatureListCreator;

// Loads the local state, and returns the application locale. An empty return
// value indicates the ResouceBundle couldn't be loaded.
std::string LoadLocalState(
    ChromeFeatureListCreator* chrome_feature_list_creator,
    ui::ResourceBundle::Delegate* resource_bundle_delegate,
    bool is_running_tests);

#endif  // CHROME_BROWSER_CHROME_RESOURCE_BUNDLE_HELPER_H_
