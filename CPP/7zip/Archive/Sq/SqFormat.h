// SquashFS 4.0 on-disk constants shared by the reader and the writer.
// Only little-endian images with LZ4 compression are handled.

#pragma once

#include "Common/MyTypes.h"

namespace NArchive {
namespace NSq {

const UInt32 kMagic = 0x73717368; // "hsqs"
const UInt32 kSuperBlockSize = 96;
const UInt16 kCompLz4 = 5;

const UInt32 kMetaSize = 8192;
const UInt16 kMetaUncompressed = 0x8000;
const UInt32 kBlockUncompressed = 1u << 24;
const UInt32 kBlockSizeMask = kBlockUncompressed - 1;
const UInt32 kNoFragment = 0xFFFFFFFF;
const UInt64 kInvalidTable = (UInt64)(Int64)-1;

// Superblock flags.
const UInt16 kFlagNoFragments = 0x0010;
const UInt16 kFlagDuplicates = 0x0040;
const UInt16 kFlagCompOptions = 0x0400;

// Inode types.
const UInt16 kTypeDir = 1;
const UInt16 kTypeFile = 2;
const UInt16 kTypeLDir = 8;
const UInt16 kTypeLFile = 9;

// LZ4 compressor options written by mksquashfs -Xhc.
const UInt32 kLz4Legacy = 1;
const UInt32 kLz4FlagHc = 1;

const UInt32 kModeDir = 0040000;
const UInt32 kModeFile = 0100000;

struct CSuperBlock
{
  UInt32 NumInodes = 0;
  UInt32 MTime = 0;
  UInt32 BlockSize = 0;
  UInt32 NumFrags = 0;
  UInt16 Compression = 0;
  UInt16 BlockLog = 0;
  UInt16 Flags = 0;
  UInt16 NumIds = 0;
  UInt16 Major = 0;
  UInt16 Minor = 0;
  UInt64 RootInode = 0;
  UInt64 BytesUsed = 0;
  UInt64 IdTable = 0;
  UInt64 XattrTable = 0;
  UInt64 InodeTable = 0;
  UInt64 DirTable = 0;
  UInt64 FragTable = 0;
  UInt64 ExportTable = 0;
};

inline UInt16 Get16(const Byte *p) { return (UInt16)(p[0] | ((UInt16)p[1] << 8)); }
inline UInt32 Get32(const Byte *p) { return (UInt32)Get16(p) | ((UInt32)Get16(p + 2) << 16); }
inline UInt64 Get64(const Byte *p) { return (UInt64)Get32(p) | ((UInt64)Get32(p + 4) << 32); }

inline void Set16(Byte *p, UInt16 v) { p[0] = (Byte)v; p[1] = (Byte)(v >> 8); }
inline void Set32(Byte *p, UInt32 v) { Set16(p, (UInt16)v); Set16(p + 2, (UInt16)(v >> 16)); }
inline void Set64(Byte *p, UInt64 v) { Set32(p, (UInt32)v); Set32(p + 4, (UInt32)(v >> 32)); }

inline bool ParseSuperBlock(const Byte *p, CSuperBlock &h)
{
  if (Get32(p) != kMagic)
    return false;
  h.NumInodes = Get32(p + 4);
  h.MTime = Get32(p + 8);
  h.BlockSize = Get32(p + 12);
  h.NumFrags = Get32(p + 16);
  h.Compression = Get16(p + 20);
  h.BlockLog = Get16(p + 22);
  h.Flags = Get16(p + 24);
  h.NumIds = Get16(p + 26);
  h.Major = Get16(p + 28);
  h.Minor = Get16(p + 30);
  h.RootInode = Get64(p + 32);
  h.BytesUsed = Get64(p + 40);
  h.IdTable = Get64(p + 48);
  h.XattrTable = Get64(p + 56);
  h.InodeTable = Get64(p + 64);
  h.DirTable = Get64(p + 72);
  h.FragTable = Get64(p + 80);
  h.ExportTable = Get64(p + 88);
  return h.Major == 4
      && h.Compression == kCompLz4
      && h.BlockLog >= 12 && h.BlockLog <= 20
      && h.BlockSize == ((UInt32)1 << h.BlockLog);
}

inline void WriteSuperBlock(Byte *p, const CSuperBlock &h)
{
  Set32(p, kMagic);
  Set32(p + 4, h.NumInodes);
  Set32(p + 8, h.MTime);
  Set32(p + 12, h.BlockSize);
  Set32(p + 16, h.NumFrags);
  Set16(p + 20, h.Compression);
  Set16(p + 22, h.BlockLog);
  Set16(p + 24, h.Flags);
  Set16(p + 26, h.NumIds);
  Set16(p + 28, h.Major);
  Set16(p + 30, h.Minor);
  Set64(p + 32, h.RootInode);
  Set64(p + 40, h.BytesUsed);
  Set64(p + 48, h.IdTable);
  Set64(p + 56, h.XattrTable);
  Set64(p + 64, h.InodeTable);
  Set64(p + 72, h.DirTable);
  Set64(p + 80, h.FragTable);
  Set64(p + 88, h.ExportTable);
}

}}
