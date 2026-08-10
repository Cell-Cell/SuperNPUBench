#ifndef _INCLUDE_CPUSIM_TYPE_H_
#define _INCLUDE_CPUSIM_TYPE_H_

#include <cstddef>
#include <stdint.h>
#include <type_traits>

#define __in__
#define __out__
#define __vec__
#define __mtc__

struct __fp32 {
  float value;
  constexpr __fp32(float v = 0.0f) : value(v) {}
  constexpr operator float() const { return value; }
};
struct __tf32 { uint32_t bits; };
struct __hf32 { uint32_t bits; };
struct __half {
  uint16_t bits;
  constexpr __half(float = 0.0f) : bits(0) {}
};
struct __hif8 { uint8_t bits; };
struct __fp8_e4m3 { uint8_t bits; };
struct __fp8_e5m2 { uint8_t bits; };
struct __fp6_e3m2 { uint8_t bits; };
struct __fp6_e2m3 { uint8_t bits; };
struct __fp4_e2m1x2 { uint8_t bits; };
struct __fp4_e1m2x2 { uint8_t bits; };
struct __fp8_e8m0 { uint8_t bits; };
struct __fp4_hif4x2 { uint8_t bits; };
struct __int4x2 { uint8_t bits; };
struct __uint4x2 { uint8_t bits; };

enum __type_code {
  __type_fp64 = 0, __type_fp32 = 1, __type_tf32 = 2, __type_hf32 = 3,
  __type_fp16 = 4, __type_bf16 = 5, __type_hif8 = 6,
  __type_fp8_e4m3 = 7, __type_fp8_e5m2 = 8, __type_fp6_e3m2 = 9, __type_fp5_e2m3 = 10,
  __type_fp4_e2m1x2 = 11, __type_fp4_e1m2x2 = 12, __type_fp8_e8m0 = 13, __type_fp4_hif4x2 = 14,
  __type_int64 = 16, __type_int32 = 17, __type_int16 = 18, __type_int8 = 19, __type_int4x2 = 20,
  __type_uint64 = 24, __type_uint32 = 25, __type_uint16 = 26, __type_uint8 = 27, __type_uint4x2 = 28,
};

template <int C, int b> struct type_traits_base {
  static constexpr int TypeCode = C;
  static constexpr int bits = b;
};

template<> struct type_traits<double>         : public type_traits_base<__type_fp64, 64> {};
template<> struct type_traits<__fp32>         : public type_traits_base<__type_fp32, 32> {};
template<> struct type_traits<__tf32>         : public type_traits_base<__type_tf32, 32> {};
template<> struct type_traits<__hf32>         : public type_traits_base<__type_hf32, 32> {};
template<> struct type_traits<__half>         : public type_traits_base<__type_fp16, 16> {};
template<> struct type_traits<__hif8>         : public type_traits_base<__type_hif8, 8> {};
template<> struct type_traits<__fp8_e4m3>     : public type_traits_base<__type_fp8_e4m3, 8> {};
template<> struct type_traits<__fp8_e5m2>     : public type_traits_base<__type_fp8_e5m2, 8> {};
template<> struct type_traits<int64_t>        : public type_traits_base<__type_int64, 64> {};
template<> struct type_traits<int32_t>        : public type_traits_base<__type_int32, 32> {};
template<> struct type_traits<int16_t>        : public type_traits_base<__type_int16, 16> {};
template<> struct type_traits<int8_t>         : public type_traits_base<__type_int8, 8> {};
template<> struct type_traits<unsigned long>  : public type_traits_base<__type_uint64, 64> {};
template<> struct type_traits<unsigned int>   : public type_traits_base<__type_uint32, 32> {};
template<> struct type_traits<unsigned short> : public type_traits_base<__type_uint16, 16> {};
template<> struct type_traits<unsigned char>  : public type_traits_base<__type_uint8, 8> {};

#endif
