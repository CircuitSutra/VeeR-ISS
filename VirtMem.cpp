#include <cmath>
#include "VirtMem.hpp"

using namespace WdRiscv;


VirtMem::VirtMem(unsigned hartIx, Memory& memory, unsigned pageSize,
                 unsigned tlbSize)
  : memory_(memory), mode_(Sv32), pageSize_(pageSize), hartIx_(hartIx),
    tlb_(tlbSize)
{
  pageBits_ = static_cast<unsigned>(std::log2(pageSize_));
  unsigned p2PageSize =  unsigned(1) << pageBits_;

  assert(p2PageSize == pageSize);
  assert(pageSize >= 64);

  pageMask_ = pageSize_ - 1;
}


inline
ExceptionCause
pageFaultType(bool read, bool write, bool exec)
{
  if (exec)  return ExceptionCause::INST_PAGE_FAULT;
  if (read)  return ExceptionCause::LOAD_PAGE_FAULT;
  if (write) return ExceptionCause::STORE_PAGE_FAULT;
  assert(0);
  return ExceptionCause::STORE_PAGE_FAULT;
}


ExceptionCause
VirtMem::translate(size_t va, PrivilegeMode priv, bool read, bool write,
                   bool exec, size_t& pa)
{
  if (mode_ == Bare)
    {
      pa = va;
      return ExceptionCause::NONE;
    }

  // Lookup virtual page number in TLB.
  size_t virPageNum = va >> pageBits_;
  TlbEntry* entry = tlb_.findEntry(virPageNum, asid_);
  if (entry)
    {
      // Use TLB entry.
      if (priv == PrivilegeMode::User and not entry->user_)
        return pageFaultType(read, write, exec);
      if (priv == PrivilegeMode::Supervisor and entry->user_ and not supervisorOk_)
        return pageFaultType(read, write, exec);
      bool entryRead = entry->read_ or (execReadable_ and entry->exec_);
      if ((read and not entryRead) or (write and not entry->write_) or
          (exec and not entry->exec_))
        return pageFaultType(read, write, exec);
      if (not entry->accessed_ or (write and not entry->dirty_))
        {
          if (faultOnFirstAccess_)
            return pageFaultType(read, write, exec);
          entry->accessed_ = true;
          if (write)
            entry->dirty_ = true;
        }
      pa = (entry->physPageNum_ << pageBits_) | (va & pageMask_);
      return ExceptionCause::NONE;
    }

  // Perform a page table walk.
  ExceptionCause cause = ExceptionCause::LOAD_PAGE_FAULT;
  TlbEntry tmpTlbEntry;

  if (mode_ == Sv32)
    cause = pageTableWalk<Pte32, Va32>(va, priv, read, write, exec, pa, tmpTlbEntry);
  else if (mode_ == Sv39)
    {
      // Part 1 of address translation: Bits 63-39 must equal bit 38
      uint64_t mask = (va >> 38) & 1;
      if (mask)
        mask = 0x1ffffff;  // Least sig 25 bits set
      if ((va >> 39) != mask)
        return pageFaultType(read, write, exec);
      cause = pageTableWalk<Pte39, Va39>(va, priv, read, write, exec, pa, tmpTlbEntry);
    }
  else if (mode_ == Sv48)
    {
      // Part 1 of address translation: Bits 63-47 muse equal bit 47
      uint64_t mask = (va >> 47) & 1;
      if (mask)
        mask = 0xffff;  // Least sig 16 bits set
      if ((va >> 48) != mask)
        return pageFaultType(read, write, exec);
      cause = pageTableWalk<Pte48, Va48>(va, priv, read, write, exec, pa, tmpTlbEntry);
    }
  else
    assert(0 and "Unspupported virtual memory mode.");

  // If successful, cache translation results in TLB.
  if (cause == ExceptionCause::NONE)
    tlb_.insertEntry(tmpTlbEntry);

  return cause;
}


template<typename PTE, typename VA>
ExceptionCause
VirtMem::pageTableWalk(size_t address, PrivilegeMode privMode, bool read, bool write,
                       bool exec, size_t& pa, TlbEntry& tlbEntry)
{
  // 1. Done in translate method.

  PTE pte(0);

  const unsigned levels = pte.levels();
  const unsigned pteSize = pte.size();

  VA va(address);

  // 2. Root is "a" in section 4.3.2 of privileged spec.
  uint64_t root = pageTableRootPage_ * pageSize_;
  uint64_t pteAddr = 0;
  int ii = levels - 1;

  while (true)
    {
      // 3.
      uint32_t vpn = va.vpn(ii);
      uint64_t pteAddr = root + vpn*pteSize;

      // TBD: Check pmp
      if (! memory_.read(pteAddr, pte.data_))
        return pageFaultType(read, write, exec);

      // 4.
      if (not pte.valid() or (not pte.read() and pte.write()))
        return pageFaultType(read, write, exec);

      // 5.
      if (not pte.read() and not pte.exec())
        {
          ii = ii - 1;
          if (ii < 0)
            return pageFaultType(read, write, exec);
          root = pte.ppn() * pageSize_;
          // goto 3.
        }
      else
        break;  // goto 6.
    }

  // 6.  pte.read_ or pte.exec_ : leaf pte
  if (privMode == PrivilegeMode::User and not pte.user())
    return pageFaultType(read, write, exec);
  if (privMode == PrivilegeMode::Supervisor and pte.user() and
      not supervisorOk_)
    return pageFaultType(read, write, exec);

  bool pteRead = pte.read() or (execReadable_ and pte.exec());
  if ((read and not pteRead) or (write and not pte.write()) or
      (exec and pte.exec()))
    return pageFaultType(read, write, exec);

  // 7.
  for (int j = 0; j < ii; ++j)
    if (pte.ppn(j) != 0)
      return pageFaultType(read, write, exec);

  // 8.
  if (not pte.accessed() or (write and not pte.dirty()))
    {
      // We have a choice:
      // A. Page fault
      if (faultOnFirstAccess_)
        return pageFaultType(read, write, exec);  // A

      // Or B
      // B1. Set pte->accessed_ to 1 and, if a write, set pte->dirty_ to 1.
      // B2. Access fault if PMP violation.
      pte.bits_.accessed_ = 1;
      if (write)
        pte.bits_.dirty_ = 1;

      // TBD: Check pmp
      if (not memory_.write(hartIx_, pteAddr, pte.data_))
        return pageFaultType(read, write, exec);
    }

  // 9.
  pa = va.offset();

  for (int j = 0; j < ii; ++j)
    pa = pa | (va.vpn(j) << pte.paPpnShift(j)); // Copy from va to pa

  for (unsigned j = ii; j < levels; ++j)
    pa = pa | pte.ppn(j) << pte.paPpnShift(j);

  // Update tlb-entry with data found in page table entry.
  tlbEntry.virtPageNum_ = address >> pageBits_;
  tlbEntry.physPageNum_ = pa >> pageBits_;
  tlbEntry.asid_ = asid_;
  tlbEntry.valid_ = true;
  tlbEntry.global_ = pte.global();
  tlbEntry.user_ = pte.user();
  tlbEntry.read_ = pte.read();
  tlbEntry.write_ = pte.write();
  tlbEntry.exec_ = pte.exec();
  tlbEntry.accessed_ = pte.accessed();
  tlbEntry.dirty_ = pte.dirty();

  return ExceptionCause::NONE;
}


bool
VirtMem::setPageSize(uint64_t size)
{
  if (size == 0)
    return false;

  unsigned bits = static_cast<unsigned>(std::log2(pageSize_));
  uint64_t p2Size =  uint64_t(1) << bits;

  if (size != p2Size)
    return false;
  
  if (mode_ == Sv32)
    {
      if (size != 4096)
        return false;
      pageBits_ = bits;
      pageSize_ = size;
      return true;
    }

  if (mode_ == Sv39)
    {
      if (size != 4096 and size != 2*1024*1024 and size != 1024*1024*1024)
        return false;
      pageBits_ = bits;
      pageSize_ = size;
      return true;
    }

  if (mode_ == Sv48)
    {
      if (size != 4096 and size != 2*1024*1024 and size != 1024*1024*1024
          and size != 512L*1024L*1024L*1024L)
        return false;
      pageBits_ = bits;
      pageSize_ = size;
    }

  assert(0 && "Translation modes Sv57 and Sv64 are not currently supported");
  return false;
}
