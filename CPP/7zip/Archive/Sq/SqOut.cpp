// Writing SquashFS 4.0 images with LZ4 compression, laid out like mksquashfs -comp lz4 -Xhc:
// data blocks and fragments, then inode, directory, fragment and id tables.

#include "StdAfx.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Common/StringToInt.h"
#include "Common/UTFConvert.h"
#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"
#include "7zip/Common/MethodProps.h"
#include "7zip/Common/StreamUtils.h"

#include "SqHandler.h"

#include "lz4.h"
#include "lz4hc.h"
#include "Sha256.h"

#include <map>
#include <stdio.h>
#include <string>
#include <time.h>

namespace NArchive {
namespace NSq {

static const UInt32 kDefaultDirMode = 0755;
static const UInt32 kDefaultFileMode = 0644;
static const unsigned kMaxNameLen = 256;

class CCompressor
{
  int _level = 0;
  std::vector<UInt64> _state;
  std::vector<Byte> _dst;
public:
  void Init(int level)
  {
    _level = level;
    const int size = level >= 3 ? LZ4_sizeofStateHC() : LZ4_sizeofState();
    _state.resize((size_t)size / 8 + 1);
  }

  // Returns false when the data should be stored as is (incompressible or level 0).
  bool Compress(const Byte *src, size_t size, const Byte *&data, size_t &dataSize)
  {
    data = src;
    dataSize = size;
    if (_level == 0 || size == 0)
      return false;
    _dst.resize(size);
    const int cap = (int)size - 1;
    const int n = _level >= 3
        ? LZ4_compress_HC_extStateHC(_state.data(), (const char *)src, (char *)_dst.data(), (int)size, cap, _level)
        : LZ4_compress_fast_extState(_state.data(), (const char *)src, (char *)_dst.data(), (int)size, cap, 1);
    if (n <= 0)
      return false;
    data = _dst.data();
    dataSize = (size_t)n;
    return true;
  }
};

// Accumulates a metadata table as a chain of 8 KiB blocks.
class CMetaWriter
{
  CCompressor &_comp;
  std::vector<Byte> _cur;

  void FlushBlock(size_t size)
  {
    const Byte *data;
    size_t dataSize;
    const bool packed = _comp.Compress(_cur.data(), size, data, dataSize);
    BlockStarts.push_back((UInt32)Out.size());
    Byte hdr[2];
    Set16(hdr, (UInt16)(dataSize | (packed ? 0 : kMetaUncompressed)));
    Out.insert(Out.end(), hdr, hdr + 2);
    Out.insert(Out.end(), data, data + dataSize);
    _cur.erase(_cur.begin(), _cur.begin() + (ptrdiff_t)size);
  }

public:
  std::vector<Byte> Out;
  std::vector<UInt32> BlockStarts;
  UInt64 Written = 0;

  explicit CMetaWriter(CCompressor &comp): _comp(comp) {}

  UInt64 Ref() const { return ((UInt64)Out.size() << 16) | _cur.size(); }

  void Write(const void *data, size_t size)
  {
    const Byte *p = (const Byte *)data;
    _cur.insert(_cur.end(), p, p + size);
    Written += size;
    while (_cur.size() >= kMetaSize)
      FlushBlock(kMetaSize);
  }

  void Finish()
  {
    if (!_cur.empty())
      FlushBlock(_cur.size());
  }
};

struct CNode
{
  std::string Name; // UTF-8
  bool IsDir = false;
  bool Skip = false;
  UInt32 Mode = 0;
  UInt32 MTime = 0;
  int UpdateIndex = -1; // new data from the update callback
  int ArcIndex = -1;    // data reused from the opened archive
  UInt64 Size = 0;
  std::map<std::string, unsigned> Children; // byte order, as SquashFS requires

  UInt32 Number = 0;
  UInt64 Ref = 0;
  UInt64 BlocksStart = 0;
  UInt32 Frag = kNoFragment;
  UInt32 FragOffset = 0;
  std::vector<UInt32> Blocks;
};

static bool ParseBlockSize(const PROPVARIANT &prop, UInt32 &log)
{
  UInt64 v = 0;
  if (prop.vt == VT_UI4)
    v = prop.ulVal;
  else if (prop.vt == VT_BSTR)
  {
    const wchar_t *end;
    v = ConvertStringToUInt64(prop.bstrVal, &end);
    switch (MyCharLower_Ascii(*end))
    {
      case 0: break;
      case 'k': v <<= 10; end++; break;
      case 'm': v <<= 20; end++; break;
      default: return false;
    }
    if (*end != 0)
      return false;
  }
  else
    return false;
  for (UInt32 i = 12; i <= 20; i++)
    if (v == ((UInt64)1 << i))
    {
      log = i;
      return true;
    }
  return false;
}

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *names, const PROPVARIANT *values, UInt32 numProps))
{
  COM_TRY_BEGIN
  for (UInt32 i = 0; i < numProps; i++)
  {
    UString name = names[i];
    name.MakeLower_Ascii();
    const PROPVARIANT &prop = values[i];
    if (_ogsr && (name.IsPrefixedBy_Ascii_NoCase("x") || name.IsEqualTo("block") || name.IsEqualTo("frags")))
    {
      // "sq (OGSR)" always uses the wiki settings: mksquashfs -comp lz4 -Xhc.
    }
    else if (name.IsPrefixedBy_Ascii_NoCase("x") || name.IsEqualTo("level"))
    {
      // -mx9 arrives as name "x9" with an empty value, -mx=9 and -mlevel=9 as a value.
      UInt32 level = 9;
      RINOK(ParsePropToUInt32(name.IsEqualTo("level") ? UString() : name.Ptr(1), prop, level))
      if (level > LZ4HC_CLEVEL_MAX)
        return E_INVALIDARG;
      _level = (int)level;
    }
    else if (name.IsEqualTo("block"))
    {
      if (!ParseBlockSize(prop, _blockLog))
        return E_INVALIDARG;
      _blockLogSet = true;
    }
    else if (name.IsEqualTo("frags"))
    {
      RINOK(PROPVARIANT_to_bool(prop, _useFrags))
    }
    else if (name.IsPrefixedBy_Ascii_NoCase("mt") || name.IsEqualTo("tm") || name.IsEqualTo("tc")
        || name.IsEqualTo("ta") || name.IsEqualTo("tp") || name.IsEqualTo("memuse"))
    {
      // Generic 7-Zip options that do not apply to this format.
    }
    else
      return E_INVALIDARG;
  }
  return S_OK;
  COM_TRY_END
}

// The engine mounts the archive root as gamedata\, so a top-level gamedata folder is dropped.
// Returns true for the gamedata folder itself, which has nothing left to store.
static bool StripGamedata(UString &path)
{
  UString p = path;
  p.Replace(L'\\', L'/');
  const unsigned n = 8; // "gamedata"
  if (!p.IsPrefixedBy_Ascii_NoCase("gamedata") || (p.Len() > n && p[n] != L'/'))
    return false;
  if (p.Len() <= n + 1)
    return true;
  path = p.Ptr(n + 1);
  return false;
}

Z7_COM7F_IMF(CHandler::GetFileTimeType(UInt32 *type))
{
  *type = NFileTimeType::kUnix;
  return S_OK;
}

namespace {

class CUpdater
{
public:
  CHandler &Arc;
  IArchiveUpdateCallback *Callback;
  IOutStream *Out;
  CCompressor Comp;
  UInt32 BlockLog = 17;
  UInt32 BlockSize = 1 << 17;
  bool UseFrags = true;
  bool Ogsr = false; // names in CP1251, as the engine passes them to libsquashfs untranslated
  std::vector<CNode> Nodes;
  std::vector<CFragment> Frags;
  std::vector<Byte> FragBuf;
  // Identical files share one copy of the data, as with mksquashfs.
  std::map<std::string, unsigned> Dups;
  UInt64 Pos = 0;
  UInt64 Completed = 0;
  UInt32 NumInodes = 0;

  CUpdater(CHandler &arc, IArchiveUpdateCallback *callback, IOutStream *out):
      Arc(arc), Callback(callback), Out(out) {}

  HRESULT WriteOut(const void *data, size_t size)
  {
    Pos += size;
    return WriteStream(Out, data, size);
  }

  HRESULT AddProgress(UInt64 size)
  {
    Completed += size;
    return Callback->SetCompleted(&Completed);
  }

  HRESULT FindNode(const UString &path, bool isDir, UInt32 mtime, unsigned &result);
  HRESULT WriteBlock(CNode &node, const Byte *data, size_t size);
  HRESULT FlushFragment();
  HRESULT AddFragment(CNode &node, const Byte *data, size_t size);
  HRESULT WriteFile(unsigned index);
  HRESULT WriteData(unsigned index);

  void ShareData(CNode &node, const CNode &same)
  {
    node.BlocksStart = same.BlocksStart;
    node.Blocks = same.Blocks;
    node.Frag = same.Frag;
    node.FragOffset = same.FragOffset;
  }
  void Number(unsigned index);
  void WriteInodes(unsigned index, UInt32 parent, CMetaWriter &inodes, CMetaWriter &dirs);
  HRESULT WriteTable(CMetaWriter &table, UInt64 &indexPos);
};

HRESULT CUpdater::FindNode(const UString &path, bool isDir, UInt32 mtime, unsigned &result)
{
  UString p = path;
  p.Replace(L'\\', L'/');
  UStringVector parts;
  int start = 0;
  for (;;)
  {
    const int slash = p.Find(L'/', (unsigned)start);
    const UString part = p.Mid((unsigned)start, (unsigned)((slash < 0 ? (int)p.Len() : slash) - start));
    if (part == L"..")
      return E_INVALIDARG;
    if (!part.IsEmpty() && part != L".")
      parts.Add(part);
    if (slash < 0)
      break;
    start = slash + 1;
  }
  if (parts.IsEmpty())
    return E_INVALIDARG;

  unsigned cur = 0;
  for (unsigned i = 0; i < parts.Size(); i++)
  {
    AString utf;
    if (Ogsr)
    {
      bool defaultUsed = false;
      utf = UnicodeStringToMultiByte(parts[i], 1251, '_', defaultUsed);
      if (defaultUsed)
        return E_INVALIDARG; // the name has characters outside CP1251
    }
    else
      ConvertUnicodeToUTF8(parts[i], utf);
    if (utf.Len() > kMaxNameLen)
      return E_INVALIDARG;
    const std::string name(utf.Ptr(), utf.Len());
    const bool last = (i + 1 == parts.Size());
    const bool wantDir = !last || isDir;
    const auto it = Nodes[cur].Children.find(name);
    if (it != Nodes[cur].Children.end())
    {
      if (Nodes[it->second].IsDir != wantDir)
        return E_INVALIDARG;
      cur = it->second;
      continue;
    }
    CNode node;
    node.Name = name;
    node.IsDir = wantDir;
    node.Mode = wantDir ? kDefaultDirMode : kDefaultFileMode;
    node.MTime = mtime;
    const unsigned index = (unsigned)Nodes.size();
    Nodes.push_back(std::move(node));
    Nodes[cur].Children[name] = index;
    cur = index;
  }
  result = cur;
  return S_OK;
}

HRESULT CUpdater::WriteBlock(CNode &node, const Byte *data, size_t size)
{
  const Byte *packed;
  size_t packSize;
  const bool compressed = Comp.Compress(data, size, packed, packSize);
  node.Blocks.push_back((UInt32)packSize | (compressed ? 0 : kBlockUncompressed));
  return WriteOut(packed, packSize);
}

HRESULT CUpdater::FlushFragment()
{
  if (FragBuf.empty())
    return S_OK;
  const Byte *packed;
  size_t packSize;
  const bool compressed = Comp.Compress(FragBuf.data(), FragBuf.size(), packed, packSize);
  CFragment f;
  f.Start = Pos;
  f.Size = (UInt32)packSize | (compressed ? 0 : kBlockUncompressed);
  Frags.push_back(f);
  FragBuf.clear();
  return WriteOut(packed, packSize);
}

HRESULT CUpdater::AddFragment(CNode &node, const Byte *data, size_t size)
{
  if (FragBuf.size() + size > BlockSize)
    RINOK(FlushFragment())
  node.Frag = (UInt32)Frags.size();
  node.FragOffset = (UInt32)FragBuf.size();
  FragBuf.insert(FragBuf.end(), data, data + size);
  return S_OK;
}

// Sequential source of file data: a new stream or a decoded item of the opened archive.
class CFileSource
{
  CHandler &_arc;
  const CItem *_item = nullptr;
  CMyComPtr<ISequentialInStream> _stream;
  size_t _block = 0;
  UInt64 _pos = 0;
  bool _tailDone = false;
  std::vector<Byte> _buf;
  size_t _bufPos = 0;
public:
  bool DataError = false;

  explicit CFileSource(CHandler &arc): _arc(arc) {}
  void SetStream(ISequentialInStream *s) { _stream = s; }
  void SetItem(const CItem &item) { _item = &item; _pos = item.BlocksStart; }

  // Fills size bytes unless the data ends first.
  HRESULT Read(Byte *data, size_t &size)
  {
    if (_stream)
      return ReadStream(_stream, data, &size);
    size_t done = 0;
    while (done < size && !DataError)
    {
      if (_bufPos == _buf.size())
      {
        _bufPos = 0;
        if (_block < _item->Blocks.size())
          RINOK(_arc.ReadBlock(*_item, _block++, _pos, _buf, DataError))
        else if (!_tailDone)
        {
          _tailDone = true;
          RINOK(_arc.ReadTail(*_item, _buf, DataError))
        }
        else
          break;
        continue;
      }
      size_t n = _buf.size() - _bufPos;
      if (n > size - done)
        n = size - done;
      memcpy(data + done, _buf.data() + _bufPos, n);
      _bufPos += n;
      done += n;
    }
    size = done;
    return S_OK;
  }
};

HRESULT CUpdater::WriteFile(unsigned index)
{
  CNode &node = Nodes[index];
  node.BlocksStart = Pos;
  const CItem *src = node.ArcIndex >= 0 ? &Arc.Item((UInt32)node.ArcIndex) : nullptr;

  // Unchanged files keep their full blocks: copy them without recompression.
  if (src && Arc.BlockSize() == BlockSize)
  {
    // Files that already shared data in the old archive keep sharing it.
    char key[80];
    sprintf(key, "o%llx:%llx:%x:%x", (unsigned long long)src->BlocksStart,
        (unsigned long long)src->Size, src->Frag, src->FragOffset);
    const auto it = Dups.find(key);
    if (it != Dups.end() && src->Size != 0)
    {
      ShareData(node, Nodes[it->second]);
      node.Size = src->Size;
      return AddProgress(src->Size);
    }
    Dups[key] = index;

    UInt64 srcPos = src->BlocksStart;
    std::vector<Byte> buf;
    for (const UInt32 entry : src->Blocks)
    {
      const UInt32 size = entry & kBlockSizeMask;
      if (size > BlockSize)
        return S_FALSE;
      buf.resize(size);
      RINOK(Arc.Stream()->Seek((Int64)srcPos, STREAM_SEEK_SET, NULL))
      RINOK(ReadStream_FAIL(Arc.Stream(), buf.data(), size))
      srcPos += size;
      node.Blocks.push_back(entry);
      RINOK(WriteOut(buf.data(), size))
      RINOK(AddProgress(BlockSize))
    }
    bool dataError = false;
    RINOK(Arc.ReadTail(*src, buf, dataError))
    if (dataError)
      return S_FALSE;
    if (!buf.empty())
    {
      if (node.Blocks.empty() && UseFrags)
        RINOK(AddFragment(node, buf.data(), buf.size()))
      else
        RINOK(WriteBlock(node, buf.data(), buf.size()))
    }
    node.Size = src->Size;
    return S_OK;
  }

  CFileSource source(Arc);
  if (src)
    source.SetItem(*src);
  else
  {
    CMyComPtr<ISequentialInStream> stream;
    const HRESULT res = Callback->GetStream((UInt32)node.UpdateIndex, &stream);
    if (res == S_FALSE)
    {
      node.Skip = true; // the file could not be opened; 7-Zip reports it itself
      return S_OK;
    }
    RINOK(res)
    if (!stream)
    {
      node.Skip = true;
      return S_OK;
    }
    source.SetStream(stream);
  }

  std::vector<Byte> chunk(BlockSize);
  UInt64 size = 0;
  CSha256 sha;
  Sha256_Init(&sha);
  for (;;)
  {
    size_t n = BlockSize;
    RINOK(source.Read(chunk.data(), n))
    if (source.DataError)
      return S_FALSE;
    if (n == 0)
      break;
    Sha256_Update(&sha, chunk.data(), n);
    if (size == 0 && n < BlockSize && UseFrags)
    {
      size = n;
      RINOK(AddFragment(node, chunk.data(), n))
      RINOK(AddProgress(n))
      break;
    }
    size += n;
    RINOK(WriteBlock(node, chunk.data(), n))
    RINOK(AddProgress(n))
    if (n < BlockSize)
      break;
  }
  node.Size = size;
  if (!src)
    RINOK(Callback->SetOperationResult(NUpdate::NOperationResult::kOK))

  if (size == 0)
    return S_OK;
  Byte digest[SHA256_DIGEST_SIZE];
  Sha256_Final(&sha, digest);
  std::string key((const char *)digest, sizeof(digest));
  key.append((const char *)&size, sizeof(size));
  const auto it = Dups.find(key);
  if (it == Dups.end())
  {
    Dups[key] = index;
    return S_OK;
  }
  // The same data is already stored: drop the copy just written.
  if (node.Frag != kNoFragment)
    FragBuf.resize(node.FragOffset);
  else
  {
    RINOK(Out->Seek((Int64)node.BlocksStart, STREAM_SEEK_SET, NULL))
    Pos = node.BlocksStart;
  }
  ShareData(node, Nodes[it->second]);
  return S_OK;
}

HRESULT CUpdater::WriteData(unsigned index)
{
  for (const auto &child : Nodes[index].Children)
  {
    if (Nodes[child.second].IsDir)
      RINOK(WriteData(child.second))
    else
      RINOK(WriteFile(child.second))
  }
  return S_OK;
}

// Inodes are numbered in the order they are written: children first, the directory last.
void CUpdater::Number(unsigned index)
{
  for (const auto &child : Nodes[index].Children)
  {
    CNode &node = Nodes[child.second];
    if (node.Skip)
      continue;
    if (node.IsDir)
      Number(child.second);
    else
      node.Number = ++NumInodes;
  }
  Nodes[index].Number = ++NumInodes;
}

static void WriteInodeHeader(CMetaWriter &w, UInt16 type, const CNode &node)
{
  Byte p[16];
  Set16(p, type);
  Set16(p + 2, (UInt16)(node.Mode & 07777));
  Set16(p + 4, 0); // uid index
  Set16(p + 6, 0); // gid index
  Set32(p + 8, node.MTime);
  Set32(p + 12, node.Number);
  w.Write(p, 16);
}

static void WriteFileInode(CMetaWriter &w, CNode &node)
{
  node.Ref = w.Ref();
  Byte p[40];
  if (node.BlocksStart <= 0xFFFFFFFF && node.Size <= 0xFFFFFFFF)
  {
    WriteInodeHeader(w, kTypeFile, node);
    Set32(p, (UInt32)node.BlocksStart);
    Set32(p + 4, node.Frag);
    Set32(p + 8, node.FragOffset);
    Set32(p + 12, (UInt32)node.Size);
    w.Write(p, 16);
  }
  else
  {
    WriteInodeHeader(w, kTypeLFile, node);
    Set64(p, node.BlocksStart);
    Set64(p + 8, node.Size);
    Set64(p + 16, 0); // sparse bytes
    Set32(p + 24, 1); // links
    Set32(p + 28, node.Frag);
    Set32(p + 32, node.FragOffset);
    Set32(p + 36, 0xFFFFFFFF); // no xattrs
    w.Write(p, 40);
  }
  for (const UInt32 b : node.Blocks)
  {
    Set32(p, b);
    w.Write(p, 4);
  }
}

void CUpdater::WriteInodes(unsigned index, UInt32 parent, CMetaWriter &inodes, CMetaWriter &dirs)
{
  std::vector<unsigned> entries;
  UInt32 numSubDirs = 0;
  for (const auto &child : Nodes[index].Children)
  {
    CNode &node = Nodes[child.second];
    if (node.Skip)
      continue;
    if (node.IsDir)
    {
      WriteInodes(child.second, Nodes[index].Number, inodes, dirs);
      numSubDirs++;
    }
    else
      WriteFileInode(inodes, node);
    entries.push_back(child.second);
  }

  // Directory listing: entries grouped by inode block, at most 256 per header.
  const UInt64 listRef = dirs.Ref();
  const UInt64 listStart = dirs.Written;
  for (size_t i = 0; i < entries.size();)
  {
    const CNode &first = Nodes[entries[i]];
    const UInt32 block = (UInt32)(first.Ref >> 16);
    size_t j = i;
    while (j < entries.size() && j - i < 256)
    {
      const CNode &n = Nodes[entries[j]];
      const Int64 delta = (Int64)n.Number - (Int64)first.Number;
      if ((UInt32)(n.Ref >> 16) != block || delta < -32768 || delta > 32767)
        break;
      j++;
    }
    Byte h[12];
    Set32(h, (UInt32)(j - i - 1));
    Set32(h + 4, block);
    Set32(h + 8, first.Number);
    dirs.Write(h, 12);
    for (; i < j; i++)
    {
      const CNode &n = Nodes[entries[i]];
      Byte e[8];
      Set16(e, (UInt16)(n.Ref & 0xFFFF));
      Set16(e + 2, (UInt16)(Int16)((Int64)n.Number - (Int64)first.Number));
      Set16(e + 4, n.IsDir ? kTypeDir : kTypeFile);
      Set16(e + 6, (UInt16)(n.Name.size() - 1));
      dirs.Write(e, 8);
      dirs.Write(n.Name.data(), n.Name.size());
    }
  }
  const UInt64 fileSize = dirs.Written - listStart + 3;

  CNode &dir = Nodes[index];
  dir.Ref = inodes.Ref();
  Byte p[24];
  if (fileSize <= 0xFFFF)
  {
    WriteInodeHeader(inodes, kTypeDir, dir);
    Set32(p, (UInt32)(listRef >> 16));
    Set32(p + 4, 2 + numSubDirs);
    Set16(p + 8, (UInt16)fileSize);
    Set16(p + 10, (UInt16)(listRef & 0xFFFF));
    Set32(p + 12, parent);
    inodes.Write(p, 16);
  }
  else
  {
    WriteInodeHeader(inodes, kTypeLDir, dir);
    Set32(p, 2 + numSubDirs);
    Set32(p + 4, (UInt32)fileSize);
    Set32(p + 8, (UInt32)(listRef >> 16));
    Set32(p + 12, parent);
    Set16(p + 16, 0); // no directory index
    Set16(p + 18, (UInt16)(listRef & 0xFFFF));
    Set32(p + 20, 0xFFFFFFFF); // no xattrs
    inodes.Write(p, 24);
  }
}

// Writes the table blocks followed by the array of their absolute positions.
HRESULT CUpdater::WriteTable(CMetaWriter &table, UInt64 &indexPos)
{
  table.Finish();
  const UInt64 start = Pos;
  RINOK(WriteOut(table.Out.data(), table.Out.size()))
  indexPos = Pos;
  for (const UInt32 b : table.BlockStarts)
  {
    Byte p[8];
    Set64(p, start + b);
    RINOK(WriteOut(p, 8))
  }
  return S_OK;
}

}

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *callback))
{
  COM_TRY_BEGIN
  CMyComPtr<IOutStream> out;
  outStream->QueryInterface(IID_IOutStream, (void **)&out);
  if (!out)
    return E_NOTIMPL; // the superblock is written last, so the output must be seekable

  CUpdater u(*this, callback, out);
  u.Ogsr = _ogsr;
  u.BlockLog = _ogsr ? 17 : (_stream && !_blockLogSet) ? _h.BlockLog : _blockLog;
  u.BlockSize = (UInt32)1 << u.BlockLog;
  u.UseFrags = _ogsr || _useFrags;
  u.Comp.Init(_ogsr && _level < 3 ? 9 : _level);

  const UInt32 now = (UInt32)time(NULL);
  u.Nodes.resize(1);
  u.Nodes[0].IsDir = true;
  u.Nodes[0].Mode = kDefaultDirMode;
  u.Nodes[0].MTime = now;

  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData = 0, newProps = 0;
    UInt32 indexInArc = (UInt32)(Int32)-1;
    RINOK(callback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc))
    const CItem *src = (indexInArc != (UInt32)(Int32)-1 && _stream && indexInArc < _items.size())
        ? &_items[indexInArc] : nullptr;

    UString path;
    bool isDir = false;
    UInt32 mtime = now;
    UInt32 mode = 0;
    bool hasMode = false;
    if (src)
    {
      path = src->Path;
      isDir = src->IsDir;
      mtime = src->MTime;
      mode = src->Mode;
      hasMode = true;
    }
    if (newProps || !src)
    {
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidIsAnti, &prop))
      if (prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE)
        continue;
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidPath, &prop))
      if (prop.vt == VT_BSTR)
        path = prop.bstrVal;
      else if (!src)
        return E_INVALIDARG;
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidIsDir, &prop))
      if (prop.vt == VT_BOOL)
        isDir = (prop.boolVal != VARIANT_FALSE);
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidMTime, &prop))
      if (prop.vt == VT_FILETIME)
      {
        UInt32 t;
        NWindows::NTime::FileTime_To_UnixTime(prop.filetime, t); // clamps out-of-range times
        mtime = t;
      }
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidPosixAttrib, &prop))
      if (prop.vt == VT_UI4)
      {
        mode = prop.ulVal & 07777;
        hasMode = true;
      }
    }
    if (src && src->IsDir != isDir)
      return E_INVALIDARG;
    if (_ogsr && StripGamedata(path))
      continue;

    unsigned index;
    RINOK(u.FindNode(path, isDir, mtime, index))
    CNode &node = u.Nodes[index];
    node.MTime = mtime;
    if (hasMode)
      node.Mode = mode;
    if (isDir)
      continue;
    if (newData)
    {
      node.UpdateIndex = (int)i;
      node.ArcIndex = -1;
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidSize, &prop))
      if (prop.vt == VT_UI8)
        node.Size = prop.uhVal.QuadPart;
    }
    else if (src)
    {
      node.ArcIndex = (int)indexInArc;
      node.UpdateIndex = -1;
      node.Size = src->Size;
    }
    else
      return E_INVALIDARG;
  }

  for (const CNode &node : u.Nodes)
    if (!node.IsDir)
      total += node.Size;
  RINOK(callback->SetTotal(total))

  // Superblock placeholder and LZ4 options; the superblock is filled in at the end.
  const bool hc = _level >= 3;
  Byte head[kSuperBlockSize + 10] = {};
  size_t headSize = kSuperBlockSize;
  if (hc)
  {
    Set16(head + headSize, (UInt16)(8 | kMetaUncompressed));
    Set32(head + headSize + 2, kLz4Legacy);
    Set32(head + headSize + 6, kLz4FlagHc);
    headSize += 10;
  }
  RINOK(out->Seek(0, STREAM_SEEK_SET, NULL))
  RINOK(u.WriteOut(head, headSize))

  RINOK(u.WriteData(0))
  RINOK(u.FlushFragment())

  u.Number(0);
  CMetaWriter inodes(u.Comp), dirs(u.Comp);
  u.WriteInodes(0, u.NumInodes + 1, inodes, dirs);
  inodes.Finish();
  dirs.Finish();

  CSuperBlock h;
  h.NumInodes = u.NumInodes;
  h.MTime = now;
  h.BlockSize = u.BlockSize;
  h.NumFrags = (UInt32)u.Frags.size();
  h.Compression = kCompLz4;
  h.BlockLog = (UInt16)u.BlockLog;
  h.Flags = (UInt16)(kFlagDuplicates | (hc ? kFlagCompOptions : 0) | (u.UseFrags ? 0 : kFlagNoFragments));
  h.NumIds = 1;
  h.Major = 4;
  h.Minor = 0;
  h.RootInode = u.Nodes[0].Ref;
  h.XattrTable = kInvalidTable;
  h.ExportTable = kInvalidTable;

  h.InodeTable = u.Pos;
  RINOK(u.WriteOut(inodes.Out.data(), inodes.Out.size()))
  h.DirTable = u.Pos;
  RINOK(u.WriteOut(dirs.Out.data(), dirs.Out.size()))

  CMetaWriter frags(u.Comp);
  for (const CFragment &f : u.Frags)
  {
    Byte p[16];
    Set64(p, f.Start);
    Set32(p + 8, f.Size);
    Set32(p + 12, 0);
    frags.Write(p, 16);
  }
  RINOK(u.WriteTable(frags, h.FragTable))

  CMetaWriter ids(u.Comp);
  const Byte rootId[4] = {};
  ids.Write(rootId, 4);
  RINOK(u.WriteTable(ids, h.IdTable))

  h.BytesUsed = u.Pos;
  const size_t pad = (size_t)((4096 - (u.Pos & 4095)) & 4095);
  if (pad != 0)
  {
    const std::vector<Byte> zeros(pad);
    RINOK(u.WriteOut(zeros.data(), pad))
  }

  WriteSuperBlock(head, h);
  RINOK(out->Seek(0, STREAM_SEEK_SET, NULL))
  RINOK(WriteStream(out, head, kSuperBlockSize))
  RINOK(out->Seek((Int64)u.Pos, STREAM_SEEK_SET, NULL))
  // A dropped duplicate may have been the last data written past the end.
  return out->SetSize(u.Pos);
  COM_TRY_END
}

}}
