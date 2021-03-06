//===- MipsInstructionSelector.cpp ------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file implements the targeting of the InstructionSelector class for
/// Mips.
/// \todo This should be generated by TableGen.
//===----------------------------------------------------------------------===//

#include "MipsRegisterBankInfo.h"
#include "MipsTargetMachine.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelectorImpl.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"

#define DEBUG_TYPE "mips-isel"

using namespace llvm;

namespace {

#define GET_GLOBALISEL_PREDICATE_BITSET
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATE_BITSET

class MipsInstructionSelector : public InstructionSelector {
public:
  MipsInstructionSelector(const MipsTargetMachine &TM, const MipsSubtarget &STI,
                          const MipsRegisterBankInfo &RBI);

  bool select(MachineInstr &I, CodeGenCoverage &CoverageInfo) const override;
  static const char *getName() { return DEBUG_TYPE; }

private:
  bool selectImpl(MachineInstr &I, CodeGenCoverage &CoverageInfo) const;

  const MipsTargetMachine &TM;
  const MipsSubtarget &STI;
  const MipsInstrInfo &TII;
  const MipsRegisterInfo &TRI;
  const MipsRegisterBankInfo &RBI;

#define GET_GLOBALISEL_PREDICATES_DECL
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATES_DECL

#define GET_GLOBALISEL_TEMPORARIES_DECL
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_TEMPORARIES_DECL
};

} // end anonymous namespace

#define GET_GLOBALISEL_IMPL
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_IMPL

MipsInstructionSelector::MipsInstructionSelector(
    const MipsTargetMachine &TM, const MipsSubtarget &STI,
    const MipsRegisterBankInfo &RBI)
    : InstructionSelector(), TM(TM), STI(STI), TII(*STI.getInstrInfo()),
      TRI(*STI.getRegisterInfo()), RBI(RBI),

#define GET_GLOBALISEL_PREDICATES_INIT
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_PREDICATES_INIT
#define GET_GLOBALISEL_TEMPORARIES_INIT
#include "MipsGenGlobalISel.inc"
#undef GET_GLOBALISEL_TEMPORARIES_INIT
{
}

static bool selectCopy(MachineInstr &I, const TargetInstrInfo &TII,
                       MachineRegisterInfo &MRI, const TargetRegisterInfo &TRI,
                       const RegisterBankInfo &RBI) {
  unsigned DstReg = I.getOperand(0).getReg();
  if (TargetRegisterInfo::isPhysicalRegister(DstReg))
    return true;

  const TargetRegisterClass *RC = &Mips::GPR32RegClass;

  if (!RBI.constrainGenericRegister(DstReg, *RC, MRI)) {
    LLVM_DEBUG(dbgs() << "Failed to constrain " << TII.getName(I.getOpcode())
                      << " operand\n");
    return false;
  }
  return true;
}

bool MipsInstructionSelector::select(MachineInstr &I,
                                     CodeGenCoverage &CoverageInfo) const {

  MachineBasicBlock &MBB = *I.getParent();
  MachineFunction &MF = *MBB.getParent();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  if (!isPreISelGenericOpcode(I.getOpcode())) {
    if (I.isCopy())
      return selectCopy(I, TII, MRI, TRI, RBI);

    return true;
  }

  if (selectImpl(I, CoverageInfo)) {
    return true;
  }

  MachineInstr *MI = nullptr;
  using namespace TargetOpcode;

  switch (I.getOpcode()) {
  case G_GEP: {
    MI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::ADDu))
             .add(I.getOperand(0))
             .add(I.getOperand(1))
             .add(I.getOperand(2));
    break;
  }
  case G_FRAME_INDEX: {
    MI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::ADDiu))
             .add(I.getOperand(0))
             .add(I.getOperand(1))
             .addImm(0);
    break;
  }
  case G_STORE:
  case G_LOAD: {
    const unsigned DestReg = I.getOperand(0).getReg();
    const unsigned DestRegBank = RBI.getRegBank(DestReg, MRI, TRI)->getID();
    const unsigned OpSize = MRI.getType(DestReg).getSizeInBits();

    if (DestRegBank != Mips::GPRBRegBankID || OpSize != 32)
      return false;

    const unsigned NewOpc = I.getOpcode() == G_STORE ? Mips::SW : Mips::LW;

    MI = BuildMI(MBB, I, I.getDebugLoc(), TII.get(NewOpc))
             .add(I.getOperand(0))
             .add(I.getOperand(1))
             .addImm(0)
             .addMemOperand(*I.memoperands_begin());
    break;
  }
  case G_CONSTANT: {
    int Imm = I.getOperand(1).getCImm()->getValue().getLimitedValue();
    unsigned LUiReg = MRI.createVirtualRegister(&Mips::GPR32RegClass);
    MachineInstr *LUi, *ORi;

    LUi = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::LUi))
              .addDef(LUiReg)
              .addImm(Imm >> 16);

    ORi = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::ORi))
              .addDef(I.getOperand(0).getReg())
              .addUse(LUiReg)
              .addImm(Imm & 0xFFFF);

    if (!constrainSelectedInstRegOperands(*LUi, TII, TRI, RBI))
      return false;
    if (!constrainSelectedInstRegOperands(*ORi, TII, TRI, RBI))
      return false;

    I.eraseFromParent();
    return true;
  }
  case G_GLOBAL_VALUE: {
    if (MF.getTarget().isPositionIndependent())
      return false;

    const llvm::GlobalValue *GVal = I.getOperand(1).getGlobal();
    unsigned LUiReg = MRI.createVirtualRegister(&Mips::GPR32RegClass);
    MachineInstr *LUi, *ADDiu;

    LUi = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::LUi))
              .addDef(LUiReg)
              .addGlobalAddress(GVal);
    LUi->getOperand(1).setTargetFlags(MipsII::MO_ABS_HI);

    ADDiu = BuildMI(MBB, I, I.getDebugLoc(), TII.get(Mips::ADDiu))
                .addDef(I.getOperand(0).getReg())
                .addUse(LUiReg)
                .addGlobalAddress(GVal);
    ADDiu->getOperand(2).setTargetFlags(MipsII::MO_ABS_LO);

    if (!constrainSelectedInstRegOperands(*LUi, TII, TRI, RBI))
      return false;
    if (!constrainSelectedInstRegOperands(*ADDiu, TII, TRI, RBI))
      return false;

    I.eraseFromParent();
    return true;
  }
  case G_ICMP: {
    struct Instr {
      unsigned Opcode, Def, LHS, RHS;
      Instr(unsigned Opcode, unsigned Def, unsigned LHS, unsigned RHS)
          : Opcode(Opcode), Def(Def), LHS(LHS), RHS(RHS){};

      bool hasImm() const {
        if (Opcode == Mips::SLTiu || Opcode == Mips::XORi)
          return true;
        return false;
      }
    };

    SmallVector<struct Instr, 2> Instructions;
    unsigned ICMPReg = I.getOperand(0).getReg();
    unsigned Temp = MRI.createVirtualRegister(&Mips::GPR32RegClass);
    unsigned LHS = I.getOperand(2).getReg();
    unsigned RHS = I.getOperand(3).getReg();
    CmpInst::Predicate Cond =
        static_cast<CmpInst::Predicate>(I.getOperand(1).getPredicate());

    switch (Cond) {
    case CmpInst::ICMP_EQ: // LHS == RHS -> (LHS ^ RHS) < 1
      Instructions.emplace_back(Mips::XOR, Temp, LHS, RHS);
      Instructions.emplace_back(Mips::SLTiu, ICMPReg, Temp, 1);
      break;
    case CmpInst::ICMP_NE: // LHS != RHS -> 0 < (LHS ^ RHS)
      Instructions.emplace_back(Mips::XOR, Temp, LHS, RHS);
      Instructions.emplace_back(Mips::SLTu, ICMPReg, Mips::ZERO, Temp);
      break;
    case CmpInst::ICMP_UGT: // LHS >  RHS -> RHS < LHS
      Instructions.emplace_back(Mips::SLTu, ICMPReg, RHS, LHS);
      break;
    case CmpInst::ICMP_UGE: // LHS >= RHS -> !(LHS < RHS)
      Instructions.emplace_back(Mips::SLTu, Temp, LHS, RHS);
      Instructions.emplace_back(Mips::XORi, ICMPReg, Temp, 1);
      break;
    case CmpInst::ICMP_ULT: // LHS <  RHS -> LHS < RHS
      Instructions.emplace_back(Mips::SLTu, ICMPReg, LHS, RHS);
      break;
    case CmpInst::ICMP_ULE: // LHS <= RHS -> !(RHS < LHS)
      Instructions.emplace_back(Mips::SLTu, Temp, RHS, LHS);
      Instructions.emplace_back(Mips::XORi, ICMPReg, Temp, 1);
      break;
    case CmpInst::ICMP_SGT: // LHS >  RHS -> RHS < LHS
      Instructions.emplace_back(Mips::SLT, ICMPReg, RHS, LHS);
      break;
    case CmpInst::ICMP_SGE: // LHS >= RHS -> !(LHS < RHS)
      Instructions.emplace_back(Mips::SLT, Temp, LHS, RHS);
      Instructions.emplace_back(Mips::XORi, ICMPReg, Temp, 1);
      break;
    case CmpInst::ICMP_SLT: // LHS <  RHS -> LHS < RHS
      Instructions.emplace_back(Mips::SLT, ICMPReg, LHS, RHS);
      break;
    case CmpInst::ICMP_SLE: // LHS <= RHS -> !(RHS < LHS)
      Instructions.emplace_back(Mips::SLT, Temp, RHS, LHS);
      Instructions.emplace_back(Mips::XORi, ICMPReg, Temp, 1);
      break;
    default:
      return false;
    }

    MachineIRBuilder B(I);
    for (const struct Instr &Instruction : Instructions) {
      MachineInstrBuilder MIB = B.buildInstr(
          Instruction.Opcode, {Instruction.Def}, {Instruction.LHS});

      if (Instruction.hasImm())
        MIB.addImm(Instruction.RHS);
      else
        MIB.addUse(Instruction.RHS);

      if (!MIB.constrainAllUses(TII, TRI, RBI))
        return false;
    }

    I.eraseFromParent();
    return true;
  }
  default:
    return false;
  }

  I.eraseFromParent();
  return constrainSelectedInstRegOperands(*MI, TII, TRI, RBI);
}

namespace llvm {
InstructionSelector *createMipsInstructionSelector(const MipsTargetMachine &TM,
                                                   MipsSubtarget &Subtarget,
                                                   MipsRegisterBankInfo &RBI) {
  return new MipsInstructionSelector(TM, Subtarget, RBI);
}
} // end namespace llvm
