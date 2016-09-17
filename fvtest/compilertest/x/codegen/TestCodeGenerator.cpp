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

#include "env/OMRMemory.hpp"
#include "codegen/Linkage.hpp"
#include "control/Recompilation.hpp"
#include "x/codegen/FPTreeEvaluator.hpp"
#include "codegen/X86Instruction.hpp"
#include "codegen/CodeGenerator.hpp"

#ifdef TR_TARGET_64BIT
#include "x/amd64/codegen/AMD64SystemLinkage.hpp"
#else
#include "x/i386/codegen/IA32SystemLinkage.hpp"
#endif
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/TreeTop.hpp"
#include "compiler/il/OMRTreeTop_inlines.hpp"

namespace Test
{
namespace X86
{

TR::Linkage *
CodeGenerator::createLinkage(TR_LinkageConventions lc)
   {
   TR::Compilation *comp = self()->comp();
   TR::Linkage *linkage = NULL;

   switch (lc)
      {
      case TR_Private:
         // HACK HACK HACK "intentional" fall through to system linkage
      case TR_Helper:
         // Intentional fall through
      case TR_System:
         if (TR::Compiler->target.isLinux())
            {
#if defined(TR_TARGET_64BIT)
            linkage = new (self()->trHeapMemory()) TR_AMD64ABILinkage(self());
#else
            linkage = new (self()->trHeapMemory()) TR_IA32SystemLinkage(self());
#endif
            }
         else if (TR::Compiler->target.isWindows())
            {
#if defined(TR_TARGET_64BIT)
            linkage = new (self()->trHeapMemory()) TR_AMD64Win64FastCallLinkage(self());
#else
            linkage = new (self()->trHeapMemory()) TR_IA32SystemLinkage(self());
#endif
            }
         else
            {
            TR_ASSERT(0, "\nTestarossa error: Illegal linkage convention %d\n", lc);
            }
         break;

      default :
         TR_ASSERT(0, "\nTestarossa error: Illegal linkage convention %d\n", lc);
      }

   self()->setLinkage(lc, linkage);
   return linkage;
   }

void
CodeGenerator::beginInstructionSelection()
   {
   TR::Compilation *comp = self()->comp();
   _returnTypeInfoInstruction = NULL;
   TR::ResolvedMethodSymbol * methodSymbol = comp->getJittedMethodSymbol();
   TR::Recompilation * recompilation = comp->getRecompilationInfo();
   TR::Node * startNode = comp->getStartTree()->getNode();

   if (recompilation && recompilation->generatePrePrologue() != NULL)
      {
      // Return type info will have been generated by recompilation info
      //
      if (methodSymbol->getLinkageConvention() == TR_Private)
         _returnTypeInfoInstruction = (TR::X86ImmInstruction*)comp->getAppendInstruction();

      if (methodSymbol->getLinkageConvention() == TR_System)
         _returnTypeInfoInstruction = (TR::X86ImmInstruction*)comp->getAppendInstruction();
      }

   if (methodSymbol->getLinkageConvention() == TR_Private && !_returnTypeInfoInstruction)
      {
      // linkageInfo word
      if (comp->getAppendInstruction())
         _returnTypeInfoInstruction = generateImmInstruction(DDImm4, startNode, 0, self());
      else
         _returnTypeInfoInstruction = new (self()->trHeapMemory()) TR::X86ImmInstruction((TR::Instruction *)NULL, DDImm4, 0, self());
      }

   if (methodSymbol->getLinkageConvention() == TR_System && !_returnTypeInfoInstruction)
      {
      // linkageInfo word
      if (comp->getAppendInstruction())
         _returnTypeInfoInstruction = generateImmInstruction(DDImm4, startNode, 0, self());
      else
         _returnTypeInfoInstruction = new (self()->trHeapMemory()) TR::X86ImmInstruction((TR::Instruction *)NULL, DDImm4, 0, self());
      }

   TR::RegisterDependencyConditions  *deps = generateRegisterDependencyConditions((uint8_t)0, (uint8_t)1, self());
   if (_linkageProperties->getMethodMetaDataRegister() != TR::RealRegister::NoReg)
      {
      deps->addPostCondition(self()->getVMThreadRegister(),
                             (TR::RealRegister::RegNum)self()->getVMThreadRegister()->getAssociation(), self());
      }
   deps->stopAddingPostConditions();

   if (comp->getAppendInstruction())
      generateInstruction(PROCENTRY, startNode, deps, self());
   else
      new (self()->trHeapMemory()) TR::Instruction(deps, PROCENTRY, (TR::Instruction *)NULL, self());

   // Set the default FPCW to single precision mode if we are allowed to.
   //
   if (self()->enableSinglePrecisionMethods() && comp->getJittedMethodSymbol()->usesSinglePrecisionMode())
      {
      TR_IA32ConstantDataSnippet * cds = self()->findOrCreate2ByteConstant(startNode, SINGLE_PRECISION_ROUND_TO_NEAREST);
      generateMemInstruction(LDCWMem, startNode, generateX86MemoryReference(cds, self()), self());
      }
   }

void
CodeGenerator::endInstructionSelection()
   {
   TR::Compilation *comp = self()->comp();
   // *this    swipeable for debugging purposes
   if (_returnTypeInfoInstruction != NULL)
      {
      TR_ReturnInfo returnInfo = comp->getReturnInfo();

      // Note: this will get clobbered again in code generation on AMD64
      _returnTypeInfoInstruction->setSourceImmediate(returnInfo);
      }

   // Reset the FPCW in the dummy finally block.
   //
   if (self()->enableSinglePrecisionMethods() &&
       comp->getJittedMethodSymbol()->usesSinglePrecisionMode())
      {
      TR_ASSERT(self()->getLastCatchAppendInstruction(),
             "endInstructionSelection() ==> Could not find the dummy finally block!\n");

      TR_IA32ConstantDataSnippet * cds = self()->findOrCreate2ByteConstant(self()->getLastCatchAppendInstruction()->getNode(), DOUBLE_PRECISION_ROUND_TO_NEAREST);
      generateMemInstruction(self()->getLastCatchAppendInstruction(), LDCWMem, generateX86MemoryReference(cds, self()), self());
      }
   }

TR::Instruction *
CodeGenerator::generateSwitchToInterpreterPrePrologue(TR::Instruction * prev, uint8_t alignment, uint8_t alignmentMargin)
   {
   TR_ASSERT(0, "unimplemented");
   return 0;
   }

} // namespace X86
} // namespace Test

void saveFirstTwoBytes(void *startPC, int32_t startPCToSaveArea)
   {
   TR_ASSERT(0, "unimplemented");
   }

