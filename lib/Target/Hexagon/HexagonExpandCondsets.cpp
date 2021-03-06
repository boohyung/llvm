//===--- HexagonExpandCondsets.cpp ----------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// Replace mux instructions with the corresponding legal instructions.
// It is meant to work post-SSA, but still on virtual registers. It was
// originally placed between register coalescing and machine instruction
// scheduler.
// In this place in the optimization sequence, live interval analysis had
// been performed, and the live intervals should be preserved. A large part
// of the code deals with preserving the liveness information.
//
// Liveness tracking aside, the main functionality of this pass is divided
// into two steps. The first step is to replace an instruction
//   vreg0 = C2_mux vreg1, vreg2, vreg3
// with a pair of conditional transfers
//   vreg0 = A2_tfrt vreg1, vreg2
//   vreg0 = A2_tfrf vreg1, vreg3
// It is the intention that the execution of this pass could be terminated
// after this step, and the code generated would be functionally correct.
//
// If the uses of the source values vreg1 and vreg2 are kills, and their
// definitions are predicable, then in the second step, the conditional
// transfers will then be rewritten as predicated instructions. E.g.
//   vreg0 = A2_or vreg1, vreg2
//   vreg3 = A2_tfrt vreg99, vreg0<kill>
// will be rewritten as
//   vreg3 = A2_port vreg99, vreg1, vreg2
//
// This replacement has two variants: "up" and "down". Consider this case:
//   vreg0 = A2_or vreg1, vreg2
//   ... [intervening instructions] ...
//   vreg3 = A2_tfrt vreg99, vreg0<kill>
// variant "up":
//   vreg3 = A2_port vreg99, vreg1, vreg2
//   ... [intervening instructions, vreg0->vreg3] ...
//   [deleted]
// variant "down":
//   [deleted]
//   ... [intervening instructions] ...
//   vreg3 = A2_port vreg99, vreg1, vreg2
//
// Both, one or none of these variants may be valid, and checks are made
// to rule out inapplicable variants.
//
// As an additional optimization, before either of the two steps above is
// executed, the pass attempts to coalesce the target register with one of
// the source registers, e.g. given an instruction
//   vreg3 = C2_mux vreg0, vreg1, vreg2
// vreg3 will be coalesced with either vreg1 or vreg2. If this succeeds,
// the instruction would then be (for example)
//   vreg3 = C2_mux vreg0, vreg3, vreg2
// and, under certain circumstances, this could result in only one predicated
// instruction:
//   vreg3 = A2_tfrf vreg0, vreg2
//

// Splitting a definition of a register into two predicated transfers
// creates a complication in liveness tracking. Live interval computation
// will see both instructions as actual definitions, and will mark the
// first one as dead. The definition is not actually dead, and this
// situation will need to be fixed. For example:
//   vreg1<def,dead> = A2_tfrt ...  ; marked as dead
//   vreg1<def> = A2_tfrf ...
//
// Since any of the individual predicated transfers may end up getting
// removed (in case it is an identity copy), some pre-existing def may
// be marked as dead after live interval recomputation:
//   vreg1<def,dead> = ...          ; marked as dead
//   ...
//   vreg1<def> = A2_tfrf ...       ; if A2_tfrt is removed
// This case happens if vreg1 was used as a source in A2_tfrt, which means
// that is it actually live at the A2_tfrf, and so the now dead definition
// of vreg1 will need to be updated to non-dead at some point.
//
// This issue could be remedied by adding implicit uses to the predicated
// transfers, but this will create a problem with subsequent predication,
// since the transfers will no longer be possible to reorder. To avoid
// that, the initial splitting will not add any implicit uses. These
// implicit uses will be added later, after predication. The extra price,
// however, is that finding the locations where the implicit uses need
// to be added, and updating the live ranges will be more involved.
//
// An additional problem appears when subregister liveness tracking is
// enabled. In such a scenario, the live interval for the super-register
// will have live ranges for each subregister (i.e. subranges). This sub-
// range contains all liveness information about the subregister, except
// for one case: a "read-undef" flag from another subregister will not
// be reflected: given
//   vreg1:subreg_hireg<def,read-undef> = ...  ; "undefines" subreg_loreg
// the subrange for subreg_loreg will not have any indication that it is
// undefined at this point. Calculating subregister liveness based only
// on the information from the subrange may create a segment which spans
// over such a "read-undef" flag. This would create inconsistencies in
// the liveness data, resulting in assertions or incorrect code.
// Example:
//   vreg1:subreg_loreg<def> = ...
//   vreg1:subreg_hireg<def, read-undef> = ... ; "undefines" subreg_loreg
//   ...
//   vreg1:subreg_loreg<def> = A2_tfrt ...     ; may end up with imp-use
//                                             ; of subreg_loreg
// The remedy takes advantage of the fact, that at this point we have
// an unconditional definition of the subregister. What this means is
// that any preceding value in this subregister will be overwritten,
// or in other words, the last use before this def is a kill. This also
// implies that the first of the predicated transfers at this location
// should not have any implicit uses.
// Assume for a moment that no part of the corresponding super-register
// is used as a source. In such case, the entire super-register can be
// considered undefined immediately before this instruction. Because of
// that, we can insert an IMPLICIT_DEF of the super-register at this
// location, which will cause it to be reflected in all the associated
// subranges. What is important here is that if an IMPLICIT_DEF of
// subreg_loreg was used, we would lose the indication that subreg_hireg
// is also considered undefined. This could lead to having implicit uses
// incorrectly added.
//
// What is left is the two cases when the super-register is used as a
// source.
// * Case 1: the used part is the same as the one that is defined:
//   vreg1<def> = ...
//   ...
//   vreg1:subreg_loreg<def,read-undef> = C2_mux ..., vreg1:subreg_loreg
// In the end, the subreg_loreg should be marked as live at the point of
// the splitting:
//   vreg1:subreg_loreg<def,read-undef> = A2_tfrt ; should have imp-use
//   vreg1:subreg_loreg<def,read-undef> = A2_tfrf ; should have imp-use
// Hence, an IMPLICIT_DEF of only vreg1:subreg_hireg would be sufficient.
// * Case 2: the used part does not overlap the part being defined:
//   vreg1<def> = ...
//   ...
//   vreg1:subreg_loreg<def,read-undef> = C2_mux ..., vreg1:subreg_hireg
// For this case, we insert an IMPLICIT_DEF of vreg1:subreg_hireg after
// the C2_mux.

#define DEBUG_TYPE "expand-condsets"

#include "HexagonTargetMachine.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

using namespace llvm;

static cl::opt<unsigned> OptTfrLimit("expand-condsets-tfr-limit",
  cl::init(~0U), cl::Hidden, cl::desc("Max number of mux expansions"));
static cl::opt<unsigned> OptCoaLimit("expand-condsets-coa-limit",
  cl::init(~0U), cl::Hidden, cl::desc("Max number of segment coalescings"));

namespace llvm {
  void initializeHexagonExpandCondsetsPass(PassRegistry&);
  FunctionPass *createHexagonExpandCondsets();
}

namespace {
  class HexagonExpandCondsets : public MachineFunctionPass {
  public:
    static char ID;
    HexagonExpandCondsets() :
        MachineFunctionPass(ID), HII(0), TRI(0), MRI(0),
        LIS(0), CoaLimitActive(false),
        TfrLimitActive(false), CoaCounter(0), TfrCounter(0) {
      if (OptCoaLimit.getPosition())
        CoaLimitActive = true, CoaLimit = OptCoaLimit;
      if (OptTfrLimit.getPosition())
        TfrLimitActive = true, TfrLimit = OptTfrLimit;
      initializeHexagonExpandCondsetsPass(*PassRegistry::getPassRegistry());
    }

    const char *getPassName() const override {
      return "Hexagon Expand Condsets";
    }
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<LiveIntervals>();
      AU.addPreserved<LiveIntervals>();
      AU.addPreserved<SlotIndexes>();
      AU.addRequired<MachineDominatorTree>();
      AU.addPreserved<MachineDominatorTree>();
      MachineFunctionPass::getAnalysisUsage(AU);
    }
    bool runOnMachineFunction(MachineFunction &MF) override;

  private:
    const HexagonInstrInfo *HII;
    const TargetRegisterInfo *TRI;
    MachineDominatorTree *MDT;
    MachineRegisterInfo *MRI;
    LiveIntervals *LIS;
    std::set<MachineInstr*> LocalImpDefs;

    bool CoaLimitActive, TfrLimitActive;
    unsigned CoaLimit, TfrLimit, CoaCounter, TfrCounter;

    struct RegisterRef {
      RegisterRef(const MachineOperand &Op) : Reg(Op.getReg()),
          Sub(Op.getSubReg()) {}
      RegisterRef(unsigned R = 0, unsigned S = 0) : Reg(R), Sub(S) {}
      bool operator== (RegisterRef RR) const {
        return Reg == RR.Reg && Sub == RR.Sub;
      }
      bool operator!= (RegisterRef RR) const { return !operator==(RR); }
      bool operator< (RegisterRef RR) const {
        return Reg < RR.Reg || (Reg == RR.Reg && Sub < RR.Sub);
      }
      unsigned Reg, Sub;
    };

    typedef DenseMap<unsigned,unsigned> ReferenceMap;
    enum { Sub_Low = 0x1, Sub_High = 0x2, Sub_None = (Sub_Low | Sub_High) };
    enum { Exec_Then = 0x10, Exec_Else = 0x20 };
    unsigned getMaskForSub(unsigned Sub);
    bool isCondset(const MachineInstr *MI);
    LaneBitmask getLaneMask(unsigned Reg, unsigned Sub);

    void addRefToMap(RegisterRef RR, ReferenceMap &Map, unsigned Exec);
    bool isRefInMap(RegisterRef, ReferenceMap &Map, unsigned Exec);

    void removeImpDefSegments(LiveRange &Range);
    void updateDeadsInRange(unsigned Reg, LaneBitmask LM, LiveRange &Range);
    void updateKillFlags(unsigned Reg);
    void updateDeadFlags(unsigned Reg);
    void recalculateLiveInterval(unsigned Reg);
    void removeInstr(MachineInstr *MI);
    void updateLiveness(std::set<unsigned> &RegSet, bool Recalc,
        bool UpdateKills, bool UpdateDeads);

    unsigned getCondTfrOpcode(const MachineOperand &SO, bool Cond);
    MachineInstr *genCondTfrFor(MachineOperand &SrcOp,
        MachineBasicBlock::iterator At, unsigned DstR,
        unsigned DstSR, const MachineOperand &PredOp, bool PredSense,
        bool ReadUndef, bool ImpUse);
    bool split(MachineInstr *MI, std::set<unsigned> &UpdRegs);
    bool splitInBlock(MachineBasicBlock &B, std::set<unsigned> &UpdRegs);

    bool isPredicable(MachineInstr *MI);
    MachineInstr *getReachingDefForPred(RegisterRef RD,
        MachineBasicBlock::iterator UseIt, unsigned PredR, bool Cond);
    bool canMoveOver(MachineInstr *MI, ReferenceMap &Defs, ReferenceMap &Uses);
    bool canMoveMemTo(MachineInstr *MI, MachineInstr *ToI, bool IsDown);
    void predicateAt(const MachineOperand &DefOp, MachineInstr *MI,
        MachineBasicBlock::iterator Where, const MachineOperand &PredOp,
        bool Cond, std::set<unsigned> &UpdRegs);
    void renameInRange(RegisterRef RO, RegisterRef RN, unsigned PredR,
        bool Cond, MachineBasicBlock::iterator First,
        MachineBasicBlock::iterator Last);
    bool predicate(MachineInstr *TfrI, bool Cond,
        std::set<unsigned> &UpdRegs);
    bool predicateInBlock(MachineBasicBlock &B,
        std::set<unsigned> &UpdRegs);

    bool isIntReg(RegisterRef RR, unsigned &BW);
    bool isIntraBlocks(LiveInterval &LI);
    bool coalesceRegisters(RegisterRef R1, RegisterRef R2);
    bool coalesceSegments(MachineFunction &MF);
  };
}

char HexagonExpandCondsets::ID = 0;

INITIALIZE_PASS_BEGIN(HexagonExpandCondsets, "expand-condsets",
  "Hexagon Expand Condsets", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(SlotIndexes)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(HexagonExpandCondsets, "expand-condsets",
  "Hexagon Expand Condsets", false, false)

unsigned HexagonExpandCondsets::getMaskForSub(unsigned Sub) {
  switch (Sub) {
    case Hexagon::subreg_loreg:
      return Sub_Low;
    case Hexagon::subreg_hireg:
      return Sub_High;
    case Hexagon::NoSubRegister:
      return Sub_None;
  }
  llvm_unreachable("Invalid subregister");
}


bool HexagonExpandCondsets::isCondset(const MachineInstr *MI) {
  unsigned Opc = MI->getOpcode();
  switch (Opc) {
    case Hexagon::C2_mux:
    case Hexagon::C2_muxii:
    case Hexagon::C2_muxir:
    case Hexagon::C2_muxri:
    case Hexagon::MUX64_rr:
        return true;
      break;
  }
  return false;
}


LaneBitmask HexagonExpandCondsets::getLaneMask(unsigned Reg, unsigned Sub) {
  assert(TargetRegisterInfo::isVirtualRegister(Reg));
  return Sub != 0 ? TRI->getSubRegIndexLaneMask(Sub)
                  : MRI->getMaxLaneMaskForVReg(Reg);
}


void HexagonExpandCondsets::addRefToMap(RegisterRef RR, ReferenceMap &Map,
      unsigned Exec) {
  unsigned Mask = getMaskForSub(RR.Sub) | Exec;
  ReferenceMap::iterator F = Map.find(RR.Reg);
  if (F == Map.end())
    Map.insert(std::make_pair(RR.Reg, Mask));
  else
    F->second |= Mask;
}


bool HexagonExpandCondsets::isRefInMap(RegisterRef RR, ReferenceMap &Map,
      unsigned Exec) {
  ReferenceMap::iterator F = Map.find(RR.Reg);
  if (F == Map.end())
    return false;
  unsigned Mask = getMaskForSub(RR.Sub) | Exec;
  if (Mask & F->second)
    return true;
  return false;
}


void HexagonExpandCondsets::updateKillFlags(unsigned Reg) {
  auto KillAt = [this,Reg] (SlotIndex K, LaneBitmask LM) -> void {
    // Set the <kill> flag on a use of Reg whose lane mask is contained in LM.
    MachineInstr *MI = LIS->getInstructionFromIndex(K);
    for (auto &Op : MI->operands()) {
      if (!Op.isReg() || !Op.isUse() || Op.getReg() != Reg)
        continue;
      LaneBitmask SLM = getLaneMask(Reg, Op.getSubReg());
      if ((SLM & LM) == SLM) {
        // Only set the kill flag on the first encountered use of Reg in this
        // instruction.
        Op.setIsKill(true);
        break;
      }
    }
  };

  LiveInterval &LI = LIS->getInterval(Reg);
  for (auto I = LI.begin(), E = LI.end(); I != E; ++I) {
    if (!I->end.isRegister())
      continue;
    // Do not mark the end of the segment as <kill>, if the next segment
    // starts with a predicated instruction.
    auto NextI = std::next(I);
    if (NextI != E && NextI->start.isRegister()) {
      MachineInstr *DefI = LIS->getInstructionFromIndex(NextI->start);
      if (HII->isPredicated(*DefI))
        continue;
    }
    bool WholeReg = true;
    if (LI.hasSubRanges()) {
      auto EndsAtI = [I] (LiveInterval::SubRange &S) -> bool {
        LiveRange::iterator F = S.find(I->end);
        return F != S.end() && I->end == F->end;
      };
      // Check if all subranges end at I->end. If so, make sure to kill
      // the whole register.
      for (LiveInterval::SubRange &S : LI.subranges()) {
        if (EndsAtI(S))
          KillAt(I->end, S.LaneMask);
        else
          WholeReg = false;
      }
    }
    if (WholeReg)
      KillAt(I->end, MRI->getMaxLaneMaskForVReg(Reg));
  }
}


void HexagonExpandCondsets::removeImpDefSegments(LiveRange &Range) {
  auto StartImpDef = [this] (LiveRange::Segment &S) -> bool {
    return S.start.isRegister() &&
           LocalImpDefs.count(LIS->getInstructionFromIndex(S.start));
  };
  Range.segments.erase(std::remove_if(Range.begin(), Range.end(), StartImpDef),
                       Range.end());
}

void HexagonExpandCondsets::updateDeadsInRange(unsigned Reg, LaneBitmask LM,
      LiveRange &Range) {
  assert(TargetRegisterInfo::isVirtualRegister(Reg));
  if (Range.empty())
    return;

  auto IsRegDef = [this,Reg,LM] (MachineOperand &Op) -> bool {
    if (!Op.isReg() || !Op.isDef())
      return false;
    unsigned DR = Op.getReg(), DSR = Op.getSubReg();
    if (!TargetRegisterInfo::isVirtualRegister(DR) || DR != Reg)
      return false;
    LaneBitmask SLM = getLaneMask(DR, DSR);
    return (SLM & LM) != 0;
  };

  // The splitting step will create pairs of predicated definitions without
  // any implicit uses (since implicit uses would interfere with predication).
  // This can cause the reaching defs to become dead after live range
  // recomputation, even though they are not really dead.
  // We need to identify predicated defs that need implicit uses, and
  // dead defs that are not really dead, and correct both problems.

  SetVector<MachineBasicBlock*> Defs;
  auto Dominate = [this] (SetVector<MachineBasicBlock*> &Defs,
                          MachineBasicBlock *Dest) -> bool {
    for (MachineBasicBlock *D : Defs)
      if (D != Dest && MDT->dominates(D, Dest))
        return true;

    MachineBasicBlock *Entry = &Dest->getParent()->front();
    SetVector<MachineBasicBlock*> Work(Dest->pred_begin(), Dest->pred_end());
    for (unsigned i = 0; i < Work.size(); ++i) {
      MachineBasicBlock *B = Work[i];
      if (Defs.count(B))
        continue;
      if (B == Entry)
        return false;
      for (auto *P : B->predecessors())
        Work.insert(P);
    }
    return true;
  };

  // First, try to extend live range within individual basic blocks. This
  // will leave us only with dead defs that do not reach any predicated
  // defs in the same block.
  SmallVector<SlotIndex,4> PredDefs;
  for (auto &Seg : Range) {
    if (!Seg.start.isRegister())
      continue;
    MachineInstr *DefI = LIS->getInstructionFromIndex(Seg.start);
    if (LocalImpDefs.count(DefI))
      continue;
    Defs.insert(DefI->getParent());
    if (HII->isPredicated(*DefI))
      PredDefs.push_back(Seg.start);
  }
  for (auto &SI : PredDefs) {
    MachineBasicBlock *BB = LIS->getMBBFromIndex(SI);
    if (Range.extendInBlock(LIS->getMBBStartIdx(BB), SI))
      SI = SlotIndex();
  }

  // Calculate reachability for those predicated defs that were not handled
  // by the in-block extension.
  SmallVector<SlotIndex,4> ExtTo;
  for (auto &SI : PredDefs) {
    if (!SI.isValid())
      continue;
    MachineBasicBlock *BB = LIS->getMBBFromIndex(SI);
    if (BB->pred_empty())
      continue;
    // If the defs from this range reach SI via all predecessors, it is live.
    if (Dominate(Defs, BB))
      ExtTo.push_back(SI);
  }
  LIS->extendToIndices(Range, ExtTo);

  // Remove <dead> flags from all defs that are not dead after live range
  // extension, and collect all def operands. They will be used to generate
  // the necessary implicit uses.
  std::set<RegisterRef> DefRegs;
  for (auto &Seg : Range) {
    if (!Seg.start.isRegister())
      continue;
    MachineInstr *DefI = LIS->getInstructionFromIndex(Seg.start);
    if (LocalImpDefs.count(DefI))
      continue;
    for (auto &Op : DefI->operands()) {
      if (Seg.start.isDead() || !IsRegDef(Op))
        continue;
      DefRegs.insert(Op);
      Op.setIsDead(false);
    }
  }


  // Finally, add implicit uses to each predicated def that is reached
  // by other defs. Remove segments started by implicit-defs first, since
  // they do not define registers.
  removeImpDefSegments(Range);

  for (auto &Seg : Range) {
    if (!Seg.start.isRegister() || !Range.liveAt(Seg.start.getPrevSlot()))
      continue;
    MachineInstr *DefI = LIS->getInstructionFromIndex(Seg.start);
    if (!HII->isPredicated(*DefI))
      continue;
    MachineFunction &MF = *DefI->getParent()->getParent();
    // Construct the set of all necessary implicit uses, based on the def
    // operands in the instruction.
    std::set<RegisterRef> ImpUses;
    for (auto &Op : DefI->operands())
      if (Op.isReg() && Op.isDef() && DefRegs.count(Op))
        ImpUses.insert(Op);
    for (RegisterRef R : ImpUses)
      MachineInstrBuilder(MF, DefI).addReg(R.Reg, RegState::Implicit, R.Sub);
  }
}


void HexagonExpandCondsets::updateDeadFlags(unsigned Reg) {
  LiveInterval &LI = LIS->getInterval(Reg);
  if (LI.hasSubRanges()) {
    for (LiveInterval::SubRange &S : LI.subranges()) {
      updateDeadsInRange(Reg, S.LaneMask, S);
      LIS->shrinkToUses(S, Reg);
      // LI::shrinkToUses will add segments started by implicit-defs.
      // Remove them again.
      removeImpDefSegments(S);
    }
    LI.clear();
    LIS->constructMainRangeFromSubranges(LI);
  } else {
    updateDeadsInRange(Reg, MRI->getMaxLaneMaskForVReg(Reg), LI);
  }
}


void HexagonExpandCondsets::recalculateLiveInterval(unsigned Reg) {
  LIS->removeInterval(Reg);
  LIS->createAndComputeVirtRegInterval(Reg);
}


void HexagonExpandCondsets::removeInstr(MachineInstr *MI) {
  LIS->RemoveMachineInstrFromMaps(*MI);
  MI->eraseFromParent();
}


void HexagonExpandCondsets::updateLiveness(std::set<unsigned> &RegSet,
      bool Recalc, bool UpdateKills, bool UpdateDeads) {
  UpdateKills |= UpdateDeads;
  for (auto R : RegSet) {
    if (Recalc)
      recalculateLiveInterval(R);
    if (UpdateKills)
      MRI->clearKillFlags(R);
    if (UpdateDeads)
      updateDeadFlags(R);
    // Fixing <dead> flags may extend live ranges, so reset <kill> flags
    // after that.
    if (UpdateKills)
      updateKillFlags(R);
    LIS->getInterval(R).verify();
  }
}


/// Get the opcode for a conditional transfer of the value in SO (source
/// operand). The condition (true/false) is given in Cond.
unsigned HexagonExpandCondsets::getCondTfrOpcode(const MachineOperand &SO,
      bool IfTrue) {
  using namespace Hexagon;
  if (SO.isReg()) {
    unsigned PhysR;
    RegisterRef RS = SO;
    if (TargetRegisterInfo::isVirtualRegister(RS.Reg)) {
      const TargetRegisterClass *VC = MRI->getRegClass(RS.Reg);
      assert(VC->begin() != VC->end() && "Empty register class");
      PhysR = *VC->begin();
    } else {
      assert(TargetRegisterInfo::isPhysicalRegister(RS.Reg));
      PhysR = RS.Reg;
    }
    unsigned PhysS = (RS.Sub == 0) ? PhysR : TRI->getSubReg(PhysR, RS.Sub);
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(PhysS);
    switch (RC->getSize()) {
      case 4:
        return IfTrue ? A2_tfrt : A2_tfrf;
      case 8:
        return IfTrue ? A2_tfrpt : A2_tfrpf;
    }
    llvm_unreachable("Invalid register operand");
  }
  if (SO.isImm() || SO.isFPImm())
    return IfTrue ? C2_cmoveit : C2_cmoveif;
  llvm_unreachable("Unexpected source operand");
}


/// Generate a conditional transfer, copying the value SrcOp to the
/// destination register DstR:DstSR, and using the predicate register from
/// PredOp. The Cond argument specifies whether the predicate is to be
/// if(PredOp), or if(!PredOp).
MachineInstr *HexagonExpandCondsets::genCondTfrFor(MachineOperand &SrcOp,
      MachineBasicBlock::iterator At,
      unsigned DstR, unsigned DstSR, const MachineOperand &PredOp,
      bool PredSense, bool ReadUndef, bool ImpUse) {
  MachineInstr *MI = SrcOp.getParent();
  MachineBasicBlock &B = *At->getParent();
  const DebugLoc &DL = MI->getDebugLoc();

  // Don't avoid identity copies here (i.e. if the source and the destination
  // are the same registers). It is actually better to generate them here,
  // since this would cause the copy to potentially be predicated in the next
  // step. The predication will remove such a copy if it is unable to
  /// predicate.

  unsigned Opc = getCondTfrOpcode(SrcOp, PredSense);
  unsigned State = RegState::Define | (ReadUndef ? RegState::Undef : 0);
  MachineInstrBuilder MIB = BuildMI(B, At, DL, HII->get(Opc))
        .addReg(DstR, State, DstSR)
        .addOperand(PredOp)
        .addOperand(SrcOp);

  // We don't want any kills yet.
  MIB->clearKillInfo();
  DEBUG(dbgs() << "created an initial copy: " << *MIB);
  return &*MIB;
}


/// Replace a MUX instruction MI with a pair A2_tfrt/A2_tfrf. This function
/// performs all necessary changes to complete the replacement.
bool HexagonExpandCondsets::split(MachineInstr *MI,
      std::set<unsigned> &UpdRegs) {
  if (TfrLimitActive) {
    if (TfrCounter >= TfrLimit)
      return false;
    TfrCounter++;
  }
  DEBUG(dbgs() << "\nsplitting BB#" << MI->getParent()->getNumber()
               << ": " << *MI);
  MachineOperand &MD  = MI->getOperand(0); // Definition
  MachineOperand &MP  = MI->getOperand(1); // Predicate register
  MachineOperand &MS1 = MI->getOperand(2); // Source value #1
  MachineOperand &MS2 = MI->getOperand(3); // Source value #2
  assert(MD.isDef());
  unsigned DR = MD.getReg(), DSR = MD.getSubReg();
  bool ReadUndef = MD.isUndef();
  MachineBasicBlock::iterator At = MI;

  if (ReadUndef && DSR != 0 && MRI->shouldTrackSubRegLiveness(DR)) {
    unsigned NewSR = 0;
    MachineBasicBlock::iterator DefAt = At;
    bool SameReg = (MS1.isReg() && DR == MS1.getReg()) ||
                   (MS2.isReg() && DR == MS2.getReg());
    if (SameReg) {
      NewSR = (DSR == Hexagon::subreg_loreg) ? Hexagon::subreg_hireg
                                             : Hexagon::subreg_loreg;
      // Advance the insertion point if the subregisters differ between
      // the source and the target (with the same super-register).
      // Note: this case has never occured during tests.
      if ((MS1.isReg() && NewSR == MS1.getSubReg()) ||
          (MS2.isReg() && NewSR == MS2.getSubReg()))
        ++DefAt;
    }
    // Use "At", since "DefAt" may be end().
    MachineBasicBlock &B = *At->getParent();
    DebugLoc DL = At->getDebugLoc();
    auto ImpD = BuildMI(B, DefAt, DL, HII->get(TargetOpcode::IMPLICIT_DEF))
                  .addReg(DR, RegState::Define, NewSR);
    LIS->InsertMachineInstrInMaps(*ImpD);
    LocalImpDefs.insert(&*ImpD);
  }

  // First, create the two invididual conditional transfers, and add each
  // of them to the live intervals information. Do that first and then remove
  // the old instruction from live intervals.
  MachineInstr *TfrT = genCondTfrFor(MI->getOperand(2), At, DR, DSR, MP, true,
                                     ReadUndef, false);
  MachineInstr *TfrF = genCondTfrFor(MI->getOperand(3), At, DR, DSR, MP, false,
                                     ReadUndef, true);
  LIS->InsertMachineInstrInMaps(*TfrT);
  LIS->InsertMachineInstrInMaps(*TfrF);

  // Will need to recalculate live intervals for all registers in MI.
  for (auto &Op : MI->operands())
    if (Op.isReg())
      UpdRegs.insert(Op.getReg());

  removeInstr(MI);
  return true;
}


/// Split all MUX instructions in the given block into pairs of conditional
/// transfers.
bool HexagonExpandCondsets::splitInBlock(MachineBasicBlock &B,
      std::set<unsigned> &UpdRegs) {
  bool Changed = false;
  MachineBasicBlock::iterator I, E, NextI;
  for (I = B.begin(), E = B.end(); I != E; I = NextI) {
    NextI = std::next(I);
    if (isCondset(I))
      Changed |= split(I, UpdRegs);
  }
  return Changed;
}


bool HexagonExpandCondsets::isPredicable(MachineInstr *MI) {
  if (HII->isPredicated(*MI) || !HII->isPredicable(*MI))
    return false;
  if (MI->hasUnmodeledSideEffects() || MI->mayStore())
    return false;
  // Reject instructions with multiple defs (e.g. post-increment loads).
  bool HasDef = false;
  for (auto &Op : MI->operands()) {
    if (!Op.isReg() || !Op.isDef())
      continue;
    if (HasDef)
      return false;
    HasDef = true;
  }
  for (auto &Mo : MI->memoperands())
    if (Mo->isVolatile())
      return false;
  return true;
}


/// Find the reaching definition for a predicated use of RD. The RD is used
/// under the conditions given by PredR and Cond, and this function will ignore
/// definitions that set RD under the opposite conditions.
MachineInstr *HexagonExpandCondsets::getReachingDefForPred(RegisterRef RD,
      MachineBasicBlock::iterator UseIt, unsigned PredR, bool Cond) {
  MachineBasicBlock &B = *UseIt->getParent();
  MachineBasicBlock::iterator I = UseIt, S = B.begin();
  if (I == S)
    return 0;

  bool PredValid = true;
  do {
    --I;
    MachineInstr *MI = &*I;
    // Check if this instruction can be ignored, i.e. if it is predicated
    // on the complementary condition.
    if (PredValid && HII->isPredicated(*MI)) {
      if (MI->readsRegister(PredR) && (Cond != HII->isPredicatedTrue(*MI)))
        continue;
    }

    // Check the defs. If the PredR is defined, invalidate it. If RD is
    // defined, return the instruction or 0, depending on the circumstances.
    for (auto &Op : MI->operands()) {
      if (!Op.isReg() || !Op.isDef())
        continue;
      RegisterRef RR = Op;
      if (RR.Reg == PredR) {
        PredValid = false;
        continue;
      }
      if (RR.Reg != RD.Reg)
        continue;
      // If the "Reg" part agrees, there is still the subregister to check.
      // If we are looking for vreg1:loreg, we can skip vreg1:hireg, but
      // not vreg1 (w/o subregisters).
      if (RR.Sub == RD.Sub)
        return MI;
      if (RR.Sub == 0 || RD.Sub == 0)
        return 0;
      // We have different subregisters, so we can continue looking.
    }
  } while (I != S);

  return 0;
}


/// Check if the instruction MI can be safely moved over a set of instructions
/// whose side-effects (in terms of register defs and uses) are expressed in
/// the maps Defs and Uses. These maps reflect the conditional defs and uses
/// that depend on the same predicate register to allow moving instructions
/// over instructions predicated on the opposite condition.
bool HexagonExpandCondsets::canMoveOver(MachineInstr *MI, ReferenceMap &Defs,
      ReferenceMap &Uses) {
  // In order to be able to safely move MI over instructions that define
  // "Defs" and use "Uses", no def operand from MI can be defined or used
  // and no use operand can be defined.
  for (auto &Op : MI->operands()) {
    if (!Op.isReg())
      continue;
    RegisterRef RR = Op;
    // For physical register we would need to check register aliases, etc.
    // and we don't want to bother with that. It would be of little value
    // before the actual register rewriting (from virtual to physical).
    if (!TargetRegisterInfo::isVirtualRegister(RR.Reg))
      return false;
    // No redefs for any operand.
    if (isRefInMap(RR, Defs, Exec_Then))
      return false;
    // For defs, there cannot be uses.
    if (Op.isDef() && isRefInMap(RR, Uses, Exec_Then))
      return false;
  }
  return true;
}


/// Check if the instruction accessing memory (TheI) can be moved to the
/// location ToI.
bool HexagonExpandCondsets::canMoveMemTo(MachineInstr *TheI, MachineInstr *ToI,
      bool IsDown) {
  bool IsLoad = TheI->mayLoad(), IsStore = TheI->mayStore();
  if (!IsLoad && !IsStore)
    return true;
  if (HII->areMemAccessesTriviallyDisjoint(TheI, ToI))
    return true;
  if (TheI->hasUnmodeledSideEffects())
    return false;

  MachineBasicBlock::iterator StartI = IsDown ? TheI : ToI;
  MachineBasicBlock::iterator EndI = IsDown ? ToI : TheI;
  bool Ordered = TheI->hasOrderedMemoryRef();

  // Search for aliased memory reference in (StartI, EndI).
  for (MachineBasicBlock::iterator I = std::next(StartI); I != EndI; ++I) {
    MachineInstr *MI = &*I;
    if (MI->hasUnmodeledSideEffects())
      return false;
    bool L = MI->mayLoad(), S = MI->mayStore();
    if (!L && !S)
      continue;
    if (Ordered && MI->hasOrderedMemoryRef())
      return false;

    bool Conflict = (L && IsStore) || S;
    if (Conflict)
      return false;
  }
  return true;
}


/// Generate a predicated version of MI (where the condition is given via
/// PredR and Cond) at the point indicated by Where.
void HexagonExpandCondsets::predicateAt(const MachineOperand &DefOp,
      MachineInstr *MI, MachineBasicBlock::iterator Where,
      const MachineOperand &PredOp, bool Cond, std::set<unsigned> &UpdRegs) {
  // The problem with updating live intervals is that we can move one def
  // past another def. In particular, this can happen when moving an A2_tfrt
  // over an A2_tfrf defining the same register. From the point of view of
  // live intervals, these two instructions are two separate definitions,
  // and each one starts another live segment. LiveIntervals's "handleMove"
  // does not allow such moves, so we need to handle it ourselves. To avoid
  // invalidating liveness data while we are using it, the move will be
  // implemented in 4 steps: (1) add a clone of the instruction MI at the
  // target location, (2) update liveness, (3) delete the old instruction,
  // and (4) update liveness again.

  MachineBasicBlock &B = *MI->getParent();
  DebugLoc DL = Where->getDebugLoc();  // "Where" points to an instruction.
  unsigned Opc = MI->getOpcode();
  unsigned PredOpc = HII->getCondOpcode(Opc, !Cond);
  MachineInstrBuilder MB = BuildMI(B, Where, DL, HII->get(PredOpc));
  unsigned Ox = 0, NP = MI->getNumOperands();
  // Skip all defs from MI first.
  while (Ox < NP) {
    MachineOperand &MO = MI->getOperand(Ox);
    if (!MO.isReg() || !MO.isDef())
      break;
    Ox++;
  }
  // Add the new def, then the predicate register, then the rest of the
  // operands.
  MB.addReg(DefOp.getReg(), getRegState(DefOp), DefOp.getSubReg());
  MB.addReg(PredOp.getReg(), PredOp.isUndef() ? RegState::Undef : 0,
            PredOp.getSubReg());
  while (Ox < NP) {
    MachineOperand &MO = MI->getOperand(Ox);
    if (!MO.isReg() || !MO.isImplicit())
      MB.addOperand(MO);
    Ox++;
  }

  MachineFunction &MF = *B.getParent();
  MachineInstr::mmo_iterator I = MI->memoperands_begin();
  unsigned NR = std::distance(I, MI->memoperands_end());
  MachineInstr::mmo_iterator MemRefs = MF.allocateMemRefsArray(NR);
  for (unsigned i = 0; i < NR; ++i)
    MemRefs[i] = *I++;
  MB.setMemRefs(MemRefs, MemRefs+NR);

  MachineInstr *NewI = MB;
  NewI->clearKillInfo();
  LIS->InsertMachineInstrInMaps(*NewI);

  for (auto &Op : NewI->operands())
    if (Op.isReg())
      UpdRegs.insert(Op.getReg());
}


/// In the range [First, Last], rename all references to the "old" register RO
/// to the "new" register RN, but only in instructions predicated on the given
/// condition.
void HexagonExpandCondsets::renameInRange(RegisterRef RO, RegisterRef RN,
      unsigned PredR, bool Cond, MachineBasicBlock::iterator First,
      MachineBasicBlock::iterator Last) {
  MachineBasicBlock::iterator End = std::next(Last);
  for (MachineBasicBlock::iterator I = First; I != End; ++I) {
    MachineInstr *MI = &*I;
    // Do not touch instructions that are not predicated, or are predicated
    // on the opposite condition.
    if (!HII->isPredicated(*MI))
      continue;
    if (!MI->readsRegister(PredR) || (Cond != HII->isPredicatedTrue(*MI)))
      continue;

    for (auto &Op : MI->operands()) {
      if (!Op.isReg() || RO != RegisterRef(Op))
        continue;
      Op.setReg(RN.Reg);
      Op.setSubReg(RN.Sub);
      // In practice, this isn't supposed to see any defs.
      assert(!Op.isDef() && "Not expecting a def");
    }
  }
}


/// For a given conditional copy, predicate the definition of the source of
/// the copy under the given condition (using the same predicate register as
/// the copy).
bool HexagonExpandCondsets::predicate(MachineInstr *TfrI, bool Cond,
      std::set<unsigned> &UpdRegs) {
  // TfrI - A2_tfr[tf] Instruction (not A2_tfrsi).
  unsigned Opc = TfrI->getOpcode();
  (void)Opc;
  assert(Opc == Hexagon::A2_tfrt || Opc == Hexagon::A2_tfrf);
  DEBUG(dbgs() << "\nattempt to predicate if-" << (Cond ? "true" : "false")
               << ": " << *TfrI);

  MachineOperand &MD = TfrI->getOperand(0);
  MachineOperand &MP = TfrI->getOperand(1);
  MachineOperand &MS = TfrI->getOperand(2);
  // The source operand should be a <kill>. This is not strictly necessary,
  // but it makes things a lot simpler. Otherwise, we would need to rename
  // some registers, which would complicate the transformation considerably.
  if (!MS.isKill())
    return false;
  // Avoid predicating instructions that define a subregister if subregister
  // liveness tracking is not enabled.
  if (MD.getSubReg() && !MRI->shouldTrackSubRegLiveness(MD.getReg()))
    return false;

  RegisterRef RT(MS);
  unsigned PredR = MP.getReg();
  MachineInstr *DefI = getReachingDefForPred(RT, TfrI, PredR, Cond);
  if (!DefI || !isPredicable(DefI))
    return false;

  DEBUG(dbgs() << "Source def: " << *DefI);

  // Collect the information about registers defined and used between the
  // DefI and the TfrI.
  // Map: reg -> bitmask of subregs
  ReferenceMap Uses, Defs;
  MachineBasicBlock::iterator DefIt = DefI, TfrIt = TfrI;

  // Check if the predicate register is valid between DefI and TfrI.
  // If it is, we can then ignore instructions predicated on the negated
  // conditions when collecting def and use information.
  bool PredValid = true;
  for (MachineBasicBlock::iterator I = std::next(DefIt); I != TfrIt; ++I) {
    if (!I->modifiesRegister(PredR, 0))
      continue;
    PredValid = false;
    break;
  }

  for (MachineBasicBlock::iterator I = std::next(DefIt); I != TfrIt; ++I) {
    MachineInstr *MI = &*I;
    // If this instruction is predicated on the same register, it could
    // potentially be ignored.
    // By default assume that the instruction executes on the same condition
    // as TfrI (Exec_Then), and also on the opposite one (Exec_Else).
    unsigned Exec = Exec_Then | Exec_Else;
    if (PredValid && HII->isPredicated(*MI) && MI->readsRegister(PredR))
      Exec = (Cond == HII->isPredicatedTrue(*MI)) ? Exec_Then : Exec_Else;

    for (auto &Op : MI->operands()) {
      if (!Op.isReg())
        continue;
      // We don't want to deal with physical registers. The reason is that
      // they can be aliased with other physical registers. Aliased virtual
      // registers must share the same register number, and can only differ
      // in the subregisters, which we are keeping track of. Physical
      // registers ters no longer have subregisters---their super- and
      // subregisters are other physical registers, and we are not checking
      // that.
      RegisterRef RR = Op;
      if (!TargetRegisterInfo::isVirtualRegister(RR.Reg))
        return false;

      ReferenceMap &Map = Op.isDef() ? Defs : Uses;
      addRefToMap(RR, Map, Exec);
    }
  }

  // The situation:
  //   RT = DefI
  //   ...
  //   RD = TfrI ..., RT

  // If the register-in-the-middle (RT) is used or redefined between
  // DefI and TfrI, we may not be able proceed with this transformation.
  // We can ignore a def that will not execute together with TfrI, and a
  // use that will. If there is such a use (that does execute together with
  // TfrI), we will not be able to move DefI down. If there is a use that
  // executed if TfrI's condition is false, then RT must be available
  // unconditionally (cannot be predicated).
  // Essentially, we need to be able to rename RT to RD in this segment.
  if (isRefInMap(RT, Defs, Exec_Then) || isRefInMap(RT, Uses, Exec_Else))
    return false;
  RegisterRef RD = MD;
  // If the predicate register is defined between DefI and TfrI, the only
  // potential thing to do would be to move the DefI down to TfrI, and then
  // predicate. The reaching def (DefI) must be movable down to the location
  // of the TfrI.
  // If the target register of the TfrI (RD) is not used or defined between
  // DefI and TfrI, consider moving TfrI up to DefI.
  bool CanUp =   canMoveOver(TfrI, Defs, Uses);
  bool CanDown = canMoveOver(DefI, Defs, Uses);
  // The TfrI does not access memory, but DefI could. Check if it's safe
  // to move DefI down to TfrI.
  if (DefI->mayLoad() || DefI->mayStore())
    if (!canMoveMemTo(DefI, TfrI, true))
      CanDown = false;

  DEBUG(dbgs() << "Can move up: " << (CanUp ? "yes" : "no")
               << ", can move down: " << (CanDown ? "yes\n" : "no\n"));
  MachineBasicBlock::iterator PastDefIt = std::next(DefIt);
  if (CanUp)
    predicateAt(MD, DefI, PastDefIt, MP, Cond, UpdRegs);
  else if (CanDown)
    predicateAt(MD, DefI, TfrIt, MP, Cond, UpdRegs);
  else
    return false;

  if (RT != RD) {
    renameInRange(RT, RD, PredR, Cond, PastDefIt, TfrIt);
    UpdRegs.insert(RT.Reg);
  }

  removeInstr(TfrI);
  removeInstr(DefI);
  return true;
}


/// Predicate all cases of conditional copies in the specified block.
bool HexagonExpandCondsets::predicateInBlock(MachineBasicBlock &B,
      std::set<unsigned> &UpdRegs) {
  bool Changed = false;
  MachineBasicBlock::iterator I, E, NextI;
  for (I = B.begin(), E = B.end(); I != E; I = NextI) {
    NextI = std::next(I);
    unsigned Opc = I->getOpcode();
    if (Opc == Hexagon::A2_tfrt || Opc == Hexagon::A2_tfrf) {
      bool Done = predicate(I, (Opc == Hexagon::A2_tfrt), UpdRegs);
      if (!Done) {
        // If we didn't predicate I, we may need to remove it in case it is
        // an "identity" copy, e.g.  vreg1 = A2_tfrt vreg2, vreg1.
        if (RegisterRef(I->getOperand(0)) == RegisterRef(I->getOperand(2))) {
          for (auto &Op : I->operands())
            if (Op.isReg())
              UpdRegs.insert(Op.getReg());
          removeInstr(&*I);
        }
      }
      Changed |= Done;
    }
  }
  return Changed;
}


bool HexagonExpandCondsets::isIntReg(RegisterRef RR, unsigned &BW) {
  if (!TargetRegisterInfo::isVirtualRegister(RR.Reg))
    return false;
  const TargetRegisterClass *RC = MRI->getRegClass(RR.Reg);
  if (RC == &Hexagon::IntRegsRegClass) {
    BW = 32;
    return true;
  }
  if (RC == &Hexagon::DoubleRegsRegClass) {
    BW = (RR.Sub != 0) ? 32 : 64;
    return true;
  }
  return false;
}


bool HexagonExpandCondsets::isIntraBlocks(LiveInterval &LI) {
  for (LiveInterval::iterator I = LI.begin(), E = LI.end(); I != E; ++I) {
    LiveRange::Segment &LR = *I;
    // Range must start at a register...
    if (!LR.start.isRegister())
      return false;
    // ...and end in a register or in a dead slot.
    if (!LR.end.isRegister() && !LR.end.isDead())
      return false;
  }
  return true;
}


bool HexagonExpandCondsets::coalesceRegisters(RegisterRef R1, RegisterRef R2) {
  if (CoaLimitActive) {
    if (CoaCounter >= CoaLimit)
      return false;
    CoaCounter++;
  }
  unsigned BW1, BW2;
  if (!isIntReg(R1, BW1) || !isIntReg(R2, BW2) || BW1 != BW2)
    return false;
  if (MRI->isLiveIn(R1.Reg))
    return false;
  if (MRI->isLiveIn(R2.Reg))
    return false;

  LiveInterval &L1 = LIS->getInterval(R1.Reg);
  LiveInterval &L2 = LIS->getInterval(R2.Reg);
  bool Overlap = L1.overlaps(L2);

  DEBUG(dbgs() << "compatible registers: ("
               << (Overlap ? "overlap" : "disjoint") << ")\n  "
               << PrintReg(R1.Reg, TRI, R1.Sub) << "  " << L1 << "\n  "
               << PrintReg(R2.Reg, TRI, R2.Sub) << "  " << L2 << "\n");
  if (R1.Sub || R2.Sub)
    return false;
  if (Overlap)
    return false;

  // Coalescing could have a negative impact on scheduling, so try to limit
  // to some reasonable extent. Only consider coalescing segments, when one
  // of them does not cross basic block boundaries.
  if (!isIntraBlocks(L1) && !isIntraBlocks(L2))
    return false;

  MRI->replaceRegWith(R2.Reg, R1.Reg);

  // Move all live segments from L2 to L1.
  typedef DenseMap<VNInfo*,VNInfo*> ValueInfoMap;
  ValueInfoMap VM;
  for (LiveInterval::iterator I = L2.begin(), E = L2.end(); I != E; ++I) {
    VNInfo *NewVN, *OldVN = I->valno;
    ValueInfoMap::iterator F = VM.find(OldVN);
    if (F == VM.end()) {
      NewVN = L1.getNextValue(I->valno->def, LIS->getVNInfoAllocator());
      VM.insert(std::make_pair(OldVN, NewVN));
    } else {
      NewVN = F->second;
    }
    L1.addSegment(LiveRange::Segment(I->start, I->end, NewVN));
  }
  while (L2.begin() != L2.end())
    L2.removeSegment(*L2.begin());

  updateKillFlags(R1.Reg);
  DEBUG(dbgs() << "coalesced: " << L1 << "\n");
  L1.verify();

  return true;
}


/// Attempt to coalesce one of the source registers to a MUX intruction with
/// the destination register. This could lead to having only one predicated
/// instruction in the end instead of two.
bool HexagonExpandCondsets::coalesceSegments(MachineFunction &MF) {
  SmallVector<MachineInstr*,16> Condsets;
  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I) {
    MachineBasicBlock &B = *I;
    for (MachineBasicBlock::iterator J = B.begin(), F = B.end(); J != F; ++J) {
      MachineInstr *MI = &*J;
      if (!isCondset(MI))
        continue;
      MachineOperand &S1 = MI->getOperand(2), &S2 = MI->getOperand(3);
      if (!S1.isReg() && !S2.isReg())
        continue;
      Condsets.push_back(MI);
    }
  }

  bool Changed = false;
  for (unsigned i = 0, n = Condsets.size(); i < n; ++i) {
    MachineInstr *CI = Condsets[i];
    RegisterRef RD = CI->getOperand(0);
    RegisterRef RP = CI->getOperand(1);
    MachineOperand &S1 = CI->getOperand(2), &S2 = CI->getOperand(3);
    bool Done = false;
    // Consider this case:
    //   vreg1 = instr1 ...
    //   vreg2 = instr2 ...
    //   vreg0 = C2_mux ..., vreg1, vreg2
    // If vreg0 was coalesced with vreg1, we could end up with the following
    // code:
    //   vreg0 = instr1 ...
    //   vreg2 = instr2 ...
    //   vreg0 = A2_tfrf ..., vreg2
    // which will later become:
    //   vreg0 = instr1 ...
    //   vreg0 = instr2_cNotPt ...
    // i.e. there will be an unconditional definition (instr1) of vreg0
    // followed by a conditional one. The output dependency was there before
    // and it unavoidable, but if instr1 is predicable, we will no longer be
    // able to predicate it here.
    // To avoid this scenario, don't coalesce the destination register with
    // a source register that is defined by a predicable instruction.
    if (S1.isReg()) {
      RegisterRef RS = S1;
      MachineInstr *RDef = getReachingDefForPred(RS, CI, RP.Reg, true);
      if (!RDef || !HII->isPredicable(*RDef))
        Done = coalesceRegisters(RD, RegisterRef(S1));
    }
    if (!Done && S2.isReg()) {
      RegisterRef RS = S2;
      MachineInstr *RDef = getReachingDefForPred(RS, CI, RP.Reg, false);
      if (!RDef || !HII->isPredicable(*RDef))
        Done = coalesceRegisters(RD, RegisterRef(S2));
    }
    Changed |= Done;
  }
  return Changed;
}


bool HexagonExpandCondsets::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(*MF.getFunction()))
    return false;

  HII = static_cast<const HexagonInstrInfo*>(MF.getSubtarget().getInstrInfo());
  TRI = MF.getSubtarget().getRegisterInfo();
  MDT = &getAnalysis<MachineDominatorTree>();
  LIS = &getAnalysis<LiveIntervals>();
  MRI = &MF.getRegInfo();
  LocalImpDefs.clear();

  DEBUG(LIS->print(dbgs() << "Before expand-condsets\n",
                   MF.getFunction()->getParent()));

  bool Changed = false;
  std::set<unsigned> SplitUpd, PredUpd;

  // Try to coalesce the target of a mux with one of its sources.
  // This could eliminate a register copy in some circumstances.
  Changed |= coalesceSegments(MF);

  // First, simply split all muxes into a pair of conditional transfers
  // and update the live intervals to reflect the new arrangement. The
  // goal is to update the kill flags, since predication will rely on
  // them.
  for (auto &B : MF)
    Changed |= splitInBlock(B, SplitUpd);
  updateLiveness(SplitUpd, true, true, false);

  // Traverse all blocks and collapse predicable instructions feeding
  // conditional transfers into predicated instructions.
  // Walk over all the instructions again, so we may catch pre-existing
  // cases that were not created in the previous step.
  for (auto &B : MF)
    Changed |= predicateInBlock(B, PredUpd);

  updateLiveness(PredUpd, true, true, true);
  // Remove from SplitUpd all registers contained in PredUpd to avoid
  // unnecessary liveness recalculation.
  std::set<unsigned> Diff;
  std::set_difference(SplitUpd.begin(), SplitUpd.end(),
                      PredUpd.begin(), PredUpd.end(),
                      std::inserter(Diff, Diff.begin()));
  updateLiveness(Diff, false, false, true);

  for (auto *ImpD : LocalImpDefs)
    removeInstr(ImpD);

  DEBUG({
    if (Changed)
      LIS->print(dbgs() << "After expand-condsets\n",
                 MF.getFunction()->getParent());
  });

  return Changed;
}


//===----------------------------------------------------------------------===//
//                         Public Constructor Functions
//===----------------------------------------------------------------------===//

FunctionPass *llvm::createHexagonExpandCondsets() {
  return new HexagonExpandCondsets();
}
