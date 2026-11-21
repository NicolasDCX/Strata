#include "Geometry/IcosphereCache.h"

#include "Core/Timer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

namespace se::geo {

namespace {

constexpr char kMagic[8] = {'S', 'E', 'I', 'C', 'O', 'S', 'P', 'H'};
constexpr std::size_t kIoChunkBytes = std::size_t{64} << 20;

/// 8 independent FNV-style lanes over 64-bit words, folded at the end, so the
/// loop is not one long multiply chain. Trailing bytes are zero-padded.
struct Checksum {
    static constexpr int kLanes = 8;
    u64 lanes[kLanes];
    u64 bytes = 0;

    Checksum()
    {
        for (int i = 0; i < kLanes; ++i) {
            lanes[i] = 1469598103934665603ull + static_cast<u64>(i) * 0x9E3779B97F4A7C15ull;
        }
    }

    void add(const void* data, std::size_t size)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        std::size_t words = size / 8;
        std::size_t i = 0;
        for (; i + kLanes <= words; i += kLanes) {
            for (int l = 0; l < kLanes; ++l) {
                u64 w;
                std::memcpy(&w, p + (i + static_cast<std::size_t>(l)) * 8, 8);
                lanes[l] = (lanes[l] ^ w) * 1099511628211ull;
            }
        }
        for (; i < words; ++i) {
            u64 w;
            std::memcpy(&w, p + i * 8, 8);
            lanes[i % kLanes] = (lanes[i % kLanes] ^ w) * 1099511628211ull;
        }
        const std::size_t rest = size - words * 8;
        if (rest > 0) {
            u64 w = 0;
            std::memcpy(&w, p + words * 8, rest);
            lanes[0] = (lanes[0] ^ w) * 1099511628211ull;
        }
        bytes += size;
    }

    [[nodiscard]] u64 value() const
    {
        u64 h = bytes;
        for (const u64 lane : lanes) {
            h = (h ^ lane) * 1099511628211ull;
            h ^= h >> 29;
        }
        return h;
    }
};

IcosphereCacheHeader makeHeader(int subdivisions, float radius, VertexOrder order, u64 vertexCount, u64 triangleCount,
                                u64 checksum)
{
    IcosphereCacheHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(kMagic));
    header.formatVersion = kIcosphereCacheFormatVersion;
    header.generatorVersion = kIcosphereGeneratorVersion;
    header.subdivisions = subdivisions;
    header.radius = radius;
    header.vertexOrder = static_cast<u32>(order);
    header.headerBytes = sizeof(IcosphereCacheHeader);
    header.vertexCount = vertexCount;
    header.triangleCount = triangleCount;
    header.checksum = checksum;
    return header;
}

bool readExact(std::ifstream& in, void* data, std::size_t size)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        const std::size_t chunk = std::min(size, kIoChunkBytes);
        in.read(p, static_cast<std::streamsize>(chunk));
        if (!in || static_cast<std::size_t>(in.gcount()) != chunk) {
            return false;
        }
        p += chunk;
        size -= chunk;
    }
    return true;
}

bool writeExact(std::ofstream& out, const void* data, std::size_t size)
{
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        const std::size_t chunk = std::min(size, kIoChunkBytes);
        out.write(p, static_cast<std::streamsize>(chunk));
        if (!out) {
            return false;
        }
        p += chunk;
        size -= chunk;
    }
    return true;
}

} // namespace

std::string icosphereCacheFileName(int subdivisions, float radius, VertexOrder order)
{
    subdivisions = std::clamp(subdivisions, 0, kMaxIcosphereSubdivisions);
    // The radius goes into the name by its bits when it is not a whole number,
    // so two radii never share a file.
    std::string radiusText;
    if (radius == std::floor(radius) && radius >= 0.0f && radius < 1e6f) {
        radiusText = std::to_string(static_cast<long>(radius));
    } else {
        u32 bits;
        std::memcpy(&bits, &radius, 4);
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "x%08x", bits);
        radiusText = buffer;
    }
    return "icosphere_L" + std::to_string(subdivisions) + "_r" + radiusText +
           (order == VertexOrder::Spatial ? "_spatial" : "_generation") + "_f" +
           std::to_string(kIcosphereCacheFormatVersion) + "_g" + std::to_string(kIcosphereGeneratorVersion) + ".bin";
}

u64 icosphereCacheChecksum(const std::vector<glm::vec3>& positions, const std::vector<u32>& indices)
{
    Checksum checksum;
    checksum.add(positions.data(), positions.size() * sizeof(glm::vec3));
    checksum.add(indices.data(), indices.size() * sizeof(u32));
    return checksum.value();
}

bool saveIcosphereCache(const std::filesystem::path& file, int subdivisions, float radius, VertexOrder order,
                        const std::vector<glm::vec3>& positions, const std::vector<u32>& indices, IcosphereCacheStats* stats)
{
    core::Stopwatch timer;
    const u64 checksum = icosphereCacheChecksum(positions, indices);
    if (stats != nullptr) {
        stats->checksumMs += timer.elapsedMs();
    }
    timer.reset();
    const IcosphereCacheHeader header =
        makeHeader(subdivisions, radius, order, positions.size(), indices.size() / 3, checksum);
    std::filesystem::path temporary = file;
    temporary += ".partial";
    bool ok = false;
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        ok = out && writeExact(out, &header, sizeof(header)) &&
             writeExact(out, positions.data(), positions.size() * sizeof(glm::vec3)) &&
             writeExact(out, indices.data(), indices.size() * sizeof(u32));
        out.close();
        ok = ok && !out.fail();
    }
    std::error_code error;
    if (ok) {
        std::filesystem::remove(file, error);
        std::filesystem::rename(temporary, file, error);
        ok = !error;
    }
    if (!ok) {
        std::filesystem::remove(temporary, error);
    }
    if (stats != nullptr) {
        stats->writeMs += timer.elapsedMs();
        stats->fileBytes = ok ? sizeof(header) + positions.size() * sizeof(glm::vec3) + indices.size() * sizeof(u32) : 0;
    }
    return ok;
}

bool loadIcosphereCache(const std::filesystem::path& file, int subdivisions, float radius, VertexOrder order,
                        std::vector<glm::vec3>& positions, std::vector<u32>& indices, std::string& reason,
                        IcosphereCacheStats* stats)
{
    subdivisions = std::clamp(subdivisions, 0, kMaxIcosphereSubdivisions);
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) {
        reason = "no cache file";
        return false;
    }
    core::Stopwatch timer;
    std::ifstream in(file, std::ios::binary);
    IcosphereCacheHeader header{};
    if (!in || !readExact(in, &header, sizeof(header))) {
        reason = "unreadable header";
        return false;
    }
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) {
        reason = "not an icosphere cache";
        return false;
    }
    if (header.formatVersion != kIcosphereCacheFormatVersion || header.headerBytes != sizeof(IcosphereCacheHeader)) {
        reason = "format version " + std::to_string(header.formatVersion) + ", expected " +
                 std::to_string(kIcosphereCacheFormatVersion);
        return false;
    }
    if (header.generatorVersion != kIcosphereGeneratorVersion) {
        reason = "generator version " + std::to_string(header.generatorVersion) + ", expected " +
                 std::to_string(kIcosphereGeneratorVersion);
        return false;
    }
    if (header.subdivisions != subdivisions || std::memcmp(&header.radius, &radius, 4) != 0 ||
        header.vertexOrder != static_cast<u32>(order)) {
        reason = "generated with other settings";
        return false;
    }
    if (header.vertexCount != icosphereVertexCount(subdivisions) ||
        header.triangleCount != icosphereTriangleCount(subdivisions)) {
        reason = "unexpected element counts";
        return false;
    }
    const u64 expectedBytes = sizeof(header) + header.vertexCount * sizeof(glm::vec3) + header.triangleCount * 3 * sizeof(u32);
    if (size != expectedBytes) {
        reason = "file size " + std::to_string(size) + ", expected " + std::to_string(expectedBytes);
        return false;
    }
    std::vector<glm::vec3> loadedPositions(static_cast<std::size_t>(header.vertexCount));
    std::vector<u32> loadedIndices(static_cast<std::size_t>(header.triangleCount) * 3);
    if (!readExact(in, loadedPositions.data(), loadedPositions.size() * sizeof(glm::vec3)) ||
        !readExact(in, loadedIndices.data(), loadedIndices.size() * sizeof(u32))) {
        reason = "truncated payload";
        return false;
    }
    if (stats != nullptr) {
        stats->readMs += timer.elapsedMs();
    }
    timer.reset();
    const u64 checksum = icosphereCacheChecksum(loadedPositions, loadedIndices);
    if (stats != nullptr) {
        stats->checksumMs += timer.elapsedMs();
        stats->fileBytes = size;
    }
    if (checksum != header.checksum) {
        reason = "checksum mismatch";
        return false;
    }
    positions = std::move(loadedPositions);
    indices = std::move(loadedIndices);
    reason.clear();
    return true;
}

Mesh makeIcosphereCached(const std::filesystem::path& directory, int subdivisions, float radius, VertexOrder order,
                         const MeshLayout& layout, IcosphereCacheStats* stats)
{
    IcosphereCacheStats local;
    IcosphereCacheStats& s = stats != nullptr ? *stats : local;
    s = {};
    subdivisions = std::clamp(subdivisions, 0, kMaxIcosphereSubdivisions);
    std::vector<glm::vec3> positions;
    std::vector<u32> indices;
    bool loaded = false;
    if (!directory.empty()) {
        s.file = directory / icosphereCacheFileName(subdivisions, radius, order);
        std::string reason;
        loaded = loadIcosphereCache(s.file, subdivisions, radius, order, positions, indices, reason, &s);
        if (!loaded && reason != "no cache file") {
            s.rejected = reason;
        }
    }
    if (loaded) {
        s.source = IcosphereSource::CacheLoaded;
    } else {
        const core::Stopwatch timer;
        generateIcosphereArrays(subdivisions, radius, order, positions, indices);
        s.generateMs = timer.elapsedMs();
        s.source = IcosphereSource::Generated;
        if (!directory.empty()) {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (saveIcosphereCache(s.file, subdivisions, radius, order, positions, indices, &s)) {
                s.source = IcosphereSource::CacheCreated;
            }
        }
    }
    const core::Stopwatch timer;
    Mesh mesh(std::move(positions), std::move(indices), layout);
    s.buildMs = timer.elapsedMs();
    return mesh;
}

} // namespace se::geo
