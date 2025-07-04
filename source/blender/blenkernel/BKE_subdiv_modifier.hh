/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BKE_mesh_types.hh"
#include "BKE_subdiv.hh"

#include "DNA_mesh_types.h"

/* Hardcoded for until GPU shaders are automatically generated, then we will have a more
 * programmatic way of detecting this. */
#define MAX_GPU_SUBDIV_SSBOS 12

struct Mesh;
struct Object;
struct Scene;
struct SubsurfModifierData;

namespace blender::bke::subdiv {
struct Subdiv;
struct Settings;
}  // namespace blender::bke::subdiv

/* Runtime subsurf modifier data, cached in modifier on evaluated meshes. */
struct SubsurfRuntimeData {
  /* Subdivision settings, exists before descriptor or mesh wrapper is created. */
  blender::bke::subdiv::Settings settings;

  /* Cached subdivision surface descriptor, with topology and settings. */
  blender::bke::subdiv::Subdiv *subdiv_cpu;
  blender::bke::subdiv::Subdiv *subdiv_gpu;

  /* Recent usage markers for UI diagnostics. To avoid UI flicker due to races
   * between evaluation and UI redraw, they are set to 2 when an evaluator is used,
   * and count down every frame. */
  char used_cpu, used_gpu;

  /* Cached mesh wrapper data, to be used for GPU subdiv or lazy evaluation on CPU. */
  bool has_gpu_subdiv;
  int resolution;
  bool use_optimal_display;
  bool use_loop_normals;

  /* Cached from the draw code for stats display. */
  int stats_totvert;
  int stats_totedge;
  int stats_faces_num;
  int stats_totloop;
};

blender::bke::subdiv::Settings BKE_subsurf_modifier_settings_init(const SubsurfModifierData *smd,
                                                                  bool use_render_params);

bool BKE_subsurf_modifier_runtime_init(SubsurfModifierData *smd, bool use_render_params);

bool BKE_subsurf_modifier_use_custom_loop_normals(const SubsurfModifierData *smd,
                                                  const Mesh *mesh);
bool BKE_subsurf_modifier_has_split_normals(const SubsurfModifierData *smd, const Mesh *mesh);

/**
 * Return true if GPU subdivision evaluation is disabled by force due to incompatible mesh or
 * modifier settings. This will only return true if GPU subdivision is enabled in the preferences
 * and supported by the GPU. It is mainly useful for showing UI messages.
 */
bool BKE_subsurf_modifier_force_disable_gpu_evaluation_for_mesh(const SubsurfModifierData *smd,
                                                                const Mesh *mesh);
/**
 * Return true if GPU subdivision can be used for this modifier. It does not check if
 * the modifier is in the right place in the modifier stack, only if the settings and
 * GPU are compatible.
 */
bool BKE_subsurf_modifier_can_do_gpu_subdiv(const SubsurfModifierData *smd, const Mesh *mesh);

inline bool BKE_subsurf_modifier_has_gpu_subdiv(const Mesh *mesh)
{
  SubsurfRuntimeData *runtime_data = mesh->runtime->subsurf_runtime_data;
  return runtime_data && runtime_data->has_gpu_subdiv;
}

extern void (*BKE_subsurf_modifier_free_gpu_cache_cb)(blender::bke::subdiv::Subdiv *subdiv);

/**
 * Main goal of this function is to give usable subdivision surface descriptor
 * which matches settings and topology.
 */
blender::bke::subdiv::Subdiv *BKE_subsurf_modifier_subdiv_descriptor_ensure(
    SubsurfRuntimeData *runtime_data, const Mesh *mesh, bool for_draw_code);

/**
 * Return the #ModifierMode required for the evaluation of the subsurf modifier,
 * which should be used to check if the modifier is enabled.
 */
int BKE_subsurf_modifier_eval_required_mode(bool is_final_render, bool is_edit_mode);
