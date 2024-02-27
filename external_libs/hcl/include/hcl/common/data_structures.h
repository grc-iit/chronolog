/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Distributed under BSD 3-Clause license.                                   *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Illinois Institute of Technology.                        *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of Hermes. The full Hermes copyright notice, including  *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the top directory. If you do not  *
 * have access to the file, you may request a copy from help@hdfgroup.org.   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*-------------------------------------------------------------------------
 *
 * Created: data_structures.h
 * May 28 2018
 * Hariharan Devarajan <hdevarajan@hdfgroup.org>
 *
 * Purpose: Defines all data structures required in HCL.
 *
 *-------------------------------------------------------------------------
 */

#ifndef INCLUDE_HCL_COMMON_DATA_STRUCTURES_H_
#define INCLUDE_HCL_COMMON_DATA_STRUCTURES_H_

#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/containers/vector.hpp>

#ifdef HCL_ENABLE_RPCLIB
#include <rpc/msgpack.hpp>
#endif

#include <boost/concept_check.hpp>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "typedefs.h"

namespace bip = boost::interprocess;

typedef struct CharStruct {
 private:
  char value[256];
  void Set(char *data_, size_t size) {
    snprintf(this->value, size + 1, "%s", data_);
  }
  void Set(std::string data_) {
    snprintf(this->value, data_.length() + 1, "%s", data_.c_str());
  }

 public:
  CharStruct() {}
  CharStruct(const CharStruct &other)
      : CharStruct(other.value) {} /* copy constructor*/
  CharStruct(CharStruct &&other)
      : CharStruct(other.value) {} /* move constructor*/

  CharStruct(const char *data_) {
    snprintf(this->value, strlen(data_) + 1, "%s", data_);
  }
  CharStruct(std::string data_) : CharStruct(data_.c_str()) {}

  CharStruct(char *data_, size_t size) {
    snprintf(this->value, size, "%s", data_);
  }
  const char *c_str() const { return value; }
  std::string string() const { return std::string(value); }

  char *data() { return value; }
  const size_t size() const { return strlen(value); }
  /**
   * Operators
   */
  CharStruct &operator=(const CharStruct &other) {
    strcpy(value, other.c_str());
    return *this;
  }
  /* equal operator for comparing two Chars. */
  bool operator==(const CharStruct &o) const {
    return strcmp(value, o.value) == 0;
  }
  CharStruct operator+(const CharStruct &o) {
    std::string added = std::string(this->c_str()) + std::string(o.c_str());
    return CharStruct(added);
  }
  CharStruct operator+(std::string &o) {
    std::string added = std::string(this->c_str()) + o;
    return CharStruct(added);
  }
  CharStruct &operator+=(const CharStruct &rhs) {
    std::string added = std::string(this->c_str()) + std::string(rhs.c_str());
    Set(added);
    return *this;
  }
  bool operator>(const CharStruct &o) const {
    return strcmp(this->value, o.c_str()) > 0;
  }
  bool operator>=(const CharStruct &o) const {
    return strcmp(this->value, o.c_str()) >= 0;
  }
  bool operator<(const CharStruct &o) const {
    return strcmp(this->value, o.c_str()) < 0;
  }
  bool operator<=(const CharStruct &o) const {
    return strcmp(this->value, o.c_str()) <= 0;
  }

} CharStruct;

static CharStruct operator+(const std::string &a1, const CharStruct &a2) {
  std::string added = a1 + std::string(a2.c_str());
  return CharStruct(added);
}

namespace std {
template <>
struct hash<CharStruct> {
  size_t operator()(const CharStruct &k) const {
    std::string val(k.c_str());
    return std::hash<std::string>()(val);
  }
};
}  // namespace std

#ifdef HCL_ENABLE_RPCLIB
namespace clmdep_msgpack {
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) {
  namespace adaptor {
  namespace mv1 = clmdep_msgpack::v1;
  template <>
  struct convert<CharStruct> {
    mv1::object const &operator()(mv1::object const &o,
                                  CharStruct &input) const {
      std::string v = std::string();
      v.assign(o.via.str.ptr, o.via.str.size);
      input = CharStruct(v);
      return o;
    }
  };

  template <>
  struct pack<CharStruct> {
    template <typename Stream>
    packer<Stream> &operator()(mv1::packer<Stream> &o,
                               CharStruct const &input) const {
      uint32_t size = checked_get_container_size(input.size());
      o.pack_str(size);
      o.pack_str_body(input.c_str(), size);
      return o;
    }
  };

  template <>
  struct object_with_zone<CharStruct> {
    void operator()(mv1::object::with_zone &o, CharStruct const &input) const {
      uint32_t size = checked_get_container_size(input.size());
      o.type = clmdep_msgpack::type::STR;
      char *ptr = static_cast<char *>(
          o.zone.allocate_align(size, MSGPACK_ZONE_ALIGNOF(char)));
      o.via.str.ptr = ptr;
      o.via.str.size = size;
      std::memcpy(ptr, input.c_str(), input.size());
    }
  };
  }  // namespace adaptor
}
}  // namespace clmdep_msgpack
#endif

/**
 * Outstream conversions
 */

template <typename T, typename O>
inline std::ostream &operator<<(std::ostream &os, std::pair<T, O> const m) {
  return os << "{TYPE:pair,"
            << "first:" << m.first << ","
            << "second:" << m.second << "}";
}

inline std::ostream &operator<<(std::ostream &os, char const *m) {
  return os << std::string(m);
}

inline std::ostream &operator<<(std::ostream &os, uint8_t const &m) {
  return os << std::to_string(m);
}

inline std::ostream &operator<<(std::ostream &os, CharStruct const &m) {
  return os << "{TYPE:CharStruct,"
            << "value:" << m.c_str() << "}";
}

template <typename T>
inline std::ostream &operator<<(std::ostream &os, std::vector<T> const &ms) {
  os << "[";
  for (auto m : ms) {
    os << m << ",";
  }
  os << "]";
  return os;
}

template <typename T>
class CalculateSize {
 public:
  really_long GetSize(T value) { return sizeof(value); }
};
template <>
class CalculateSize<std::string> {
 public:
  really_long GetSize(std::string value) { return strlen(value.c_str()) + 1; }
};
template <>
class CalculateSize<bip::string> {
 public:
  really_long GetSize(bip::string value) { return strlen(value.c_str()) + 1; }
};

#endif  // INCLUDE_HCL_COMMON_DATA_STRUCTURES_H_
