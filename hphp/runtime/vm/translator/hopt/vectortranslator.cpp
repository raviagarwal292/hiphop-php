/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "runtime/base/strings.h"
#include "runtime/vm/member_operations.h"
#include "runtime/vm/translator/hopt/ir.h"
#include "runtime/vm/translator/hopt/hhbctranslator.h"
#include "runtime/vm/translator/translator-x64.h"

// This file does ugly things with macros so include it last
#include "runtime/vm/translator/hopt/vectortranslator-internal.h"

namespace HPHP { namespace VM { namespace JIT {

TRACE_SET_MOD(hhir);

using Transl::MInstrState;
using Transl::mInstrHasUnknownOffsets;

HhbcTranslator::VectorTranslator::VectorTranslator(
  const NormalizedInstruction& ni,
  HhbcTranslator& ht)
    : m_ni(ni)
    , m_ht(ht)
    , m_tb(*m_ht.m_tb)
    , m_mii(getMInstrInfo(ni.mInstrOp()))
    , m_needMIS(true)
    , m_misBase(nullptr)
    , m_base(nullptr)
    , m_result(nullptr)
{
}

void HhbcTranslator::VectorTranslator::emit() {
  // Assign stack slots to our stack inputs
  numberStackInputs();
  // Emit the base and every intermediate op
  emitMPre();
  // Emit the final operation
  emitFinalMOp();
  // Cleanup: decref inputs and scratch values
  emitMPost();
}

// Returns a pointer to the base of the current MInstrState struct, or
// an empty SSATmp if it's not needed
SSATmp* HhbcTranslator::VectorTranslator::genMisPtr() {
  if (m_needMIS) {
    return m_tb.genLdAddr(m_misBase, kReservedRSPSpillSpace);
  } else {
    return m_tb.genDefVoid();
  }
}

void HhbcTranslator::VectorTranslator::emitMPre() {
  if (!mInstrHasUnknownOffsets(m_ni) &&
      (m_ni.mInstrOp() == OpCGetM || m_ni.mInstrOp() == OpSetM)) {
    setNoMIState();
  }

  if (m_needMIS) {
    m_misBase = m_tb.genDefMIStateBase();
    SSATmp* uninit = m_tb.genDefUninit();

    SKTRACE(2, m_ni.source, "%s nLogicalRatchets=%u\n",
            __func__, nLogicalRatchets());
    if (nLogicalRatchets() > 0) {
      m_tb.genStMem(m_misBase, MISOFF(tvRef), uninit, true);
      m_tb.genStMem(m_misBase, MISOFF(tvRef2), uninit, true);
    }
    m_tb.genStRaw(m_misBase, RawMemSlot::MisBaseStrOff, m_tb.genDefConst(false),
                  kReservedRSPSpillSpace);
  }

  SKTRACE(2, m_ni.source, "%s\n", __func__);

  // The base location is input 0 or 1, and the location code is stored
  // separately from m_ni.immVecM, so input indices (iInd) and member indices
  // (mInd) commonly differ.  Additionally, W members have no corresponding
  // inputs, so it is necessary to track the two indices separately.
  m_iInd = m_mii.valCount();
  emitBaseOp();
  ++m_iInd;

  // Iterate over all but the last member, which is consumed by a final
  // operation.
  for (m_mInd = 0; m_mInd < m_ni.immVecM.size() - 1; ++m_mInd) {
    emitIntermediateOp();
    emitRatchetRefs();
  }
}

// Build a map from (stack) input index to stack index.
void HhbcTranslator::VectorTranslator::numberStackInputs() {
  // Stack inputs are pushed in the order they appear in the vector
  // from left to right, so earlier elements in the vector are at
  // higher offsets in the stack. m_mii.valCount() tells us how many
  // rvals the instruction takes on the stack; they're pushed after
  // any vector elements and we want to ignore them here.
  int stackIdx = m_mii.valCount() + m_ni.immVec.numStackValues() - 1;
  for (unsigned i = m_mii.valCount(); i < m_ni.inputs.size(); ++i) {
    const Location& l = m_ni.inputs[i]->location;
    switch (l.space) {
      case Location::Stack:
        assert(stackIdx >= 0);
        m_stackInputs[i] = stackIdx--;
        break;

      default:
        break;
    }
  }
  assert(stackIdx == (m_mii.valCount() - 1));
}

SSATmp* HhbcTranslator::VectorTranslator::getInput(unsigned i) {
  const DynLocation& dl = *m_ni.inputs[i];
  const Location& l = dl.location;

  assert(mapContains(m_stackInputs, i) == (l.space == Location::Stack));
  switch (l.space) {
    case Location::Stack:
      return m_ht.top(Type::fromRuntimeType(dl.rtt), m_stackInputs[i]);

    case Location::Local:
      return m_tb.genLdLoc(l.offset);

    case Location::Litstr:
      return m_tb.genDefConst<const StringData*>(m_ht.lookupStringId(l.offset));

    case Location::Litint:
      return m_tb.genDefConst<int64>(l.offset);

    case Location::This:
      return m_tb.genLdThis(nullptr);

    default: not_reached();
  }
}

void HhbcTranslator::VectorTranslator::emitBaseLCR() {
  const MInstrAttr& mia = m_mii.getAttr(m_ni.immVec.locationCode());
  const DynLocation& base = *m_ni.inputs[m_iInd];
  if (mia & MIA_warn) {
    if (base.rtt.isUninit()) {
      LocalId data(base.location.offset);
      m_tb.gen(RaiseUninitWarning, &data);
    }
  }

  if (base.isVariant()) {
    PUNT(emitBaseLCR-Variant);
  }

  bool promoteUninit = (mia & MIA_define) && base.rtt.isUninit();
  if (base.location.isLocal()) {
    uint32_t loc = base.location.offset;
    if (promoteUninit) {
      m_tb.genStLoc(loc, m_tb.genDefNull(), false, true, nullptr);
    }
    if (base.rtt.isNull()) {
      // The base local might get promoted to an Array or stdClass for set
      // operations. Punt for now.
      PUNT(emitBaseLCR-NullBase);
    }

    if (base.rtt.isObject() && mcodeMaybePropName(m_ni.immVecM[0])) {
      // We can pass the base to helpers by value in this case
      m_base = m_tb.genLdLoc(loc);
      assert(m_base->isA(Type::Obj));
    } else {
      // Everything else is passed by reference
      LocalId baseLocalId(loc);
      m_base = m_tb.gen(LdLocAddr, &baseLocalId, m_tb.getFp());
      if (base.rtt.valueType() == KindOfArray) {
        // The local's type will be unchanged but its value might change
        // because of COW. Kill the value but leave the type intact. This is
        // pessimistic for now: we only really need to kill the value when
        // mutating the array.
        auto t = m_tb.getLocalType(loc);
        // Task #2071378: Move this to
        // TraceBuilder::updateTrackedState once you use the m_base
        // that is a local address.
        m_tb.killLocalValue(loc);
        m_tb.genAssertLoc(loc, t);
      }
    }
  } else {
    // Only locals can be KindOfUninit
    assert(!promoteUninit);

    // Stack bases require a little more stack magic than we have right
    // now.
    PUNT(emitBaseLCR-CellBase);
  }
}

void HhbcTranslator::VectorTranslator::emitBaseH() {
  m_base = m_tb.genLdThis(nullptr);
}

void HhbcTranslator::VectorTranslator::emitBaseN() {
  PUNT(emitBaseN);
}

void HhbcTranslator::VectorTranslator::emitBaseG() {
  PUNT(emitBaseG);
}

void HhbcTranslator::VectorTranslator::emitBaseS() {
  PUNT(emitBaseS);
}

void HhbcTranslator::VectorTranslator::emitBaseOp() {
  LocationCode lCode = m_ni.immVec.locationCode();
  switch (lCode) {
  case LL: case LC: case LR: emitBaseLCR(); break;
  case LH:                   emitBaseH();   break;
  case LGL: case LGC:        emitBaseG();   break;
  case LNL: case LNC:        emitBaseN();   break;
  case LSL: case LSC:        emitBaseS();   break;
  default:                   not_reached();
  }
}

void HhbcTranslator::VectorTranslator::emitIntermediateOp() {
  switch (m_ni.immVecM[m_mInd]) {
    case MEC: case MEL: case MET: case MEI: {
      emitElem();
      ++m_iInd;
      break;
    }
    case MPC: case MPL: case MPT:
      emitProp();
      ++m_iInd;
      break;
    case MW:
      assert(m_mii.newElem());
      emitNewElem();
      break;
    default: not_reached();
  }
}


void HhbcTranslator::VectorTranslator::emitProp() {
  const Class* knownCls = nullptr;
  const int propOffset  = getPropertyOffset(m_ni, knownCls, m_mii,
                                            m_mInd, m_iInd);
  if (propOffset == -1) {
    emitPropGeneric();
  } else {
    PUNT(Prop-KnownOffset);
  }
}

template <KeyType keyType, bool unboxKey, MInstrAttr attrs, bool isObj>
static inline TypedValue* propImpl(Class* ctx, TypedValue* base,
                                   TypedValue* key,
                                   MInstrState* mis) {
  key = unbox<keyType, unboxKey>(key);
  return Prop<WDU(attrs), isObj, keyType>(
    mis->tvScratch, mis->tvRef, ctx, base, key);
}

#define HELPER_TABLE(m)                                              \
  /* name        key  unboxKey attrs        isObj */                 \
  m(propC,     AnyKey,  false, None,        false)                   \
  m(propCD,    AnyKey,  false, Define,      false)                   \
  m(propCDO,   AnyKey,  false, Define,       true)                   \
  m(propCO,    AnyKey,  false, None,         true)                   \
  m(propCU,    AnyKey,  false, Unset,       false)                   \
  m(propCUO,   AnyKey,  false, Unset,        true)                   \
  m(propCW,    AnyKey,  false, Warn,        false)                   \
  m(propCWD,   AnyKey,  false, WarnDefine,  false)                   \
  m(propCWDO,  AnyKey,  false, WarnDefine,   true)                   \
  m(propCWO,   AnyKey,  false, Warn,         true)                   \
  m(propL,     AnyKey,   true, None,        false)                   \
  m(propLD,    AnyKey,   true, Define,      false)                   \
  m(propLDO,   AnyKey,   true, Define,       true)                   \
  m(propLO,    AnyKey,   true, None,         true)                   \
  m(propLU,    AnyKey,   true, Unset,       false)                   \
  m(propLUO,   AnyKey,   true, Unset,        true)                   \
  m(propLW,    AnyKey,   true, Warn,        false)                   \
  m(propLWD,   AnyKey,   true, WarnDefine,  false)                   \
  m(propLWDO,  AnyKey,   true, WarnDefine,   true)                   \
  m(propLWO,   AnyKey,   true, Warn,         true)                   \
  m(propLS,    StrKey,   true, None,        false)                   \
  m(propLSD,   StrKey,   true, Define,      false)                   \
  m(propLSDO,  StrKey,   true, Define,       true)                   \
  m(propLSO,   StrKey,   true, None,         true)                   \
  m(propLSU,   StrKey,   true, Unset,       false)                   \
  m(propLSUO,  StrKey,   true, Unset,        true)                   \
  m(propLSW,   StrKey,   true, Warn,        false)                   \
  m(propLSWD,  StrKey,   true, WarnDefine,  false)                   \
  m(propLSWDO, StrKey,   true, WarnDefine,   true)                   \
  m(propLSWO,  StrKey,   true, Warn,         true)                   \
  m(propS,     StrKey,  false, None,        false)                   \
  m(propSD,    StrKey,  false, Define,      false)                   \
  m(propSDO,   StrKey,  false, Define,       true)                   \
  m(propSO,    StrKey,  false, None,         true)                   \
  m(propSU,    StrKey,  false, Unset,       false)                   \
  m(propSUO,   StrKey,  false, Unset,        true)                   \
  m(propSW,    StrKey,  false, Warn,        false)                   \
  m(propSWD,   StrKey,  false, WarnDefine,  false)                   \
  m(propSWDO,  StrKey,  false, WarnDefine,   true)                   \
  m(propSWO,   StrKey,  false, Warn,         true)

#define PROP(nm, ...)                                                   \
static TypedValue* nm(Class* ctx, TypedValue* base, TypedValue key,     \
                      MInstrState* mis) {                               \
  return propImpl<__VA_ARGS__>(ctx, base, &key, mis);                   \
}
HELPER_TABLE(PROP)
#undef PROP

void HhbcTranslator::VectorTranslator::emitPropGeneric() {
  MemberCode mCode = m_ni.immVecM[m_mInd];
  MInstrAttr mia = MInstrAttr(m_mii.getAttr(mCode) & MIA_intermediate_prop);

  typedef TypedValue* (*OpFunc)(Class*, TypedValue*, TypedValue, MInstrState*);
  SSATmp* key = getInput(m_iInd);
  BUILD_OPTAB(getKeyTypeS(key), key->isBoxed(), mia, m_base->isA(Type::Obj));
  m_base = m_tb.genPropX((TCA)opFunc, CTX(), objOrPtr(m_base), noLitInt(key),
                         genMisPtr());
}
#undef HELPER_TABLE

void HhbcTranslator::VectorTranslator::emitElem() {
  PUNT(emitElem);
}

void HhbcTranslator::VectorTranslator::emitNewElem() {
  PUNT(emitNewElem);
}

void HhbcTranslator::VectorTranslator::emitRatchetRefs() {
  if (ratchetInd() < 0 || ratchetInd() >= int(nLogicalRatchets())) {
    return;
  }

  {
    // XXX Check for uninit here and don't punt once we have control
    // flow (#2020251)
    PUNT(emitRatchetRefs);

    // Clean up tvRef2 before overwriting it.
    if (ratchetInd() > 0) {
      m_tb.genDecRefMem(m_misBase, MISOFF(tvRef2), Type::Gen);
    }
    // Copy tvRef to tvRef2. Use mmx at some point
    SSATmp* tvRef = m_tb.genLdMem(m_misBase, MISOFF(tvRef), Type::Gen, nullptr);
    m_tb.genStMem(m_misBase, MISOFF(tvRef2), tvRef, true);

    // Reset tvRef.
    m_tb.genStMem(m_misBase, MISOFF(tvRef), m_tb.genDefUninit(), true);

    // Adjust base pointer.
    assert(m_base->getType().isPtr());
    m_base = m_tb.genLdAddr(m_misBase, MISOFF(tvRef2));
  }
}

void HhbcTranslator::VectorTranslator::emitFinalMOp() {
  typedef void (HhbcTranslator::VectorTranslator::*MemFun)();

  switch (m_ni.immVecM[m_mInd]) {
  case MEC: case MEL: case MET: case MEI:
    if (m_mii.instr() == MI_CGetM) {
      emitCGetElem();
      break;
    }
    PUNT(emitFinalMOp-ME);
    /*static MemFun elemOps[] = {
#   define MII(instr, ...) &HhbcTranslator::VectorTranslator::emit##instr##Elem,
    MINSTRS
#   undef MII
    };
    (this->*elemOps[m_mii.instr()])();*/
    break;

  case MPC: case MPL: case MPT:
    if (m_mii.instr() == MI_CGetM) {
      emitCGetProp();
      break;
    }
    PUNT(emitFinalMOp-MP);
    /*static MemFun propOps[] = {
#   define MII(instr, ...) &HhbcTranslator::VectorTranslator::emit##instr##Prop,
    MINSTRS
#   undef MII
    };
    (this->*propOps[m_mii.instr()])();*/
    break;

  case MW:
    PUNT(emitFinalMOp-MW);
    /*assert(m_mii.getAttr(MW) & MIA_final);
    static MemFun newOps[] = {
#   define MII(instr, attrs, bS, iS, vC, fN) \
      &HhbcTranslator::VectorTranslator::emit##fN,
    MINSTRS
#   undef MII
    };
    (this->*newOp[m_mii.instr()])();*/
    break;

  default: not_reached();
  }
}

template <KeyType keyType, bool unboxKey, bool isObj>
static inline TypedValue cGetPropImpl(Class* ctx, TypedValue* base,
                                      TypedValue keyVal, MInstrState* mis) {
  TypedValue result;
  TypedValue* key = keyPtr<keyType>(keyVal);
  key = unbox<keyType, unboxKey>(key);
  base = Prop<true, false, false, isObj, keyType>(
    result, mis->tvRef, ctx, base, key);
  if (base != &result) {
    // Grab a reference to the result.
    tvDup(base, &result);
  }
  if (result.m_type == KindOfRef) {
    tvUnbox(&result);
  }
  return result;
}

#define HELPER_TABLE(m)                                       \
  /* name           hot        key  unboxKey  isObj */        \
  m(cGetPropC,    ,            AnyKey, false, false)          \
  m(cGetPropCO,   ,            AnyKey, false,  true)          \
  m(cGetPropL,    ,            AnyKey,  true, false)          \
  m(cGetPropLO,   ,            AnyKey,  true,  true)          \
  m(cGetPropLS,   ,            StrKey,  true, false)          \
  m(cGetPropLSO,  ,            StrKey,  true,  true)          \
  m(cGetPropS,    ,            StrKey, false, false)          \
  m(cGetPropSO,   HOT_FUNC_VM, StrKey, false,  true)

#define PROP(nm, hot, ...)                                              \
hot                                                                     \
static TypedValue nm(Class* ctx, TypedValue* base, TypedValue key,      \
                     MInstrState* mis) {                                \
  return cGetPropImpl<__VA_ARGS__>(ctx, base, key, mis);                \
}
HELPER_TABLE(PROP)
#undef PROP

void HhbcTranslator::VectorTranslator::emitCGetProp() {
  assert(!m_ni.outLocal);

  const Class* knownCls = nullptr;
  const int propOffset  = getPropertyOffset(m_ni, knownCls,
                                            m_mii, m_mInd, m_iInd);
  if (propOffset != -1) {
    PUNT(CGetProp-KnownOffset);
  }

  typedef TypedValue (*OpFunc)(Class*, TypedValue*, TypedValue, MInstrState*);
  SSATmp* key = getInput(m_iInd);
  BUILD_OPTABH(getKeyTypeS(key), key->isBoxed(), m_base->isA(Type::Obj));
  m_result = m_tb.genCGetProp((TCA)opFunc, CTX(), objOrPtr(m_base),
                              noLitInt(key), genMisPtr());
}
#undef HELPER_TABLE

template <KeyType keyType, bool unboxKey>
static inline TypedValue cGetElemImpl(TypedValue* base, TypedValue keyVal,
                                      MInstrState* mis) {
  TypedValue result;
  TypedValue* key = keyPtr<keyType>(keyVal);
  key = unbox<keyType, unboxKey>(key);
  base = Elem<true, keyType>(result, mis->tvRef, base, mis->baseStrOff, key);
  if (base != &result) {
    // Save a copy of the result.
    tvDup(base, &result);
  }
  if (result.m_type == KindOfRef) {
    tvUnbox(&result);
  }
  return result;
}

#define HELPER_TABLE(m)                                          \
  /* name         hot         key   unboxKey */                  \
  m(cGetElemC,  ,            AnyKey,   false)                    \
  m(cGetElemI,  ,            IntKey,   false)                    \
  m(cGetElemL,  HOT_FUNC_VM, AnyKey,    true)                    \
  m(cGetElemLI, HOT_FUNC_VM, IntKey,    true)                    \
  m(cGetElemLS, ,            StrKey,    true)                    \
  m(cGetElemS,  HOT_FUNC_VM, StrKey,   false)

#define ELEM(nm, hot, ...)                                              \
hot                                                                     \
static TypedValue nm(TypedValue* base, TypedValue key, MInstrState* mis) { \
  return cGetElemImpl<__VA_ARGS__>(base, key, mis);                     \
}
HELPER_TABLE(ELEM)
#undef ELEM

void HhbcTranslator::VectorTranslator::emitCGetElem() {
  typedef TypedValue (*OpFunc)(TypedValue*, TypedValue, MInstrState*);
  SSATmp* key = getInput(m_iInd);
  BUILD_OPTABH(getKeyTypeIS(key), key->isBoxed());
  m_result = m_tb.genCGetElem((TCA)opFunc, ptr(m_base), key, genMisPtr());
}
#undef HELPER_TABLE

void HhbcTranslator::VectorTranslator::emitMPost() {
  // Decref stack inputs.
  unsigned nStack = 0;
  for (unsigned i = 0; i < m_ni.inputs.size(); ++i) {
    const DynLocation& input = *m_ni.inputs[i];
    switch (input.location.space) {
    case Location::Stack: {
      ++nStack;
      DataType dt = input.outerType();
      if (IS_REFCOUNTED_TYPE(dt)) {
        SKTRACE(2, m_ni.source, "%s decref stack input %u, type %s\n",
                __func__, i, tname(dt).c_str());
        m_tb.genDecRef(getInput(i));
      }
      break;
    }
    case Location::Local:
    case Location::Litstr:
    case Location::Litint:
    case Location::This: {
      // Do nothing.
      SKTRACE(2, m_ni.source, "%s (no decref needed) input %u, loc(%s, %lld)\n",
              __func__, i, input.location.spaceName(), input.location.offset);
      break;
    }

    default: not_reached();
    }
  }

  // Pop off all stack inputs
  m_ht.discard(nStack);

  // Push result
  assert(m_result);
  m_ht.push(m_result);

  // Clean up tvRef(2)
  if (nLogicalRatchets() > 1) {
    SKTRACE(2, m_ni.source, "%s decref tvRef2\n", __func__);
    m_tb.genDecRefMem(m_misBase, MISOFF(tvRef2), Type::Gen);
  }
  if (nLogicalRatchets() > 0) {
    SKTRACE(2, m_ni.source, "%s decref tvRef\n", __func__);
    m_tb.genDecRefMem(m_misBase, MISOFF(tvRef), Type::Gen);
  }
}

bool HhbcTranslator::VectorTranslator::needFirstRatchet() const {
  if (m_ni.inputs[m_mii.valCount()]->valueType() == KindOfArray) {
    switch (m_ni.immVecM[0]) {
    case MEC: case MEL: case MET: case MEI: return false;
    case MPC: case MPL: case MPT: case MW:  return true;
    default: not_reached();
    }
  }
  return true;
}

bool HhbcTranslator::VectorTranslator::needFinalRatchet() const {
  return m_mii.finalGet();
}

// Ratchet operations occur after each intermediate operation, except
// possibly the first and last (see need{First,Final}Ratchet()).  No actual
// ratchet occurs after the final operation, but this means that both tvRef
// and tvRef2 can contain references just after the final operation.  Here we
// pretend that a ratchet occurs after the final operation, i.e. a "logical"
// ratchet.  The reason for counting logical ratchets as part of the total is
// the following case, in which the logical count is 0:
//
//   (base is array)
//   BaseL
//   IssetElemL
//     no logical ratchet
//
// Following are a few more examples to make the algorithm clear:
//
//   (base is array)      (base is object)   (base is object)
//   BaseL                BaseL              BaseL
//   ElemL                ElemL              CGetPropL
//     no ratchet           ratchet            logical ratchet
//   ElemL                PropL
//     ratchet              ratchet
//   ElemL                CGetElemL
//     ratchet              logical ratchet
//   IssetElemL
//     logical ratchet
//
//   (base is array)
//   BaseL
//   ElemL
//     no ratchet
//   ElemL
//     ratchet
//   ElemL
//     logical ratchet
//   SetElemL
//     no ratchet
unsigned HhbcTranslator::VectorTranslator::nLogicalRatchets() const {
  // If we've proven elsewhere that we don't need an MInstrState struct, we
  // know this translation won't need any ratchets
  if (!m_needMIS) return 0;

  unsigned ratchets = m_ni.immVecM.size();
  if (!needFirstRatchet()) --ratchets;
  if (!needFinalRatchet()) --ratchets;
  return ratchets;
}

int HhbcTranslator::VectorTranslator::ratchetInd() const {
  return needFirstRatchet() ? int(m_mInd) : int(m_mInd) - 1;
}

} } }