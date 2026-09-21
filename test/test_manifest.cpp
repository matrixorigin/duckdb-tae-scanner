// Copyright 2026 Matrix Origin
// SPDX-License-Identifier: Apache-2.0

#include "catch.hpp"
#include "tae_scanner.hpp"

#include <string>

namespace {
std::string Manifest(const std::string &extra = "", const std::string &path = "obj/0001") {
    return R"({"database":"db","table":"t","columns":[{"name":"amount","oid":32,"width":12,"scale":2,"seqnum":7}],"objects":[{"path":")" +
           path + R"(","rows":8192,"blocks":1,"size":4096}])" + extra + "}";
}
} // namespace

TEST_CASE("Manifest bytes preserve metadata with and without a sort column", "[manifest]") {
    for (const auto &extra : {std::string(), std::string(R"(,"sort_column":"amount")")}) {
        CAPTURE(extra);
        tae::TAEScanBindData bind;
        // The root and object need not exist: parsing must not perform I/O.
        tae::ParseManifestBytes(Manifest(extra), "/nonexistent/tae-manifest-test", bind);
        CHECK(bind.db_name == "db");
        CHECK(bind.table_name == "t");
        CHECK(bind.data_dir == "/nonexistent/tae-manifest-test");
        REQUIRE(bind.all_col_names.size() == 1);
        CHECK(bind.all_col_names[0] == "amount");
        CHECK(bind.all_col_widths[0] == 12);
        CHECK(bind.all_col_scales[0] == 2);
        CHECK(bind.all_col_seqnums[0] == 7);
        CHECK(bind.sort_column_idx == (extra.empty() ? -1 : 0));
        CHECK(bind.total_rows == 8192);
        CHECK(bind.total_blocks == 1);
        auto copied = bind.Copy();
        auto &copy = copied->Cast<tae::TAEScanBindData>();
        CHECK(copy.data_dir == bind.data_dir);
        CHECK(copy.all_col_types == bind.all_col_types);
        CHECK(copy.all_col_widths == bind.all_col_widths);
        CHECK(copy.all_col_scales == bind.all_col_scales);
        CHECK(copy.all_col_seqnums == bind.all_col_seqnums);
        REQUIRE(copy.objects.size() == 1);
        CHECK(copy.objects[0].file_path == "obj/0001");
        CHECK(copy.sort_column_idx == bind.sort_column_idx);
    }
}

TEST_CASE("Standalone manifest identity is optional but malformed identity is rejected",
          "[manifest]") {
    const std::string legacy = R"({"columns":[{"name":"c","oid":22}],"objects":[]})";
    tae::TAEScanBindData bind;
    REQUIRE_NOTHROW(tae::ParseManifestBytes(legacy, "", bind));
    CHECK(bind.db_name.empty());
    CHECK(bind.table_name.empty());
    CHECK(bind.all_col_seqnums[0] == 0);
    tae::TAEScanBindData strict;
    REQUIRE_THROWS(tae::ParseManifestBytes(legacy, "/registered", strict));
    for (const auto &identity : {R"("database":null,)", R"("table":42,)"}) {
        tae::TAEScanBindData invalid;
        REQUIRE_THROWS(
            tae::ParseManifestBytes("{" + std::string(identity) + legacy.substr(1), "", invalid));
    }
}

TEST_CASE("Authoritative manifest root confines paths without changing standalone resolution",
          "[manifest]") {
    for (const auto &path :
         {"/absolute/object", "../object", "a/../object", "s3://bucket/object", "a\\\\object"}) {
        CAPTURE(path);
        tae::TAEScanBindData strict;
        REQUIRE_THROWS(tae::ParseManifestBytes(Manifest("", path), "/registered", strict));
        tae::TAEScanBindData legacy;
        REQUIRE_NOTHROW(tae::ParseManifestBytes(Manifest("", path), "", legacy));
    }
    tae::TAEScanBindData mismatch;
    REQUIRE_THROWS(
        tae::ParseManifestBytes(Manifest(R"(,"data_dir":"/different")"), "/registered", mismatch));
    tae::TAEScanBindData matching;
    REQUIRE_NOTHROW(
        tae::ParseManifestBytes(Manifest(R"(,"data_dir":"/registered")"), "/registered", matching));
}

TEST_CASE("Manifest parser rejects malformed structure and oversized bytes", "[manifest]") {
    for (const auto &json : {"", "{", "[]", R"({"columns":null,"objects":[]})"}) {
        tae::TAEScanBindData bind;
        REQUIRE_THROWS(tae::ParseManifestBytes(json, "", bind));
    }
    tae::TAEScanBindData bind;
    REQUIRE_THROWS(tae::ParseManifestBytes(std::string((64u << 20) + 1, ' '), "", bind));
}
