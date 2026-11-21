// The pristine-sphere cache (IcosphereCache.h): a loaded sphere must be bit
// identical to a generated one (positions, triangle indices, their order, and
// the Mesh built from them), and a file that does not match the request or the
// current versions, or is damaged, must be rejected with a reason.

#include "Geometry/IcosphereCache.h"
#include "TestUtils.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace se;

namespace {

namespace fs = std::filesystem;

class IcosphereCacheTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        directory = fs::temp_directory_path() /
                    ("se_icosphere_cache_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(directory);
    }
    void TearDown() override { fs::remove_all(directory); }

    fs::path directory;
};

std::vector<glm::vec3> positionsOf(const geo::Mesh& mesh)
{
    std::vector<glm::vec3> out(mesh.vertexCount());
    mesh.copyPositions(0, mesh.vertexCount(), out.data());
    return out;
}

std::vector<u32> indicesOf(const geo::Mesh& mesh)
{
    std::vector<u32> out(static_cast<std::size_t>(mesh.triangleCount()) * 3);
    mesh.copyIndices(0, mesh.triangleCount(), out.data());
    return out;
}

std::vector<glm::vec3> normalsOf(const geo::Mesh& mesh)
{
    return {mesh.normals().begin(), mesh.normals().end()};
}

void patchFile(const fs::path& file, std::size_t offset, const void* bytes, std::size_t size)
{
    std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(static_cast<std::streamoff>(offset));
    io.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
}

} // namespace

TEST_F(IcosphereCacheTest, CreatesThenLoadsABitIdenticalSphere)
{
    const geo::Mesh generated = geo::makeIcosphere(4, 1.0f, geo::VertexOrder::Spatial);

    geo::IcosphereCacheStats first;
    const geo::Mesh created = geo::makeIcosphereCached(directory, 4, 1.0f, geo::VertexOrder::Spatial, {}, &first);
    EXPECT_EQ(first.source, geo::IcosphereSource::CacheCreated);
    EXPECT_TRUE(first.rejected.empty());
    ASSERT_TRUE(fs::exists(first.file));
    EXPECT_EQ(fs::file_size(first.file), sizeof(geo::IcosphereCacheHeader) + generated.vertexCount() * sizeof(glm::vec3) +
                                             static_cast<std::size_t>(generated.triangleCount()) * 3 * sizeof(u32));
    EXPECT_EQ(first.fileBytes, fs::file_size(first.file));

    geo::IcosphereCacheStats second;
    const geo::Mesh loaded = geo::makeIcosphereCached(directory, 4, 1.0f, geo::VertexOrder::Spatial, {}, &second);
    EXPECT_EQ(second.source, geo::IcosphereSource::CacheLoaded);
    EXPECT_EQ(second.file, first.file);

    for (const geo::Mesh* mesh : {&created, &loaded}) {
        ASSERT_EQ(mesh->vertexCount(), generated.vertexCount());
        ASSERT_EQ(mesh->triangleCount(), generated.triangleCount());
        EXPECT_TRUE(test::bitwiseEqual(positionsOf(*mesh), positionsOf(generated)));
        EXPECT_EQ(indicesOf(*mesh), indicesOf(generated));
        EXPECT_TRUE(test::bitwiseEqual(normalsOf(*mesh), normalsOf(generated)));
    }
}

TEST_F(IcosphereCacheTest, NoDirectoryGeneratesWithoutWriting)
{
    geo::IcosphereCacheStats stats;
    const geo::Mesh mesh = geo::makeIcosphereCached({}, 3, 1.0f, geo::VertexOrder::Spatial, {}, &stats);
    EXPECT_EQ(stats.source, geo::IcosphereSource::Generated);
    EXPECT_TRUE(stats.file.empty());
    EXPECT_EQ(indicesOf(mesh), indicesOf(geo::makeIcosphere(3)));
}

TEST_F(IcosphereCacheTest, FileNameNamesTheSettingsAndVersions)
{
    const std::string name = geo::icosphereCacheFileName(11, 1.0f, geo::VertexOrder::Spatial);
    EXPECT_EQ(name, "icosphere_L11_r1_spatial_f" + std::to_string(geo::kIcosphereCacheFormatVersion) + "_g" +
                        std::to_string(geo::kIcosphereGeneratorVersion) + ".bin");
    EXPECT_NE(geo::icosphereCacheFileName(11, 0.5f, geo::VertexOrder::Spatial), name);
    EXPECT_NE(geo::icosphereCacheFileName(11, 1.0f, geo::VertexOrder::Generation), name);
}

TEST_F(IcosphereCacheTest, RejectsIncompatibleOrDamagedFiles)
{
    geo::IcosphereCacheStats created;
    (void)geo::makeIcosphereCached(directory, 3, 1.0f, geo::VertexOrder::Spatial, {}, &created);
    const fs::path file = created.file;
    const fs::path pristine = directory / "pristine.bin";
    fs::copy_file(file, pristine);

    const auto expectRejected = [&](const char* what, auto damage, const char* reasonPart) {
        SCOPED_TRACE(what);
        fs::remove(file); // (MinGW's copy_file does not overwrite reliably)
        fs::copy_file(pristine, file);
        damage();
        std::vector<glm::vec3> positions;
        std::vector<u32> indices;
        std::string reason;
        EXPECT_FALSE(geo::loadIcosphereCache(file, 3, 1.0f, geo::VertexOrder::Spatial, positions, indices, reason));
        EXPECT_NE(reason.find(reasonPart), std::string::npos) << reason;
        EXPECT_TRUE(positions.empty());
        // makeIcosphereCached regenerates and rewrites it, with the reason.
        geo::IcosphereCacheStats stats;
        const geo::Mesh mesh = geo::makeIcosphereCached(directory, 3, 1.0f, geo::VertexOrder::Spatial, {}, &stats);
        EXPECT_EQ(stats.source, geo::IcosphereSource::CacheCreated);
        EXPECT_NE(stats.rejected.find(reasonPart), std::string::npos) << stats.rejected;
        EXPECT_EQ(indicesOf(mesh), indicesOf(geo::makeIcosphere(3)));
        EXPECT_EQ(fs::file_size(file), fs::file_size(pristine));
    };

    expectRejected("magic", [&] { patchFile(file, 0, "XXXXXXXX", 8); }, "not an icosphere cache");
    expectRejected("format version", [&] {
        const u32 version = geo::kIcosphereCacheFormatVersion + 1;
        patchFile(file, offsetof(geo::IcosphereCacheHeader, formatVersion), &version, 4);
    }, "format version");
    expectRejected("generator version", [&] {
        const u32 version = geo::kIcosphereGeneratorVersion + 1;
        patchFile(file, offsetof(geo::IcosphereCacheHeader, generatorVersion), &version, 4);
    }, "generator version");
    expectRejected("settings", [&] {
        const float radius = 2.0f;
        patchFile(file, offsetof(geo::IcosphereCacheHeader, radius), &radius, 4);
    }, "other settings");
    expectRejected("truncated", [&] { fs::resize_file(file, fs::file_size(pristine) - 12); }, "file size");
    expectRejected("payload", [&] {
        const u32 garbage = 0xDEADBEEFu;
        patchFile(file, sizeof(geo::IcosphereCacheHeader) + 100, &garbage, 4);
    }, "checksum");
}
