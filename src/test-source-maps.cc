/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gtest/gtest.h"
#include "source-maps.h"

// Use a macro instead of a function to get meaningful line numbers on failure.
#define EXPECT_SEGMENT_EQ(lhs, rhs)                              \
  do {                                                           \
    EXPECT_EQ(lhs.generated_col, rhs.generated_col);             \
    EXPECT_EQ(lhs.generated_col_delta, rhs.generated_col_delta); \
    EXPECT_EQ(lhs.has_source, rhs.has_source);                   \
    EXPECT_EQ(lhs.source, rhs.source);                           \
    EXPECT_EQ(lhs.source_line, rhs.source_line);                 \
    EXPECT_EQ(lhs.source_line_delta, rhs.source_line_delta);     \
    EXPECT_EQ(lhs.source_col, rhs.source_col);                   \
    EXPECT_EQ(lhs.source_col_delta, rhs.source_col_delta);       \
    EXPECT_EQ(lhs.has_name, rhs.has_name);                       \
    EXPECT_EQ(lhs.name, rhs.name);                               \
  } while (0)

TEST(source_mappings, comparisons) {
  SourceMapGenerator::SourceMapping a = {{1, 1}, {1, 1}, 0};
  SourceMapGenerator::SourceMapping b = {{1, 1}, {1, 1}, 0};
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a < b);
  EXPECT_FALSE(b < a);
  b = {{1, 1}, {2, 1}, 0};
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  b = {{1, 1}, {1, 2}, 0};
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  b = {{1, 0}, {1, 2}, 0};
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  b = {{0, 0}, {1, 2}, 0};
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  b = {{1, 2}, {1, 0}, 0};
  EXPECT_FALSE(a == b);
  EXPECT_FALSE(a < b);
  EXPECT_TRUE(b < a);

  b = {{1, 1}, {1, 1}, 1};
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
}

TEST(source_maps, constructor) { SourceMapGenerator("file", "source-root"); }

TEST(source_maps, sources) {
  SourceMapGenerator smg("source.out", "source-root");
  smg.AddMapping({2, 3}, {1, 1}, "asdf1");
  smg.AddMapping({2, 2}, {1, 1}, "");
  smg.AddMapping({2, 3}, {1, 1}, "asdf2");
  const auto& map = smg.GetMap();
  EXPECT_EQ("source.out", map.file);
  EXPECT_EQ("source-root", map.source_root);
  ASSERT_EQ(3UL, map.sources.size());
  EXPECT_EQ("asdf1", map.sources[0]);
  EXPECT_EQ("", map.sources[1]);
  EXPECT_EQ("asdf2", map.sources[2]);
  SourceMapGenerator smg2("", "");
  const auto& map2 = smg2.GetMap();
  EXPECT_EQ("", map2.file);
  EXPECT_EQ("", map2.source_root);
  EXPECT_EQ(0UL, map2.sources.size());
}

TEST(source_maps, zero_mappings) {
  SourceMapGenerator smg("", "");
  smg.AddMapping({1, 0}, {1, 0}, "");
  const auto& map = smg.GetMap();
  ASSERT_EQ(1UL, map.segment_groups.size());
  EXPECT_EQ(1U, map.segment_groups.back().generated_line);
  ASSERT_EQ(1UL, map.segment_groups.back().segments.size());
  const auto& seg = map.segment_groups.back().segments.back();
  SourceMap::Segment s = {{0, 0}, {true, 0}, {1, 1}, {0, 0}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, seg);
}

TEST(source_maps, invalid_mappings) {
  SourceMapGenerator smg("", "");
  // For now gen, orig, and source are all required.
  EXPECT_FALSE(smg.AddMapping({0, 1}, {1, 1}, ""));
  EXPECT_FALSE(smg.AddMapping({1, 1}, {0, 1}, ""));
  EXPECT_TRUE(smg.AddMapping({1, 0}, {1, 1}, ""));
  EXPECT_TRUE(smg.AddMapping({1, 1}, {1, 0}, ""));
}

TEST(source_maps, incremental_mappings) {
  // Check cases where there is no delta; i.e. the first instances of fields.
  SourceMapGenerator smg("", "");
  smg.AddMapping({4, 7}, {5, 1}, "asdf");
  auto& map = smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(1UL, map.segment_groups.back().segments.size());
  const auto& seg = map.segment_groups.back().segments.back();
  SourceMap::Segment s = {{7, 7}, {true, 0}, {5, 5}, {1, 1}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, seg);

  // Duplicate mapping (no new segment)
  smg.AddMapping({4, 7}, {5, 1}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(1UL, map.segment_groups.back().segments.size());

  // New generated column, same line, same source
  smg.AddMapping({4, 8}, {5, 1}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(2UL, map.segment_groups.back().segments.size());
  // s = {{8, 1}, {true, 0}, {5, 1}, {1, 0}, {false, 0}};
  // Not sure which is more readable; pass a whole new segment on one line or
  // update by field name?
  s.generated_col = 8;
  s.generated_col_delta = 1;
  s.source_line_delta = 0;
  s.source_col_delta = 0;
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // New generated column, same line, new source col
  smg.AddMapping({4, 9}, {5, 2}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(3UL, map.segment_groups.back().segments.size());
  s = {{9, 1}, {true, 0}, {5, 0}, {2, 1}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // New generated column, same line, new source line, negative source col delta
  smg.AddMapping({4, 10}, {6, 0}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(4UL, map.segment_groups.back().segments.size());
  s = {{10, 1}, {true, 0}, {6, 1}, {0, -2}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // Same generated line and col, different source.
  // The JS sourcemapper allows and encodes this
  // (I guess it overrides the previous mapping?)
  smg.AddMapping({4, 10}, {7, 10}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(5UL, map.segment_groups.back().segments.size());
  s = {{10, 0}, {true, 0}, {7, 1}, {10, 10}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // New generated col, negative source col delta
  smg.AddMapping({4, 11}, {7, 9}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(4UL, map.segment_groups.size());
  EXPECT_EQ(4U, map.segment_groups.back().generated_line);
  ASSERT_EQ(6UL, map.segment_groups.back().segments.size());
  s = {{11, 1}, {true, 0}, {7, 0}, {9, -1}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // New generated line (new segment, leave 1 hole)
  smg.AddMapping({6, 1}, {8, 0}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(6UL, map.segment_groups.size());
  // Empty segment at line 5
  EXPECT_EQ(5U, map.segment_groups[4].generated_line);
  ASSERT_EQ(0UL, map.segment_groups[4].segments.size());
  // Populated segment at line 6
  EXPECT_EQ(6U, map.segment_groups.back().generated_line);
  ASSERT_EQ(1UL, map.segment_groups.back().segments.size());
  // Generated col delta is 1 because it's a new line
  s = {{1, 1}, {true, 0}, {8, 1}, {0, -9}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());

  // New generated line inserted into the hole
  smg.AddMapping({5, 1}, {8, 0}, "asdf");
  smg.GetMap();
  ASSERT_TRUE(map.Validate(true));
  ASSERT_EQ(6UL, map.segment_groups.size());
  EXPECT_EQ(5U, map.segment_groups[4].generated_line);
  ASSERT_EQ(1UL, map.segment_groups[4].segments.size());
  // Inserted segment. Generated col delta is 0
  s = {{1, 1}, {true, 0}, {8, 1}, {0, -9}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups[4].segments.back());
  // Following segment
  s = {{1, 1}, {true, 0}, {8, 0}, {0, 0}, {false, 0}};
  EXPECT_SEGMENT_EQ(s, map.segment_groups.back().segments.back());
}

#define EXPECT_JSON_CONTAINS_STR(output, key, value)       \
  EXPECT_TRUE(output.find(std::string("\"") + key + "\" : \"" + value + "\"") != std::string::npos)
#define EXPECT_JSON_CONTAINS_NUM(output, key, value)       \
  EXPECT_TRUE(output.find(std::string("\"") + key + "\" : " + value) != std::string::npos)


TEST(source_maps, serialization_empty) {
  SourceMapGenerator smg("source.out", "source-root");
  std::string output = smg.SerializeMappings();
  EXPECT_JSON_CONTAINS_NUM(output, "version", "3");
  EXPECT_JSON_CONTAINS_STR(output, "file", "source.out");
  EXPECT_JSON_CONTAINS_STR(output, "sourceRoot", "source-root");
  EXPECT_JSON_CONTAINS_NUM(output, "sources", "[]"); // TODO: fix this abuse of NUM
}
