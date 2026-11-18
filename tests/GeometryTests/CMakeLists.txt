se_add_test(se_geometry_tests
    BvhTests.cpp
    ChunkLodTests.cpp
    ChunkedBvhTests.cpp
    CompactAdjacencyTests.cpp
    CoreTests.cpp
    DerivedNeighborsTests.cpp
    EditRegionTests.cpp
    IcosphereTests.cpp
    IntersectTests.cpp
    MeshTests.cpp
    NormalConeTests.cpp
    PagedMeshTests.cpp
    ResidencyTests.cpp
    SpatialOrderTests.cpp)
target_link_libraries(se_geometry_tests PRIVATE se_geometry)
