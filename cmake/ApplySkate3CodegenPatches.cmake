if(NOT DEFINED SKATE3_SOURCE_DIR)
  message(FATAL_ERROR "SKATE3_SOURCE_DIR is required")
endif()

set(_skate3_init_header "${SKATE3_SOURCE_DIR}/generated/skate3_init.h")
if(NOT EXISTS "${_skate3_init_header}")
  message(FATAL_ERROR "Generated Skate 3 init header not found")
endif()
file(READ "${_skate3_init_header}" _skate3_init_contents)
if(NOT _skate3_init_contents MATCHES "skate3_function_coverage.h")
  set(_coverage_patch
"#include \"skate3_function_coverage.h\"
#include \"skate3_target_trace.h\"
#undef REX_FUNC_PROLOGUE
#if defined(__clang__)
#define REX_FUNC_PROLOGUE() do { \\
  __builtin_assume(((size_t)base & 0x1F) == 0); \\
  skate3::function_coverage::MaybeRecord(__func__); \\
  skate3::target_trace::MaybeRecord(__func__, ctx); \\
} while (0)
#elif defined(__GNUC__)
#define REX_FUNC_PROLOGUE() do { \\
  if (((size_t)base & 0x1F) != 0) __builtin_unreachable(); \\
  skate3::function_coverage::MaybeRecord(__func__); \\
  skate3::target_trace::MaybeRecord(__func__, ctx); \\
} while (0)
#else
#define REX_FUNC_PROLOGUE() do { \\
  skate3::function_coverage::MaybeRecord(__func__); \\
  skate3::target_trace::MaybeRecord(__func__, ctx); \\
} while (0)
#endif

")
  string(REPLACE
    "//=============================================================================\n// Memory Access"
    "${_coverage_patch}//=============================================================================\n// Memory Access"
    _skate3_init_contents
    "${_skate3_init_contents}")
  file(WRITE "${_skate3_init_header}" "${_skate3_init_contents}")
  message(STATUS "Applied Skate 3 generated function-coverage prologue")
endif()

# Upgrade an already coverage-patched generated header with targeted tracing.
if(NOT _skate3_init_contents MATCHES "target_trace::MaybeRecord")
  if(NOT _skate3_init_contents MATCHES "skate3_target_trace.h")
    string(REPLACE
      "#include \"skate3_function_coverage.h\"\n"
      "#include \"skate3_function_coverage.h\"\n#include \"skate3_target_trace.h\"\n"
      _skate3_init_contents
      "${_skate3_init_contents}")
  endif()
  set(_coverage_only_prologue [=[
#undef REX_FUNC_PROLOGUE
#if defined(__clang__)
#define REX_FUNC_PROLOGUE() do { \
  __builtin_assume(((size_t)base & 0x1F) == 0); \
  skate3::function_coverage::MaybeRecord(__func__); \
} while (0)
#elif defined(__GNUC__)
#define REX_FUNC_PROLOGUE() do { \
  if (((size_t)base & 0x1F) != 0) __builtin_unreachable(); \
  skate3::function_coverage::MaybeRecord(__func__); \
} while (0)
#else
#define REX_FUNC_PROLOGUE() \
  skate3::function_coverage::MaybeRecord(__func__)
#endif
]=])
  set(_target_trace_prologue [=[
#undef REX_FUNC_PROLOGUE
#if defined(__clang__)
#define REX_FUNC_PROLOGUE() do { \
  __builtin_assume(((size_t)base & 0x1F) == 0); \
  skate3::function_coverage::MaybeRecord(__func__); \
  skate3::target_trace::MaybeRecord(__func__, ctx); \
} while (0)
#elif defined(__GNUC__)
#define REX_FUNC_PROLOGUE() do { \
  if (((size_t)base & 0x1F) != 0) __builtin_unreachable(); \
  skate3::function_coverage::MaybeRecord(__func__); \
  skate3::target_trace::MaybeRecord(__func__, ctx); \
} while (0)
#else
#define REX_FUNC_PROLOGUE() do { \
  skate3::function_coverage::MaybeRecord(__func__); \
  skate3::target_trace::MaybeRecord(__func__, ctx); \
} while (0)
#endif
]=])
  string(FIND "${_skate3_init_contents}" "${_coverage_only_prologue}"
    _coverage_only_prologue_anchor)
  if(_coverage_only_prologue_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to upgrade generated function prologue with targeted tracing")
  endif()
  string(REPLACE
    "${_coverage_only_prologue}"
    "${_target_trace_prologue}"
    _skate3_init_contents
    "${_skate3_init_contents}")
  file(WRITE "${_skate3_init_header}" "${_skate3_init_contents}")
  message(STATUS "Applied Skate 3 targeted function tracing prologue")
endif()

if(NOT _skate3_init_contents MATCHES "Skate3WatchedLoadU32")
  string(REPLACE
    "#include \"skate3_function_coverage.h\"\n"
    "#include \"skate3_function_coverage.h\"\n#include \"skate3_input_history_watch.h\"\n"
    _skate3_init_contents
    "${_skate3_init_contents}")
  set(_input_load_site
"#define REX_LOAD_U32(x) __builtin_bswap32(*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))")
  set(_input_load_patch
"inline u32 Skate3WatchedLoadU32(u8* base, u32 address,
                                const char* function_name) noexcept {
  const u32 value = __builtin_bswap32(
      *(volatile u32*)(base + address + REX_PHYS_HOST_OFFSET(address)));
  return skate3::input_history_watch::MaybeObserveLoad(
      function_name, address, value);
}
#define REX_LOAD_U32(x) Skate3WatchedLoadU32(base, (u32)(x), __func__)")
  string(FIND "${_skate3_init_contents}" "${_input_load_site}" _input_load_anchor)
  if(_input_load_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to patch REX_LOAD_U32 for input-history telemetry")
  endif()
  string(REPLACE
    "${_input_load_site}"
    "${_input_load_patch}"
    _skate3_init_contents
    "${_skate3_init_contents}")
  file(WRITE "${_skate3_init_header}" "${_skate3_init_contents}")
  message(STATUS "Applied Skate 3 input-history load watcher")
endif()

if(NOT _skate3_init_contents MATCHES "Skate3WatchedLoadU8")
  set(_small_input_load_site
"#define REX_LOAD_U8(x) (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_LOAD_U16(x) __builtin_bswap16(*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))")
  set(_small_input_load_patch
"inline u8 Skate3WatchedLoadU8(u8* base, u32 address,
                              const char* function_name) noexcept {
  const u8 value =
      *(volatile u8*)(base + address + REX_PHYS_HOST_OFFSET(address));
  return static_cast<u8>(skate3::input_history_watch::MaybeObserveLoad(
      function_name, address, value));
}
inline u16 Skate3WatchedLoadU16(u8* base, u32 address,
                                const char* function_name) noexcept {
  const u16 value = __builtin_bswap16(
      *(volatile u16*)(base + address + REX_PHYS_HOST_OFFSET(address)));
  return static_cast<u16>(skate3::input_history_watch::MaybeObserveLoad(
      function_name, address, value));
}
#define REX_LOAD_U8(x) Skate3WatchedLoadU8(base, (u32)(x), __func__)
#define REX_LOAD_U16(x) Skate3WatchedLoadU16(base, (u32)(x), __func__)")
  string(FIND "${_skate3_init_contents}" "${_small_input_load_site}"
    _small_input_load_anchor)
  if(_small_input_load_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to patch REX_LOAD_U8/U16 for input-history telemetry")
  endif()
  string(REPLACE
    "${_small_input_load_site}"
    "${_small_input_load_patch}"
    _skate3_init_contents
    "${_skate3_init_contents}")
  file(WRITE "${_skate3_init_header}" "${_skate3_init_contents}")
  message(STATUS "Applied Skate 3 byte/halfword input-history load watchers")
endif()

if(NOT _skate3_init_contents MATCHES "Skate3WatchedRawAddr")
  set(_raw_addr_site
"#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))")
  set(_raw_addr_patch
"inline u8* Skate3WatchedRawAddr(u8* base, u32 address,
                                const char* function_name) noexcept {
  u8* host_address = base + address + REX_PHYS_HOST_OFFSET(address);
  return skate3::input_history_watch::MaybeObserveRawAddress(
      function_name, address, host_address);
}
#define REX_RAW_ADDR(x) Skate3WatchedRawAddr(base, (u32)(x), __func__)")
  string(FIND "${_skate3_init_contents}" "${_raw_addr_site}" _raw_addr_anchor)
  if(_raw_addr_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to patch REX_RAW_ADDR for input-history telemetry")
  endif()
  string(REPLACE
    "${_raw_addr_site}"
    "${_raw_addr_patch}"
    _skate3_init_contents
    "${_skate3_init_contents}")
  file(WRITE "${_skate3_init_header}" "${_skate3_init_contents}")
  message(STATUS "Applied Skate 3 vector input-history watcher")
endif()

file(GLOB _skate3_recomp_files
  LIST_DIRECTORIES false
  "${SKATE3_SOURCE_DIR}/generated/skate3_recomp.*.cpp")
if(NOT _skate3_recomp_files)
  message(FATAL_ERROR "No generated Skate 3 recompilation files found")
endif()

# Retire temporary crash-diagnosis callbacks from already patched generated
# sources. Recomp generation is incremental, so deleting a patch recipe does
# not itself remove a statement inserted by an earlier build.
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  set(_original_contents "${_contents}")
  string(REPLACE
    "\tskate3::trick_pipeline::ObserveAnimationCreateStream(ctx, base);\n"
    ""
    _contents
    "${_contents}")
  string(REPLACE
    "\tskate3::trick_pipeline::ObserveAnimationRawEval(ctx, base);\n"
    ""
    _contents
    "${_contents}")
  string(REPLACE
    "\tskate3::trick_pipeline::ObserveAnimationStreamTableEval(ctx, base);\n"
    ""
    _contents
    "${_contents}")
  string(REPLACE
    "\tskate3::trick_pipeline::ApplyActiveCustomDisplayNameBeforePublish(\n\t    base, ctx.r29.u32);\n"
    ""
    _contents
    "${_contents}")
  string(REPLACE
    "\tskate3::dlc_runtime::ObserveVaultDlcLoadUpdate(ctx);\n"
    ""
    _contents
    "${_contents}")
  if(NOT _contents STREQUAL _original_contents)
    file(WRITE "${_file}" "${_contents}")
    message(STATUS
      "Removed retired Skate 3 animation diagnostic callbacks from ${_file}")
  endif()
endforeach()

function(_skate3_add_include _contents_var _include)
  set(_contents "${${_contents_var}}")
  if(NOT _contents MATCHES "#include \"${_include}\"")
    string(REPLACE
      "#include \"skate3_init.h\"\n"
      "#include \"skate3_init.h\"\n#include \"${_include}\"\n"
      _contents
      "${_contents}")
  endif()
  set(${_contents_var} "${_contents}" PARENT_SCOPE)
endfunction()

set(_native_grind_load_observer_patched FALSE)
set(_native_grind_add_observer_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)

  if(_contents MATCHES "ObserveSplineDataLoad\\(ctx, base\\)")
    set(_native_grind_load_observer_patched TRUE)
  elseif(_contents MATCHES "DEFINE_REX_FUNC\\(sub_82C1EEF0\\)")
    set(_native_grind_load_site
"DEFINE_REX_FUNC(sub_82C1EEF0) {
	REX_FUNC_PROLOGUE();")
    set(_native_grind_load_patch
"DEFINE_REX_FUNC(sub_82C1EEF0) {
	REX_FUNC_PROLOGUE();
	skate3::native_grind::ObserveSplineDataLoad(ctx, base);")
    string(REPLACE "${_native_grind_load_site}"
      "${_native_grind_load_patch}" _patched_contents "${_contents}")
    if(NOT _patched_contents STREQUAL _contents)
      set(_contents "${_patched_contents}")
      set(_native_grind_load_observer_patched TRUE)
    endif()
  endif()

  if(_contents MATCHES "ObserveGrindDataAdd\\(ctx, base\\)")
    set(_native_grind_add_observer_patched TRUE)
  elseif(_contents MATCHES "DEFINE_REX_FUNC\\(sub_82C1ED60\\)")
    set(_native_grind_add_site
"DEFINE_REX_FUNC(sub_82C1ED60) {
	REX_FUNC_PROLOGUE();")
    set(_native_grind_add_patch
"DEFINE_REX_FUNC(sub_82C1ED60) {
	REX_FUNC_PROLOGUE();
	skate3::native_grind::ObserveGrindDataAdd(ctx, base);")
    string(REPLACE "${_native_grind_add_site}"
      "${_native_grind_add_patch}" _patched_contents "${_contents}")
    if(NOT _patched_contents STREQUAL _contents)
      set(_contents "${_patched_contents}")
      set(_native_grind_add_observer_patched TRUE)
    endif()
  endif()

  if(_contents MATCHES "ObserveSplineDataLoad\\(ctx, base\\)" OR
     _contents MATCHES "ObserveGrindDataAdd\\(ctx, base\\)")
    _skate3_add_include(_contents "skate3_native_grind.h")
    file(WRITE "${_file}" "${_contents}")
  endif()
endforeach()
if(NOT _native_grind_load_observer_patched)
  message(FATAL_ERROR
    "Failed to apply native grind tSplineData load observer")
endif()
if(NOT _native_grind_add_observer_patched)
  message(FATAL_ERROR
    "Failed to apply native GrindData add observer")
endif()

set(_native_collision_streamer_observer_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ObserveWorldStreamerAddVolume\\(ctx, base\\)")
    set(_native_collision_streamer_observer_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82776B58\\)")
    continue()
  endif()
  set(_native_collision_streamer_site
"DEFINE_REX_FUNC(sub_82776B58) {
	REX_FUNC_PROLOGUE();")
  set(_native_collision_streamer_patch
"DEFINE_REX_FUNC(sub_82776B58) {
	REX_FUNC_PROLOGUE();
	skate3::native_collision::ObserveWorldStreamerAddVolume(ctx, base);")
  string(REPLACE "${_native_collision_streamer_site}"
    "${_native_collision_streamer_patch}" _patched_contents "${_contents}")
  if(_patched_contents STREQUAL _contents)
    continue()
  endif()
  set(_contents "${_patched_contents}")
  _skate3_add_include(_contents "skate3_native_collision.h")
  file(WRITE "${_file}" "${_contents}")
  set(_native_collision_streamer_observer_patched TRUE)
  message(STATUS
    "Applied owned native-collision streamer observer in ${_file}")
  break()
endforeach()
if(NOT _native_collision_streamer_observer_patched)
  message(FATAL_ERROR
    "Failed to apply owned native-collision streamer observer")
endif()

set(_native_collision_query_observer_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
      "ObserveNativeTriangleResult\\(ctx\\.r3\\.u32\\)")
    set(_native_collision_query_observer_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_827719B8\\)")
    continue()
  endif()

  foreach(_label IN ITEMS 82771ABC 82771E10)
    set(_site
"loc_${_label}:
	skate3::function_coverage::MaybeRecordAddress(0x${_label});
	// lwz r11,4(r24)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r24.u32 + 4);")
    set(_patch
"loc_${_label}:
	skate3::function_coverage::MaybeRecordAddress(0x${_label});
	// lwz r11,4(r24)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r24.u32 + 4);
	skate3::native_collision::ObserveNativeQueryMesh(ctx.r11.u32);")
    string(REPLACE "${_site}" "${_patch}" _patched "${_contents}")
    if(_patched STREQUAL _contents)
      message(FATAL_ERROR
        "Failed to patch native query mesh observer at ${_label}")
    endif()
    set(_contents "${_patched}")
  endforeach()

  set(_decode_one_site
"	ctx.lr = 0x82771AEC;
	sub_82ACA170(ctx, base);
	// lwz r28,80(r1)
	ctx.r28.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);")
  set(_decode_one_patch
"	ctx.lr = 0x82771AEC;
	sub_82ACA170(ctx, base);
	// lwz r28,80(r1)
	ctx.r28.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	skate3::native_collision::ObserveNativeClusterDecode(ctx.r28.u32);")
  string(REPLACE "${_decode_one_site}" "${_decode_one_patch}"
    _patched "${_contents}")
  if(_patched STREQUAL _contents)
    message(FATAL_ERROR "Failed to patch first native cluster decoder")
  endif()
  set(_contents "${_patched}")

  set(_decode_two_site
"	ctx.lr = 0x82771E40;
	sub_82ACA170(ctx, base);
	// lwz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);")
  set(_decode_two_patch
"	ctx.lr = 0x82771E40;
	sub_82ACA170(ctx, base);
	// lwz r11,80(r1)
	ctx.r11.u64 = REX_LOAD_U32(ctx.r1.u32 + 80);
	skate3::native_collision::ObserveNativeClusterDecode(ctx.r11.u32);")
  string(REPLACE "${_decode_two_site}" "${_decode_two_patch}"
    _patched "${_contents}")
  if(_patched STREQUAL _contents)
    message(FATAL_ERROR "Failed to patch second native cluster decoder")
  endif()
  set(_contents "${_patched}")

  foreach(_return_address IN ITEMS 82771B58 82771EAC)
    set(_triangle_site
"	ctx.lr = 0x${_return_address};
	sub_82ADF5D8(ctx, base);")
    set(_triangle_patch
"	ctx.lr = 0x${_return_address};
	sub_82ADF5D8(ctx, base);
	skate3::native_collision::ObserveNativeTriangleResult(ctx.r3.u32);")
    string(REPLACE "${_triangle_site}" "${_triangle_patch}"
      _patched "${_contents}")
    if(_patched STREQUAL _contents)
      message(FATAL_ERROR
        "Failed to patch native triangle result at ${_return_address}")
    endif()
    set(_contents "${_patched}")
  endforeach()

  _skate3_add_include(_contents "skate3_native_collision.h")
  file(WRITE "${_file}" "${_contents}")
  set(_native_collision_query_observer_patched TRUE)
  message(STATUS
    "Applied owned native-collision query observers in ${_file}")
  break()
endforeach()
if(NOT _native_collision_query_observer_patched)
  message(FATAL_ERROR
    "Failed to apply owned native-collision query observers")
endif()

function(_skate3_patch_native_collision_entry
         _symbol _statement _marker)
  set(_patched FALSE)
  foreach(_file IN LISTS _skate3_recomp_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "${_marker}")
      set(_patched TRUE)
      break()
    endif()
    if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(${_symbol}\\)")
      continue()
    endif()
    set(_site
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();")
    set(_patch
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();
	${_statement}")
    string(REPLACE "${_site}" "${_patch}" _patched_contents "${_contents}")
    if(_patched_contents STREQUAL _contents)
      continue()
    endif()
    set(_contents "${_patched_contents}")
    _skate3_add_include(_contents "skate3_native_collision.h")
    file(WRITE "${_file}" "${_contents}")
    set(_patched TRUE)
    message(STATUS
      "Applied owned native-collision entry observer to ${_symbol} in ${_file}")
    break()
  endforeach()
  if(NOT _patched)
    message(FATAL_ERROR
      "Failed to apply owned native-collision entry observer to ${_symbol}")
  endif()
endfunction()

# Keep an already-generated tree in sync when the batch observer signature
# evolves; generated recomp files are intentionally not the source of truth.
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  string(REPLACE
    "ObserveNativeLineQueryBatch(ctx.r6.u32); // native_line_query_batch"
    "ObserveNativeLineQueryBatch(ctx.r6.u32, base); // native_line_query_batch"
    _patched_contents "${_contents}")
  if(NOT _patched_contents STREQUAL _contents)
    file(WRITE "${_file}" "${_patched_contents}")
  endif()
endforeach()

_skate3_patch_native_collision_entry(
  sub_827719B8
  "skate3::native_collision::ObserveNativeLineWorker(REX_LOAD_U32(ctx.r8.u32 + 4));"
  "ObserveNativeLineWorker")
_skate3_patch_native_collision_entry(
  sub_82772028
  "skate3::native_collision::ObserveNativeBoxWorker(REX_LOAD_U32(ctx.r5.u32 + 4));"
  "ObserveNativeBoxWorker")
_skate3_patch_native_collision_entry(
  sub_8276CC70
  "skate3::native_collision::ObserveNativeIteratorMesh(ctx.r4.u32); // native_bbox_iterator"
  "native_bbox_iterator")
_skate3_patch_native_collision_entry(
  sub_8276CE90
  "skate3::native_collision::ObserveNativeIteratorMesh(ctx.r4.u32); // native_line_iterator"
  "native_line_iterator")
_skate3_patch_native_collision_entry(
  sub_82770650
  "skate3::native_collision::ObserveNativeLineQueryBatch(ctx.r6.u32, base); // native_line_query_batch"
  "native_line_query_batch")
_skate3_patch_native_collision_entry(
  sub_82770B40
  "skate3::native_collision::PrepareNativeBoxQueryBatch(ctx.r6.u32, base); // native_box_query_batch"
  "native_box_query_batch")

function(_skate3_patch_trick_pipeline_entry _symbol _statement _marker)
  set(_patched FALSE)
  foreach(_file IN LISTS _skate3_recomp_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "${_marker}")
      set(_patched TRUE)
      break()
    endif()
    if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(${_symbol}\\)")
      continue()
    endif()
    set(_site
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();")
    set(_patch
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();
	${_statement}")
    string(REPLACE "${_site}" "${_patch}" _patched_contents "${_contents}")
    if(_patched_contents STREQUAL _contents)
      continue()
    endif()
    set(_contents "${_patched_contents}")
    _skate3_add_include(_contents "skate3_trick_pipeline.h")
    file(WRITE "${_file}" "${_contents}")
    set(_patched TRUE)
    message(STATUS
      "Applied Skate 3 trick-pipeline observer to ${_symbol} in ${_file}")
    break()
  endforeach()
  if(NOT _patched)
    message(FATAL_ERROR
      "Failed to apply Skate 3 trick-pipeline observer to ${_symbol}")
  endif()
endfunction()

_skate3_patch_trick_pipeline_entry(
  sub_825999F0
  "skate3::trick_pipeline::ActionGraphInputFillObservationScope action_graph_input_fill_observation(ctx, base);"
  "ActionGraphInputFillObservationScope action_graph_input_fill_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82455030
  "skate3::trick_pipeline::ActionGraphIntentInsertObservationScope action_graph_intent_insert_observation(ctx, base);"
  "ActionGraphIntentInsertObservationScope action_graph_intent_insert_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82B98F70
  "skate3::trick_pipeline::GestureMappingInitObservationScope gesture_mapping_init_observation(ctx, base);"
  "GestureMappingInitObservationScope gesture_mapping_init_observation")
_skate3_patch_trick_pipeline_entry(
  sub_823C3B00
  "skate3::trick_pipeline::FastStringMappingObservationScope fast_string_mapping_observation(ctx, base);"
  "FastStringMappingObservationScope fast_string_mapping_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA07F0
  "skate3::trick_pipeline::GestureIntentObservationScope gesture_intent_observation(ctx, base);"
  "GestureIntentObservationScope gesture_intent_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA1C30
  "skate3::trick_pipeline::CreateTrickIntentObservationScope create_trick_intent_observation(ctx, base);"
  "CreateTrickIntentObservationScope create_trick_intent_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA33E0
  "skate3::trick_pipeline::ObserveScoreModuleUpdate(ctx, base);"
  "ObserveScoreModuleUpdate\\(ctx, base\\)")

# Retire the first experimental placement. The generated sources persist
# between configure passes, so merely changing the insertion recipe would
# otherwise leave this call immediately after sub_82DAC498, where the
# following virtual update overwrites score_output+152.
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  set(_stale_custom_scorable_publish
"	ctx.lr = 0x82DA3550;
	sub_82DAC498(ctx, base);
	skate3::trick_pipeline::PublishCustomScorableIdAfterInputUpdate(
	    base, ctx.r31.u32, ctx.r29.u32);
	// lwz r3,4(r31)")
  set(_retired_custom_scorable_publish
"	ctx.lr = 0x82DA3550;
	sub_82DAC498(ctx, base);
	// lwz r3,4(r31)")
  string(FIND "${_contents}" "${_stale_custom_scorable_publish}"
    _stale_custom_scorable_publish_anchor)
  if(NOT _stale_custom_scorable_publish_anchor EQUAL -1)
    string(REPLACE "${_stale_custom_scorable_publish}"
      "${_retired_custom_scorable_publish}" _contents "${_contents}")
    file(WRITE "${_file}" "${_contents}")
  endif()
endforeach()

set(_custom_scorable_publish_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "PublishCustomScorableIdAfterInputUpdate")
    set(_custom_scorable_publish_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82DA33E0\\)")
    continue()
  endif()
  set(_custom_scorable_publish_site
"	ctx.lr = 0x82DA3564;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	// lwz r11,0(r31)")
  set(_custom_scorable_publish_patch
"	ctx.lr = 0x82DA3564;
	REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
	skate3::trick_pipeline::PublishCustomScorableIdAfterInputUpdate(
	    base, ctx.r31.u32, ctx.r29.u32);
	// lwz r11,0(r31)")
  string(FIND "${_contents}" "${_custom_scorable_publish_site}"
    _custom_scorable_publish_anchor)
  if(_custom_scorable_publish_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to find final ScoreModule descriptor-to-ID publish boundary")
  endif()
  string(REPLACE "${_custom_scorable_publish_site}"
    "${_custom_scorable_publish_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_custom_scorable_publish_patched TRUE)
  break()
endforeach()
if(NOT _custom_scorable_publish_patched)
  message(FATAL_ERROR "Failed to publish custom Scorable ID")
endif()
_skate3_patch_trick_pipeline_entry(
  sub_82DA4010
  "skate3::trick_pipeline::ScoreCollectorTransitionObservationScope score_collector_transition_observation(ctx, base);"
  "ScoreCollectorTransitionObservationScope score_collector_transition_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA8A70
  "skate3::trick_pipeline::ObserveAirTrickAnalysis(ctx, base);"
  "ObserveAirTrickAnalysis\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA9218
  "skate3::trick_pipeline::ObserveAirEndTrick(ctx, base);"
  "ObserveAirEndTrick\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA92B8
  "skate3::trick_pipeline::ObserveAirStartTrick(ctx, base);"
  "ObserveAirStartTrick\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA45C8
  "skate3::trick_pipeline::ScorableStartObservationScope scorable_start_observation(ctx, base);"
  "ScorableStartObservationScope scorable_start_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA25B8
  "skate3::trick_pipeline::CustomScorableNameResolutionScope custom_scorable_name_resolution(ctx, base);"
  "CustomScorableNameResolutionScope custom_scorable_name_resolution")
_skate3_patch_trick_pipeline_entry(
  sub_82DA22A8
  "skate3::trick_pipeline::CustomScorableIdResolutionScope custom_scorable_id_resolution(ctx, base);"
  "CustomScorableIdResolutionScope custom_scorable_id_resolution")
_skate3_patch_trick_pipeline_entry(
  sub_82DA4E98
  "if (skate3::trick_pipeline::TryReturnCustomScorableName(ctx, base)) return;"
  "TryReturnCustomScorableName\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA93D8
  "skate3::trick_pipeline::CustomAirCollectorUpdateScope custom_air_collector_update(ctx, base);"
  "CustomAirCollectorUpdateScope custom_air_collector_update")

set(_custom_air_collector_start_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
      "custom_air_collector_start_metadata\\(base, ctx\\.r31\\.u32\\)")
    set(_custom_air_collector_start_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82DA92B8\\)")
    continue()
  endif()

  set(_custom_air_collector_constructor_site
"	ctx.lr = 0x82DA92E8;
	sub_82DAC698(ctx, base);
	// mr r4,r30")
  set(_custom_air_collector_constructor_patch
"	ctx.lr = 0x82DA92E8;
	sub_82DAC698(ctx, base);
	skate3::trick_pipeline::CustomAirCollectorStartMetadataScope
	    custom_air_collector_start_metadata(base, ctx.r31.u32);
	// mr r4,r30")
  string(FIND "${_contents}" "${_custom_air_collector_constructor_site}"
    _custom_air_collector_constructor_anchor)
  if(_custom_air_collector_constructor_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to find AirCollector post-construction metadata boundary")
  endif()
  string(REPLACE "${_custom_air_collector_constructor_site}"
    "${_custom_air_collector_constructor_patch}" _contents "${_contents}")

  set(_custom_air_collector_start_site
"	// bl 0x82da92b8
	ctx.lr = 0x82DA962C;
	sub_82DA92B8(ctx, base);")
  set(_custom_air_collector_start_patch
"	// bl 0x82da92b8
	skate3::trick_pipeline::PromoteCustomScorableForAirCollectorStart(ctx);
	ctx.lr = 0x82DA962C;
	sub_82DA92B8(ctx, base);")
  string(FIND "${_contents}" "${_custom_air_collector_start_site}"
    _custom_air_collector_start_anchor)
  if(_custom_air_collector_start_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find primary AirCollector start boundary")
  endif()
  string(REPLACE "${_custom_air_collector_start_site}"
    "${_custom_air_collector_start_patch}" _contents "${_contents}")

  set(_custom_air_collector_start_fallback_site
"	// bl 0x82da92b8
	ctx.lr = 0x82DA9654;
	sub_82DA92B8(ctx, base);")
  set(_custom_air_collector_start_fallback_patch
"	// bl 0x82da92b8
	skate3::trick_pipeline::PromoteCustomScorableForAirCollectorStart(ctx);
	ctx.lr = 0x82DA9654;
	sub_82DA92B8(ctx, base);")
  string(FIND "${_contents}" "${_custom_air_collector_start_fallback_site}"
    _custom_air_collector_start_fallback_anchor)
  if(_custom_air_collector_start_fallback_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find fallback AirCollector start boundary")
  endif()
  string(REPLACE "${_custom_air_collector_start_fallback_site}"
    "${_custom_air_collector_start_fallback_patch}" _contents "${_contents}")

  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_custom_air_collector_start_patched TRUE)
  break()
endforeach()
if(NOT _custom_air_collector_start_patched)
  message(FATAL_ERROR "Failed to adapt custom AirCollector boundaries")
endif()
_skate3_patch_trick_pipeline_entry(
  sub_82DA5D18
  "skate3::trick_pipeline::PointPenaltyObservationScope point_penalty_observation(ctx, base);"
  "PointPenaltyObservationScope point_penalty_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA93D8
  "skate3::trick_pipeline::ObserveAirUpdateTricks(ctx, base);"
  "ObserveAirUpdateTricks\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA5F98
  "skate3::trick_pipeline::ObserveScoreHolderRecordTrick(ctx, base);"
  "ObserveScoreHolderRecordTrick\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA60F0
  "skate3::trick_pipeline::ObserveScoreHolderCancelTrick(ctx, base);"
  "ObserveScoreHolderCancelTrick\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82DA6260
  "skate3::trick_pipeline::CustomRepetitionEndAirScope custom_repetition_end_air(ctx, base);"
  "CustomRepetitionEndAirScope custom_repetition_end_air")
_skate3_patch_trick_pipeline_entry(
  sub_82DA6260
  "skate3::trick_pipeline::ScoreHolderEndAirObservationScope score_holder_end_air_observation(ctx, base);"
  "ScoreHolderEndAirObservationScope score_holder_end_air_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA6468
  "skate3::trick_pipeline::ScoreHolderRewardAirSequenceObservationScope score_holder_reward_air_sequence_observation(ctx, base);"
  "ScoreHolderRewardAirSequenceObservationScope score_holder_reward_air_sequence_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA6538
  "skate3::trick_pipeline::ScoreHolderPublishAirSequenceObservationScope score_holder_publish_air_sequence_observation(ctx, base);"
  "ScoreHolderPublishAirSequenceObservationScope score_holder_publish_air_sequence_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DA9B08
  "skate3::trick_pipeline::GrindCollectorExitObservationScope grind_collector_exit_observation(ctx, base);"
  "GrindCollectorExitObservationScope grind_collector_exit_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DB9100
  "skate3::trick_pipeline::WipeoutRequestedObservationScope wipeout_requested_observation(ctx, base);"
  "WipeoutRequestedObservationScope wipeout_requested_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82D886B8
  "skate3::trick_pipeline::CollisionForceWipeoutObservationScope collision_force_wipeout_observation(ctx, base);"
  "CollisionForceWipeoutObservationScope collision_force_wipeout_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BC2FD8
  "skate3::trick_pipeline::PhysicsWantsWipeoutConditionFactoryObservationScope physics_wants_wipeout_condition_factory_observation(ctx, base);"
  "PhysicsWantsWipeoutConditionFactoryObservationScope physics_wants_wipeout_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA4400
  "skate3::trick_pipeline::PhysicsWantsWipeoutConditionEvaluationScope physics_wants_wipeout_condition_evaluation(ctx, base);"
  "PhysicsWantsWipeoutConditionEvaluationScope physics_wants_wipeout_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82BC4FB8
  "skate3::trick_pipeline::CanLandOnBoardConditionFactoryObservationScope can_land_on_board_condition_factory_observation(ctx, base);"
  "CanLandOnBoardConditionFactoryObservationScope can_land_on_board_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BC52B0
  "skate3::trick_pipeline::TiltTooLargeForPrelandConditionFactoryObservationScope tilt_too_large_for_preland_condition_factory_observation(ctx, base);"
  "TiltTooLargeForPrelandConditionFactoryObservationScope tilt_too_large_for_preland_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BC2598
  "skate3::trick_pipeline::IsLandingOnBoardConditionFactoryObservationScope is_landing_on_board_condition_factory_observation(ctx, base);"
  "IsLandingOnBoardConditionFactoryObservationScope is_landing_on_board_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BC8DE0
  "skate3::trick_pipeline::IsLandingConditionFactoryObservationScope is_landing_condition_factory_observation(ctx, base);"
  "IsLandingConditionFactoryObservationScope is_landing_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA5D30
  "skate3::trick_pipeline::CanLandOnBoardConditionEvaluationScope can_land_on_board_condition_evaluation(ctx, base);"
  "CanLandOnBoardConditionEvaluationScope can_land_on_board_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA69F0
  "skate3::trick_pipeline::TiltTooLargeForPrelandConditionEvaluationScope tilt_too_large_for_preland_condition_evaluation(ctx, base);"
  "TiltTooLargeForPrelandConditionEvaluationScope tilt_too_large_for_preland_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82BA16F0
  "skate3::trick_pipeline::IsLandingOnBoardConditionEvaluationScope is_landing_on_board_condition_evaluation(ctx, base);"
  "IsLandingOnBoardConditionEvaluationScope is_landing_on_board_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82BACBE0
  "skate3::trick_pipeline::IsLandingConditionEvaluationScope is_landing_on_board_mode_condition_evaluation(ctx, base, true);"
  "IsLandingConditionEvaluationScope is_landing_on_board_mode_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82BACC30
  "skate3::trick_pipeline::IsLandingConditionEvaluationScope is_landing_off_board_mode_condition_evaluation(ctx, base, false);"
  "IsLandingConditionEvaluationScope is_landing_off_board_mode_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82DFC118
  "skate3::trick_pipeline::IsOffboardConditionFactoryObservationScope is_offboard_condition_factory_observation(ctx, base);"
  "IsOffboardConditionFactoryObservationScope is_offboard_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DFC258
  "skate3::trick_pipeline::IsAirOffboardConditionFactoryObservationScope is_air_offboard_condition_factory_observation(ctx, base);"
  "IsAirOffboardConditionFactoryObservationScope is_air_offboard_condition_factory_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DF51F8
  "skate3::trick_pipeline::IsOffboardConditionEvaluationScope is_offboard_condition_evaluation(ctx, base, false);"
  "IsOffboardConditionEvaluationScope is_offboard_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82DF52B0
  "skate3::trick_pipeline::IsOffboardConditionEvaluationScope is_air_offboard_condition_evaluation(ctx, base, true);"
  "IsOffboardConditionEvaluationScope is_air_offboard_condition_evaluation")
_skate3_patch_trick_pipeline_entry(
  sub_82DEFB98
  "skate3::trick_pipeline::AnimationCompletedObservationScope animation_completed_observation(ctx, base);"
  "AnimationCompletedObservationScope animation_completed_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82DEFCC0
  "skate3::trick_pipeline::AnimationCurrentObservationScope animation_current_observation(ctx, base);"
  "AnimationCurrentObservationScope animation_current_observation")
_skate3_patch_trick_pipeline_entry(
  sub_825E51A0
  "skate3::trick_pipeline::TrickDisplayRefreshObservationScope trick_display_refresh_observation(ctx, base);"
  "TrickDisplayRefreshObservationScope trick_display_refresh_observation")

_skate3_patch_trick_pipeline_entry(
  sub_828254A0
  "skate3::trick_pipeline::SceneAnimationLoaderAddObservationScope scene_animation_loader_add_observation(ctx, base);"
  "SceneAnimationLoaderAddObservationScope scene_animation_loader_add_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82825060
  "skate3::trick_pipeline::AnimationLoaderDataLookupObservationScope animation_loader_data_lookup_observation(ctx, base);"
  "AnimationLoaderDataLookupObservationScope animation_loader_data_lookup_observation")
_skate3_patch_trick_pipeline_entry(
  sub_828248E8
  "skate3::trick_pipeline::SceneAnimationLoaderLoadObservationScope scene_animation_loader_load_observation(ctx, base);"
  "SceneAnimationLoaderLoadObservationScope scene_animation_loader_load_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82824BC8
  "skate3::trick_pipeline::SceneAnimationLoaderPollObservationScope scene_animation_loader_poll_observation(ctx, base);"
  "SceneAnimationLoaderPollObservationScope scene_animation_loader_poll_observation")
_skate3_patch_trick_pipeline_entry(
  sub_8298F430
  "skate3::trick_pipeline::SceneAnimationAsyncLoadObservationScope scene_animation_async_load_observation(ctx, base);"
  "SceneAnimationAsyncLoadObservationScope scene_animation_async_load_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82D19518
  "skate3::trick_pipeline::PlaybackDataConstructionObservationScope playback_data_construction_observation(ctx, base);"
  "PlaybackDataConstructionObservationScope playback_data_construction_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82D19728
  "skate3::trick_pipeline::PlaybackDataLookupObservationScope playback_data_lookup_observation(ctx, base);"
  "PlaybackDataLookupObservationScope playback_data_lookup_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82D19BD0
  "skate3::trick_pipeline::AndaleDatabaseLoadObservationScope andale_database_load_observation(ctx, base);"
  "AndaleDatabaseLoadObservationScope andale_database_load_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82D19A40
  "skate3::trick_pipeline::AndaleDatabaseContentObservationScope andale_database_content_observation(ctx, base);"
  "AndaleDatabaseContentObservationScope andale_database_content_observation")
_skate3_patch_trick_pipeline_entry(
  sub_82A8A018
  "skate3::trick_pipeline::ObserveXenonFileDeviceRead(ctx);"
  "ObserveXenonFileDeviceRead\\(ctx\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82D21E28
  "skate3::trick_pipeline::ObserveAnimationEvalCommandBuffer(ctx, base);"
  "ObserveAnimationEvalCommandBuffer\\(ctx, base\\)")
_skate3_patch_trick_pipeline_entry(
  sub_82D26010
  "skate3::trick_pipeline::CustomAnimationStreamEvalScope custom_animation_stream_eval_scope(ctx, base);"
  "CustomAnimationStreamEvalScope custom_animation_stream_eval_scope")
_skate3_patch_trick_pipeline_entry(
  sub_82D1DE08
  "skate3::trick_pipeline::VbrExtractObservationScope vbr_extract_observation_scope(ctx, base);"
  "VbrExtractObservationScope vbr_extract_observation_scope")
_skate3_patch_trick_pipeline_entry(
  sub_82BD8918
  "skate3::trick_pipeline::CacGestureFinalPoseObservationScope cac_gesture_final_pose_observation(ctx, base);"
  "CacGestureFinalPoseObservationScope cac_gesture_final_pose_observation")

set(_andale_custom_asset_stages_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
      "ObserveActiveCustomAnimationAssetLoadStage\\(\"content-return\"\\)")
    set(_andale_custom_asset_stages_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82D19BD0\\)")
    continue()
  endif()
  set(_stage_site
"	ctx.lr = 0x82D19C48;
	sub_8298F430(ctx, base);")
  set(_stage_patch
"	ctx.lr = 0x82D19C48;
	sub_8298F430(ctx, base);
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"async-submit-return\");")
  string(FIND "${_contents}" "${_stage_site}" _stage_anchor)
  if(_stage_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find Andale async-submit stage")
  endif()
  string(REPLACE "${_stage_site}" "${_stage_patch}"
    _contents "${_contents}")

  set(_stage_site
"	ctx.lr = 0x82D19C5C;
	sub_828AAE90(ctx, base);")
  set(_stage_patch
"	ctx.lr = 0x82D19C5C;
	sub_828AAE90(ctx, base);
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"database-allocation-return\");")
  string(FIND "${_contents}" "${_stage_site}" _stage_anchor)
  if(_stage_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find Andale allocation stage")
  endif()
  string(REPLACE "${_stage_site}" "${_stage_patch}"
    _contents "${_contents}")

  set(_stage_site
"loc_82D19D34:
	skate3::function_coverage::MaybeRecordAddress(0x82D19D34);")
  set(_stage_patch
"loc_82D19D34:
	skate3::function_coverage::MaybeRecordAddress(0x82D19D34);
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"async-complete\");")
  string(FIND "${_contents}" "${_stage_site}" _stage_anchor)
  if(_stage_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find Andale async-complete stage")
  endif()
  string(REPLACE "${_stage_site}" "${_stage_patch}"
    _contents "${_contents}")

  set(_stage_site
"	ctx.lr = 0x82D19D78;
	sub_82D1B340(ctx, base);")
  set(_stage_patch
"	ctx.lr = 0x82D19D78;
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"content-begin\");
	sub_82D1B340(ctx, base);
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"content-return\");")
  string(FIND "${_contents}" "${_stage_site}" _stage_anchor)
  if(_stage_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find Andale content stage")
  endif()
  string(REPLACE "${_stage_site}" "${_stage_patch}"
    _contents "${_contents}")

  set(_stage_site
"	ctx.lr = 0x82D19D94;
	sub_82D1B488(ctx, base);")
  set(_stage_patch
"	ctx.lr = 0x82D19D94;
	sub_82D1B488(ctx, base);
	skate3::trick_pipeline::ObserveActiveCustomAnimationAssetLoadStage(\"manager-register-return\");")
  string(FIND "${_contents}" "${_stage_site}" _stage_anchor)
  if(_stage_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find Andale manager-register stage")
  endif()
  string(REPLACE "${_stage_site}" "${_stage_patch}"
    _contents "${_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_andale_custom_asset_stages_patched TRUE)
  message(STATUS
    "Applied Skate 3 Andale custom-asset stage probes in ${_file}")
  break()
endforeach()
if(NOT _andale_custom_asset_stages_patched)
  message(FATAL_ERROR
    "Failed to apply Andale custom-asset stage probes")
endif()

set(_gesture_mapping_match_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ObserveGestureMappingMatch\\(ctx, base\\)")
    set(_gesture_mapping_match_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82BA07F0\\)")
    continue()
  endif()
  set(_gesture_mapping_match_covered_site
"loc_82BA098C:
	skate3::function_coverage::MaybeRecordAddress(0x82BA098C);")
  set(_gesture_mapping_match_covered_patch
"loc_82BA098C:
	skate3::function_coverage::MaybeRecordAddress(0x82BA098C);
	skate3::trick_pipeline::ObserveGestureMappingMatch(ctx, base);")
  string(REPLACE
    "${_gesture_mapping_match_covered_site}"
    "${_gesture_mapping_match_covered_patch}"
    _patched_contents
    "${_contents}")
  if(_patched_contents STREQUAL _contents)
    set(_gesture_mapping_match_site "loc_82BA098C:")
    set(_gesture_mapping_match_patch
"loc_82BA098C:
	skate3::trick_pipeline::ObserveGestureMappingMatch(ctx, base);")
    string(REPLACE
      "${_gesture_mapping_match_site}"
      "${_gesture_mapping_match_patch}"
      _patched_contents
      "${_contents}")
  endif()
  if(_patched_contents STREQUAL _contents)
    message(FATAL_ERROR
      "Failed to find GestureTrickMapping match-success site")
  endif()
  set(_contents "${_patched_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_gesture_mapping_match_patched TRUE)
  message(STATUS
    "Applied Skate 3 gesture-mapping match observer in ${_file}")
  break()
endforeach()
if(NOT _gesture_mapping_match_patched)
  message(FATAL_ERROR "Failed to apply GestureTrickMapping match observer")
endif()

set(_skater_animation_asset_preload_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "PreloadConfiguredSkaterAnimationAssets")
    set(_skater_animation_asset_preload_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82858810\\)")
    continue()
  endif()
  set(_skater_animation_asset_preload_site
"	ctx.lr = 0x82858AD0;
	sub_82549888(ctx, base);")
  set(_skater_animation_asset_preload_patch
"	ctx.lr = 0x82858AD0;
	sub_82549888(ctx, base);
	skate3::trick_pipeline::PreloadConfiguredSkaterAnimationAssets(
	    ctx, base, ctx.r30.u32);")
  string(FIND "${_contents}" "${_skater_animation_asset_preload_site}"
    _skater_animation_asset_preload_anchor)
  if(_skater_animation_asset_preload_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to find SKATER OnBoard database attachment site")
  endif()
  string(REPLACE "${_skater_animation_asset_preload_site}"
    "${_skater_animation_asset_preload_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_skater_animation_asset_preload_patched TRUE)
  message(STATUS
    "Applied Skate 3 configured SKATER animation asset preload in ${_file}")
  break()
endforeach()
if(NOT _skater_animation_asset_preload_patched)
  message(FATAL_ERROR
    "Failed to apply configured SKATER animation asset preload")
endif()

set(_play_animation_override_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ApplySelectedAnimationOverride")
    set(_play_animation_override_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82BB5188\\)")
    continue()
  endif()
  set(_play_animation_override_site
"loc_82BB5510:
	skate3::function_coverage::MaybeRecordAddress(0x82BB5510);
	// lwz r11,0(r29)")
  set(_play_animation_override_patch
"loc_82BB5510:
	skate3::function_coverage::MaybeRecordAddress(0x82BB5510);
	skate3::trick_pipeline::ApplySelectedAnimationOverride(
	    ctx, base, ctx.r1.u32 + 128);
	// lwz r11,0(r29)")
  string(FIND "${_contents}" "${_play_animation_override_site}"
    _play_animation_override_anchor)
  if(_play_animation_override_anchor EQUAL -1)
    set(_play_animation_override_site
"loc_82BB5510:
	// lwz r11,0(r29)")
    set(_play_animation_override_patch
"loc_82BB5510:
	skate3::trick_pipeline::ApplySelectedAnimationOverride(
	    ctx, base, ctx.r1.u32 + 128);
	// lwz r11,0(r29)")
    string(FIND "${_contents}" "${_play_animation_override_site}"
      _play_animation_override_anchor)
  endif()
  if(_play_animation_override_anchor EQUAL -1)
    message(FATAL_ERROR
      "Failed to find PlayAnimation selected-intent dispatch site")
  endif()
  string(REPLACE "${_play_animation_override_site}"
    "${_play_animation_override_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_play_animation_override_patched TRUE)
  message(STATUS
    "Applied Skate 3 selected-animation override in ${_file}")
  break()
endforeach()
if(NOT _play_animation_override_patched)
  message(FATAL_ERROR "Failed to apply selected-animation override")
endif()

set(_coverage_labels_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "MaybeRecordAddress")
    set(_coverage_labels_patched TRUE)
    continue()
  endif()
  string(REGEX REPLACE
    "(loc_([0-9A-Fa-f]+):\n)"
    "\\1\tskate3::function_coverage::MaybeRecordAddress(0x\\2);\n"
    _patched_contents
    "${_contents}")
  if(NOT _patched_contents STREQUAL _contents)
    file(WRITE "${_file}" "${_patched_contents}")
    set(_coverage_labels_patched TRUE)
  endif()
endforeach()
if(NOT _coverage_labels_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated basic-block coverage probes")
endif()

set(_scoring_trick_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_82BBFA88\\)[^\n]*\n[^\n]*\n[^\n]*ObserveExecuteTrick")
    set(_scoring_trick_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82BBFA88\\)")
    continue()
  endif()
  set(_scoring_trick_site
"DEFINE_REX_FUNC(sub_82BBFA88) {
	REX_FUNC_PROLOGUE();")
  set(_scoring_trick_patch
"DEFINE_REX_FUNC(sub_82BBFA88) {
	REX_FUNC_PROLOGUE();
	skate3::scoring::ObserveExecuteTrick(ctx, base);")
  string(REPLACE
    "${_scoring_trick_site}"
    "${_scoring_trick_patch}"
    _contents
    "${_contents}")
  _skate3_add_include(_contents "skate3_scoring.h")
  file(WRITE "${_file}" "${_contents}")
  set(_scoring_trick_patched TRUE)
  message(STATUS "Applied Skate 3 ScoringTrick observer in ${_file}")
  break()
endforeach()
if(NOT _scoring_trick_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 ScoringTrick observer")
endif()

set(_input_manager_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_8296D288\\)[^\n]*\n[^\n]*\n[^\n]*RegisterManager")
    set(_input_manager_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_8296D288\\)")
    continue()
  endif()
  set(_input_manager_site
"DEFINE_REX_FUNC(sub_8296D288) {
	REX_FUNC_PROLOGUE();")
  set(_input_manager_patch
"DEFINE_REX_FUNC(sub_8296D288) {
	REX_FUNC_PROLOGUE();
	skate3::input_history_watch::RegisterManager(ctx.r3.u32);")
  string(REPLACE
    "${_input_manager_site}"
    "${_input_manager_patch}"
    _contents
    "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  set(_input_manager_patched TRUE)
  message(STATUS "Applied Skate 3 input-manager registration hook in ${_file}")
  break()
endforeach()
if(NOT _input_manager_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 input-manager registration hook")
endif()

set(_input_history_manager_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_8296D0D0\\)[^\n]*\n[^\n]*\n[^\n]*RegisterManager")
    set(_input_history_manager_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_8296D0D0\\)")
    continue()
  endif()
  set(_input_history_manager_site
"DEFINE_REX_FUNC(sub_8296D0D0) {
	REX_FUNC_PROLOGUE();")
  set(_input_history_manager_patch
"DEFINE_REX_FUNC(sub_8296D0D0) {
	REX_FUNC_PROLOGUE();
	skate3::input_history_watch::RegisterManager(ctx.r3.u32);")
  string(REPLACE
    "${_input_history_manager_site}"
    "${_input_history_manager_patch}"
    _contents
    "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  set(_input_history_manager_patched TRUE)
  message(STATUS "Applied Skate 3 input-history manager hook in ${_file}")
  break()
endforeach()
if(NOT _input_history_manager_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 input-history manager hook")
endif()

set(_input_frame_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_8296D5F8\\)[^\n]*\n[^\n]*\n[^\n]*RegisterFrame")
    set(_input_frame_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_8296D5F8\\)")
    continue()
  endif()
  set(_input_frame_site
"DEFINE_REX_FUNC(sub_8296D5F8) {
	REX_FUNC_PROLOGUE();")
  set(_input_frame_patch
"DEFINE_REX_FUNC(sub_8296D5F8) {
	REX_FUNC_PROLOGUE();
	skate3::input_history_watch::RegisterFrame(ctx.r6.u32);")
  string(REPLACE
    "${_input_frame_site}"
    "${_input_frame_patch}"
    _contents
    "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  set(_input_frame_patched TRUE)
  message(STATUS "Applied Skate 3 normalized input-frame hook in ${_file}")
  break()
endforeach()
if(NOT _input_frame_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 normalized input-frame hook")
endif()

set(_processed_input_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_82966F30\\)[^\n]*\n[^\n]*\n[^\n]*ObserveProcessedChannels")
    set(_processed_input_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82966F30\\)")
    continue()
  endif()
  set(_processed_input_site
"DEFINE_REX_FUNC(sub_82966F30) {
	REX_FUNC_PROLOGUE();")
  set(_processed_input_patch
"DEFINE_REX_FUNC(sub_82966F30) {
	REX_FUNC_PROLOGUE();
	skate3::input_history_watch::ObserveProcessedChannels(
	    base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);")
  string(REPLACE
    "${_processed_input_site}"
    "${_processed_input_patch}"
    _contents
    "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  set(_processed_input_patched TRUE)
  message(STATUS "Applied Skate 3 processed-input hook in ${_file}")
  break()
endforeach()
if(NOT _processed_input_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 processed-input hook")
endif()

set(_processed_input_coordinator_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_82699230\\)[^\n]*\n[^\n]*\n[^\n]*ObserveProcessedInputCoordinator")
    set(_processed_input_coordinator_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82699230\\)")
    continue()
  endif()
  set(_processed_input_coordinator_site
"DEFINE_REX_FUNC(sub_82699230) {
	REX_FUNC_PROLOGUE();")
  set(_processed_input_coordinator_patch
"DEFINE_REX_FUNC(sub_82699230) {
	REX_FUNC_PROLOGUE();
	skate3::input_lab::ObserveProcessedInputCoordinator(
	    base, ctx.r3.u32, ctx.r4.u32);")
  string(REPLACE
    "${_processed_input_coordinator_site}"
    "${_processed_input_coordinator_patch}"
    _contents
    "${_contents}")
  _skate3_add_include(_contents "skate3_input_lab.h")
  file(WRITE "${_file}" "${_contents}")
  set(_processed_input_coordinator_patched TRUE)
  message(STATUS
    "Applied Skate 3 processed-input coordinator observer in ${_file}")
  break()
endforeach()
if(NOT _processed_input_coordinator_patched)
  message(FATAL_ERROR
    "Failed to apply Skate 3 processed-input coordinator observer")
endif()

set(_local_player_factory_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ObserveLocalPlayerCreated\\(base, ctx\\.r31\\.u32\\)")
    set(_local_player_factory_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82DCAF50\\)")
    continue()
  endif()
  set(_local_player_factory_site
"loc_82DCAFEC:
	skate3::function_coverage::MaybeRecordAddress(0x82DCAFEC);
	// stw r31,0(r27)")
  set(_local_player_factory_patch
"loc_82DCAFEC:
	skate3::function_coverage::MaybeRecordAddress(0x82DCAFEC);
	skate3::input_lab::ObserveLocalPlayerCreated(base, ctx.r31.u32);
	// stw r31,0(r27)")
  string(FIND "${_contents}" "${_local_player_factory_site}"
    _local_player_factory_anchor)
  if(_local_player_factory_anchor EQUAL -1)
    message(FATAL_ERROR "Failed to find LocalPlayer factory completion site")
  endif()
  string(REPLACE
    "${_local_player_factory_site}"
    "${_local_player_factory_patch}"
    _contents
    "${_contents}")
  _skate3_add_include(_contents "skate3_input_lab.h")
  file(WRITE "${_file}" "${_contents}")
  set(_local_player_factory_patched TRUE)
  message(STATUS "Applied Skate 3 LocalPlayer factory observer in ${_file}")
  break()
endforeach()
if(NOT _local_player_factory_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 LocalPlayer factory observer")
endif()

set(_frustum_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3UltrawideGameFrustumPatchScope")
    set(_frustum_patched TRUE)
    break()
  endif()
  string(FIND "${_contents}" "ctx.r6.u64 = REX_LOAD_U32(ctx.r4.u32 + 5260);" _frustum_anchor)
  if(_frustum_anchor EQUAL -1)
    continue()
  endif()

  string(SUBSTRING "${_contents}" ${_frustum_anchor} 12000 _frustum_window)
  string(REGEX MATCH
    "\t// bl 0x[0-9a-fA-F]+\n\tctx\\.lr = 0x[0-9A-F]+;\n\tsub_[0-9A-F]+\\(ctx, base\\);"
    _frustum_call
    "${_frustum_window}")
  if(_frustum_call STREQUAL "")
    message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; call near frustum anchor not found in ${_file}")
  endif()

  string(REGEX REPLACE
    "(\tctx\\.lr = 0x[0-9A-F]+;\n)"
    "\\1\tSkate3UltrawideGameFrustumPatchScope skate3_ultrawide_game_frustum_patch_scope(\n\t\tctx, base, ctx.r4.u32);\n"
    _frustum_patch
    "${_frustum_call}")
  string(REPLACE "${_frustum_call}" "${_frustum_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_ultrawide_guest.h")
  file(WRITE "${_file}" "${_contents}")
  set(_frustum_patched TRUE)
  message(STATUS "Applied Skate 3 generated frustum patch in ${_file}")
  break()
endforeach()
if(NOT _frustum_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated frustum patch; frustum anchor not found")
endif()

set(_fov_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "Skate3MaybeOverrideProjectionFovRadians")
    set(_fov_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "ctx\\.f27\\.f64 = ctx\\.f1\\.f64;")
    continue()
  endif()
  if(NOT _contents MATCHES "ctx\\.f4\\.f64 = double\\(float\\(ctx\\.f1\\.f64 \\* ctx\\.f0\\.f64\\)\\);")
    continue()
  endif()

  set(_projection_fov_site "ctx.f27.f64 = ctx.f1.f64;")
  set(_projection_fov_patch
"ctx.f1.f64 = double(Skate3MaybeOverrideProjectionFovRadians(float(ctx.f1.f64)));
	ctx.f27.f64 = ctx.f1.f64;")
  string(REPLACE "${_projection_fov_site}" "${_projection_fov_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_fov.h")
  file(WRITE "${_file}" "${_contents}")
  set(_fov_patched TRUE)
  message(STATUS "Applied Skate 3 generated projection FOV patch in ${_file}")
  break()
endforeach()
if(NOT _fov_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 generated projection FOV patch; projection FOV anchor not found")
endif()

set(_demo_path_movie_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ShouldForceIntroMovieComplete")
    set(_demo_path_movie_patched TRUE)
    break()
  endif()

  set(_demo_path_movie_site
"	// bl 0x825d60c8
	ctx.lr = 0x825E05A0;
	sub_825D60C8(ctx, base);")
  if(NOT _contents MATCHES "ctx\\.lr = 0x825E05A0;")
    continue()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_825E0510\\)")
    continue()
  endif()
  string(FIND "${_contents}" "${_demo_path_movie_site}" _demo_path_movie_anchor)
  if(_demo_path_movie_anchor EQUAL -1)
    continue()
  endif()

  set(_demo_path_movie_patch
"	if (skate3::demo_path::ShouldForceIntroMovieComplete()) {
		ctx.r3.u64 = 0;
	} else {
		// bl 0x825d60c8
		ctx.lr = 0x825E05A0;
		sub_825D60C8(ctx, base);
	}")
  string(REPLACE "${_demo_path_movie_site}" "${_demo_path_movie_patch}" _contents "${_contents}")
  _skate3_add_include(_contents "skate3_demo_path.h")
  file(WRITE "${_file}" "${_contents}")
  set(_demo_path_movie_patched TRUE)
  message(STATUS "Applied Skate 3 demo path intro movie patch in ${_file}")
  break()
endforeach()
if(NOT _demo_path_movie_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 demo path intro movie patch; FEMoviePlayer::Update anchor not found")
endif()

set(_demo_path_state_probe_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "DEFINE_REX_FUNC\\(sub_82D0AFA0\\)[^\n]*\n[^\n]*\n[^\n]*ObserveFrontEndState")
    set(_demo_path_state_probe_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82D0AFA0\\)")
    continue()
  endif()
  set(_demo_path_state_probe_site
"DEFINE_REX_FUNC(sub_82D0AFA0) {
	REX_FUNC_PROLOGUE();")
  set(_demo_path_state_probe_patch
"DEFINE_REX_FUNC(sub_82D0AFA0) {
	REX_FUNC_PROLOGUE();
	skate3::demo_path::ObserveFrontEndState(ctx.r3.u32, ctx.r4.u32,
	                                      ctx.r5.u32, ctx.lr);")
  string(REPLACE
    "${_demo_path_state_probe_site}"
    "${_demo_path_state_probe_patch}"
    _contents
    "${_contents}")
  _skate3_add_include(_contents "skate3_demo_path.h")
  file(WRITE "${_file}" "${_contents}")
  set(_demo_path_state_probe_patched TRUE)
  message(STATUS "Applied Skate 3 frontend-state probe in ${_file}")
  break()
endforeach()
if(NOT _demo_path_state_probe_patched)
  message(FATAL_ERROR "Failed to apply Skate 3 frontend-state probe")
endif()

set(_local_skateboard_spatial_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "skate3::trick_pipeline::ObserveLocalSkateboardSpatialState\\(ctx, base\\);")
    set(_local_skateboard_spatial_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82D76D20\\)")
    continue()
  endif()
  set(_local_skateboard_spatial_site
"DEFINE_REX_FUNC(sub_82D76D20) {
	REX_FUNC_PROLOGUE();")
  set(_local_skateboard_spatial_patch
"DEFINE_REX_FUNC(sub_82D76D20) {
	REX_FUNC_PROLOGUE();
	skate3::trick_pipeline::ObserveLocalSkateboardSpatialState(ctx, base);")
  string(REPLACE
    "${_local_skateboard_spatial_site}"
    "${_local_skateboard_spatial_patch}"
    _contents
    "${_contents}")
  _skate3_add_include(_contents "skate3_trick_pipeline.h")
  file(WRITE "${_file}" "${_contents}")
  set(_local_skateboard_spatial_patched TRUE)
  message(STATUS
    "Applied Skate 3 local skateboard spatial observer in ${_file}")
  break()
endforeach()
if(NOT _local_skateboard_spatial_patched)
  message(FATAL_ERROR
    "Failed to apply Skate 3 local skateboard spatial observer")
endif()

set(_owned_world_collision_bridge_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES
     "OwnedWorldCollisionBridgeScope owned_world_collision_bridge")
    set(_owned_world_collision_bridge_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES
     "skate3::trick_pipeline::ObserveLocalSkateboardSpatialState\\(ctx, base\\);")
    continue()
  endif()
  set(_owned_world_collision_bridge_site
"	skate3::trick_pipeline::ObserveLocalSkateboardSpatialState(ctx, base);")
  set(_owned_world_collision_bridge_patch
"	skate3::trick_pipeline::OwnedWorldCollisionBridgeScope owned_world_collision_bridge(ctx, base);
	skate3::trick_pipeline::ObserveLocalSkateboardSpatialState(ctx, base);")
  string(REPLACE
    "${_owned_world_collision_bridge_site}"
    "${_owned_world_collision_bridge_patch}"
    _contents
    "${_contents}")
  file(WRITE "${_file}" "${_contents}")
  set(_owned_world_collision_bridge_patched TRUE)
  message(STATUS
    "Applied Skate 3 owned-world collision bridge in ${_file}")
  break()
endforeach()
if(NOT _owned_world_collision_bridge_patched)
  message(FATAL_ERROR
    "Failed to apply Skate 3 owned-world collision bridge")
endif()

# CharacterGesture indexes a compiled table of 37 B_GSTR_* lifecycle
# descriptors. DLC gesture rows are appended after those retail entries, so
# redirect only the first appended address to a known retail lifecycle. The
# numeric index remains 37 and independently selects the custom VLT/ABIN row.
function(_skate3_patch_cac_gesture_entry _symbol _statement _marker)
  set(_patched FALSE)
  foreach(_file IN LISTS _skate3_recomp_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "${_marker}")
      set(_patched TRUE)
      break()
    endif()
    if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(${_symbol}\\)")
      continue()
    endif()
    set(_site
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();")
    set(_patch
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();
	${_statement}")
    string(REPLACE "${_site}" "${_patch}" _patched_contents "${_contents}")
    if(_patched_contents STREQUAL _contents)
      continue()
    endif()
    set(_contents "${_patched_contents}")
    _skate3_add_include(_contents "skate3_cac_gesture.h")
    file(WRITE "${_file}" "${_contents}")
    set(_patched TRUE)
    message(STATUS
      "Applied Skate 3 CAC gesture observer to ${_symbol} in ${_file}")
    break()
  endforeach()
  if(NOT _patched)
    message(FATAL_ERROR
      "Failed to apply Skate 3 CAC gesture observer to ${_symbol}")
  endif()
endfunction()

_skate3_patch_cac_gesture_entry(
  sub_82532E58
  "skate3::cac_gesture::ObserveLoadBaseAnims(ctx, base);"
  "ObserveLoadBaseAnims\\(ctx, base\\)")
_skate3_patch_cac_gesture_entry(
  sub_82532F60
  "skate3::cac_gesture::ObservePendingGestureUpdate(ctx, base);"
  "ObservePendingGestureUpdate\\(ctx, base\\)")
_skate3_patch_cac_gesture_entry(
  sub_82533278
  "skate3::cac_gesture::ObserveLoadGestureById(ctx, base);"
  "ObserveLoadGestureById\\(ctx, base\\)")
_skate3_patch_cac_gesture_entry(
  sub_82533380
  "skate3::cac_gesture::ObserveLoadGestureByName(ctx, base);"
  "ObserveLoadGestureByName\\(ctx, base\\)")
_skate3_patch_cac_gesture_entry(
  sub_8258FB48
  "skate3::cac_gesture::ObservePendingGestureGetter(ctx, base);"
  "ObservePendingGestureGetter\\(ctx, base\\)")

function(_skate3_patch_dlc_runtime_entry _symbol _statement _marker)
  set(_patched FALSE)
  foreach(_file IN LISTS _skate3_recomp_files)
    file(READ "${_file}" _contents)
    if(_contents MATCHES "${_marker}")
      set(_patched TRUE)
      break()
    endif()
    if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(${_symbol}\\)")
      continue()
    endif()
    set(_site
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();")
    set(_patch
"DEFINE_REX_FUNC(${_symbol}) {
	REX_FUNC_PROLOGUE();
	${_statement}")
    string(REPLACE "${_site}" "${_patch}" _patched_contents "${_contents}")
    if(_patched_contents STREQUAL _contents)
      continue()
    endif()
    set(_contents "${_patched_contents}")
    _skate3_add_include(_contents "skate3_dlc_runtime.h")
    file(WRITE "${_file}" "${_contents}")
    set(_patched TRUE)
    message(STATUS
      "Applied Skate 3 DLC runtime observer to ${_symbol} in ${_file}")
    break()
  endforeach()
  if(NOT _patched)
    message(FATAL_ERROR
      "Failed to apply Skate 3 DLC runtime observer to ${_symbol}")
  endif()
endfunction()

_skate3_patch_dlc_runtime_entry(
  sub_82580980
  "skate3::dlc_runtime::ObserveManagerConstructor(ctx);"
  "ObserveManagerConstructor\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82580E68
  "skate3::dlc_runtime::ObserveManagerRun(ctx);"
  "ObserveManagerRun\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_825810B0
  "skate3::dlc_runtime::ObserveManagerEnumerate(ctx);"
  "ObserveManagerEnumerate\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82581698
  "skate3::dlc_runtime::ObserveManagerRefresh(ctx);"
  "ObserveManagerRefresh\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82581918
  "skate3::dlc_runtime::ObserveDriverMount(ctx);"
  "ObserveDriverMount\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82582410
  "skate3::dlc_runtime::ObserveContentManagerEnumerate(ctx, base);"
  "ObserveContentManagerEnumerate\\(ctx, base\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82A83080
  "skate3::dlc_runtime::ObserveAddArchiveFromFile(ctx, base);"
  "ObserveAddArchiveFromFile\\(ctx, base\\)")
_skate3_patch_dlc_runtime_entry(
  sub_8298F430
  "skate3::dlc_runtime::ObserveAsyncFileOpen(ctx, base);"
  "ObserveAsyncFileOpen\\(ctx, base\\)")
_skate3_patch_dlc_runtime_entry(
  sub_828D9378
  "skate3::dlc_runtime::ObserveVaultDlcLoadUpdate(ctx, base);"
  "ObserveVaultDlcLoadUpdate\\(ctx, base\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82B69B08
  "skate3::dlc_runtime::ObserveFindCollection(ctx);"
  "ObserveFindCollection\\(ctx\\)")
_skate3_patch_dlc_runtime_entry(
  sub_82D085C0
  "skate3::dlc_runtime::ObserveLanguageDlcLoadUpdate(ctx);"
  "ObserveLanguageDlcLoadUpdate\\(ctx\\)")

# Intercept the fixed gesture-intent descriptor lookup so the runtime can
# replace Air Guitar's intent name after the custom DLC has mounted.
set(_cac_gesture_descriptor_alias_patched FALSE)
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ResolveGestureDescriptorAddress")
    set(_cac_gesture_descriptor_alias_patched TRUE)
    break()
  endif()
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_82BAB080\\)")
    continue()
  endif()
  set(_cac_gesture_descriptor_original
"	// add r11,r10,r11
	ctx.r11.u64 = ctx.r10.u64 + ctx.r11.u64;
	// addi r10,r11,4")
  set(_cac_gesture_descriptor_alias
"	// add r11,r10,r11
	ctx.r11.u64 = ctx.r10.u64 + ctx.r11.u64;
	ctx.r11.u64 = skate3::cac_gesture::ResolveGestureDescriptorAddress(
	    ctx, base, ctx.r11.u32);
	// addi r10,r11,4")
  string(REPLACE "${_cac_gesture_descriptor_original}"
    "${_cac_gesture_descriptor_alias}" _patched_contents "${_contents}")
  if(_patched_contents STREQUAL _contents)
    continue()
  endif()
  set(_contents "${_patched_contents}")
  _skate3_add_include(_contents "skate3_cac_gesture.h")
  file(WRITE "${_file}" "${_contents}")
  set(_cac_gesture_descriptor_alias_patched TRUE)
  message(STATUS
    "Applied Skate 3 CAC gesture descriptor override in ${_file}")
  break()
endforeach()
if(NOT _cac_gesture_descriptor_alias_patched)
  message(FATAL_ERROR
    "Failed to apply the CAC gesture descriptor override")
endif()

# Retire the earlier slot-37 experiment. These values are no-gesture sentinels,
# not a registration count, and changing them cannot select a cac_body row.
foreach(_file IN LISTS _skate3_recomp_files)
  file(READ "${_file}" _contents)
  if(NOT _contents MATCHES "DEFINE_REX_FUNC\\(sub_825953B0\\)")
    continue()
  endif()
  foreach(_register IN ITEMS r4 r8 r9)
    string(REPLACE
      "	// li ${_register},38\n	ctx.${_register}.s64 = 38;"
      "	// li ${_register},37\n	ctx.${_register}.s64 = 37;"
      _contents
      "${_contents}")
  endforeach()
  file(WRITE "${_file}" "${_contents}")
  break()
endforeach()
