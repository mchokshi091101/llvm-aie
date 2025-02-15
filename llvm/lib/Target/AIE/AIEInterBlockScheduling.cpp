//===- AIEInterBlockScheduling.cpp - Inter-block scheduling infrastructure ===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Implementations of the classes used to support inter-block scheduling
//===----------------------------------------------------------------------===//

#include "AIEInterBlockScheduling.h"
#include "AIEMaxLatencyFinder.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/Support/ErrorHandling.h"
#include <memory>

#define DEBUG_TYPE "machine-scheduler"

// These are more specific debug classes, separating the function and block
// level logging from the detailed scheduling info.
// useful combis:
// --debug-only=sched-blocks,loop-aware
// --debug-only=sched-blocks,machine-scheduler
#define DEBUG_LOOPAWARE(X) DEBUG_WITH_TYPE("loop-aware", X)
#define DEBUG_BLOCKS(X) DEBUG_WITH_TYPE("sched-blocks", X)

using namespace llvm;

static cl::opt<bool>
    LoopAware("aie-loop-aware", cl::init(true),
              cl::desc("[AIE] Schedule single block loops iteratively"));

static cl::opt<bool> LoopEpilogueAnalysis(
    "aie-loop-epilogue-analysis", cl::init(true),
    cl::desc("[AIE] Perform Loop/Epilogue analysis with loop scheduling"));

namespace llvm::AIE {

void dumpInterBlock(const InterBlockEdges &Edges) {
  for (const SUnit &SU : Edges) {
    dbgs() << "SU" << SU.NodeNum << ": " << *SU.getInstr();
  }
}

void emitBundlesInScoreboard(const std::vector<MachineBundle> &Bundles,
                             ResourceScoreboard<FuncUnitWrapper> &Scoreboard,
                             AIEHazardRecognizer *HR) {

  const int TotalBundles = Bundles.size();
  const int AmountToEmit = std::min(TotalBundles, HR->getConflictHorizon());
  // Do not emit more than the specified by the conflict horizon. More
  // then this will not cause conflicts.
  for (int i = TotalBundles - AmountToEmit; i < TotalBundles; i++) {
    for (MachineInstr *MI : Bundles[i].getInstrs())
      HR->emitInScoreboard(Scoreboard, MI->getDesc(), 0);

    Scoreboard.advance();
  }
}

void emitBundlesInScoreboardDelta(
    const std::vector<MachineBundle> &Bundles,
    ResourceScoreboard<FuncUnitWrapper> &Scoreboard, int &Delta,
    AIEHazardRecognizer *HR) {

  for (auto &Bundle : Bundles) {
    // We don't need to replay more instructions, because we exhausted the
    // scoreboard.
    if (Delta >= 0)
      break;

    for (MachineInstr *MI : Bundle.getInstrs())
      HR->emitInScoreboard(Scoreboard, MI->getDesc(), Delta);

    Delta++;
  }
}

MachineBasicBlock *getSinglePredecessor(const MachineBasicBlock &MBB) {
  assert(MBB.pred_size() == 1 && "MBB contains more than 1 predecessor");
  MachineBasicBlock *SinglePredMBB = *MBB.predecessors().begin();
  return SinglePredMBB;
}

InterBlockScheduling::InterBlockScheduling(const MachineSchedContext *C,
                                           bool InterBlock)
    : Context(C), InterBlockScoreboard(InterBlock) {}

// This sets up the scheduling mode for each block. It defines which
// CFG edges will be prioritized for interblock scheduling and which blocks
// should take care of the latency repair.
void InterBlockScheduling::markEpilogueBlocks() {
  // Mark up the epilogues of the Loops we have found
  for (auto &[MBB, BS] : Blocks) {
    if (BS.Kind != BlockType::Loop) {
      continue;
    }
    llvm::for_each(MBB->successors(), [L = MBB, this](auto *S) {
      if (S != L) {
        getBlockState(S).Kind = BlockType::Epilogue;
      }
    });
  }
}

void InterBlockScheduling::enterFunction(MachineFunction *MF) {
  DEBUG_BLOCKS(dbgs() << ">> enterFunction " << MF->getName() << "\n");

  // Get ourselves a hazard recognizer
  HR = std::make_unique<AIEHazardRecognizer>(MF->getSubtarget());

  // Define our universe of blocks
  for (MachineBasicBlock &MBB : *MF) {
    Blocks.emplace(&MBB, &MBB);
  }
  if (LoopAware) {
    // Mark epilogues of the loops we found. This is only necessary if
    // we have created Loops in the first place, as indicated by LoopAware.
    markEpilogueBlocks();
  }

  defineSchedulingOrder(MF);
}

void InterBlockScheduling::leaveFunction() {
  DEBUG_BLOCKS(dbgs() << "<< leaveFunction\n");
  Blocks.clear();
}

void InterBlockScheduling::enterBlock(MachineBasicBlock *BB) {
  CurrentBlock = &getBlockState(BB);
  CurrentBlock->resetRegion();
  DEBUG_BLOCKS(dbgs() << "  >> enterBlock " << BB->getNumber() << " "
                      << CurrentBlock->kindAsString() << " FixPointIter="
                      << CurrentBlock->FixPoint.NumIters << "\n");
}

bool InterBlockScheduling::leaveBlock() {
  DEBUG_BLOCKS(dbgs() << "  << leaveBlock "
                      << CurrentBlock->TheBlock->getNumber() << "\n");
  // After scheduling a basic block, check convergence to determine which block
  // to schedule next and with what parameters
  auto &BS = *CurrentBlock;
  if (BS.Kind == BlockType::Loop && !updateFixPoint(BS)) {
    BS.FixPoint.NumIters++;
    // Iterate on CurrentBlock
    // If we are very unlucky, we may step both the latency margin and
    // the resource margin to the max. Any more indicates failure to converge,
    // and we abort to prevent an infinite loop.
    if (BS.FixPoint.NumIters > 2 * HR->getConflictHorizon()) {
      report_fatal_error("Inter-block scheduling did not converge.");
    }
    return false;
  }

  BS.setScheduled();
  CurrentBlock = nullptr;
  return true;
}

bool InterBlockScheduling::resourcesConverged(BlockState &BS) const {
  // We are a single-block loop body. Check that there is no resource conflict
  // on the backedge, by overlaying top and bottom region
  assert(!BS.getRegions().empty());
  const int Depth = HR->getMaxLookAhead();
  ResourceScoreboard<FuncUnitWrapper> Bottom;
  Bottom.reset(Depth);

  emitBundlesInScoreboard(BS.getBottom().Bundles, Bottom, HR.get());

  DEBUG_LOOPAWARE(dbgs() << "Bottom scoreboard\n"; Bottom.dump());
  // We have two successors, the loop itself and the epilogue
  assert(BS.TheBlock->succ_size() == 2);
  for (auto *Succ : BS.TheBlock->successors()) {
    auto &SBS = getBlockState(Succ);
    if (SBS.Kind == BlockType::Epilogue) {
      // We can ignore the epilogue
      continue;
    }

    assert(SBS.Kind == BlockType::Loop);
    ResourceScoreboard<FuncUnitWrapper> Top;
    Top.reset(Depth);
    int Cycle = -Depth;

    emitBundlesInScoreboardDelta(BS.getBottom().Bundles, Top, Cycle, HR.get());

    DEBUG_LOOPAWARE(dbgs() << "Top scoreboard\n"; Top.dump());
    if (Bottom.conflict(Top, Depth)) {
      return false;
    }
  }
  // Bottom represents the resources that are sticking out of the block.
  // The last non-empty cycle is a safe upperbound for the resource
  // safety margin.
  BS.FixPoint.MaxResourceExtent = Bottom.lastOccupied();
  return true;
}

bool InterBlockScheduling::latencyConverged(BlockState &BS) const {
  const auto &SubTarget = BS.TheBlock->getParent()->getSubtarget();
  auto *TII = static_cast<const AIEBaseInstrInfo *>(SubTarget.getInstrInfo());
  auto *ItinData = SubTarget.getInstrItineraryData();

  assert(!BS.getRegions().empty());

  // BackEdges represents all dependence edges that span the loop edge
  // We will iterate over all backedge dependences by running over the
  // SUnits connected to instructions in the bottom bundles and check
  // successor SUnits to be in the Top region (using a boundary check)
  // If the successor is in Top, we lookup its depth in TopDepth
  const Region &Bottom = BS.getBottom();
  const Region &Top = BS.getTop();
  const InterBlockEdges &BackEdges = BS.getBoundaryEdges();

  // Record the depth of all instructions in Top. Don't record the ones that
  // can't cause problems
  std::map<MachineInstr *, int> TopDepth;
  int Depth = 0;
  for (auto &Bundle : Top.Bundles) {
    for (auto *MI : Bundle.getInstrs()) {
      TopDepth[MI] = Depth;
    }
    if (++Depth > HR->getConflictHorizon()) {
      break;
    }
  }

  // Now check all inter-block edges. We prune by checking whether
  // max latency reaches the successor at all
  int MaxExtent = 0;
  int Height = 1;
  for (auto &Bundle : reverse(Bottom.Bundles)) {
    DEBUG_LOOPAWARE(dbgs() << "--- Height=" << Height << "---\n");
    for (auto *MI : Bundle.getInstrs()) {
      int Extending = AIE::maxLatency(MI, *TII, *ItinData, false) - Height;
      DEBUG_LOOPAWARE(dbgs()
                      << "Extending=" << Extending << " " << *MI << "\n");
      if (Extending <= 0) {
        continue;
      }
      MaxExtent = std::max(MaxExtent, Extending);
      const SUnit *Pred = BackEdges.getPreBoundaryNode(MI);
      for (auto &SDep : Pred->Succs) {
        auto *Succ = SDep.getSUnit();
        if (!BackEdges.isPostBoundaryNode(Succ)) {
          continue;
        }
        DEBUG_LOOPAWARE(dbgs() << "  Backedge to " << Succ->NodeNum << "\n");
        auto DepthIt = TopDepth.find(Succ->getInstr());
        if (DepthIt == TopDepth.end()) {
          // Over the horizon
          continue;
        }
        DEBUG_LOOPAWARE(dbgs() << "  Depth=" << DepthIt->second << "\n");
        int Latency = SDep.getSignedLatency();
        int Distance = Height + DepthIt->second;
        if (Distance < Latency) {
          DEBUG_LOOPAWARE(dbgs() << "  Latency(" << Pred->NodeNum << "->"
                                 << Succ->NodeNum << ")=" << Latency
                                 << " not met (" << Distance << ")\n");
          DEBUG_LOOPAWARE(dbgs() << "  " << Succ->NodeNum << ": "
                                 << *Succ->getInstr());
          return false;
        }
      }
    }
    if (++Height > HR->getConflictHorizon()) {
      break;
    }
  }

  // MaxExtent tracks anything sticking out of the block, so is a safe
  // upperbound of the latency safety margin that should be provided by
  // the epilogue
  BS.FixPoint.MaxLatencyExtent = MaxExtent;
  return true;
}

bool InterBlockScheduling::updateFixPoint(BlockState &BS) {
  if (!BS.FixPoint.NumIters) {
    // This is the first time we have scheduled this loop. In that first
    // iteration, we have recorded the region decomposition.
    // Now we can create the interblock edges between the top and the bottom
    // region
    BS.initInterBlock(*Context);
  }

  if (InterBlockScoreboard && !resourcesConverged(BS)) {
    BS.FixPoint.ResourceMargin++;
    DEBUG_LOOPAWARE(dbgs() << "  not converged: resources RM=>"
                           << BS.FixPoint.ResourceMargin
                           << " LM=" << BS.FixPoint.LatencyMargin << "\n");
    // Iterate on CurMBB
    return false;
  }
  if (!latencyConverged(BS)) {
    BS.FixPoint.LatencyMargin++;
    DEBUG_LOOPAWARE(dbgs() << "  not converged: latency RM="
                           << BS.FixPoint.ResourceMargin << " LM=>"
                           << BS.FixPoint.LatencyMargin << "\n");
    // Iterate on CurMBB
    return false;
  }

  DEBUG_LOOPAWARE(dbgs() << "Converged,"
                         << " LatencyExtent=" << BS.FixPoint.MaxLatencyExtent
                         << " ResourceExtent=" << BS.FixPoint.MaxResourceExtent
                         << "\n");

  return true;
}

bool InterBlockScheduling::successorsAreScheduled(
    MachineBasicBlock *MBB) const {
  return llvm::all_of(MBB->successors(), [&](MachineBasicBlock *B) {
    const auto &BS = getBlockState(B);
    return BS.isScheduled();
  });
}

std::optional<int>
InterBlockScheduling::getLatencyCap(MachineBasicBlock *BB) const {
  auto &BS = getBlockState(BB);
  if (BS.Kind != BlockType::Loop) {
    return {};
  }
  return BS.FixPoint.LatencyMargin;
}

std::optional<int>
InterBlockScheduling::getBlockedResourceCap(MachineBasicBlock *BB) const {
  auto &BS = getBlockState(BB);
  if (BS.Kind != BlockType::Loop) {
    return {};
  }
  return BS.FixPoint.ResourceMargin;
}

void InterBlockScheduling::defineSchedulingOrder(MachineFunction *MF) {
  // First do the (single-block) loops. We don't want these to be constrained
  // by their epilogues
  for (auto &[MBB, BS] : Blocks) {
    if (BS.Kind == BlockType::Loop) {
      MBBSequence.push_back(MBB);
    }
  }

  // Then the rest in postorder to optimize the number of already scheduled
  // successors
  for (MachineBasicBlock *MBB : post_order(MF)) {
    if (getBlockState(MBB).Kind != BlockType::Loop) {
      MBBSequence.push_back(MBB);
    }
  }

  // Now initialize the index to the start.
  NextInOrder = 0;
  DEBUG_BLOCKS(dbgs() << "MBB scheduling sequence : ";
               for (const auto &MBBSeq
                    : MBBSequence) dbgs()
               << MBBSeq->getNumber() << " -> ";
               dbgs() << "\n";);

  assert(MF->size() == MBBSequence.size() &&
         "Missing MBB in scheduling sequence");
}

MachineBasicBlock *InterBlockScheduling::nextBlock() {
  auto &BS = getBlockState(MBBSequence[NextInOrder]);
  if (!BS.isScheduled()) {
    return MBBSequence[NextInOrder];
  }

  ++NextInOrder;
  if (NextInOrder >= MBBSequence.size()) {
    return nullptr;
  }
  return MBBSequence[NextInOrder];
}

const BlockState &
InterBlockScheduling::getBlockState(MachineBasicBlock *BB) const {
  return Blocks.at(BB);
}

BlockState &InterBlockScheduling::getBlockState(MachineBasicBlock *BB) {
  return Blocks.at(BB);
}

void InterBlockScheduling::enterRegion(MachineBasicBlock *BB,
                                       MachineBasicBlock::iterator RegionBegin,
                                       MachineBasicBlock::iterator RegionEnd) {
  auto &BS = getBlockState(BB);
  DEBUG_BLOCKS(dbgs() << "    >> enterRegion, Iter=" << BS.FixPoint.NumIters
                      << "\n");
  if (!BS.FixPoint.NumIters) {
    BS.addRegion(BB, RegionBegin, RegionEnd);
  }
}
int InterBlockScheduling::getNumEntryNops(const BlockState &BS) const {
  // Epilogues should supply the safety margin for their loop.
  // That loop is the only predecessor by construction of
  // BlockType::Epilogue
  if (BS.Kind != BlockType::Epilogue) {
    return 0;
  }
  const MachineBasicBlock &BB = *BS.TheBlock;
  assert(BB.pred_size() == 1);
  MachineBasicBlock *Loop = getSinglePredecessor(BB);
  auto &LBS = getBlockState(Loop);

  // We can only analyze non-empty epilogue blocks because we need
  // to build a DDG, which is not possible.
  // For empty ones, we need to be conservative because we are not aware of
  // content of epilogues' successor.
  if (LoopEpilogueAnalysis && BB.size() > 0) {
    int ExistingLatency = getCyclesToRespectTiming(BS, LBS);
    // Start the next step only after clearing latencies.
    return getCyclesToAvoidResourceConflicts(ExistingLatency, BS, LBS);
  }

  return LBS.getSafetyMargin();
}

int InterBlockScheduling::getCyclesToRespectTiming(
    const BlockState &EpilogueBS, const BlockState &LoopBS) const {

  const MachineBasicBlock &EpilogueMBB = *EpilogueBS.TheBlock;
  const MachineBasicBlock *LoopMBB = getSinglePredecessor(EpilogueMBB);

  DEBUG_LOOPAWARE(dbgs() << "** Loop/Epilogue-carried latency dependencies:"
                         << " Original Loop " << *LoopMBB
                         << " Original Epilogue " << EpilogueMBB << "\n");

  InterBlockEdges Edges(*Context);
  std::map<const MachineInstr *, int> DistancesFromLoopEntry;
  int DistFromLoopEntry = 0;
  int EntryNops = 0;

  auto AddRegionToEdges = [&](const Region &R) {
    for (auto &Bundle : R.Bundles) {
      for (MachineInstr *MI : Bundle.getInstrs()) {
        DistancesFromLoopEntry[MI] = DistFromLoopEntry;
        Edges.addNode(MI);
      }
      ++DistFromLoopEntry;
    }
  };

  // Construction of the superblock containing Loop+Epilogue
  // First part is the loop
  AddRegionToEdges(LoopBS.getBottom());
  Edges.markBoundary();
  // Second part is the epilogue itself
  AddRegionToEdges(EpilogueBS.getTop());
  Edges.buildEdges();

  DEBUG_LOOPAWARE(dumpInterBlock(Edges));
  // Check cross-boundary latencies.
  int Height = 1;
  for (auto &Bundle : reverse(LoopBS.getBottom().Bundles)) {
    for (auto *PreBoundaryMI : Bundle.getInstrs()) {
      const SUnit *Pred = Edges.getPreBoundaryNode(PreBoundaryMI);

      for (auto &SDep : Pred->Succs) {
        auto *Succ = SDep.getSUnit();

        if (!Edges.isPostBoundaryNode(Succ))
          continue;

        const MachineInstr *PostBoundaryMI = Succ->getInstr();

        const int PostBoundOrExitDist =
            (PostBoundaryMI != nullptr)
                ? DistancesFromLoopEntry[PostBoundaryMI]
                // When getInstr returns nullptr, we reached
                // ExitSU. We can consider the DistFromLoopEntry as
                // depth of the ExitSU.
                : DistFromLoopEntry;

        const int Latency = SDep.getSignedLatency();
        const int Distance =
            PostBoundOrExitDist - DistancesFromLoopEntry[PreBoundaryMI];

        DEBUG_LOOPAWARE(dbgs() << "Data dependency found:\n"
                               << " Loop instruction SU: " << *PreBoundaryMI);
        DEBUG_LOOPAWARE(dbgs() << " Epilogue instruction: ";
                        if (PostBoundaryMI) PostBoundaryMI->dump();
                        else dbgs() << "nullptr (ExitSU)";);
        DEBUG_LOOPAWARE(dbgs() << "\n Latency: " << Latency
                               << "\n Distance: " << Distance << "\n");

        EntryNops = std::max(EntryNops, Latency - Distance);
      }
    }
    if (++Height > HR->getConflictHorizon()) {
      break;
    }
  }

  return EntryNops;
}

int InterBlockScheduling::getCyclesToAvoidResourceConflicts(
    int ExistingLatency, const BlockState &EpilogueBS,
    const BlockState &LoopBS) const {

  const MachineBasicBlock &EpilogueMBB = *EpilogueBS.TheBlock;
  MachineBasicBlock *LoopMBB = LoopBS.TheBlock;
  int Depth = HR->getMaxLookAhead();
  ResourceScoreboard<FuncUnitWrapper> Bottom;
  Bottom.reset(Depth);

  DEBUG_LOOPAWARE(dbgs() << "* Loop/Epilogue-carried resource conflicts:"
                         << " Original Loop " << *LoopMBB << " Original Epilog "
                         << EpilogueMBB << "\n");

  emitBundlesInScoreboard(LoopBS.getBottom().Bundles, Bottom, HR.get());

  // We know how many latency cycles we need to respect, and we can advance
  // the scoreboard to the first possible cycle that can accommodate another
  // instruction and start the resource verification from this point, tracking
  // the number of NOPS.
  int NopCounter = 0;
  for (NopCounter = 0; NopCounter < ExistingLatency; ++NopCounter)
    Bottom.advance();

  DEBUG_LOOPAWARE(dbgs() << "Loop scoreboard\n"; Bottom.dump());

  ResourceScoreboard<FuncUnitWrapper> Top;
  Top.reset(Depth);
  int Cycle = -Depth;

  auto Bundles = EpilogueBS.getBottom().Bundles;

  emitBundlesInScoreboardDelta(EpilogueBS.getBottom().Bundles, Top, Cycle,
                               HR.get());

  DEBUG_LOOPAWARE(dbgs() << "Epilogue scoreboard\n"; Top.dump());

  // Use scoreboard comparison to calculate the number of nops
  while (Bottom.conflict(Top, Depth)) {
    Bottom.advance();
    NopCounter++;
  }

  DEBUG_LOOPAWARE(dbgs() << "Resource conflict avoidance between"
                         << " loop: " << *LoopMBB
                         << " And epilogue: " << EpilogueMBB << " Requires "
                         << NopCounter << " Nops\n");

  return NopCounter;
}

void InterBlockEdges::addNode(MachineInstr *MI) {
  if (auto Index = DDG.initSUnit(*MI)) {
    IndexMap &TheMap = Boundary ? SuccMap : PredMap;
    TheMap.emplace(MI, *Index);
  }
}

// Mark the boundary between the predecessor block and the successor block
void InterBlockEdges::markBoundary() { Boundary = DDG.SUnits.size(); }

const SUnit *InterBlockEdges::getPreBoundaryNode(MachineInstr *MI) const {
  auto Found = PredMap.find(MI);
  if (Found == PredMap.end()) {
    return nullptr;
  }

  return &DDG.SUnits.at(Found->second);
}

bool InterBlockEdges::isPostBoundaryNode(SUnit *SU) const {
  return Boundary ? SU->NodeNum >= *Boundary : false;
}

BlockState::BlockState(MachineBasicBlock *Block) : TheBlock(Block) {
  classify();
}

// This safety margin is independent of the successor block, and is therefore
// conservative
int BlockState::getSafetyMargin() const {
  assert(Kind == BlockType::Loop);
  assert(isScheduled());
  auto Margin = std::max(FixPoint.MaxLatencyExtent, FixPoint.MaxResourceExtent);
  DEBUG_LOOPAWARE(dbgs() << "Epilogue margin=" << Margin << "\n");
  return Margin;
}

void BlockState::setScheduled() { FixPoint.IsScheduled = true; }

void BlockState::clearSchedule() {
  // We are rescheduling this block. Clear the results of the previous
  // iteration, to prepare for the next round.
  for (auto &R : Regions) {
    R.Bundles.clear();
  }
  CurrentRegion = 0;
}

void BlockState::classify() {
  // Detect whether this block is amenable to loop-aware scheduling.
  // We must push the safety margin to our epilogue block(s)
  // This can only be done if we have an epilogue and the epilogue is not itself
  // a loop.
  auto IsLoop = [](MachineBasicBlock *MBB) {
    int NumLoopEdges = 0;
    int NumExitEdges = 0;
    for (auto *S : MBB->successors()) {
      if (S == MBB) {
        NumLoopEdges++;
      } else {
        NumExitEdges++;
      }
    }
    return NumLoopEdges == 1 && NumExitEdges == 1;
  };
  // We generalize slightly; we require the epilogue to be a dedicated exit of
  // the loop.
  auto CanFixLoopSchedule = [L = TheBlock](auto *S) {
    // Either the backedge, or a dedicated loop exit
    return S == L || S->pred_size() == 1;
  };

  // If we don't mark up any loops, we will iterate in the same order and apply
  // the same safetymargins as before.
  if (LoopAware && IsLoop(TheBlock) &&
      llvm::all_of(TheBlock->successors(), CanFixLoopSchedule)) {
    Kind = BlockType::Loop;
  }

  // We will mark the epilogues in a second sweep, when all states have been
  // constructed. That sweep is driven by the Loops we've classified on
  // construction.
}

void BlockState::initInterBlock(const MachineSchedContext &Context) {
  BoundaryEdges = std::make_unique<InterBlockEdges>(Context);

  // We are called just after the first round of scheduling a block.
  // These loops run over the original 'semantical order' that was collected
  // in this first fixpoint iteration
  for (auto *MI : getBottom()) {
    BoundaryEdges->addNode(MI);
  }
  BoundaryEdges->markBoundary();
  for (auto *MI : getTop()) {
    BoundaryEdges->addNode(MI);
  }
  BoundaryEdges->buildEdges();
  DEBUG_LOOPAWARE(dumpInterBlock(*BoundaryEdges));
}

} // namespace llvm::AIE
