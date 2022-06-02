#ifndef _MEMORYMANAGER_HH_
#define _MEMORYMANAGER_HH_

#include <mutex>
#include <vector>
#include <numeric>
#include <tuple>
#include <bitset>
#include <memory>
#include <cmath>

/* A custom memory manager because I feel like writing one. Memory is organized into
 * pages, which is further subdivided into blocks. Allocs are tracked via a bitmask,
 * where each bit corresponds to each block in a page. Multi-block allocs are supported
 * I envision two MMs, one to do strax fragments and one to do transfer buffers, where
 * each StraxFormatter owns its own MM.
 * Maybe eventually a third for output buffers but this is a bit harder
 */

class FragmentManager {
  public:
    FragmentManager();
    virtual ~FragmentManager();

    virtual void Init(int, int);
    virtual void* allocate(size_t=0);
    virtual void release(void*);

    long GetCurrentUsage() {return std::accumulate(fPages.begin(), fPages.end(), 0l, [&](long tot, Page& p){return std::move(tot) + p.bitmask.sum()*fBytesPerBlock;});}
    long GetCurrentAllocation(){return fPages.size() * fBytesPerBlock;}

  protected:
    virtual void AllocateNewPage();
    virtual void* GetBlockFromPage(int, int, int=0);
    virtual inline int AllocHeaderSize() {return 2*sizeof(int);}

    int fBytesPerBlock;
    int fBlocksPerPage;
    size_t fBytesPerPage;

    struct Page {
      Page(std::unique_ptr<char[]>&& addr_, size_t bytes_, int bits) : addr(std::move(addr_)), bytes(bytes_), bitmask(bits) {}
      std::unique_ptr<char[]> addr;
      size_t bytes;

      struct Bitmask {
        // define here because we need run-time size
        // also stores which blocks are available, not which are used
        using WORD=unsigned long;
        Bitmask(int bits=128) : fWords(std::ceil(0.125*bits/sizeof(WORD)), 0) {}
        inline int sum() {return std::accumulate(fWords.begin(), fWords.end(), 0,
            [](int tot, WORD word)
            {return std::move(tot)+std::bitset<8*sizeof(WORD)>(word).count();});}
        inline bool any() {for (auto& w : fWords) if (w) return true; return false;}
        inline bool avail(int i) {return fWords[i/sizeof(WORD)] & (1 << (i%sizeof(WORD)));}
        inline void claim(int i) {fWords[i/sizeof(WORD)] &= ~(1 << (i%sizeof(WORD)));}
        inline void clear(int i) {fWords[i/sizeof(WORD)] |= (1 << (i%sizeof(WORD)));}
        std::tuple<int, int> find_space(unsigned int);

        private:
          std::vector<WORD> fWords;
      };
      Bitmask bitmask;
    };
    std::vector<Page> fPages;
};

// this class does the transfer buffers. A bit more hassle because it's multithreaded
// and also supports allocs that span blocks. We add an extra int to the header to 
// hold the number of blocks that alloc consumes. The first int also holds a 
// control nibble so we can check for overruns.
class XferManager : public FragmentManager {
  public:
    XferManager() {}
    virtual ~XferManager() {}

    virtual void* allocate(size_t);
    virtual void release(void*);

  protected:
    virtual void* GetBlockFromPage(int, int, int);
    virtual inline int AllocHeaderSize() {return 3*sizeof(int);}

  private:
    std::mutex fMutex;
    const unsigned char CONTROL_NIBBLE = 0xA;
};

#endif // _MEMORYMANAGER_HH_ defined
