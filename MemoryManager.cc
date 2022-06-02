#include "MemoryManager.hh"
#include <cstring>
#include <iostream>


FragmentManager::FragmentManager() {
  fPages.reserve(4);
}

FragmentManager::~FragmentManager() {
}

void FragmentManager::Init(int bytes_per_block, int blocks_per_page) {
  // this can't be in the ctor because we need to call AllocHeaderSize
  fBytesPerBlock = bytes_per_block;
  fBlocksPerPage = blocks_per_page;
  fBytesPerPage = 1l * (bytes_per_block + AllocHeaderSize()) * blocks_per_page;
  AllocateNewPage();
}

void FragmentManager::AllocateNewPage() {
  fPages.emplace_back(std::make_unique<char[]>(fBytesPerPage), fBytesPerPage, fBlocksPerPage);
}

void* FragmentManager::allocate(size_t) {
  for (unsigned p = 0; p < fPages.size(); ++p) {
    //if (fPages[p].bitmask.any())
    for (int i = 0; i < fBlocksPerPage; ++i)
      if (fPages[p].bitmask.avail(i)) {
        return GetBlockFromPage(p, i, 1);
      }
  }
  // no space in existing pages, make a new one
  AllocateNewPage();
  return GetBlockFromPage(fPages.size()-1, 0, 1);
}

void* FragmentManager::GetBlockFromPage(int page_i, int bit, int) {
  fPages[page_i].bitmask.claim(bit);
  char* addr = fPages[page_i].addr.get() + bit*(fBytesPerBlock+AllocHeaderSize());
  // set control words
  std::memcpy(addr, &page_i, sizeof(page_i));
  std::memcpy(addr+sizeof(page_i), &bit, sizeof(bit));
  return (void*)(addr + AllocHeaderSize());
}

void FragmentManager::release(void* release_me) {
  char* header = (char*)release_me - AllocHeaderSize();
  int page = *(int*)(header);
  int start_bit = *(int*)(header + sizeof(int));
  fPages[page].bitmask.clear(start_bit);
  return;
}

std::tuple<int,int> FragmentManager::Page::Bitmask::find_space(unsigned int span) {
  if (span >= 8*sizeof(unsigned long)) throw std::runtime_error("Can\'t find span of " + std::to_string(span));
  unsigned long mask = (1l<<span)-1; // eg 0b111 for span=3, 0b11 for span=2
  for (unsigned w = 0; w < fWords.size(); ++w)
    for (unsigned i = 0; i < 8*sizeof(unsigned long); ++i)
      if ((mask & (fWords[w] >> i)) == mask)
        return {w, i};
  return {-1, -1};
}

void* XferManager::allocate(size_t bytes) {
  const std::lock_guard<std::mutex> lg(fMutex);
  int num_blocks = std::ceil(1.*bytes/fBytesPerBlock);
  int page = -1, start_bit = -1;
  for (unsigned p = 0; p < fPages.size(); ++p) {
    std::tie(page, start_bit) = fPages[p].bitmask.find_space(num_blocks);
    if (page == -1 || start_bit == -1) continue;
    return GetBlockFromPage(page, start_bit, num_blocks);
  }
  // didn't find space, make a new page
  AllocateNewPage();
  return GetBlockFromPage(fPages.size()-1, 0, num_blocks);
}

void* XferManager::GetBlockFromPage(int page, int start_bit, int blocks) {
  char* addr = fPages[page].addr.get() + start_bit * (fBytesPerBlock + AllocHeaderSize());
  for (int i = 0; i < blocks; ++i) fPages[page].bitmask.claim(start_bit+i);
  // copy control words
  int shift_by = 8*sizeof(int)-4; // access the most significant nibble
  blocks |= (CONTROL_NIBBLE << shift_by); // assuming allocs aren't THAT large
  std::memcpy(addr, &blocks, sizeof(int));
  std::memcpy(addr+sizeof(size_t), &page, sizeof(int));
  std::memcpy(addr+sizeof(size_t)+sizeof(int), &start_bit, sizeof(int));
  return (void*)(addr + AllocHeaderSize());
}

void XferManager::release(void* release_me) {
  const std::lock_guard<std::mutex> lg(fMutex);
  unsigned int* header = (unsigned int*)((char*)release_me - AllocHeaderSize());
  unsigned int blocks = header[0];
  int shift_by = 8*sizeof(int)-4;
  if ((blocks >> shift_by) != CONTROL_NIBBLE)
    throw std::runtime_error("Memory corruption detected");
  blocks &= ~(CONTROL_NIBBLE << shift_by); // strip control nibble
  int page = header[1];
  int bit = header[2];
  for (unsigned i = 0; i < blocks; ++i) fPages[page].bitmask.clear(bit+i);
  return;
}

