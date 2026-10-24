#include "StdAfx.h"

#include "7zip/Common/RegisterArc.h"

#include "SqHandler.h"

namespace NArchive {
namespace NSq {

static const Byte k_Signature[] = { 'h', 's', 'q', 's' };

// Only LZ4 images are claimed; other SquashFS images are left to the built-in handler.
API_FUNC_static_IsArc IsArc_Sq(const Byte *p, size_t size)
{
  if (size < kSuperBlockSize)
    return k_IsArc_Res_NEED_MORE;
  CSuperBlock h;
  return ParseSuperBlock(p, h) ? k_IsArc_Res_YES : k_IsArc_Res_NO;
}
}

static const UInt32 kTimeFlags =
    TIME_PREC_TO_ARC_FLAGS_MASK (NFileTimeType::kUnix)
  | TIME_PREC_TO_ARC_FLAGS_TIME_DEFAULT (NFileTimeType::kUnix);

static IInArchive *CreateArcOgsr() { return new CHandler(true); }
static IOutArchive *CreateArcOutOgsr() { return new CHandler(true); }
static IInArchive *CreateArcPlain() { return new CHandler(false); }
static IOutArchive *CreateArcOutPlain() { return new CHandler(false); }

// Game archives (.sq, .sq_01 ...) open as "sq (OGSR)", so edits keep the game layout.
static const CArcInfo g_ArcInfoOgsr = {
  NArcInfoFlags::kMTime, 0xB6, sizeof(k_Signature), 0, k_Signature,
  "sq (OGSR)", "sq sq_01 sq_02 sq_03 sq_04 sq_05 sq_06 sq_07 sq_08 sq_09", NULL,
  kTimeFlags, CreateArcOgsr, CreateArcOutOgsr, IsArc_Sq };

static const CArcInfo g_ArcInfoPlain = {
  NArcInfoFlags::kMTime, 0xB5, sizeof(k_Signature), 0, k_Signature,
  "sq", "sqfs", NULL,
  kTimeFlags, CreateArcPlain, CreateArcOutPlain, IsArc_Sq };

struct CRegisterArcs
{
  CRegisterArcs()
  {
    RegisterArc(&g_ArcInfoOgsr);
    RegisterArc(&g_ArcInfoPlain);
  }
};
static CRegisterArcs g_RegisterArcs;

}}
