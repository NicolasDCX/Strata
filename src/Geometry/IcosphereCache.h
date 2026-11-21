#pragma once

#include "Geometry/Icosphere.h"

#include <filesystem>
#include <string>

namespace se::geo {

/// Optional on-disk cache of the pristine icosphere: the flat vertex positions
/// and triangle indices exactly as makeIcosphere() hands them to the Mesh
/// constructor (after spatial ordering), so a loaded mesh is bit-identical to a
/// generated one. Page layout is not stored: the Mesh is built from the arrays
/// with whatever MeshLayout the caller asks for.
///
/// File layout (little-endian, as written by this machine):
///   IcosphereCacheHeader (64 bytes)
///   positions: vertexCount x 3 float
///   indices:   triangleCount x 3 u32
/// The header names the format version, the generator version and the
/// generation settings (subdivisions, radius, vertex order), the element counts
/// and a 64-bit checksum of the two arrays. A file whose header does not match
/// the request exactly, has the wrong size or the wrong checksum is ignored
/// (and, by makeIcosphereCached(), regenerated and overwritten).
struct IcosphereCacheHeader {
    char magic[8];             ///< "SEICOSPH"
    u32 formatVersion;         ///< kIcosphereCacheFormatVersion
    u32 generatorVersion;      ///< kIcosphereGeneratorVersion
    i32 subdivisions;
    float radius;
    u32 vertexOrder;           ///< VertexOrder as an integer
    u32 headerBytes;           ///< sizeof(IcosphereCacheHeader)
    u64 vertexCount;
    u64 triangleCount;
    u64 checksum;              ///< icosphereCacheChecksum() of positions then indices
    u64 reserved;
};
static_assert(sizeof(IcosphereCacheHeader) == 64, "cache header layout changed");

/// Bump when the file layout changes.
inline constexpr u32 kIcosphereCacheFormatVersion = 1;

/// How makeIcosphereCached() obtained the mesh.
enum class IcosphereSource {
    Generated,       ///< no cache directory: generated as before
    CacheLoaded,     ///< read from a valid cache file
    CacheCreated,    ///< generated, then written to the cache (missing or incompatible file)
};

struct IcosphereCacheStats {
    IcosphereSource source = IcosphereSource::Generated;
    std::filesystem::path file;  ///< cache file used (empty when no directory was given)
    std::string rejected;        ///< why an existing file was not used (empty if none was found or it was used)
    double generateMs = 0.0;     ///< subdivision + spatial ordering (Generated, CacheCreated)
    double readMs = 0.0;         ///< reading the file (CacheLoaded)
    double checksumMs = 0.0;     ///< checksum on load or on write
    double writeMs = 0.0;        ///< writing the file (CacheCreated)
    double buildMs = 0.0;        ///< Mesh construction from the arrays (adjacency, normals, pages)
    u64 fileBytes = 0;
};

/// File name for the given settings, e.g. "icosphere_L11_r1_spatial_f1_g1.bin".
[[nodiscard]] std::string icosphereCacheFileName(int subdivisions, float radius, VertexOrder order);

/// 64-bit checksum of the arrays as stored (word-wise, independent lanes).
[[nodiscard]] u64 icosphereCacheChecksum(const std::vector<glm::vec3>& positions, const std::vector<u32>& indices);

/// Writes the arrays to `file` (through a temporary file, renamed when complete).
/// Returns false and leaves no partial file on failure.
bool saveIcosphereCache(const std::filesystem::path& file, int subdivisions, float radius, VertexOrder order,
                        const std::vector<glm::vec3>& positions, const std::vector<u32>& indices,
                        IcosphereCacheStats* stats = nullptr);

/// Reads `file` into the arrays if its header matches the settings and the
/// current versions, its size is exact and its checksum matches. Otherwise
/// returns false with the reason in `reason`.
bool loadIcosphereCache(const std::filesystem::path& file, int subdivisions, float radius, VertexOrder order,
                        std::vector<glm::vec3>& positions, std::vector<u32>& indices, std::string& reason,
                        IcosphereCacheStats* stats = nullptr);

/// makeIcosphere() through the cache in `directory`: loads the matching file if
/// it is valid, else generates the sphere and writes the file (creating the
/// directory). An empty `directory` generates without caching. The result is
/// bit-identical in every case.
[[nodiscard]] Mesh makeIcosphereCached(const std::filesystem::path& directory, int subdivisions, float radius = 1.0f,
                                       VertexOrder order = VertexOrder::Spatial, const MeshLayout& layout = {},
                                       IcosphereCacheStats* stats = nullptr);

} // namespace se::geo
