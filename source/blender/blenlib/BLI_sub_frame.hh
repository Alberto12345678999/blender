/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include <cmath>

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_math_base.h"
#include "BLI_struct_equality_utils.hh"

namespace blender {

/**
 * Contains an integer frame number and a subframe float in the range [0, 1).
 */
struct SubFrame {
 private:
  int frame_;
  float subframe_;

 public:
  SubFrame(const int frame = 0, float subframe = 0.0f) : frame_(frame), subframe_(subframe)
  {
    BLI_assert(subframe >= 0.0f);
    BLI_assert(subframe < 1.0f);
  }

  SubFrame(const float frame) : SubFrame(int(floorf(frame)), fractf(frame)) {}

  int frame() const
  {
    return frame_;
  }

  float subframe() const
  {
    return subframe_;
  }

  explicit operator float() const
  {
    return float(frame_) + float(subframe_);
  }

  explicit operator double() const
  {
    return double(frame_) + double(subframe_);
  }

  static SubFrame min()
  {
    return {INT32_MIN, 0.0f};
  }

  static SubFrame max()
  {
    return {INT32_MAX, std::nexttowardf(1.0f, 0.0)};
  }

  uint64_t hash() const
  {
    return get_default_hash(frame_, subframe_);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_2(SubFrame, frame_, subframe_)

  friend bool operator<(const SubFrame &a, const SubFrame &b)
  {
    return a.frame_ < b.frame_ || (a.frame_ == b.frame_ && a.subframe_ < b.subframe_);
  }

  friend bool operator<=(const SubFrame &a, const SubFrame &b)
  {
    return a.frame_ < b.frame_ || (a.frame_ == b.frame_ && a.subframe_ <= b.subframe_);
  }

  friend bool operator>(const SubFrame &a, const SubFrame &b)
  {
    return a.frame_ > b.frame_ || (a.frame_ == b.frame_ && a.subframe_ > b.subframe_);
  }

  friend bool operator>=(const SubFrame &a, const SubFrame &b)
  {
    return a.frame_ > b.frame_ || (a.frame_ == b.frame_ && a.subframe_ >= b.subframe_);
  }
};

}  // namespace blender
