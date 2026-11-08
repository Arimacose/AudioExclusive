#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <limits>
// Independent decoder: convert emitted driver bytes back to normalized values.
// Deliberately does not call production size/packing functions for expectations.
struct Layout { long type; unsigned bytes, valid; bool little, floating; };
inline constexpr Layout layouts[] = {
 {0,2,16,false,false},{1,3,24,false,false},{2,4,32,false,false},
 {3,4,32,false,true},{4,8,64,false,true},
 {8,4,16,false,false},{9,4,18,false,false},{10,4,20,false,false},{11,4,24,false,false},
 {16,2,16,true,false},{17,3,24,true,false},{18,4,32,true,false},
 {19,4,32,true,true},{20,8,64,true,true},
 {24,4,16,true,false},{25,4,18,true,false},{26,4,20,true,false},{27,4,24,true,false}
};
inline void VerifySample(const unsigned char* data,const Layout& l,std::int32_t source) {
 std::uint64_t raw=0;
 for(unsigned i=0;i<l.bytes;++i) raw=raw*256+data[l.little?l.bytes-1-i:i];
 if(l.floating) {
  double actual;
  if(l.bytes==4){auto bits=static_cast<std::uint32_t>(raw);float f;std::memcpy(&f,&bits,4);actual=f;}
  else std::memcpy(&actual,&raw,8);
  const double normalized=std::ldexp(static_cast<double>(source),-32+1);
  const double expected=l.bytes==4?static_cast<double>(static_cast<float>(normalized)):normalized;
  if(actual!=expected)throw std::runtime_error("floating driver sample mismatch");
 } else {
  // Signed reconstruction at container width also validates padded sign extension.
  const auto sign=std::uint64_t{1}<<(l.bytes*8-1);
  const auto actual=(raw&sign)?static_cast<std::int64_t>(raw)-static_cast<std::int64_t>(sign*2):static_cast<std::int64_t>(raw);
  const auto expected=static_cast<std::int64_t>(std::floor(std::ldexp(static_cast<double>(source),static_cast<int>(l.valid)-32)));
  if(actual!=expected)throw std::runtime_error("integer driver sample/sign extension mismatch");
 }
}
inline std::int32_t SourceSample(unsigned bits,std::uint64_t index,unsigned channel) {
 // Quantized source lattice, boundary values plus deterministic changing payload.
 const std::int64_t limit=std::int64_t{1}<<(bits-1);
 const std::int64_t edge[]={-limit,limit-1,0,1,-1,limit/2,-limit/2,limit-2,-limit+1};
 const auto n=index*2+channel;
 const std::int64_t value=n%31<9?edge[n%31]:static_cast<std::int64_t>((n*2654435761ULL)&((std::uint64_t{1}<<bits)-1))-limit;
 return static_cast<std::int32_t>(value*(std::int64_t{1}<<(32-bits)));
}
