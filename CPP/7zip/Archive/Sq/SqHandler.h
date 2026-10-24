#pragma once

#include "Common/MyCom.h"
#include "Common/MyString.h"

#include "Archive/IArchive.h"

#include "SqFormat.h"

#include <map>
#include <vector>

namespace NArchive {
namespace NSq {

struct CItem
{
  UString Path; // '/' separated
  bool IsDir = false;
  UInt32 Mode = 0; // permission bits only
  UInt32 MTime = 0;
  UInt64 Size = 0;
  UInt64 PackSize = 0;
  UInt64 BlocksStart = 0;
  UInt32 Frag = kNoFragment;
  UInt32 FragOffset = 0;
  std::vector<UInt32> Blocks;

  UInt64 TailSize(UInt32 blockSize) const
    { return Frag == kNoFragment ? 0 : Size - (UInt64)Blocks.size() * blockSize; }
};

struct CFragment
{
  UInt64 Start = 0;
  UInt32 Size = 0; // with kBlockUncompressed flag
};

struct CMetaBlock
{
  std::vector<Byte> Data;
  UInt64 Next = 0;
};

Z7_CLASS_IMP_CHandler_IInArchive_2(ISetProperties, IOutArchive)

  CMyComPtr<IInStream> _stream;
  CSuperBlock _h;
  UInt64 _phySize = 0;
  std::vector<CItem> _items;
  std::vector<CFragment> _frags;
  std::map<UInt64, CMetaBlock> _meta;

  // Cache of the last decoded fragment block used by extraction and update.
  UInt32 _cachedFrag = kNoFragment;
  std::vector<Byte> _fragData;

  // "sq (OGSR)": the archive root is the game's gamedata folder (see SqOut.cpp).
  bool _ogsr = false;

  // Options for new archives.
  int _level = 9;
  UInt32 _blockLog = 17;
  bool _blockLogSet = false; // otherwise an opened archive keeps its block size
  bool _useFrags = true;

  HRESULT ReadAt(UInt64 pos, void *data, size_t size);
  HRESULT GetMetaBlock(UInt64 pos, const CMetaBlock *&block);
  HRESULT ReadMeta(UInt64 &block, UInt32 &offset, void *data, size_t size);
  HRESULT ReadInode(UInt64 ref, const UString &path, unsigned depth);
  HRESULT ReadDir(UInt32 startBlock, UInt32 offset, UInt32 size, const UString &path, unsigned depth);
  HRESULT ReadFragmentTable();
  HRESULT Open2();
  void ClearState();

public:
  explicit CHandler(bool ogsr = false): _ogsr(ogsr) {}

  // Reads the unpacked data of an item block by block; used by Extract and UpdateItems.
  HRESULT ReadBlock(const CItem &item, size_t blockIndex, UInt64 &pos, std::vector<Byte> &out, bool &dataError);
  HRESULT ReadTail(const CItem &item, std::vector<Byte> &out, bool &dataError);
  const CItem &Item(UInt32 index) const { return _items[index]; }
  UInt32 BlockSize() const { return _h.BlockSize; }
  bool IsOpen() const { return _stream != nullptr; }
  IInStream *Stream() const { return _stream; }
};

}}
