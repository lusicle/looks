#include <cmath>

#include "test_framework.h"
#include "util/json.h"

using looks::json::parse;
using looks::json::Value;
using looks::json::write;

TEST(json_parse_scalars) {
    CHECK(parse("null").value->is_null());
    CHECK_EQ(parse("true").value->as_bool(), true);
    CHECK_EQ(parse("false").value->as_bool(), false);
    CHECK_EQ(parse("42").value->as_number(), 42.0);
    CHECK_EQ(parse("-3.5").value->as_number(), -3.5);
    CHECK_EQ(parse("1e3").value->as_number(), 1000.0);
    CHECK_EQ(parse("\"hi\"").value->as_string(), "hi");
}

TEST(json_parse_structures) {
    auto result = parse(R"({"name":"clip","layers":[1,2,3],"nested":{"a":true}})");
    CHECK(result.value.has_value());
    const Value& v = *result.value;
    CHECK_EQ(v.get("name").as_string(), "clip");
    CHECK_EQ(v.get("layers").size(), size_t{3});
    CHECK_EQ(v.get("layers").array()[2].as_int(), 3);
    CHECK_EQ(v.get("nested").get("a").as_bool(), true);
    CHECK(v.get("missing").is_null());
}

TEST(json_parse_string_escapes) {
    auto result = parse(R"("a\n\t\"\\Aé😀")");
    CHECK(result.value.has_value());
    CHECK_EQ(result.value->as_string(), "a\n\t\"\\A\xC3\xA9\xF0\x9F\x98\x80");
}

TEST(json_parse_errors) {
    CHECK(!parse("").value.has_value());
    CHECK(!parse("{").value.has_value());
    CHECK(!parse("[1,]").value.has_value());
    CHECK(!parse("{\"a\":}").value.has_value());
    CHECK(!parse("tru").value.has_value());
    CHECK(!parse("1 2").value.has_value());
    CHECK(!parse("\"unterminated").value.has_value());
    CHECK(!parse("\"bad\\escape\"").value.has_value());
    CHECK(!parse("\"\\ud800\"").value.has_value());  // unpaired surrogate
    CHECK(!parse("1e999").value.has_value());        // non-finite
    // Error messages carry position info.
    auto bad = parse("{\n  \"a\": nope\n}");
    CHECK(!bad.value.has_value());
    CHECK(bad.error.find("line 2") != std::string::npos);
}

TEST(json_roundtrip) {
    Value doc = Value::make_object();
    doc.set("name", "project");
    doc.set("seed", int64_t{123456789});
    doc.set("opacity", 0.75);
    doc.set("enabled", true);
    doc.set("nothing", nullptr);
    Value layers = Value::make_array();
    Value layer = Value::make_object();
    layer.set("path", "layer0.fx2.threshold");
    layers.push(std::move(layer));
    doc.set("layers", std::move(layers));

    for (bool pretty : {true, false}) {
        auto reparsed = parse(write(doc, pretty));
        CHECK(reparsed.value.has_value());
        CHECK(*reparsed.value == doc);
    }
}

TEST(json_object_preserves_insertion_order) {
    Value doc = Value::make_object();
    doc.set("zebra", 1);
    doc.set("alpha", 2);
    doc.set("zebra", 3);   // update in place, keeps slot
    const auto& members = doc.object();
    CHECK_EQ(members.size(), size_t{2});
    CHECK_EQ(members[0].first, "zebra");
    CHECK_EQ(members[0].second.as_int(), 3);
    CHECK_EQ(members[1].first, "alpha");
}

TEST(json_number_formatting) {
    CHECK_EQ(write(Value(42.0), false), "42");
    CHECK_EQ(write(Value(-7.0), false), "-7");
    // Fractional survives round-trip exactly.
    auto v = parse(write(Value(0.1), false));
    CHECK_EQ(v.value->as_number(), 0.1);
}

TEST(json_deep_nesting_guard) {
    std::string deep(200, '[');
    deep += std::string(200, ']');
    CHECK(!parse(deep).value.has_value());
}
