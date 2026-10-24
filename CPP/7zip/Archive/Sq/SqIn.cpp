// Reading SquashFS 4.0 images with LZ4 compression.

#include "StdAfx.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"
#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"
#include "7zip/Common/StreamUtils.h"

#include "SqHandler.h"

#include "lz4.h"

#include <string.h>

namespace NArchive {
namespace NSq {

static const unsigned kMaxDepth = 256;

static const Byte kProps[] =
{
  kpidPath,
  kpidIsDir,
  kpidSize,
  kpidPackSize,
  kpidMTime,
  kpidPosixAttrib
};

static const Byte kArcProps[] =
{
  kpidMethod,
  kpidClusterSize
};

IMP_IInArchive_Props
IMP_IInArchive_ArcProps

void CHandler::ClearState()
{
  _stream.Release();
  _h = CSuperBlock();
  _phySize = 0;
  _items.clear();
  _frags.clear();
  _meta.clear();
  _cachedFrag = kNoFragment;
  _fragData.clear();
}

HRESULT CHandler::ReadAt(UInt64 pos, void *data, size_t size)
{
  RINOK(_stream->Seek((Int64)pos, STREAM_SEEK_SET, NULL))
  return ReadStream_FALSE(_stream, data, size);
}

HRESULT CHandler::GetMetaBlock(UInt64 pos, const CMetaBlock *&block)
{
  const auto it = _meta.find(pos);
  if (it != _meta.end())
  {
    block = &it->second;
    return S_OK;
  }
  Byte hdr[2];
  RINOK(ReadAt(pos, hdr, 2))
  const UInt32 packSize = Get16(hdr) & ~kMetaUncompressed;
  if (packSize == 0 || packSize > kMetaSize)
    return S_FALSE;
  std::vector<Byte> packed(packSize);
  RINOK(ReadAt(pos + 2, packed.data(), packSize))
  CMetaBlock mb;
  mb.Next = pos + 2 + packSize;
  if (Get16(hdr) & kMetaUncompressed)
    mb.Data.swap(packed);
  else
  {
    mb.Data.resize(kMetaSize);
    const int n = LZ4_decompress_safe((const char *)packed.data(), (char *)mb.Data.data(), (int)packSize, (int)kMetaSize);
    if (n <= 0)
      return S_FALSE;
    mb.Data.resize((size_t)n);
  }
  block = &(_meta[pos] = std::move(mb));
  return S_OK;
}

// Metadata may span several blocks; (block, offset) is advanced past the read bytes.
HRESULT CHandler::ReadMeta(UInt64 &block, UInt32 &offset, void *data, size_t size)
{
  Byte *dest = (Byte *)data;
  while (size != 0)
  {
    const CMetaBlock *mb;
    RINOK(GetMetaBlock(block, mb))
    if (offset > mb->Data.size())
      return S_FALSE;
    if (offset == mb->Data.size())
    {
      block = mb->Next;
      offset = 0;
      continue;
    }
    size_t n = mb->Data.size() - offset;
    if (n > size)
      n = size;
    memcpy(dest, mb->Data.data() + offset, n);
    dest += n;
    size -= n;
    offset += (UInt32)n;
  }
  return S_OK;
}

static UString NameToUnicode(const AString &name)
{
  UString u;
  if (ConvertUTF8ToUnicode(name, u))
    return u;
  return MultiByteToUnicodeString(name, 1251); // names written by "sq (OGSR)"
}

HRESULT CHandler::ReadInode(UInt64 ref, const UString &path, unsigned depth)
{
  if (depth > kMaxDepth)
    return S_FALSE;
  UInt64 block = _h.InodeTable + (ref >> 16);
  UInt32 offset = (UInt32)(ref & 0xFFFF);
  Byte p[40];
  RINOK(ReadMeta(block, offset, p, 16))
  const UInt16 type = Get16(p);
  CItem item;
  item.Path = path;
  item.Mode = Get16(p + 2) & 07777;
  item.MTime = Get32(p + 8);

  if (type == kTypeDir || type == kTypeLDir)
  {
    UInt32 startBlock, fileSize, dirOffset;
    if (type == kTypeDir)
    {
      RINOK(ReadMeta(block, offset, p, 16))
      startBlock = Get32(p);
      fileSize = Get16(p + 8);
      dirOffset = Get16(p + 10);
    }
    else
    {
      RINOK(ReadMeta(block, offset, p, 24))
      fileSize = Get32(p + 4);
      startBlock = Get32(p + 8);
      dirOffset = Get16(p + 18);
    }
    item.IsDir = true;
    if (depth != 0)
      _items.push_back(item);
    return fileSize > 3 ? ReadDir(startBlock, dirOffset, fileSize - 3, path, depth) : S_OK;
  }

  if (type != kTypeFile && type != kTypeLFile)
    return S_OK; // symlinks and special files are not part of game archives

  if (type == kTypeFile)
  {
    RINOK(ReadMeta(block, offset, p, 16))
    item.BlocksStart = Get32(p);
    item.Frag = Get32(p + 4);
    item.FragOffset = Get32(p + 8);
    item.Size = Get32(p + 12);
  }
  else
  {
    RINOK(ReadMeta(block, offset, p, 40))
    item.BlocksStart = Get64(p);
    item.Size = Get64(p + 8);
    item.Frag = Get32(p + 28);
    item.FragOffset = Get32(p + 32);
  }
  UInt64 numBlocks = item.Size >> _h.BlockLog;
  if (item.Frag == kNoFragment && (item.Size & (_h.BlockSize - 1)) != 0)
    numBlocks++;
  if (item.Frag != kNoFragment && item.Frag >= _frags.size())
    return S_FALSE;
  if (numBlocks > (_phySize >> 1) + 1)
    return S_FALSE;
  item.Blocks.resize((size_t)numBlocks);
  for (size_t i = 0; i < item.Blocks.size(); i++)
  {
    RINOK(ReadMeta(block, offset, p, 4))
    item.Blocks[i] = Get32(p);
    item.PackSize += item.Blocks[i] & kBlockSizeMask;
  }
  _items.push_back(std::move(item));
  return S_OK;
}

HRESULT CHandler::ReadDir(UInt32 startBlock, UInt32 offset, UInt32 size, const UString &path, unsigned depth)
{
  std::vector<Byte> buf(size);
  UInt64 block = _h.DirTable + startBlock;
  RINOK(ReadMeta(block, offset, buf.data(), size))

  struct CEntry { UInt64 Ref; UString Path; };
  std::vector<CEntry> entries;
  size_t pos = 0;
  while (pos < size)
  {
    if (size - pos < 12)
      return S_FALSE;
    const UInt32 count = Get32(&buf[pos]) + 1;
    const UInt32 inodeBlock = Get32(&buf[pos + 4]);
    pos += 12;
    for (UInt32 i = 0; i < count; i++)
    {
      if (size - pos < 8)
        return S_FALSE;
      const UInt32 nameLen = (UInt32)Get16(&buf[pos + 6]) + 1;
      const UInt64 ref = ((UInt64)inodeBlock << 16) | Get16(&buf[pos]);
      pos += 8;
      if (size - pos < nameLen)
        return S_FALSE;
      AString name;
      name.SetFrom((const char *)&buf[pos], nameLen);
      pos += nameLen;
      UString u = NameToUnicode(name);
      if (u.IsEmpty() || u == L"." || u == L".." || u.Find(L'/') >= 0)
        return S_FALSE;
      CEntry e;
      e.Ref = ref;
      e.Path = path.IsEmpty() ? u : path + L'/' + u;
      entries.push_back(e);
    }
  }
  for (const CEntry &e : entries)
  {
    RINOK(ReadInode(e.Ref, e.Path, depth + 1))
  }
  return S_OK;
}

HRESULT CHandler::ReadFragmentTable()
{
  if (_h.NumFrags == 0)
    return S_OK;
  if (_h.NumFrags > (1u << 24))
    return S_FALSE;
  const UInt32 perBlock = kMetaSize / 16;
  const UInt32 numIndexes = (_h.NumFrags + perBlock - 1) / perBlock;
  std::vector<Byte> index((size_t)numIndexes * 8);
  RINOK(ReadAt(_h.FragTable, index.data(), index.size()))
  _frags.resize(_h.NumFrags);
  for (UInt32 i = 0; i < _h.NumFrags; i++)
  {
    UInt64 block = Get64(&index[(i / perBlock) * 8]);
    UInt32 offset = (i % perBlock) * 16;
    Byte p[16];
    RINOK(ReadMeta(block, offset, p, 16))
    _frags[i].Start = Get64(p);
    _frags[i].Size = Get32(p + 8);
  }
  return S_OK;
}

HRESULT CHandler::Open2()
{
  Byte p[kSuperBlockSize];
  RINOK(ReadAt(0, p, kSuperBlockSize))
  if (!ParseSuperBlock(p, _h))
    return S_FALSE;
  UInt64 fileSize;
  RINOK(_stream->Seek(0, STREAM_SEEK_END, &fileSize))
  if (_h.BytesUsed > fileSize)
    return S_FALSE;
  // mksquashfs pads the image to 4 KiB; the padding belongs to the archive.
  _phySize = _h.BytesUsed;
  const UInt64 padded = (_phySize + 4095) & ~(UInt64)4095;
  if (padded <= fileSize)
    _phySize = padded;
  RINOK(ReadFragmentTable())
  return ReadInode(_h.RootInode, UString(), 0);
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *))
{
  COM_TRY_BEGIN
  Close();
  _stream = stream;
  const HRESULT res = Open2();
  if (res != S_OK)
    ClearState();
  return res;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  ClearState();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = (UInt32)_items.size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NWindows::NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidMethod:
    {
      if (!_stream)
        break;
      AString s("LZ4");
      if (_h.Flags & kFlagCompOptions)
      {
        UInt64 block = kSuperBlockSize;
        UInt32 offset = 0;
        Byte opt[8];
        if (ReadMeta(block, offset, opt, 8) == S_OK && (Get32(opt + 4) & kLz4FlagHc))
          s += " HC";
      }
      prop = s;
      break;
    }
    case kpidClusterSize: if (_stream) prop = _h.BlockSize; break;
    case kpidPhySize: if (_stream) prop = _phySize; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NWindows::NCOM::CPropVariant prop;
  const CItem &item = _items[index];
  switch (propID)
  {
    case kpidPath:
    {
      UString path = item.Path;
      path.Replace(L'/', WCHAR_PATH_SEPARATOR);
      prop = path;
      break;
    }
    case kpidIsDir: prop = item.IsDir; break;
    case kpidSize: if (!item.IsDir) prop = item.Size; break;
    case kpidPackSize: if (!item.IsDir) prop = item.PackSize; break;
    case kpidMTime: PropVariant_SetFrom_UnixTime(prop, item.MTime); break;
    case kpidPosixAttrib: prop = (UInt32)(item.Mode | (item.IsDir ? kModeDir : kModeFile)); break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

// Decodes block (blockIndex) of a file; pos is the packed position of that block and is advanced.
HRESULT CHandler::ReadBlock(const CItem &item, size_t blockIndex, UInt64 &pos, std::vector<Byte> &out, bool &dataError)
{
  const UInt64 done = (UInt64)blockIndex << _h.BlockLog;
  UInt64 want = item.Size - done;
  if (want > _h.BlockSize)
    want = _h.BlockSize;
  out.resize((size_t)want);
  const UInt32 entry = item.Blocks[blockIndex];
  const UInt32 packSize = entry & kBlockSizeMask;
  if (packSize == 0)
  {
    memset(out.data(), 0, out.size()); // sparse block
    return S_OK;
  }
  if (packSize > _h.BlockSize)
  {
    dataError = true;
    return S_OK;
  }
  std::vector<Byte> packed(packSize);
  const HRESULT res = ReadAt(pos, packed.data(), packSize);
  pos += packSize;
  if (res == S_FALSE)
  {
    dataError = true;
    return S_OK;
  }
  RINOK(res)
  if (entry & kBlockUncompressed)
  {
    if (packSize != want)
      dataError = true;
    else
      memcpy(out.data(), packed.data(), packSize);
    return S_OK;
  }
  const int n = LZ4_decompress_safe((const char *)packed.data(), (char *)out.data(), (int)packSize, (int)want);
  if (n != (int)want)
    dataError = true;
  return S_OK;
}

HRESULT CHandler::ReadTail(const CItem &item, std::vector<Byte> &out, bool &dataError)
{
  const UInt64 tail = item.TailSize(_h.BlockSize);
  out.clear();
  if (tail == 0)
    return S_OK;
  if (_cachedFrag != item.Frag)
  {
    _cachedFrag = kNoFragment;
    const CFragment &f = _frags[item.Frag];
    const UInt32 packSize = f.Size & kBlockSizeMask;
    if (packSize == 0 || packSize > _h.BlockSize)
    {
      dataError = true;
      return S_OK;
    }
    std::vector<Byte> packed(packSize);
    const HRESULT res = ReadAt(f.Start, packed.data(), packSize);
    if (res == S_FALSE)
    {
      dataError = true;
      return S_OK;
    }
    RINOK(res)
    if (f.Size & kBlockUncompressed)
      _fragData.swap(packed);
    else
    {
      _fragData.resize(_h.BlockSize);
      const int n = LZ4_decompress_safe((const char *)packed.data(), (char *)_fragData.data(), (int)packSize, (int)_h.BlockSize);
      if (n <= 0)
      {
        dataError = true;
        return S_OK;
      }
      _fragData.resize((size_t)n);
    }
    _cachedFrag = item.Frag;
  }
  if ((UInt64)item.FragOffset + tail > _fragData.size())
  {
    dataError = true;
    return S_OK;
  }
  out.assign(_fragData.begin() + item.FragOffset, _fragData.begin() + (size_t)(item.FragOffset + tail));
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFiles = (numItems == (UInt32)(Int32)-1);
  if (allFiles)
    numItems = (UInt32)_items.size();
  if (numItems == 0)
    return S_OK;

  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
    total += _items[allFiles ? i : indices[i]].Size;
  RINOK(extractCallback->SetTotal(total))

  UInt64 completed = 0;
  std::vector<Byte> buf;
  for (UInt32 i = 0; i < numItems; i++)
  {
    RINOK(extractCallback->SetCompleted(&completed))
    const UInt32 index = allFiles ? i : indices[i];
    const CItem &item = _items[index];
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    CMyComPtr<ISequentialOutStream> outStream;
    RINOK(extractCallback->GetStream(index, &outStream, askMode))
    if (!testMode && !outStream)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    if (item.IsDir)
    {
      RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK))
      continue;
    }

    bool dataError = false;
    UInt64 pos = item.BlocksStart;
    for (size_t b = 0; b < item.Blocks.size() && !dataError; b++)
    {
      RINOK(ReadBlock(item, b, pos, buf, dataError))
      if (!dataError && outStream)
        RINOK(WriteStream(outStream, buf.data(), buf.size()))
      completed += buf.size();
      RINOK(extractCallback->SetCompleted(&completed))
    }
    if (!dataError)
    {
      RINOK(ReadTail(item, buf, dataError))
      if (!dataError && outStream)
        RINOK(WriteStream(outStream, buf.data(), buf.size()))
      completed += buf.size();
    }
    outStream.Release();
    RINOK(extractCallback->SetOperationResult(dataError ?
        NExtract::NOperationResult::kDataError :
        NExtract::NOperationResult::kOK))
  }
  return S_OK;
  COM_TRY_END
}

}}
