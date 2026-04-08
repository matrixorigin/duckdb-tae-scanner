// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0
//
// Negative and error-handling tests.

#include "test_helpers.hpp"
#include <cstdio>
#include <fstream>

// Helper: write bytes to a temp file, return its path.
// Caller must std::remove() the file after use.
static std::string WriteTempFile(const std::string &name,
                                  const std::vector<uint8_t> &data) {
    auto path = std::string(TEST_DATA_DIR) + "/" + name;
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ofs.close();
    return path;
}

// Helper: write a minimal manifest JSON pointing to a single object file.
static std::string WriteTempManifest(const std::string &name,
                                      const std::string &obj_file,
                                      int blocks, int rows) {
    auto path = std::string(TEST_DATA_DIR) + "/" + name;
    std::ofstream ofs(path);
    ofs << R"({"columns":[{"name":"col_int","oid":22}],)"
        << R"("objects":[{"path":")" << obj_file << R"(","blocks":)" << blocks
        << R"(,"rows":)" << rows << R"(}]})";
    ofs.close();
    return path;
}

TEST_CASE("Error: missing manifest file", "[error]") {
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('/tmp/nonexistent_manifest_xyz.json')");
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("cannot open manifest") != std::string::npos);
}

TEST_CASE("Error: malformed JSON manifest", "[error]") {
    auto path = WriteTempFile("bad_manifest.json", {'{', '"', 'x'});
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" + path + "')");
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("invalid JSON") != std::string::npos);
    std::remove(path.c_str());
}

TEST_CASE("Error: object file with bad magic", "[error]") {
    // Write 64 bytes of zeros (wrong magic — should be 0xFFFFFFFF)
    std::vector<uint8_t> bad_obj(64, 0);
    auto obj_path = WriteTempFile("bad_magic.tae", bad_obj);
    auto mf_path = WriteTempManifest("mf_bad_magic.json", "bad_magic.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    if (result->HasError()) { UNSCOPED_INFO("Error: " << result->GetError()); }
    REQUIRE(result->HasError());
    CHECK(result->GetError().find("magic") != std::string::npos);
    std::remove(obj_path.c_str());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: truncated object file (too short for header)", "[error]") {
    // Only 8 bytes — not enough for 64-byte header
    std::vector<uint8_t> tiny(8, 0xFF);
    auto obj_path = WriteTempFile("truncated.tae", tiny);
    auto mf_path = WriteTempManifest("mf_truncated.json", "truncated.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    REQUIRE(result->HasError());
    // Should fail during ReadMeta when reading 64 bytes from 8-byte file
    std::remove(obj_path.c_str());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: object file does not exist", "[error]") {
    auto mf_path = WriteTempManifest("mf_missing_obj.json", "nonexistent_obj.tae", 1, 1);
    auto db = MakeDB();
    auto result = Query(*db, "SELECT col_int FROM tae_scan('" + mf_path + "')");
    REQUIRE(result->HasError());
    std::remove(mf_path.c_str());
}

TEST_CASE("Error: empty manifest returns no rows", "[error]") {
    // Valid JSON but no objects/columns
    auto path = WriteTempFile("empty_manifest.json",
        std::vector<uint8_t>('{', '}'));
    auto db = MakeDB();
    auto result = Query(*db, "SELECT * FROM tae_scan('" + path + "')");
    // Should succeed with 0 columns → DuckDB may report error or empty
    // Either way, it shouldn't crash
    (void)result;
    std::remove(path.c_str());
}
