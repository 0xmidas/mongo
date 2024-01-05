/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     envoy/type/v3/percent.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#ifndef ENVOY_TYPE_V3_PERCENT_PROTO_UPB_H_
#define ENVOY_TYPE_V3_PERCENT_PROTO_UPB_H_

#include "upb/generated_code_support.h"
// Must be last. 
#include "upb/port/def.inc"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct envoy_type_v3_Percent envoy_type_v3_Percent;
typedef struct envoy_type_v3_FractionalPercent envoy_type_v3_FractionalPercent;
extern const upb_MiniTable envoy_type_v3_Percent_msg_init;
extern const upb_MiniTable envoy_type_v3_FractionalPercent_msg_init;

typedef enum {
  envoy_type_v3_FractionalPercent_HUNDRED = 0,
  envoy_type_v3_FractionalPercent_TEN_THOUSAND = 1,
  envoy_type_v3_FractionalPercent_MILLION = 2
} envoy_type_v3_FractionalPercent_DenominatorType;



/* envoy.type.v3.Percent */

UPB_INLINE envoy_type_v3_Percent* envoy_type_v3_Percent_new(upb_Arena* arena) {
  return (envoy_type_v3_Percent*)_upb_Message_New(&envoy_type_v3_Percent_msg_init, arena);
}
UPB_INLINE envoy_type_v3_Percent* envoy_type_v3_Percent_parse(const char* buf, size_t size, upb_Arena* arena) {
  envoy_type_v3_Percent* ret = envoy_type_v3_Percent_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &envoy_type_v3_Percent_msg_init, NULL, 0, arena) != kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE envoy_type_v3_Percent* envoy_type_v3_Percent_parse_ex(const char* buf, size_t size,
                           const upb_ExtensionRegistry* extreg,
                           int options, upb_Arena* arena) {
  envoy_type_v3_Percent* ret = envoy_type_v3_Percent_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &envoy_type_v3_Percent_msg_init, extreg, options, arena) !=
      kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE char* envoy_type_v3_Percent_serialize(const envoy_type_v3_Percent* msg, upb_Arena* arena, size_t* len) {
  char* ptr;
  (void)upb_Encode(msg, &envoy_type_v3_Percent_msg_init, 0, arena, &ptr, len);
  return ptr;
}
UPB_INLINE char* envoy_type_v3_Percent_serialize_ex(const envoy_type_v3_Percent* msg, int options,
                                 upb_Arena* arena, size_t* len) {
  char* ptr;
  (void)upb_Encode(msg, &envoy_type_v3_Percent_msg_init, options, arena, &ptr, len);
  return ptr;
}
UPB_INLINE void envoy_type_v3_Percent_clear_value(envoy_type_v3_Percent* msg) {
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 1, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_8Byte << kUpb_FieldRep_Shift)};
  _upb_Message_ClearNonExtensionField(msg, &field);
}
UPB_INLINE double envoy_type_v3_Percent_value(const envoy_type_v3_Percent* msg) {
  double default_val = 0;
  double ret;
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 1, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_8Byte << kUpb_FieldRep_Shift)};
  _upb_Message_GetNonExtensionField(msg, &field, &default_val, &ret);
  return ret;
}

UPB_INLINE void envoy_type_v3_Percent_set_value(envoy_type_v3_Percent *msg, double value) {
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 1, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_8Byte << kUpb_FieldRep_Shift)};
  _upb_Message_SetNonExtensionField(msg, &field, &value);
}

/* envoy.type.v3.FractionalPercent */

UPB_INLINE envoy_type_v3_FractionalPercent* envoy_type_v3_FractionalPercent_new(upb_Arena* arena) {
  return (envoy_type_v3_FractionalPercent*)_upb_Message_New(&envoy_type_v3_FractionalPercent_msg_init, arena);
}
UPB_INLINE envoy_type_v3_FractionalPercent* envoy_type_v3_FractionalPercent_parse(const char* buf, size_t size, upb_Arena* arena) {
  envoy_type_v3_FractionalPercent* ret = envoy_type_v3_FractionalPercent_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &envoy_type_v3_FractionalPercent_msg_init, NULL, 0, arena) != kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE envoy_type_v3_FractionalPercent* envoy_type_v3_FractionalPercent_parse_ex(const char* buf, size_t size,
                           const upb_ExtensionRegistry* extreg,
                           int options, upb_Arena* arena) {
  envoy_type_v3_FractionalPercent* ret = envoy_type_v3_FractionalPercent_new(arena);
  if (!ret) return NULL;
  if (upb_Decode(buf, size, ret, &envoy_type_v3_FractionalPercent_msg_init, extreg, options, arena) !=
      kUpb_DecodeStatus_Ok) {
    return NULL;
  }
  return ret;
}
UPB_INLINE char* envoy_type_v3_FractionalPercent_serialize(const envoy_type_v3_FractionalPercent* msg, upb_Arena* arena, size_t* len) {
  char* ptr;
  (void)upb_Encode(msg, &envoy_type_v3_FractionalPercent_msg_init, 0, arena, &ptr, len);
  return ptr;
}
UPB_INLINE char* envoy_type_v3_FractionalPercent_serialize_ex(const envoy_type_v3_FractionalPercent* msg, int options,
                                 upb_Arena* arena, size_t* len) {
  char* ptr;
  (void)upb_Encode(msg, &envoy_type_v3_FractionalPercent_msg_init, options, arena, &ptr, len);
  return ptr;
}
UPB_INLINE void envoy_type_v3_FractionalPercent_clear_numerator(envoy_type_v3_FractionalPercent* msg) {
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 13, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_ClearNonExtensionField(msg, &field);
}
UPB_INLINE uint32_t envoy_type_v3_FractionalPercent_numerator(const envoy_type_v3_FractionalPercent* msg) {
  uint32_t default_val = (uint32_t)0u;
  uint32_t ret;
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 13, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_GetNonExtensionField(msg, &field, &default_val, &ret);
  return ret;
}
UPB_INLINE void envoy_type_v3_FractionalPercent_clear_denominator(envoy_type_v3_FractionalPercent* msg) {
  const upb_MiniTableField field = {2, 4, 0, kUpb_NoSub, 5, (int)kUpb_FieldMode_Scalar | (int)kUpb_LabelFlags_IsAlternate | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_ClearNonExtensionField(msg, &field);
}
UPB_INLINE int32_t envoy_type_v3_FractionalPercent_denominator(const envoy_type_v3_FractionalPercent* msg) {
  int32_t default_val = 0;
  int32_t ret;
  const upb_MiniTableField field = {2, 4, 0, kUpb_NoSub, 5, (int)kUpb_FieldMode_Scalar | (int)kUpb_LabelFlags_IsAlternate | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_GetNonExtensionField(msg, &field, &default_val, &ret);
  return ret;
}

UPB_INLINE void envoy_type_v3_FractionalPercent_set_numerator(envoy_type_v3_FractionalPercent *msg, uint32_t value) {
  const upb_MiniTableField field = {1, 0, 0, kUpb_NoSub, 13, (int)kUpb_FieldMode_Scalar | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_SetNonExtensionField(msg, &field, &value);
}
UPB_INLINE void envoy_type_v3_FractionalPercent_set_denominator(envoy_type_v3_FractionalPercent *msg, int32_t value) {
  const upb_MiniTableField field = {2, 4, 0, kUpb_NoSub, 5, (int)kUpb_FieldMode_Scalar | (int)kUpb_LabelFlags_IsAlternate | ((int)kUpb_FieldRep_4Byte << kUpb_FieldRep_Shift)};
  _upb_Message_SetNonExtensionField(msg, &field, &value);
}

extern const upb_MiniTableFile envoy_type_v3_percent_proto_upb_file_layout;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#include "upb/port/undef.inc"

#endif  /* ENVOY_TYPE_V3_PERCENT_PROTO_UPB_H_ */