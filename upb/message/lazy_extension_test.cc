// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Tests for lazily parsed extensions: decoding stores the serialized payload,
// which is parsed on demand by upb_Message_PromoteLazyExtension().

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include <gtest/gtest.h>
#include "absl/synchronization/notification.h"
#include "google/protobuf/test_messages_proto2.upb.h"
#include "google/protobuf/test_messages_proto2.upb_minitable.h"
#include "upb/base/string_view.h"
#include "upb/base/upcast.h"
#include "upb/mem/arena.h"
#include "upb/message/accessors.h"
#include "upb/message/compare.h"
#include "upb/message/copy.h"
#include "upb/message/internal/accessors.h"
#include "upb/message/internal/extension.h"
#include "upb/message/internal/message.h"
#include "upb/message/message.h"
#include "upb/message/promote.h"
#include "upb/message/value.h"
#include "upb/mini_table/extension.h"
#include "upb/mini_table/extension_registry.h"
#include "upb/mini_table/message.h"
#include "upb/test/test.upb.h"
#include "upb/test/test.upb_minitable.h"
#include "upb/wire/decode.h"
#include "upb/wire/encode.h"

// Must be last.
#include "upb/port/def.inc"

namespace {

const upb_MiniTable* kModelMiniTable = &upb_0test__ModelWithExtensions_msg_init;

std::string ToString(upb_StringView sv) {
  return std::string(sv.data, sv.size);
}

// Serializes a ModelWithExtensions{random_int32: 42, [model_ext] {str: str}}
// using the (eager) generated code.
std::string SerializeModelWithExt1(const char* str) {
  upb_Arena* arena = upb_Arena_New();
  upb_test_ModelWithExtensions* msg = upb_test_ModelWithExtensions_new(arena);
  upb_test_ModelWithExtensions_set_random_int32(msg, 42);
  upb_test_ModelExtension1* ext = upb_test_ModelExtension1_new(arena);
  upb_test_ModelExtension1_set_str(ext, upb_StringView_FromString(str));
  upb_test_ModelExtension1_set_model_ext(msg, ext, arena);
  size_t size;
  char* buf = upb_test_ModelWithExtensions_serialize(msg, arena, &size);
  std::string ret(buf, size);
  upb_Arena_Free(arena);
  return ret;
}

class LazyExtensionTest : public ::testing::Test {
 protected:
  LazyExtensionTest() {
    // The registry (and the extension minitables it refers to) must outlive
    // any message parsed with it.
    registry_arena_ = upb_Arena_New();
    lazy_ext_ = *upb_test_ModelExtension1_model_ext_ext;
    EXPECT_TRUE(upb_MiniTableExtension_SetLazy(&lazy_ext_, true));
    EXPECT_TRUE(upb_MiniTableExtension_IsLazy(&lazy_ext_));
    registry_ = upb_ExtensionRegistry_New(registry_arena_);
    EXPECT_EQ(upb_ExtensionRegistry_Add(registry_, &lazy_ext_),
              kUpb_ExtensionRegistryStatus_Ok);
    arena_ = upb_Arena_New();
  }

  ~LazyExtensionTest() override {
    upb_Arena_Free(arena_);
    upb_Arena_Free(registry_arena_);
  }

  // Parses `wire` into a fresh message in `arena` using the lazy registry.
  upb_Message* Parse(const std::string& wire, int options = 0,
                     upb_Arena* arena = nullptr) {
    if (!arena) arena = arena_;
    upb_Message* msg = upb_Message_New(kModelMiniTable, arena);
    EXPECT_EQ(upb_Decode(wire.data(), wire.size(), msg, kModelMiniTable,
                         registry_, options, arena),
              kUpb_DecodeStatus_Ok);
    return msg;
  }

  // Returns the aux_data entry for the lazy extension, or a null tagged
  // pointer if there is none.
  upb_TaggedAuxPtr Entry(const upb_Message* msg) {
    upb_TaggedAuxPtr ptr = upb_TaggedAuxPtr_Null();
    UPB_PRIVATE(_upb_Message_FindExtensionEntry)(msg, &lazy_ext_, nullptr,
                                                 &ptr);
    return ptr;
  }

  std::string Encode(const upb_Message* msg, int options = 0) {
    upb_Arena* arena = upb_Arena_New();
    char* buf;
    size_t size;
    EXPECT_EQ(upb_Encode(msg, kModelMiniTable, options, arena, &buf, &size),
              kUpb_EncodeStatus_Ok);
    std::string ret(buf, size);
    upb_Arena_Free(arena);
    return ret;
  }

  upb_Arena* registry_arena_;
  upb_Arena* arena_;
  upb_MiniTableExtension lazy_ext_;
  upb_ExtensionRegistry* registry_;
};

TEST_F(LazyExtensionTest, SetLazyRejectsNonMessageExtensions) {
  upb_MiniTableExtension scalar =
      *protobuf_test_messages_proto2_extension_int32_ext;
  EXPECT_FALSE(upb_MiniTableExtension_SetLazy(&scalar, true));
  EXPECT_FALSE(upb_MiniTableExtension_IsLazy(&scalar));
  // SetLazy(false) is always allowed on a message extension.
  upb_MiniTableExtension ext = *upb_test_ModelExtension1_model_ext_ext;
  EXPECT_TRUE(upb_MiniTableExtension_SetLazy(&ext, true));
  EXPECT_TRUE(upb_MiniTableExtension_SetLazy(&ext, false));
  EXPECT_FALSE(upb_MiniTableExtension_IsLazy(&ext));
}

TEST_F(LazyExtensionTest, DecodeStoresPayloadWithoutParsing) {
  const std::string wire = SerializeModelWithExt1("hello");
  upb_Message* msg = Parse(wire);

  // The extension is present but has no parsed value yet.
  EXPECT_TRUE(upb_Message_HasExtension(msg, &lazy_ext_));
  EXPECT_EQ(upb_Message_GetExtensionMessage(msg, &lazy_ext_, nullptr), nullptr);
  EXPECT_EQ(upb_Message_ExtensionCount(msg), 1);
  EXPECT_FALSE(upb_Message_HasUnknown(msg));

  // Iteration skips unpromoted lazy extensions.
  const upb_MiniTableExtension* e;
  upb_MessageValue v;
  uintptr_t iter = kUpb_Message_ExtensionBegin;
  EXPECT_FALSE(upb_Message_NextExtension(msg, &e, &v, &iter));

  // The payload was copied (no aliasing requested).
  upb_TaggedAuxPtr entry = Entry(msg);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  EXPECT_FALSE(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry));
  const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
  EXPECT_EQ(lazy->ext, &lazy_ext_);
  EXPECT_EQ(lazy->registry, registry_);
  EXPECT_TRUE(lazy->data.data < wire.data() ||
              lazy->data.data >= wire.data() + wire.size());
  EXPECT_EQ(lazy->data.data, reinterpret_cast<const char*>(lazy + 1));
}

TEST_F(LazyExtensionTest, DecodeAliasesPayloadWhenRequested) {
  const std::string wire = SerializeModelWithExt1("hello");
  upb_Message* msg = Parse(wire, kUpb_DecodeOption_AliasString);

  upb_TaggedAuxPtr entry = Entry(msg);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry));
  const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
  EXPECT_GE(lazy->data.data, wire.data());
  EXPECT_LE(lazy->data.data + lazy->data.size, wire.data() + wire.size());

  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(
      ToString(upb_test_ModelExtension1_str(
          reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val))),
      "hello");
}

TEST_F(LazyExtensionTest, PromoteParsesAndPublishes) {
  upb_Message* msg = Parse(SerializeModelWithExt1("hello"));

  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  ASSERT_NE(val.msg_val, nullptr);
  const auto* ext1 =
      reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val);
  EXPECT_EQ(ToString(upb_test_ModelExtension1_str(ext1)), "hello");

  // The entry now carries the promoted tag and behaves like any other
  // canonical extension.
  upb_TaggedAuxPtr entry = Entry(msg);
  EXPECT_TRUE(upb_TaggedAuxPtr_IsPromotedExtension(entry));
  EXPECT_TRUE(upb_TaggedAuxPtr_IsCanonicalExtension(entry));
  EXPECT_EQ(upb_Message_GetExtensionMessage(msg, &lazy_ext_, nullptr),
            val.msg_val);
  EXPECT_EQ(upb_Message_ExtensionCount(msg), 1);

  const upb_MiniTableExtension* e;
  upb_MessageValue v;
  uintptr_t iter = kUpb_Message_ExtensionBegin;
  ASSERT_TRUE(upb_Message_NextExtension(msg, &e, &v, &iter));
  EXPECT_EQ(e, &lazy_ext_);
  EXPECT_EQ(v.msg_val, val.msg_val);
  EXPECT_FALSE(upb_Message_NextExtension(msg, &e, &v, &iter));

  // Promoting again is idempotent.
  upb_MessageValue val2;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val2),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(val2.msg_val, val.msg_val);

  // upb_Message_GetOrPromoteExtension() also understands lazy entries.
  upb_Message* msg2 = Parse(SerializeModelWithExt1("world"));
  upb_MessageValue val3;
  ASSERT_EQ(
      upb_Message_GetOrPromoteExtension(msg2, &lazy_ext_, 0, arena_, &val3),
      kUpb_GetExtension_Ok);
  EXPECT_EQ(
      ToString(upb_test_ModelExtension1_str(
          reinterpret_cast<const upb_test_ModelExtension1*>(val3.msg_val))),
      "world");
}

TEST_F(LazyExtensionTest, PromoteNotPresent) {
  upb_Message* msg = upb_Message_New(kModelMiniTable, arena_);
  upb_MessageValue val;
  EXPECT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_NotPresent);
}

TEST_F(LazyExtensionTest, PromoteEagerlyParsedExtension) {
  // An extension that was set directly (not lazily) is returned as-is.
  upb_Message* msg = upb_Message_New(kModelMiniTable, arena_);
  upb_test_ModelExtension1* ext1 = upb_test_ModelExtension1_new(arena_);
  ASSERT_TRUE(upb_Message_SetExtensionMessage(msg, &lazy_ext_, UPB_UPCAST(ext1),
                                              arena_));
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(val.msg_val, UPB_UPCAST(ext1));
  EXPECT_FALSE(upb_TaggedAuxPtr_IsPromotedExtension(Entry(msg)));
}

TEST_F(LazyExtensionTest, RepeatedOccurrencesAreCoalescedAndMerged) {
  // Two occurrences of the same singular message extension merge on parse;
  // "world" is set last and therefore wins.
  const std::string wire =
      SerializeModelWithExt1("hello") + SerializeModelWithExt1("world");
  for (int options : {0, static_cast<int>(kUpb_DecodeOption_AliasString)}) {
    upb_Message* msg = Parse(wire, options);
    EXPECT_EQ(upb_Message_ExtensionCount(msg), 1);

    upb_TaggedAuxPtr entry = Entry(msg);
    ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
    // Coalesced payloads are always copied into one contiguous block.
    EXPECT_FALSE(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry));
    const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
    EXPECT_EQ(lazy->data.data, reinterpret_cast<const char*>(lazy + 1));

    upb_MessageValue val;
    ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
              kUpb_GetExtension_Ok);
    EXPECT_EQ(
        ToString(upb_test_ModelExtension1_str(
            reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val))),
        "world");
  }
}

TEST_F(LazyExtensionTest, ThreeOccurrencesGrowTheBlock) {
  const std::string wire = SerializeModelWithExt1("a") +
                           SerializeModelWithExt1("bb") +
                           SerializeModelWithExt1("ccc");
  upb_Message* msg = Parse(wire);
  upb_TaggedAuxPtr entry = Entry(msg);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
  // Each occurrence contributes a `str` field of [2 byte tag][len][str].
  EXPECT_EQ(lazy->data.size, (3 + 1) + (3 + 2) + (3 + 3));

  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(
      ToString(upb_test_ModelExtension1_str(
          reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val))),
      "ccc");
}

TEST_F(LazyExtensionTest, LazyPayloadMergesIntoExistingParsedValue) {
  // If the message already holds a parsed value for the extension, the decoder
  // falls back to an eager parse so that the two can be merged.
  upb_Message* msg = upb_Message_New(kModelMiniTable, arena_);
  upb_test_ModelExtension1* ext1 = upb_test_ModelExtension1_new(arena_);
  ASSERT_TRUE(upb_Message_SetExtensionMessage(msg, &lazy_ext_, UPB_UPCAST(ext1),
                                              arena_));
  const std::string wire = SerializeModelWithExt1("merged");
  ASSERT_EQ(upb_Decode(wire.data(), wire.size(), msg, kModelMiniTable,
                       registry_, 0, arena_),
            kUpb_DecodeStatus_Ok);
  EXPECT_FALSE(upb_TaggedAuxPtr_IsLazyExtension(Entry(msg)));
  EXPECT_EQ(upb_Message_GetExtensionMessage(msg, &lazy_ext_, nullptr),
            UPB_UPCAST(ext1));
  EXPECT_EQ(ToString(upb_test_ModelExtension1_str(ext1)), "merged");
  EXPECT_EQ(upb_Message_ExtensionCount(msg), 1);
}

TEST_F(LazyExtensionTest, EncodePreservesLazyPayload) {
  const std::string wire = SerializeModelWithExt1("hello");
  upb_Message* msg = Parse(wire);

  // Encoding a message with an unpromoted lazy extension writes the payload
  // back out verbatim, in both encoding modes.
  const std::string lazy_bytes = Encode(msg);
  EXPECT_EQ(lazy_bytes, wire);
  EXPECT_EQ(Encode(msg, kUpb_EncodeOption_Deterministic), wire);

  // Promotion does not change the serialized form.
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(Encode(msg), wire);
  EXPECT_EQ(Encode(msg, kUpb_EncodeOption_Deterministic), wire);
}

TEST_F(LazyExtensionTest, EncodeCoalescedPayloadRoundTrips) {
  const std::string wire =
      SerializeModelWithExt1("hello") + SerializeModelWithExt1("world");
  upb_Message* msg = Parse(wire);
  const std::string lazy_bytes = Encode(msg, kUpb_EncodeOption_Deterministic);

  // The re-encoded message parses (eagerly, with the generated code and the
  // generated extension minitable) to the merged value.
  upb_ExtensionRegistry* eager_registry = upb_ExtensionRegistry_New(arena_);
  ASSERT_EQ(upb_ExtensionRegistry_Add(eager_registry,
                                      upb_test_ModelExtension1_model_ext_ext),
            kUpb_ExtensionRegistryStatus_Ok);
  upb_test_ModelWithExtensions* parsed = upb_test_ModelWithExtensions_parse_ex(
      lazy_bytes.data(), lazy_bytes.size(), eager_registry, 0, arena_);
  ASSERT_NE(parsed, nullptr);
  EXPECT_EQ(upb_test_ModelWithExtensions_random_int32(parsed), 42);
  const upb_test_ModelExtension1* ext1 =
      upb_test_ModelExtension1_model_ext(parsed);
  ASSERT_NE(ext1, nullptr);
  EXPECT_EQ(ToString(upb_test_ModelExtension1_str(ext1)), "world");
}

TEST_F(LazyExtensionTest, MalformedPayloadIsDetectedAtPromotion) {
  // Field 1547, length-delimited, payload "\x0a\x05ab" claims a 5 byte string
  // but only provides 2 bytes.
  std::string wire;
  const uint32_t tag = (1547 << 3) | 2;
  // Varint-encode the tag.
  wire.push_back(static_cast<char>((tag & 0x7f) | 0x80));
  wire.push_back(static_cast<char>(tag >> 7));
  wire.push_back(4);
  wire += std::string(
      "\x0a\x05"
      "ab",
      4);

  // The outer parse succeeds because the payload is not inspected.
  upb_Message* msg = Parse(wire);
  EXPECT_TRUE(upb_Message_HasExtension(msg, &lazy_ext_));

  upb_MessageValue val;
  EXPECT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_ParseError);
  // The message is unchanged and the error is reproducible.
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(Entry(msg)));
  EXPECT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_ParseError);
  // And the bytes still round-trip.
  EXPECT_EQ(Encode(msg), wire);
}

TEST_F(LazyExtensionTest, SetExtensionReplacesLazyPayload) {
  upb_Message* msg = Parse(SerializeModelWithExt1("hello"));
  upb_test_ModelExtension1* ext1 = upb_test_ModelExtension1_new(arena_);
  upb_test_ModelExtension1_set_str(ext1, upb_StringView_FromString("set"));
  ASSERT_TRUE(upb_Message_SetExtensionMessage(msg, &lazy_ext_, UPB_UPCAST(ext1),
                                              arena_));
  EXPECT_EQ(upb_Message_ExtensionCount(msg), 1);
  EXPECT_FALSE(upb_TaggedAuxPtr_IsLazyExtension(Entry(msg)));
  EXPECT_EQ(upb_Message_GetExtensionMessage(msg, &lazy_ext_, nullptr),
            UPB_UPCAST(ext1));
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(val.msg_val, UPB_UPCAST(ext1));
}

TEST_F(LazyExtensionTest, ClearExtensionRemovesLazyPayload) {
  upb_Message* msg = Parse(SerializeModelWithExt1("hello"));
  upb_Message_ClearExtension(msg, &lazy_ext_);
  EXPECT_FALSE(upb_Message_HasExtension(msg, &lazy_ext_));
  EXPECT_EQ(upb_Message_ExtensionCount(msg), 0);
  EXPECT_TRUE(upb_TaggedAuxPtr_IsNull(Entry(msg)));
  upb_MessageValue val;
  EXPECT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_NotPresent);
  // Only `random_int32: 42` remains.
  EXPECT_EQ(Encode(msg), std::string("\x18\x2a", 2));
}

TEST_F(LazyExtensionTest, DiscardUnknownKeepsLazyExtensions) {
  // Append an unknown field (number 2000, varint 1) to the wire format.
  std::string wire = SerializeModelWithExt1("hello");
  const uint32_t tag = (2000 << 3) | 0;
  wire.push_back(static_cast<char>((tag & 0x7f) | 0x80));
  wire.push_back(static_cast<char>(tag >> 7));
  wire.push_back(1);
  upb_Message* msg = Parse(wire);
  EXPECT_TRUE(upb_Message_HasUnknown(msg));
  _upb_Message_DiscardUnknown_shallow(msg);
  EXPECT_FALSE(upb_Message_HasUnknown(msg));
  EXPECT_TRUE(upb_Message_HasExtension(msg, &lazy_ext_));
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(Entry(msg)));
}

TEST_F(LazyExtensionTest, DeepCloneCopiesLazyPayload) {
  upb_Arena* src_arena = upb_Arena_New();
  upb_Message* src = Parse(SerializeModelWithExt1("hello"), 0, src_arena);
  const upb_LazyExtensionData* src_lazy =
      upb_TaggedAuxPtr_LazyExtension(Entry(src));

  upb_Message* clone = upb_Message_DeepClone(src, kModelMiniTable, arena_);
  ASSERT_NE(clone, nullptr);
  upb_TaggedAuxPtr entry = Entry(clone);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  EXPECT_FALSE(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry));
  const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
  EXPECT_NE(lazy, src_lazy);
  EXPECT_NE(lazy->data.data, src_lazy->data.data);
  EXPECT_EQ(lazy->ext, &lazy_ext_);
  EXPECT_EQ(lazy->registry, registry_);
  EXPECT_EQ(lazy->options, src_lazy->options);

  // The clone is fully independent of the source.
  upb_Arena_Free(src_arena);
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(clone, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_EQ(
      ToString(upb_test_ModelExtension1_str(
          reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val))),
      "hello");
}

TEST_F(LazyExtensionTest, DeepClonePromotedExtensionBecomesCanonical) {
  upb_Message* src = Parse(SerializeModelWithExt1("hello"));
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(src, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);

  upb_Message* clone = upb_Message_DeepClone(src, kModelMiniTable, arena_);
  ASSERT_NE(clone, nullptr);
  upb_TaggedAuxPtr entry = Entry(clone);
  EXPECT_TRUE(upb_TaggedAuxPtr_IsCanonicalExtension(entry));
  EXPECT_FALSE(upb_TaggedAuxPtr_IsPromotedExtension(entry));
  upb_Message* cloned_sub =
      upb_Message_GetExtensionMessage(clone, &lazy_ext_, nullptr);
  ASSERT_NE(cloned_sub, nullptr);
  EXPECT_NE(cloned_sub, val.msg_val);
  EXPECT_EQ(ToString(upb_test_ModelExtension1_str(
                reinterpret_cast<const upb_test_ModelExtension1*>(cloned_sub))),
            "hello");
}

TEST_F(LazyExtensionTest, ShallowCopyAliasesLazyPayload) {
  upb_Message* src = Parse(SerializeModelWithExt1("hello"));
  const upb_LazyExtensionData* src_lazy =
      upb_TaggedAuxPtr_LazyExtension(Entry(src));

  upb_Message* dst = upb_Message_New(kModelMiniTable, arena_);
  ASSERT_TRUE(upb_Message_ShallowCopy(dst, src, kModelMiniTable, arena_));
  upb_TaggedAuxPtr entry = Entry(dst);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry));
  const upb_LazyExtensionData* lazy = upb_TaggedAuxPtr_LazyExtension(entry);
  EXPECT_NE(lazy, src_lazy);
  EXPECT_EQ(lazy->data.data, src_lazy->data.data);
  EXPECT_EQ(lazy->data.size, src_lazy->data.size);

  // Promoting the copy does not affect the source.
  upb_MessageValue val;
  ASSERT_EQ(upb_Message_PromoteLazyExtension(dst, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(Entry(src)));
}

TEST_F(LazyExtensionTest, IsEqualHandlesLazyExtensions) {
  const std::string hello = SerializeModelWithExt1("hello");
  const std::string world = SerializeModelWithExt1("world");
  const upb_Message* lazy_hello = Parse(hello);
  const upb_Message* lazy_hello2 = Parse(hello);
  const upb_Message* lazy_world = Parse(world);

  // Eagerly parsed counterpart, using the same (lazy) extension minitable.
  upb_Message* eager_hello = upb_Message_New(kModelMiniTable, arena_);
  upb_test_ModelWithExtensions_set_random_int32(
      reinterpret_cast<upb_test_ModelWithExtensions*>(eager_hello), 42);
  upb_test_ModelExtension1* ext1 = upb_test_ModelExtension1_new(arena_);
  upb_test_ModelExtension1_set_str(ext1, upb_StringView_FromString("hello"));
  ASSERT_TRUE(upb_Message_SetExtensionMessage(eager_hello, &lazy_ext_,
                                              UPB_UPCAST(ext1), arena_));

  // Lazy vs lazy with identical bytes (fast path).
  EXPECT_TRUE(upb_Message_IsEqual(lazy_hello, lazy_hello2, kModelMiniTable, 0));
  // Lazy vs lazy with different bytes.
  EXPECT_FALSE(upb_Message_IsEqual(lazy_hello, lazy_world, kModelMiniTable, 0));
  // Lazy vs eager, both directions.
  EXPECT_TRUE(upb_Message_IsEqual(lazy_hello, eager_hello, kModelMiniTable, 0));
  EXPECT_TRUE(upb_Message_IsEqual(eager_hello, lazy_hello, kModelMiniTable, 0));
  EXPECT_FALSE(
      upb_Message_IsEqual(lazy_world, eager_hello, kModelMiniTable, 0));
  // Comparing never promotes.
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(Entry(lazy_hello)));
  EXPECT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(Entry(lazy_world)));

  // Missing on one side.
  upb_Message* empty = upb_Message_New(kModelMiniTable, arena_);
  upb_test_ModelWithExtensions_set_random_int32(
      reinterpret_cast<upb_test_ModelWithExtensions*>(empty), 42);
  EXPECT_FALSE(upb_Message_IsEqual(lazy_hello, empty, kModelMiniTable, 0));
  EXPECT_FALSE(upb_Message_IsEqual(empty, lazy_hello, kModelMiniTable, 0));

  // Still equal after promoting one side.
  upb_MessageValue val;
  ASSERT_EQ(
      upb_Message_PromoteLazyExtension(lazy_hello, &lazy_ext_, arena_, &val),
      kUpb_GetExtension_Ok);
  EXPECT_TRUE(upb_Message_IsEqual(lazy_hello, lazy_hello2, kModelMiniTable, 0));
  EXPECT_TRUE(upb_Message_IsEqual(lazy_hello2, lazy_hello, kModelMiniTable, 0));
}

TEST_F(LazyExtensionTest, FrozenMessagePromotesFrozenSubmessage) {
  upb_Message* msg = Parse(SerializeModelWithExt1("hello"));
  upb_Message_Freeze(msg, kModelMiniTable);
  ASSERT_TRUE(upb_Message_IsFrozen(msg));

  const upb_Message* const_msg = msg;
  upb_MessageValue val;
  ASSERT_EQ(
      upb_Message_PromoteLazyExtension(const_msg, &lazy_ext_, arena_, &val),
      kUpb_GetExtension_Ok);
  EXPECT_TRUE(upb_Message_IsFrozen(val.msg_val));
  EXPECT_EQ(
      ToString(upb_test_ModelExtension1_str(
          reinterpret_cast<const upb_test_ModelExtension1*>(val.msg_val))),
      "hello");
}

TEST_F(LazyExtensionTest, ConcurrentPromotionPublishesExactlyOneValue) {
  constexpr int kThreads = 16;
  constexpr int kRounds = 20;
  for (int round = 0; round < kRounds; round++) {
    upb_Message* msg = Parse(SerializeModelWithExt1("hello"));
    upb_Message_Freeze(msg, kModelMiniTable);
    const upb_Message* const_msg = msg;

    absl::Notification start;
    std::vector<const upb_Message*> results(kThreads, nullptr);
    std::vector<upb_GetExtension_Status> statuses(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; i++) {
      threads.emplace_back([&, i] {
        start.WaitForNotification();
        upb_MessageValue val;
        statuses[i] = upb_Message_PromoteLazyExtension(const_msg, &lazy_ext_,
                                                       arena_, &val);
        results[i] = val.msg_val;
        // Concurrent readers must observe either no value or the published
        // one; never a torn or stale pointer.
        const upb_Message* seen =
            upb_Message_GetExtensionMessage(const_msg, &lazy_ext_, nullptr);
        EXPECT_TRUE(seen == nullptr || seen == results[i]);
      });
    }
    start.Notify();
    for (auto& t : threads) t.join();

    for (int i = 0; i < kThreads; i++) {
      EXPECT_EQ(statuses[i], kUpb_GetExtension_Ok);
      EXPECT_EQ(results[i], results[0]);
    }
    ASSERT_NE(results[0], nullptr);
    EXPECT_TRUE(upb_Message_IsFrozen(results[0]));
    EXPECT_EQ(
        ToString(upb_test_ModelExtension1_str(
            reinterpret_cast<const upb_test_ModelExtension1*>(results[0]))),
        "hello");
    EXPECT_EQ(upb_Message_GetExtensionMessage(const_msg, &lazy_ext_, nullptr),
              results[0]);
  }
}

TEST_F(LazyExtensionTest, NestedDepthLimitIsPreserved) {
  // With max depth 1 the extension submessage uses up the last level of
  // nesting; the remaining budget (zero) cannot be expressed as a lazy decode
  // option, so the decoder falls back to parsing eagerly, exactly like it would
  // without laziness.
  const std::string wire = SerializeModelWithExt1("hello");
  upb_Message* msg = upb_Message_New(kModelMiniTable, arena_);
  ASSERT_EQ(upb_Decode(wire.data(), wire.size(), msg, kModelMiniTable,
                       registry_, upb_DecodeOptions_MaxDepth(1), arena_),
            kUpb_DecodeStatus_Ok);
  EXPECT_TRUE(upb_TaggedAuxPtr_IsCanonicalExtension(Entry(msg)));
  EXPECT_FALSE(upb_TaggedAuxPtr_IsPromotedExtension(Entry(msg)));
  EXPECT_NE(upb_Message_GetExtensionMessage(msg, &lazy_ext_, nullptr), nullptr);

  // With depth 2 the payload is stored lazily and remembers that only one
  // more level of nesting is allowed.
  msg = upb_Message_New(kModelMiniTable, arena_);
  ASSERT_EQ(upb_Decode(wire.data(), wire.size(), msg, kModelMiniTable,
                       registry_, upb_DecodeOptions_MaxDepth(2), arena_),
            kUpb_DecodeStatus_Ok);
  upb_TaggedAuxPtr entry = Entry(msg);
  ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
  EXPECT_EQ(upb_DecodeOptions_GetEffectiveMaxDepth(
                upb_TaggedAuxPtr_LazyExtension(entry)->options),
            1);
  upb_MessageValue val;
  EXPECT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext_, arena_, &val),
            kUpb_GetExtension_Ok);
}

// MessageSet wire format.

const upb_MiniTable* kMsgSetMiniTable =
    &protobuf_0test_0messages__proto2__TestAllTypesProto2__MessageSetCorrect_msg_init;

TEST(LazyMessageSetTest, DecodePromoteAndEncode) {
  upb_Arena* arena = upb_Arena_New();
  upb_MiniTableExtension lazy_ext =
      *protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1_message_set_extension_ext;
  ASSERT_TRUE(upb_MiniTableExtension_SetLazy(&lazy_ext, true));
  upb_ExtensionRegistry* registry = upb_ExtensionRegistry_New(arena);
  ASSERT_EQ(upb_ExtensionRegistry_Add(registry, &lazy_ext),
            kUpb_ExtensionRegistryStatus_Ok);

  // Build the wire format with the generated (eager) code.
  auto* src =
      protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrect_new(
          arena);
  auto* src_ext =
      protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1_new(
          arena);
  protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1_set_str(
      src_ext, upb_StringView_FromString("hello"));
  protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1_set_message_set_extension(
      src, src_ext, arena);
  size_t size;
  char* buf =
      protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrect_serialize(
          src, arena, &size);
  const std::string wire(buf, size);

  for (int options : {0, static_cast<int>(kUpb_DecodeOption_AliasString)}) {
    upb_Message* msg = upb_Message_New(kMsgSetMiniTable, arena);
    ASSERT_EQ(upb_Decode(wire.data(), wire.size(), msg, kMsgSetMiniTable,
                         registry, options, arena),
              kUpb_DecodeStatus_Ok);
    upb_TaggedAuxPtr entry = upb_TaggedAuxPtr_Null();
    ASSERT_TRUE(UPB_PRIVATE(_upb_Message_FindExtensionEntry)(msg, &lazy_ext,
                                                             nullptr, &entry));
    ASSERT_TRUE(upb_TaggedAuxPtr_IsLazyExtension(entry));
    EXPECT_EQ(upb_TaggedAuxPtr_IsLazyExtensionAliased(entry),
              options == kUpb_DecodeOption_AliasString);

    // Lazy MessageSet items are re-emitted with MessageSet item framing.
    for (int enc_options :
         {0, static_cast<int>(kUpb_EncodeOption_Deterministic)}) {
      char* out;
      size_t out_size;
      ASSERT_EQ(upb_Encode(msg, kMsgSetMiniTable, enc_options, arena, &out,
                           &out_size),
                kUpb_EncodeStatus_Ok);
      EXPECT_EQ(std::string(out, out_size), wire);
    }

    upb_MessageValue val;
    ASSERT_EQ(upb_Message_PromoteLazyExtension(msg, &lazy_ext, arena, &val),
              kUpb_GetExtension_Ok);
    upb_StringView str =
        protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1_str(
            reinterpret_cast<
                const protobuf_test_messages_proto2_TestAllTypesProto2_MessageSetCorrectExtension1*>(
                val.msg_val));
    EXPECT_EQ(ToString(str), "hello");
  }
  upb_Arena_Free(arena);
}

}  // namespace
