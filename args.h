// Copyright 2017 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

// Helpers for parsing command line arguments. No include guard needed.

#include "pik_params.h"  // Override

#include <stdio.h>
#include <string>

#include "status.h"
#include "codec.h"

namespace pik {

static inline bool ParseOverride(const char* arg, Override* out) {
  const std::string s_arg(arg);
  if (s_arg == "1") {
    *out = Override::kOn;
    return true;
  }
  if (s_arg == "0") {
    *out = Override::kOff;
    return true;
  }
  fprintf(stderr, "Invalid flag, %s must be 0 or 1\n", arg);
  return PIK_FAILURE("Args");
}

static inline bool ParseUnsigned(const char* arg, size_t* out) {
  char* end;
  *out = static_cast<size_t>(strtoull(arg, &end, 0));
  if (end[0] != '\0') {
    fprintf(stderr, "Unable to interpret as unsigned integer: %s.\n", arg);
    return PIK_FAILURE("Args");
  }
  return true;
}

static inline bool ParseGaborishStrength(const char* arg,
                                         GaborishStrength* out) {
  size_t strength;
  if (!ParseUnsigned(arg, &strength)) return false;
  if (strength >= static_cast<size_t>(GaborishStrength::kMaxValue)) {
    fprintf(stderr, "Invalid GaborishStrenght value: %s.\n", arg);
    return PIK_FAILURE("Args");
  }
  *out = static_cast<GaborishStrength>(strength);
  return true;
}

static inline bool ParseFloat(const char* arg, float* out) {
  char* end;
  *out = static_cast<float>(strtod(arg, &end));
  if (end[0] != '\0') {
    fprintf(stderr, "Unable to interpret as double: %s.\n", arg);
    return PIK_FAILURE("Args");
  }
  return true;
}

static inline bool ParseAndAppendKeyValue(const char* arg, DecoderHints* out) {
  const char* eq = strchr(arg, '=');
  if (!eq) {
    fprintf(stderr, "Expected argument as 'key=value' but received '%s'\n",
            arg);
    return false;
  }
  std::string key(arg, eq);
  out->Add(key, std::string(eq + 1));
  return true;
}

static inline bool ParseString(const char* arg, std::string* out) {
  out->assign(arg);
  return true;
}

static inline bool ParseCString(const char* arg, const char** out) {
  *out = arg;
  return true;
}

static inline bool SetBooleanTrue(bool* out) {
  *out = true;
  return true;
}

}  // namespace pik
