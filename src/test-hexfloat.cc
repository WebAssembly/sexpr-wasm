/*
 * Copyright 2016 WebAssembly Community Group participants
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

#include <array>
#include <cstdio>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "wabt/literal.h"

#define FOREACH_UINT32_MULTIPLIER 1

#define FOREACH_UINT32(bits) \
  uint32_t last_bits = 0;    \
  uint32_t bits = shard;     \
  int last_top_byte = -1;    \
  for (; bits >= last_bits;  \
       last_bits = bits, bits += num_threads_ * FOREACH_UINT32_MULTIPLIER)

#define LOG_COMPLETION(bits)                                                  \
  if (shard == 0) {                                                           \
    int top_byte = bits >> 24;                                                \
    if (top_byte != last_top_byte) {                                          \
      printf("value: 0x%08x (%d%%)\r", bits,                                  \
             static_cast<int>(static_cast<double>(bits) * 100 / UINT32_MAX)); \
      fflush(stdout);                                                         \
      last_top_byte = top_byte;                                               \
    }                                                                         \
  }

#define LOG_DONE()     \
  if (shard == 0) {    \
    printf("done.\n"); \
    fflush(stdout);    \
  }

using namespace wabt;

template <typename T, typename F>
T bit_cast(F value) {
  T result;
  memcpy(&result, &value, sizeof(result));
  return result;
}

static bool is_infinity_or_nan(uint32_t float_bits) {
  return ((float_bits >> 23) & 0xff) == 0xff;
}

static bool is_infinity_or_nan(uint64_t double_bits) {
  return ((double_bits >> 52) & 0x7ff) == 0x7ff;
}

class ThreadedTest : public ::testing::Test {
 protected:
  static constexpr int kDefaultNumThreads = 2;

  virtual void SetUp() {
    num_threads_ = std::thread::hardware_concurrency();
    if (num_threads_ == 0)
      num_threads_ = kDefaultNumThreads;
  }

  virtual void RunShard(int shard) = 0;

  void RunThreads() {
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads_; ++i) {
      threads.emplace_back(&ThreadedTest::RunShard, this, i);
    }

    for (std::thread& thread : threads) {
      thread.join();
    }
  }

  int num_threads_;
};

/* floats */
class AllFloatsParseTest : public ThreadedTest {
 protected:
  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(bits) {
      LOG_COMPLETION(bits);
      if (is_infinity_or_nan(bits))
        continue;

      float value = bit_cast<float>(bits);
      int len = snprintf(buffer, sizeof(buffer), "%a", value);

      uint32_t me;
      ASSERT_EQ(Result::Ok,
                ParseFloat(LiteralType::Hexfloat, buffer, buffer + len, &me));
      ASSERT_EQ(me, bits);
    }
    LOG_DONE();
  }
};

TEST_F(AllFloatsParseTest, Run) {
  RunThreads();
}

class AllFloatsWriteTest : public ThreadedTest {
 protected:
  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(bits) {
      LOG_COMPLETION(bits);
      if (is_infinity_or_nan(bits))
        continue;

      WriteFloatHex(buffer, sizeof(buffer), bits);

      char* endptr;
      float them_float = strtof(buffer, &endptr);
      uint32_t them_bits = bit_cast<uint32_t>(them_float);
      ASSERT_EQ(bits, them_bits);
    }
    LOG_DONE();
  }
};

TEST_F(AllFloatsWriteTest, Run) {
  RunThreads();
}

class AllFloatsRoundtripTest : public ThreadedTest {
 protected:
  static LiteralType ClassifyFloat(uint32_t float_bits) {
    if (is_infinity_or_nan(float_bits)) {
      if (float_bits & 0x7fffff) {
        return LiteralType::Nan;
      } else {
        return LiteralType::Infinity;
      }
    } else {
      return LiteralType::Hexfloat;
    }
  }

  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(bits) {
      LOG_COMPLETION(bits);
      WriteFloatHex(buffer, sizeof(buffer), bits);
      int len = strlen(buffer);

      uint32_t new_bits;
      ASSERT_EQ(Result::Ok, ParseFloat(ClassifyFloat(bits), buffer,
                                       buffer + len, &new_bits));
      ASSERT_EQ(new_bits, bits);
    }
    LOG_DONE();
  }
};

TEST_F(AllFloatsRoundtripTest, Run) {
  RunThreads();
}

/* doubles */
class ManyDoublesParseTest : public ThreadedTest {
 protected:
  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(halfbits) {
      LOG_COMPLETION(halfbits);
      uint64_t bits = (static_cast<uint64_t>(halfbits) << 32) | halfbits;
      if (is_infinity_or_nan(bits))
        continue;

      double value = bit_cast<double>(bits);
      int len = snprintf(buffer, sizeof(buffer), "%a", value);

      uint64_t me;
      ASSERT_EQ(Result::Ok,
                ParseDouble(LiteralType::Hexfloat, buffer, buffer + len, &me));
      ASSERT_EQ(me, bits);
    }
    LOG_DONE();
  }
};

TEST_F(ManyDoublesParseTest, Run) {
  RunThreads();
}

class ManyDoublesWriteTest : public ThreadedTest {
 protected:
  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(halfbits) {
      LOG_COMPLETION(halfbits);
      uint64_t bits = (static_cast<uint64_t>(halfbits) << 32) | halfbits;
      if (is_infinity_or_nan(bits))
        continue;

      WriteDoubleHex(buffer, sizeof(buffer), bits);

      char* endptr;
      double them_double = strtod(buffer, &endptr);
      uint64_t them_bits = bit_cast<uint64_t>(them_double);
      ASSERT_EQ(bits, them_bits);
    }
    LOG_DONE();
  }
};

TEST_F(ManyDoublesWriteTest, Run) {
  RunThreads();
}

class ManyDoublesRoundtripTest : public ThreadedTest {
 protected:
  static LiteralType ClassifyDouble(uint64_t double_bits) {
    if (is_infinity_or_nan(double_bits)) {
      if (double_bits & 0xfffffffffffffULL) {
        return LiteralType::Nan;
      } else {
        return LiteralType::Infinity;
      }
    } else {
      return LiteralType::Hexfloat;
    }
  }

  virtual void RunShard(int shard) {
    char buffer[100];
    FOREACH_UINT32(halfbits) {
      LOG_COMPLETION(halfbits);
      uint64_t bits = (static_cast<uint64_t>(halfbits) << 32) | halfbits;
      WriteDoubleHex(buffer, sizeof(buffer), bits);
      int len = strlen(buffer);

      uint64_t new_bits;
      ASSERT_EQ(Result::Ok, ParseDouble(ClassifyDouble(bits), buffer,
                                        buffer + len, &new_bits));
      ASSERT_EQ(new_bits, bits);
    }
    LOG_DONE();
  }
};

TEST_F(ManyDoublesRoundtripTest, Run) {
  RunThreads();
}

class SpecificFloatsTest : public ::testing::Test {
 protected:
  uint32_t ConstructUint32(const std::array<uint8_t, 4>& bytes) {
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      result |= static_cast<uint32_t>(bytes[i]) << (i * 8);
    }
    return result;
  }

  uint64_t ConstructUint64(const std::array<uint8_t, 8>& bytes) {
    uint64_t result = 0;
    for (int i = 0; i < 8; ++i) {
      result |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return result;
  }

  void TestFloat(const std::array<uint8_t, 4>& bytes,
                 const std::string& expected_output) {
    uint32_t float_bits = ConstructUint32(bytes);
    char buffer[100];
    WriteFloatHex(buffer, sizeof(buffer), float_bits);
    ASSERT_STREQ(buffer, expected_output.c_str());
    uint32_t parsed_bits;
    ASSERT_EQ(Result::Ok, ParseFloat(LiteralType::Hexfloat, buffer,
                                     buffer + strlen(buffer), &parsed_bits));
    ASSERT_EQ(parsed_bits, float_bits);
  }

  void TestDouble(const std::array<uint8_t, 8>& bytes,
                  const std::string& expected_output) {
    uint64_t float_bits = ConstructUint64(bytes);
    char buffer[100];
    WriteDoubleHex(buffer, sizeof(buffer), float_bits);
    ASSERT_STREQ(buffer, expected_output.c_str());
    uint64_t parsed_bits;
    ASSERT_EQ(Result::Ok, ParseDouble(LiteralType::Hexfloat, buffer,
                                      buffer + strlen(buffer), &parsed_bits));
    ASSERT_EQ(parsed_bits, float_bits);
  }
};

TEST_F(SpecificFloatsTest, Run) {
  TestFloat({0x00, 0x00, 0x00, 0x80}, "-0x0p+0");
  TestFloat({0x00, 0x00, 0x00, 0x00}, "0x0p+0");
  TestFloat({0x01, 0x00, 0x80, 0xd8}, "-0x1.000002p+50");
  TestFloat({0x01, 0x00, 0x80, 0xa6}, "-0x1.000002p-50");
  TestFloat({0x01, 0x00, 0x80, 0x58}, "0x1.000002p+50");
  TestFloat({0x01, 0x00, 0x80, 0x26}, "0x1.000002p-50");
  TestFloat({0x01, 0x00, 0x00, 0x7f}, "0x1.000002p+127");
  TestFloat({0x02, 0x00, 0x80, 0xd8}, "-0x1.000004p+50");
  TestFloat({0x02, 0x00, 0x80, 0xa6}, "-0x1.000004p-50");
  TestFloat({0x02, 0x00, 0x80, 0x58}, "0x1.000004p+50");
  TestFloat({0x02, 0x00, 0x80, 0x26}, "0x1.000004p-50");
  TestFloat({0x03, 0x00, 0x80, 0xd8}, "-0x1.000006p+50");
  TestFloat({0x03, 0x00, 0x80, 0xa6}, "-0x1.000006p-50");
  TestFloat({0x03, 0x00, 0x80, 0x58}, "0x1.000006p+50");
  TestFloat({0x03, 0x00, 0x80, 0x26}, "0x1.000006p-50");
  TestFloat({0xb4, 0xa2, 0x11, 0x52}, "0x1.234568p+37");
  TestFloat({0xb4, 0xa2, 0x91, 0x5b}, "0x1.234568p+56");
  TestFloat({0xb4, 0xa2, 0x11, 0x65}, "0x1.234568p+75");
  TestFloat({0x99, 0x76, 0x96, 0xfe}, "-0x1.2ced32p+126");
  TestFloat({0x99, 0x76, 0x96, 0x7e}, "0x1.2ced32p+126");
  TestFloat({0x03, 0x00, 0x00, 0x80}, "-0x1.8p-148");
  TestFloat({0x03, 0x00, 0x00, 0x00}, "0x1.8p-148");
  TestFloat({0xff, 0x2f, 0x59, 0x2d}, "0x1.b25ffep-37");
  TestFloat({0xa3, 0x79, 0xeb, 0x4c}, "0x1.d6f346p+26");
  TestFloat({0x7b, 0x4d, 0x7f, 0x6c}, "0x1.fe9af6p+89");
  TestFloat({0x00, 0x00, 0x00, 0xff}, "-0x1p+127");
  TestFloat({0x00, 0x00, 0x00, 0x7f}, "0x1p+127");
  TestFloat({0x02, 0x00, 0x00, 0x80}, "-0x1p-148");
  TestFloat({0x02, 0x00, 0x00, 0x00}, "0x1p-148");
  TestFloat({0x01, 0x00, 0x00, 0x80}, "-0x1p-149");
  TestFloat({0x01, 0x00, 0x00, 0x00}, "0x1p-149");
  TestFloat({0x00, 0x00, 0x80, 0xd8}, "-0x1p+50");
  TestFloat({0x00, 0x00, 0x80, 0xa6}, "-0x1p-50");
  TestFloat({0x00, 0x00, 0x80, 0x58}, "0x1p+50");
  TestFloat({0x00, 0x00, 0x80, 0x26}, "0x1p-50");
  TestFloat({0x00, 0x00, 0x80, 0x3f}, "0x1p+0");
  TestFloat({0x00, 0x00, 0x80, 0xbf}, "-0x1p+0");
  TestFloat({0xff, 0xff, 0x7f, 0x7f}, "0x1.fffffep+127");
  TestFloat({0xff, 0xff, 0x7f, 0xff}, "-0x1.fffffep+127");
  TestFloat({0x00, 0x00, 0x80, 0x4b}, "0x1p+24");
  TestFloat({0x00, 0x00, 0x80, 0xcb}, "-0x1p+24");
  TestFloat({0xa4, 0x70, 0x9d, 0x3f}, "0x1.3ae148p+0");

  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, "-0x0p+0");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "0x0p+0");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xc3},
             "-0x1.0000000000001p+60");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x43},
             "0x1.0000000000001p+60");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe5},
             "-0x1.0000000000001p+600");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x9a},
             "-0x1.0000000000001p-600");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x65},
             "0x1.0000000000001p+600");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x1a},
             "0x1.0000000000001p-600");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6},
             "-0x1.0000000000001p+97");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46},
             "0x1.0000000000001p+97");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xfe},
             "-0x1.0000000000001p+999");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x7e},
             "0x1.0000000000001p+999");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xc3},
             "-0x1.0000000000002p+60");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x43},
             "0x1.0000000000002p+60");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe5},
             "-0x1.0000000000002p+600");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x9a},
             "-0x1.0000000000002p-600");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x65},
             "0x1.0000000000002p+600");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x1a},
             "0x1.0000000000002p-600");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6},
             "-0x1.0000000000002p+97");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46},
             "0x1.0000000000002p+97");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xfe},
             "-0x1.0000000000002p+999");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x7e},
             "0x1.0000000000002p+999");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x80},
             "-0x1.0000000000003p-1022");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00},
             "0x1.0000000000003p-1022");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe5},
             "-0x1.0000000000003p+600");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x9a},
             "-0x1.0000000000003p-600");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x65},
             "0x1.0000000000003p+600");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x1a},
             "0x1.0000000000003p-600");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6},
             "-0x1.0000000000003p+97");
  TestDouble({0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46},
             "0x1.0000000000003p+97");
  TestDouble({0xa0, 0xc8, 0xeb, 0x85, 0xf3, 0xcc, 0xe1, 0xff},
             "-0x1.1ccf385ebc8ap+1023");
  TestDouble({0xa0, 0xc8, 0xeb, 0x85, 0xf3, 0xcc, 0xe1, 0x7f},
             "0x1.1ccf385ebc8ap+1023");
  TestDouble({0xdf, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0xc2, 0x43},
             "0x1.23456789abcdfp+61");
  TestDouble({0xdf, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0xf2, 0x44},
             "0x1.23456789abcdfp+80");
  TestDouble({0xdf, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x22, 0x46},
             "0x1.23456789abcdfp+99");
  TestDouble({0x11, 0x43, 0x2b, 0xd6, 0xff, 0x25, 0xab, 0x3d},
             "0x1.b25ffd62b4311p-37");
  TestDouble({0x12, 0xec, 0x36, 0xd6, 0xff, 0x25, 0xab, 0x3d},
             "0x1.b25ffd636ec12p-37");
  TestDouble({0x58, 0xa4, 0x0c, 0x54, 0x34, 0x6f, 0x9d, 0x41},
             "0x1.d6f34540ca458p+26");
  TestDouble({0x00, 0x00, 0x00, 0x54, 0x34, 0x6f, 0x9d, 0x41},
             "0x1.d6f3454p+26");
  TestDouble({0xfa, 0x16, 0x5e, 0x5b, 0xaf, 0xe9, 0x8f, 0x45},
             "0x1.fe9af5b5e16fap+89");
  TestDouble({0xd5, 0xcb, 0x6b, 0x5b, 0xaf, 0xe9, 0x8f, 0x45},
             "0x1.fe9af5b6bcbd5p+89");
  TestDouble({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0xff},
             "-0x1.fffffffffffffp+1023");
  TestDouble({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0x7f},
             "0x1.fffffffffffffp+1023");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xff}, "-0x1p+1023");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x7f}, "0x1p+1023");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, "-0x1p-1073");
  TestDouble({0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "0x1p-1073");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80}, "-0x1p-1074");
  TestDouble({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "0x1p-1074");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xc3}, "-0x1p+60");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x43}, "0x1p+60");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xe5}, "-0x1p+600");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x9a}, "-0x1p-600");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x65}, "0x1p+600");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x1a}, "0x1p-600");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6}, "-0x1p+97");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}, "0x1p+97");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0xfe}, "-0x1p+999");
  TestDouble({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x7e}, "0x1p+999");
}
