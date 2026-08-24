#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>

struct PPCContext;

namespace skate3::native_collision {

// Observes the retail WorldStreamerView::AddVolume boundary and returns true
// only when an installed exclusive owned world must reject that retail
// streamer batch before it can enter a native physics query.
bool ShouldSuppressWorldStreamerAddVolume(const PPCContext& ctx,
                                          std::uint8_t* base) noexcept;

// Read-only probes used by the generated native line/box query path. They
// identify the first stage at which an owned mesh stops producing contacts.
void ObserveNativeLineWorker(std::uint32_t mesh) noexcept;
void ObserveNativeBoxWorker(std::uint32_t mesh) noexcept;
void ObserveNativeIteratorMesh(std::uint32_t mesh) noexcept;
void ObserveNativeQueryMesh(std::uint32_t mesh) noexcept;
void ObserveNativeLineQueryBatch(std::uint32_t batch,
                                 std::uint8_t* base) noexcept;
void PrepareNativeBoxQueryBatch(std::uint32_t batch,
                                std::uint8_t* base) noexcept;
void ObserveNativeClusterDecode(std::uint32_t triangle_count) noexcept;
void PrepareNativeTriangleTest(std::uint32_t result,
                               std::uint32_t line_start,
                               std::uint32_t line_delta,
                               std::uint8_t* base) noexcept;
void ObserveNativeTriangleResult(std::uint32_t hit,
                                 std::uint32_t decoded_triangle,
                                 std::uint8_t* base) noexcept;
// Called only after Skate's native audio/physics/pattern query masks accept a
// geometric triangle hit. This separates contacts that can influence a query
// from the broader TriangleLineSegIntersect candidate stream.
void ObserveNativeTriangleAccepted(std::uint32_t decoded_triangle,
                                   std::uint32_t worker,
                                   std::uint8_t* base) noexcept;
// Called after the native result collector has had an opportunity to commit
// an accepted candidate. For worker 1, result and candidate are compared
// byte-for-byte across the fields copied by the collector. Worker 2 calls
// this only from its closer-hit update branch.
void ObserveNativeTriangleSelected(std::uint32_t decoded_triangle,
                                   std::uint32_t result,
                                   std::uint32_t candidate,
                                   std::uint32_t worker,
                                   std::uint8_t* base) noexcept;

// Brackets rw::collision::PrimitivePairIntersect so telemetry can distinguish
// physical volume contacts from the line/box query hits above.
void BeginNativePrimitivePair(std::uint32_t result,
                              std::uint32_t volume_a,
                              std::uint32_t transform_a,
                              std::uint32_t volume_b,
                              std::uint32_t transform_b,
                              std::uint8_t* base) noexcept;
void EndNativePrimitivePair(std::uint32_t hit,
                            std::uint8_t* base) noexcept;

// Emits bounded, position-aware collision telemetry from the verified local
// board seam. It compares the player against the package collision world and
// records native query activity without changing the player or contact data.
void ObservePlayerCollisionTelemetry(const float world_position[3],
                                     std::uint64_t frame) noexcept;

bool Enabled();

// Returns the exact translation registered with the native collision
// collection. Presentation uses this same value so visible and physical
// geometry cannot drift onto separate coordinate seams.
bool MapWorldOrigin(float out_origin[3]) noexcept;

// Returns the last pose committed to both native collision buffers. The
// renderer consumes this publication so visual and physical motion share one
// authoritative transform.
bool KinematicObjectPose(std::size_t index, float out_position[3],
                         float out_velocity[3],
                         std::uint64_t* out_frame = nullptr) noexcept;
bool HingedDoorPose(std::size_t index, float* out_angle_radians,
                    float* out_angular_velocity,
                    std::uint64_t* out_frame = nullptr) noexcept;

// Compiles and registers the project-owned map as a real RenderWare
// ClusteredMesh/AggregateVolume. The native board ground point is used only
// to place the local map in world space; this function never writes a player
// or board transform.
void EnsureInstalled(PPCContext& ctx,
                     std::uint8_t* base,
                     std::uint32_t skateboard,
                     const float map_origin[3]) noexcept;

// Advances project-owned moving objects once per deterministic input frame.
// Active native query jobs are drained before both collection buffers are
// updated with the game's own volume-entry constructor.
void UpdateKinematicObjects(PPCContext& ctx,
                            std::uint8_t* base) noexcept;
void UpdateHingedDoors(PPCContext& ctx,
                       std::uint8_t* base) noexcept;

void AppendTelemetry(std::ostream& out);

}  // namespace skate3::native_collision
