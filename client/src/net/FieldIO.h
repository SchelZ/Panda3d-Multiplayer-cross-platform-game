// Thin, allocation-free wrappers over Panda3D's DCPacker for reading and
// writing the arguments of one DC field. The DC file decides the encoding
// (int16 / 10, string(1-16), arrays, structs...); these just walk it.
#pragma once

#include "dcPacker.h"

#include <cstdint>
#include <string>

namespace arena::net {

// Reads the parameters of a field the repository has already validated, so
// handlers can consume values without per-value error checks.
class FieldReader {
public:
    explicit FieldReader(DCPacker &packer) : p_(packer) {}

    double f64() { return p_.unpack_double(); }
    int32_t i32() { return p_.unpack_int(); }
    uint32_t u32() { return p_.unpack_uint(); }
    std::string str() { return p_.unpack_string(); }

    // Arrays and structs are nested: push() enters one, pop() leaves it.
    void push() { p_.push(); }
    void pop() { p_.pop(); }
    bool more() const { return p_.more_nested_fields(); }

private:
    DCPacker &p_;
};

class FieldWriter {
public:
    explicit FieldWriter(DCPacker &packer) : p_(packer) {}

    FieldWriter &f64(double v) { p_.pack_double(v); return *this; }
    FieldWriter &i32(int32_t v) { p_.pack_int(v); return *this; }
    FieldWriter &u32(uint32_t v) { p_.pack_uint(v); return *this; }
    FieldWriter &str(const std::string &v) { p_.pack_string(v); return *this; }
    FieldWriter &push() { p_.push(); return *this; }
    FieldWriter &pop() { p_.pop(); return *this; }

private:
    DCPacker &p_;
};

} // namespace arena::net
