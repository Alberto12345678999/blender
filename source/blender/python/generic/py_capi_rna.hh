/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * \file
 * \ingroup pygen
 */

#pragma once

#include <Python.h>

struct EnumPropertyItem;

/**
 * Convert all items into a single comma separated string.
 * Use for creating useful error messages.
 */
[[nodiscard]] char *pyrna_enum_repr(const EnumPropertyItem *item);

/**
 * Same as #RNA_enum_value_from_id, but raises an exception.
 */
[[nodiscard]] int pyrna_enum_value_from_id(const EnumPropertyItem *item,
                                           const char *identifier,
                                           int *r_value,
                                           const char *error_prefix);

/**
 * Takes a set of strings and map it to and array of booleans.
 *
 * Useful when the values aren't flags.
 *
 * \param type_convert_sign: Maps signed to unsigned range,
 * needed when we want to use the full range of a signed short/char.
 */
[[nodiscard]] unsigned int *pyrna_enum_bitmap_from_set(const EnumPropertyItem *items,
                                                       PyObject *value,
                                                       int type_size,
                                                       bool type_convert_sign,
                                                       int bitmap_size,
                                                       const char *error_prefix);

/**
 * 'value' _must_ be a set type, error check before calling.
 */
[[nodiscard]] int pyrna_enum_bitfield_from_set(const EnumPropertyItem *items,
                                               PyObject *value,
                                               int *r_value,
                                               const char *error_prefix);

[[nodiscard]] PyObject *pyrna_enum_bitfield_as_set(const EnumPropertyItem *items, int value);

/**
 * Data for #pyrna_enum_value_parse_string & #pyrna_enum_bitfield_parse_set parsing utilities.
 * Use with #PyArg_ParseTuple's `O&` formatting.
 */
struct BPy_EnumProperty_Parse {
  const EnumPropertyItem *items;
  /**
   * Set when the value was successfully parsed.
   * Useful if the input ever needs to be included in an error message.
   * (if the value is not supported under certain conditions).
   */
  PyObject *value_orig;

  int value;
  bool is_set;
};
/**
 * Use with #PyArg_ParseTuple's `O&` formatting.
 */
[[nodiscard]] int pyrna_enum_value_parse_string(PyObject *o, void *p);
/**
 * Use with #PyArg_ParseTuple's `O&` formatting.
 */
[[nodiscard]] int pyrna_enum_bitfield_parse_set(PyObject *o, void *p);
