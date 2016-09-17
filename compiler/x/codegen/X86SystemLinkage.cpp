/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include "x/codegen/X86SystemLinkage.hpp"

#include <stddef.h>                            // for NULL
#include "codegen/CodeGenerator.hpp"           // for CodeGenerator
#include "codegen/Instruction.hpp"             // for Instruction
#include "codegen/Machine.hpp"                 // for Machine, etc
#include "codegen/MemoryReference.hpp"
#include "codegen/RealRegister.hpp"            // for RealRegister, etc
#include "compile/Compilation.hpp"             // for Compilation
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/TRMemory.hpp"                    // for TR_Memory, etc
#include "il/ILOpCodes.hpp"                    // for ILOpCodes::BBStart, etc
#include "il/Node.hpp"                         // for Node
#include "il/Node_inlines.hpp"                 // for Node::getChild, etc
#include "il/Symbol.hpp"                       // for Symbol
#include "il/symbol/AutomaticSymbol.hpp"       // for AutomaticSymbol
#include "il/symbol/ParameterSymbol.hpp"       // for ParameterSymbol
#include "il/symbol/ResolvedMethodSymbol.hpp"  // for ResolvedMethodSymbol
#include "infra/Assert.hpp"                    // for TR_ASSERT
#include "infra/BitVector.hpp"                 // for TR_BitVector
#include "infra/List.hpp"                      // for ListIterator, List
#include "ras/Debug.hpp"                       // for TR_DebugBase
#include "codegen/X86Instruction.hpp"        // for TR::X86RegInstruction, etc
#include "x/codegen/X86Ops.hpp"                // for TR_X86OpCodes, etc
#include "env/CompilerEnv.hpp"

TR_X86SystemLinkage::TR_X86SystemLinkage(TR::CodeGenerator *cg)
   : TR::Linkage(cg)
   {
   }

const TR_X86LinkageProperties& TR_X86SystemLinkage::getProperties()
   {
   return _properties;
   }

TR::Register *TR_X86SystemLinkage::buildIndirectDispatch(TR::Node *callNode)
   {
   TR_ASSERT(0, "TR_X86SystemLinkage::buildIndirectDispatch is not supported");
   return NULL;
   }

int32_t TR_X86SystemLinkage::computeMemoryArgSize(
      TR::Node *callNode,
      int32_t first,
      int32_t last,
      int8_t direction)
   {
   int32_t                  sizeOfOutGoingArgs= 0;
   uint16_t                 numIntArgs = 0,  numFloatArgs = 0;
   int32_t i;
   for (i = first; i != last; i += direction)
      {
      parmLayoutResult layoutResult;
      TR::Node *child = callNode->getChild(i);

      layoutParm(child, sizeOfOutGoingArgs, numIntArgs, numFloatArgs, layoutResult);
      }
   return sizeOfOutGoingArgs;
   }



static const TR::RealRegister::RegNum NOT_ASSIGNED = (TR::RealRegister::RegNum)-1;

// Copies parameters from where they enter the method (either on stack or in a
// linkage register) to their "home location" where the method body will expect
// to find them (either on stack or in a global register).
//
TR::Instruction *
TR_X86SystemLinkage::copyParametersToHomeLocation(TR::Instruction *cursor)
   {
   TR::Machine *machine = cg()->machine();
   TR::RealRegister *framePointer = machine->getX86RealRegister(TR::RealRegister::vfp);

   TR::ResolvedMethodSymbol             *bodySymbol = comp()->getJittedMethodSymbol();
   ListIterator<TR::ParameterSymbol>  paramIterator(&(bodySymbol->getParameterList()));
   TR::ParameterSymbol               *paramCursor;

   const TR::RealRegister::RegNum noReg = TR::RealRegister::NoReg;
   TR_ASSERT(noReg == 0, "noReg must be zero so zero-initializing movStatus will work");

   TR_MovStatus movStatus[TR::RealRegister::NumRegisters] = {{(TR::RealRegister::RegNum)0,(TR::RealRegister::RegNum)0,(TR_MovDataTypes)0}};

   // We must always do the stores first, then the reg-reg copies, then the
   // loads, so that we never clobber a register we will need later.  However,
   // the logic is simpler if we do the loads and stores in the same loop.
   // Therefore, we maintain a separate instruction cursor for the loads.
   //
   // We defer the initialization of loadCursor until we generate the first
   // load.  Otherwise, if we happen to generate some stores first, then the
   // store cursor would get ahead of the loadCursor, and the instructions
   // would end up in the wrong order despite our efforts.
   //
   TR::Instruction *loadCursor = NULL;

   // Phase 1: generate RegMem and MemReg movs, and collect information about
   // the required RegReg movs.
   //
   for (paramCursor = paramIterator.getFirst();
       paramCursor != NULL;
       paramCursor = paramIterator.getNext())
      {
      int8_t lri = paramCursor->getLinkageRegisterIndex();     // How the parameter enters the method
      TR::RealRegister::RegNum ai                              // Where method body expects to find it
         = (TR::RealRegister::RegNum)paramCursor->getAllocatedIndex();
      int32_t offset = paramCursor->getParameterOffset();      // Location of the parameter's stack slot
      TR_MovDataTypes movDataType = paramMovType(paramCursor); // What sort of MOV instruction does it need?

      // Copy the parameter to wherever it should be
      //
      if (lri == NOT_LINKAGE) // It's on the stack
         {
         if (ai == NOT_ASSIGNED) // It only needs to be on the stack
            {
            // Nothing to do
            }
         else // Method body expects it to be in the ai register
            {
            if (loadCursor == NULL)
               loadCursor = cursor;

            if (debug("traceCopyParametersToHomeLocation"))
               diagnostic("copyParametersToHomeLocation: Loading %d\n", ai);
            // ai := stack
            loadCursor = generateRegMemInstruction(
               loadCursor,
               movOpcodes[RegMem][movDataType],
               machine->getX86RealRegister(ai),
               generateX86MemoryReference(framePointer, offset, cg()),
               cg()
               );
            }
         }
      else // It's in a linkage register
         {
         TR::RealRegister::RegNum sourceIndex = getProperties().getArgumentRegister(lri, isFloat(movDataType));

         // Copy to the stack if necessary
         //
         if (ai == NOT_ASSIGNED || hasToBeOnStack(paramCursor))
            {
            if (comp()->getOption(TR_TraceCG))
              traceMsg(comp(), "copyToHomeLocation param %p, linkage reg index %d, allocated index %d, parameter offset %d, hasToBeOnStack %d, parm->isParmHasToBeOnStack() %d.\n", paramCursor, lri, ai, offset, hasToBeOnStack(paramCursor), paramCursor->isParmHasToBeOnStack());
            if (debug("traceCopyParametersToHomeLocation"))
               diagnostic("copyParametersToHomeLocation: Storing %d\n", sourceIndex);
            // stack := lri
            cursor = generateMemRegInstruction(
               cursor,
               movOpcodes[MemReg][movDataType],
               generateX86MemoryReference(framePointer, offset, cg()),
               machine->getX86RealRegister(sourceIndex),
               cg()
               );
            }

         // Copy to the ai register if necessary
         //
         if (ai != NOT_ASSIGNED && ai != sourceIndex)
            {
            // This parameter needs a RegReg move.  We don't know yet whether
            // we need the value in the target register, so for now we just
            // remember that we need to do this and keep going.
            //
            TR_ASSERT(movStatus[ai         ].sourceReg == noReg, "Each target reg must have only one source");
            TR_ASSERT(movStatus[sourceIndex].targetReg == noReg, "Each source reg must have only one target");
            if (debug("traceCopyParametersToHomeLocation"))
               diagnostic("copyParametersToHomeLocation: Planning to move %d to %d\n", sourceIndex, ai);
            movStatus[ai].sourceReg                  = sourceIndex;
            movStatus[sourceIndex].targetReg         = ai;
            movStatus[sourceIndex].outgoingDataType  = movDataType;
            }

         if (debug("traceCopyParametersToHomeLocation") && ai == sourceIndex)
            {
            diagnostic("copyParametersToHomeLocation: Parameter #%d already in register %d\n", lri, ai);
            }
         }
      }

   // Phase 2: Iterate through the parameters again to insert the RegReg moves.
   //
   for (paramCursor = paramIterator.getFirst();
       paramCursor != NULL;
       paramCursor = paramIterator.getNext())
      {
      if (paramCursor->getLinkageRegisterIndex() == NOT_LINKAGE)
         continue;

      const TR::RealRegister::RegNum paramReg =
         getProperties().getArgumentRegister(paramCursor->getLinkageRegisterIndex(), isFloat(paramMovType(paramCursor)));

      if (movStatus[paramReg].targetReg == 0)
         {
         // This parameter does not need to be copied anywhere
         if (debug("traceCopyParametersToHomeLocation"))
            diagnostic("copyParametersToHomeLocation: Not moving %d\n", paramReg);
         }
      else
         {
         if (debug("traceCopyParametersToHomeLocation"))
            diagnostic("copyParametersToHomeLocation: Preparing to move %d\n", paramReg);

         // If a mov's target register is the source for another mov, we need
         // to do that other mov first.  The idea is to find the end point of
         // the chain of movs starting with paramReg and ending with a
         // register whose current value is not needed; then do that chain of
         // movs in reverse order.
         //
         TR_ASSERT(noReg == 0, "noReg must be zero (not %d) for zero-filled initialization to work", noReg);

         TR::RealRegister::RegNum regCursor;

         // Find the last target in the chain
         //
         regCursor = movStatus[paramReg].targetReg;
         while(movStatus[regCursor].targetReg != noReg)
            {
            // Haven't found the end yet
            regCursor = movStatus[regCursor].targetReg;
            TR_ASSERT(regCursor != paramReg, "Can't yet handle cyclic dependencies");

            // TODO:AMD64 Use scratch register to break cycles
            // A properly-written pickRegister should never
            // cause cycles to occur in the first place.  However, we may want
            // to consider adding cycle-breaking logic so that (1) pickRegister
            // has more flexibility, and (2) we're more robust against
            // otherwise harmless bugs in pickRegister.
            }

         // Work our way backward along the chain, generating all the necessary movs
         //
         while(movStatus[regCursor].sourceReg != noReg)
            {
            TR::RealRegister::RegNum source = movStatus[regCursor].sourceReg;
            if (debug("traceCopyParametersToHomeLocation"))
               diagnostic("copyParametersToHomeLocation: Moving %d to %d\n", source, regCursor);
            // regCursor := regCursor.sourceReg
            cursor = generateRegRegInstruction(
               cursor,
               movOpcodes[RegReg][movStatus[source].outgoingDataType],
               machine->getX86RealRegister(regCursor),
               machine->getX86RealRegister(source),
               cg()
               );
            // Update movStatus as we go so we don't generate redundant movs
            movStatus[regCursor].sourceReg = noReg;
            movStatus[source   ].targetReg = noReg;
            // Continue with the next register in the chain
            regCursor = source;
            }
         }
      }

   // Return the last instruction we inserted, whether or not it was a load.
   //
   return loadCursor? loadCursor : cursor;
   }


TR::Instruction *
TR_X86SystemLinkage::savePreservedRegisters(TR::Instruction *cursor)
   {
   // For IA32, if disableShrinkWrapping, usePushForPreservedRegs will be true; otherwise false;
   //          For X64,  shrinkWraping is always on, and usePushForPreservedRegs always false;
    // TR_ASSERT(!getProperties().getUsesPushesForPreservedRegs(), "assertion failure");
   TR::ResolvedMethodSymbol *bodySymbol = comp()->getJittedMethodSymbol();
   const int32_t localSize = getProperties().getOffsetToFirstLocal() - bodySymbol->getLocalMappingCursor();
   const int32_t pointerSize = getProperties().getPointerSize();

   int32_t offsetCursor = -localSize + getProperties().getOffsetToFirstLocal() - pointerSize;

   if (_properties.getUsesPushesForPreservedRegs())
      {
      for (int32_t pindex = _properties.getMaxRegistersPreservedInPrologue()-1;
           pindex >= 0;
           pindex--)
         {
         TR::RealRegister::RegNum idx = _properties.getPreservedRegister((uint32_t)pindex);
         TR::RealRegister *reg = machine()->getX86RealRegister(idx);
         if (reg->getHasBeenAssignedInMethod() && reg->getState() != TR::RealRegister::Locked)
            {
            cursor = new (trHeapMemory()) TR::X86RegInstruction(cursor, PUSHReg, reg, cg());
            }
         }
      }
   else
      {
      TR_BitVector *p = cg()->getPreservedRegsInPrologue();
      for (int32_t pindex = getProperties().getMaxRegistersPreservedInPrologue()-1;
           pindex >= 0;
           pindex--)
         {
         TR::RealRegister::RegNum idx = _properties.getPreservedRegister((uint32_t)pindex);
         TR::RealRegister *reg = machine()->getX86RealRegister(getProperties().getPreservedRegister((uint32_t)pindex));
         if(reg->getHasBeenAssignedInMethod() && reg->getState() != TR::RealRegister::Locked)
            {
            if (!p || p->get(idx))
               {
               cursor = generateMemRegInstruction(
                  cursor,
                  movOpcodes[MemReg][fullRegisterMovType(reg)],
                  generateX86MemoryReference(machine()->getX86RealRegister(TR::RealRegister::vfp), offsetCursor, cg()),
                  reg,
                  cg()
                  );
               }
            offsetCursor -= pointerSize;
            }
         }
      }
   return cursor;
   }


void
TR_X86SystemLinkage::createPrologue(TR::Instruction *cursor)
   {
#if defined(DEBUG)
   // TODO:AMD64: Get this into the debug DLL

   class TR_DebugFrameSegmentInfo
      {
      private:

      TR_DebugFrameSegmentInfo *_next;
      const char *_description;
      TR::RealRegister *_register;
      int32_t _lowOffset;
      uint8_t _size;
      TR::Compilation * _comp;

      public:

      TR_ALLOC(TR_Memory::CodeGenerator)

      TR_DebugFrameSegmentInfo(
         TR::Compilation * c,
         int32_t lowOffset,
         uint8_t size,
         const char *description,
         TR_DebugFrameSegmentInfo *next,
         TR::RealRegister *reg=NULL
         ):
         _comp(c),
         _next(next),
         _description(description),
         _register(reg),
         _lowOffset(lowOffset),
         _size(size)
         {}

      TR::Compilation * comp() { return _comp; }

      TR_DebugFrameSegmentInfo *getNext(){ return _next; }

      TR_DebugFrameSegmentInfo *sort()
         {
         TR_DebugFrameSegmentInfo *result;
         TR_DebugFrameSegmentInfo *tail = _next? _next->sort() : NULL;
         TR_DebugFrameSegmentInfo *before=NULL, *after;
         for (after = tail; after; before=after, after=after->_next)
            {
            if (after->_lowOffset > _lowOffset)
               break;
            }
         _next = after;
         if (before)
            {
            before->_next = this;
            result = tail;
            }
         else
            {
            result = this;
            }
         return result;
         }

      void print(TR_Debug *debug)
         {
         if (_next)
            _next->print(debug);
         if (_size > 0)
            {
            diagnostic("        % 4d: % 4d -> % 4d (% 4d) %5.5s %s\n",
               _lowOffset, _lowOffset, _lowOffset + _size - 1, _size,
               _register? debug->getName(_register, TR_DoubleWordReg) : "",
               _description
               );
            }
         else
            {
            diagnostic("        % 4d: % 4d -> ---- (% 4d) %5.5s %s\n",
               _lowOffset, _lowOffset, _size,
               _register? debug->getName(_register, TR_DoubleWordReg) : "",
               _description
               );
            }
         }

      };

   TR_DebugFrameSegmentInfo *debugFrameSlotInfo=NULL;
#endif

   TR::RealRegister *espReal = machine()->getX86RealRegister(TR::RealRegister::esp);

   TR::ResolvedMethodSymbol *bodySymbol = comp()->getJittedMethodSymbol();

   const TR_X86LinkageProperties &properties = getProperties();

   const uint32_t outgoingArgSize = cg()->getLargestOutgoingArgSize();

   // Compute the nature of the preserved regs
   //
   uint32_t preservedRegsSize = 0;
   uint32_t registerSaveDescription = 0; // bit N corresponds to _preservedRegisters[N], with 1=preserved

   int32_t pindex; // Preserved register index
   for (pindex = 0; pindex < properties.getMaxRegistersPreservedInPrologue(); pindex++)
      {
      TR::RealRegister *reg = machine()->getX86RealRegister(properties.getPreservedRegister((uint32_t)pindex));
      if (reg->getHasBeenAssignedInMethod() && reg->getState() != TR::RealRegister::Locked)
         {
         preservedRegsSize += properties.getPointerSize();
         registerSaveDescription |= reg->getRealRegisterMask();
         }
      }

   cg()->setRegisterSaveDescription(registerSaveDescription);

   // Compute frame size
   //
   // allocSize: bytes to be subtracted from the stack pointer when allocating the frame
   // frameSize: total bytes of stack the prologue consumes
   //
   //const int32_t localSize = -properties.getOffsetToFirstLocal() - bodySymbol->getLocalMappingCursor();
   const int32_t localSize = properties.getOffsetToFirstLocal() - bodySymbol->getLocalMappingCursor();
   TR_ASSERT(localSize >= 0, "expecting positive localSize");

   // Note that the return address doesn't appear here because it is allocated by
   // the call instruction
   //
   int32_t allocSize = localSize
      + ( properties.getUsesPushesForPreservedRegs()? 0 : preservedRegsSize )
      + ( properties.getReservesOutgoingArgsInPrologue()? outgoingArgSize : 0 );

   int32_t frameSize = localSize + preservedRegsSize
      + ( properties.getReservesOutgoingArgsInPrologue()? outgoingArgSize : 0 )
      + ( properties.getAlwaysDedicateFramePointerRegister() ? properties.getGPRWidth() : 0);

   uint32_t adjust = 0;
   if (_properties.getOutgoingArgAlignment())
      {
      // AMD64 SysV spec requires: The end of the input argument area shall be aligned on a 16 (32, if __m256 is passed on stack) byte boundary. In other words, the value (%rsp + 8) is always a multiple of 16 (32) when control is transferred to the function entry point.
      TR_ASSERT(_properties.getOutgoingArgAlignment() == 16 || _properties.getOutgoingArgAlignment() == 4, "AMD64 SysV linkage require outgoingArgAlignment be 16/32 bytes aligned, while IA32 linkage require 4 bytes aligned.  We currently haven't support 32 bytes alignment for AMD64 SysV ABI yet.\n");
      const uint32_t stackAlignMinus1 = _properties.getOutgoingArgAlignment() - 1;
      adjust = ((frameSize + properties.getRetAddressWidth()  + stackAlignMinus1) & ~stackAlignMinus1) - (frameSize + properties.getRetAddressWidth());
      frameSize += adjust;
      allocSize += adjust;
      }

   cg()->setFrameSizeInBytes(frameSize);

   // Set the VFP state for the PROCENTRY instruction
   //
   if (properties.getAlwaysDedicateFramePointerRegister())
      {
      cursor = new (trHeapMemory()) TR::X86RegInstruction(
         cursor,
         PUSHReg,
         machine()->getX86RealRegister(properties.getFramePointerRegister()),
         cg());

      TR::RealRegister *stackPointerReg = machine()->getX86RealRegister(TR::RealRegister::esp);
      cursor = new (trHeapMemory()) TR::X86RegRegInstruction(
         cursor,
         MOVRegReg(),
         machine()->getX86RealRegister(properties.getFramePointerRegister()),
         stackPointerReg,
         cg());

      cg()->initializeVFPState(properties.getFramePointerRegister(), _properties.getPointerSize());
      }
   else
      {
      cg()->initializeVFPState(TR::RealRegister::esp, 0);
      }

   // Entry breakpoint
   //
   if (comp()->getOption(TR_EntryBreakPoints))
      {
      cursor = new (trHeapMemory()) TR::Instruction(BADIA32Op, cursor, cg());
      }

   // Allocate the stack frame
   //
   if (allocSize == 0)
      {
      // No need to do anything
      }
   else
      {
      const TR_X86OpCodes subOp = (allocSize <= 127)? SUBRegImms() : SUBRegImm4();
      cursor = new (trHeapMemory()) TR::X86RegImmInstruction(cursor, subOp, espReal, allocSize, cg());
      }

   // Save preserved regs, and tell the frontend how many there are
   //
   bodySymbol->setProloguePushSlots(preservedRegsSize / properties.getPointerSize());

   cursor = savePreservedRegisters(cursor);

   if (comp()->getOption(TR_TraceCG))
      {
      traceMsg(comp(), "create prologue using system linkage, after savePreservedRegisters, cursor is %p.\n", cursor);
      }

   cursor = copyParametersToHomeLocation(cursor);

   if (comp()->getOption(TR_TraceCG))
      {
      traceMsg(comp(), "create prologue using system linkage, after copyParametersToHomeLocation, cursor is %p.\n", cursor);
      }

#if defined(DEBUG)
   if (comp()->getOption(TR_TraceCG))
      {
      traceMsg(comp(), "\nFrame size: locals=%d frame=%d\n",localSize, frameSize);
      }
   ListIterator<TR::ParameterSymbol>  paramIterator(&(bodySymbol->getParameterList()));
   TR::ParameterSymbol               *paramCursor;
   for (
      paramCursor = paramIterator.getFirst();
      paramCursor != NULL;
      paramCursor = paramIterator.getNext()
      ){
      TR::RealRegister::RegNum ai = (TR::RealRegister::RegNum)paramCursor->getAllocatedIndex();
      debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
         paramCursor->getOffset(), paramCursor->getSize(), "Parameter",
         debugFrameSlotInfo,
         (ai==NOT_ASSIGNED)? NULL : machine()->getX86RealRegister(ai)
         );
      }

   if (properties.getAlwaysDedicateFramePointerRegister())
      {
      debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
         properties.getOffsetToFirstLocal(), properties.getGPRWidth(), "EBP-save",
         debugFrameSlotInfo
         );
      }

   ListIterator<TR::AutomaticSymbol>  autoIterator(&bodySymbol->getAutomaticList());
   TR::AutomaticSymbol               *autoCursor;
   for (
      autoCursor = autoIterator.getFirst();
      autoCursor != NULL;
      autoCursor = autoIterator.getNext()
      ){
      debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
         autoCursor->getOffset(), autoCursor->getSize(), "Local",
         debugFrameSlotInfo
         );
      }

   debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
      0, properties.getRetAddressWidth(), "Return address",
      debugFrameSlotInfo
      );
   debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
      properties.getOffsetToFirstLocal() - localSize - preservedRegsSize,
      preservedRegsSize, "Preserved args size",
      debugFrameSlotInfo
      );
   debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
      properties.getOffsetToFirstLocal() - localSize - preservedRegsSize - adjust,
      adjust, "adjust size for stack alignment",
      debugFrameSlotInfo
      );
   debugFrameSlotInfo = new (trHeapMemory()) TR_DebugFrameSegmentInfo(comp(),
      properties.getOffsetToFirstLocal() - localSize - preservedRegsSize - outgoingArgSize - adjust,
      outgoingArgSize, "Outgoing args",
      debugFrameSlotInfo
      );
   if (comp()->getOption(TR_TraceCG))
      {
      diagnostic("\nFrame layout:\n");
      diagnostic("        +rsp  +vfp     end  size        what\n");
      debugFrameSlotInfo->sort()->print(cg()->getDebug());
      diagnostic("\n");
      }
#endif

   }


TR::Instruction *
TR_X86SystemLinkage::restorePreservedRegisters(TR::Instruction *cursor)
   {
   TR::ResolvedMethodSymbol *bodySymbol = comp()->getJittedMethodSymbol();
   const int32_t localSize   = _properties.getOffsetToFirstLocal() - bodySymbol->getLocalMappingCursor();
   const int32_t pointerSize = _properties.getPointerSize();

   if (cg()->pushPreservedRegisters())
      {
      for (int32_t pindex = 0;
           pindex <_properties.getMaxRegistersPreservedInPrologue();
           pindex++)
         {
         TR::RealRegister::RegNum idx = _properties.getPreservedRegister((uint32_t)pindex);
         TR::RealRegister *reg = machine()->getX86RealRegister(idx);
         if (reg->getHasBeenAssignedInMethod())
            {
            cursor = new (trHeapMemory()) TR::X86RegInstruction(cursor, POPReg, reg, cg());
            }
         }
      }
   else
      {
      int32_t offsetCursor = -localSize +  _properties.getOffsetToFirstLocal() - pointerSize;
      TR_BitVector *p = cg()->getPreservedRegsInPrologue();
      for (int32_t pindex = _properties.getMaxRegistersPreservedInPrologue()-1;
           pindex >= 0;
           pindex--)
         {
         TR::RealRegister::RegNum idx = _properties.getPreservedRegister((uint32_t)pindex);
         TR::RealRegister *reg = machine()->getX86RealRegister(idx);

         if (comp()->getOption(TR_TraceCG))
            {
            traceMsg(comp(), "reg %d, getHasBeenAssignedInMethod %d, p %x, p->get(idx) %d.\n", idx, reg->getHasBeenAssignedInMethod(), p, p ? p->get(idx) : -1);
            }

         if (reg->getHasBeenAssignedInMethod())
            {
            if (!p || p->get(idx))
               {
               cursor = generateRegMemInstruction(
                  cursor,
                  movOpcodes[RegMem][fullRegisterMovType(reg)],
                  reg,
                  generateX86MemoryReference(machine()->getX86RealRegister(TR::RealRegister::vfp), offsetCursor, cg()),
                  cg()
                  );
               }
            offsetCursor -= pointerSize;
            }
         }
      }

   return cursor;
   }


void
TR_X86SystemLinkage::createEpilogue(TR::Instruction *cursor)
   {
   TR::RealRegister    *espReal      = machine()->getX86RealRegister(TR::RealRegister::esp);
   TR::ResolvedMethodSymbol *bodySymbol = comp()->getJittedMethodSymbol();

   const int32_t localSize = _properties.getOffsetToFirstLocal() - bodySymbol->getLocalMappingCursor();
   int32_t allocSize = _properties.getUsesPushesForPreservedRegs()? localSize : cg()->getFrameSizeInBytes();

   if (cg()->pushPreservedRegisters())
      {
      // Deallocate outgoing arg area in preparation to pop preserved regs
      //
      allocSize = localSize;
      const uint32_t outgoingArgSize = cg()->getLargestOutgoingArgSize();
      TR_X86OpCodes op = (outgoingArgSize <= 127) ? ADDRegImms() : ADDRegImm4();
      cursor = new (trHeapMemory()) TR::X86RegImmInstruction(cursor, op, espReal, outgoingArgSize, cg());
      }

   // Restore preserved regs
   //
   cursor = restorePreservedRegisters(cursor);

   if (comp()->getOption(TR_TraceCG))
      {
      traceMsg(comp(), "create epilogue using system linkage, after restorePreservedRegisters, cursor is %x.\n", cursor);
      }

   // Deallocate the stack frame
   //
   if (_properties.getAlwaysDedicateFramePointerRegister())
      {
      // Restore stack pointer from frame pointer
      //
      cursor = new (trHeapMemory()) TR::X86RegRegInstruction(cursor, MOVRegReg(), espReal, machine()->getX86RealRegister(_properties.getFramePointerRegister()), cg());
      cursor = new (trHeapMemory()) TR::X86RegInstruction(cursor, POPReg, machine()->getX86RealRegister(_properties.getFramePointerRegister()), cg());
      }
   else if (allocSize == 0)
      {
      // No need to do anything
      }
   else
      {
      TR_X86OpCodes op = (allocSize <= 127) ? ADDRegImms() : ADDRegImm4();
      cursor = new (trHeapMemory()) TR::X86RegImmInstruction(cursor, op, espReal, allocSize, cg());
      }

   if (comp()->getOption(TR_TraceCG))
      {
      traceMsg(comp(), "create epilogue using system linkage, after delocating stack frame, cursor is %x.\n", cursor);
      }

   if (cursor->getNext()->getOpCodeValue() == RETImm2)
      {
      toIA32ImmInstruction(cursor->getNext())->setSourceImmediate(bodySymbol->getNumParameterSlots() << getProperties().getParmSlotShift());

      if (comp()->getOption(TR_TraceCG))
         {
         traceMsg(comp(), "create epilogue using system linkage, ret_IMM set to %d.\n", bodySymbol->getNumParameterSlots() << getProperties().getParmSlotShift());
         }
      }
   }


// TODO: add struct/union support in this function
void
TR_X86SystemLinkage::copyLinkageInfoToParameterSymbols()
   {
   TR::ResolvedMethodSymbol *bodySymbol = comp()->getJittedMethodSymbol();
   ListIterator<TR::ParameterSymbol>paramIterator(&(bodySymbol->getParameterList()));
   const TR_X86LinkageProperties &properties = getProperties();
   uint16_t numIntArgs = 0, numFloatArgs = 0;
   int32_t sizeOfOutGoingArgs = 0;
   int32_t maxIntArgs   = properties.getNumIntegerArgumentRegisters();
   int32_t maxFloatArgs = properties.getNumFloatArgumentRegisters();

  for (TR::ParameterSymbol *paramCursor = paramIterator.getFirst(); paramCursor != NULL; paramCursor = paramIterator.getNext())
      {
      parmLayoutResult layoutResult;
      layoutParm(paramCursor, sizeOfOutGoingArgs, numIntArgs, numFloatArgs, layoutResult);
      if (layoutResult.abstract & parmLayoutResult::IN_LINKAGE_REG_PAIR)
         {
         TR_ASSERT(false, "haven't provide register pair support yet.\n");
         }
      else if (layoutResult.abstract & parmLayoutResult::IN_LINKAGE_REG)
         {
         paramCursor->setLinkageRegisterIndex(layoutResult.regs[0].regIndex);
         }
      // If we're out of registers, just stop now instead of looping doing nothing
      //
      if (numIntArgs >= maxIntArgs && numFloatArgs >= maxFloatArgs)
         break;

      }
   }


void
TR_X86SystemLinkage::mapIncomingParms(TR::ResolvedMethodSymbol *method)
   {
   TR_ASSERT(!getProperties().passArgsRightToLeft(), "Right-to-left not yet implemented on AMD64");
   int32_t sizeOfOutGoingArgs = 0;
   uint16_t numIntArgs = 0, numFloatArgs = 0;
   ListIterator<TR::ParameterSymbol> parameterIterator(&method->getParameterList());
   uint32_t bump = getProperties().getOffsetToFirstParm();

   for (TR::ParameterSymbol *parmCursor = parameterIterator.getFirst(); parmCursor; parmCursor = parameterIterator.getNext())
      {
      parmLayoutResult layoutResult;
      layoutParm(parmCursor, sizeOfOutGoingArgs, numIntArgs, numFloatArgs, layoutResult);
      if (layoutResult.abstract & parmLayoutResult::ON_STACK)
         {
         parmCursor->setParameterOffset(layoutResult.offset + bump);
         if (comp()->getOption(TR_TraceCG))
            traceMsg(comp(), "mapIncomingParms setParameterOffset %d for param symbol %p\n", parmCursor->getParameterOffset(), parmCursor);
         }
      }
   }


void
TR_X86SystemLinkage::copyGlRegDepsToParameterSymbols(
      TR::Node *bbStart,
      TR::CodeGenerator *cg)
   {
   TR_ASSERT(bbStart->getOpCodeValue() == TR::BBStart, "assertion failure");
   if (bbStart->getNumChildren() > 0)
      {
      TR::Node *glRegDeps = bbStart->getFirstChild();
      if (!glRegDeps) // No global register info, so nothing to do
         return;

      TR_ASSERT(glRegDeps->getOpCodeValue() == TR::GlRegDeps, "First child of first Node must be a GlRegDeps");

      uint16_t childNum;
      for (childNum=0; childNum < glRegDeps->getNumChildren(); childNum++)
         {
         TR::Node *child = glRegDeps->getChild(childNum);
         TR::ParameterSymbol *sym = child->getSymbol()->getParmSymbol();
         sym->setAllocatedIndex(cg->getGlobalRegister(child->getGlobalRegisterNumber()));
         }
      }
   }


int32_t
TR_X86SystemLinkage::getParameterStartingPos(
      int32_t &dataCursor,
      uint32_t align)
   {
   if (align <= getProperties().getParmSlotSize())
      {
      const int32_t stackSlotMinus1 = getProperties().getParmSlotSize() - 1;
      dataCursor = (dataCursor + stackSlotMinus1) & ~stackSlotMinus1;
      }
   else
      {
      uint32_t alignMinus1 = align - 1;
      dataCursor = (dataCursor + alignMinus1) & (~alignMinus1);
      }
   return dataCursor;
   }


int32_t
TR_X86SystemLinkage::layoutTypeOnStack(
      TR::DataTypes type,
      int32_t &dataCursor,
      parmLayoutResult &layoutResult)
   {
   uint32_t typeAlign = getAlignment(type);
   layoutResult.offset = getParameterStartingPos(dataCursor, typeAlign);
   switch (type)
      {
      case TR::Float:
         dataCursor += 4;
         break;
      case TR::Double:
         dataCursor += 8;
         break;
      case TR::Int8:
         dataCursor += 1;
         break;
      case TR::Int16:
         dataCursor += 2;
         break;
      case TR::Int32:
         dataCursor += 4;
         break;
      case TR::Int64:
      case TR::Address:
         dataCursor += 8;
         break;
      case TR::Aggregate:
      default:
         // TODO: struct/union support should be added as a case branch.
         TR_ASSERT(false, "types still not supported in data layout yet.\n");
         return 0;
      }
   return typeAlign;
   }
