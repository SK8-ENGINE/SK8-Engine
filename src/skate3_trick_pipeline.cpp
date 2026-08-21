#include "skate3_trick_pipeline.h"

#include "skate3_mechanics_sandbox.h"

#include "generated/skate3_init.h"
#include "skate3_cac_gesture.h"
#include "skate3_custom_trick.h"
#include "skate3_input_history_watch.h"
#include "skate3_input_lab.h"
#include "skate3_trick_overrides.h"
#include "skate3_trick_types.h"
#include "generated/skate3_blender_pose_override.h"
#include "generated/skate3_gesture_pose_bridge.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

REXCVAR_DEFINE_BOOL(
    skate3_custom_input_ls_rs, false, "Skate 3",
    "Experimental: publish CODEX_CORKSCREW as a new action-graph input "
    "token on a simultaneous LS+RS press; no custom trick consumer yet");
REXCVAR_DEFINE_BOOL(
    skate3_custom_input_outer_circle, false, "Skate 3",
    "Enable the in-game right-stick clockwise outer-rim 360 recognizer and "
    "publish the selected custom trick through ActionGraph inputs");
REXCVAR_DEFINE_BOOL(
    skate3_custom_trick_native_graph, false, "Skate 3",
    "Load the project-owned MotionGraph_OnBoard_Codex.xml and route the "
    "outer-circle gesture through its disjoint custom trick state");
REXCVAR_DEFINE_BOOL(
    skate3_outer_circle_retail_360flip_control, false, "Skate 3",
    "Research control: resolve the completed outer-circle gesture to the "
    "retail 360Flip descriptor while preserving the retail trick state");
REXCVAR_DEFINE_DOUBLE(
    skate3_360flip_matched_test_height, -1.0, "Skate 3",
    "Research only: when non-negative and the retail 360Flip control is "
    "enabled, replace SetTrickHeight's computed output with this fixed value");
REXCVAR_DEFINE_BOOL(
    skate3_cac_gesture_fullbody_bridge, false, "Skate 3",
    "Replace the decoded Air Guitar body pose with the Blender-authored "
    "full-body gesture while retaining the retail gesture lifecycle");
REXCVAR_DEFINE_BOOL(
    skate3_cac_gesture_fullbody_asset, false, "Skate 3",
    "Resolve Air Guitar body phases to the configured native-VBR "
    "CODEX_GUNPLAY_FULLBODY asset");
REXCVAR_DEFINE_BOOL(
    skate3_cac_gesture_suppress_airguitar_body, false, "Skate 3",
    "Suppress the compact B_GSTR Air Guitar body intent so the registered "
    "131-bone CAC gesture owns the visible body pose");

namespace skate3::trick_pipeline {
void OverrideTrickHeightForMatchedExperiment(PPCContext &ctx) {
  const double configured = REXCVAR_GET(skate3_360flip_matched_test_height);
  if (!REXCVAR_GET(skate3_outer_circle_retail_360flip_control) ||
      configured < 0.0) {
    return;
  }
  ctx.f1.f64 =
      static_cast<double>(static_cast<float>(std::clamp(configured, 0.0, 1.0)));
}

namespace {
thread_local uint32_t g_active_focused_skeleton_ik = 0;
thread_local std::string g_active_gesture_stream;
thread_local uint32_t g_active_gesture_clip = 0;

constexpr size_t kTrickIntentWordCount = trick::TrickIntents::kWordCount;
constexpr size_t kAnimationIntentWordCount =
    trick::TrickIntentDescriptor::kWordCount;
constexpr size_t kFocusedEventCapacity = 512;
constexpr size_t kLocalSpatialSampleCapacity = 512;
constexpr uint64_t kLocalSpatialSampleIntervalFrames = 1;

struct ArmPose {
  std::array<float, 4> upper;
  std::array<float, 4> forearm;
};

constexpr std::array<ArmPose, 13> kBlenderGroundArmPoses{{
    ArmPose{{-0.173369f, 0.239687f, -0.651613f, 0.699019f},
     {-0.159082f, 0.159136f, 0.351347f, 0.911542f}},
    ArmPose{{-0.177063f, 0.246453f, -0.641856f, 0.702630f},
     {-0.158554f, 0.158607f, 0.350180f, 0.908515f}},
    ArmPose{{-0.183408f, 0.261891f, -0.625415f, 0.709705f},
     {-0.159035f, 0.159089f, 0.351242f, 0.911272f}},
    ArmPose{{-0.188642f, 0.281861f, -0.604828f, 0.718852f},
     {-0.158879f, 0.158932f, 0.350896f, 0.910374f}},
    ArmPose{{-0.187931f, 0.294735f, -0.584043f, 0.731432f},
     {-0.158557f, 0.158610f, 0.350186f, 0.908531f}},
    ArmPose{{-0.179444f, 0.290904f, -0.568265f, 0.750492f},
     {-0.158678f, 0.158731f, 0.350453f, 0.909224f}},
    ArmPose{{-0.165276f, 0.270871f, -0.555031f, 0.766762f},
     {-0.158351f, 0.158404f, 0.349731f, 0.907351f}},
    ArmPose{{-0.155590f, 0.254342f, -0.551364f, 0.781896f},
     {-0.158694f, 0.158747f, 0.350489f, 0.909318f}},
    ArmPose{{-0.131039f, 0.218009f, -0.539550f, 0.801227f},
     {-0.158849f, 0.158902f, 0.350831f, 0.910205f}},
    ArmPose{{-0.116779f, 0.196444f, -0.530781f, 0.817300f},
     {-0.158418f, 0.158471f, 0.349879f, 0.907734f}},
    ArmPose{{-0.095292f, 0.163430f, -0.515319f, 0.832787f},
     {-0.158939f, 0.158993f, 0.351031f, 0.910723f}},
    ArmPose{{-0.077427f, 0.135468f, -0.505007f, 0.844453f},
     {-0.159058f, 0.159112f, 0.351293f, 0.911404f}},
    ArmPose{{-0.068326f, 0.120977f, -0.504335f, 0.853978f},
     {-0.158906f, 0.158959f, 0.350957f, 0.910531f}},
}};

constexpr std::array<ArmPose, 28> kBlenderAirArmPoses{{
    ArmPose{{-0.064232f, 0.116900f, -0.499607f, 0.854752f},
     {-0.130953f, 0.513746f, -0.224140f, 0.818798f}},
    ArmPose{{-0.064395f, 0.117198f, -0.500879f, 0.856928f},
     {-0.130802f, 0.513155f, -0.223882f, 0.817855f}},
    ArmPose{{-0.064430f, 0.117261f, -0.501151f, 0.857392f},
     {-0.130700f, 0.512753f, -0.223707f, 0.817215f}},
    ArmPose{{-0.064056f, 0.116581f, -0.498243f, 0.852419f},
     {-0.130615f, 0.512421f, -0.223562f, 0.816687f}},
    ArmPose{{-0.064187f, 0.116818f, -0.499257f, 0.854152f},
     {-0.130583f, 0.512296f, -0.223507f, 0.816487f}},
    ArmPose{{-0.064360f, 0.117133f, -0.500602f, 0.856453f},
     {-0.130663f, 0.512609f, -0.223644f, 0.816985f}},
    ArmPose{{-0.064130f, 0.116715f, -0.498816f, 0.853398f},
     {-0.130860f, 0.513380f, -0.223980f, 0.818214f}},
    ArmPose{{-0.064262f, 0.116955f, -0.499840f, 0.855150f},
     {-0.131112f, 0.514369f, -0.224412f, 0.819792f}},
    ArmPose{{-0.064319f, 0.117059f, -0.500285f, 0.855912f},
     {-0.131220f, 0.514792f, -0.224597f, 0.820466f}},
    ArmPose{{-0.064310f, 0.117043f, -0.500218f, 0.855797f},
     {-0.130505f, 0.511989f, -0.223374f, 0.815998f}},
    ArmPose{{-0.064313f, 0.117049f, -0.500241f, 0.855837f},
     {-0.130498f, 0.511961f, -0.223362f, 0.815954f}},
    ArmPose{{-0.064311f, 0.117044f, -0.500223f, 0.855806f},
     {-0.131035f, 0.514068f, -0.224281f, 0.819311f}},
    ArmPose{{-0.064305f, 0.117034f, -0.500178f, 0.855728f},
     {-0.131175f, 0.514617f, -0.224520f, 0.820186f}},
    ArmPose{{-0.064342f, 0.117101f, -0.500466f, 0.856221f},
     {-0.130956f, 0.513757f, -0.224145f, 0.818816f}},
    ArmPose{{-0.064440f, 0.117279f, -0.501225f, 0.857520f},
     {-0.130986f, 0.513877f, -0.224197f, 0.819006f}},
    ArmPose{{-0.064535f, 0.117452f, -0.501965f, 0.858785f},
     {-0.131262f, 0.514959f, -0.224669f, 0.820732f}},
    ArmPose{{-0.064341f, 0.117100f, -0.500460f, 0.856210f},
     {-0.131003f, 0.513943f, -0.224226f, 0.819112f}},
    ArmPose{{-0.064221f, 0.116881f, -0.499527f, 0.854615f},
     {-0.130655f, 0.512577f, -0.223630f, 0.816936f}},
    ArmPose{{-0.064143f, 0.116739f, -0.498919f, 0.853574f},
     {-0.130381f, 0.511501f, -0.223160f, 0.815219f}},
    ArmPose{{-0.064223f, 0.116884f, -0.499538f, 0.854633f},
     {-0.130446f, 0.511757f, -0.223272f, 0.815629f}},
    ArmPose{{-0.064412f, 0.117228f, -0.501007f, 0.857146f},
     {-0.130728f, 0.512861f, -0.223754f, 0.817388f}},
    ArmPose{{-0.064441f, 0.117282f, -0.501237f, 0.857540f},
     {-0.130967f, 0.513800f, -0.224163f, 0.818883f}},
    ArmPose{{-0.064241f, 0.116918f, -0.499682f, 0.854879f},
     {-0.131077f, 0.514233f, -0.224352f, 0.819574f}},
    ArmPose{{-0.064077f, 0.116619f, -0.498406f, 0.852696f},
     {-0.131122f, 0.514410f, -0.224430f, 0.819856f}},
    ArmPose{{-0.064525f, 0.117434f, -0.501889f, 0.858656f},
     {-0.130924f, 0.513630f, -0.224090f, 0.818614f}},
    ArmPose{{-0.064230f, 0.116897f, -0.499593f, 0.854728f},
     {-0.130833f, 0.513277f, -0.223935f, 0.818050f}},
    ArmPose{{-0.064126f, 0.116708f, -0.498785f, 0.853345f},
     {-0.130840f, 0.513304f, -0.223947f, 0.818094f}},
    ArmPose{{-0.064207f, 0.116855f, -0.499416f, 0.854424f},
     {-0.130908f, 0.513568f, -0.224062f, 0.818514f}},
}};

struct LocalSpatialSample {
  uint64_t frame;
  uint32_t position_x_bits;
  uint32_t position_y_bits;
  uint32_t position_z_bits;
  uint32_t x_axis_x_bits;
  uint32_t x_axis_y_bits;
  uint32_t x_axis_z_bits;
  uint32_t z_axis_x_bits;
  uint32_t z_axis_y_bits;
  uint32_t z_axis_z_bits;
};

struct ActorSpatialSnapshot {
  uint64_t frame;
  uint32_t controller;
  uint32_t position_x_bits;
  uint32_t position_y_bits;
  uint32_t position_z_bits;
};

enum class EventKind : size_t {
  ActionGraphIntentInsert,
  CustomInputTokenInsert,
  GestureLookup,
  GestureMatch,
  GestureInjection,
  GestureReplacement,
  GestureDisable,
  CreateIntent,
  ScoreUpdate,
  AirAnalysis,
  AirStart,
  ScorableStart,
  ScorableOverride,
  AirEnd,
  AirUpdate,
  ScoreHolderRecord,
  ScoreHolderCancel,
  ScoreHolderEndAir,
  ScoreHolderRewardAirSequence,
  ScoreHolderPublishAirSequence,
  GrindCollectorExit,
  ScoreCollectorTransition,
  AnimationCurrent,
  AnimationCompleted,
  TrickDisplayRefresh,
  AnimationCurrentResult,
  AnimationCompletedResult,
  AnimationOverride,
  AnimationEvalCommandBuffer,
  WipeoutRequested,
  CollisionForceWipeout,
  PhysicsWantsWipeout,
  CanLandOnBoard,
  TiltTooLargeForPreland,
  IsLandingOnBoard,
  IsLanding,
  IsOffboardCondition,
  SkeletonAnimAttributes,
  BlendPoseStream,
  AnimationTreeLookup,
  PushAnimationAttributes,
  SkeletonIkBlend,
  SkateboardOffset,
  Count,
};

std::atomic<bool> g_armed{false};
std::atomic<bool> g_focused{false};
std::array<std::atomic<uint64_t>, static_cast<size_t>(EventKind::Count)>
    g_counts{};
std::mutex g_event_mutex;
std::vector<std::string> g_focused_events;
std::vector<std::string> g_score_reward_events;
std::vector<std::string> g_local_score_collector_events;
std::vector<LocalSpatialSample> g_local_spatial_samples;
std::map<uint32_t, ActorSpatialSnapshot> g_actor_spatial_snapshots;
std::map<std::string, std::string> g_last_signatures;
std::atomic<bool> g_mapping_initialized{false};
std::atomic<bool> g_mapping_init_active{false};
std::atomic<uint32_t> g_mapping_address{0};
std::array<std::atomic<uint32_t>, trick::GestureTrickMappingLayout::kGroupCount>
    g_mapping_table_counts{};
std::array<std::atomic<uint32_t>, trick::GestureTrickMappingLayout::kGroupCount>
    g_mapping_bucket_counts{};
std::map<std::array<uint32_t, trick::TrickIntentDescriptor::kWordCount>,
         std::string>
    g_mapping_fast_string_names;
std::atomic<uint64_t> g_replacement_count{0};
std::atomic<uint64_t> g_disable_count{0};
std::atomic<uint64_t> g_scorable_override_count{0};
std::atomic<uint64_t> g_display_name_override_count{0};
std::atomic<uint64_t> g_animation_override_count{0};
std::atomic<uint64_t> g_custom_input_token_insert_count{0};
std::atomic<uint64_t> g_outer_circle_intent_injection_count{0};
std::atomic<uint64_t> g_outer_circle_intent_pending_frame{0};
std::atomic<uint64_t> g_outer_circle_last_injection_frame{0};
std::atomic<uint64_t> g_outer_circle_custom_hold_until_frame{0};
std::atomic<uint32_t> g_outer_circle_intent_pending_intents{0};
std::atomic<uint32_t> g_anim_attribute_focus_skeleton{0};
std::atomic<uint32_t> g_local_action_graph_intents{0};
std::atomic<uint32_t> g_local_phys_out{0};
std::atomic<uint32_t> g_local_phys_out_actor{0};
std::atomic<uint32_t> g_local_phys_out_root_offset{0xFFFFFFFFu};
std::atomic<uint32_t> g_local_phys_out_child_offset{0xFFFFFFFFu};
std::atomic<uint32_t> g_local_phys_out_component_offset{0xFFFFFFFFu};
std::atomic<uint32_t> g_local_phys_out_interface_id{0};
std::atomic<uint32_t> g_local_score_module{0};
std::atomic<uint32_t> g_local_score_holder{0};
std::atomic<uint32_t> g_pending_custom_air_collector{0};
std::atomic<uint64_t> g_custom_scorable_publish_call_count{0};
std::atomic<uint64_t> g_custom_scorable_publish_local_count{0};
std::atomic<uint64_t> g_custom_scorable_publish_accept_count{0};
std::atomic<uint64_t> g_custom_scorable_publish_last_frame{0};
std::atomic<uint32_t> g_custom_scorable_publish_last_module{0};
std::atomic<uint32_t> g_custom_scorable_publish_last_phys_out{0};
std::atomic<uint32_t> g_custom_scorable_publish_last_output{0};
std::atomic<uint32_t> g_custom_scorable_publish_last_before{0};
std::atomic<uint32_t> g_custom_scorable_publish_last_after{0};
thread_local uint32_t g_air_collector_update_custom_id = 0;
std::atomic<uint64_t> g_local_score_collector_transition_event_count{0};
std::atomic<uint64_t> g_local_score_grind_exit_event_count{0};
// Sequence lock for the board transform below. Odd means the sim thread is
// publishing a new sample; even means every field belongs to one coherent
// sample. Individual atomics prevent data races but do not prevent a reader
// from combining position/axes from adjacent simulation updates.
std::atomic<uint64_t> g_local_spatial_revision{0};
std::atomic<uint64_t> g_local_spatial_frame{0};
std::atomic<uint64_t> g_local_spatial_sample_time_us{0};
std::atomic<uint32_t> g_local_board_controller{0};
std::atomic<uint32_t> g_local_board_body{0};
std::atomic<uint32_t> g_local_board_transform_state{0};
std::atomic<uint32_t> g_local_board_position_x_bits{0};
std::atomic<uint32_t> g_local_board_position_y_bits{0};
std::atomic<uint32_t> g_local_board_position_z_bits{0};
std::atomic<uint32_t> g_local_board_x_axis_x_bits{0};
std::atomic<uint32_t> g_local_board_x_axis_y_bits{0};
std::atomic<uint32_t> g_local_board_x_axis_z_bits{0};
std::atomic<uint32_t> g_local_board_z_axis_x_bits{0};
std::atomic<uint32_t> g_local_board_z_axis_y_bits{0};
std::atomic<uint32_t> g_local_board_z_axis_z_bits{0};
std::atomic<uint64_t> g_animation_loader_add_count{0};
std::atomic<uint64_t> g_animation_loader_lookup_count{0};
std::atomic<uint64_t> g_animation_loader_load_count{0};
std::atomic<uint64_t> g_animation_loader_completion_count{0};
std::atomic<uint64_t> g_playback_data_construction_count{0};
std::atomic<uint64_t> g_playback_data_lookup_count{0};
std::atomic<uint64_t> g_andale_database_load_count{0};
std::atomic<uint64_t> g_andale_database_content_count{0};
std::atomic<uint64_t> g_custom_animation_asset_load_attempt_count{0};
std::atomic<uint64_t> g_custom_animation_asset_load_success_count{0};
std::atomic<uint64_t> g_custom_animation_asset_stream_eval_count{0};
std::atomic<uint64_t> g_animation_leaf_replacement_bind_count{0};
std::atomic<uint64_t> g_animation_leaf_replacement_eval_count{0};
std::atomic<uint64_t> g_wipeout_request_check_count{0};
std::atomic<uint64_t> g_wipeout_requested_true_count{0};
std::atomic<uint32_t> g_last_wipeout_request_player{0};
std::atomic<uint64_t> g_collision_force_wipeout_check_count{0};
std::atomic<uint64_t> g_collision_force_wipeout_true_count{0};
std::atomic<uint32_t> g_collision_force_wipeout_last_skeleton{0};
std::atomic<uint32_t> g_collision_force_wipeout_last_argument_1_bits{0};
std::atomic<uint32_t> g_collision_force_wipeout_last_argument_2_bits{0};
std::atomic<uint64_t> g_physics_wants_wipeout_factory_count{0};
std::atomic<uint32_t> g_physics_wants_wipeout_object{0};
std::atomic<uint32_t> g_physics_wants_wipeout_vtable{0};
constexpr size_t kPhysicsWantsWipeoutVtableSlotCount = 16;
std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
    g_physics_wants_wipeout_vtable_slots{};
std::atomic<uint64_t> g_physics_wants_wipeout_check_count{0};
std::atomic<uint64_t> g_physics_wants_wipeout_true_count{0};
std::atomic<uint64_t> g_local_physics_wants_wipeout_true_count{0};
std::atomic<uint32_t> g_local_action_graph_listener{0};
std::atomic<uint32_t> g_local_action_graph_actor{0};
std::atomic<uint32_t> g_local_action_graph_skater_anim{0};

std::atomic<uint32_t> g_physics_wants_wipeout_last_context{0};
std::atomic<uint32_t> g_physics_wants_wipeout_last_actor{0};
std::atomic<uint32_t> g_physics_wants_wipeout_last_skater_anim{0};
std::atomic<uint64_t> g_physics_wants_wipeout_last_true_frame{0};
std::atomic<uint32_t> g_physics_wants_wipeout_last_true_context{0};
std::atomic<uint32_t> g_physics_wants_wipeout_last_true_actor{0};
std::atomic<uint32_t> g_physics_wants_wipeout_last_true_skater_anim{0};
std::atomic<uint64_t> g_can_land_on_board_factory_count{0};
std::atomic<uint32_t> g_can_land_on_board_object{0};
std::atomic<uint32_t> g_can_land_on_board_vtable{0};
std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
    g_can_land_on_board_vtable_slots{};
std::atomic<uint64_t> g_tilt_too_large_for_preland_factory_count{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_object{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_vtable{0};
std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
    g_tilt_too_large_for_preland_vtable_slots{};
std::atomic<uint64_t> g_is_landing_on_board_factory_count{0};
std::atomic<uint32_t> g_is_landing_on_board_object{0};
std::atomic<uint32_t> g_is_landing_on_board_vtable{0};
std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
    g_is_landing_on_board_vtable_slots{};
std::atomic<uint64_t> g_is_landing_factory_count{0};
std::atomic<uint32_t> g_is_landing_object{0};
std::atomic<uint32_t> g_is_landing_vtable{0};
std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
    g_is_landing_vtable_slots{};
std::atomic<uint64_t> g_can_land_on_board_check_count{0};
std::atomic<uint64_t> g_can_land_on_board_true_count{0};
std::atomic<uint64_t> g_local_can_land_on_board_check_count{0};
std::atomic<uint64_t> g_local_can_land_on_board_true_count{0};
std::atomic<uint64_t> g_can_land_on_board_last_frame{0};
std::atomic<uint32_t> g_can_land_on_board_last_context{0};
std::atomic<uint32_t> g_can_land_on_board_last_actor{0};
std::atomic<uint32_t> g_can_land_on_board_last_result{0};
std::atomic<uint64_t> g_tilt_too_large_for_preland_check_count{0};
std::atomic<uint64_t> g_tilt_too_large_for_preland_true_count{0};
std::atomic<uint64_t> g_local_tilt_too_large_for_preland_check_count{0};
std::atomic<uint64_t> g_local_tilt_too_large_for_preland_true_count{0};
std::atomic<uint64_t> g_tilt_too_large_for_preland_last_frame{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_last_context{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_last_actor{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_last_interface{0};
std::atomic<uint32_t> g_tilt_too_large_for_preland_last_result{0};
std::atomic<uint64_t> g_is_landing_on_board_check_count{0};
std::atomic<uint64_t> g_is_landing_on_board_true_count{0};
std::atomic<uint64_t> g_local_is_landing_on_board_check_count{0};
std::atomic<uint64_t> g_local_is_landing_on_board_true_count{0};
std::atomic<uint64_t> g_is_landing_on_board_last_frame{0};
std::atomic<uint32_t> g_is_landing_on_board_last_actor{0};
std::atomic<uint32_t> g_is_landing_on_board_last_result{0};
std::array<std::atomic<uint64_t>, 2> g_is_landing_check_counts{};
std::array<std::atomic<uint64_t>, 2> g_is_landing_true_counts{};
std::array<std::atomic<uint64_t>, 2> g_local_is_landing_check_counts{};
std::array<std::atomic<uint64_t>, 2> g_local_is_landing_true_counts{};
std::array<std::atomic<uint64_t>, 2> g_is_landing_last_frames{};
std::array<std::atomic<uint32_t>, 2> g_is_landing_last_actors{};
std::array<std::atomic<uint32_t>, 2> g_is_landing_last_interfaces{};
std::array<std::atomic<uint32_t>, 2> g_is_landing_last_results{};
std::atomic<uint64_t> g_is_offboard_condition_factory_count{0};
std::atomic<uint32_t> g_is_offboard_condition_object{0};
std::atomic<uint32_t> g_is_offboard_condition_vtable{0};
constexpr size_t kIsOffboardVtableSlotCount = 16;
std::array<std::atomic<uint32_t>, kIsOffboardVtableSlotCount>
    g_is_offboard_condition_vtable_slots{};
std::atomic<uint64_t> g_is_air_offboard_condition_factory_count{0};
std::atomic<uint32_t> g_is_air_offboard_condition_object{0};
std::atomic<uint32_t> g_is_air_offboard_condition_vtable{0};
std::array<std::atomic<uint32_t>, kIsOffboardVtableSlotCount>
    g_is_air_offboard_condition_vtable_slots{};
std::atomic<uint64_t> g_is_offboard_condition_check_count{0};
std::atomic<uint64_t> g_is_offboard_condition_true_count{0};
std::atomic<uint32_t> g_is_offboard_condition_last_result{0};
std::atomic<uint32_t> g_is_offboard_condition_last_context{0};
std::atomic<uint32_t> g_is_offboard_condition_last_owner{0};
std::atomic<uint32_t> g_is_offboard_condition_last_provider{0};
std::atomic<uint64_t> g_is_air_offboard_condition_check_count{0};
std::atomic<uint64_t> g_is_air_offboard_condition_true_count{0};
std::atomic<uint32_t> g_is_air_offboard_condition_last_result{0};
std::atomic<uint32_t> g_offboard_provider_vtable{0};
std::atomic<uint32_t> g_offboard_provider_method_608{0};
std::atomic<uint32_t> g_offboard_provider_method_612{0};
std::atomic<uint32_t> g_offboard_provider_method_616{0};
std::atomic<uint64_t> g_board_state_sample_count{0};
std::atomic<uint64_t> g_board_state_offboard_count{0};
std::atomic<uint32_t> g_board_state_last_packed{0xFFFFFFFFu};
std::atomic<uint64_t> g_board_state_last_frame{0};
std::atomic<uint32_t> g_andale_database_manager{0};
std::atomic<uint32_t> g_andale_database_allocator{0};
std::atomic<uint32_t> g_andale_database_slot{0};
std::atomic<uint32_t> g_andale_onboard_database{0};

struct BoundAnimationLeafReplacement {
  std::string name;
  std::string asset;
  std::string source_animation;
  std::string target_animation;
  uint32_t source_clip{};
  uint32_t target_clip{};
  uint32_t allocation_size{};
};

std::mutex g_animation_asset_mutex;
std::mutex g_gesture_stream_mutex;
std::map<uint32_t, std::string> g_gesture_streams;
std::map<std::string, std::string> g_animation_loader_registrations;
std::map<std::string, uint32_t> g_animation_loader_lookup_results;
std::map<std::string, std::string> g_animation_loader_loads;
std::map<std::string, uint32_t> g_animation_loader_async_paths;
std::map<std::string, std::string> g_animation_loader_completions;
std::map<std::string, std::string> g_playback_data_constructions;
std::map<std::string, std::string> g_playback_data_lookup_results;
std::map<std::string, std::string> g_andale_database_loads;
std::map<std::string, std::string> g_andale_database_contents;
std::map<std::string, uint32_t> g_latest_playback_data_instances;
std::mutex g_custom_animation_asset_load_mutex;
std::map<std::string, bool> g_custom_animation_asset_loaded;
std::map<std::string, std::string> g_custom_animation_asset_results;
std::map<std::string, uint32_t> g_custom_animation_asset_databases;
std::map<std::string, uint32_t> g_custom_animation_asset_clips;
std::map<std::string, std::string> g_custom_animation_asset_eval_results;
std::vector<BoundAnimationLeafReplacement>
    g_bound_animation_leaf_replacements;
std::map<std::string, std::string> g_animation_leaf_replacement_results;
thread_local uint32_t g_active_gesture_group = 0xFFFFFFFF;
thread_local std::string g_active_custom_animation_asset_rule;
thread_local std::string g_active_custom_animation_asset_path;
thread_local std::string g_active_custom_animation_eval_rule;
std::atomic<uint32_t> g_vbr_extract_observation_count{0};

struct ActiveActionGraphInputFill {
  uint32_t depth{};
  uint32_t listener{};
  uint32_t actor{};
  uint32_t intents{};
};

thread_local ActiveActionGraphInputFill g_active_action_graph_input_fill;

struct ActiveGestureMatch {
  bool matched{};
  std::string key_name;
  std::string primary_name;
  std::string secondary_name;
};

thread_local ActiveGestureMatch g_active_gesture_match;

uint32_t LoadU32(uint8_t *base, uint32_t address) {
  if (!base || !address) {
    return 0;
  }
  return REX_LOAD_U32(address);
}

uint8_t LoadU8(uint8_t *base, uint32_t address) {
  if (!base || !address) {
    return 0;
  }
  return REX_LOAD_U8(address);
}

std::array<float, 4> InterpolateQuaternion(
    const std::array<float, 4> &from, const std::array<float, 4> &to,
    float alpha) {
  float dot = 0.0f;
  for (size_t index = 0; index < 4; ++index) {
    dot += from[index] * to[index];
  }
  std::array<float, 4> result{};
  float length_squared = 0.0f;
  const float sign = dot < 0.0f ? -1.0f : 1.0f;
  for (size_t index = 0; index < 4; ++index) {
    result[index] =
        from[index] + (to[index] * sign - from[index]) * alpha;
    length_squared += result[index] * result[index];
  }
  if (length_squared > 0.000001f) {
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    for (float &component : result) {
      component *= inverse_length;
    }
  }
  return result;
}

void StoreQuaternion(uint8_t *base, uint32_t address,
                     const std::array<float, 4> &quaternion) {
  if (!base || !address) {
    return;
  }
  for (uint32_t component = 0; component < 4; ++component) {
    REX_STORE_U32(address + component * 4,
                  std::bit_cast<uint32_t>(quaternion[component]));
  }
}

template <size_t N>
std::array<uint32_t, N> LoadWords(uint8_t *base, uint32_t address) {
  std::array<uint32_t, N> words{};
  if (!base || !address) {
    return words;
  }
  for (size_t i = 0; i < N; ++i) {
    words[i] =
        REX_LOAD_U32(address + static_cast<uint32_t>(i * sizeof(uint32_t)));
  }
  return words;
}

void AppendHex(std::ostream &stream, uint32_t value) {
  stream << "0x" << std::hex << std::uppercase << value << std::dec;
}

std::string LoadToken(uint8_t *base, uint32_t address) {
  std::string token;
  if (!base || !address) {
    return token;
  }
  constexpr size_t kMaximumLength = 63;
  for (size_t index = 0; index < kMaximumLength; ++index) {
    const uint8_t value = LoadU8(base, address + static_cast<uint32_t>(index));
    if (value == 0) {
      break;
    }
    const char character = static_cast<char>(value);
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '_' || character == '-' || character == '.';
    token.push_back(safe ? character : '_');
  }
  return token;
}

std::string DecodeFastString(uint8_t *base, uint32_t address) {
  if (!base || !address) {
    return {};
  }
  constexpr uint32_t kInitialSeed = 79235168;
  constexpr std::string_view kCharacters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
  std::string result;
  result.reserve(36);
  for (uint32_t word_index = 0; word_index < 6; ++word_index) {
    uint32_t value = LoadU32(base, address + word_index * 4);
    uint32_t seed = kInitialSeed;
    while (value > 0 && seed > 0) {
      const uint32_t index = value / seed;
      if (index == 0 || index > kCharacters.size()) {
        break;
      }
      result.push_back(kCharacters[index - 1]);
      value %= seed;
      seed /= 38;
    }
  }
  return result;
}

std::string DecodeFastString30(uint8_t *base, uint32_t address) {
  if (!base || !address) {
    return {};
  }
  constexpr uint32_t kInitialSeed = 79235168;
  constexpr std::string_view kCharacters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
  std::string result;
  result.reserve(30);
  for (uint32_t word_index = 0; word_index < 5; ++word_index) {
    uint32_t value = LoadU32(base, address + word_index * 4);
    uint32_t seed = kInitialSeed;
    while (value > 0 && seed > 0) {
      const uint32_t index = value / seed;
      if (index == 0 || index > kCharacters.size()) {
        break;
      }
      result.push_back(kCharacters[index - 1]);
      value %= seed;
      seed /= 38;
    }
  }
  return result;
}

void StoreDescriptorWords(uint8_t *base, uint32_t address,
                          const trick::TrickIntentDescriptor &descriptor) {
  if (!base || !address) {
    return;
  }
  for (size_t index = 0; index < descriptor.words.size(); ++index) {
    REX_STORE_U32(address + static_cast<uint32_t>(index * sizeof(uint32_t)),
                  descriptor.words[index]);
  }
}

bool StoreFastString36(uint8_t *base, uint32_t address,
                       std::string_view value) {
  if (!base || !address || value.empty() || value.size() > 36) {
    return false;
  }
  constexpr uint32_t kInitialSeed = 79235168;
  constexpr std::string_view kCharacters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_";
  for (uint32_t word_index = 0; word_index < 6; ++word_index) {
    uint32_t encoded = 0;
    uint32_t seed = kInitialSeed;
    const size_t first = word_index * 6;
    for (size_t index = first; index < std::min(first + 6, value.size());
         ++index) {
      const size_t character = kCharacters.find(value[index]);
      if (character == std::string_view::npos) {
        return false;
      }
      encoded += static_cast<uint32_t>(character + 1) * seed;
      seed /= 38;
    }
    REX_STORE_U32(address + word_index * 4, encoded);
  }
  return true;
}

bool StoreAssetPath(uint8_t *base, uint32_t address, std::string_view path) {
  if (!base || !address || path.empty() || path.size() > 127) {
    return false;
  }
  for (size_t index = 0; index < path.size(); ++index) {
    char character = path[index];
    if (character == '/') {
      character = '\\';
    }
    REX_STORE_U8(address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(character));
  }
  REX_STORE_U8(address + static_cast<uint32_t>(path.size()), 0);
  return true;
}

std::string LoadTelemetryText(uint8_t *base, uint32_t address,
                              size_t maximum_length) {
  std::string text;
  if (!base || !address) {
    return text;
  }
  for (size_t index = 0; index < maximum_length; ++index) {
    const uint8_t value = LoadU8(base, address + static_cast<uint32_t>(index));
    if (value == 0) {
      break;
    }
    const char character = static_cast<char>(value);
    // Observation responses are space-delimited key/value fields, so spaces
    // must be encoded even though the in-game display buffer retains them.
    const bool printable = character > 0x20 && character <= 0x7E;
    text.push_back(printable && character != ':' && character != ',' ? character
                                                                     : '_');
  }
  return text;
}

void StoreDisplayText(uint8_t *guest_base, uint32_t address,
                      std::string_view display_name) {
  if (!guest_base || !address || display_name.empty()) {
    return;
  }
  uint8_t *base = guest_base;
  REX_STORE_U8(address, '#');
  size_t index = 0;
  for (; index < display_name.size() && index < 126; ++index) {
    REX_STORE_U8(address + 1 + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(display_name[index]));
  }
  REX_STORE_U8(address + 1 + static_cast<uint32_t>(index), 0);
}

template <size_t N>
void AppendWords(std::ostringstream &stream,
                 const std::array<uint32_t, N> &words) {
  for (uint32_t word : words) {
    stream << ':';
    AppendHex(stream, word);
  }
}

template <size_t N>
void CaptureMatrixRows(
    uint8_t *base, uint32_t owner,
    const std::array<uint32_t, N> &offsets,
    std::array<uint32_t, N * 16> &output) {
  if (!base || !owner) {
    return;
  }
  size_t component = 0;
  for (const uint32_t offset : offsets) {
    const uint32_t matrix = owner + offset;
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t column = 0; column < 4; ++column) {
        output[component++] =
            LoadU32(base, matrix + row * 16 + column * 4);
      }
    }
  }
}

template <size_t N>
void AppendMatrixRows(
    std::ostringstream &stream, const std::array<uint32_t, N> &offsets,
    const std::array<uint32_t, N * 16> &before,
    const std::array<uint32_t, N * 16> &after) {
  size_t component = 0;
  for (const uint32_t offset : offsets) {
    stream << ':';
    AppendHex(stream, offset);
    for (size_t index = 0; index < 16; ++index) {
      stream << ':';
      AppendHex(stream, before[component + index]);
    }
    for (size_t index = 0; index < 16; ++index) {
      stream << ':';
      AppendHex(stream, after[component + index]);
    }
    component += 16;
  }
}

bool BeginArmedEvent(EventKind kind) {
  if (!g_armed.load(std::memory_order_acquire)) {
    return false;
  }
  g_counts[static_cast<size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool BeginEvent(EventKind kind) {
  if (!BeginArmedEvent(kind)) {
    return false;
  }
  return g_focused.load(std::memory_order_acquire);
}

void PushFocusedEvent(std::string event) {
  std::lock_guard lock(g_event_mutex);
  if (g_focused_events.size() < kFocusedEventCapacity) {
    g_focused_events.push_back(std::move(event));
  }
}

void PushScoreRewardEvent(std::string event) {
  std::lock_guard lock(g_event_mutex);
  if (g_score_reward_events.size() < 32) {
    g_score_reward_events.push_back(std::move(event));
  }
}

void PushFocusedEventIfChanged(std::string key, std::string signature,
                               std::string event) {
  std::lock_guard lock(g_event_mutex);
  const auto previous = g_last_signatures.find(key);
  if (previous != g_last_signatures.end() && previous->second == signature) {
    return;
  }
  g_last_signatures[std::move(key)] = std::move(signature);
  if (g_focused_events.size() < kFocusedEventCapacity) {
    g_focused_events.push_back(std::move(event));
  }
}

bool ResolveMappingDescriptorByName(std::string_view name,
                                    trick::TrickIntentDescriptor &descriptor) {
  bool found = false;
  std::lock_guard lock(g_event_mutex);
  for (const auto &[words, candidate_name] : g_mapping_fast_string_names) {
    if (candidate_name != name) {
      continue;
    }
    if (found && descriptor.words != words) {
      return false;
    }
    descriptor.words = words;
    found = true;
  }
  return found;
}

void ObserveSimpleCollector(EventKind kind, const char *tag, PPCContext &ctx,
                            uint8_t *base) {
  if (!BeginEvent(kind)) {
    return;
  }
  const uint32_t collector = ctx.r3.u32;
  const uint32_t current_id =
      LoadU32(base, collector + trick::AirCollectorLayout::kCurrentScorableId);
  const uint32_t active = LoadU8(
      base, collector + trick::AirCollectorLayout::kCurrentScorableActive);
  const uint32_t record_id =
      LoadU32(base, collector + trick::AirCollectorLayout::kCurrentScorable +
                        trick::ScorableLayout::kId);
  std::ostringstream event;
  event << tag << '@' << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.lr);
  event << ':';
  AppendHex(event, collector);
  event << ':';
  AppendHex(event, current_id);
  event << ':' << active;
  event << ':';
  AppendHex(event, record_id);
  std::ostringstream signature;
  AppendHex(signature, current_id);
  signature << ':' << active << ':';
  AppendHex(signature, record_id);
  std::ostringstream key;
  key << tag << ':';
  AppendHex(key, collector);
  PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
}

void ObserveAnimation(EventKind kind, const char *tag, uint64_t frame,
                      uint32_t caller, uint32_t conditioner, uint8_t *base) {
  if (!BeginEvent(kind)) {
    return;
  }
  const uint32_t phys_out =
      LoadU32(base, conditioner + trick::AnimationConditionerLayout::kPhysOut);
  const uint32_t trick_state =
      LoadU32(base, phys_out + trick::PhysOutLayout::kTrickState);
  const auto live_intent = LoadWords<kAnimationIntentWordCount>(
      base, trick_state + trick::AnimationTrickStateLayout::kLiveDescriptor);
  const auto published_intent = LoadWords<kAnimationIntentWordCount>(
      base,
      trick_state + trick::AnimationTrickStateLayout::kPublishedDescriptor);
  const auto current_output = LoadWords<kAnimationIntentWordCount>(
      base, trick_state + trick::PhysOutTrickLayout::kCurrentDescriptor);
  const auto completed_producer = LoadWords<kAnimationIntentWordCount>(
      base, trick_state + trick::PhysOutTrickLayout::kAnimOutTrickData +
                trick::AnimOutTrickDataLayout::kCompletedDescriptor);
  const uint32_t producer_word =
      LoadU32(base, trick_state + trick::PhysOutTrickLayout::kAnimOutTrickData +
                        trick::AnimOutTrickDataLayout::kWord48);
  const uint32_t producer_float_52_bits =
      LoadU32(base, trick_state + trick::PhysOutTrickLayout::kAnimOutTrickData +
                        trick::AnimOutTrickDataLayout::kFloat52);
  const uint32_t producer_float_56_bits =
      LoadU32(base, trick_state + trick::PhysOutTrickLayout::kAnimOutTrickData +
                        trick::AnimOutTrickDataLayout::kFloat56);

  std::ostringstream signature;
  signature << static_cast<uint32_t>(
      LoadU8(base, trick_state + trick::AnimationTrickStateLayout::kActive));
  AppendWords(signature, live_intent);
  AppendWords(signature, published_intent);
  AppendWords(signature, current_output);
  AppendWords(signature, completed_producer);
  AppendHex(signature, producer_word);
  AppendHex(signature, producer_float_52_bits);
  AppendHex(signature, producer_float_56_bits);

  std::ostringstream event;
  event << tag << '@' << frame << ':';
  AppendHex(event, caller);
  event << ':';
  AppendHex(event, conditioner);
  event << ':';
  AppendHex(event, phys_out);
  event << ':';
  AppendHex(event, trick_state);
  event << ':'
        << static_cast<uint32_t>(LoadU8(
               base, trick_state + trick::AnimationTrickStateLayout::kActive));
  AppendWords(event, live_intent);
  AppendWords(event, published_intent);
  // Preserve fields 6..17 for the existing autonomous pipeline assertion.
  // Appended fields are: filtered current output, producer completed
  // descriptor, producer word +48, and the raw IEEE-754 bits at +52/+56.
  AppendWords(event, current_output);
  AppendWords(event, completed_producer);
  event << ':';
  AppendHex(event, producer_word);
  event << ':';
  AppendHex(event, producer_float_52_bits);
  event << ':';
  AppendHex(event, producer_float_56_bits);
  std::ostringstream key;
  key << tag << ':';
  AppendHex(key, conditioner);
  PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
}

struct GuestReferencePath {
  uint32_t root_offset{0xFFFFFFFFu};
  uint32_t child_offset{0xFFFFFFFFu};
  uint32_t component_offset{0xFFFFFFFFu};
  uint32_t interface_id{};
};

std::optional<GuestReferencePath>
FindGuestReferencePath(uint8_t *base, uint32_t root, uint32_t target) {
  if (!base || !root || !target) {
    return std::nullopt;
  }
  constexpr uint32_t kGuestHeapStart = 0x40000000;
  constexpr uint32_t kGuestHeapEnd = 0x72000000;
  constexpr uint32_t kMaximumComponentTableBytes = 0x1000;
  constexpr uint32_t kComponentOwnerScanBytes = 0x800;
  // The player-0 action-graph actor is also the PlayAnimation state context.
  // Retail sub_82965630 searches its sorted (interface-id, component-pointer)
  // vector at +4/+8. Accept PhysOut only as one of those component pointers.
  const uint32_t components_begin = LoadU32(base, root + 4);
  const uint32_t components_end = LoadU32(base, root + 8);
  if (components_begin < kGuestHeapStart || components_end < components_begin ||
      components_end > kGuestHeapEnd ||
      components_end - components_begin > kMaximumComponentTableBytes ||
      (components_end - components_begin) % 8 != 0) {
    return std::nullopt;
  }
  for (uint32_t entry = components_begin; entry < components_end; entry += 8) {
    const uint32_t interface_id = LoadU32(base, entry);
    const uint32_t component = LoadU32(base, entry + 4);
    const uint32_t entry_offset = entry + 4 - components_begin;
    if (component == target) {
      return GuestReferencePath{4, entry_offset, 0xFFFFFFFFu, interface_id};
    }
    if (component < kGuestHeapStart ||
        component + kComponentOwnerScanBytes > kGuestHeapEnd) {
      continue;
    }
    for (uint32_t component_offset = 0;
         component_offset < kComponentOwnerScanBytes; component_offset += 4) {
      if (LoadU32(base, component + component_offset) == target) {
        return GuestReferencePath{4, entry_offset, component_offset,
                                  interface_id};
      }
    }
  }
  return std::nullopt;
}

bool ResolveLocalPhysOut(uint8_t *base, uint32_t phys_out) {
  const uint32_t actor =
      g_local_action_graph_actor.load(std::memory_order_acquire);
  if (!actor || !phys_out) {
    return false;
  }
  const uint32_t resolved = g_local_phys_out.load(std::memory_order_acquire);
  const uint32_t resolved_actor =
      g_local_phys_out_actor.load(std::memory_order_acquire);
  if (resolved && phys_out == resolved && actor == resolved_actor) {
    return true;
  }
  const auto path = FindGuestReferencePath(base, actor, phys_out);
  if (!path) {
    return false;
  }
  // The direct-boot lifecycle may replace the player's PhysOut component
  // while retaining the same ActionGraph Actor allocation. Do not permanently
  // pin the pre-freeroam component: promote a replacement only when it is
  // independently found in that actor's verified component table.
  if (resolved != phys_out || resolved_actor != actor) {
    g_local_spatial_revision.fetch_add(
        1, std::memory_order_acq_rel);
    g_local_spatial_frame.store(0, std::memory_order_relaxed);
    g_local_spatial_sample_time_us.store(
        0, std::memory_order_relaxed);
    g_local_spatial_revision.fetch_add(
        1, std::memory_order_release);
  }
  g_local_phys_out_actor.store(actor, std::memory_order_release);
  g_local_phys_out_root_offset.store(path->root_offset,
                                     std::memory_order_release);
  g_local_phys_out_child_offset.store(path->child_offset,
                                      std::memory_order_release);
  g_local_phys_out_component_offset.store(path->component_offset,
                                          std::memory_order_release);
  g_local_phys_out_interface_id.store(path->interface_id,
                                      std::memory_order_release);
  g_local_phys_out.store(phys_out, std::memory_order_release);
  return true;
}

} // namespace

uint32_t LocalActionGraphActor() {
  return g_local_action_graph_actor.load(std::memory_order_acquire);
}

uint32_t SelectMotionGraphPath(PPCContext &ctx, uint8_t *base,
                               uint32_t retail_path) {
  if (!base || !retail_path ||
      !REXCVAR_GET(skate3_custom_trick_native_graph)) {
    return retail_path;
  }

  constexpr std::string_view kCustomPath =
      "state\\MotionGraph_OnBoard_Codex.xml";
  // sub_82598B30 owns a 128-byte frame. Its live locals begin at +80, leaving
  // this lower range available for the synchronous HierarchicalController
  // load. Cache::Load hashes/copies the normalized path before returning.
  const uint32_t path_address = ctx.r1.u32 + 0x20;
  for (size_t index = 0; index < kCustomPath.size(); ++index) {
    REX_STORE_U8(path_address + static_cast<uint32_t>(index),
                 static_cast<uint8_t>(kCustomPath[index]));
  }
  REX_STORE_U8(path_address + static_cast<uint32_t>(kCustomPath.size()), 0);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_acq_rel)) {
    REXLOG_WARN(
        "Skate 3 custom trick: redirecting motion graph path "
        "guest=0x{:08X} to {}",
        path_address, kCustomPath);
  }
  return path_address;
}

bool NativeCustomTrickGraphEnabled() {
  return REXCVAR_GET(skate3_custom_trick_native_graph);
}

void FilterTrackedOuterCircleIntentQuery(PPCContext &ctx, uint8_t *base,
                                         uint32_t intent_name) {
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  const uint64_t hold_until =
      g_outer_circle_custom_hold_until_frame.load(std::memory_order_acquire);
  const bool reservation_active =
      input_history_watch::OuterCircleGestureActive(0) ||
      (frame != 0 && frame <= hold_until);
  if (!base || !intent_name ||
      !REXCVAR_GET(skate3_custom_input_outer_circle) ||
      !REXCVAR_GET(skate3_custom_trick_native_graph) ||
      !reservation_active) {
    return;
  }

  // A vanilla Ollie resolves roughly halfway through a bottom-origin circle,
  // before the full custom gesture can be distinguished. Reserve only that
  // competing state while the recognizer is tracking. Retail gesture-token
  // candidates are independently suppressed at their insertion and mapping
  // boundaries; the active custom token remains eligible after completion.
  const std::string name = DecodeFastString(base, intent_name);
  if ((ctx.r3.u32 != 0 || name == custom_trick::ActiveInputToken()) &&
      BeginEvent(EventKind::GestureInjection)) {
    std::ostringstream event;
    event << "HQ@" << input_history_watch::CurrentFrameSequence() << ':'
          << (name.empty() ? "UNKNOWN" : name) << ':'
          << (ctx.r3.u32 != 0 ? "true" : "false")
          << ":outer_circle_active";
    PushFocusedEventIfChanged("HQ:" + name,
                              ctx.r3.u32 != 0 ? "true" : "false",
                              event.str());
  }
  const uint64_t completed_frame =
      g_outer_circle_last_injection_frame.load(std::memory_order_acquire);
  const bool completed_window =
      (frame != 0 && frame <= hold_until) ||
      (completed_frame != 0 && frame >= completed_frame &&
       frame - completed_frame <= 8);
  if (ctx.r3.u32 != 0 &&
      (name == "OLLIE" || name == "NOLLIE" ||
       (name == "TRICK" && !completed_window))) {
    ctx.r3.u32 = 0;
    if (BeginEvent(EventKind::GestureInjection)) {
      std::ostringstream event;
      event << "IR@" << frame << ':' << name
            << ":suppressed_outer_circle_reservation";
      PushFocusedEventIfChanged("IR:" + name, "suppressed", event.str());
    }
  }
}

HasIntentConditionScope::HasIntentConditionScope(PPCContext &ctx,
                                                 uint8_t *base)
    : ctx_(ctx), base_(base),
      intent_name_(ctx.r3.u32 ? ctx.r3.u32 + 40 : 0) {}

HasIntentConditionScope::~HasIntentConditionScope() {
  FilterTrackedOuterCircleIntentQuery(ctx_, base_, intent_name_);
}

bool ShouldForceTextStateGraph(uint8_t *base, uint32_t normalized_path) {
  if (!base || !normalized_path ||
      !REXCVAR_GET(skate3_custom_trick_native_graph)) {
    return false;
  }
  constexpr std::string_view kCustomRoot =
      "state\\MotionGraph_OnBoard_Codex.xml";
  constexpr std::string_view kProjectIncludeRoot =
      "state\\MotionGraphIncludes\\";

  std::string path;
  path.reserve(128);
  for (uint32_t index = 0; index < 512; ++index) {
    const char value =
        static_cast<char>(REX_LOAD_U8(normalized_path + index));
    if (value == '\0') {
      break;
    }
    path.push_back(value);
  }

  const auto ascii_equal = [](char lhs, char rhs) {
    const auto fold = [](char value) {
      return value >= 'A' && value <= 'Z'
                 ? static_cast<char>(value + ('a' - 'A'))
                 : value;
    };
    return fold(lhs) == fold(rhs);
  };
  const auto equals = [&](std::string_view expected) {
    return path.size() == expected.size() &&
           std::equal(path.begin(), path.end(), expected.begin(), ascii_equal);
  };
  const auto starts_with = [&](std::string_view expected) {
    return path.size() >= expected.size() &&
           std::equal(expected.begin(), expected.end(), path.begin(),
                      ascii_equal);
  };

  // The root XML includes the recovered source tree, but Skate normally
  // resolves each child to its retail compiled .stategraph cache. Force text
  // parsing for the project-owned include subtree too, otherwise edits in
  // onboard.xml (including genuinely new states) are silently bypassed.
  const bool force =
      equals(kCustomRoot) || starts_with(kProjectIncludeRoot);
  if (force) {
    static std::atomic<uint32_t> logged_force_count{0};
    if (logged_force_count.fetch_add(1, std::memory_order_acq_rel) < 12) {
      REXLOG_WARN(
          "Skate 3 custom trick: forcing XML StateGraph parser for {}",
          path);
    }
  }
  return force;
}

void ObserveStateGraphIncludePath(uint8_t *base, uint32_t path_address) {
  if (!base || !path_address ||
      !REXCVAR_GET(skate3_custom_trick_native_graph)) {
    return;
  }
  std::string path;
  path.reserve(128);
  for (uint32_t index = 0; index < 512; ++index) {
    const char value =
        static_cast<char>(REX_LOAD_U8(path_address + index));
    if (value == '\0') {
      break;
    }
    path.push_back(value);
  }
  static std::atomic<uint32_t> logged_count{0};
  const bool focused =
      path.find("onboard.xml") != std::string::npos ||
      path.find("Tricks.xml") != std::string::npos ||
      path.find("T_Codex720Flip.xml") != std::string::npos;
  if (focused ||
      logged_count.fetch_add(1, std::memory_order_acq_rel) < 12) {
    REXLOG_WARN("Skate 3 custom trick: XML include path {}", path);
  }
}

ActionGraphInputFillObservationScope::ActionGraphInputFillObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      previous_depth_(g_active_action_graph_input_fill.depth),
      previous_listener_(g_active_action_graph_input_fill.listener),
      previous_actor_(g_active_action_graph_input_fill.actor),
      previous_intents_(g_active_action_graph_input_fill.intents) {
  g_active_action_graph_input_fill.depth = previous_depth_ + 1;
  g_active_action_graph_input_fill.listener = ctx.r3.u32;
  g_active_action_graph_input_fill.actor =
      LoadU32(base, g_active_action_graph_input_fill.listener + 4);
  g_active_action_graph_input_fill.intents = 0;
}

ActionGraphInputFillObservationScope::~ActionGraphInputFillObservationScope() {
  const uint32_t player_index =
      LoadU32(base_, g_active_action_graph_input_fill.listener);
  if (player_index == 0 && g_active_action_graph_input_fill.actor != 0) {
    const uint32_t previous_actor =
        g_local_action_graph_actor.load(std::memory_order_acquire);
    if (previous_actor != 0 &&
        previous_actor != g_active_action_graph_input_fill.actor) {
      g_local_phys_out.store(0, std::memory_order_release);
      g_local_phys_out_actor.store(0, std::memory_order_release);
      g_local_phys_out_root_offset.store(0xFFFFFFFFu,
                                         std::memory_order_release);
      g_local_phys_out_child_offset.store(0xFFFFFFFFu,
                                          std::memory_order_release);
      g_local_phys_out_component_offset.store(0xFFFFFFFFu,
                                              std::memory_order_release);
      g_local_phys_out_interface_id.store(0, std::memory_order_release);
      g_local_score_module.store(0, std::memory_order_release);
      g_local_score_holder.store(0, std::memory_order_release);
    }
    g_local_action_graph_listener.store(
        g_active_action_graph_input_fill.listener, std::memory_order_release);
    g_local_action_graph_actor.store(g_active_action_graph_input_fill.actor,
                                     std::memory_order_release);
    g_local_action_graph_skater_anim.store(
        LoadU32(base_, g_active_action_graph_input_fill.actor + 1808),
        std::memory_order_release);
    g_local_action_graph_intents.store(
        g_active_action_graph_input_fill.intents, std::memory_order_release);
    mechanics_sandbox::ObserveLocalActionGraphActor(
        input_history_watch::CurrentFrameSequence(),
        g_active_action_graph_input_fill.actor);
  }
  input_history_watch::OuterCircleGesture outer_circle{};
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  const uint64_t hold_until =
      g_outer_circle_custom_hold_until_frame.load(std::memory_order_acquire);
  if (REXCVAR_GET(skate3_custom_trick_native_graph) && player_index == 0 &&
      g_active_action_graph_input_fill.intents != 0 && frame != 0 &&
      frame <= hold_until) {
    PPCContext hold_ctx = ctx_;
    hold_ctx.r1.u32 = (ctx_.r1.u32 - 0x100) & ~0xFu;
    const uint32_t hold_name = hold_ctx.r1.u32 + 0x40;
    const auto hold_input = [&](std::string_view name) {
      if (!StoreFastString36(base_, hold_name, name)) {
        return;
      }
      hold_ctx.r3.u32 = g_active_action_graph_input_fill.intents;
      hold_ctx.r4.u32 = hold_name;
      hold_ctx.f1.f64 = 1.0;
      hold_ctx.lr = 0x825999F0;
      sub_82455030(hold_ctx, base_);
    };
    hold_input("TRICK");
    hold_input(custom_trick::ActiveInputToken());
  }
  const bool circle_triggered =
      REXCVAR_GET(skate3_custom_input_outer_circle) && player_index == 0 &&
      g_active_action_graph_input_fill.intents != 0 &&
      input_history_watch::ConsumeOuterCircleGesture(8, outer_circle);
  const bool chord_triggered =
      !circle_triggered && REXCVAR_GET(skate3_custom_input_ls_rs) &&
      player_index == 0 && g_active_action_graph_input_fill.intents != 0 &&
      input_history_watch::ConsumeLsRsChordPress(4);
  if (circle_triggered || chord_triggered) {
    PPCContext insert_ctx = ctx_;
    insert_ctx.r1.u32 = (ctx_.r1.u32 - 0x100) & ~0xFu;
    const uint32_t name_address = insert_ctx.r1.u32 + 0x40;
    const std::string_view token = custom_trick::ActiveInputToken();
    const auto insert_input = [&](std::string_view name, float value) {
      if (!StoreFastString36(base_, name_address, name)) {
        return false;
      }
      insert_ctx.r3.u32 = g_active_action_graph_input_fill.intents;
      insert_ctx.r4.u32 = name_address;
      insert_ctx.f1.f64 = static_cast<double>(value);
      insert_ctx.lr = 0x825999F0;
      sub_82455030(insert_ctx, base_);
      return true;
    };

    if (circle_triggered &&
        REXCVAR_GET(skate3_custom_trick_native_graph)) {
      insert_input("TRICK", 1.0f);
    }
    if (insert_input(token, 1.0f)) {
      g_custom_input_token_insert_count.fetch_add(1, std::memory_order_relaxed);
      if (circle_triggered) {
        g_outer_circle_intent_pending_intents.store(
            g_active_action_graph_input_fill.intents,
            std::memory_order_release);
        g_outer_circle_intent_pending_frame.store(
            input_history_watch::CurrentFrameSequence(),
            std::memory_order_release);
        g_outer_circle_custom_hold_until_frame.store(
            input_history_watch::CurrentFrameSequence() + 8,
            std::memory_order_release);
      }
      custom_trick::RequestFromActionGraphToken(
          input_history_watch::CurrentFrameSequence(), base_,
          g_active_action_graph_input_fill.listener,
          g_active_action_graph_input_fill.actor,
          g_active_action_graph_input_fill.intents, false);
      if (BeginEvent(EventKind::CustomInputTokenInsert)) {
        std::ostringstream event;
        event << "CI@" << input_history_watch::CurrentFrameSequence()
              << ':' << (circle_triggered ? "RS_CW360" : "LS_RS") << ':'
              << token << ':';
        AppendHex(event, g_active_action_graph_input_fill.intents);
        if (circle_triggered) {
          event << ":duration=" << outer_circle.duration_frames << ":speed=";
          AppendHex(event, outer_circle.speed_bits);
        }
        PushFocusedEvent(event.str());
      }
    }
  }
  if (player_index == 0 && g_active_action_graph_input_fill.actor != 0) {
    custom_trick::TryDispatchQueuedGroundLayer(
        ctx_, base_, g_active_action_graph_input_fill.actor);
    custom_trick::TryDispatchQueuedAirLayer(
        ctx_, base_, g_active_action_graph_input_fill.actor);
    custom_trick::TryDispatchPendingHotkey(
        ctx_, base_, g_active_action_graph_input_fill.listener,
        g_active_action_graph_input_fill.actor,
        g_active_action_graph_input_fill.intents);
  }
  g_active_action_graph_input_fill.depth = previous_depth_;
  g_active_action_graph_input_fill.listener = previous_listener_;
  g_active_action_graph_input_fill.actor = previous_actor_;
  g_active_action_graph_input_fill.intents = previous_intents_;
}

ActionGraphIntentInsertObservationScope::
    ActionGraphIntentInsertObservationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()),
      listener_(g_active_action_graph_input_fill.listener),
      actor_(g_active_action_graph_input_fill.actor), intents_(ctx.r3.u32),
      name_(ctx.r4.u32), caller_(ctx.lr),
      value_bits_(std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64))),
      suppressed_(false), focused_(false) {
  if (g_active_action_graph_input_fill.depth != 0 &&
      g_active_action_graph_input_fill.intents == 0) {
    g_active_action_graph_input_fill.intents = intents_;
  }
  const float value = std::bit_cast<float>(value_bits_);
  // The retail input fill publishes partial Flickit action tokens while a
  // larger gesture is still in progress. For the custom outer circle these
  // tokens can start a shuvit animation before the circle has completed,
  // even though GetGestureIntents is suppressed later in the pipeline.
  // Suppress only the retail gesture-token insertion callsite on player 0's
  // active fill. Other controls and the completed custom token remain intact.
  const bool local_player_fill =
      g_active_action_graph_input_fill.depth != 0 && listener_ != 0 &&
      LoadU32(base_, listener_) == 0;
  if (REXCVAR_GET(skate3_custom_input_outer_circle) && local_player_fill &&
      caller_ == 0x8259B77C && value != 0.0f &&
      input_history_watch::OuterCircleGestureActive(0)) {
    ctx_.f1.f64 = 0.0;
    suppressed_ = true;
  }
  // Reserve the generic Trick category as well as concrete retail gesture
  // tokens. Leaving this value high makes the state graph consume its rising
  // edge before the full circle is distinguishable. The completed custom
  // pulse is inserted from 0x825999F0 and is therefore deliberately retained.
  if (REXCVAR_GET(skate3_custom_trick_native_graph) && local_player_fill &&
      caller_ == 0x8259B76C && value != 0.0f &&
      input_history_watch::OuterCircleGestureActive(0) &&
      DecodeFastString(base_, name_) == "TRICK") {
    ctx_.f1.f64 = 0.0;
    suppressed_ = true;
  }
  if (g_active_action_graph_input_fill.depth != 0 && value != 0.0f) {
    focused_ = BeginEvent(EventKind::ActionGraphIntentInsert);
  }
}

ActionGraphIntentInsertObservationScope::
    ~ActionGraphIntentInsertObservationScope() {
  if (!focused_) {
    return;
  }
  std::string name = DecodeFastString(base_, name_);
  if (name.empty()) {
    name = "UNKNOWN";
  }
  std::ostringstream event;
  event << "AI@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, listener_);
  event << ':';
  AppendHex(event, actor_);
  event << ':';
  AppendHex(event, intents_);
  event << ':' << name << ':';
  AppendHex(event, value_bits_);
  if (suppressed_) {
    event << ":suppressed_outer_circle";
  }
  PushFocusedEventIfChanged("AI:" + name, "active", event.str());
}

GestureMappingInitObservationScope::GestureMappingInitObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), mapping_(ctx.r3.u32) {
  {
    std::lock_guard lock(g_event_mutex);
    g_mapping_fast_string_names.clear();
  }
  g_mapping_init_active.store(true, std::memory_order_release);
}

GestureMappingInitObservationScope::~GestureMappingInitObservationScope() {
  g_mapping_init_active.store(false, std::memory_order_release);
  g_mapping_address.store(mapping_, std::memory_order_release);
  for (uint32_t table = 0;
       table < trick::GestureTrickMappingLayout::kGroupCount; ++table) {
    const uint32_t table_address =
        mapping_ + table * trick::GestureTrickMappingLayout::kGroupTableStride;
    // EASTL hashtable header: +4 bucket array, +8 bucket count, +12 size.
    g_mapping_bucket_counts[table].store(LoadU32(base_, table_address + 8),
                                         std::memory_order_release);
    g_mapping_table_counts[table].store(LoadU32(base_, table_address + 12),
                                        std::memory_order_release);
  }
  g_mapping_initialized.store(true, std::memory_order_release);
}

FastStringMappingObservationScope::FastStringMappingObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base), destination_(ctx.r3.u32), source_(ctx.r4.u32),
      active_(g_mapping_init_active.load(std::memory_order_acquire)) {}

FastStringMappingObservationScope::~FastStringMappingObservationScope() {
  uint8_t *base = base_;
  const std::string token = LoadToken(base_, source_);
  if (token.empty()) {
    return;
  }
  if (cac_gesture::IsUpdating()) {
    constexpr std::string_view kAirGuitarPrefix = "B_GSTR_AIRGUITAR_";
    constexpr std::string_view kFullBodyAsset = "codex-gunplay-fullbody";
    constexpr std::string_view kFullBodyAnimation = "CODEX_GUNPLAY_FULLBODY";
    bool asset_loaded = false;
    {
      std::lock_guard asset_lock(g_animation_asset_mutex);
      const auto loaded =
          g_custom_animation_asset_loaded.find(std::string(kFullBodyAsset));
      asset_loaded =
          loaded != g_custom_animation_asset_loaded.end() && loaded->second;
    }
    if (asset_loaded &&
        REXCVAR_GET(skate3_cac_gesture_fullbody_asset) &&
        token.starts_with(kAirGuitarPrefix) &&
        token != kFullBodyAnimation && ctx_.r1.u32 >= 0x200) {
      PPCContext replacement = ctx_;
      replacement.r1.u32 = (ctx_.r1.u32 - 0x100) & ~UINT32_C(15);
      REX_STORE_U32(replacement.r1.u32, ctx_.r1.u32);
      const uint32_t name_address = replacement.r1.u32 + 0x20;
      if (StoreAssetPath(base_, name_address, kFullBodyAnimation)) {
        replacement.r3.u64 = destination_;
        replacement.r4.u64 = name_address;
        sub_823C3B00(replacement, base_);
        REXLOG_WARN(
            "cac-gesture: fullbody descriptor phase={} selected={} "
            "source='{}' target='{}'",
            cac_gesture::Phase(), cac_gesture::Selected(), token,
            kFullBodyAnimation);
      }
    }
    const auto gesture_words =
        LoadWords<trick::TrickIntentDescriptor::kWordCount>(base_,
                                                            destination_);
    REXLOG_WARN(
        "cac-gesture: body-intent phase={} selected={} name='{}' "
        "descriptor={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
        cac_gesture::Phase(), cac_gesture::Selected(), token,
        gesture_words[0], gesture_words[1], gesture_words[2],
        gesture_words[3], gesture_words[4], gesture_words[5]);
    if (token.starts_with("B_GSTR_AIRGUITAR_")) {
      const uint32_t database =
          g_andale_onboard_database.load(std::memory_order_acquire);
      if (database && ctx_.r1.u32 >= 0x20000) {
        PPCContext lookup = ctx_;
        lookup.r1.u32 = (ctx_.r1.u32 - 0x100) & ~UINT32_C(15);
        REX_STORE_U32(lookup.r1.u32, ctx_.r1.u32);
        const uint32_t name_address = lookup.r1.u32 + 0x20;
        if (StoreFastString36(base_, name_address, token)) {
          lookup.r3.u64 = database;
          lookup.r4.u64 = name_address;
          sub_82D1C200(lookup, base_);
          const uint32_t clip = lookup.r3.u32;
          if (clip) {
            std::lock_guard gesture_lock(g_gesture_stream_mutex);
            g_gesture_streams[clip] = token;
            REXLOG_WARN(
                "cac-gesture: body-leaf bound name='{}' clip=0x{:08X} "
                "database=0x{:08X}",
                token, clip, database);
          }
        }
      }
    }
    if (REXCVAR_GET(skate3_cac_gesture_suppress_airguitar_body) &&
        token.starts_with("B_GSTR_AIRGUITAR_")) {
      // CharacterGesture also requests this compact on-board body layer while
      // the CAC animation library evaluates the registered 131-bone gesture.
      // For a full-body CAC asset the compact layer wins the same joints and
      // hides the authored motion. Leave the descriptor empty so the CAC
      // stream is the sole pose owner for this gesture.
      StoreDescriptorWords(base_, destination_, {});
      REXLOG_WARN(
          "cac-gesture: compact body suppressed phase={} selected={} "
          "source='{}'",
          cac_gesture::Phase(), cac_gesture::Selected(), token);
    }
  }
  if (!active_) {
    return;
  }
  const auto words =
      LoadWords<trick::TrickIntentDescriptor::kWordCount>(base_, destination_);
  std::lock_guard lock(g_event_mutex);
  const auto [iterator, inserted] =
      g_mapping_fast_string_names.emplace(words, token);
  if (!inserted && iterator->second != token) {
    iterator->second = "HASH_COLLISION";
  }
}

GestureIntentObservationScope::GestureIntentObservationScope(PPCContext &ctx,
                                                             uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()), mapping_(ctx.r3.u32),
      intents_(ctx.r4.u32), group_(ctx.r5.u32), output_(ctx.r6.u32),
      caller_(ctx.lr), previous_active_group_(g_active_gesture_group),
      armed_(g_armed.load(std::memory_order_acquire)), focused_(false) {
  g_active_gesture_group = group_;
  g_active_gesture_match = {};
  if (armed_) {
    g_counts[static_cast<size_t>(EventKind::GestureLookup)].fetch_add(
        1, std::memory_order_relaxed);
    focused_ = g_focused.load(std::memory_order_acquire);
  }
}

GestureIntentObservationScope::~GestureIntentObservationScope() {
  uint8_t *base = base_;
  g_active_gesture_group = previous_active_group_;
  uint32_t result = ctx_.r3.u32 & 0xFF;
  bool injected = false;
  if (REXCVAR_GET(skate3_custom_input_outer_circle)) {
    const uint64_t pending_frame =
        g_outer_circle_intent_pending_frame.load(std::memory_order_acquire);
    const uint32_t pending_intents =
        g_outer_circle_intent_pending_intents.load(std::memory_order_acquire);
    const uint64_t age =
        frame_ >= pending_frame ? frame_ - pending_frame : UINT64_MAX;
    const bool local_intents =
        intents_ != 0 &&
        intents_ == g_local_action_graph_intents.load(std::memory_order_acquire);
    const bool pending_local_intent =
        pending_frame != 0 && age <= 8 && group_ == 0 && local_intents &&
        intents_ == pending_intents;

    // HasGestureIntent asks without an output buffer. Returning true enters
    // the native CreateTrickIntentFromGesture behavior. With the custom motion
    // graph enabled, Begin receives a genuinely disjoint custom-trick
    // descriptor; the custom state itself then owns G/A playback and the
    // ordinary trick lifecycle. The Ollie descriptor remains only as a
    // compatibility path for older experiment manifests.
    if (pending_local_intent &&
        (caller_ == 0x82BA1150 || caller_ == 0x82BA1D70)) {
      if (caller_ == 0x82BA1150) {
        ctx_.r3.u32 = 1;
        result = 1;
        injected = true;
      } else if (output_ != 0) {
        const bool native_custom_graph =
            REXCVAR_GET(skate3_custom_trick_native_graph);
        bool stored = false;
        if (native_custom_graph) {
          stored =
              StoreFastString36(base_, output_, custom_trick::ActiveInputToken());
        } else {
          const std::string_view descriptor_name =
              REXCVAR_GET(skate3_outer_circle_retail_360flip_control)
                  ? "360Flip"
                  : "Ollie";
          trick::TrickIntentDescriptor descriptor{};
          if (ResolveMappingDescriptorByName(descriptor_name, descriptor)) {
            StoreDescriptorWords(base_, output_, descriptor);
            stored = true;
          }
        }
        if (stored) {
          REX_STORE_U32(output_ + trick::TrickIntentDescriptor::kSizeBytes, 0);
          ctx_.r3.u32 = 1;
          result = 1;
          injected = true;
          uint64_t expected = pending_frame;
          if (g_outer_circle_intent_pending_frame.compare_exchange_strong(
                  expected, 0, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            g_outer_circle_intent_injection_count.fetch_add(
                1, std::memory_order_relaxed);
            g_outer_circle_last_injection_frame.store(
                frame_, std::memory_order_release);
          }
        }
      }
      if (injected && BeginEvent(EventKind::GestureInjection)) {
        std::ostringstream event;
        event << "GI@" << frame_ << ':';
        AppendHex(event, caller_);
        event << ':';
        AppendHex(event, intents_);
        event << ':' << group_ << ':';
        AppendHex(event, output_);
        event << ':' << (output_ == 0 ? "condition" : "result")
              << ':'
              << (REXCVAR_GET(skate3_custom_trick_native_graph)
                      ? custom_trick::ActiveInputToken()
                      : (REXCVAR_GET(
                             skate3_outer_circle_retail_360flip_control)
                             ? "360Flip"
                             : "OllieBase"));
        PushFocusedEvent(event.str());
      }
    } else if (pending_frame != 0 && age > 8) {
      uint64_t expected = pending_frame;
      g_outer_circle_intent_pending_frame.compare_exchange_strong(
          expected, 0, std::memory_order_acq_rel,
          std::memory_order_acquire);
    }

    // Intermediate quarter/half-circle positions must not leak as retail
    // Flickit tricks on the same player lane. A successfully synthesized
    // condition/result is deliberately retained.
    if (!injected && local_intents &&
        input_history_watch::OuterCircleGestureActive(2)) {
      if (output_ != 0) {
        for (size_t index = 0; index < kTrickIntentWordCount; ++index) {
          REX_STORE_U32(output_ + static_cast<uint32_t>(index * 4), 0);
        }
      }
      ctx_.r3.u32 = 0;
      result = 0;
    }
  }
  if (!injected && result && g_active_gesture_match.matched) {
    const auto override_result = trick_overrides::ApplyGestureOverride(
        base_, output_,
        {
            .group = group_,
            .key_name = g_active_gesture_match.key_name,
            .primary_name = g_active_gesture_match.primary_name,
            .secondary_name = g_active_gesture_match.secondary_name,
        },
        ResolveMappingDescriptorByName);
    if (override_result.action ==
            trick_overrides::Action::ReplacePrimaryWithSecondary ||
        override_result.action ==
            trick_overrides::Action::ReplacePrimaryWithNamedDescriptor) {
      g_replacement_count.fetch_add(1, std::memory_order_relaxed);
      if (BeginEvent(EventKind::GestureReplacement)) {
        std::ostringstream event;
        event << "GR@" << frame_ << ':' << group_ << ':';
        AppendHex(event, output_);
        event << ':' << override_result.source_name << ':'
              << override_result.source_name << ':'
              << override_result.target_name << ':'
              << override_result.rule_name;
        AppendWords(event, override_result.original_primary.words);
        AppendWords(event, override_result.final_primary.words);
        PushFocusedEvent(event.str());
      }
    } else if (override_result.action == trick_overrides::Action::Disable) {
      ctx_.r3.u32 = 0;
      result = 0;
      g_disable_count.fetch_add(1, std::memory_order_relaxed);
      if (BeginEvent(EventKind::GestureDisable)) {
        std::ostringstream event;
        event << "GD@" << frame_ << ':' << group_ << ':';
        AppendHex(event, output_);
        event << ':' << override_result.source_name << ':'
              << override_result.target_name << ':'
              << override_result.rule_name;
        AppendWords(event, override_result.original_primary.words);
        PushFocusedEvent(event.str());
      }
    }
  }
  if (!armed_ || !focused_) {
    return;
  }
  const auto words = result ? LoadWords<kTrickIntentWordCount>(base_, output_)
                            : std::array<uint32_t, kTrickIntentWordCount>{};
  std::ostringstream event;
  event << "G@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, mapping_);
  event << ':';
  AppendHex(event, intents_);
  event << ':' << group_ << ':';
  AppendHex(event, output_);
  event << ':' << result;
  AppendWords(event, words);
  PushFocusedEvent(event.str());
}

void ObserveGestureMappingMatch(PPCContext &ctx, uint8_t *base) {
  if (!BeginEvent(EventKind::GestureMatch)) {
    return;
  }
  const uint32_t node = ctx.r30.u32;
  const uint32_t value = LoadU32(base, node + 24);
  const auto key_words =
      LoadWords<trick::TrickIntentDescriptor::kWordCount>(base, node);
  const auto value_words =
      LoadWords<trick::TrickIntents::kWordCount>(base, value);
  std::string key_name = "UNKNOWN";
  std::string primary_name = "UNKNOWN";
  std::string secondary_name = "UNKNOWN";
  {
    std::lock_guard lock(g_event_mutex);
    const auto resolve = [](const auto &names,
                            const auto &words) -> std::string {
      const auto found = names.find(words);
      return found != names.end() ? found->second : "UNKNOWN";
    };
    key_name = resolve(g_mapping_fast_string_names, key_words);
    std::array<uint32_t, trick::TrickIntentDescriptor::kWordCount>
        primary_words{};
    std::array<uint32_t, trick::TrickIntentDescriptor::kWordCount>
        secondary_words{};
    std::copy_n(value_words.begin(), primary_words.size(),
                primary_words.begin());
    std::copy_n(value_words.begin() + primary_words.size(),
                secondary_words.size(), secondary_words.begin());
    primary_name = resolve(g_mapping_fast_string_names, primary_words);
    secondary_name = resolve(g_mapping_fast_string_names, secondary_words);
  }
  g_active_gesture_match = {
      .matched = true,
      .key_name = key_name,
      .primary_name = primary_name,
      .secondary_name = secondary_name,
  };
  const uint32_t group = g_active_gesture_group;
  uint32_t physical_table = 0xFFFFFFFF;
  if (group < trick::GestureTrickMappingLayout::kGroupCount) {
    physical_table = trick::GestureTrickMappingLayout::GroupTableOffset(group) /
                     trick::GestureTrickMappingLayout::kGroupTableStride;
  }

  std::ostringstream event;
  event << "GM@" << input_history_watch::CurrentFrameSequence() << ':';
  event << group << ':' << physical_table;
  event << ':';
  AppendHex(event, node);
  event << ':';
  AppendHex(event, value);
  event << ':' << key_name << ':' << primary_name << ':' << secondary_name
        << ':' << value_words.back();
  AppendWords(event, key_words);
  AppendWords(event, value_words);
  PushFocusedEvent(event.str());
}

CreateTrickIntentObservationScope::CreateTrickIntentObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()),
      behavior_(ctx.r3.u32), context_(ctx.r4.u32),
      state_context_(LoadU32(base, context_ + 4)),
      instance_(LoadU32(base, context_ + 8)),
      group_(LoadU32(base, behavior_ + 28)), caller_(ctx.lr),
      armed_(g_armed.load(std::memory_order_acquire)), focused_(false) {
  if (armed_) {
    g_counts[static_cast<size_t>(EventKind::CreateIntent)].fetch_add(
        1, std::memory_order_relaxed);
    focused_ = g_focused.load(std::memory_order_acquire);
  }
}

CreateTrickIntentObservationScope::~CreateTrickIntentObservationScope() {
  if (!armed_ || !focused_) {
    return;
  }
  const uint32_t observed_instance = LoadU32(base_, context_ + 8);
  const uint32_t final_instance =
      observed_instance != 0 ? observed_instance : instance_;
  const auto intent = LoadWords<kAnimationIntentWordCount>(
      base_,
      final_instance + trick::CreateTrickIntentInstanceLayout::kDescriptor);
  std::ostringstream event;
  event << "C@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, behavior_);
  event << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, state_context_);
  event << ':';
  AppendHex(event, final_instance);
  event << ':' << group_ << ':'
        << static_cast<uint32_t>(LoadU8(
               base_, final_instance +
                          trick::CreateTrickIntentInstanceLayout::kActive));
  AppendWords(event, intent);
  PushFocusedEvent(event.str());
}

ScorableStartObservationScope::ScorableStartObservationScope(PPCContext &ctx,
                                                             uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      scorable_(ctx.r3.u32), name_(ctx.r4.u32), context_argument_(ctx.r5.u32),
      phys_out_(ctx.r6.u32), start_word_(ctx.r7.u32), caller_(ctx.lr),
      start_value_bits_(
          std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64))),
      definition_adjustment_bits_(
          std::bit_cast<uint32_t>(static_cast<float>(ctx.f2.f64))),
      armed_(g_armed.load(std::memory_order_acquire)), focused_(false) {
  if (armed_) {
    g_counts[static_cast<size_t>(EventKind::ScorableStart)].fetch_add(
        1, std::memory_order_relaxed);
    focused_ = g_focused.load(std::memory_order_acquire);
  }
}

ScorableStartObservationScope::~ScorableStartObservationScope() {
  const std::string scorable_name = LoadToken(base_, name_);
  custom_trick::TryBindResolvedScorable(frame_, base_, scorable_,
                                        scorable_name, phys_out_);
  const auto override_result =
      trick_overrides::ApplyScorableOverride(base_, scorable_, scorable_name);
  if (override_result.applied) {
    g_scorable_override_count.fetch_add(1, std::memory_order_relaxed);
    if (override_result.base_points_applied &&
        BeginEvent(EventKind::ScorableOverride)) {
      std::ostringstream override_event;
      override_event << "SO@" << frame_ << ':';
      AppendHex(override_event, scorable_);
      override_event << ':' << override_result.scorable_name << ':'
                     << override_result.rule_name << ':';
      AppendHex(override_event, override_result.original_base_points);
      override_event << ':';
      AppendHex(override_event, override_result.final_base_points);
      PushFocusedEvent(override_event.str());
    }
  }
  if (!armed_ || !focused_) {
    return;
  }

  std::ostringstream event;
  event << "SC@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, scorable_);
  event << ':' << scorable_name << ':';
  AppendHex(event, name_);
  event << ':';
  AppendHex(event, context_argument_);
  event << ':';
  AppendHex(event, phys_out_);
  event << ':';
  AppendHex(event, start_word_);
  event << ':';
  AppendHex(event, start_value_bits_);
  event << ':';
  AppendHex(event, definition_adjustment_bits_);

  constexpr std::array<uint32_t, 18> kObservedWords = {
      trick::ScorableLayout::kDefinitionWord0,
      trick::ScorableLayout::kValue,
      trick::ScorableLayout::kDefinitionWord8,
      trick::ScorableLayout::kId,
      trick::ScorableLayout::kDefinitionWord112,
      trick::ScorableLayout::kDefinitionWord120,
      trick::ScorableLayout::kDefinitionWord128,
      trick::ScorableLayout::kDefinitionWord132,
      trick::ScorableLayout::kDefinitionWord136,
      trick::ScorableLayout::kDefinitionWord140,
      trick::ScorableLayout::kDefinitionFloat144,
      trick::ScorableLayout::kDefinitionWord148,
      trick::ScorableLayout::kNameHash,
      trick::ScorableLayout::kNameHash + 4,
      trick::ScorableLayout::kDefinitionWord160,
      trick::ScorableLayout::kCalculatedValue,
      trick::ScorableLayout::kStartValue,
      trick::ScorableLayout::kAccumulatedValue,
  };
  for (uint32_t offset : kObservedWords) {
    event << ':';
    AppendHex(event, LoadU32(base_, scorable_ + offset));
  }
  event << ':';
  AppendHex(event,
            LoadU32(base_, scorable_ + trick::ScorableLayout::kStartWord180));
  event << ':';
  AppendHex(
      event,
      LoadU32(base_, scorable_ + trick::ScorableLayout::kCalculatedWord184));
  for (uint32_t index = 0; index < trick::ScorableLayout::kRuntimeFlagCount;
       ++index) {
    event << ':'
          << static_cast<uint32_t>(LoadU8(
                 base_,
                 scorable_ + trick::ScorableLayout::kRuntimeFlags + index));
  }
  event << ':'
        << static_cast<uint32_t>(
               LoadU8(base_, scorable_ + trick::ScorableLayout::kActive));
  PushFocusedEvent(event.str());
}

CustomScorableNameResolutionScope::CustomScorableNameResolutionScope(
    PPCContext& ctx, uint8_t* base)
    : ctx_(ctx), base_(base), scorable_(ctx.r3.u32),
      original_name_(ctx.r4.u32),
      custom_(custom_trick::IsActiveScorableName(
          LoadToken(base, original_name_))) {
  if (custom_) {
    // "kickflip" is the verified flip-class structural definition. Only its
    // unclassified default fields are inherited; the custom identity, name,
    // and points are installed before this resolver returns.
    ctx_.r4.u32 = 0x820850F0u;
  }
}

CustomScorableNameResolutionScope::~CustomScorableNameResolutionScope() {
  if (!custom_) {
    return;
  }
  custom_trick::FinalizeResolvedScorable(base_, scorable_);
  ctx_.r4.u32 = original_name_;
}

CustomScorableIdResolutionScope::CustomScorableIdResolutionScope(
    PPCContext& ctx, uint8_t* base)
    : ctx_(ctx), base_(base), scorable_(ctx.r3.u32),
      original_id_(ctx.r4.u32),
      custom_(custom_trick::IsActiveScorableId(original_id_)) {
  if (custom_) {
    ctx_.r4.u32 = custom_trick::kFlipScorableMetadataTemplateId;
  }
}

CustomScorableIdResolutionScope::~CustomScorableIdResolutionScope() {
  if (!custom_) {
    return;
  }
  custom_trick::FinalizeResolvedScorable(base_, scorable_);
  ctx_.r4.u32 = original_id_;
}

bool TryReturnCustomScorableName(PPCContext& ctx, uint8_t* base) {
  return custom_trick::TryReturnScorableName(ctx, base);
}

CustomAirCollectorUpdateScope::CustomAirCollectorUpdateScope(
    PPCContext& ctx, uint8_t* base)
    : base_(base), collector_(ctx.r3.u32), custom_id_(0),
      previous_thread_custom_id_(g_air_collector_update_custom_id),
      mapped_current_(false), mapped_active_(false) {
  if (!base_ || !collector_) {
    return;
  }
  const uint32_t original_current_id = LoadU32(base_, collector_ + 52);
  uint32_t incoming_id = original_current_id;
  if (!custom_trick::IsActiveScorableId(incoming_id)) {
    // EndTrick clears kCurrentScorableId before the update that records the
    // active Scorable into ScoreHolder.  Recover identity from the active
    // object first; the request publication window may legitimately have
    // expired by this point.
    const uint32_t active = collector_ + 584;
    const uint32_t active_id =
        LoadU8(base_, active + trick::ScorableLayout::kActive)
            ? LoadU32(base_, active + trick::ScorableLayout::kId)
            : 0;
    if (custom_trick::IsActiveScorableId(active_id)) {
      incoming_id = active_id;
    }
  }
  if (!custom_trick::IsActiveScorableId(incoming_id)) {
    const uint32_t module =
        g_local_score_module.load(std::memory_order_acquire);
    const uint32_t phys_out =
        g_local_phys_out.load(std::memory_order_acquire);
    const bool active_local_collector =
        collector_ ==
            g_pending_custom_air_collector.load(std::memory_order_acquire) ||
        (module && phys_out &&
         LoadU32(base_, module + trick::ScoreModuleLayout::kActiveCollector) ==
             collector_);
    if (!active_local_collector ||
        !custom_trick::ShouldPublishActiveScorable(
            input_history_watch::CurrentFrameSequence(), phys_out)) {
      return;
    }
    incoming_id = custom_trick::ActiveScorableId();
  }
  custom_id_ = incoming_id;
  g_air_collector_update_custom_id = custom_id_;
  if (custom_trick::IsActiveScorableId(original_current_id)) {
    REX_STORE_U32(collector_ + 52,
                  custom_trick::kFlipScorableMetadataTemplateId);
    mapped_current_ = true;
  }

  const uint32_t active = collector_ + 584;
  if (LoadU8(base_, active + 124) &&
      LoadU32(base_, active + 12) == custom_id_) {
    REX_STORE_U32(active + 12,
                  custom_trick::kFlipScorableMetadataTemplateId);
    mapped_active_ = true;
  }
}

CustomAirCollectorUpdateScope::~CustomAirCollectorUpdateScope() {
  uint8_t* base = base_;
  if (custom_id_ && base_ && collector_) {
    if (mapped_current_ &&
        LoadU32(base_, collector_ + 52) ==
        custom_trick::kFlipScorableMetadataTemplateId) {
      REX_STORE_U32(collector_ + 52, custom_id_);
    }
    const uint32_t active = collector_ + 584;
    if (mapped_active_ &&
        LoadU32(base_, active + 12) ==
            custom_trick::kFlipScorableMetadataTemplateId) {
      REX_STORE_U32(active + 12, custom_id_);
    }
  }
  g_air_collector_update_custom_id = previous_thread_custom_id_;
}

void PromoteCustomScorableForAirCollectorStart(PPCContext& ctx) {
  if (g_air_collector_update_custom_id &&
      ctx.r4.u32 == custom_trick::kFlipScorableMetadataTemplateId) {
    ctx.r4.u32 = g_air_collector_update_custom_id;
  }
}

CustomAirCollectorStartMetadataScope::CustomAirCollectorStartMetadataScope(
    uint8_t* base, uint32_t collector)
    : base_(base), collector_(collector), custom_id_(0), mapped_(false) {
  if (!base_ || !collector_) {
    return;
  }
  const uint32_t active = collector_ + 584;
  const uint32_t id = LoadU32(base_, active + 12);
  if (!LoadU8(base_, active + 124) ||
      !custom_trick::IsActiveScorableId(id)) {
    return;
  }
  custom_id_ = id;
  REX_STORE_U32(active + 12,
                custom_trick::kFlipScorableMetadataTemplateId);
  mapped_ = true;
}

CustomAirCollectorStartMetadataScope::~CustomAirCollectorStartMetadataScope() {
  uint8_t* base = base_;
  if (!mapped_ || !base_ || !collector_) {
    return;
  }
  const uint32_t active = collector_ + 584;
  if (LoadU32(base_, active + 12) ==
      custom_trick::kFlipScorableMetadataTemplateId) {
    REX_STORE_U32(active + 12, custom_id_);
  }
}

PointPenaltyObservationScope::PointPenaltyObservationScope(PPCContext &ctx,
                                                           uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()), holder_(ctx.r3.u32),
      retail_id_(ctx.r4.s32), retail_count_(0),
      adapted_(custom_trick::BeginPointPenaltyAdapter(
          base_, holder_, retail_id_, retail_count_)) {}

PointPenaltyObservationScope::~PointPenaltyObservationScope() {
  if (adapted_) {
    custom_trick::EndPointPenaltyAdapter(base_, holder_, retail_id_,
                                         retail_count_);
  }
  custom_trick::ObservePointPenalty(
      frame_, holder_, retail_id_,
      std::bit_cast<uint32_t>(static_cast<float>(ctx_.f1.f64)), adapted_);
}

CustomRepetitionEndAirScope::CustomRepetitionEndAirScope(PPCContext &ctx,
                                                         uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      holder_(ctx.r3.u32), retail_id_(ctx.r4.s32), retail_id_count_(0),
      retail_secondary_count_(0), group_index_(0), retail_group_count_(0),
      adapted_(custom_trick::BeginEndAirAdapter(
          base_, holder_, retail_id_, retail_id_count_, retail_secondary_count_,
          group_index_, retail_group_count_)) {}

CustomRepetitionEndAirScope::~CustomRepetitionEndAirScope() {
  if (adapted_) {
    custom_trick::EndEndAirAdapter(frame_, base_, holder_, retail_id_,
                                   retail_id_count_, retail_secondary_count_,
                                   group_index_, retail_group_count_);
  }
  custom_trick::ObserveEndAirRepetitionResult(base_, holder_, retail_id_);
}

ScoreHolderEndAirObservationScope::ScoreHolderEndAirObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      holder_(ctx.r3.u32), caller_(ctx.lr), retail_id_(ctx.r4.s32),
      value_bits_(std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64))),
      pattern_class_(
          base && retail_id_ >= 0 &&
                  static_cast<uint32_t>(retail_id_) <
                      trick::ScorableMetadataTableLayout::kEntryCount
              ? LoadU32(base,
                        trick::ScorableMetadataTableLayout::EntryAddress(
                            static_cast<uint32_t>(retail_id_)) +
                            trick::ScorableMetadataTableLayout::kPatternClass)
              : UINT32_MAX),
      component_a_before_bits_(
          base && holder_
              ? LoadU32(base_,
                        holder_ +
                            trick::ScoreHolderLayout::kPendingRewardComponent0)
              : 0),
      component_b_before_bits_(
          base && holder_
              ? LoadU32(base_,
                        holder_ +
                            trick::ScoreHolderLayout::kPendingRewardComponent1)
              : 0) {}

ScoreHolderEndAirObservationScope::~ScoreHolderEndAirObservationScope() {
  if (!base_ || !holder_ || !BeginEvent(EventKind::ScoreHolderEndAir)) {
    return;
  }
  std::ostringstream event;
  event << "HE@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, holder_);
  event << ':';
  AppendHex(event, static_cast<uint32_t>(retail_id_));
  event << ':';
  AppendHex(event, value_bits_);
  event << ':' << pattern_class_ << ':';
  AppendHex(event, component_a_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ + trick::ScoreHolderLayout::kPendingRewardComponent0));
  event << ':';
  AppendHex(event, component_b_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ + trick::ScoreHolderLayout::kPendingRewardComponent1));
  PushFocusedEvent(event.str());
}

ScoreHolderRewardAirSequenceObservationScope::
    ScoreHolderRewardAirSequenceObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      holder_(ctx.r3.u32), caller_(ctx.lr),
      multiplier_bits_(std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64))),
      accumulator_before_bits_(
          base && holder_
              ? LoadU32(
                    base,
                    holder_ +
                        trick::ScoreHolderLayout::kAirSequenceRewardAccumulator)
              : 0),
      component_a_before_bits_(
          base && holder_
              ? LoadU32(base,
                        holder_ +
                            trick::ScoreHolderLayout::kPendingRewardComponent0)
              : 0),
      component_b_before_bits_(
          base && holder_
              ? LoadU32(base,
                        holder_ +
                            trick::ScoreHolderLayout::kPendingRewardComponent1)
              : 0),
      pending_count_before_(
          base && holder_
              ? LoadU32(base,
                        holder_ + trick::ScoreHolderLayout::kPendingRewardCount)
              : 0),
      pending_before_(
          base && holder_
              ? LoadU8(base,
                       holder_ +
                           trick::ScoreHolderLayout::kAirSequenceRewardPending)
              : 0) {}

ScoreHolderRewardAirSequenceObservationScope::
    ~ScoreHolderRewardAirSequenceObservationScope() {
  if (!base_ || !holder_ ||
      !BeginEvent(EventKind::ScoreHolderRewardAirSequence)) {
    return;
  }
  std::ostringstream event;
  event << "SR@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, holder_);
  event << ':';
  AppendHex(event, multiplier_bits_);
  event << ':' << static_cast<uint32_t>(pending_before_) << ':'
        << pending_count_before_ << ':';
  AppendHex(event, accumulator_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ +
                  trick::ScoreHolderLayout::kAirSequenceRewardAccumulator));
  event << ':';
  AppendHex(event, component_a_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ + trick::ScoreHolderLayout::kPendingRewardComponent0));
  event << ':';
  AppendHex(event, component_b_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ + trick::ScoreHolderLayout::kPendingRewardComponent1));
  event << ':'
        << static_cast<uint32_t>(LoadU8(
               base_,
               holder_ + trick::ScoreHolderLayout::kAirSequenceRewardPending))
        << ':'
        << LoadU32(base_,
                   holder_ + trick::ScoreHolderLayout::kPendingRewardCount);
  PushScoreRewardEvent(event.str());
}

ScoreHolderPublishAirSequenceObservationScope::
    ScoreHolderPublishAirSequenceObservationScope(PPCContext &ctx,
                                                  uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      holder_(ctx.r3.u32), caller_(ctx.lr),
      published_reward_bits_(
          std::bit_cast<uint32_t>(static_cast<float>(ctx.f1.f64))),
      cumulative_before_bits_(
          base && holder_
              ? LoadU32(
                    base,
                    holder_ +
                        trick::ScoreHolderLayout::kCumulativePublishedAirReward)
              : 0),
      accumulator_before_bits_(
          base && holder_
              ? LoadU32(
                    base,
                    holder_ +
                        trick::ScoreHolderLayout::kAirSequenceRewardAccumulator)
              : 0),
      previous_last_reward_bits_(
          base && holder_
              ? LoadU32(base,
                        holder_ +
                            trick::ScoreHolderLayout::kLastAirSequenceReward)
              : 0),
      grind_before_bits_(
          base && holder_
              ? LoadU32(base,
                        holder_ + trick::ScoreHolderLayout::kPendingGrindReward)
              : 0),
      add_to_cumulative_((ctx.r5.u32 & 0xFFu) != 0) {}

ScoreHolderPublishAirSequenceObservationScope::
    ~ScoreHolderPublishAirSequenceObservationScope() {
  const uint32_t cumulative_after_bits =
      base_ && holder_
          ? LoadU32(base_,
                    holder_ +
                        trick::ScoreHolderLayout::kCumulativePublishedAirReward)
          : 0;
  custom_trick::ObserveScoreHolderAirSequenceBank(
      frame_, holder_, published_reward_bits_, cumulative_after_bits);
  if (!base_ || !holder_ ||
      !BeginEvent(EventKind::ScoreHolderPublishAirSequence)) {
    return;
  }
  std::ostringstream event;
  event << "SB@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, holder_);
  event << ':';
  AppendHex(event, published_reward_bits_);
  event << ':' << (add_to_cumulative_ ? 1 : 0) << ':';
  AppendHex(event, cumulative_before_bits_);
  event << '>';
  AppendHex(event, cumulative_after_bits);
  event << ':';
  AppendHex(event, accumulator_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ +
                  trick::ScoreHolderLayout::kAirSequenceRewardAccumulator));
  event << ':';
  AppendHex(event, previous_last_reward_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_,
              holder_ + trick::ScoreHolderLayout::kLastAirSequenceReward));
  event << ':';
  AppendHex(event, grind_before_bits_);
  event << '>';
  AppendHex(
      event,
      LoadU32(base_, holder_ + trick::ScoreHolderLayout::kPendingGrindReward));
  PushScoreRewardEvent(event.str());
}

GrindCollectorExitObservationScope::GrindCollectorExitObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      collector_(ctx.r3.u32),
      holder_(base && collector_
                  ? LoadU32(base, collector_ +
                                      trick::GrindCollectorLayout::kScoreHolder)
                  : 0),
      caller_(ctx.lr),
      current_before_bits_(
          base && collector_
              ? LoadU32(base, collector_ +
                                  trick::GrindCollectorLayout::kCurrentReward)
              : 0),
      snapshot_before_bits_(
          base && collector_
              ? LoadU32(
                    base,
                    collector_ +
                        trick::GrindCollectorLayout::kPublishedRewardSnapshot)
              : 0),
      grind_before_bits_(
          base && holder_
              ? LoadU32(base,
                        holder_ + trick::ScoreHolderLayout::kPendingGrindReward)
              : 0),
      publish_((ctx.r4.u32 & 0xFFu) != 0) {}

GrindCollectorExitObservationScope::~GrindCollectorExitObservationScope() {
  if (!base_ || !collector_) {
    return;
  }
  if (holder_ &&
      holder_ == g_local_score_holder.load(std::memory_order_acquire)) {
    g_local_score_grind_exit_event_count.fetch_add(1,
                                                   std::memory_order_relaxed);
  }
  if (!BeginArmedEvent(EventKind::GrindCollectorExit)) {
    return;
  }
  std::ostringstream event;
  event << "SG@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, collector_);
  event << ':';
  AppendHex(event, holder_);
  event << ':' << (publish_ ? 1 : 0) << ':';
  AppendHex(event, current_before_bits_);
  event << ':';
  AppendHex(event, snapshot_before_bits_);
  event << '>';
  AppendHex(event,
            LoadU32(base_,
                    collector_ +
                        trick::GrindCollectorLayout::kPublishedRewardSnapshot));
  event << ':';
  AppendHex(event, grind_before_bits_);
  event << '>';
  AppendHex(
      event,
      holder_ ? LoadU32(base_,
                        holder_ + trick::ScoreHolderLayout::kPendingGrindReward)
              : 0);
  PushScoreRewardEvent(event.str());
}

WipeoutRequestedObservationScope::WipeoutRequestedObservationScope(
    PPCContext &ctx, uint8_t *)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      player_(ctx.r3.u32), caller_(ctx.lr),
      focused_(g_focused.load(std::memory_order_acquire)) {}

WipeoutRequestedObservationScope::~WipeoutRequestedObservationScope() {
  const bool requested = (ctx_.r3.u32 & 0xFFu) != 0;
  g_wipeout_request_check_count.fetch_add(1, std::memory_order_relaxed);
  g_last_wipeout_request_player.store(player_, std::memory_order_release);
  if (requested) {
    g_wipeout_requested_true_count.fetch_add(1, std::memory_order_relaxed);
  }
  // This function is polled for several physical players multiple times per
  // frame. Preserve aggregate false counts, but only spend focused-event
  // capacity when retail actually requests a wipeout.
  if (!requested || !focused_ || !BeginEvent(EventKind::WipeoutRequested)) {
    return;
  }
  std::ostringstream event;
  event << "WR@" << frame_ << ':';
  AppendHex(event, player_);
  event << ':' << (requested ? 1 : 0) << ':';
  AppendHex(event, caller_);
  PushFocusedEvent(event.str());
}

CollisionForceWipeoutObservationScope::CollisionForceWipeoutObservationScope(
    PPCContext &ctx, uint8_t *)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      skeleton_(ctx.r3.u32), argument_1_bits_(ctx.r4.u32),
      argument_2_bits_(ctx.r5.u32), caller_(ctx.lr),
      focused_(g_focused.load(std::memory_order_acquire)) {}

CollisionForceWipeoutObservationScope::
    ~CollisionForceWipeoutObservationScope() {
  const bool force_wipeout = (ctx_.r3.u32 & 0xFFu) != 0;
  g_collision_force_wipeout_check_count.fetch_add(1, std::memory_order_relaxed);
  if (force_wipeout) {
    g_collision_force_wipeout_true_count.fetch_add(1,
                                                   std::memory_order_relaxed);
  }
  g_collision_force_wipeout_last_skeleton.store(skeleton_,
                                                std::memory_order_release);
  g_collision_force_wipeout_last_argument_1_bits.store(
      argument_1_bits_, std::memory_order_release);
  g_collision_force_wipeout_last_argument_2_bits.store(
      argument_2_bits_, std::memory_order_release);
  // Do not emit focused events here. This provisional community symbol
  // returns true during ordinary collision processing and previously crowded
  // out the actor-owned PhysicsWantsWipeOut event stream.
}

SkeletonAnimAttributesObservationScope::
    SkeletonAnimAttributesObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      skeleton_(ctx.r3.u32),
      focused_(g_focused.load(std::memory_order_acquire)) {
  if (!base_ || !skeleton_ || !focused_ ||
      !BeginEvent(EventKind::SkeletonAnimAttributes)) {
    return;
  }

  // ProcessAnimAttributes reads the current AnimOut attribute vector through
  // Skeleton+11700. TU3 stores [begin,end) at +2672/+2676 and each native
  // Attribute record occupies 64 bytes. The FastString<36> name begins at
  // record+32; preserve the remaining record as raw words until each field's
  // retail meaning is proven.
  const uint32_t anim_out = LoadU32(base_, skeleton_ + 11700);
  if (!anim_out) {
    return;
  }
  const uint32_t begin = LoadU32(base_, anim_out + 2672);
  const uint32_t end = LoadU32(base_, anim_out + 2676);
  if (!begin || end < begin || ((end - begin) % 64) != 0 ||
      (end - begin) > 64 * 128) {
    return;
  }

  const uint64_t injection_frame =
      g_outer_circle_last_injection_frame.load(std::memory_order_acquire);
  uint32_t focused_skeleton =
      g_anim_attribute_focus_skeleton.load(std::memory_order_acquire);
  if (!focused_skeleton && injection_frame && frame_ >= injection_frame &&
      frame_ - injection_frame <= 8) {
    for (uint32_t record = begin; record < end; record += 64) {
      if (DecodeFastString30(base_, record + 32) == "FLIPTRICK" &&
          LoadU32(base_, record) != 0) {
        uint32_t expected = 0;
        g_anim_attribute_focus_skeleton.compare_exchange_strong(
            expected, skeleton_, std::memory_order_acq_rel,
            std::memory_order_acquire);
        focused_skeleton =
            g_anim_attribute_focus_skeleton.load(std::memory_order_acquire);
        break;
      }
    }
  }
  if (!focused_skeleton || skeleton_ != focused_skeleton) {
    return;
  }

  for (uint32_t record = begin; record < end; record += 64) {
    const std::string name = DecodeFastString30(base_, record + 32);
    if (name.empty()) {
      continue;
    }
    const bool relevant =
        name.find("FLIP") != std::string::npos ||
        name.find("TRICK") != std::string::npos ||
        name.find("BOARD") != std::string::npos ||
        name.find("FOOT") != std::string::npos ||
        name.find("LAND") != std::string::npos ||
        name.find("CATCH") != std::string::npos ||
        name.find("JUMP") != std::string::npos ||
        name.find("OFFSET") != std::string::npos ||
        name.find("GRIND") != std::string::npos ||
        name.find("BODYADJUST") != std::string::npos;
    if (!relevant) {
      continue;
    }

    std::ostringstream signature;
    for (uint32_t offset = 0; offset < 64; offset += 4) {
      AppendHex(signature, LoadU32(base_, record + offset));
      signature << ':';
    }

    std::ostringstream event;
    event << "PA@" << frame_ << ':';
    AppendHex(event, skeleton_);
    event << ':' << name;
    for (uint32_t offset = 0; offset < 64; offset += 4) {
      event << ':';
      AppendHex(event, LoadU32(base_, record + offset));
    }

    std::ostringstream key;
    key << "PA:";
    AppendHex(key, skeleton_);
    key << ':' << name;
    PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
  }
}

CacGestureFinalPoseObservationScope::CacGestureFinalPoseObservationScope(
    PPCContext &ctx, uint8_t *base) {
  if (!base || !cac_gesture::IsActive() || !ctx.r3.u32 || !ctx.r4.u32) {
    return;
  }
  const uint32_t pose = LoadU32(base, ctx.r4.u32);
  if (!pose) {
    return;
  }
  const uint32_t player_entity = input_lab::CurrentObservedPlayerEntity();
  const uint32_t player_actor =
      g_local_action_graph_actor.load(std::memory_order_acquire);
  const uint32_t player_skater_anim =
      player_actor ? LoadU32(base, player_actor + 1804) : 0;
  static std::mutex ownership_mutex;
  static std::map<uint32_t, bool> ownership_logged;
  {
    std::lock_guard lock(ownership_mutex);
    if (!ownership_logged.contains(ctx.r3.u32)) {
      ownership_logged[ctx.r3.u32] = true;
      std::ostringstream matches;
      std::ostringstream allocation_matches;
      std::ostringstream nested_matches;
      const uint32_t skeleton_interface =
          LoadU32(base, ctx.r3.u32 + 11696);
      if (player_entity) {
        for (uint32_t offset = 0; offset < 11712; offset += 4) {
          if (LoadU32(base, ctx.r3.u32 + offset) == player_entity) {
            if (!matches.str().empty()) {
              matches << ',';
            }
            matches << offset;
          }
        }
      }
      if (player_skater_anim >= 14960) {
        const uint32_t allocation = player_skater_anim - 14960;
        for (uint32_t offset = 0; offset < 15328; offset += 4) {
          const uint32_t value = LoadU32(base, allocation + offset);
          if (value == ctx.r3.u32 || value == ctx.r4.u32 ||
              value == skeleton_interface) {
            if (!allocation_matches.str().empty()) {
              allocation_matches << ',';
            }
            allocation_matches << offset << '=';
            AppendHex(allocation_matches, value);
          }
          if (value >= 0x40000000 && value < 0x50000000) {
            for (uint32_t nested_offset = 0; nested_offset < 256;
                 nested_offset += 4) {
              const uint32_t nested = LoadU32(base, value + nested_offset);
              if (nested == ctx.r3.u32 || nested == ctx.r4.u32 ||
                  nested == skeleton_interface) {
                if (!nested_matches.str().empty()) {
                  nested_matches << ',';
                }
                nested_matches << offset << '>' << nested_offset << '=';
                AppendHex(nested_matches, nested);
              }
            }
          }
        }
      }
      REXLOG_WARN(
          "cac-gesture: pose-owner-probe skeleton=0x{:08X} "
          "anim_in=0x{:08X} player_entity=0x{:08X} "
          "player_actor=0x{:08X} player_skater_anim=0x{:08X} "
          "interface=0x{:08X} direct_offsets={} allocation_matches={} "
          "nested_matches={}",
          ctx.r3.u32, ctx.r4.u32, player_entity,
          player_actor, player_skater_anim,
          skeleton_interface, matches.str(), allocation_matches.str(),
          nested_matches.str());
    }
  }
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  static std::mutex sample_mutex;
  static std::map<uint32_t, uint64_t> last_sample_frames;
  static std::atomic<uint32_t> sample_count{0};
  {
    std::lock_guard lock(sample_mutex);
    const auto previous = last_sample_frames.find(ctx.r3.u32);
    if (previous != last_sample_frames.end() &&
        frame - previous->second < 10) {
      return;
    }
    last_sample_frames[ctx.r3.u32] = frame;
  }
  const uint32_t observation =
      sample_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (observation > 120) {
    return;
  }

  std::ostringstream matrices;
  constexpr std::array<uint32_t, 17> kGestureBones{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
  for (const uint32_t bone : kGestureBones) {
    if (bone != kGestureBones.front()) {
      matrices << ';';
    }
    matrices << bone << '@';
    const uint32_t matrix = pose + bone * 64;
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t column = 0; column < 3; ++column) {
        if (row || column) {
          matrices << ':';
        }
        AppendHex(matrices,
                  LoadU32(base, matrix + row * 16 + column * 4));
      }
    }
  }
  REXLOG_WARN(
      "cac-gesture: final-pose observation={} frame={} phase={} selected={} "
      "skeleton=0x{:08X} anim_in=0x{:08X} pose=0x{:08X} matrices={}",
      observation, frame, cac_gesture::Phase(), cac_gesture::Selected(),
      ctx.r3.u32, ctx.r4.u32, pose, matrices.str());
}

BlendPoseStreamObservationScope::BlendPoseStreamObservationScope(
    PPCContext &ctx, uint8_t *base) {
  if (!base) {
    return;
  }
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  // Static instruction flow verifies the runtime argument assignment:
  // r5=selected Clip*, r6=leaf FastString<30>, r7=SQTExt*, r8=int, r9=bool.
  // The community demangler is retained as provenance but its rendered
  // parameter order is not trusted over the generated TU3 body.
  const std::string stream_name = DecodeFastString30(base, ctx.r6.u32);
  if (stream_name.empty()) {
    return;
  }
  {
    std::lock_guard lock(g_gesture_stream_mutex);
    if (g_gesture_streams.size() < 2048 ||
        g_gesture_streams.contains(ctx.r5.u32)) {
      g_gesture_streams[ctx.r5.u32] = stream_name;
    }
  }
  if (cac_gesture::IsActive()) {
    REXLOG_WARN(
        "cac-gesture: stream frame={} phase={} selected={} name='{}' "
        "clip=0x{:08X} sqt=0x{:08X} blend={} option={}",
        frame, cac_gesture::Phase(), cac_gesture::Selected(), stream_name,
        ctx.r5.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
  }
  if (!g_focused.load(std::memory_order_acquire) ||
      !BeginEvent(EventKind::BlendPoseStream)) {
    return;
  }
  std::ostringstream event;
  event << "PS@" << frame << ':';
  AppendHex(event, ctx.r3.u32);
  event << ':';
  AppendHex(event, ctx.r4.u32);
  event << ':' << stream_name << ':';
  AppendHex(event, ctx.r5.u32);
  event << ':';
  AppendHex(event, ctx.r7.u32);
  event << ':' << ctx.r8.u32 << ':' << ctx.r9.u32 << ':';
  AppendHex(event, ctx.lr);
  std::ostringstream key;
  key << "PS:";
  AppendHex(key, ctx.r3.u32);
  key << ':' << stream_name;
  std::ostringstream signature;
  AppendHex(signature, ctx.r5.u32);
  signature << ':';
  AppendHex(signature, ctx.r7.u32);
  signature << ':' << ctx.r8.u32 << ':' << ctx.r9.u32;
  PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
}

namespace {

bool IsFocusedTrickAnimationName(std::string_view name) {
  return name.find("360FLIP") != std::string_view::npos ||
         name.find("CODEX_720FLIP") != std::string_view::npos;
}

} // namespace

AnimationTreeLookupObservationScope::AnimationTreeLookupObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()),
      playback_(ctx.r3.u32), name_(ctx.r5.u32), caller_(ctx.lr),
      relevant_(false) {
  if (!base_ || !g_focused.load(std::memory_order_acquire)) {
    return;
  }
  relevant_ = IsFocusedTrickAnimationName(DecodeFastString(base_, name_));
}

AnimationTreeLookupObservationScope::~AnimationTreeLookupObservationScope() {
  if (!relevant_ || !BeginEvent(EventKind::AnimationTreeLookup)) {
    return;
  }
  std::ostringstream event;
  event << "AT@" << frame_ << ':';
  AppendHex(event, playback_);
  event << ':' << DecodeFastString(base_, name_) << ':';
  AppendHex(event, ctx_.r3.u32);
  event << ':';
  AppendHex(event, caller_);
  PushFocusedEvent(event.str());
}

PushAnimationAttributesObservationScope::
    PushAnimationAttributesObservationScope(PPCContext &ctx, uint8_t *base,
                                             const char *tag)
    : tag_(tag) {
  if (!base || !tag_ || !g_focused.load(std::memory_order_acquire)) {
    return;
  }
  const std::string animation = DecodeFastString(base, ctx.r6.u32);
  if (!IsFocusedTrickAnimationName(animation) ||
      !BeginEvent(EventKind::PushAnimationAttributes)) {
    return;
  }
  std::ostringstream event;
  event << tag_ << '@' << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.r3.u32);
  event << ':';
  AppendHex(event, ctx.r4.u32);
  event << ':' << animation << ':';
  AppendHex(event, ctx.r5.u32);
  event << ':';
  AppendHex(event, ctx.lr);
  PushFocusedEvent(event.str());
}

SkeletonIkBlendObservationScope::SkeletonIkBlendObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      skeleton_ik_(ctx.r3.u32),
      focused_(g_focused.load(std::memory_order_acquire) &&
               g_anim_attribute_focus_skeleton.load(
                   std::memory_order_acquire) == ctx.r3.u32 + 0xCB0) {
  if (focused_) {
    g_active_focused_skeleton_ik = skeleton_ik_;
  }
}

SkeletonIkBlendObservationScope::~SkeletonIkBlendObservationScope() {
  if (focused_ && g_active_focused_skeleton_ik == skeleton_ik_) {
    g_active_focused_skeleton_ik = 0;
  }
  if (!base_ || !skeleton_ik_ || !focused_ ||
      !BeginEvent(EventKind::SkeletonIkBlend)) {
    return;
  }
  std::ostringstream event;
  event << "IK@" << frame_ << ':';
  AppendHex(event, skeleton_ik_);
  for (uint32_t index = 0; index < 4; ++index) {
    const uint32_t weight = LoadU32(base_, skeleton_ik_ + 468 + index * 4);
    const uint32_t state = LoadU32(base_, skeleton_ik_ + 500 + index * 4);
    event << ':';
    AppendHex(event, weight);
    event << ':' << state;
  }
  // BlendTransforms consumes complete 4x4 foot target matrices at +1664 and
  // +1728. Retain only these verified inputs. The broader pre/post matrix
  // banks and Matrix::SLerp traces were useful during basis recovery but
  // displaced lifecycle events from the bounded focused-event buffer.
  for (uint32_t target = 0; target < 2; ++target) {
    const uint32_t matrix = skeleton_ik_ + 1664 + target * 64;
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t column = 0; column < 3; ++column) {
        event << ':';
        AppendHex(event, LoadU32(base_, matrix + row * 16 + column * 4));
      }
    }
  }
  PushFocusedEvent(event.str());
}

SkeletonIkMatrixSlerpObservationScope::
    SkeletonIkMatrixSlerpObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      caller_(ctx.lr), result_(ctx.r3.u32), first_(ctx.r4.u32),
      second_(ctx.r5.u32), weight_(ctx.r6.u32),
      focused_(g_focused.load(std::memory_order_acquire) &&
               g_active_focused_skeleton_ik != 0 &&
               (caller_ == 0x82BEF248 || caller_ == 0x82BEF524)) {
  if (!base_ || !focused_) {
    return;
  }
  for (size_t matrix = 0; matrix < 2; ++matrix) {
    const uint32_t address = matrix == 0 ? first_ : second_;
    for (size_t component = 0; component < 12; ++component) {
      const size_t row = component / 3;
      const size_t column = component % 3;
      candidates_[matrix * 12 + component] =
          LoadU32(base_, address + static_cast<uint32_t>(row * 16 +
                                                        column * 4));
    }
  }
}

SkeletonIkMatrixSlerpObservationScope::
    ~SkeletonIkMatrixSlerpObservationScope() {
  // Retain this narrowly filtered hook for future matrix work, but do not
  // emit its large per-frame payload during ordinary play or regressions.
}

SkateboardOffsetObservationScope::SkateboardOffsetObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()),
      skeleton_(ctx.r3.u32),
      focused_(g_focused.load(std::memory_order_acquire)),
      before_weight_0_(base && skeleton_ ? LoadU32(base, skeleton_ + 16392)
                                         : 0),
      before_weight_1_(base && skeleton_ ? LoadU32(base, skeleton_ + 16400)
                                         : 0),
      before_flag_0_(base && skeleton_ ? LoadU8(base, skeleton_ + 16389) : 0),
      before_flag_1_(base && skeleton_ ? LoadU8(base, skeleton_ + 16396) : 0) {}

SkateboardOffsetObservationScope::~SkateboardOffsetObservationScope() {
  if (!base_ || !skeleton_ || !focused_ ||
      !BeginEvent(EventKind::SkateboardOffset)) {
    return;
  }
  const uint32_t after_weight_0_ = LoadU32(base_, skeleton_ + 16392);
  const uint32_t after_weight_1_ = LoadU32(base_, skeleton_ + 16400);
  const uint8_t after_flag_0_ = LoadU8(base_, skeleton_ + 16389);
  const uint8_t after_flag_1_ = LoadU8(base_, skeleton_ + 16396);

  std::ostringstream signature;
  signature << before_weight_0_ << ':' << before_weight_1_ << ':'
            << static_cast<uint32_t>(before_flag_0_) << ':'
            << static_cast<uint32_t>(before_flag_1_) << ':'
            << after_weight_0_ << ':' << after_weight_1_ << ':'
            << static_cast<uint32_t>(after_flag_0_) << ':'
            << static_cast<uint32_t>(after_flag_1_);
  std::ostringstream event;
  event << "BO@" << frame_ << ':';
  AppendHex(event, skeleton_);
  event << ':';
  AppendHex(event, before_weight_0_);
  event << ':';
  AppendHex(event, before_weight_1_);
  event << ':' << static_cast<uint32_t>(before_flag_0_) << ':'
        << static_cast<uint32_t>(before_flag_1_) << ':';
  AppendHex(event, after_weight_0_);
  event << ':';
  AppendHex(event, after_weight_1_);
  event << ':' << static_cast<uint32_t>(after_flag_0_) << ':'
        << static_cast<uint32_t>(after_flag_1_) << ':';
  AppendHex(event, ctx_.lr);
  std::ostringstream key;
  key << "BO:";
  AppendHex(key, skeleton_);
  PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
}

PhysicsWantsWipeoutConditionFactoryObservationScope::
    PhysicsWantsWipeoutConditionFactoryObservationScope(PPCContext &ctx,
                                                        uint8_t *base)
    : ctx_(ctx), base_(base) {}

PhysicsWantsWipeoutConditionFactoryObservationScope::
    ~PhysicsWantsWipeoutConditionFactoryObservationScope() {
  const uint32_t object = ctx_.r3.u32;
  if (!base_ || !object) {
    return;
  }
  const uint32_t vtable = LoadU32(base_, object);
  g_physics_wants_wipeout_factory_count.fetch_add(1, std::memory_order_relaxed);
  g_physics_wants_wipeout_object.store(object, std::memory_order_release);
  g_physics_wants_wipeout_vtable.store(vtable, std::memory_order_release);
  if (!vtable) {
    return;
  }
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    g_physics_wants_wipeout_vtable_slots[slot].store(
        LoadU32(base_, vtable + static_cast<uint32_t>(slot * 4)),
        std::memory_order_release);
  }
}

PhysicsWantsWipeoutConditionEvaluationScope::
    PhysicsWantsWipeoutConditionEvaluationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()), context_(ctx.r4.u32),
      actor_(base && context_ ? LoadU32(base, context_ + 4) : 0),
      skater_anim_interface_(base && actor_ ? LoadU32(base, actor_ + 1808) : 0),
      focused_(g_focused.load(std::memory_order_acquire)) {}

PhysicsWantsWipeoutConditionEvaluationScope::
    ~PhysicsWantsWipeoutConditionEvaluationScope() {
  const bool wants_wipeout = (ctx_.r3.u32 & 0xFFu) != 0;
  g_physics_wants_wipeout_check_count.fetch_add(1, std::memory_order_relaxed);
  if (wants_wipeout) {
    g_physics_wants_wipeout_true_count.fetch_add(1, std::memory_order_relaxed);
    if (actor_ != 0 &&
        actor_ == g_local_action_graph_actor.load(std::memory_order_acquire)) {
      g_local_physics_wants_wipeout_true_count.fetch_add(
          1, std::memory_order_relaxed);
      custom_trick::ObservePhysicsWantsWipeout(frame_, actor_);
    }
    g_physics_wants_wipeout_last_true_frame.store(frame_,
                                                  std::memory_order_release);
    g_physics_wants_wipeout_last_true_context.store(context_,
                                                    std::memory_order_release);
    g_physics_wants_wipeout_last_true_actor.store(actor_,
                                                  std::memory_order_release);
    g_physics_wants_wipeout_last_true_skater_anim.store(
        skater_anim_interface_, std::memory_order_release);
  }
  g_physics_wants_wipeout_last_context.store(context_,
                                             std::memory_order_release);
  g_physics_wants_wipeout_last_actor.store(actor_, std::memory_order_release);
  g_physics_wants_wipeout_last_skater_anim.store(skater_anim_interface_,
                                                 std::memory_order_release);
  if (!wants_wipeout || !focused_ ||
      !BeginEvent(EventKind::PhysicsWantsWipeout)) {
    return;
  }
  std::ostringstream event;
  event << "PW@" << frame_ << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, actor_);
  event << ':';
  AppendHex(event, skater_anim_interface_);
  PushFocusedEvent(event.str());
}

namespace {

void CaptureLandingConditionFactory(
    PPCContext &ctx, uint8_t *base, std::atomic<uint64_t> &count,
    std::atomic<uint32_t> &object_out, std::atomic<uint32_t> &vtable_out,
    std::array<std::atomic<uint32_t>, kPhysicsWantsWipeoutVtableSlotCount>
        &slots_out) {
  const uint32_t object = ctx.r3.u32;
  if (!base || !object) {
    return;
  }
  const uint32_t vtable = LoadU32(base, object);
  count.fetch_add(1, std::memory_order_relaxed);
  object_out.store(object, std::memory_order_release);
  vtable_out.store(vtable, std::memory_order_release);
  for (size_t slot = 0; slot < slots_out.size(); ++slot) {
    slots_out[slot].store(
        vtable ? LoadU32(base, vtable + static_cast<uint32_t>(slot * 4)) : 0,
        std::memory_order_release);
  }
}

} // namespace

CanLandOnBoardConditionFactoryObservationScope::
    CanLandOnBoardConditionFactoryObservationScope(PPCContext &ctx,
                                                   uint8_t *base)
    : ctx_(ctx), base_(base) {}

CanLandOnBoardConditionFactoryObservationScope::
    ~CanLandOnBoardConditionFactoryObservationScope() {
  CaptureLandingConditionFactory(ctx_, base_, g_can_land_on_board_factory_count,
                                 g_can_land_on_board_object,
                                 g_can_land_on_board_vtable,
                                 g_can_land_on_board_vtable_slots);
}

TiltTooLargeForPrelandConditionFactoryObservationScope::
    TiltTooLargeForPrelandConditionFactoryObservationScope(PPCContext &ctx,
                                                           uint8_t *base)
    : ctx_(ctx), base_(base) {}

TiltTooLargeForPrelandConditionFactoryObservationScope::
    ~TiltTooLargeForPrelandConditionFactoryObservationScope() {
  CaptureLandingConditionFactory(
      ctx_, base_, g_tilt_too_large_for_preland_factory_count,
      g_tilt_too_large_for_preland_object, g_tilt_too_large_for_preland_vtable,
      g_tilt_too_large_for_preland_vtable_slots);
}

IsLandingOnBoardConditionFactoryObservationScope::
    IsLandingOnBoardConditionFactoryObservationScope(PPCContext &ctx,
                                                     uint8_t *base)
    : ctx_(ctx), base_(base) {}

IsLandingOnBoardConditionFactoryObservationScope::
    ~IsLandingOnBoardConditionFactoryObservationScope() {
  CaptureLandingConditionFactory(
      ctx_, base_, g_is_landing_on_board_factory_count,
      g_is_landing_on_board_object, g_is_landing_on_board_vtable,
      g_is_landing_on_board_vtable_slots);
}

IsLandingConditionFactoryObservationScope::
    IsLandingConditionFactoryObservationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base) {}

IsLandingConditionFactoryObservationScope::
    ~IsLandingConditionFactoryObservationScope() {
  CaptureLandingConditionFactory(ctx_, base_, g_is_landing_factory_count,
                                 g_is_landing_object, g_is_landing_vtable,
                                 g_is_landing_vtable_slots);
}

CanLandOnBoardConditionEvaluationScope::CanLandOnBoardConditionEvaluationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      context_(ctx.r4.u32),
      actor_(base && context_ ? LoadU32(base, context_ + 4) : 0) {}

CanLandOnBoardConditionEvaluationScope::
    ~CanLandOnBoardConditionEvaluationScope() {
  const bool result = (ctx_.r3.u32 & 0xFFu) != 0;
  const bool local = actor_ != 0 && actor_ == g_local_action_graph_actor.load(
                                                  std::memory_order_acquire);
  g_can_land_on_board_check_count.fetch_add(1, std::memory_order_relaxed);
  if (result) {
    g_can_land_on_board_true_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (local) {
    g_local_can_land_on_board_check_count.fetch_add(1,
                                                    std::memory_order_relaxed);
    if (result) {
      g_local_can_land_on_board_true_count.fetch_add(1,
                                                     std::memory_order_relaxed);
    }
  }
  g_can_land_on_board_last_frame.store(frame_, std::memory_order_release);
  g_can_land_on_board_last_context.store(context_, std::memory_order_release);
  g_can_land_on_board_last_actor.store(actor_, std::memory_order_release);
  g_can_land_on_board_last_result.store(result ? 1 : 0,
                                        std::memory_order_release);
  if (!local || !g_focused.load(std::memory_order_acquire) ||
      !BeginEvent(EventKind::CanLandOnBoard)) {
    return;
  }
  std::ostringstream event;
  event << "CL@" << frame_ << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, actor_);
  event << ':' << (result ? 1 : 0);
  PushFocusedEventIfChanged("CL:local", result ? "1" : "0", event.str());
}

TiltTooLargeForPrelandConditionEvaluationScope::
    TiltTooLargeForPrelandConditionEvaluationScope(PPCContext &ctx,
                                                   uint8_t *base)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      context_(ctx.r4.u32),
      actor_(base && context_ ? LoadU32(base, context_ + 4) : 0),
      tilt_interface_(base && actor_ ? LoadU32(base, actor_ + 1800) : 0) {}

TiltTooLargeForPrelandConditionEvaluationScope::
    ~TiltTooLargeForPrelandConditionEvaluationScope() {
  const bool result = (ctx_.r3.u32 & 0xFFu) != 0;
  const bool local = actor_ != 0 && actor_ == g_local_action_graph_actor.load(
                                                  std::memory_order_acquire);
  g_tilt_too_large_for_preland_check_count.fetch_add(1,
                                                     std::memory_order_relaxed);
  if (result) {
    g_tilt_too_large_for_preland_true_count.fetch_add(
        1, std::memory_order_relaxed);
  }
  if (local) {
    g_local_tilt_too_large_for_preland_check_count.fetch_add(
        1, std::memory_order_relaxed);
    if (result) {
      g_local_tilt_too_large_for_preland_true_count.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
  g_tilt_too_large_for_preland_last_frame.store(frame_,
                                                std::memory_order_release);
  g_tilt_too_large_for_preland_last_context.store(context_,
                                                  std::memory_order_release);
  g_tilt_too_large_for_preland_last_actor.store(actor_,
                                                std::memory_order_release);
  g_tilt_too_large_for_preland_last_interface.store(tilt_interface_,
                                                    std::memory_order_release);
  g_tilt_too_large_for_preland_last_result.store(result ? 1 : 0,
                                                 std::memory_order_release);
  if (!local || !g_focused.load(std::memory_order_acquire) ||
      !BeginEvent(EventKind::TiltTooLargeForPreland)) {
    return;
  }
  std::ostringstream event;
  event << "TL@" << frame_ << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, actor_);
  event << ':';
  AppendHex(event, tilt_interface_);
  event << ':' << (result ? 1 : 0);
  PushFocusedEventIfChanged("TL:local", result ? "1" : "0", event.str());
}

IsLandingOnBoardConditionEvaluationScope::
    IsLandingOnBoardConditionEvaluationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      context_(ctx.r4.u32),
      actor_(base && context_ ? LoadU32(base, context_ + 4) : 0) {}

IsLandingOnBoardConditionEvaluationScope::
    ~IsLandingOnBoardConditionEvaluationScope() {
  const bool result = (ctx_.r3.u32 & 0xFFu) != 0;
  const bool local = actor_ != 0 && actor_ == g_local_action_graph_actor.load(
                                                  std::memory_order_acquire);
  g_is_landing_on_board_check_count.fetch_add(1, std::memory_order_relaxed);
  if (result) {
    g_is_landing_on_board_true_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (local) {
    g_local_is_landing_on_board_check_count.fetch_add(
        1, std::memory_order_relaxed);
    if (result) {
      g_local_is_landing_on_board_true_count.fetch_add(
          1, std::memory_order_relaxed);
    }
  }
  g_is_landing_on_board_last_frame.store(frame_, std::memory_order_release);
  g_is_landing_on_board_last_actor.store(actor_, std::memory_order_release);
  g_is_landing_on_board_last_result.store(result ? 1 : 0,
                                          std::memory_order_release);
  if (!local || !g_focused.load(std::memory_order_acquire) ||
      !BeginEvent(EventKind::IsLandingOnBoard)) {
    return;
  }
  std::ostringstream event;
  event << "LB@" << frame_ << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, actor_);
  event << ':' << (result ? 1 : 0);
  PushFocusedEventIfChanged("LB:local", result ? "1" : "0", event.str());
}

IsLandingConditionEvaluationScope::IsLandingConditionEvaluationScope(
    PPCContext &ctx, uint8_t *base, bool on_board_mode)
    : ctx_(ctx), frame_(input_history_watch::CurrentFrameSequence()),
      context_(ctx.r4.u32),
      actor_(base && context_ ? LoadU32(base, context_ + 4) : 0),
      landing_interface_(base && actor_ ? LoadU32(base, actor_ + 1800) : 0),
      on_board_mode_(on_board_mode) {}

IsLandingConditionEvaluationScope::~IsLandingConditionEvaluationScope() {
  const bool result = (ctx_.r3.u32 & 0xFFu) != 0;
  const bool local = actor_ != 0 && actor_ == g_local_action_graph_actor.load(
                                                  std::memory_order_acquire);
  const size_t mode = on_board_mode_ ? 1 : 0;
  g_is_landing_check_counts[mode].fetch_add(1, std::memory_order_relaxed);
  if (result) {
    g_is_landing_true_counts[mode].fetch_add(1, std::memory_order_relaxed);
  }
  if (local) {
    g_local_is_landing_check_counts[mode].fetch_add(1,
                                                    std::memory_order_relaxed);
    if (result) {
      g_local_is_landing_true_counts[mode].fetch_add(1,
                                                     std::memory_order_relaxed);
    }
  }
  g_is_landing_last_frames[mode].store(frame_, std::memory_order_release);
  g_is_landing_last_actors[mode].store(actor_, std::memory_order_release);
  g_is_landing_last_interfaces[mode].store(landing_interface_,
                                           std::memory_order_release);
  g_is_landing_last_results[mode].store(result ? 1 : 0,
                                        std::memory_order_release);
  if (local) {
    custom_trick::ObserveLandingPolicy(frame_, actor_, on_board_mode_, result);
  }
  if (!local || !g_focused.load(std::memory_order_acquire) ||
      !BeginEvent(EventKind::IsLanding)) {
    return;
  }
  std::ostringstream event;
  event << "IL@" << frame_ << ':' << mode << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, actor_);
  event << ':';
  AppendHex(event, landing_interface_);
  event << ':' << (result ? 1 : 0);
  PushFocusedEventIfChanged("IL:local:" + std::to_string(mode),
                            result ? "1" : "0", event.str());
}

IsOffboardConditionFactoryObservationScope::
    IsOffboardConditionFactoryObservationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base) {}

IsOffboardConditionFactoryObservationScope::
    ~IsOffboardConditionFactoryObservationScope() {
  const uint32_t object = ctx_.r3.u32;
  if (!base_ || !object) {
    return;
  }
  const uint32_t vtable = LoadU32(base_, object);
  if (!vtable) {
    return;
  }
  g_is_offboard_condition_factory_count.fetch_add(1, std::memory_order_relaxed);
  g_is_offboard_condition_object.store(object, std::memory_order_release);
  g_is_offboard_condition_vtable.store(vtable, std::memory_order_release);
  for (size_t slot = 0; slot < kIsOffboardVtableSlotCount; ++slot) {
    g_is_offboard_condition_vtable_slots[slot].store(
        LoadU32(base_, vtable + static_cast<uint32_t>(slot * 4)),
        std::memory_order_release);
  }
}

IsAirOffboardConditionFactoryObservationScope::
    IsAirOffboardConditionFactoryObservationScope(PPCContext &ctx,
                                                  uint8_t *base)
    : ctx_(ctx), base_(base) {}

IsAirOffboardConditionFactoryObservationScope::
    ~IsAirOffboardConditionFactoryObservationScope() {
  const uint32_t object = ctx_.r3.u32;
  if (!base_ || !object) {
    return;
  }
  const uint32_t vtable = LoadU32(base_, object);
  if (!vtable) {
    return;
  }
  g_is_air_offboard_condition_factory_count.fetch_add(
      1, std::memory_order_relaxed);
  g_is_air_offboard_condition_object.store(object, std::memory_order_release);
  g_is_air_offboard_condition_vtable.store(vtable, std::memory_order_release);
  for (size_t slot = 0; slot < kIsOffboardVtableSlotCount; ++slot) {
    g_is_air_offboard_condition_vtable_slots[slot].store(
        LoadU32(base_, vtable + static_cast<uint32_t>(slot * 4)),
        std::memory_order_release);
  }
}

IsOffboardConditionEvaluationScope::IsOffboardConditionEvaluationScope(
    PPCContext &ctx, uint8_t *base, bool air_condition)
    : ctx_(ctx), base_(base),
      frame_(input_history_watch::CurrentFrameSequence()),
      condition_(ctx.r3.u32), context_(ctx.r4.u32),
      owner_(context_ ? LoadU32(base_, context_ + 12) : 0),
      provider_(owner_ ? LoadU32(base_, owner_ + 616) : 0),
      air_condition_(air_condition),
      focused_(g_focused.load(std::memory_order_acquire)) {
  const uint32_t provider_vtable = provider_ ? LoadU32(base_, provider_) : 0;
  if (provider_vtable) {
    g_offboard_provider_vtable.store(provider_vtable,
                                     std::memory_order_release);
    g_offboard_provider_method_608.store(LoadU32(base_, provider_vtable + 608),
                                         std::memory_order_release);
    g_offboard_provider_method_612.store(LoadU32(base_, provider_vtable + 612),
                                         std::memory_order_release);
    g_offboard_provider_method_616.store(LoadU32(base_, provider_vtable + 616),
                                         std::memory_order_release);
  }
}

IsOffboardConditionEvaluationScope::~IsOffboardConditionEvaluationScope() {
  const bool result = (ctx_.r3.u32 & 0xFFu) != 0;
  auto &check_count = air_condition_ ? g_is_air_offboard_condition_check_count
                                     : g_is_offboard_condition_check_count;
  auto &true_count = air_condition_ ? g_is_air_offboard_condition_true_count
                                    : g_is_offboard_condition_true_count;
  auto &last_result = air_condition_ ? g_is_air_offboard_condition_last_result
                                     : g_is_offboard_condition_last_result;
  check_count.fetch_add(1, std::memory_order_relaxed);
  if (result) {
    true_count.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t previous =
      last_result.exchange(result ? 1u : 0u, std::memory_order_acq_rel);
  g_is_offboard_condition_last_context.store(context_,
                                             std::memory_order_release);
  g_is_offboard_condition_last_owner.store(owner_, std::memory_order_release);
  g_is_offboard_condition_last_provider.store(provider_,
                                              std::memory_order_release);
  if (!focused_ || previous == (result ? 1u : 0u) ||
      !BeginEvent(EventKind::IsOffboardCondition)) {
    return;
  }
  std::ostringstream event;
  event << (air_condition_ ? "OA@" : "OB@") << frame_ << ':' << (result ? 1 : 0)
        << ':';
  AppendHex(event, condition_);
  event << ':';
  AppendHex(event, context_);
  event << ':';
  AppendHex(event, owner_);
  event << ':';
  AppendHex(event, provider_);
  PushFocusedEvent(event.str());
}

TrickDisplayRefreshObservationScope::TrickDisplayRefreshObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      manager_(ctx.r3.u32), caller_(ctx.lr),
      armed_(g_armed.load(std::memory_order_acquire)), focused_(false) {
  if (armed_) {
    g_counts[static_cast<size_t>(EventKind::TrickDisplayRefresh)].fetch_add(
        1, std::memory_order_relaxed);
    focused_ = g_focused.load(std::memory_order_acquire);
  }
}

TrickDisplayRefreshObservationScope::~TrickDisplayRefreshObservationScope() {
  if (!manager_) {
    return;
  }
  const uint32_t selected_index = LoadU32(base_, manager_ + 8);
  const std::string retail_text = LoadTelemetryText(base_, manager_ + 28, 127);
  const auto display_override =
      trick_overrides::FindDisplayNameOverride(selected_index);
  if (display_override.applied) {
    StoreDisplayText(base_, manager_ + 28, display_override.display_name);
    g_display_name_override_count.fetch_add(1, std::memory_order_relaxed);
  }
  if (!armed_ || !focused_) {
    return;
  }
  const std::string final_text = LoadTelemetryText(base_, manager_ + 28, 127);
  std::ostringstream event;
  event << "TD@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, manager_);
  event << ':';
  AppendHex(event, selected_index);
  event << ':'
        << (display_override.applied ? display_override.rule_name
                                     : std::string_view{"none"})
        << ':' << retail_text << ':' << final_text;
  std::ostringstream key;
  key << "TD:";
  AppendHex(key, manager_);
  PushFocusedEventIfChanged(key.str(), final_text, event.str());
}

void ObserveScoreModuleUpdate(PPCContext &ctx, uint8_t *base) {
  const uint32_t module = ctx.r3.u32;
  const uint32_t external_input = ctx.r4.u32;
  const uint32_t phys_out =
      LoadU32(base, module + trick::ScoreModuleLayout::kPhysOut);
  const uint32_t air_collector =
      LoadU32(base, module + trick::ScoreModuleLayout::kAirCollector);
  const uint32_t holder =
      LoadU32(base, module + trick::ScoreModuleLayout::kScoreHolder);
  if (ResolveLocalPhysOut(base, phys_out) && holder) {
    g_local_score_module.store(module, std::memory_order_release);
    g_local_score_holder.store(holder, std::memory_order_release);
  }
  custom_trick::ObserveScoreModuleOwnership(
      input_history_watch::CurrentFrameSequence(), phys_out, holder);
  if (!BeginEvent(EventKind::ScoreUpdate)) {
    return;
  }
  const uint32_t flags = LoadU32(base, external_input + 2788);
  std::ostringstream event;
  event << "S@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.lr);
  event << ':';
  AppendHex(event, module);
  event << ':';
  AppendHex(event, external_input);
  event << ':';
  AppendHex(event, flags);
  event << ':';
  AppendHex(event, phys_out);
  event << ':';
  AppendHex(event, air_collector);
  event << ':';
  AppendHex(event, holder);
  std::ostringstream signature;
  AppendHex(signature, external_input);
  signature << ':';
  AppendHex(signature, flags);
  signature << ':';
  AppendHex(signature, phys_out);
  signature << ':';
  AppendHex(signature, air_collector);
  signature << ':';
  AppendHex(signature, holder);
  std::ostringstream key;
  key << "S:";
  AppendHex(key, module);
  PushFocusedEventIfChanged(key.str(), signature.str(), event.str());
}

void PublishCustomScorableIdAfterInputUpdate(uint8_t* base, uint32_t module,
                                             uint32_t external_input) {
  g_custom_scorable_publish_call_count.fetch_add(1, std::memory_order_relaxed);
  if (!base || !module || !external_input) {
    return;
  }
  // The retail descriptor-to-ID conditioner cannot find a high-namespace
  // registry entry. Extend that exact boundary only for the player-owned
  // ScoreModule during the bounded custom state-entry window.
  const uint32_t phys_out =
      LoadU32(base, module + trick::ScoreModuleLayout::kPhysOut);
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  if (phys_out == g_local_phys_out.load(std::memory_order_acquire)) {
    g_custom_scorable_publish_local_count.fetch_add(1,
                                                     std::memory_order_relaxed);
    g_custom_scorable_publish_last_frame.store(frame,
                                                std::memory_order_release);
    g_custom_scorable_publish_last_module.store(module,
                                                 std::memory_order_release);
    g_custom_scorable_publish_last_phys_out.store(
        phys_out, std::memory_order_release);
  }
  const bool publish_custom =
      custom_trick::ShouldPublishActiveScorable(frame, phys_out);
  const bool reserve_outer_circle =
      phys_out == g_local_phys_out.load(std::memory_order_acquire) &&
      REXCVAR_GET(skate3_custom_input_outer_circle) &&
      REXCVAR_GET(skate3_custom_trick_native_graph) &&
      input_history_watch::OuterCircleGestureActive(0);
  if (!publish_custom && !reserve_outer_circle) {
    return;
  }
  if (publish_custom) {
    g_custom_scorable_publish_accept_count.fetch_add(
        1, std::memory_order_relaxed);
  }
  const uint32_t owner = LoadU32(base, module);
  const uint32_t score_output = owner ? LoadU32(base, owner + 60) : 0;
  g_custom_scorable_publish_last_output.store(score_output,
                                               std::memory_order_release);
  if (score_output) {
    g_custom_scorable_publish_last_before.store(
        LoadU32(base, score_output + 152), std::memory_order_release);
    REX_STORE_U32(score_output + 152,
                  publish_custom ? custom_trick::ActiveScorableId()
                                 : 0xFFFFFFFFu);
    g_custom_scorable_publish_last_after.store(
        LoadU32(base, score_output + 152), std::memory_order_release);
  }
}

uint32_t CurrentLocalPhysOut() {
  return g_local_phys_out.load(std::memory_order_acquire);
}

bool CurrentLocalBoardPosition(float out_position[3]) {
  if (out_position == nullptr) {
    return false;
  }
  LiveSpatialSnapshot snapshot;
  if (!CurrentLiveSpatialSnapshot(snapshot)) {
    return false;
  }
  out_position[0] =
      std::bit_cast<float>(snapshot.position_bits[0]);
  out_position[1] =
      std::bit_cast<float>(snapshot.position_bits[1]);
  out_position[2] =
      std::bit_cast<float>(snapshot.position_bits[2]);
  return std::isfinite(out_position[0]) && std::isfinite(out_position[1]) &&
         std::isfinite(out_position[2]);
}

bool CurrentLiveSpatialSnapshot(LiveSpatialSnapshot &out) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    const uint64_t revision_before =
        g_local_spatial_revision.load(std::memory_order_acquire);
    if ((revision_before & 1u) != 0) {
      continue;
    }
    LiveSpatialSnapshot snapshot{};
    snapshot.frame =
        g_local_spatial_frame.load(std::memory_order_relaxed);
    snapshot.sample_time_us =
        g_local_spatial_sample_time_us.load(
            std::memory_order_relaxed);
    snapshot.phys_out =
        g_local_phys_out.load(std::memory_order_relaxed);
    snapshot.board_controller =
        g_local_board_controller.load(std::memory_order_relaxed);
    snapshot.board_body =
        g_local_board_body.load(std::memory_order_relaxed);
    snapshot.transform_state =
        g_local_board_transform_state.load(std::memory_order_relaxed);
    snapshot.position_bits = {
        g_local_board_position_x_bits.load(std::memory_order_relaxed),
        g_local_board_position_y_bits.load(std::memory_order_relaxed),
        g_local_board_position_z_bits.load(std::memory_order_relaxed),
    };
    snapshot.x_axis_bits = {
        g_local_board_x_axis_x_bits.load(std::memory_order_relaxed),
        g_local_board_x_axis_y_bits.load(std::memory_order_relaxed),
        g_local_board_x_axis_z_bits.load(std::memory_order_relaxed),
    };
    snapshot.z_axis_bits = {
        g_local_board_z_axis_x_bits.load(std::memory_order_relaxed),
        g_local_board_z_axis_y_bits.load(std::memory_order_relaxed),
        g_local_board_z_axis_z_bits.load(std::memory_order_relaxed),
    };
    snapshot.board_state_flags =
        g_board_state_last_packed.load(std::memory_order_relaxed);
    const uint64_t revision_after =
        g_local_spatial_revision.load(std::memory_order_acquire);
    if (revision_before == revision_after &&
        (revision_after & 1u) == 0) {
      if (snapshot.frame == 0 || snapshot.phys_out == 0) {
        return false;
      }
      out = snapshot;
      return true;
    }
  }
  return false;
}

OwnedWorldCollisionBridgeScope::OwnedWorldCollisionBridgeScope(
    PPCContext& ctx, uint8_t* base)
    : ctx_(ctx),
      base_(base),
      controller_(ctx.r3.u32),
      phys_out_(ctx.r4.u32) {}

OwnedWorldCollisionBridgeScope::~OwnedWorldCollisionBridgeScope() {
  mechanics_sandbox::ApplyOwnedWorldCollisionAfterPhysOut(
      ctx_, base_, controller_, phys_out_);
}

void ObserveLocalSkateboardSpatialState(PPCContext &ctx, uint8_t *base) {
  const uint32_t controller = ctx.r3.u32;
  const uint32_t phys_out = ctx.r4.u32;
  if (!base || !controller || !phys_out) {
    return;
  }
  const uint32_t board_body = LoadU32(base, controller + 436);
  if (!board_body) {
    return;
  }
  const uint32_t state = LoadU32(base, controller + 448);
  const uint32_t transform = board_body + (state == 3 ? 112u : 192u);
  const uint64_t frame = input_history_watch::CurrentFrameSequence();
  const uint32_t position_x_bits = LoadU32(base, transform + 48);
  const uint32_t position_y_bits = LoadU32(base, transform + 52);
  const uint32_t position_z_bits = LoadU32(base, transform + 56);
  const uint32_t x_axis_x_bits = LoadU32(base, transform + 0);
  const uint32_t x_axis_y_bits = LoadU32(base, transform + 4);
  const uint32_t x_axis_z_bits = LoadU32(base, transform + 8);
  const uint32_t z_axis_x_bits = LoadU32(base, transform + 32);
  const uint32_t z_axis_y_bits = LoadU32(base, transform + 36);
  const uint32_t z_axis_z_bits = LoadU32(base, transform + 40);
  const uint64_t sample_time_us =
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
  {
    std::lock_guard lock(g_event_mutex);
    g_actor_spatial_snapshots[phys_out] = {frame, controller, position_x_bits,
                                           position_y_bits, position_z_bits};
  }

  if (phys_out != g_local_phys_out.load(std::memory_order_acquire)) {
    return;
  }

  g_local_spatial_revision.fetch_add(
      1, std::memory_order_acq_rel);
  g_local_board_controller.store(controller, std::memory_order_relaxed);
  g_local_board_body.store(board_body, std::memory_order_relaxed);
  g_local_board_transform_state.store(state, std::memory_order_relaxed);
  g_local_board_position_x_bits.store(position_x_bits,
                                      std::memory_order_relaxed);
  g_local_board_position_y_bits.store(position_y_bits,
                                      std::memory_order_relaxed);
  g_local_board_position_z_bits.store(position_z_bits,
                                      std::memory_order_relaxed);
  g_local_board_x_axis_x_bits.store(x_axis_x_bits, std::memory_order_relaxed);
  g_local_board_x_axis_y_bits.store(x_axis_y_bits, std::memory_order_relaxed);
  g_local_board_x_axis_z_bits.store(x_axis_z_bits, std::memory_order_relaxed);
  g_local_board_z_axis_x_bits.store(z_axis_x_bits, std::memory_order_relaxed);
  g_local_board_z_axis_y_bits.store(z_axis_y_bits, std::memory_order_relaxed);
  g_local_board_z_axis_z_bits.store(z_axis_z_bits, std::memory_order_relaxed);
  g_local_spatial_sample_time_us.store(
      sample_time_us, std::memory_order_relaxed);
  g_local_spatial_frame.store(frame, std::memory_order_relaxed);
  g_local_spatial_revision.fetch_add(
      1, std::memory_order_release);

  if (!g_focused.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard lock(g_event_mutex);
  if (g_local_spatial_samples.size() >= kLocalSpatialSampleCapacity ||
      (!g_local_spatial_samples.empty() &&
       frame - g_local_spatial_samples.back().frame <
           kLocalSpatialSampleIntervalFrames)) {
    return;
  }
  g_local_spatial_samples.push_back(
      {frame, position_x_bits, position_y_bits, position_z_bits, x_axis_x_bits,
       x_axis_y_bits, x_axis_z_bits, z_axis_x_bits, z_axis_y_bits,
       z_axis_z_bits});
}

ScoreCollectorTransitionObservationScope::
    ScoreCollectorTransitionObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      module_(ctx.r3.u32), caller_(ctx.lr),
      holder_(
          base && module_
              ? LoadU32(base, module_ + trick::ScoreModuleLayout::kScoreHolder)
              : 0),
      phys_out_(
          base && module_
              ? LoadU32(base, module_ + trick::ScoreModuleLayout::kPhysOut)
              : 0),
      old_state_(
          base && module_
              ? LoadU32(base,
                        module_ + trick::ScoreModuleLayout::kCollectorState)
              : 0),
      old_collector_(
          base && module_
              ? LoadU32(base,
                        module_ + trick::ScoreModuleLayout::kActiveCollector)
              : 0),
      old_vtable_(base && old_collector_ ? LoadU32(base, old_collector_) : 0) {}

ScoreCollectorTransitionObservationScope::
    ~ScoreCollectorTransitionObservationScope() {
  uint8_t* base = base_;
  if (!base_ || !module_) {
    return;
  }
  const uint32_t new_state =
      LoadU32(base_, module_ + trick::ScoreModuleLayout::kCollectorState);
  if (new_state == old_state_) {
    return;
  }
  const bool local =
      module_ && holder_ &&
      phys_out_ == g_local_phys_out.load(std::memory_order_acquire);
  if (local) {
    g_local_score_collector_transition_event_count.fetch_add(
        1, std::memory_order_relaxed);
  }
  const uint32_t new_collector =
      LoadU32(base_, module_ + trick::ScoreModuleLayout::kActiveCollector);
  if (local &&
      new_state == static_cast<uint32_t>(trick::ScoreCollectorState::Air) &&
      new_collector &&
      custom_trick::ShouldPublishActiveScorable(frame_, phys_out_)) {
    // The retail collector swap copies only IDs from its fixed registry. Carry
    // the already-published custom descriptor into the newly entered
    // AirCollector before its first Update call.
    g_pending_custom_air_collector.store(new_collector,
                                         std::memory_order_release);
    REX_STORE_U32(
        new_collector + trick::AirCollectorLayout::kCurrentScorableId,
        custom_trick::ActiveScorableId());
  }
  const uint32_t new_vtable = new_collector ? LoadU32(base_, new_collector) : 0;
  if (local) {
    std::ostringstream local_event;
    local_event << "LST@" << frame_ << ':';
    AppendHex(local_event, module_);
    local_event << ':';
    AppendHex(local_event, phys_out_);
    local_event << ':';
    AppendHex(local_event, holder_);
    local_event << ':' << old_state_ << '>' << new_state << ':';
    AppendHex(local_event, old_collector_);
    local_event << '>';
    AppendHex(local_event, new_collector);
    std::lock_guard lock(g_event_mutex);
    if (g_local_score_collector_events.size() < 64) {
      g_local_score_collector_events.push_back(local_event.str());
    }
  }
  if (!BeginEvent(EventKind::ScoreCollectorTransition)) {
    return;
  }
  std::ostringstream event;
  event << "ST@" << frame_ << ':';
  AppendHex(event, caller_);
  event << ':';
  AppendHex(event, module_);
  event << ':';
  AppendHex(event, holder_);
  event << ':' << old_state_ << '>' << new_state << ':';
  AppendHex(event, old_collector_);
  event << '@';
  AppendHex(event, old_vtable_);
  event << '>';
  AppendHex(event, new_collector);
  event << '@';
  AppendHex(event, new_vtable);
  PushFocusedEvent(event.str());
  if (new_state == static_cast<uint32_t>(trick::ScoreCollectorState::Grind)) {
    ActorSpatialSnapshot snapshot{};
    bool found = false;
    {
      std::lock_guard lock(g_event_mutex);
      const auto it = g_actor_spatial_snapshots.find(phys_out_);
      if (it != g_actor_spatial_snapshots.end()) {
        snapshot = it->second;
        found = true;
      }
    }
    if (found) {
      std::ostringstream grind_position;
      grind_position << "GP@" << frame_ << ':';
      AppendHex(grind_position, phys_out_);
      grind_position << ':';
      AppendHex(grind_position, holder_);
      grind_position << ':' << snapshot.frame << ':';
      AppendHex(grind_position, snapshot.position_x_bits);
      grind_position << ':';
      AppendHex(grind_position, snapshot.position_y_bits);
      grind_position << ':';
      AppendHex(grind_position, snapshot.position_z_bits);
      PushFocusedEvent(grind_position.str());
    }
  }
}

void ObserveAirTrickAnalysis(PPCContext &ctx, uint8_t *base) {
  ObserveSimpleCollector(EventKind::AirAnalysis, "AA", ctx, base);
}

void ObserveAirStartTrick(PPCContext &ctx, uint8_t *base) {
  if (!BeginEvent(EventKind::AirStart)) {
    return;
  }
  const uint32_t collector = ctx.r3.u32;
  const float degree_or_value = static_cast<float>(ctx.f1.f64);
  std::ostringstream event;
  event << "A+@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.lr);
  event << ':';
  AppendHex(event, collector);
  event << ':';
  AppendHex(event, ctx.r4.u32);
  event << ':';
  AppendHex(event, ctx.r5.u32);
  event << ':';
  AppendHex(event, std::bit_cast<uint32_t>(degree_or_value));
  event << ':';
  AppendHex(
      event,
      LoadU32(base, collector + trick::AirCollectorLayout::kCurrentScorableId));
  PushFocusedEvent(event.str());
}

void ObserveAirEndTrick(PPCContext &ctx, uint8_t *base) {
  ObserveSimpleCollector(EventKind::AirEnd, "A-", ctx, base);
}

void ObserveAirUpdateTricks(PPCContext &ctx, uint8_t *base) {
  ObserveSimpleCollector(EventKind::AirUpdate, "AU", ctx, base);
}

void ObserveScoreHolderRecordTrick(PPCContext &ctx, uint8_t *base) {
  custom_trick::ObserveScoreHolderRecord(
      input_history_watch::CurrentFrameSequence(), base, ctx.r3.u32,
      ctx.r4.u32);
  if (!BeginEvent(EventKind::ScoreHolderRecord)) {
    return;
  }
  const uint32_t holder = ctx.r3.u32;
  const uint32_t scorable = ctx.r4.u32;
  std::ostringstream event;
  event << "HR@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.lr);
  event << ':';
  AppendHex(event, holder);
  event << ':';
  AppendHex(event, scorable);
  event << ':';
  AppendHex(event, LoadU32(base, scorable + trick::ScorableLayout::kId));
  event << ':'
        << static_cast<uint32_t>(
               LoadU8(base, scorable + trick::ScorableLayout::kActive));
  event << ':';
  AppendHex(event, LoadU32(base, scorable + trick::ScorableLayout::kValue));
  PushFocusedEvent(event.str());
}

void ObserveScoreHolderCancelTrick(PPCContext &ctx, uint8_t *base) {
  custom_trick::ObserveScoreHolderCancel(
      input_history_watch::CurrentFrameSequence(), base, ctx.r3.u32,
      ctx.r4.u32);
  if (!BeginEvent(EventKind::ScoreHolderCancel)) {
    return;
  }
  const uint32_t holder = ctx.r3.u32;
  const uint32_t scorable = ctx.r4.u32;
  std::ostringstream event;
  event << "HX@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.lr);
  event << ':';
  AppendHex(event, holder);
  event << ':';
  AppendHex(event, scorable);
  event << ':';
  AppendHex(event, LoadU32(base, scorable + trick::ScorableLayout::kId));
  event << ':'
        << static_cast<uint32_t>(
               LoadU8(base, scorable + trick::ScorableLayout::kActive));
  PushFocusedEvent(event.str());
}

AnimationCurrentObservationScope::AnimationCurrentObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      conditioner_(ctx.r3.u32), caller_(ctx.lr) {
  ObserveAnimation(EventKind::AnimationCurrent, "AC", frame_, caller_,
                   conditioner_, base_);
}

AnimationCurrentObservationScope::~AnimationCurrentObservationScope() {
  ObserveAnimation(EventKind::AnimationCurrentResult, "AX", frame_, caller_,
                   conditioner_, base_);
}

AnimationCompletedObservationScope::AnimationCompletedObservationScope(
    PPCContext &ctx, uint8_t *base)
    : base_(base), frame_(input_history_watch::CurrentFrameSequence()),
      conditioner_(ctx.r3.u32), caller_(ctx.lr) {
  ObserveAnimation(EventKind::AnimationCompleted, "AD", frame_, caller_,
                   conditioner_, base_);
}

AnimationCompletedObservationScope::~AnimationCompletedObservationScope() {
  ObserveAnimation(EventKind::AnimationCompletedResult, "AE", frame_, caller_,
                   conditioner_, base_);
}

SceneAnimationLoaderAddObservationScope::
    SceneAnimationLoaderAddObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), manager_(ctx.r3.u32), scene_(ctx.r4.u32), name_(ctx.r5.u32),
      caller_(ctx.lr) {}

SceneAnimationLoaderAddObservationScope::
    ~SceneAnimationLoaderAddObservationScope() {
  g_animation_loader_add_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = LoadToken(base_, name_);
  if (name.empty()) {
    return;
  }
  std::ostringstream value;
  AppendHex(value, manager_);
  value << '@';
  AppendHex(value, scene_);
  value << '@';
  AppendHex(value, caller_);
  std::lock_guard lock(g_animation_asset_mutex);
  const bool is_cac_gesture =
      name.find("cac_") != std::string::npos ||
      name.find("CAC_") != std::string::npos ||
      name.find("airguitar") != std::string::npos ||
      name.find("AIRGUITAR") != std::string::npos;
  if (g_animation_loader_registrations.size() < 64 || is_cac_gesture ||
      g_animation_loader_registrations.contains(name)) {
    g_animation_loader_registrations[name] = value.str();
  }
}

AnimationLoaderDataLookupObservationScope::
    AnimationLoaderDataLookupObservationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base), manager_(ctx.r3.u32), name_(ctx.r4.u32),
      caller_(ctx.lr) {}

AnimationLoaderDataLookupObservationScope::
    ~AnimationLoaderDataLookupObservationScope() {
  g_animation_loader_lookup_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = LoadToken(base_, name_);
  if (name.empty()) {
    return;
  }
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_animation_loader_lookup_results.size() < 128 ||
      g_animation_loader_lookup_results.contains(name)) {
    g_animation_loader_lookup_results[name] = ctx_.r3.u32;
  }
}

SceneAnimationLoaderLoadObservationScope::
    SceneAnimationLoaderLoadObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), loader_(ctx.r3.u32), caller_(ctx.lr) {}

SceneAnimationLoaderLoadObservationScope::
    ~SceneAnimationLoaderLoadObservationScope() {
  g_animation_loader_load_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = LoadToken(base_, LoadU32(base_, loader_ + 4));
  if (name.empty()) {
    return;
  }
  std::ostringstream value;
  AppendHex(value, loader_);
  value << '@';
  AppendHex(value, caller_);
  value << '@';
  AppendHex(value, LoadU32(base_, loader_ + 20));
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_animation_loader_loads.size() < 128 ||
      g_animation_loader_loads.contains(name)) {
    g_animation_loader_loads[name] = value.str();
  }
}

SceneAnimationLoaderPollObservationScope::
    SceneAnimationLoaderPollObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), loader_(ctx.r3.u32), caller_(ctx.lr),
      was_complete_(base && loader_ && LoadU8(base, loader_ + 32) != 0) {}

SceneAnimationLoaderPollObservationScope::
    ~SceneAnimationLoaderPollObservationScope() {
  if (was_complete_ || !base_ || !loader_ || LoadU8(base_, loader_ + 32) == 0) {
    return;
  }
  g_animation_loader_completion_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = LoadToken(base_, LoadU32(base_, loader_ + 4));
  if (name.empty()) {
    return;
  }
  std::ostringstream value;
  AppendHex(value, LoadU32(base_, loader_ + 24));
  value << '@' << LoadU32(base_, loader_ + 28) << '@';
  AppendHex(value, loader_);
  value << '@';
  AppendHex(value, caller_);
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_animation_loader_completions.size() < 128 ||
      g_animation_loader_completions.contains(name)) {
    g_animation_loader_completions[name] = value.str();
  }
}

SceneAnimationAsyncLoadObservationScope::
    SceneAnimationAsyncLoadObservationScope(PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base), path_(ctx.r4.u32), caller_(ctx.lr) {}

SceneAnimationAsyncLoadObservationScope::
    ~SceneAnimationAsyncLoadObservationScope() {
  constexpr uint32_t kSceneCharacterAnimLoadCaller = 0x82824B00;
  if (caller_ != kSceneCharacterAnimLoadCaller) {
    return;
  }
  const std::string path = LoadToken(base_, path_);
  if (path.empty()) {
    return;
  }
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_animation_loader_async_paths.size() < 128 ||
      g_animation_loader_async_paths.contains(path)) {
    g_animation_loader_async_paths[path] = ctx_.r3.u32;
  }
}

PlaybackDataConstructionObservationScope::
    PlaybackDataConstructionObservationScope(PPCContext &ctx, uint8_t *base)
    : base_(base), playback_data_(ctx.r3.u32), manager_(ctx.r4.u32),
      name_(ctx.r5.u32), kind_(ctx.r6.u32), caller_(ctx.lr) {}

PlaybackDataConstructionObservationScope::
    ~PlaybackDataConstructionObservationScope() {
  g_playback_data_construction_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = DecodeFastString(base_, name_);
  if (name.empty()) {
    return;
  }
  std::ostringstream value;
  AppendHex(value, playback_data_);
  value << '@';
  AppendHex(value, manager_);
  value << '@' << kind_ << '@';
  AppendHex(value, caller_);
  std::lock_guard lock(g_animation_asset_mutex);
  g_latest_playback_data_instances.try_emplace(name, playback_data_);
  if (g_playback_data_constructions.size() < 128 ||
      g_playback_data_constructions.contains(name)) {
    g_playback_data_constructions[name] = value.str();
  }
}

PlaybackDataLookupObservationScope::PlaybackDataLookupObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base), name_(ctx.r4.u32), caller_(ctx.lr) {}

PlaybackDataLookupObservationScope::~PlaybackDataLookupObservationScope() {
  g_playback_data_lookup_count.fetch_add(1, std::memory_order_relaxed);
  const std::string name = DecodeFastString(base_, name_);
  if (name.empty()) {
    return;
  }
  std::ostringstream value;
  AppendHex(value, ctx_.r3.u32);
  value << '@';
  AppendHex(value, caller_);
  std::lock_guard lock(g_animation_asset_mutex);
  if (ctx_.r3.u32) {
    g_latest_playback_data_instances[name] = ctx_.r3.u32;
  }
  if (g_playback_data_lookup_results.size() < 128 ||
      g_playback_data_lookup_results.contains(name)) {
    g_playback_data_lookup_results[name] = value.str();
  }
}

AndaleDatabaseLoadObservationScope::AndaleDatabaseLoadObservationScope(
    PPCContext &ctx, uint8_t *base)
    : ctx_(ctx), base_(base), manager_(ctx.r3.u32), slot_(ctx.r4.u32),
      allocator_(ctx.r5.u32), path_(ctx.r6.u32), caller_(ctx.lr) {}

AndaleDatabaseLoadObservationScope::~AndaleDatabaseLoadObservationScope() {
  g_andale_database_load_count.fetch_add(1, std::memory_order_relaxed);
  const std::string path = LoadToken(base_, path_);
  if (path.empty()) {
    return;
  }
  if (path == "data_anim_OnBoard.abin") {
    g_andale_database_manager.store(manager_, std::memory_order_release);
    g_andale_database_allocator.store(allocator_, std::memory_order_release);
    g_andale_database_slot.store(slot_, std::memory_order_release);
    if (ctx_.r3.u32) {
      g_andale_onboard_database.store(ctx_.r3.u32,
                                      std::memory_order_release);
    }
  }
  std::ostringstream value;
  AppendHex(value, ctx_.r3.u32);
  value << '@';
  AppendHex(value, manager_);
  value << '@';
  AppendHex(value, allocator_);
  value << '@' << slot_ << '@';
  AppendHex(value, caller_);
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_andale_database_loads.size() < 128 ||
      g_andale_database_loads.contains(path)) {
    g_andale_database_loads[path] = value.str();
  }
}

AndaleDatabaseContentObservationScope::AndaleDatabaseContentObservationScope(
    PPCContext &ctx, uint8_t *)
    : ctx_(ctx), manager_(ctx.r3.u32), slot_(ctx.r4.u32), content_(ctx.r5.u32),
      policy_(ctx.r6.u32), flags_(ctx.r7.u32), caller_(ctx.lr) {}

AndaleDatabaseContentObservationScope::
    ~AndaleDatabaseContentObservationScope() {
  g_andale_database_content_count.fetch_add(1, std::memory_order_relaxed);
  std::ostringstream key;
  key << slot_;
  key << '@';
  AppendHex(key, caller_);
  std::ostringstream value;
  AppendHex(value, ctx_.r3.u32);
  value << '@';
  AppendHex(value, manager_);
  value << '@';
  AppendHex(value, content_);
  value << '@';
  AppendHex(value, policy_);
  value << '@' << flags_;
  std::lock_guard lock(g_animation_asset_mutex);
  if (g_andale_database_contents.size() < 128 ||
      g_andale_database_contents.contains(key.str())) {
    g_andale_database_contents[key.str()] = value.str();
  }
}

struct CustomAnimationAssetLoadResult {
  bool configured{};
  bool success{};
  bool load_only{};
};

void BindConfiguredAnimationLeafReplacements(
    PPCContext &source_context, uint8_t *base, std::string_view asset,
    uint32_t custom_database) {
  const uint32_t retail_database =
      g_andale_onboard_database.load(std::memory_order_acquire);
  if (!base || !custom_database || !retail_database ||
      source_context.r1.u32 < 0x20000) {
    return;
  }

  PPCContext lookup_context = source_context;
  lookup_context.r1.u32 = (source_context.r1.u32 - 0x200) & ~0xFu;
  REX_STORE_U32(lookup_context.r1.u32, source_context.r1.u32);
  const uint32_t source_name_address = lookup_context.r1.u32 + 0x20;
  const uint32_t target_name_address = lookup_context.r1.u32 + 0x60;

  const auto lookup = [&](uint32_t database, uint32_t name_address) {
    PPCContext context = lookup_context;
    context.r3.u64 = database;
    context.r4.u64 = name_address;
    sub_82D1C200(context, base);
    return context.r3.u32;
  };

  const uint32_t count =
      trick_overrides::LoadedAnimationLeafReplacementRuleCount();
  for (uint32_t index = 0; index < count; ++index) {
    const auto rule =
        trick_overrides::FindAnimationLeafReplacementByIndex(index);
    if (!rule || rule->asset != asset) {
      continue;
    }

    {
      std::lock_guard lock(g_animation_asset_mutex);
      const bool already_bound = std::any_of(
          g_bound_animation_leaf_replacements.begin(),
          g_bound_animation_leaf_replacements.end(),
          [&](const BoundAnimationLeafReplacement &bound) {
            return bound.name == rule->name;
          });
      if (already_bound) {
        continue;
      }
    }

    if (!StoreFastString36(base, source_name_address,
                           rule->source_animation) ||
        !StoreFastString36(base, target_name_address,
                           rule->target_animation)) {
      continue;
    }
    const uint32_t source_clip =
        lookup(retail_database, source_name_address);
    const uint32_t target_clip =
        lookup(custom_database, target_name_address);
    const uint32_t source_aligned =
        source_clip ? (source_clip + 55) & ~UINT32_C(15) : 0;
    const uint32_t target_aligned =
        target_clip ? (target_clip + 55) & ~UINT32_C(15) : 0;
    const uint32_t source_allocation =
        source_aligned ? LoadU32(base, source_aligned + 44) : 0;
    const uint32_t target_allocation =
        target_aligned ? LoadU32(base, target_aligned + 44) : 0;
    const uint32_t source_codec =
        source_clip ? LoadU32(base, source_clip + 8) : 0;
    const uint32_t target_codec =
        target_clip ? LoadU32(base, target_clip + 8) : 0;
    constexpr uint32_t kVbrAnimationCodec = UINT32_C(0x00524256);
    // A rebuilt VBR clip may legitimately own a larger allocation than the
    // retail leaf. Codec changes are not interchangeable here: the failed
    // 2026-08-03 RAW-over-VBR experiment hard-reset the skater even though
    // the stream object remained alive. Keep leaf replacement native-VBR.
    const bool compatible_allocation =
        source_allocation && target_allocation &&
        source_codec == kVbrAnimationCodec &&
        target_codec == source_codec;

    std::ostringstream value;
    value << (source_clip && target_clip && compatible_allocation
                  ? "bound@"
                  : "failed@");
    AppendHex(value, source_clip);
    value << '@';
    AppendHex(value, target_clip);
    value << '@' << source_allocation << '@' << target_allocation << '@';
    AppendHex(value, source_codec);
    value << '@';
    AppendHex(value, target_codec);
    value << '@'
          << rule->source_animation << '@' << rule->target_animation;

    std::lock_guard lock(g_animation_asset_mutex);
    g_animation_leaf_replacement_results[std::string(rule->name)] =
        value.str();
    if (!source_clip || !target_clip || !compatible_allocation) {
      continue;
    }
    g_bound_animation_leaf_replacements.push_back({
        .name = std::string(rule->name),
        .asset = std::string(rule->asset),
        .source_animation = std::string(rule->source_animation),
        .target_animation = std::string(rule->target_animation),
        .source_clip = source_clip,
        .target_clip = target_clip,
        .allocation_size = source_allocation,
    });
    g_custom_animation_asset_clips[
        std::string(rule->asset) + ':' +
        std::string(rule->target_animation)] = target_clip;
    g_animation_leaf_replacement_bind_count.fetch_add(
        1, std::memory_order_relaxed);
    REXLOG_WARN(
        "Skate 3 animation leaf replacement bound: {} {}@0x{:08X} "
        "-> {}@0x{:08X} allocation={}",
        rule->name, rule->source_animation, source_clip,
        rule->target_animation, target_clip, source_allocation);
  }
}

CustomAnimationAssetLoadResult
EnsureCustomAnimationAssetLoaded(PPCContext &source_context, uint8_t *base,
                                 std::string_view animation,
                                 uint32_t preferred_playback_data = 0) {
  const auto rule = trick_overrides::FindAnimationAsset(animation);
  if (!rule) {
    return {};
  }

  CustomAnimationAssetLoadResult result{
      .configured = true,
      .success = false,
      .load_only = rule->load_only,
  };
  std::lock_guard load_lock(g_custom_animation_asset_load_mutex);
  const auto loaded =
      g_custom_animation_asset_loaded.find(std::string(rule->name));
  if (loaded != g_custom_animation_asset_loaded.end() && loaded->second) {
    uint32_t database = 0;
    {
      std::lock_guard asset_lock(g_animation_asset_mutex);
      const auto found = g_custom_animation_asset_databases.find(
          std::string(rule->name));
      if (found != g_custom_animation_asset_databases.end()) {
        database = found->second;
      }
    }
    BindConfiguredAnimationLeafReplacements(
        source_context, base, rule->name, database);
    result.success = true;
    return result;
  }

  g_custom_animation_asset_load_attempt_count.fetch_add(
      1, std::memory_order_relaxed);
  const uint32_t manager =
      g_andale_database_manager.load(std::memory_order_acquire);
  const uint32_t allocator =
      g_andale_database_allocator.load(std::memory_order_acquire);
  const uint32_t retail_slot =
      g_andale_database_slot.load(std::memory_order_acquire);
  std::vector<uint32_t> playback_data;
  if (preferred_playback_data) {
    playback_data.push_back(preferred_playback_data);
  }
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    const auto found =
        g_latest_playback_data_instances.find(std::string(rule->playback_data));
    if (playback_data.empty() &&
        found != g_latest_playback_data_instances.end() && found->second) {
      playback_data.push_back(found->second);
    }
  }

  auto record_stage = [&](std::string_view stage) {
    {
      std::lock_guard asset_lock(g_animation_asset_mutex);
      g_custom_animation_asset_results[std::string(rule->name)] =
          std::string("stage@") + std::string(stage) + '@' +
          std::string(rule->path);
    }
    REXLOG_WARN("Skate 3 animation asset '{}' stage={} path={} playback={}",
                rule->name, stage, rule->path, rule->playback_data);
  };
  auto record_failure = [&](std::string_view reason) {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    g_custom_animation_asset_results[std::string(rule->name)] =
        std::string("failed@") + std::string(reason) + '@' +
        std::string(rule->path);
  };
  if (!base || !manager || !allocator || !retail_slot ||
      retail_slot != rule->memory_group || playback_data.empty() ||
      source_context.r1.u32 < 0x20000) {
    record_failure("runtime-not-ready");
    return result;
  }

  PPCContext load_context = source_context;
  // Reserve a conventional, small PPC caller frame. The previous prototype
  // jumped 64 KiB below r1 and crossed the recomp stack's valid mapping.
  load_context.r1.u32 = (source_context.r1.u32 - 0x200) & ~0xFu;
  REX_STORE_U32(load_context.r1.u32, source_context.r1.u32);
  const uint32_t animation_name_address = load_context.r1.u32 + 0x20;
  const uint32_t path_address = load_context.r1.u32 + 0x40;
  const uint32_t database_slot_address = load_context.r1.u32 + 0x100;
  const uint32_t air_animation_name_address = load_context.r1.u32 + 0x130;
  if (!StoreFastString36(base, animation_name_address, rule->animation) ||
      !StoreAssetPath(base, path_address, rule->path)) {
    record_failure("guest-encoding");
    return result;
  }

  load_context.r3.u64 = manager;
  load_context.r4.u64 = retail_slot;
  // 0x82D19BD0 forwards r5 to the game's 228-byte database allocation.
  // It is the retail allocator-policy pointer, not a database-name string.
  load_context.r5.u64 = allocator;
  load_context.r6.u64 = path_address;
  g_active_custom_animation_asset_rule = rule->name;
  g_active_custom_animation_asset_path = rule->path;
  record_stage("pre-load");
  sub_82D19BD0(load_context, base);
  g_active_custom_animation_asset_rule.clear();
  g_active_custom_animation_asset_path.clear();
  const uint32_t database = load_context.r3.u32;
  record_stage(database ? "post-load" : "post-load-null");
  if (!database) {
    record_failure("database-load");
    return result;
  }

  PPCContext lookup_context = source_context;
  lookup_context.r1.u32 = load_context.r1.u32;
  lookup_context.r3.u64 = database;
  lookup_context.r4.u64 = animation_name_address;
  record_stage("pre-clip");
  sub_82D1C200(lookup_context, base);
  const uint32_t clip = lookup_context.r3.u32;
  record_stage(clip ? "post-clip" : "post-clip-null");
  if (!clip) {
    record_failure("clip-not-found");
    return result;
  }
  uint32_t air_clip = 0;
  const auto &active_definition = custom_trick::ActiveDefinition();
  if (!active_definition.skater_animation_air.empty() &&
      active_definition.skater_animation == rule->animation &&
      StoreFastString36(base, air_animation_name_address,
                        active_definition.skater_animation_air)) {
    PPCContext air_lookup_context = source_context;
    air_lookup_context.r1.u32 = load_context.r1.u32;
    air_lookup_context.r3.u64 = database;
    air_lookup_context.r4.u64 = air_animation_name_address;
    sub_82D1C200(air_lookup_context, base);
    air_clip = air_lookup_context.r3.u32;
  }
  REX_STORE_U32(database_slot_address, database);
  uint32_t attached = 0;
  for (const uint32_t playback : playback_data) {
    if (!playback) {
      continue;
    }
    PPCContext insert_context = source_context;
    insert_context.r1.u32 = load_context.r1.u32;
    insert_context.r3.u64 = playback + 28;
    insert_context.r4.u64 = database_slot_address;
    record_stage("pre-attach");
    sub_82549888(insert_context, base);
    record_stage("post-attach");
    ++attached;
  }
  if (!attached) {
    record_failure("playback-attach");
    return result;
  }

  std::ostringstream value;
  value << "loaded@";
  AppendHex(value, database);
  value << '@';
  AppendHex(value, clip);
  value << '@' << attached << '@' << rule->database_name << '@'
        << rule->playback_data << '@' << rule->path;
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    g_custom_animation_asset_results[std::string(rule->name)] = value.str();
    g_custom_animation_asset_databases[std::string(rule->name)] = database;
    g_custom_animation_asset_clips[std::string(rule->name)] = clip;
    if (air_clip) {
      g_custom_animation_asset_clips[std::string(rule->name) + ":air"] =
          air_clip;
    }
  }
  g_custom_animation_asset_loaded[std::string(rule->name)] = true;
  g_custom_animation_asset_load_success_count.fetch_add(
      1, std::memory_order_relaxed);
  BindConfiguredAnimationLeafReplacements(
      source_context, base, rule->name, database);
  result.success = true;
  return result;
}

void ObserveActiveCustomAnimationAssetLoadStage(const char *stage) {
  if (!stage || g_active_custom_animation_asset_rule.empty()) {
    return;
  }
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    g_custom_animation_asset_results[g_active_custom_animation_asset_rule] =
        std::string("stage@") + stage + '@' +
        g_active_custom_animation_asset_path;
  }
  REXLOG_WARN("Skate 3 animation asset '{}' internal-stage={} path={}",
              g_active_custom_animation_asset_rule, stage,
              g_active_custom_animation_asset_path);
}

void ObserveXenonFileDeviceRead(PPCContext &ctx) {
  static std::atomic<uint32_t> large_read_count{0};
  const bool custom = !g_active_custom_animation_asset_rule.empty();
  const bool large = ctx.r6.u32 >= 1024 * 1024;
  if (!custom && (!large || large_read_count.fetch_add(
                                1, std::memory_order_relaxed) >= 32)) {
    return;
  }
  REXLOG_WARN("Skate 3 XenonFileDeviceDriver::Read custom={} rule={} "
              "r3=0x{:08X} r4=0x{:08X} buffer=0x{:08X} "
              "length={} end=0x{:08X} lr=0x{:08X}",
              custom ? 1 : 0,
              custom ? g_active_custom_animation_asset_rule : "-", ctx.r3.u32,
              ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r5.u32 + ctx.r6.u32,
              ctx.lr);
}

void ObserveAnimationEvalCommandBuffer(PPCContext &ctx, uint8_t *base) {
  const uint32_t object = ctx.r3.u32;
  if (!base || !object) {
    return;
  }

  const uint32_t count = LoadU32(base, object + 76);
  const std::array<uint32_t, 3> output_words{
      LoadU32(base, object + 80),
      LoadU32(base, object + 84),
      LoadU32(base, object + 88),
  };
  const std::array<uint32_t, 3> cursors{
      LoadU32(base, object + 92),
      LoadU32(base, object + 96),
      LoadU32(base, object + 100),
  };
  const std::array<uint32_t, 3> command_counts{
      LoadU32(base, object + 120),
      LoadU32(base, object + 124),
      LoadU32(base, object + 128),
  };
  const bool null_output =
      count > 0 && std::any_of(output_words.begin(), output_words.end(),
                               [](uint32_t value) { return value == 0; });
  if (!null_output) {
    return;
  }

  const std::array<uint32_t, 3> next_write_addresses{
      output_words[0] + 4 * (cursors[0] + 1),
      output_words[1] + 4 * (cursors[1] + 1),
      output_words[2] + 4 * (cursors[2] + 1),
  };
  if (null_output) {
    REXLOG_WARN("Skate 3 Andale eval command buffer has null output: "
                "object=0x{:08X} lr=0x{:08X} count={} "
                "outputs={:08X},{:08X},{:08X} "
                "cursors={:08X},{:08X},{:08X} "
                "commands={},{},{}",
                object, ctx.lr, count, output_words[0], output_words[1],
                output_words[2], cursors[0], cursors[1], cursors[2],
                command_counts[0], command_counts[1], command_counts[2]);
  }
  if (!BeginEvent(EventKind::AnimationEvalCommandBuffer)) {
    return;
  }
  std::ostringstream event;
  event << "EV@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, object);
  event << ':';
  AppendHex(event, ctx.lr);
  event << ':' << count;
  for (uint32_t value : output_words) {
    event << ':';
    AppendHex(event, value);
  }
  for (uint32_t value : cursors) {
    event << ':';
    AppendHex(event, value);
  }
  for (uint32_t value : next_write_addresses) {
    event << ':';
    AppendHex(event, value);
  }
  for (uint32_t value : command_counts) {
    event << ':' << value;
  }
  PushFocusedEvent(event.str());
}

void ObserveAnimationStreamTableEval(PPCContext &ctx, uint8_t *base) {
  if (!base || !ctx.r3.u32) {
    return;
  }

  const uint32_t object = ctx.r3.u32;
  const uint32_t data = LoadU32(base, object + 20);
  if (!data) {
    return;
  }
  std::string rule_name;
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    for (const auto &[name, clip] : g_custom_animation_asset_clips) {
      if (clip == data) {
        rule_name = name;
        break;
      }
    }
  }
  if (rule_name.empty()) {
    return;
  }

  const uint32_t aligned = (data + 55) & ~UINT32_C(15);
  const uint32_t allocation_end = aligned ? LoadU32(base, aligned + 44) : 0;
  const uint32_t compressed_end = aligned ? LoadU32(base, aligned + 48) : 0;
  const uint32_t descriptor = aligned ? LoadU32(base, aligned + 52) : 0;
  const uint64_t eval_count =
      g_custom_animation_asset_stream_eval_count.fetch_add(
          1, std::memory_order_relaxed) +
      1;
  bool first_for_rule = false;
  bool leaf_replacement = false;
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    first_for_rule = g_custom_animation_asset_eval_results.find(rule_name) ==
                     g_custom_animation_asset_eval_results.end();
    leaf_replacement = std::any_of(
        g_bound_animation_leaf_replacements.begin(),
        g_bound_animation_leaf_replacements.end(),
        [&](const BoundAnimationLeafReplacement &replacement) {
          return replacement.target_clip == data;
        });
    if (first_for_rule || eval_count % 120 == 0) {
      std::ostringstream value;
      value << eval_count << '@';
      AppendHex(value, data);
      value << '@';
      AppendHex(value, compressed_end);
      value << '@';
      AppendHex(value, allocation_end);
      value << '@';
      AppendHex(value, descriptor);
      g_custom_animation_asset_eval_results[rule_name] = value.str();
    }
  }
  if (!leaf_replacement) {
    custom_trick::ObserveCustomAnimationEvaluated(
        rule_name.ends_with(":air")
            ? std::string_view(rule_name).substr(0, rule_name.size() - 4)
            : std::string_view(rule_name),
        input_history_watch::CurrentFrameSequence(), object, data);
  }
  if (first_for_rule) {
    REXLOG_WARN("Skate 3 custom animation stream evaluated: "
                "rule={} object=0x{:08X} clip=0x{:08X} "
                "compressed_end=0x{:08X} allocation_end=0x{:08X} "
                "descriptor=0x{:08X}",
                rule_name, object, data, compressed_end, allocation_end,
                descriptor);
  }
}

CustomAnimationStreamEvalScope::CustomAnimationStreamEvalScope(
    PPCContext &ctx, uint8_t *base)
    : previous_rule_(g_active_custom_animation_eval_rule),
      previous_gesture_stream_(g_active_gesture_stream),
      previous_gesture_clip_(g_active_gesture_clip) {
  // Replace the stream leaf before retail Eval constructs/advances its VBR
  // decoder. Swapping raw VBR source pointers later is incorrect because the
  // decoder carries stream-local state across frames. The stream object is
  // already the selected low/high leaf of the retail blend tree, so changing
  // only its clip pointer preserves TrickHeight selection and all parents.
  if (base && ctx.r3.u32) {
    const uint32_t object = ctx.r3.u32;
    const uint32_t data = LoadU32(base, object + 20);
    std::string replacement_name;
    std::string replacement_rule;
    uint32_t replacement_clip = 0;
    {
      std::lock_guard asset_lock(g_animation_asset_mutex);
      for (const auto &replacement :
           g_bound_animation_leaf_replacements) {
        if (data != replacement.source_clip) {
          continue;
        }
        replacement_name = replacement.name;
        replacement_rule =
            replacement.asset + ':' + replacement.target_animation;
        replacement_clip = replacement.target_clip;
        break;
      }
    }
    if (replacement_clip) {
      REX_STORE_U32(object + 20, replacement_clip);
      const uint64_t count =
          g_animation_leaf_replacement_eval_count.fetch_add(
              1, std::memory_order_relaxed) +
          1;
      std::ostringstream value;
      value << count << '@'
            << input_history_watch::CurrentFrameSequence() << '@'
            << replacement_rule << '@';
      AppendHex(value, object);
      {
        std::lock_guard asset_lock(g_animation_asset_mutex);
        g_animation_leaf_replacement_results[replacement_name] =
            value.str();
      }
      if (g_focused.load(std::memory_order_acquire)) {
        REXLOG_WARN(
            "Skate 3 animation stream leaf replaced: {} "
            "object=0x{:08X} source=0x{:08X} target=0x{:08X}",
            replacement_name, object, data, replacement_clip);
      }
    }
  }
  ObserveAnimationStreamTableEval(ctx, base);
  if (!base || !ctx.r3.u32) {
    return;
  }
  const uint32_t data = LoadU32(base, ctx.r3.u32 + 20);
  if (cac_gesture::IsActive()) {
    std::lock_guard gesture_lock(g_gesture_stream_mutex);
    const auto stream = g_gesture_streams.find(data);
    if (stream != g_gesture_streams.end()) {
      g_active_gesture_stream = stream->second;
      g_active_gesture_clip = data;
    } else {
      static std::atomic<uint32_t> unmatched_count{0};
      const uint32_t index =
          unmatched_count.fetch_add(1, std::memory_order_relaxed);
      if (index < 40) {
        REXLOG_WARN(
            "cac-gesture: unmatched stream eval index={} frame={} "
            "object=0x{:08X} data=0x{:08X}",
            index, input_history_watch::CurrentFrameSequence(), ctx.r3.u32,
            data);
      }
    }
  }
  std::lock_guard asset_lock(g_animation_asset_mutex);
  for (const auto &[name, clip] : g_custom_animation_asset_clips) {
    if (clip == data) {
      g_active_custom_animation_eval_rule = name;
      return;
    }
  }
}

CustomAnimationStreamEvalScope::~CustomAnimationStreamEvalScope() {
  g_active_custom_animation_eval_rule = previous_rule_;
  g_active_gesture_stream = previous_gesture_stream_;
  g_active_gesture_clip = previous_gesture_clip_;
}

VbrExtractObservationScope::VbrExtractObservationScope(PPCContext &ctx,
                                                       uint8_t *base)
    : base_(base),
      extracts_(ctx.r3.u32),
      count_(ctx.r4.u32),
      decoder_(ctx.r5.u32),
      frame_(input_history_watch::CurrentFrameSequence()),
      rule_(g_active_custom_animation_eval_rule),
      gesture_stream_(g_active_gesture_stream),
      gesture_clip_(g_active_gesture_clip) {
  if (rule_.empty() &&
      g_custom_animation_asset_stream_eval_count.load(
          std::memory_order_acquire) != 0) {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    if (!g_custom_animation_asset_eval_results.empty()) {
      rule_ = g_custom_animation_asset_eval_results.begin()->first;
    }
  }
  if ((rule_.empty() && gesture_stream_.empty()) || !base_ || !extracts_ ||
      !count_ || count_ > 64) {
    return;
  }
  bool contains_custom_source = false;
  if (!gesture_stream_.empty() && gesture_clip_) {
    const uint32_t aligned = (gesture_clip_ + 55) & ~UINT32_C(15);
    const uint32_t allocation_size = LoadU32(base_, aligned + 44);
    for (uint32_t index = 0; index < count_; ++index) {
      const uint32_t source = LoadU32(base_, extracts_ + index * 64 + 28);
      if (source >= gesture_clip_ &&
          source < gesture_clip_ + allocation_size) {
        contains_custom_source = true;
        break;
      }
    }
  }
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    for (const auto &[name, clip] : g_custom_animation_asset_clips) {
      const uint32_t aligned = (clip + 55) & ~UINT32_C(15);
      const uint32_t allocation_size = LoadU32(base_, aligned + 44);
      for (uint32_t index = 0; index < count_; ++index) {
        const uint32_t source = LoadU32(base_, extracts_ + index * 64 + 28);
        if (source >= clip && source < clip + allocation_size) {
          contains_custom_source = true;
          rule_ = name;
          break;
        }
      }
      if (contains_custom_source) {
        break;
      }
    }
  }
  if (!contains_custom_source) {
    return;
  }
  if (gesture_stream_.empty() &&
      !g_focused.load(std::memory_order_acquire)) {
    return;
  }
  observation_index_ =
      g_vbr_extract_observation_count.fetch_add(1, std::memory_order_relaxed) +
      1;
  if (observation_index_ > 80) {
    observation_index_ = 0;
  }
}

VbrExtractObservationScope::~VbrExtractObservationScope() {
  if (!observation_index_) {
    return;
  }
  uint32_t custom_output = 0;
  uint16_t frame_from = 0;
  uint16_t frame_to = 0;
  uint16_t fraction = 0;
  const bool air = rule_.ends_with(":air");
  // Custom trick assets normally remain native VBR. The gesture bridge is
  // different: retail gesture leaves have sparse channel topology, so their
  // decoded pose is replaced after decoding while the stock controller keeps
  // ownership of timing, blending, and cancellation.
  constexpr bool kEnableDecodedPoseBridge = false;
  bool pose_override_applied = false;
  {
    if (!gesture_stream_.empty() && gesture_clip_) {
      const uint32_t aligned = (gesture_clip_ + 55) & ~UINT32_C(15);
      const uint32_t allocation_size = LoadU32(base_, aligned + 44);
      for (uint32_t index = 0; index < count_; ++index) {
        const uint32_t extract = extracts_ + index * 64;
        const uint32_t source = LoadU32(base_, extract + 28);
        if (source >= gesture_clip_ &&
            source < gesture_clip_ + allocation_size) {
          custom_output = LoadU32(base_, extract + 32);
          frame_from = Skate3WatchedLoadU16(base_, extract + 42, __func__);
          frame_to = Skate3WatchedLoadU16(base_, extract + 44, __func__);
          fraction = Skate3WatchedLoadU16(base_, extract + 46, __func__);
          break;
        }
      }
    }
    std::lock_guard asset_lock(g_animation_asset_mutex);
    const auto clip_it = g_custom_animation_asset_clips.find(rule_);
    if (clip_it != g_custom_animation_asset_clips.end()) {
      const uint32_t clip = clip_it->second;
      const uint32_t aligned = (clip + 55) & ~UINT32_C(15);
      const uint32_t allocation_size = LoadU32(base_, aligned + 44);
      for (uint32_t index = 0; index < count_; ++index) {
        const uint32_t extract = extracts_ + index * 64;
        const uint32_t source = LoadU32(base_, extract + 28);
        if (source >= clip && source < clip + allocation_size) {
          custom_output = LoadU32(base_, extract + 32);
          frame_from =
              Skate3WatchedLoadU16(base_, extract + 42, __func__);
          frame_to = Skate3WatchedLoadU16(base_, extract + 44, __func__);
          fraction = Skate3WatchedLoadU16(base_, extract + 46, __func__);
          break;
        }
      }
    }
  }
  if (custom_output) {
    constexpr uint32_t kSqtStride = 48;
    constexpr uint32_t kQuaternionOffset = 16;
    const auto apply_pose = [&](const auto &poses) {
      const size_t from =
          std::min<size_t>(frame_from, poses.size() - 1);
      const size_t to = std::min<size_t>(frame_to, poses.size() - 1);
      const float alpha = static_cast<float>(fraction) / 65535.0f;
      for (size_t index = 0; index < poses[from].size(); ++index) {
        const auto &from_edit = poses[from][index];
        const auto &to_edit = poses[to][index];
        const auto quaternion = InterpolateQuaternion(
            from_edit.quaternion, to_edit.quaternion, alpha);
        StoreQuaternion(
            base_, custom_output + from_edit.bone * kSqtStride +
                       kQuaternionOffset,
            quaternion);
      }
    };
    if (air && kEnableDecodedPoseBridge) {
      apply_pose(generated_pose_override::kAirPoseOverrides);
      pose_override_applied = true;
    }
    if (!gesture_stream_.empty() &&
        REXCVAR_GET(skate3_cac_gesture_fullbody_bridge)) {
      using generated_gesture_pose::kCyclePoseOverrides;
      const bool into =
          gesture_stream_ == "B_GSTR_AIRGUITAR_BOTH_INTO";
      const bool cycle =
          gesture_stream_ == "B_GSTR_AIRGUITAR_BOTH_CYC";
      const bool out =
          gesture_stream_ == "B_GSTR_AIRGUITAR_BOTH_OUT";
      if (into || cycle || out) {
        const auto apply_gesture_pose = [&](size_t from, size_t to,
                                            float alpha) {
          const auto &from_pose =
              kCyclePoseOverrides[std::min(from,
                                           kCyclePoseOverrides.size() - 1)];
          const auto &to_pose =
              kCyclePoseOverrides[std::min(to,
                                           kCyclePoseOverrides.size() - 1)];
          for (size_t index = 0; index < from_pose.size(); ++index) {
            const auto quaternion = InterpolateQuaternion(
                from_pose[index].quaternion, to_pose[index].quaternion,
                alpha);
            StoreQuaternion(
                base_,
                custom_output + from_pose[index].bone * kSqtStride +
                    kQuaternionOffset,
                quaternion);
          }
        };
        if (cycle) {
          apply_gesture_pose(frame_from, frame_to,
                             static_cast<float>(fraction) / 65535.0f);
        } else {
          const size_t held_frame =
              into ? 0 : kCyclePoseOverrides.size() - 1;
          apply_gesture_pose(held_frame, held_frame, 0.0f);
        }
        pose_override_applied = true;
      }
    }
  }
  std::ostringstream details;
  details << (pose_override_applied ? "pose-bridge|" : "native-vbr|");
  for (uint32_t index = 0; index < count_; ++index) {
    const uint32_t extract = extracts_ + index * 64;
    if (index) {
      details << ',';
    }
    details << index << '@';
    AppendHex(details, LoadU32(base_, extract + 28));
    details << '@';
    AppendHex(details, LoadU32(base_, extract + 32));
    details << '@' << static_cast<uint32_t>(LoadU8(base_, extract + 40))
            << ':' << static_cast<uint32_t>(LoadU8(base_, extract + 41))
            << ':' << Skate3WatchedLoadU16(base_, extract + 42, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 44, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 46, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 52, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 54, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 60, __func__) << ':'
            << Skate3WatchedLoadU16(base_, extract + 62, __func__);
  }
  if (custom_output) {
    // Runtime-decoded local SQT evidence for both feet/toes and the board.
    // Each SQT is 48 bytes: scale@0, quaternion@16, translation@32.
    details << "|pose=";
    constexpr std::array<uint32_t, 17> kGestureBones{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    constexpr std::array<uint32_t, 9> kContactBones{
        19, 20, 23, 24, 25, 32, 33, 34, 35};
    const auto append_bones = [&](const auto &bones) {
      for (const uint32_t bone : bones) {
      details << bone << '@';
      const uint32_t sqt = custom_output + bone * 48;
      for (uint32_t component = 0; component < 4; ++component) {
        if (component) {
          details << ':';
        }
        AppendHex(details, LoadU32(base_, sqt + 16 + component * 4));
      }
      details << '@';
      for (uint32_t component = 0; component < 3; ++component) {
        if (component) {
          details << ':';
        }
        AppendHex(details, LoadU32(base_, sqt + 32 + component * 4));
      }
      details << ';';
      }
    };
    if (!gesture_stream_.empty()) {
      append_bones(kGestureBones);
    } else {
      append_bones(kContactBones);
    }
  }
  if (!gesture_stream_.empty()) {
    REXLOG_WARN(
        "cac-gesture: decoded observation={} frame={} phase={} selected={} "
        "stream='{}' clip=0x{:08X} extracts=0x{:08X} count={} "
        "decoder=0x{:08X} layout={}",
        observation_index_, frame_, cac_gesture::Phase(),
        cac_gesture::Selected(), gesture_stream_, gesture_clip_, extracts_,
        count_, decoder_, details.str());
  } else {
    REXLOG_WARN(
        "Skate 3 custom VBR extract observation={} frame={} rule={} "
        "extracts=0x{:08X} count={} decoder=0x{:08X} layout={}",
        observation_index_, frame_, rule_, extracts_, count_, decoder_,
        details.str());
  }
}

void PreloadConfiguredSkaterAnimationAssets(PPCContext &ctx, uint8_t *base,
                                            uint32_t playback_data) {
  if (!base || !playback_data) {
    return;
  }
  const uint32_t count = trick_overrides::LoadedAnimationAssetRuleCount();
  for (uint32_t index = 0; index < count; ++index) {
    const auto rule = trick_overrides::FindAnimationAssetByIndex(index);
    if (!rule || rule->playback_data != "SKATER") {
      continue;
    }
    EnsureCustomAnimationAssetLoaded(ctx, base, rule->animation, playback_data);
  }
}

void ApplySelectedAnimationOverride(PPCContext &ctx, uint8_t *base,
                                    uint32_t selected_descriptor_address) {
  custom_trick::ObservePlayerAnimationDispatchContext(
      ctx, base, g_local_action_graph_actor.load(std::memory_order_acquire));
  if (custom_trick::TryApplyQueuedAnimation(ctx, base,
                                            selected_descriptor_address)) {
    return;
  }
  const auto result = trick_overrides::ApplyAnimationOverride(
      ctx, base, selected_descriptor_address);
  if (!result.applied) {
    return;
  }

  const auto asset =
      EnsureCustomAnimationAssetLoaded(ctx, base, result.target_animation);
  if (asset.configured && (!asset.success || asset.load_only)) {
    StoreDescriptorWords(base, selected_descriptor_address,
                         result.original_descriptor);
    if (!asset.success || asset.load_only) {
      return;
    }
  }

  g_animation_override_count.fetch_add(1, std::memory_order_relaxed);
  if (!BeginEvent(EventKind::AnimationOverride)) {
    return;
  }

  std::ostringstream event;
  event << "AO@" << input_history_watch::CurrentFrameSequence() << ':';
  AppendHex(event, ctx.r31.u32);
  event << ':';
  AppendHex(event, ctx.r27.u32);
  event << ':';
  AppendHex(event, selected_descriptor_address);
  event << ':' << result.rule_name << ':' << result.source_animation << ':'
        << result.target_animation;
  for (uint32_t word : result.original_descriptor.words) {
    event << ':';
    AppendHex(event, word);
  }
  for (uint32_t word : result.final_descriptor.words) {
    event << ':';
    AppendHex(event, word);
  }
  PushFocusedEvent(event.str());
}

void ResetAndArm() {
  g_armed.store(false, std::memory_order_release);
  g_focused.store(false, std::memory_order_release);
  {
    std::lock_guard lock(g_event_mutex);
    g_focused_events.clear();
    g_score_reward_events.clear();
    g_local_score_collector_events.clear();
    g_local_spatial_samples.clear();
    g_actor_spatial_snapshots.clear();
    g_last_signatures.clear();
  }
  for (auto &count : g_counts) {
    count.store(0, std::memory_order_relaxed);
  }
  g_replacement_count.store(0, std::memory_order_relaxed);
  g_disable_count.store(0, std::memory_order_relaxed);
  g_scorable_override_count.store(0, std::memory_order_relaxed);
  g_display_name_override_count.store(0, std::memory_order_relaxed);
  g_animation_override_count.store(0, std::memory_order_relaxed);
  g_custom_input_token_insert_count.store(0, std::memory_order_relaxed);
  g_outer_circle_intent_injection_count.store(0,
                                               std::memory_order_relaxed);
  g_outer_circle_intent_pending_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_last_injection_frame.store(0, std::memory_order_relaxed);
  g_outer_circle_custom_hold_until_frame.store(0,
                                                std::memory_order_relaxed);
  g_outer_circle_intent_pending_intents.store(0, std::memory_order_relaxed);
  g_anim_attribute_focus_skeleton.store(0, std::memory_order_relaxed);
  g_local_action_graph_intents.store(0, std::memory_order_relaxed);
  g_local_phys_out.store(0, std::memory_order_relaxed);
  g_local_phys_out_actor.store(0, std::memory_order_relaxed);
  g_local_phys_out_root_offset.store(0xFFFFFFFFu, std::memory_order_relaxed);
  g_local_phys_out_child_offset.store(0xFFFFFFFFu, std::memory_order_relaxed);
  g_local_phys_out_component_offset.store(0xFFFFFFFFu,
                                          std::memory_order_relaxed);
  g_local_phys_out_interface_id.store(0, std::memory_order_relaxed);
  g_local_score_module.store(0, std::memory_order_relaxed);
  g_local_score_holder.store(0, std::memory_order_relaxed);
  g_pending_custom_air_collector.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_call_count.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_local_count.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_accept_count.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_last_frame.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_last_module.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_last_phys_out.store(0,
                                                std::memory_order_relaxed);
  g_custom_scorable_publish_last_output.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_last_before.store(0, std::memory_order_relaxed);
  g_custom_scorable_publish_last_after.store(0, std::memory_order_relaxed);
  g_local_score_collector_transition_event_count.store(
      0, std::memory_order_relaxed);
  g_local_score_grind_exit_event_count.store(0, std::memory_order_relaxed);
  g_local_spatial_revision.fetch_add(
      1, std::memory_order_acq_rel);
  g_local_spatial_frame.store(0, std::memory_order_relaxed);
  g_local_spatial_sample_time_us.store(
      0, std::memory_order_relaxed);
  g_local_board_controller.store(0, std::memory_order_relaxed);
  g_local_board_body.store(0, std::memory_order_relaxed);
  g_local_board_transform_state.store(0, std::memory_order_relaxed);
  g_local_board_position_x_bits.store(0, std::memory_order_relaxed);
  g_local_board_position_y_bits.store(0, std::memory_order_relaxed);
  g_local_board_position_z_bits.store(0, std::memory_order_relaxed);
  g_local_board_x_axis_x_bits.store(0, std::memory_order_relaxed);
  g_local_board_x_axis_y_bits.store(0, std::memory_order_relaxed);
  g_local_board_x_axis_z_bits.store(0, std::memory_order_relaxed);
  g_local_board_z_axis_x_bits.store(0, std::memory_order_relaxed);
  g_local_board_z_axis_y_bits.store(0, std::memory_order_relaxed);
  g_local_board_z_axis_z_bits.store(0, std::memory_order_relaxed);
  g_local_spatial_revision.fetch_add(
      1, std::memory_order_release);
  g_custom_animation_asset_stream_eval_count.store(0,
                                                   std::memory_order_relaxed);
  g_animation_leaf_replacement_eval_count.store(
      0, std::memory_order_relaxed);
  g_vbr_extract_observation_count.store(0, std::memory_order_relaxed);
  g_wipeout_request_check_count.store(0, std::memory_order_relaxed);
  g_wipeout_requested_true_count.store(0, std::memory_order_relaxed);
  g_last_wipeout_request_player.store(0, std::memory_order_relaxed);
  g_collision_force_wipeout_check_count.store(0, std::memory_order_relaxed);
  g_collision_force_wipeout_true_count.store(0, std::memory_order_relaxed);
  g_collision_force_wipeout_last_skeleton.store(0, std::memory_order_relaxed);
  g_collision_force_wipeout_last_argument_1_bits.store(
      0, std::memory_order_relaxed);
  g_collision_force_wipeout_last_argument_2_bits.store(
      0, std::memory_order_relaxed);
  g_physics_wants_wipeout_check_count.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_true_count.store(0, std::memory_order_relaxed);
  g_local_physics_wants_wipeout_true_count.store(0, std::memory_order_relaxed);
  g_local_action_graph_listener.store(0, std::memory_order_relaxed);
  g_local_action_graph_actor.store(0, std::memory_order_relaxed);
  g_local_action_graph_skater_anim.store(0, std::memory_order_relaxed);
  g_can_land_on_board_check_count.store(0, std::memory_order_relaxed);
  g_can_land_on_board_true_count.store(0, std::memory_order_relaxed);
  g_local_can_land_on_board_check_count.store(0, std::memory_order_relaxed);
  g_local_can_land_on_board_true_count.store(0, std::memory_order_relaxed);
  g_can_land_on_board_last_frame.store(0, std::memory_order_relaxed);
  g_can_land_on_board_last_context.store(0, std::memory_order_relaxed);
  g_can_land_on_board_last_actor.store(0, std::memory_order_relaxed);
  g_can_land_on_board_last_result.store(0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_check_count.store(0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_true_count.store(0, std::memory_order_relaxed);
  g_local_tilt_too_large_for_preland_check_count.store(
      0, std::memory_order_relaxed);
  g_local_tilt_too_large_for_preland_true_count.store(
      0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_last_frame.store(0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_last_context.store(0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_last_actor.store(0, std::memory_order_relaxed);
  g_tilt_too_large_for_preland_last_interface.store(0,
                                                    std::memory_order_relaxed);
  g_tilt_too_large_for_preland_last_result.store(0, std::memory_order_relaxed);
  g_is_landing_on_board_check_count.store(0, std::memory_order_relaxed);
  g_is_landing_on_board_true_count.store(0, std::memory_order_relaxed);
  g_local_is_landing_on_board_check_count.store(0, std::memory_order_relaxed);
  g_local_is_landing_on_board_true_count.store(0, std::memory_order_relaxed);
  g_is_landing_on_board_last_frame.store(0, std::memory_order_relaxed);
  g_is_landing_on_board_last_actor.store(0, std::memory_order_relaxed);
  g_is_landing_on_board_last_result.store(0, std::memory_order_relaxed);
  for (size_t mode = 0; mode < 2; ++mode) {
    g_is_landing_check_counts[mode].store(0, std::memory_order_relaxed);
    g_is_landing_true_counts[mode].store(0, std::memory_order_relaxed);
    g_local_is_landing_check_counts[mode].store(0, std::memory_order_relaxed);
    g_local_is_landing_true_counts[mode].store(0, std::memory_order_relaxed);
    g_is_landing_last_frames[mode].store(0, std::memory_order_relaxed);
    g_is_landing_last_actors[mode].store(0, std::memory_order_relaxed);
    g_is_landing_last_interfaces[mode].store(0, std::memory_order_relaxed);
    g_is_landing_last_results[mode].store(0, std::memory_order_relaxed);
  }
  g_physics_wants_wipeout_last_context.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_actor.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_skater_anim.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_true_frame.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_true_context.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_true_actor.store(0, std::memory_order_relaxed);
  g_physics_wants_wipeout_last_true_skater_anim.store(
      0, std::memory_order_relaxed);
  g_is_offboard_condition_check_count.store(0, std::memory_order_relaxed);
  g_is_offboard_condition_true_count.store(0, std::memory_order_relaxed);
  g_is_offboard_condition_last_result.store(0, std::memory_order_relaxed);
  g_is_offboard_condition_last_context.store(0, std::memory_order_relaxed);
  g_is_offboard_condition_last_owner.store(0, std::memory_order_relaxed);
  g_is_offboard_condition_last_provider.store(0, std::memory_order_relaxed);
  g_is_air_offboard_condition_check_count.store(0, std::memory_order_relaxed);
  g_is_air_offboard_condition_true_count.store(0, std::memory_order_relaxed);
  g_is_air_offboard_condition_last_result.store(0, std::memory_order_relaxed);
  g_board_state_sample_count.store(0, std::memory_order_relaxed);
  g_board_state_offboard_count.store(0, std::memory_order_relaxed);
  g_board_state_last_packed.store(0xFFFFFFFFu, std::memory_order_relaxed);
  g_board_state_last_frame.store(0, std::memory_order_relaxed);
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    g_custom_animation_asset_eval_results.clear();
  }
  g_armed.store(true, std::memory_order_release);
}

void SetFocus(bool focused) {
  if (focused) {
    std::lock_guard lock(g_event_mutex);
    g_focused_events.clear();
    g_local_spatial_samples.clear();
    g_last_signatures.clear();
    g_anim_attribute_focus_skeleton.store(0, std::memory_order_relaxed);
    g_outer_circle_last_injection_frame.store(0, std::memory_order_relaxed);
  }
  g_focused.store(focused, std::memory_order_release);
}

void ObserveLocalBoardState(uint64_t frame, uint8_t *base, uint32_t entity,
                            bool on_ground) {
  const uint32_t provider =
      g_is_offboard_condition_last_provider.load(std::memory_order_acquire);
  if (!base || !provider) {
    return;
  }
  const bool flag_673 = REX_LOAD_U8(provider + 673) != 0;
  const bool air_offboard = REX_LOAD_U8(provider + 674) != 0;
  const bool flag_675 = REX_LOAD_U8(provider + 675) != 0;
  const bool offboard = flag_673 || flag_675;
  const uint32_t packed =
      (flag_673 ? 1u : 0u) | (air_offboard ? 2u : 0u) | (flag_675 ? 4u : 0u);
  g_board_state_sample_count.fetch_add(1, std::memory_order_relaxed);
  if (offboard) {
    g_board_state_offboard_count.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t previous =
      g_board_state_last_packed.exchange(packed, std::memory_order_acq_rel);
  g_board_state_last_frame.store(frame, std::memory_order_release);
  if (previous != packed && g_focused.load(std::memory_order_acquire) &&
      BeginEvent(EventKind::IsOffboardCondition)) {
    std::ostringstream event;
    event << "BS@" << frame << ':';
    AppendHex(event, entity);
    event << ':';
    AppendHex(event, provider);
    event << ':' << (on_ground ? 1 : 0) << ':' << (offboard ? 1 : 0) << ':'
          << (air_offboard ? 1 : 0) << ':' << (flag_673 ? 1 : 0) << ':'
          << (flag_675 ? 1 : 0);
    PushFocusedEvent(event.str());
  }
}

void AppendObservationFields(std::ostream &response) {
  response << " custom_input_token_insert_count="
           << g_custom_input_token_insert_count.load(std::memory_order_acquire);
  response << " outer_circle_completion_count="
           << input_history_watch::OuterCircleCompletionCount();
  response << " outer_circle_reject_count="
           << input_history_watch::OuterCircleRejectCount();
  response << " outer_circle_intent_injection_count="
           << g_outer_circle_intent_injection_count.load(
                  std::memory_order_acquire);
  response << " wipeout_request_check_count="
           << g_wipeout_request_check_count.load(std::memory_order_acquire);
  response << " wipeout_requested_true_count="
           << g_wipeout_requested_true_count.load(std::memory_order_acquire);
  response << " last_wipeout_request_player=";
  AppendHex(response,
            g_last_wipeout_request_player.load(std::memory_order_acquire));
  response << " collision_force_wipeout_check_count="
           << g_collision_force_wipeout_check_count.load(
                  std::memory_order_acquire);
  response << " collision_force_wipeout_true_count="
           << g_collision_force_wipeout_true_count.load(
                  std::memory_order_acquire);
  response << " collision_force_wipeout_last_skeleton=";
  AppendHex(response, g_collision_force_wipeout_last_skeleton.load(
                          std::memory_order_acquire));
  response << " collision_force_wipeout_last_arguments=";
  AppendHex(response, g_collision_force_wipeout_last_argument_1_bits.load(
                          std::memory_order_acquire));
  response << ':';
  AppendHex(response, g_collision_force_wipeout_last_argument_2_bits.load(
                          std::memory_order_acquire));
  response << " physics_wants_wipeout_condition_factory_count="
           << g_physics_wants_wipeout_factory_count.load(
                  std::memory_order_acquire);
  response << " physics_wants_wipeout_condition_object=";
  AppendHex(response,
            g_physics_wants_wipeout_object.load(std::memory_order_acquire));
  response << " physics_wants_wipeout_condition_vtable=";
  AppendHex(response,
            g_physics_wants_wipeout_vtable.load(std::memory_order_acquire));
  response << " physics_wants_wipeout_condition_vtable_slots=";
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_physics_wants_wipeout_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " physics_wants_wipeout_check_count="
           << g_physics_wants_wipeout_check_count.load(
                  std::memory_order_acquire);
  response << " physics_wants_wipeout_true_count="
           << g_physics_wants_wipeout_true_count.load(
                  std::memory_order_acquire);
  response << " local_physics_wants_wipeout_true_count="
           << g_local_physics_wants_wipeout_true_count.load(
                  std::memory_order_acquire);
  response << " local_action_graph_listener=";
  AppendHex(response,
            g_local_action_graph_listener.load(std::memory_order_acquire));
  response << " local_action_graph_actor=";
  AppendHex(response,
            g_local_action_graph_actor.load(std::memory_order_acquire));
  response << " local_action_graph_skater_anim=";
  AppendHex(response,
            g_local_action_graph_skater_anim.load(std::memory_order_acquire));
  response << " physics_wants_wipeout_last_context=";
  AppendHex(response, g_physics_wants_wipeout_last_context.load(
                          std::memory_order_acquire));
  response << " physics_wants_wipeout_last_actor=";
  AppendHex(response,
            g_physics_wants_wipeout_last_actor.load(std::memory_order_acquire));
  response << " physics_wants_wipeout_last_skater_anim=";
  AppendHex(response, g_physics_wants_wipeout_last_skater_anim.load(
                          std::memory_order_acquire));
  response << " physics_wants_wipeout_last_true_frame="
           << g_physics_wants_wipeout_last_true_frame.load(
                  std::memory_order_acquire);
  response << " physics_wants_wipeout_last_true_context=";
  AppendHex(response, g_physics_wants_wipeout_last_true_context.load(
                          std::memory_order_acquire));
  response << " physics_wants_wipeout_last_true_actor=";
  AppendHex(response, g_physics_wants_wipeout_last_true_actor.load(
                          std::memory_order_acquire));
  response << " physics_wants_wipeout_last_true_skater_anim=";
  AppendHex(response, g_physics_wants_wipeout_last_true_skater_anim.load(
                          std::memory_order_acquire));
  response << " can_land_on_board_condition_factory_count="
           << g_can_land_on_board_factory_count.load(std::memory_order_acquire);
  response << " can_land_on_board_condition_object=";
  AppendHex(response,
            g_can_land_on_board_object.load(std::memory_order_acquire));
  response << " can_land_on_board_condition_vtable=";
  AppendHex(response,
            g_can_land_on_board_vtable.load(std::memory_order_acquire));
  response << " can_land_on_board_condition_vtable_slots=";
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_can_land_on_board_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " tilt_too_large_for_preland_condition_factory_count="
           << g_tilt_too_large_for_preland_factory_count.load(
                  std::memory_order_acquire);
  response << " tilt_too_large_for_preland_condition_object=";
  AppendHex(response, g_tilt_too_large_for_preland_object.load(
                          std::memory_order_acquire));
  response << " tilt_too_large_for_preland_condition_vtable=";
  AppendHex(response, g_tilt_too_large_for_preland_vtable.load(
                          std::memory_order_acquire));
  response << " tilt_too_large_for_preland_condition_vtable_slots=";
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_tilt_too_large_for_preland_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " is_landing_on_board_condition_factory_count="
           << g_is_landing_on_board_factory_count.load(
                  std::memory_order_acquire);
  response << " is_landing_on_board_condition_object=";
  AppendHex(response,
            g_is_landing_on_board_object.load(std::memory_order_acquire));
  response << " is_landing_on_board_condition_vtable=";
  AppendHex(response,
            g_is_landing_on_board_vtable.load(std::memory_order_acquire));
  response << " is_landing_on_board_condition_vtable_slots=";
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_is_landing_on_board_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " is_landing_condition_factory_count="
           << g_is_landing_factory_count.load(std::memory_order_acquire);
  response << " is_landing_condition_object=";
  AppendHex(response, g_is_landing_object.load(std::memory_order_acquire));
  response << " is_landing_condition_vtable=";
  AppendHex(response, g_is_landing_vtable.load(std::memory_order_acquire));
  response << " is_landing_condition_vtable_slots=";
  for (size_t slot = 0; slot < kPhysicsWantsWipeoutVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response,
              g_is_landing_vtable_slots[slot].load(std::memory_order_acquire));
  }
  response << " can_land_on_board_check_count="
           << g_can_land_on_board_check_count.load(std::memory_order_acquire);
  response << " can_land_on_board_true_count="
           << g_can_land_on_board_true_count.load(std::memory_order_acquire);
  response << " local_can_land_on_board_check_count="
           << g_local_can_land_on_board_check_count.load(
                  std::memory_order_acquire);
  response << " local_can_land_on_board_true_count="
           << g_local_can_land_on_board_true_count.load(
                  std::memory_order_acquire);
  response << " can_land_on_board_last="
           << g_can_land_on_board_last_frame.load(std::memory_order_acquire)
           << ':';
  AppendHex(response,
            g_can_land_on_board_last_context.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_can_land_on_board_last_actor.load(std::memory_order_acquire));
  response << ':'
           << g_can_land_on_board_last_result.load(std::memory_order_acquire);
  response << " tilt_too_large_for_preland_check_count="
           << g_tilt_too_large_for_preland_check_count.load(
                  std::memory_order_acquire);
  response << " tilt_too_large_for_preland_true_count="
           << g_tilt_too_large_for_preland_true_count.load(
                  std::memory_order_acquire);
  response << " local_tilt_too_large_for_preland_check_count="
           << g_local_tilt_too_large_for_preland_check_count.load(
                  std::memory_order_acquire);
  response << " local_tilt_too_large_for_preland_true_count="
           << g_local_tilt_too_large_for_preland_true_count.load(
                  std::memory_order_acquire);
  response << " tilt_too_large_for_preland_last="
           << g_tilt_too_large_for_preland_last_frame.load(
                  std::memory_order_acquire)
           << ':';
  AppendHex(response, g_tilt_too_large_for_preland_last_context.load(
                          std::memory_order_acquire));
  response << ':';
  AppendHex(response, g_tilt_too_large_for_preland_last_actor.load(
                          std::memory_order_acquire));
  response << ':';
  AppendHex(response, g_tilt_too_large_for_preland_last_interface.load(
                          std::memory_order_acquire));
  response << ':'
           << g_tilt_too_large_for_preland_last_result.load(
                  std::memory_order_acquire);
  response << " is_landing_on_board_check_count="
           << g_is_landing_on_board_check_count.load(std::memory_order_acquire);
  response << " is_landing_on_board_true_count="
           << g_is_landing_on_board_true_count.load(std::memory_order_acquire);
  response << " local_is_landing_on_board_check_count="
           << g_local_is_landing_on_board_check_count.load(
                  std::memory_order_acquire);
  response << " local_is_landing_on_board_true_count="
           << g_local_is_landing_on_board_true_count.load(
                  std::memory_order_acquire);
  response << " is_landing_on_board_last="
           << g_is_landing_on_board_last_frame.load(std::memory_order_acquire)
           << ':';
  AppendHex(response,
            g_is_landing_on_board_last_actor.load(std::memory_order_acquire));
  response << ':'
           << g_is_landing_on_board_last_result.load(std::memory_order_acquire);
  for (size_t mode = 0; mode < 2; ++mode) {
    const char *mode_name = mode ? "onboard" : "offboard";
    response << " is_landing_" << mode_name << "_check_count="
             << g_is_landing_check_counts[mode].load(std::memory_order_acquire);
    response << " is_landing_" << mode_name << "_true_count="
             << g_is_landing_true_counts[mode].load(std::memory_order_acquire);
    response << " local_is_landing_" << mode_name << "_check_count="
             << g_local_is_landing_check_counts[mode].load(
                    std::memory_order_acquire);
    response << " local_is_landing_" << mode_name << "_true_count="
             << g_local_is_landing_true_counts[mode].load(
                    std::memory_order_acquire);
    response << " is_landing_" << mode_name << "_last="
             << g_is_landing_last_frames[mode].load(std::memory_order_acquire)
             << ':';
    AppendHex(response,
              g_is_landing_last_actors[mode].load(std::memory_order_acquire));
    response << ':';
    AppendHex(response, g_is_landing_last_interfaces[mode].load(
                            std::memory_order_acquire));
    response << ':'
             << g_is_landing_last_results[mode].load(std::memory_order_acquire);
  }
  response << " is_offboard_condition_factory_count="
           << g_is_offboard_condition_factory_count.load(
                  std::memory_order_acquire);
  response << " is_offboard_condition_object=";
  AppendHex(response,
            g_is_offboard_condition_object.load(std::memory_order_acquire));
  response << " is_offboard_condition_vtable=";
  AppendHex(response,
            g_is_offboard_condition_vtable.load(std::memory_order_acquire));
  response << " is_offboard_condition_vtable_slots=";
  for (size_t slot = 0; slot < kIsOffboardVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_is_offboard_condition_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " is_offboard_condition_check_count="
           << g_is_offboard_condition_check_count.load(
                  std::memory_order_acquire);
  response << " is_offboard_condition_true_count="
           << g_is_offboard_condition_true_count.load(
                  std::memory_order_acquire);
  response << " is_offboard_condition_last_result="
           << g_is_offboard_condition_last_result.load(
                  std::memory_order_acquire);
  response << " is_air_offboard_condition_factory_count="
           << g_is_air_offboard_condition_factory_count.load(
                  std::memory_order_acquire);
  response << " is_air_offboard_condition_object=";
  AppendHex(response,
            g_is_air_offboard_condition_object.load(std::memory_order_acquire));
  response << " is_air_offboard_condition_vtable=";
  AppendHex(response,
            g_is_air_offboard_condition_vtable.load(std::memory_order_acquire));
  response << " is_air_offboard_condition_vtable_slots=";
  for (size_t slot = 0; slot < kIsOffboardVtableSlotCount; ++slot) {
    if (slot != 0) {
      response << ':';
    }
    AppendHex(response, g_is_air_offboard_condition_vtable_slots[slot].load(
                            std::memory_order_acquire));
  }
  response << " is_offboard_condition_last_context=";
  AppendHex(response, g_is_offboard_condition_last_context.load(
                          std::memory_order_acquire));
  response << " is_offboard_condition_last_owner=";
  AppendHex(response,
            g_is_offboard_condition_last_owner.load(std::memory_order_acquire));
  response << " is_offboard_condition_last_provider=";
  AppendHex(response, g_is_offboard_condition_last_provider.load(
                          std::memory_order_acquire));
  response << " is_air_offboard_condition_check_count="
           << g_is_air_offboard_condition_check_count.load(
                  std::memory_order_acquire);
  response << " is_air_offboard_condition_true_count="
           << g_is_air_offboard_condition_true_count.load(
                  std::memory_order_acquire);
  response << " is_air_offboard_condition_last_result="
           << g_is_air_offboard_condition_last_result.load(
                  std::memory_order_acquire);
  response << " offboard_provider_vtable=";
  AppendHex(response,
            g_offboard_provider_vtable.load(std::memory_order_acquire));
  response << " offboard_provider_methods=";
  AppendHex(response,
            g_offboard_provider_method_608.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_offboard_provider_method_612.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_offboard_provider_method_616.load(std::memory_order_acquire));
  response << " board_state_sample_count="
           << g_board_state_sample_count.load(std::memory_order_acquire);
  response << " board_state_offboard_count="
           << g_board_state_offboard_count.load(std::memory_order_acquire);
  const uint32_t board_state =
      g_board_state_last_packed.load(std::memory_order_acquire);
  response << " board_state_valid=" << (board_state == 0xFFFFFFFFu ? 0 : 1)
           << " board_state_offboard="
           << (board_state != 0xFFFFFFFFu &&
                       ((board_state & 1u) != 0 || (board_state & 4u) != 0)
                   ? 1
                   : 0)
           << " board_state_air_offboard="
           << (board_state != 0xFFFFFFFFu && (board_state & 2u) != 0 ? 1 : 0)
           << " board_state_flag_673="
           << (board_state != 0xFFFFFFFFu && (board_state & 1u) != 0 ? 1 : 0)
           << " board_state_flag_675="
           << (board_state != 0xFFFFFFFFu && (board_state & 4u) != 0 ? 1 : 0)
           << " board_state_last_frame="
           << g_board_state_last_frame.load(std::memory_order_acquire);
  response << " gesture_mapping_initialized="
           << (g_mapping_initialized.load(std::memory_order_acquire) ? 1 : 0);
  response << " gesture_mapping_address=";
  AppendHex(response, g_mapping_address.load(std::memory_order_acquire));
  response << " gesture_mapping_table_counts=";
  for (size_t i = 0; i < g_mapping_table_counts.size(); ++i) {
    if (i != 0) {
      response << ':';
    }
    response << g_mapping_table_counts[i].load(std::memory_order_acquire);
  }
  response << " gesture_mapping_bucket_counts=";
  for (size_t i = 0; i < g_mapping_bucket_counts.size(); ++i) {
    if (i != 0) {
      response << ':';
    }
    response << g_mapping_bucket_counts[i].load(std::memory_order_acquire);
  }
  response << " gesture_replacement_enabled="
           << (trick_overrides::KickflipReplacementEnabled() ? 1 : 0);
  response << " gesture_replacement_count="
           << g_replacement_count.load(std::memory_order_acquire);
  response << " gesture_disable_enabled="
           << (trick_overrides::KickflipDisableEnabled() ? 1 : 0);
  response << " gesture_disable_count="
           << g_disable_count.load(std::memory_order_acquire);
  response << " gesture_override_file_configured="
           << (trick_overrides::OverrideFileConfigured() ? 1 : 0);
  response << " gesture_override_file_loaded="
           << (trick_overrides::OverrideFileLoaded() ? 1 : 0);
  response << " gesture_override_file_status="
           << trick_overrides::OverrideFileStatus();
  response << " gesture_override_rule_count="
           << trick_overrides::LoadedRuleCount();
  response << " scorable_override_rule_count="
           << trick_overrides::LoadedScorableRuleCount();
  response << " animation_override_rule_count="
           << trick_overrides::LoadedAnimationRuleCount();
  response << " animation_asset_rule_count="
           << trick_overrides::LoadedAnimationAssetRuleCount();
  response << " scorable_override_count="
           << g_scorable_override_count.load(std::memory_order_acquire);
  response << " display_name_override_count="
           << g_display_name_override_count.load(std::memory_order_acquire);
  response << " animation_override_count="
           << g_animation_override_count.load(std::memory_order_acquire);
  response << " animation_loader_add_count="
           << g_animation_loader_add_count.load(std::memory_order_acquire);
  response << " animation_loader_registrations=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_animation_loader_registrations) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " animation_loader_lookup_count="
           << g_animation_loader_lookup_count.load(std::memory_order_acquire);
  response << " animation_loader_lookup_results=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, result] : g_animation_loader_lookup_results) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@';
      AppendHex(response, result);
    }
  }
  response << " animation_loader_load_count="
           << g_animation_loader_load_count.load(std::memory_order_acquire);
  response << " animation_loader_loads=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_animation_loader_loads) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " animation_loader_async_paths=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[path, result] : g_animation_loader_async_paths) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << path << '@';
      AppendHex(response, result);
    }
  }
  response << " animation_loader_completion_count="
           << g_animation_loader_completion_count.load(
                  std::memory_order_acquire);
  response << " animation_loader_completions=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_animation_loader_completions) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " playback_data_construction_count="
           << g_playback_data_construction_count.load(
                  std::memory_order_acquire);
  response << " playback_data_constructions=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_playback_data_constructions) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " playback_data_lookup_count="
           << g_playback_data_lookup_count.load(std::memory_order_acquire);
  response << " playback_data_lookup_results=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_playback_data_lookup_results) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " andale_database_load_count="
           << g_andale_database_load_count.load(std::memory_order_acquire);
  response << " andale_database_loads=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[path, value] : g_andale_database_loads) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << path << '@' << value;
    }
  }
  response << " andale_database_content_count="
           << g_andale_database_content_count.load(std::memory_order_acquire);
  response << " andale_database_contents=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[key, value] : g_andale_database_contents) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << key << '@' << value;
    }
  }
  response << " custom_animation_asset_load_attempt_count="
           << g_custom_animation_asset_load_attempt_count.load(
                  std::memory_order_acquire);
  response << " custom_animation_asset_load_success_count="
           << g_custom_animation_asset_load_success_count.load(
                  std::memory_order_acquire);
  response << " custom_animation_asset_stream_eval_count="
           << g_custom_animation_asset_stream_eval_count.load(
                  std::memory_order_acquire);
  response << " animation_leaf_replacement_rule_count="
           << trick_overrides::LoadedAnimationLeafReplacementRuleCount();
  response << " animation_leaf_replacement_bind_count="
           << g_animation_leaf_replacement_bind_count.load(
                  std::memory_order_acquire);
  response << " animation_leaf_replacement_eval_count="
           << g_animation_leaf_replacement_eval_count.load(
                  std::memory_order_acquire);
  response << " custom_animation_asset_results=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_custom_animation_asset_results) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " custom_animation_asset_eval_results=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] : g_custom_animation_asset_eval_results) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " animation_leaf_replacement_results=";
  {
    std::lock_guard asset_lock(g_animation_asset_mutex);
    bool first_asset = true;
    for (const auto &[name, value] :
         g_animation_leaf_replacement_results) {
      if (!first_asset) {
        response << ',';
      }
      first_asset = false;
      response << name << '@' << value;
    }
  }
  response << " trick_pipeline_counts=";
  for (size_t i = 0; i < g_counts.size(); ++i) {
    if (i != 0) {
      response << ':';
    }
    response << g_counts[i].load(std::memory_order_acquire);
  }
  response
      << " score_reward_event_count="
      << g_counts[static_cast<size_t>(EventKind::ScoreHolderRewardAirSequence)]
             .load(std::memory_order_acquire);
  response
      << " score_bank_event_count="
      << g_counts[static_cast<size_t>(EventKind::ScoreHolderPublishAirSequence)]
             .load(std::memory_order_acquire);
  const uint64_t grind_exit_count =
      g_counts[static_cast<size_t>(EventKind::GrindCollectorExit)].load(
          std::memory_order_acquire);
  response << " score_grind_exit_event_count=" << grind_exit_count;
  // Compatibility alias retained for manifests created before the collector
  // type label was recovered.
  response << " score_auxiliary_event_count=" << grind_exit_count;
  response << " score_collector_transition_event_count="
           << g_counts[static_cast<size_t>(EventKind::ScoreCollectorTransition)]
                  .load(std::memory_order_acquire);
  response << " local_phys_out=";
  AppendHex(response, g_local_phys_out.load(std::memory_order_acquire));
  response << " local_phys_out_actor=";
  AppendHex(response, g_local_phys_out_actor.load(std::memory_order_acquire));
  response << " local_phys_out_resolution=";
  AppendHex(response,
            g_local_phys_out_root_offset.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_phys_out_child_offset.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_phys_out_component_offset.load(std::memory_order_acquire));
  response << " local_phys_out_interface_id=";
  AppendHex(response,
            g_local_phys_out_interface_id.load(std::memory_order_acquire));
  response << " local_score_module="
           << g_local_score_module.load(std::memory_order_acquire);
  response << " local_score_holder="
           << g_local_score_holder.load(std::memory_order_acquire);
  response << " custom_scorable_publish_call_count="
           << g_custom_scorable_publish_call_count.load(
                  std::memory_order_acquire);
  response << " custom_scorable_publish_local_count="
           << g_custom_scorable_publish_local_count.load(
                  std::memory_order_acquire);
  response << " custom_scorable_publish_accept_count="
           << g_custom_scorable_publish_accept_count.load(
                  std::memory_order_acquire);
  response << " custom_scorable_publish_last_frame="
           << g_custom_scorable_publish_last_frame.load(
                  std::memory_order_acquire);
  response << " custom_scorable_publish_last_module=";
  AppendHex(response, g_custom_scorable_publish_last_module.load(
                          std::memory_order_acquire));
  response << " custom_scorable_publish_last_phys_out=";
  AppendHex(response, g_custom_scorable_publish_last_phys_out.load(
                          std::memory_order_acquire));
  response << " custom_scorable_publish_last_output=";
  AppendHex(response, g_custom_scorable_publish_last_output.load(
                          std::memory_order_acquire));
  response << " custom_scorable_publish_last_id=";
  AppendHex(response, g_custom_scorable_publish_last_before.load(
                          std::memory_order_acquire));
  response << '>';
  AppendHex(response, g_custom_scorable_publish_last_after.load(
                          std::memory_order_acquire));
  response << " local_score_collector_transition_event_count="
           << g_local_score_collector_transition_event_count.load(
                  std::memory_order_acquire);
  response << " local_score_grind_exit_event_count="
           << g_local_score_grind_exit_event_count.load(
                  std::memory_order_acquire);
  response << " local_score_collector_events=";
  {
    std::lock_guard lock(g_event_mutex);
    for (size_t index = 0; index < g_local_score_collector_events.size();
         ++index) {
      if (index != 0) {
        response << ',';
      }
      response << g_local_score_collector_events[index];
    }
  }
  response << " local_spatial_frame="
           << g_local_spatial_frame.load(std::memory_order_acquire);
  response << " local_board_controller="
           << g_local_board_controller.load(std::memory_order_acquire);
  response << " local_board_body="
           << g_local_board_body.load(std::memory_order_acquire);
  response << " local_board_transform_state="
           << g_local_board_transform_state.load(std::memory_order_acquire);
  response << " local_board_position_bits=";
  AppendHex(response,
            g_local_board_position_x_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_position_y_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_position_z_bits.load(std::memory_order_acquire));
  response << " local_board_x_axis_bits=";
  AppendHex(response,
            g_local_board_x_axis_x_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_x_axis_y_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_x_axis_z_bits.load(std::memory_order_acquire));
  response << " local_board_z_axis_bits=";
  AppendHex(response,
            g_local_board_z_axis_x_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_z_axis_y_bits.load(std::memory_order_acquire));
  response << ':';
  AppendHex(response,
            g_local_board_z_axis_z_bits.load(std::memory_order_acquire));
  response << " local_spatial_samples=";
  {
    std::lock_guard lock(g_event_mutex);
    bool first_sample = true;
    for (const auto &sample : g_local_spatial_samples) {
      if (!first_sample) {
        response << ',';
      }
      first_sample = false;
      response << sample.frame << ':';
      AppendHex(response, sample.position_x_bits);
      response << ':';
      AppendHex(response, sample.position_y_bits);
      response << ':';
      AppendHex(response, sample.position_z_bits);
      response << ':';
      AppendHex(response, sample.x_axis_x_bits);
      response << ':';
      AppendHex(response, sample.x_axis_y_bits);
      response << ':';
      AppendHex(response, sample.x_axis_z_bits);
      response << ':';
      AppendHex(response, sample.z_axis_x_bits);
      response << ':';
      AppendHex(response, sample.z_axis_y_bits);
      response << ':';
      AppendHex(response, sample.z_axis_z_bits);
    }
  }
  response << " score_reward_events=";
  {
    std::lock_guard lock(g_event_mutex);
    bool first_reward = true;
    for (const auto &event : g_score_reward_events) {
      if (!first_reward) {
        response << ',';
      }
      first_reward = false;
      response << event;
    }
  }
  response << " trick_pipeline_events=";
  std::lock_guard lock(g_event_mutex);
  bool first = true;
  for (const auto &event : g_focused_events) {
    if (!first) {
      response << ',';
    }
    first = false;
    response << event;
  }
}

} // namespace skate3::trick_pipeline
