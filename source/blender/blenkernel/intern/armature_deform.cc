/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Deform coordinates by a armature object (used by modifier).
 */

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_task.h"

#include "DNA_armature_types.h"
#include "DNA_lattice_types.h"
#include "DNA_listBase.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_editmesh.hh"
#include "BKE_lattice.hh"
#include "BKE_mesh.hh"

#include "CLG_log.h"

static CLG_LogRef LOG = {"bke.armature_deform"};

/* -------------------------------------------------------------------- */
/** \name Armature Deform Internal Utilities
 * \{ */

static float bone_envelope_falloff(const float distance_squared,
                                   const float closest_radius,
                                   const float falloff_distance)
{
  using namespace blender;

  if (distance_squared < closest_radius * closest_radius) {
    return 1.0f;
  }

  /* Zero influence beyond falloff distance. */
  if (falloff_distance == 0.0f ||
      distance_squared >= math::square(closest_radius + falloff_distance))
  {
    return 0.0f;
  }

  /* Compute influence from envelope over the falloff distance. */
  const float dist_envelope = sqrtf(distance_squared) - closest_radius;
  return 1.0f - (dist_envelope * dist_envelope) / (falloff_distance * falloff_distance);
}

float distfactor_to_bone(const blender::float3 &position,
                         const blender::float3 &head,
                         const blender::float3 &tail,
                         const float radius_head,
                         const float radius_tail,
                         const float falloff_distance)
{
  using namespace blender;

  float bone_length;
  const float3 bone_axis = math::normalize_and_get_length(tail - head, bone_length);
  /* Distance along the bone axis from head. */
  const float height = math::dot(position - head, bone_axis);

  if (height < 0.0f) {
    /* Below the start of the bone use the head radius. */
    const float distance_squared = math::distance_squared(position, head);
    return bone_envelope_falloff(distance_squared, radius_head, falloff_distance);
  }
  else if (height > bone_length) {
    /* After the end of the bone use the tail radius. */
    const float distance_squared = math::distance_squared(tail, position);
    return bone_envelope_falloff(distance_squared, radius_tail, falloff_distance);
  }
  else {
    /* Interpolate radius. */
    const float distance_squared = math::distance_squared(position, head) - height * height;
    const float closest_radius = bone_length != 0.0f ? math::interpolate(radius_head,
                                                                         radius_tail,
                                                                         height / bone_length) :
                                                       radius_head;
    return bone_envelope_falloff(distance_squared, closest_radius, falloff_distance);
  }
}

/* Add the effect of one bone or B-Bone segment to the accumulated result. */
static void pchan_deform_accumulate(const DualQuat *deform_dq,
                                    const float deform_mat[4][4],
                                    const float co_in[3],
                                    const float weight,
                                    float co_accum[3],
                                    DualQuat *dq_accum,
                                    float mat_accum[3][3],
                                    const bool full_deform)
{
  if (weight == 0.0f) {
    return;
  }

  if (dq_accum) {
    BLI_assert(!co_accum);

    add_weighted_dq_dq_pivot(dq_accum, deform_dq, co_in, weight, full_deform);
  }
  else {
    float tmp[3];
    mul_v3_m4v3(tmp, deform_mat, co_in);

    sub_v3_v3(tmp, co_in);
    madd_v3_v3fl(co_accum, tmp, weight);

    if (full_deform) {
      float tmpmat[3][3];
      copy_m3_m4(tmpmat, deform_mat);

      madd_m3_m3m3fl(mat_accum, mat_accum, tmpmat, weight);
    }
  }
}

static void b_bone_deform(const bPoseChannel *pchan,
                          const float co[3],
                          const float weight,
                          float vec[3],
                          DualQuat *dq,
                          float defmat[3][3],
                          const bool full_deform)
{
  const DualQuat *quats = pchan->runtime.bbone_dual_quats;
  const Mat4 *mats = pchan->runtime.bbone_deform_mats;
  float blend;
  int index;

  /* Calculate the indices of the 2 affecting b_bone segments. */
  BKE_pchan_bbone_deform_segment_index(pchan, co, &index, &blend);

  pchan_deform_accumulate(&quats[index],
                          mats[index + 1].mat,
                          co,
                          weight * (1.0f - blend),
                          vec,
                          dq,
                          defmat,
                          full_deform);
  pchan_deform_accumulate(
      &quats[index + 1], mats[index + 2].mat, co, weight * blend, vec, dq, defmat, full_deform);
}

static float dist_bone_deform(const bPoseChannel *pchan,
                              float vec[3],
                              DualQuat *dq,
                              float mat[3][3],
                              const float co[3],
                              const bool full_deform)
{
  const Bone *bone = pchan->bone;
  float fac, contrib = 0.0;

  if (bone == nullptr) {
    return 0.0f;
  }

  fac = distfactor_to_bone(
      co, bone->arm_head, bone->arm_tail, bone->rad_head, bone->rad_tail, bone->dist);

  if (fac > 0.0f) {
    fac *= bone->weight;
    contrib = fac;
    if (contrib > 0.0f) {
      if (bone->segments > 1 && pchan->runtime.bbone_segments == bone->segments) {
        b_bone_deform(pchan, co, fac, vec, dq, mat, full_deform);
      }
      else {
        pchan_deform_accumulate(
            &pchan->runtime.deform_dual_quat, pchan->chan_mat, co, fac, vec, dq, mat, full_deform);
      }
    }
  }

  return contrib;
}

static void pchan_bone_deform(const bPoseChannel *pchan,
                              const float weight,
                              float vec[3],
                              DualQuat *dq,
                              float mat[3][3],
                              const float co[3],
                              const bool full_deform,
                              float *contrib)
{
  const Bone *bone = pchan->bone;

  if (!weight) {
    return;
  }

  if (bone->segments > 1 && pchan->runtime.bbone_segments == bone->segments) {
    b_bone_deform(pchan, co, weight, vec, dq, mat, full_deform);
  }
  else {
    pchan_deform_accumulate(
        &pchan->runtime.deform_dual_quat, pchan->chan_mat, co, weight, vec, dq, mat, full_deform);
  }

  (*contrib) += weight;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Deform #BKE_armature_deform_coords API
 *
 * #BKE_armature_deform_coords and related functions.
 * \{ */

struct ArmatureUserdata {
  const Object *ob_arm;
  const Mesh *me_target;
  float (*vert_coords)[3];
  float (*vert_deform_mats)[3][3];
  float (*vert_coords_prev)[3];

  bool use_envelope;
  bool use_quaternion;
  bool invert_vgroup;
  bool use_dverts;

  int armature_def_nr;

  const MDeformVert *dverts;
  int dverts_len;

  bPoseChannel **pchan_from_defbase;
  int defbase_len;

  float premat[4][4];
  float postmat[4][4];

  /** Specific data types. */
  struct {
    int cd_dvert_offset;
  } bmesh;
};

static void armature_vert_task_with_dvert(const ArmatureUserdata *data,
                                          const int i,
                                          const MDeformVert *dvert)
{
  float(*const vert_coords)[3] = data->vert_coords;
  float(*const vert_deform_mats)[3][3] = data->vert_deform_mats;
  float(*const vert_coords_prev)[3] = data->vert_coords_prev;
  const bool use_envelope = data->use_envelope;
  const bool use_quaternion = data->use_quaternion;
  const bool use_dverts = data->use_dverts;
  const int armature_def_nr = data->armature_def_nr;

  DualQuat sumdq = {}, *dq = nullptr;
  const bPoseChannel *pchan;
  float *co, dco[3];
  float sumvec[3], summat[3][3];
  float *vec = nullptr, (*smat)[3] = nullptr;
  float contrib = 0.0f;
  float armature_weight = 1.0f; /* default to 1 if no overall def group */
  float prevco_weight = 0.0f;   /* weight for optional cached vertexcos */

  const bool full_deform = vert_deform_mats != nullptr;

  if (use_quaternion) {
    dq = &sumdq;
  }
  else {
    zero_v3(sumvec);
    vec = sumvec;

    if (full_deform) {
      zero_m3(summat);
      smat = summat;
    }
  }

  if (armature_def_nr != -1 && dvert) {
    armature_weight = BKE_defvert_find_weight(dvert, armature_def_nr);

    if (data->invert_vgroup) {
      armature_weight = 1.0f - armature_weight;
    }

    /* hackish: the blending factor can be used for blending with vert_coords_prev too */
    if (vert_coords_prev) {
      /* This weight specifies the contribution from the coordinates at the start of this
       * modifier evaluation, while armature_weight is normally the opposite of that. */
      prevco_weight = 1.0f - armature_weight;
      armature_weight = 1.0f;
    }
  }

  /* check if there's any  point in calculating for this vert */
  if (vert_coords_prev) {
    if (prevco_weight == 1.0f) {
      return;
    }

    /* get the coord we work on */
    co = vert_coords_prev[i];
  }
  else {
    if (armature_weight == 0.0f) {
      return;
    }

    /* get the coord we work on */
    co = vert_coords[i];
  }

  /* Apply the object's matrix */
  mul_m4_v3(data->premat, co);

  if (use_dverts && dvert && dvert->totweight) { /* use weight groups ? */
    const MDeformWeight *dw = dvert->dw;
    int deformed = 0;
    uint j;
    for (j = dvert->totweight; j != 0; j--, dw++) {
      const uint index = dw->def_nr;
      if (index >= data->defbase_len) {
        continue;
      }
      pchan = data->pchan_from_defbase[index];
      if (pchan == nullptr) {
        continue;
      }

      float weight = dw->weight;
      const Bone *bone = pchan->bone;

      deformed = 1;

      if (bone && bone->flag & BONE_MULT_VG_ENV) {
        weight *= distfactor_to_bone(
            co, bone->arm_head, bone->arm_tail, bone->rad_head, bone->rad_tail, bone->dist);
      }

      pchan_bone_deform(pchan, weight, vec, dq, smat, co, full_deform, &contrib);
    }
    /* If there are vertex-groups but not groups with bones (like for soft-body groups). */
    if (deformed == 0 && use_envelope) {
      for (pchan = static_cast<const bPoseChannel *>(data->ob_arm->pose->chanbase.first); pchan;
           pchan = pchan->next)
      {
        if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
          contrib += dist_bone_deform(pchan, vec, dq, smat, co, full_deform);
        }
      }
    }
  }
  else if (use_envelope) {
    for (pchan = static_cast<const bPoseChannel *>(data->ob_arm->pose->chanbase.first); pchan;
         pchan = pchan->next)
    {
      if (!(pchan->bone->flag & BONE_NO_DEFORM)) {
        contrib += dist_bone_deform(pchan, vec, dq, smat, co, full_deform);
      }
    }
  }

  /* actually should be EPSILON? weight values and contrib can be like 10e-39 small */
  if (contrib > 0.0001f) {
    if (use_quaternion) {
      normalize_dq(dq, contrib);

      if (armature_weight != 1.0f) {
        copy_v3_v3(dco, co);
        mul_v3m3_dq(dco, full_deform ? summat : nullptr, dq);
        sub_v3_v3(dco, co);
        mul_v3_fl(dco, armature_weight);
        add_v3_v3(co, dco);
      }
      else {
        mul_v3m3_dq(co, full_deform ? summat : nullptr, dq);
      }

      smat = summat;
    }
    else {
      mul_v3_fl(vec, armature_weight / contrib);
      add_v3_v3v3(co, vec, co);
    }

    if (full_deform) {
      float pre[3][3], post[3][3], tmpmat[3][3];

      copy_m3_m4(pre, data->premat);
      copy_m3_m4(post, data->postmat);
      copy_m3_m3(tmpmat, vert_deform_mats[i]);

      if (!use_quaternion) { /* quaternion already is scale corrected */
        mul_m3_fl(smat, armature_weight / contrib);
      }

      mul_m3_series(vert_deform_mats[i], post, smat, pre, tmpmat);
    }
  }

  /* always, check above code */
  mul_m4_v3(data->postmat, co);

  /* interpolate with previous modifier position using weight group */
  if (vert_coords_prev) {
    float mw = 1.0f - prevco_weight;
    vert_coords[i][0] = prevco_weight * vert_coords[i][0] + mw * co[0];
    vert_coords[i][1] = prevco_weight * vert_coords[i][1] + mw * co[1];
    vert_coords[i][2] = prevco_weight * vert_coords[i][2] + mw * co[2];
  }
}

static void armature_vert_task(void *__restrict userdata,
                               const int i,
                               const TaskParallelTLS *__restrict /*tls*/)
{
  const ArmatureUserdata *data = static_cast<const ArmatureUserdata *>(userdata);
  const MDeformVert *dvert;
  if (data->use_dverts || data->armature_def_nr != -1) {
    if (data->me_target) {
      BLI_assert(i < data->me_target->verts_num);
      if (data->dverts != nullptr) {
        dvert = data->dverts + i;
      }
      else {
        dvert = nullptr;
      }
    }
    else if (data->dverts && i < data->dverts_len) {
      dvert = data->dverts + i;
    }
    else {
      dvert = nullptr;
    }
  }
  else {
    dvert = nullptr;
  }

  armature_vert_task_with_dvert(data, i, dvert);
}

static void armature_vert_task_editmesh(void *__restrict userdata,
                                        MempoolIterData *iter,
                                        const TaskParallelTLS *__restrict /*tls*/)
{
  const ArmatureUserdata *data = static_cast<const ArmatureUserdata *>(userdata);
  BMVert *v = (BMVert *)iter;
  const MDeformVert *dvert = static_cast<const MDeformVert *>(
      BM_ELEM_CD_GET_VOID_P(v, data->bmesh.cd_dvert_offset));
  armature_vert_task_with_dvert(data, BM_elem_index_get(v), dvert);
}

static void armature_vert_task_editmesh_no_dvert(void *__restrict userdata,
                                                 MempoolIterData *iter,
                                                 const TaskParallelTLS *__restrict /*tls*/)
{
  const ArmatureUserdata *data = static_cast<const ArmatureUserdata *>(userdata);
  BMVert *v = (BMVert *)iter;
  armature_vert_task_with_dvert(data, BM_elem_index_get(v), nullptr);
}

static void armature_deform_coords_impl(const Object *ob_arm,
                                        const Object *ob_target,
                                        const ListBase *defbase,
                                        float (*vert_coords)[3],
                                        float (*vert_deform_mats)[3][3],
                                        const int vert_coords_len,
                                        const int deformflag,
                                        float (*vert_coords_prev)[3],
                                        const char *defgrp_name,
                                        blender::Span<MDeformVert> dverts,
                                        const Mesh *me_target,
                                        const BMEditMesh *em_target)
{
  const bArmature *arm = static_cast<const bArmature *>(ob_arm->data);
  bPoseChannel **pchan_from_defbase = nullptr;
  const bool use_envelope = (deformflag & ARM_DEF_ENVELOPE) != 0;
  const bool use_quaternion = (deformflag & ARM_DEF_QUATERNION) != 0;
  const bool invert_vgroup = (deformflag & ARM_DEF_INVERT_VGROUP) != 0;
  int defbase_len = 0; /* Safety for vertex-group index overflow. */
  bool use_dverts = false;
  int armature_def_nr = -1;
  int cd_dvert_offset = -1;

  /* in editmode, or not an armature */
  if (arm->edbo || (ob_arm->pose == nullptr)) {
    return;
  }

  if ((ob_arm->pose->flag & POSE_RECALC) != 0) {
    CLOG_ERROR(&LOG,
               "Trying to evaluate influence of armature '%s' which needs Pose recalc!",
               ob_arm->id.name);
    BLI_assert(0);
  }

  if (BKE_object_supports_vertex_groups(ob_target)) {
    /* Collect the vertex group names from the evaluated data. */
    armature_def_nr = BKE_defgroup_name_index(defbase, defgrp_name);
    defbase_len = BLI_listbase_count(defbase);

    /* Get a vertex-deform-index to pose-channel array. */
    if (deformflag & ARM_DEF_VGROUP) {
      /* if we have a Mesh, only use dverts if it has them */
      if (em_target) {
        cd_dvert_offset = CustomData_get_offset(&em_target->bm->vdata, CD_MDEFORMVERT);
        use_dverts = (cd_dvert_offset != -1);
      }
      else if (me_target) {
        use_dverts = !me_target->deform_verts().is_empty();
      }
      else if (dverts.size() == vert_coords_len) {
        use_dverts = true;
      }

      if (use_dverts) {
        pchan_from_defbase = MEM_calloc_arrayN<bPoseChannel *>(defbase_len, "defnrToBone");
        /* TODO(sergey): Some considerations here:
         *
         * - Check whether keeping this consistent across frames gives speedup.
         */
        int i;
        LISTBASE_FOREACH_INDEX (bDeformGroup *, dg, defbase, i) {
          pchan_from_defbase[i] = BKE_pose_channel_find_name(ob_arm->pose, dg->name);
          /* exclude non-deforming bones */
          if (pchan_from_defbase[i]) {
            if (pchan_from_defbase[i]->bone->flag & BONE_NO_DEFORM) {
              pchan_from_defbase[i] = nullptr;
            }
          }
        }
      }
    }
  }

  ArmatureUserdata data{};
  data.ob_arm = ob_arm;
  data.me_target = me_target;
  data.vert_coords = vert_coords;
  data.vert_deform_mats = vert_deform_mats;
  data.vert_coords_prev = vert_coords_prev;
  data.use_envelope = use_envelope;
  data.use_quaternion = use_quaternion;
  data.invert_vgroup = invert_vgroup;
  data.use_dverts = use_dverts;
  data.armature_def_nr = armature_def_nr;
  data.dverts = dverts.data();
  data.dverts_len = dverts.size();
  data.pchan_from_defbase = pchan_from_defbase;
  data.defbase_len = defbase_len;
  data.bmesh.cd_dvert_offset = cd_dvert_offset;

  float obinv[4][4];
  invert_m4_m4(obinv, ob_target->object_to_world().ptr());

  mul_m4_m4m4(data.postmat, obinv, ob_arm->object_to_world().ptr());
  invert_m4_m4(data.premat, data.postmat);

  if (em_target != nullptr) {
    /* While this could cause an extra loop over mesh data, in most cases this will
     * have already been properly set. */
    BM_mesh_elem_index_ensure(em_target->bm, BM_VERT);

    TaskParallelSettings settings;
    BLI_parallel_mempool_settings_defaults(&settings);

    if (use_dverts) {
      BLI_task_parallel_mempool(
          em_target->bm->vpool, &data, armature_vert_task_editmesh, &settings);
    }
    else {
      BLI_task_parallel_mempool(
          em_target->bm->vpool, &data, armature_vert_task_editmesh_no_dvert, &settings);
    }
  }
  else {
    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    settings.min_iter_per_thread = 32;
    BLI_task_parallel_range(0, vert_coords_len, &data, armature_vert_task, &settings);
  }

  if (pchan_from_defbase) {
    MEM_freeN(pchan_from_defbase);
  }
}

void BKE_armature_deform_coords_with_curves(
    const Object &ob_arm,
    const Object &ob_target,
    const ListBase *defbase,
    blender::MutableSpan<blender::float3> vert_coords,
    std::optional<blender::Span<blender::float3>> vert_coords_prev,
    std::optional<blender::MutableSpan<blender::float3x3>> vert_deform_mats,
    blender::Span<MDeformVert> dverts,
    int deformflag,
    blender::StringRefNull defgrp_name)
{
  /* Vertex groups must be provided explicitly, cannot rely on object vertex groups since this is
   * used for Grease Pencil layers as well. */
  BLI_assert(dverts.size() == vert_coords.size());

  blender::float3 *vert_coords_prev_data = nullptr;
  if (vert_coords_prev.has_value()) {
    /* const_cast for old positions for the C API, these are not actually written. */
    vert_coords_prev_data = const_cast<blender::float3 *>(vert_coords_prev->data());
  }

  armature_deform_coords_impl(
      &ob_arm,
      &ob_target,
      defbase,
      reinterpret_cast<float(*)[3]>(vert_coords.data()),
      vert_deform_mats ? reinterpret_cast<float(*)[3][3]>(vert_deform_mats->data()) : nullptr,
      vert_coords.size(),
      deformflag,
      reinterpret_cast<float(*)[3]>(vert_coords_prev_data),
      defgrp_name.c_str(),
      dverts,
      nullptr,
      nullptr);
}

void BKE_armature_deform_coords_with_mesh(const Object *ob_arm,
                                          const Object *ob_target,
                                          float (*vert_coords)[3],
                                          float (*vert_deform_mats)[3][3],
                                          int vert_coords_len,
                                          int deformflag,
                                          float (*vert_coords_prev)[3],
                                          const char *defgrp_name,
                                          const Mesh *me_target)
{
  /* Note armature modifier on legacy curves calls this, so vertex groups are not guaranteed to
   * exist. */
  const ID *id_target = static_cast<const ID *>(ob_target->data);
  const ListBase *defbase = nullptr;
  if (me_target) {
    /* Use the vertex groups from the evaluated mesh that is being deformed. */
    defbase = BKE_id_defgroup_list_get(&me_target->id);
  }
  else if (BKE_id_supports_vertex_groups(id_target)) {
    /* Take the vertex groups from the original object data. */
    defbase = BKE_id_defgroup_list_get(id_target);
  }

  blender::Span<MDeformVert> dverts;
  if (ob_target->type == OB_MESH) {
    if (me_target == nullptr) {
      me_target = static_cast<const Mesh *>(ob_target->data);
    }
    dverts = me_target->deform_verts();
  }
  else if (ob_target->type == OB_LATTICE) {
    const Lattice *lt = static_cast<const Lattice *>(ob_target->data);
    dverts = blender::Span<MDeformVert>(lt->dvert, lt->pntsu * lt->pntsv * lt->pntsw);
  }

  armature_deform_coords_impl(ob_arm,
                              ob_target,
                              defbase,
                              vert_coords,
                              vert_deform_mats,
                              vert_coords_len,
                              deformflag,
                              vert_coords_prev,
                              defgrp_name,
                              dverts,
                              me_target,
                              nullptr);
}

void BKE_armature_deform_coords_with_editmesh(const Object *ob_arm,
                                              const Object *ob_target,
                                              float (*vert_coords)[3],
                                              float (*vert_deform_mats)[3][3],
                                              int vert_coords_len,
                                              int deformflag,
                                              float (*vert_coords_prev)[3],
                                              const char *defgrp_name,
                                              const BMEditMesh *em_target)
{
  const ListBase *defbase = BKE_id_defgroup_list_get(static_cast<const ID *>(ob_target->data));
  armature_deform_coords_impl(ob_arm,
                              ob_target,
                              defbase,
                              vert_coords,
                              vert_deform_mats,
                              vert_coords_len,
                              deformflag,
                              vert_coords_prev,
                              defgrp_name,
                              {},
                              nullptr,
                              em_target);
}

/** \} */
