/*******************************************************************************
 * Copyright (c) 2020, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include <stdint.h>
#include "env/FrontEnd.hpp"
#include "env/j9method.h"
#include "il/Block.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/TreeTop.hpp"
#include "il/TreeTop_inlines.hpp"
#include "infra/Cfg.hpp"
#include "infra/CfgEdge.hpp"
#include "optimizer/HandleRecompilationOps.hpp"
#include "optimizer/Optimization.hpp"
#include "optimizer/Optimization_inlines.hpp"

int32_t
TR_HandleRecompilationOps::perform()
   {
   _enableTransform = true;

   if (trace())
      {
      traceMsg(comp(), "Entering HandleRecompilationOps\n");
      }

   if (comp()->getOption(TR_DisableOSR))
      {
      if (trace())
         {
         traceMsg(comp(), "Disabling Handle Recompilation Operations as OSR is disabled\n");
         }

      _enableTransform = false;
      }

   if (comp()->getHCRMode() != TR::osr)
      {
      if (trace())
         {
         traceMsg(comp(), "Disabling Handle Recompilation Operations as HCR mode is not OSR\n");
         }

      _enableTransform = false;
      }

   if (comp()->getOSRMode() == TR::involuntaryOSR)
      {
      if (trace())
         {
         traceMsg(comp(), "Disabling Handle Recompilation Operations as OSR mode is involuntary\n");
         }

      _enableTransform = false;
      }

   if (!comp()->supportsInduceOSR())
      {
      if (trace())
         {
         traceMsg(comp(), "Disabling Handle Recompilation Operations as induceOSR is not supported\n");
         }

      _enableTransform = false;
      }

   if (!comp()->allowRecompilation())
      {
      if (trace())
         {
         traceMsg(comp(), "Disabling Handle Recompilation Operations as recompilation is not permitted\n");
         }

      _enableTransform = false;
      }

   for (TR::TreeTop *tt = comp()->getStartTree(); tt != NULL; tt = tt->getNextTreeTop())
      {
      TR::Node *node = tt->getNode();
      visitNode(tt, node);
      }

   if (_enableTransform && trace())
     {
     traceMsg(comp(), "Completed HandleRecompilationOps\n");
     }

   return 0;
   }

bool
TR_HandleRecompilationOps::resolveCHKGuardsValueTypeOperation(TR::TreeTop *currTree, TR::Node *node)
   {
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "Looking at ResolveCHK node n%dn [%p]\n", node->getGlobalIndex(), node);
}
   TR::Node *resolveChild = node->getFirstChild();
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   child node n%dn [%p]\n", resolveChild->getGlobalIndex(), resolveChild);
}

   if (resolveChild->getOpCodeValue() == TR::loadaddr)
      {
      TR::SymbolReference *loadAddrSymRef = resolveChild->getSymbolReference();

      if (loadAddrSymRef->isUnresolved())
         {
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   addrSymRef isUnresolved\n");
}
         for (TR::TreeTop *nextTree = currTree->getNextTreeTop();
              nextTree != NULL && nextTree->getNode()->getOpCodeValue() != TR::BBEnd;
              nextTree = nextTree->getNextTreeTop())
            {
            TR::Node *nextNode = nextTree->getNode();
            if (nextNode->getOpCode().isTreeTop() && nextNode->getNumChildren() > 0)
               {
               nextNode = nextNode->getFirstChild();
               }

if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   Walking through nodes - n%dn [%p]\n", nextNode->getGlobalIndex(), nextNode);
}

            if (nextNode->getOpCodeValue() == TR::newvalue)
               {
               TR::Node *classAddr = nextNode->getFirstChild();

               if (classAddr == resolveChild
                   || (classAddr->getOpCodeValue() == TR::loadaddr
                       && classAddr->getSymbolReference() == loadAddrSymRef))
               {
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   Found newvalue referencing load address\n");
}
               return true;
               }
               }
            }
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   Didn't find relevant use of load address\n");
}
         }
      }
   else if (resolveChild->getOpCode().isLoadVarOrStore() && resolveChild->getOpCode().isIndirect())
      {
      TR::SymbolReference *symRef = resolveChild->getSymbolReference();

      if (symRef->getCPIndex() != -1 && static_cast<TR_ResolvedJ9Method*>(_methodSymbol->getResolvedMethod())->isFieldQType(symRef->getCPIndex()))
         {
         return true;
         }
      }

   return false;
   }

void
TR_HandleRecompilationOps::visitNode(TR::TreeTop *currTree, TR::Node *node)
   {
static char *dontUseResolveCHKRequestRecompileSymbol = feGetEnv("TR_DontUseResolveCHKRequestRecompileSymbol");

   TR::ILOpCode opcode = node->getOpCode();

   if (opcode.isResolveCheck()
       && ((!dontUseResolveCHKRequestRecompileSymbol
            && node->getSymbolReference() == getSymRefTab()->findOrCreateResolveCheckRequestRecompileSymbolRef(_methodSymbol))
           || (dontUseResolveCHKRequestRecompileSymbol
               && resolveCHKGuardsValueTypeOperation(currTree, node) )))
      {
      if (_enableTransform
          && performTransformation(comp(), "%sInserting induceOSR call after ResolveCHK node n%dn [%p]\n", optDetailString(), node->getGlobalIndex(), node))
         {
         TR_OSRMethodData *osrMethodData = comp()->getOSRCompilationData()->findOrCreateOSRMethodData(node->getByteCodeInfo().getCallerIndex(), _methodSymbol);
         TR::Block *catchBlock = osrMethodData->findOrCreateOSRCatchBlock(node);
         TR::Block *currBlock = currTree->getEnclosingBlock();

         if (!currBlock->hasExceptionSuccessor(catchBlock))
            {
            _methodSymbol->getFlowGraph()->addEdge(TR::CFGEdge::createExceptionEdge(currBlock, catchBlock, comp()->trMemory()));
            }

         if (comp()->getOption(TR_TraceILGen))
            {
            traceMsg(comp(), "Preparing to generate induceOSR for newvalue n%dn\n", node->getGlobalIndex());
            }

         TR::Node *branchNode = TR::Node::create(node, TR::Goto, 0);
         TR::TreeTop *branchTT = TR::TreeTop::create(comp(), branchNode);
         TR::TreeTop *lastTT = NULL;

         // Clean up trees following the point at which the induceOSR will be inserted
         //
         TR::TreeTop *cleanupTT = currTree->getNextTreeTop();
         while (cleanupTT)
            {
            TR::Node *cleanupNode = cleanupTT->getNode();
            if (((cleanupNode->getOpCodeValue() == TR::athrow)
                 && cleanupNode->throwInsertedByOSR())
                || (cleanupNode->getOpCodeValue() == TR::BBEnd))
               {
               break;
               }

            TR::TreeTop *nextTT = cleanupTT->getNextTreeTop();
            currTree->join(nextTT);
            cleanupNode->recursivelyDecReferenceCount();
            cleanupTT = nextTT;
            }

         TR_ASSERT_FATAL(_methodSymbol->induceOSRAfterAndRecompile(currTree, node->getByteCodeInfo(), branchTT, false, 0, &lastTT), "Unable to generate induce OSR");
         node->setSymbolReference(getSymRefTab()->findOrCreateResolveCheckSymbolRef(_methodSymbol));
         }
      else
         {
if (comp()->getOption(TR_TraceILGen))
{
traceMsg(comp(), "   Encountered ResolveCHK node n%dn [%p] with resolve-check-recompile symbol, but cannot induce OSR.  Aborting compilation\n", node->getGlobalIndex(), node);
}

//   if (comp()->isOutermostMethod())
      {
      TR::DebugCounter::incStaticDebugCounter(comp(),
         TR::DebugCounter::debugCounterName(comp(),
            "ilgen.abort/unresolved/(%s)/bc=%d",
            comp()->signature(),
            node->getByteCodeIndex()));
      }
/*
   else
      {
      TR::DebugCounter::incStaticDebugCounter(comp(),
         TR::DebugCounter::debugCounterName(comp(),
            "ilgen.abort/unresolved/%s/%s/(%s)/bc=%d/root=(%s)",
            bytecodeName,
            refType,
            _method->signature(comp()->trMemory()),
            bcIndex,
            comp()->signature()));
      }
*/
   
         comp()->failCompilation<TR::UnsupportedValueTypeOperation>("ResolveCHK encountered for node n%dn [%p]", node->getGlobalIndex(), node);
         }
      }
   }

const char *
TR_HandleRecompilationOps::optDetailString() const throw()
   {
   return "O^O HANDLE RECOMPILATION OPERATIONS:";
   }
