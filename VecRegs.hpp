// Copyright 2020 Western Digital Corporation or its affiliates.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <cassert>

namespace WdRiscv
{

  enum class GroupMultiplier : uint32_t
    {
     One      = 0,
     Two      = 1,
     Four     = 2,
     Eight    = 3,
     Reserved = 4,
     Eighth   = 5,
     Quarter  = 6,
     Half     = 7
    };


  /// Selected element width.
  enum class ElementWidth : uint32_t
    {
     Byte         = 0,
     HalfWord     = 1,
     Word         = 2,
     DoubleWord   = 3,
     QuadWord     = 4,
     OctWord      = 5,
     HalfKbits    = 6,
     Kbits        = 7
    };


  enum class VecEnums : uint32_t
    {
     GroupLimit = 8, // One past largest VecGroupMultiplier value
     WidthLimit = 8  // One past largest ElementWidth value
    };

  /// Symbolic names of the integer registers.
  enum VecRegNumber
    {
     RegV0 = 0,
     RegV1 = 1,
     RegV2 = 2,
     RegV3 = 3,
     RegV4 = 4,
     RegV5 = 5,
     RegV6 = 6,
     RegV7 = 7,
     RegV8 = 8,
     RegV9 = 9,
     RegV10 = 10,
     RegV11 = 11,
     RegV12 = 12,
     RegV13 = 13,
     RegV14 = 14,
     RegV15 = 15,
     RegV16 = 16,
     RegV17 = 17,
     RegV18 = 18,
     RegV19 = 19,
     RegV20 = 20,
     RegV21 = 21,
     RegV22 = 22,
     RegV23 = 23,
     RegV24 = 24,
     RegV25 = 25,
     RegV26 = 26,
     RegV27 = 27,
     RegV28 = 28,
     RegV29 = 29,
     RegV30 = 30,
     RegV31 = 31
    };


  template <typename URV>
  class Hart;

  /// Model a RISCV vector register file.
  class VecRegs
  {
  public:

    friend class Hart<uint32_t>;
    friend class Hart<uint64_t>;

    /// Constructor: Define an empty vector regidter file which may be
    /// reconfigured later using the config method.
    VecRegs();

    /// Destructor.
    ~VecRegs();
    
    /// Return vector register count.
    unsigned registerCount() const
    { return regCount_; }

    /// Return the number of bytes per register. This is independent
    /// of group multiplier.
    unsigned bytesPerRegister() const
    { return bytesPerReg_; }

    /// Set value to that of the element with given index within the
    /// vector register of the given number returning true on sucess
    /// and false if the combination of element index, vector number
    /// and group multipier (presecaled by 8) is invalid. We pre-scale
    /// the group multiplier to avoid passing a fraction.
    template<typename T>
    bool read(unsigned regNum, unsigned elemIx, unsigned groupX8,
              T& value) const
    {
      if ((elemIx + 1) * sizeof(T) > ((bytesPerReg_*groupX8) >> 3))
        return false;
      if (regNum*bytesPerReg_ + (elemIx + 1)*sizeof(T) > bytesPerReg_*regCount_)
        return false;
      const T* data = reinterpret_cast<const T*>(data_ + regNum*bytesPerReg_);
      value = data[elemIx];
      return true;
    }

    /// Set the element with given index within the vector register of
    /// the given number to the given value returning true on sucess
    /// and false if the combination of element index, vector number
    /// and group multipier (presecaled by 8) is invalid. We pre-scale
    /// the group multiplier to avoid passing a fraction.
    template<typename T>
    bool write(unsigned regNum, unsigned elemIx, unsigned groupX8,
               const T& value)
    {
      if ((elemIx + 1) * sizeof(T) > ((bytesPerReg_*groupX8) >> 3))
        return false;
      if (regNum*bytesPerReg_ + (elemIx + 1)*sizeof(T) > bytesPerReg_*regCount_)
        return false;
      T* data = reinterpret_cast<T*>(data_ + regNum*bytesPerReg_);
      data[elemIx] = value;
      lastWrittenReg_ = regNum;
      lastElemWidth_ = sizeof(T)*8;
      lastElemIx_ = elemIx;
      return true;
    }

    /// Return the count of registers in this register file.
    size_t size() const
    { return regCount_; }

    /// Return the number of bits in a register in this register file.
    uint32_t bitsPerReg() const
    { return 8*bytesPerReg_; }

    /// Return the currently configured element width.
    ElementWidth elemWidth() const
    { return sew_; }

    /// Return the current configured group multiplier.
    GroupMultiplier groupMultiplier() const
    { return group_; }

    /// Return the currently configured element width in bits (for example
    /// if SEW is Byte, then this reurns 8).
    unsigned elemWidthInBits() const
    { return sewInBits_; }

    /// Return the currently configured group multiplier as a unsigned
    /// integer scaled by 8.  For example if group multiplier is One,
    /// this reurns 8. If group multiplier is Eigth, this returns 1.
    /// We pre-scale by 8 to avoid division when the multiplier is a
    /// fraction.
    unsigned groupMultiplierX8() const
    {return groupX8_; }

    /// Return true if double the given element width (eew=2*sew) is
    /// legal with the given group multiplier (prescaled by 8).
    bool isDoubleWideLegal(ElementWidth sew, unsigned groupX8) const
    {
      unsigned wideGroup = groupX8 * 2;
      GroupMultiplier emul = GroupMultiplier::One;
      if (not groupNumberX8ToSymbol(wideGroup, emul))
        return false;

      ElementWidth eew = sew;
      if (not doubleSew(sew, eew))
        return false;

      return legalConfig(eew, emul);
    }

    /// Return the checksum of the elements of the given register
    /// between the given element indices inclusive.  We checksum the
    /// bytes covered by the given index range. The indices are allowed
    /// to be out of bounds. Indices outside the vector regiter file are
    /// replaced by zero.
    uint64_t checksum(unsigned regIx, unsigned elemIx0, unsigned elemIx1,
                      unsigned elemWidth) const;

    /// Set symbol to the symbolic value of the given numeric group
    /// multiplier (premultiplied by 8). Return true on success and
    /// false if groupX8 is out of bounds.
    static inline
    bool groupNumberX8ToSymbol(unsigned groupX8, GroupMultiplier& symbol)
    {
      if (groupX8 == 1)  { symbol = GroupMultiplier::Eighth;   return true; }
      if (groupX8 == 2)  { symbol = GroupMultiplier::Quarter;  return true; }
      if (groupX8 == 4)  { symbol = GroupMultiplier::Half;     return true; }
      if (groupX8 == 8)  { symbol = GroupMultiplier::One;      return true; }
      if (groupX8 == 16) { symbol = GroupMultiplier::Two;      return true; }
      if (groupX8 == 32) { symbol = GroupMultiplier::Four;     return true; }
      if (groupX8 == 64) { symbol = GroupMultiplier::Eight;    return true; }
      return false;
    }

    /// Set dsew to the double of the given sew returning true on
    /// succes and false if given sew cannot be doubled.  false if
    /// groupX8 is out of bounds.
    static
    bool doubleSew(ElementWidth sew, ElementWidth& dsew)
    {
      typedef ElementWidth EW;
      if (sew == EW::Byte       ) { dsew = EW:: HalfWord;   return true; }
      if (sew == EW::HalfWord   ) { dsew = EW:: Word;       return true; }
      if (sew == EW::Word       ) { dsew = EW:: DoubleWord; return true; }
      if (sew == EW::DoubleWord ) { dsew = EW:: QuadWord;   return true; }
      if (sew == EW::QuadWord   ) { dsew = EW:: OctWord;    return true; }
      if (sew == EW::OctWord    ) { dsew = EW:: HalfKbits;  return true; }
      if (sew == EW::HalfKbits  ) { dsew = EW:: Kbits;      return true; }
      return false;
    }

    /// Convert the given symbolic element width to a byte count.
    static uint32_t elementWidthInBytes(ElementWidth sew)
    { return uint32_t(1) << uint32_t(sew); }

    /// Convert the given symbolic group multiplier to a number scaled by
    /// eight (e.g. One is converted to 8, and Eigth to 1). Return 0 if
    /// given symbolic multiplier is not valid.
    static uint32_t groupMultiplierX8(GroupMultiplier vm)
    {
      if (vm < GroupMultiplier::Reserved)
        return 8*(uint32_t(1) << uint32_t(vm));
      if (vm > GroupMultiplier::Reserved and vm <= GroupMultiplier::Half)
        return 8 >> (8 - unsigned(vm));
      return 0;
    }

    static std::string to_string(GroupMultiplier group)
    {
      static std::vector<std::string> vec =
        {"m1", "m2", "m4", "m8", "m?", "mf8", "mf4", "mf2"};
      return size_t(group) < vec.size()? vec.at(size_t(group)) : "m?";
    }

    static std::string to_string(ElementWidth ew)
    {
      static std::vector<std::string> vec =
        {"e8", "e16", "e32", "e64", "e128", "e256", "e512", "e1024"};
      return size_t(ew) < vec.size()? vec.at(size_t(ew)) : "e?";
    }


  protected:

    /// Clear the number denoting the last written register.
    void clearLastWrittenReg()
    { lastWrittenReg_ = -1; }
    
    /// Return the number of the last written vector regsiter or -1 if no
    /// no register has been written since the last clearLastWrittenReg.
    int getLastWrittenReg(unsigned& lastElemIx, unsigned& lastElemWidth) const
    {
      if (lastWrittenReg_ >= 0)
        {
          lastElemWidth = lastElemWidth_;
          lastElemIx = lastElemIx_;
          return lastWrittenReg_;
        }
      return -1;
    }

    /// For instructions that do not use the write method, mark the
    /// last written register and the effective element widht.
    void setLastWrittenReg(unsigned reg, unsigned lastIx, unsigned elemWidth)
    { lastWrittenReg_ = reg; lastElemIx_ = lastIx; lastElemWidth_ = elemWidth; }

    /// Return true if element of given index is active with respect
    /// to the given mask vector register. Element is active if the
    /// corresponding mask bit is 1.
    bool isActive(unsigned maskReg, unsigned ix) const
    {
      if (maskReg >= regCount_)
        return false;

      unsigned byteIx = ix >> 3;
      unsigned bitIx = ix & 7;  // bit in byte
      if (byteIx >= bytesPerReg_)
        return false;

      const uint8_t* data = data_ + maskReg*bytesPerReg_;
      return (data[byteIx] >> bitIx) & 1;
    }

    /// Return the pointers to the 1st byte of the memory area
    /// associated with the given vector. Return nullptr if
    /// vector index is out of bounds.
    uint8_t* getVecData(unsigned vecIx)
    {
      if (vecIx >= regCount_)
        return nullptr;
      return data_ + vecIx*bytesPerReg_;
    }

    const uint8_t* getVecData(unsigned vecIx) const
    {
      if (vecIx >= regCount_)
        return nullptr;
      return data_ + vecIx*bytesPerReg_;
    }

    /// It is convenient to contruct an empty regiter file (bytesPerReg = 0)
    /// and configure it later. Old configuration is lost. Register of
    /// newly configured file are initlaized to zero.
    void config(unsigned bytesPerReg, unsigned maxBytesPerElem);

    void reset();

    unsigned startIndex() const
    { return start_; }

    unsigned elemCount() const
    { return elems_; }

    /// Return true if current vtype configuration is legal. This is a cached
    /// value of VTYPE.VILL.
    bool legalConfig() const
    { return not vill_; }

    /// Return true if the given element width and grouping
    /// combination is legal.
    bool legalConfig(ElementWidth ew, GroupMultiplier mul) const
    {
      if (size_t(ew) >= legalConfigs_.size()) return false;
      const auto& groupFlags = legalConfigs_.at(size_t(ew));
      if (size_t(mul) >= groupFlags.size()) return false;
      return groupFlags.at(size_t(mul));
    }

  private:

    /// Map an vector group multiplier to a flag indicating whether given
    /// group is supporte.
    typedef std::vector<bool> GroupFlags;

    /// Map an element width to a vector of flags indicating supported groups.
    typedef std::vector<GroupFlags> GroupsForWidth;

    unsigned regCount_ = 0;
    unsigned bytesPerReg_ = 0;
    unsigned bytesPerElem_ = 0;
    unsigned bytesInRegFile_ = 0;
    uint8_t* data_ = nullptr;

    GroupMultiplier group_ = GroupMultiplier::One; // Cached VTYPE.VLMUL
    ElementWidth sew_ = ElementWidth::Byte;        // Cached VTYPE.SEW
    unsigned start_ = 0;                           // Cached VSTART
    unsigned elems_ = 0;                           // Cached VL
    bool vill_ = false;                            // Cached VTYPE.VILL

    unsigned groupX8_ = 8;    // Group multipler as a number scaled by 8.
    unsigned sewInBits_ = 8;  // SEW expressed in bits (Byte corresponds to 8).

    GroupsForWidth legalConfigs_;

    int lastWrittenReg_ = -1;
    unsigned lastElemWidth_ = 0;   // Width (in bits) of last written element.
    unsigned lastElemIx_ = 0;
  };
}
