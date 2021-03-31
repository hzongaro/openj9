/*******************************************************************************
 * Copyright (c) 2021, 2021 IBM Corp. and others
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

#include "optimizer/TreeLowering.hpp"

#include "compile/Compilation.hpp"
#include "compile/SymbolReferenceTable.hpp"
#include "il/Block.hpp"
#include "il/Block_inlines.hpp"
#include "infra/ILWalk.hpp"
#include "optimizer/J9TransformUtil.hpp"

const char *
TR::TreeLowering::optDetailString() const throw()
   {
   return "O^O TREE LOWERING: ";
   }

int32_t
TR::TreeLowering::perform()
   {
   if (!TR::Compiler->om.areValueTypesEnabled())
      {
      return 0;
      }

   TR::ResolvedMethodSymbol* methodSymbol = comp()->getMethodSymbol();
   for (TR::PreorderNodeIterator nodeIter(methodSymbol->getFirstTreeTop(), comp()); nodeIter != NULL; ++nodeIter)
      {
      TR::Node* node = nodeIter.currentNode();
      TR::TreeTop* tt = nodeIter.currentTree();

      if (TR::Compiler->om.areValueTypesEnabled())
         {
         lowerValueTypeOperations(nodeIter, node, tt);
         }
      }

   return 0;
   }

void
TR::TreeLowering::moveNodeToEndOfBlock(TR::Block* const block, TR::TreeTop* const tt, TR::Node* const node)
   {
   TR::Compilation* comp = self()->comp();
   TR::TreeTop* blockExit = block->getExit();
   TR::TreeTop* iterTT = tt->getNextTreeTop();

   if (iterTT != blockExit)
      {
      if (trace())
         {
         traceMsg(comp, "Moving treetop containing node n%dn [%p] for acmp helper call to end of prevBlock in preparation of final block split\n", tt->getNode()->getGlobalIndex(), tt->getNode());
         }

      // Remove TreeTop for call node, and gather it and the treetops for stores that
      // resulted from un-commoning in a TreeTop chain from tt to lastTTForCallBlock
      tt->unlink(false);
      TR::TreeTop* lastTTForCallBlock = tt;

      while (iterTT != blockExit)
         {
         TR::TreeTop* nextTT = iterTT->getNextTreeTop();
         TR::ILOpCodes op = iterTT->getNode()->getOpCodeValue();

         if ((op == TR::iRegStore || op == TR::istore) && iterTT->getNode()->getFirstChild() == node)
            {
            if (trace())
               {
               traceMsg(comp, "Moving treetop containing node n%dn [%p] for store of acmp helper result to end of prevBlock in preparation of final block split\n", iterTT->getNode()->getGlobalIndex(), iterTT->getNode());
               }

            // Remove store node from prevBlock temporarily
            iterTT->unlink(false);
            lastTTForCallBlock->join(iterTT);
            lastTTForCallBlock = iterTT;
            }

         iterTT = nextTT;
         }

      // Move the treetops that were gathered for the call and any stores of the
      // result to the end of the block in preparation for the split of the call block
      blockExit->getPrevTreeTop()->join(tt);
      lastTTForCallBlock->join(blockExit);
      }
   }

/**
 * @brief Perform lowering related to Valhalla value types
 *
 */
void
TR::TreeLowering::lowerValueTypeOperations(TR::PreorderNodeIterator& nodeIter, TR::Node* node, TR::TreeTop* tt)
   {
   TR::SymbolReferenceTable * symRefTab = comp()->getSymRefTab();
   static char *disableInliningCheckAastore = feGetEnv("TR_DisableVT_AASTORE_Inlining");

   if (node->getOpCode().isCall())
      {
      if (symRefTab->isNonHelper(node->getSymbolReference(), TR::SymbolReferenceTable::objectEqualityComparisonSymbol))
         {
         // turn the non-helper call into a VM helper call
         node->setSymbolReference(symRefTab->findOrCreateAcmpHelperSymbolRef());
         static const bool disableAcmpFastPath =  NULL != feGetEnv("TR_DisableAcmpFastpath");
         if (!disableAcmpFastPath)
            {
            fastpathAcmpHelper(nodeIter, node, tt);
            }
         }
      else if (node->getSymbolReference()->getReferenceNumber() == TR_ldFlattenableArrayElement)
         {
         static char *disableInliningCheckAaload = feGetEnv("TR_DisableVT_AALOAD_Inlining");

         if (!disableInliningCheckAaload)
            {
            const char *counterName = TR::DebugCounter::debugCounterName(comp(), "vt-helper/inlinecheck/aaload/(%s)/bc=%d",
                                                            comp()->signature(), node->getByteCodeIndex());
            TR::DebugCounter::incStaticDebugCounter(comp(), counterName);

            lowerLoadArrayElement(node, tt);
            }
         }
      else if (node->getSymbolReference()->getReferenceNumber() == TR_strFlattenableArrayElement)
         {
         if (!disableInliningCheckAastore)
            {
            const char *counterName = TR::DebugCounter::debugCounterName(comp(), "vt-helper/inlinecheck/aastore/(%s)/bc=%d",
                                                            comp()->signature(), node->getByteCodeIndex());
            TR::DebugCounter::incStaticDebugCounter(comp(), counterName);

            lowerStoreArrayElement(node, tt);
            }
         }
      }
   else if (node->getOpCodeValue() == TR::ArrayStoreCHK && disableInliningCheckAastore)
      {
      lowerArrayStoreCHK(node, tt);
      }
   }

/**
 * @brief Copy register dependencies between GlRegDeps node at exit points.
 *
 * This function is only intended to work with GlRegDeps nodes for exit points,
 * (i.e. BBEnd, branch, or jump nodes) within the same extended basic block.
 *
 * Register dependencies are copied "logically", meaning that the actual node
 * used to represent a dependency won't necessarily be copied. If the reg dep
 * is represented by a PassThrough, then the node itself is copied and its
 * child is commoned (so it's lifetime is exteneded; note that in correctly-formed
 * IL, the child must also be the child of a reg store in the containing block).
 * Otherwise, the dependency must be represented by a reg load, which must have
 * come from the GlRegDeps node at the entry point and *must* be commoned
 * (so it won't get copied).
 *
 * In addition, this function allows *one* register dependency to be changed
 * (substituted). That is, if a register dependency is found under `sourceNode`
 * for the same register that is set on `subsituteNode`, then `substituteNode`
 * will be used instead of the dependency from `sourceNode`. Note that the
 * reference of of `substituteNode` is incremented if/when it gets added. If
 * `substituteNode` is NULL the no substitution will be attempted.
 *
 * @param targetNode is the GlRegDeps node that reg deps are copied to
 * @param sourceNode is the GlRegDeps node that reg deps are copied from
 * @param substituteNode is the reg dep node to substitute if a matching register is found in `sourceNode` (NULL if none)
 */
void
copyExitRegDepsAndSubstitue(TR::Node* const targetNode, TR::Node* const sourceNode, TR::Node* const substituteNode)
   {
   for (int i = 0; i < sourceNode->getNumChildren(); ++i)
      {
      TR::Node* child = sourceNode->getChild(i);
      if (substituteNode
          && child->getLowGlobalRegisterNumber() == substituteNode->getLowGlobalRegisterNumber()
          && child->getHighGlobalRegisterNumber() == substituteNode->getHighGlobalRegisterNumber())
         targetNode->setAndIncChild(i, substituteNode);
      else if (child->getOpCodeValue() == TR::PassThrough)
         {
         // PassThrough nodes cannot be commoned because doing so does not
         // actually anchor the child, causing it's lifetime to not be extended
         child = TR::Node::copy(child);
         if (child->getFirstChild())
            {
            child->getFirstChild()->incReferenceCount();
            }
         child->setReferenceCount(1);
         targetNode->setChild(i, child);
         }
      else
         {
         // all other nodes must be commoned as they won't get evluated otherwise
         targetNode->setAndIncChild(i, child);
         }
      }
   }

/**
 * @brief Add a GlRegDeps node to a branch by copying some other GlRegDeps.
 *
 * Given branch node, adds a GlRegDeps node by copying the dependencies from
 * a different GlRegDeps. This function allows *one* register dependency to
 * be changed (substituted). See `copyExitRegDepsAndSubstitue()` for details.
 *
 * Note that the branch node is assumed to *not* have a GlRegDeps node already.
 *
 * Returns a pointer to the newly created GlRegDeps. This is can be particularly
 * useful to have whe doing a substitution (e.g. for chaining calls).
 *
 * If the source GlRegDeps is NULL, then nothing is done and NULL is returned.
 *
 * @param branchNode is the branch node the GlRegDeps will be added to
 * @param sourceGlRegDepsNode is the GlRegDeps node used to copy the reg deps from
 * @param substituteNode is the reg dep node to be subsituted (NULL if none)
 * @return TR::Node* the newly created GlRegDeps or NULL if `sourceGelRegDepsNode` was NULL
 */
TR::Node* 
copyBranchGlRegDepsAndSubstitute(TR::Node* const branchNode, TR::Node* const sourceGlRegDepsNode, TR::Node* const substituteNode)
   {
   TR::Node* glRegDepsCopy = NULL;
   if (sourceGlRegDepsNode != NULL)
      {
      glRegDepsCopy = TR::Node::create(TR::GlRegDeps, sourceGlRegDepsNode->getNumChildren());
      copyExitRegDepsAndSubstitue(glRegDepsCopy, sourceGlRegDepsNode, substituteNode);
      branchNode->addChildren(&glRegDepsCopy, 1);
      }
   return glRegDepsCopy;
   }

TR::Block*
TR::TreeLowering::splitForFastpath(TR::Block* const block, TR::TreeTop* const splitPoint, TR::Block* const targetBlock)
   {
   TR::CFG* const cfg = self()->comp()->getFlowGraph();
   TR::Block* const newBlock = block->split(splitPoint, cfg);
   newBlock->setIsExtensionOfPreviousBlock(true);
   cfg->addEdge(block, targetBlock);
   return newBlock;
   }

#define TRACE_TL(fmt, ...) do { if (trace()) traceMsg(comp, fmt, __VA_ARGS__); } while (false)

/**
 * @brief Add checks to skip (fast-path) acmpHelper call
 *
 * @details
 *
 * This transformation adds checks for the cases where the acmp can be performed
 * without calling the VM helper. The transformed Trees represent the following operation:
 *
 * 1. If the address of lhs and rhs are the same, produce an eq (true) result
 *    and skip the call (note the two objects must be the same regardless of
 *    whether they are value types are reference types)
 * 2. Otherwise, do VM helper call
 *
 * The transformation looks as follows:
 *
 *  +----------------------+
 *  |ttprev                |
 *  |treetop               |
 *  |  icall acmpHelper    |
 *  |    aload lhs         |
 *  |    aload rhs         |
 *  |ificmpeq --> ...      |
 *  |  ==> icall           |
 *  |  iconst 0            |
 *  |BBEnd                 |
 *  +----------------------+
 *
 *  ...becomes...
 *
 *
 * +------------------------------+
 * |ttprev                        |
 * |iRegStore x                   |
 * |  iconst 1                    |
 * |ifacmpeq  +->---------------------------+
 * |  aload lhs                   |         |
 * |  aload rhs                   |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 1            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |iRegStore x                   |         |
 * |  iconst 0                    |         |
 * |ifacmpeq +->----------------------------+
 * |  aload lhs                   |         |
 * |  aconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +------------------------------+
 * |  aload rhs                   |         |
 * |  ==> aconst 0                |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +->----------------------------+
 * |  iand                        |         |
 * |    iloadi ClassFlags         |         |
 * |      aloadi J9Class          |         |
 * |        aload lhs             |         |
 * |    iconst J9ClassIsValueType |         |
 * |  iconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |ifacmpeq +->----------------------------+
 * |  iand                        |         |
 * |    iloadi ClassFlags         |         |
 * |      aloadi J9Class          |         |
 * |        aload rhs             |         |
 * |    iconst J9ClassIsValueType |         |
 * |  iconst 0                    |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> iconst 0            |         |
 * |    PassThrough ...           |         |
 * |BBEnd                         |         |
 * +------------------------------+         |
 * |BBStart (extension)           |         |
 * |iRegStore x                   |         |
 * |  icall acmpHelper            |         |
 * |    aload lhs                 |         |
 * |    aload rhs                 |         |
 * |BBEnd                         |         |
 * |  GlRegDeps                   |         |
 * |    PassThrough x             |         |
 * |      ==> icall acmpHelper    |         |
 * |    PassThrough ...           |         |
 * +-----+------------------------+         |
 *       |                                  |
 *       +----------------------------------+
 *       |
 *       v
 * +-----+-----------+
 * |BBStart
 * |ificmpeq +-> ... +
 * |  iRegLoad x     |
 * |  iconst 0       |
 * |BBEnd            |
 * +-----------------+
 *
 * Any GlRegDeps on the extension block are created by OMR::Block::splitPostGRA
 * while those on the ifacmpeq at the end of the first block are copies of those,
 * with the exception of any register (x, above) holding the result of the compare
 *
 * @param node is the current node in the tree walk
 * @param tt is the treetop at the root of the tree ancoring the current node
 *
 */
void
TR::TreeLowering::fastpathAcmpHelper(TR::PreorderNodeIterator& nodeIter, TR::Node * const node, TR::TreeTop * const tt)
   {
   TR::Compilation* comp = self()->comp();
   TR::CFG* cfg = comp->getFlowGraph();
   cfg->invalidateStructure();

   if (!performTransformation(comp, "%sPreparing for post-GRA block split by anchoring helper call and arguments\n", optDetailString()))
      return;

   // Anchor call node after split point to ensure the returned value goes into
   // either a temp or a global register.
   auto* const anchoredCallTT = TR::TreeTop::create(comp, tt, TR::Node::create(TR::treetop, 1, node));
   if (trace())
      traceMsg(comp, "Anchoring call node under treetop n%un (0x%p)\n", anchoredCallTT->getNode()->getGlobalIndex(), anchoredCallTT->getNode());

   // Anchor the call arguments just before the call. This ensures the values are
   // live before the call so that we can propagate their values in global registers if needed.
   auto* const anchoredCallArg1TT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, node->getFirstChild()));
   auto* const anchoredCallArg2TT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, node->getSecondChild()));
   if (trace())
      {
      traceMsg(comp, "Anchoring call arguments n%un and n%un under treetops n%un and n%un\n",
         node->getFirstChild()->getGlobalIndex(), node->getSecondChild()->getGlobalIndex(), anchoredCallArg1TT->getNode()->getGlobalIndex(), anchoredCallArg2TT->getNode()->getGlobalIndex());
      }

   // Split the block at the call TreeTop so that the new block created
   // after the call can become a merge point for all the fastpaths.
   TR::Block* callBlock = tt->getEnclosingBlock();
   if (!performTransformation(comp, "%sSplitting block_%d at TreeTop [0x%p], which holds helper call node n%un\n", optDetailString(), callBlock->getNumber(), tt, node->getGlobalIndex()))
      return;
   TR::Block* targetBlock = callBlock->splitPostGRA(tt->getNextTreeTop(), cfg, true, NULL);
   if (trace())
      traceMsg(comp, "Call node n%un is in block %d, targetBlock is %d\n", node->getGlobalIndex(), callBlock->getNumber(), targetBlock->getNumber());

   // As the block is split after the helper call node, it is possible that as part of un-commoning
   // code to store nodes into registers or temp-slots is appended to the original block by the call
   // to splitPostGRA above.  Move the acmp helper call treetop to the end of prevBlock, along with
   // any stores resulting from un-commoning of the nodes in the helper call tree so that it can be
   // split into its own call block.
   moveNodeToEndOfBlock(callBlock, tt, node);

   if (!performTransformation(comp, "%sInserting fastpath for lhs == rhs\n", optDetailString()))
      return;

   // Insert store of constant 1 as the result of the fastpath.
   // The value must go wherever the value returned by the helper call goes
   // so that the code in the target block (merge point) picks up the constant
   // if the branch is taken. Use the TreeTop previously inserted to anchor the
   // call to figure out where the return value of the call is being put.
   TR::Node* anchoredNode = anchoredCallTT->getNode()->getFirstChild(); // call node is under a treetop node
   if (trace())
      traceMsg(comp, "Anchored call has been transformed into %s node n%un\n", anchoredNode->getOpCode().getName(), anchoredNode->getGlobalIndex());
   auto* const1Node = TR::Node::iconst(1);
   TR::Node* storeNode = NULL;
   TR::Node* regDepForStoreNode = NULL; // this is the reg dep for the store if one is needed
   if (anchoredNode->getOpCodeValue() == TR::iRegLoad)
      {
      if (trace())
         traceMsg(comp, "Storing constant 1 in register %s\n", comp->getDebug()->getGlobalRegisterName(anchoredNode->getGlobalRegisterNumber()));
      auto const globalRegNum = anchoredNode->getGlobalRegisterNumber();
      storeNode = TR::Node::create(TR::iRegStore, 1, const1Node);
      storeNode->setGlobalRegisterNumber(globalRegNum);
      // Since the result is in a global register, we're going to need a PassThrough
      // on the exit point GlRegDeps.
      regDepForStoreNode = TR::Node::create(TR::PassThrough, 1, const1Node);
      regDepForStoreNode->setGlobalRegisterNumber(globalRegNum);
      }
   else if (anchoredNode->getOpCodeValue() == TR::iload)
      {
      if (trace())
         traceMsg(comp, "Storing constant 1 to symref %d (%s)\n", anchoredNode->getSymbolReference()->getReferenceNumber(), anchoredNode->getSymbolReference()->getName(comp->getDebug()));
      storeNode = TR::Node::create(TR::istore, 1, const1Node);
      storeNode->setSymbolReference(anchoredNode->getSymbolReference());
      }
   else
      TR_ASSERT_FATAL_WITH_NODE(anchoredNode, false, "Anchored call has been turned into unexpected opcode\n");
   tt->insertBefore(TR::TreeTop::create(comp, storeNode));

   // If the BBEnd of the block containing the call has a GlRegDeps node,
   // a matching GlRegDeps node will be needed for all the branches. The
   // fallthrough of the call block and the branch targets will be the
   // same block. So, all register dependencies will be mostly the same.
   // `exitGlRegDeps` is intended to point to the "reference" node used to
   // create the GlRegDeps for each consecutive branch.
   TR::Node* exitGlRegDeps = NULL;
   if (callBlock->getExit()->getNode()->getNumChildren() > 0)
      {
      exitGlRegDeps = callBlock->getExit()->getNode()->getFirstChild();
      }

   // Insert fastpath for lhs == rhs (reference comparison), taking care to set the
   // proper register dependencies by copying them from the BBExit of the call block
   // (through `exitGlRegDeps`) when needed.
   auto* ifacmpeqNode = TR::Node::createif(TR::ifacmpeq, anchoredCallArg1TT->getNode()->getFirstChild(), anchoredCallArg2TT->getNode()->getFirstChild(), targetBlock->getEntry());
   exitGlRegDeps = copyBranchGlRegDepsAndSubstitute(ifacmpeqNode, exitGlRegDeps, regDepForStoreNode);
   tt->insertBefore(TR::TreeTop::create(comp, ifacmpeqNode));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", ifacmpeqNode->getGlobalIndex(), callBlock->getNumber());

static char *disableNewACMPFastPaths = feGetEnv("TR_disableVT_ACMP_NewFastPaths");

if (!disableNewACMPFastPaths)
{
   if (!performTransformation(comp, "%sInserting fastpath for lhs == NULL\n", optDetailString()))
      return;

   // Create store of 0 as fastpath result by duplicate the node used to store
   // the constant 1. Also duplicate the corresponding regdep if needed.
   storeNode = storeNode->duplicateTree(true);
   storeNode->getFirstChild()->setInt(0);
   tt->insertBefore(TR::TreeTop::create(comp, storeNode));
   if (regDepForStoreNode != NULL)
      {
      regDepForStoreNode = TR::Node::copy(regDepForStoreNode);
      regDepForStoreNode->setReferenceCount(0);
      regDepForStoreNode->setAndIncChild(0, storeNode->getFirstChild());
      }

   // Using a similar strategy as above, insert check for lhs == NULL.
   auto* const nullConst = TR::Node::aconst(0);
   auto* const checkLhsNull = TR::Node::createif(TR::ifacmpeq, anchoredCallArg1TT->getNode()->getFirstChild(), nullConst, targetBlock->getEntry());
   exitGlRegDeps = copyBranchGlRegDepsAndSubstitute(checkLhsNull, exitGlRegDeps, regDepForStoreNode);
   tt->insertBefore(TR::TreeTop::create(comp, checkLhsNull));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", checkLhsNull->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for rhs == NULL\n", optDetailString()))
      return;

   auto* const checkRhsNull = TR::Node::createif(TR::ifacmpeq, anchoredCallArg2TT->getNode()->getFirstChild(), nullConst, targetBlock->getEntry());
   exitGlRegDeps = copyBranchGlRegDepsAndSubstitute(checkRhsNull, exitGlRegDeps, NULL); // substitution happened above so no need to do it again
   tt->insertBefore(TR::TreeTop::create(comp, checkRhsNull));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", checkRhsNull->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for lhs is VT\n", optDetailString()))
      return;

   auto* const vftSymRef = comp->getSymRefTab()->findOrCreateVftSymbolRef();
   auto* const classFlagsSymRef = comp->getSymRefTab()->findOrCreateClassFlagsSymbolRef();
   auto* const j9ClassIsVTFlag = TR::Node::iconst(node, J9ClassIsValueType);

   auto* const lhsVft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredCallArg1TT->getNode()->getFirstChild(), vftSymRef);
   auto* const lhsClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, lhsVft, classFlagsSymRef);
   auto* const isLhsValueType = TR::Node::create(node, TR::iand, 2, lhsClassFlags, j9ClassIsVTFlag);
   auto* const checkLhsIsVT = TR::Node::createif(TR::ificmpeq, isLhsValueType, storeNode->getFirstChild(), targetBlock->getEntry());
   copyBranchGlRegDepsAndSubstitute(checkLhsIsVT, exitGlRegDeps, NULL);
   tt->insertBefore(TR::TreeTop::create(comp, checkLhsIsVT));
   callBlock = splitForFastpath(callBlock, tt, targetBlock);
   if (trace())
      traceMsg(comp, "Added check node n%un; call node is now in block_%d\n", checkLhsIsVT->getGlobalIndex(), callBlock->getNumber());

   if (!performTransformation(comp, "%sInserting fastpath for rhs is VT\n", optDetailString()))
      return;

   // Put call in it's own block so it will be eaisy to move. Importantly,
   // the block *cannot* be an extension because everything *must* be uncommoned.
   auto* const prevBlock = callBlock;
   callBlock = callBlock->splitPostGRA(tt, cfg, true, NULL);

   if (trace())
      traceMsg(comp, "Call node isolated in block_%d by splitPostGRA\n", callBlock->getNumber());

   // Force nodeIter to first TreeTop of next block so that
   // moving callBlock won't cause problems while iterating
   while (nodeIter.currentTree() != targetBlock->getEntry())
      ++nodeIter;

   if (trace())
      traceMsg(comp, "FORCED treeLowering ITERATOR TO POINT TO NODE n%unn\n", nodeIter.currentNode()->getGlobalIndex());

   // Move call block out of line.
   // The CFG edge that exists from prevBlock to callBlock is kept because
   // it will be needed once the branch for the fastpath gets added.
   cfg->findLastTreeTop()->insertTreeTopsAfterMe(callBlock->getEntry(), callBlock->getExit());
   prevBlock->getExit()->join(targetBlock->getEntry());
   cfg->addEdge(prevBlock, targetBlock);
   if (trace())
      traceMsg(comp, "Moved call block to end of method\n");

   // Create and insert branch.
   auto* const rhsVft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredCallArg2TT->getNode()->getFirstChild(), vftSymRef);
   auto* const rhsClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, rhsVft, classFlagsSymRef);
   auto* const isRhsValueType = TR::Node::create(node, TR::iand, 2, rhsClassFlags, j9ClassIsVTFlag);
   auto* const checkRhsIsNotVT = TR::Node::createif(TR::ificmpne, isRhsValueType, storeNode->getFirstChild(), callBlock->getEntry());
   // Because we've switched the fallthrough and target blocks, the regsiter
   // dependencies also need to be switched.
   if (prevBlock->getExit()->getNode()->getNumChildren() > 0)
      {
      auto* const bbEnd = prevBlock->getExit()->getNode();
      checkRhsIsNotVT->setChild(2, bbEnd->getChild(0));
      checkRhsIsNotVT->setNumChildren(3);
      }
   if (exitGlRegDeps)
      {
      auto* const bbEnd = prevBlock->getExit()->getNode();
      auto* const glRegDeps = TR::Node::create(GlRegDeps, exitGlRegDeps->getNumChildren());
      copyExitRegDepsAndSubstitue(glRegDeps, exitGlRegDeps, NULL);
      bbEnd->setAndIncChild(0, glRegDeps);
      }
   prevBlock->append(TR::TreeTop::create(comp, checkRhsIsNotVT));
   // Note: there's no need to add a CFG edge because one already exists from
   // before callBlock was moved.
   if (trace())
      traceMsg(comp, "Added check node n%un\n", checkRhsIsNotVT->getGlobalIndex());

   // Insert goto target block in outline block.
   auto* const gotoNode = TR::Node::create(node, TR::Goto, 0, targetBlock->getEntry());
   callBlock->append(TR::TreeTop::create(comp, gotoNode));
   // Note: callBlock already has a CFG edge to targetBlock
   // from before it got moved, so adding one here is not required.

   // Move exit GlRegDeps in callBlock.
   // The correct dependencies should have been inserted by splitPostGRA,
   // so they just need to be moved from the BBEnd to the Goto.
   if (callBlock->getEntry()->getNode()->getNumChildren() > 0)
      {
      auto* const bbEnd = callBlock->getExit()->getNode();
      auto* glRegDeps = bbEnd->getChild(0);
      bbEnd->setNumChildren(0);
      glRegDeps->decReferenceCount();
      gotoNode->addChildren(&glRegDeps, 1);
      }
}
   }

/**
 * If value types are enabled, and the value that is being assigned to the array
 * element might be a null reference, lower the ArrayStoreCHK by splitting the
 * block before the ArrayStoreCHK, and inserting a NULLCHK guarded by a check
 * of whether the array's component type is a value type.
 *
 * @param node is the current node in the tree walk
 * @param tt is the treetop at the root of the tree ancoring the current node
 */
void
TR::TreeLowering::lowerArrayStoreCHK(TR::Node *node, TR::TreeTop *tt)
   {
   // Pattern match the ArrayStoreCHK operands to get the source of the assignment
   // (sourceChild) and the array to which an element will have a value assigned (destChild)
   TR::Node *firstChild = node->getFirstChild();

   TR::Node *sourceChild = firstChild->getSecondChild();
   TR::Node *destChild = firstChild->getChild(2);

   // Only need to lower if it is possible that the value is a null reference
   if (!sourceChild->isNonNull())
      {
      TR::CFG * cfg = comp()->getFlowGraph();
      cfg->invalidateStructure();

      TR::Block *prevBlock = tt->getEnclosingBlock();

      performTransformation(comp(), "%sTransforming ArrayStoreCHK n%dn [%p] by splitting block block_%d, and inserting a NULLCHK guarded with a check of whether the component type of the array is a value type\n", optDetailString(), node->getGlobalIndex(), node, prevBlock->getNumber());

      // Anchor the node containing the source of the array element
      // assignment and the node that contains the destination array
      // to ensure they are available for the ificmpeq and NULLCHK
      TR::TreeTop *anchoredArrayTT = TR::TreeTop::create(comp(), tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, destChild));
      TR::TreeTop *anchoredSourceTT = TR::TreeTop::create(comp(), anchoredArrayTT, TR::Node::create(TR::treetop, 1, sourceChild));

      // Transform
      //   +--------------------------------+
      //   | ttprev                         |
      //   | ArrayStoreCHK                  |
      //   |   astorei/awrtbari             |
      //   |     aladd                      |
      //   |       <array-reference>        |
      //   |       index-offset-calculation |
      //   |     <value-reference>          |
      //   +--------------------------------+
      //
      // into
      //   +--------------------------------+
      //   | treetop                        |
      //   |   <array-reference>            |
      //   | treetop                        |
      //   |   <value-reference>            |
      //   | ificmpeq  -->------------------*---------+
      //   |   iand                         |         |
      //   |     iloadi <isClassFlags>      |         |
      //   |       aloadi <componentClass>  |         |
      //   |         aloadi <vft-symbol>    |         |
      //   |           <array-reference>    |         |
      //   |     iconst J9ClassIsValueType  |         |
      //   |   iconst 0                     |         |
      //   | BBEnd                          |         |
      //   +--------------------------------+         |
      //   | BBStart (Extension)            |         |
      //   | NULLCHK                        |         |
      //   |   Passthrough                  |         |
      //   |     <value-reference>          |         |
      //   | BBEnd                          |         |
      //   +--------------------------------+         |
      //                   |                          |
      //                   +--------------------------+
      //                   |
      //                   v
      //   +--------------------------------+
      //   | BBStart                        |
      //   | ArrayStoreCHK                  |
      //   |   astorei/awrtbari             |
      //   |     aladd                      |
      //   |       aload <array>            |
      //   |       index-offset-calculation |
      //   |     aload <value>              |
      //   +--------------------------------+
      //
      TR::SymbolReference *vftSymRef = comp()->getSymRefTab()->findOrCreateVftSymbolRef();
      TR::SymbolReference *arrayCompSymRef = comp()->getSymRefTab()->findOrCreateArrayComponentTypeSymbolRef();
      TR::SymbolReference *classFlagsSymRef = comp()->getSymRefTab()->findOrCreateClassFlagsSymbolRef();

      TR::Node *vft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredArrayTT->getNode()->getFirstChild(), vftSymRef);
      TR::Node *arrayCompClass = TR::Node::createWithSymRef(node, TR::aloadi, 1, vft, arrayCompSymRef);
      TR::Node *loadClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, arrayCompClass, classFlagsSymRef);
      TR::Node *isValueTypeNode = TR::Node::create(node, TR::iand, 2, loadClassFlags, TR::Node::iconst(node, J9ClassIsValueType));

      TR::Node *ifNode = TR::Node::createif(TR::ificmpeq, isValueTypeNode, TR::Node::iconst(node, 0));
      ifNode->copyByteCodeInfo(node);

      TR::Node *passThru  = TR::Node::create(node, TR::PassThrough, 1, sourceChild);
      TR::ResolvedMethodSymbol *currentMethod = comp()->getMethodSymbol();

      TR::Block *arrayStoreCheckBlock = prevBlock->splitPostGRA(tt, cfg);

      ifNode->setBranchDestination(arrayStoreCheckBlock->getEntry());

      // Copy register dependencies from the end of the block split before the
      // ArrayStoreCHK to the ificmpeq that's being added to the end of that block
      if (prevBlock->getExit()->getNode()->getNumChildren() != 0)
         {
         TR::Node *blkDeps = prevBlock->getExit()->getNode()->getFirstChild();
         TR::Node *ifDeps = TR::Node::create(blkDeps, TR::GlRegDeps);

         for (int i = 0; i < blkDeps->getNumChildren(); i++)
            {
            TR::Node *regDep = blkDeps->getChild(i);

            if (regDep->getOpCodeValue() == TR::PassThrough)
               {
               TR::Node *orig= regDep;
               regDep = TR::Node::create(orig, TR::PassThrough, 1, orig->getFirstChild());
               regDep->setLowGlobalRegisterNumber(orig->getLowGlobalRegisterNumber());
               regDep->setHighGlobalRegisterNumber(orig->getHighGlobalRegisterNumber());
               }

            ifDeps->addChildren(&regDep, 1);
            }

         ifNode->addChildren(&ifDeps, 1);
         }

      prevBlock->append(TR::TreeTop::create(comp(), ifNode));

      TR::Node *nullCheck = TR::Node::createWithSymRef(node, TR::NULLCHK, 1, passThru,
                               comp()->getSymRefTab()->findOrCreateNullCheckSymbolRef(currentMethod));
      TR::TreeTop *nullCheckTT = prevBlock->append(TR::TreeTop::create(comp(), nullCheck));

      TR::Block *nullCheckBlock = prevBlock->split(nullCheckTT, cfg);

      nullCheckBlock->setIsExtensionOfPreviousBlock(true);

      cfg->addEdge(prevBlock, arrayStoreCheckBlock);
      }
   }

/*
+-----------------------------------------+       +--------------------------------------------+
|treetop                                  |       | BBStart                                    |
|   acall  jitLoadFlattenableArrayElement |       | treetop                                    |
|      ==>iRegLoad                        |       |    ==>iRegLoad                             |
|      ==>aRegLoad                        | ----> | treetop                                    |
|ResolveAndNULLCHK                        |       |    ==>aRegLoad                             |
|   iloadi  Point2D.x                     |       | aRegStore edi                              |
|      ==>acall                           |       |    aconst NULL                             |
|...                                      |       | ificmpeq -->-------------------------------+---+
+-----------------------------------------+       |    iand                                    |   |
                                                  |       iloadi  <isClassFlags>               |   |
                                                  |       ...                                  |   |
                                                  |       iconst 1024                          |   |
                                                  |    iconst 0                                |   |
                                                  |    GlRegDeps ()                            |   |
                                                  |       PassThrough rdi                      |   |
                                                  |          ==>aconst NULL                    |   |
                                                  |       ==>aRegLoad                          |   |
                                                  |       ==>iRegLoad                          |   |
                                                  | BBEnd                                      |   |
                                                  +--------------------------------------------+   |
                                                  +--------------------------------------------+   |
                                                  | BBStart                                    |   |
                                                  | treetop                                    |   |
                                                  |    acall  jitLoadFlattenableArrayElement   |   |
                                                  |       ==>iRegLoad                          |   |
                                                  |       ==>aRegLoad                          |   |
                                                  | aRegStore edi                              |   |
                                                  |    ==>acall                                |   |
                                                  | goto -->-----------------------------------+---+---+
                                                  |    GlRegDeps ()                            |   |   |
                                                  |       ==>aRegLoad                          |   |   |
                                                  |       ==>iRegLoad                          |   |   |
                                                  |       PassThrough rdi                      |   |   |
                                                  |          ==>acall                          |   |   |
                                                  | BBEnd                                      |   |   |
                                                  |    GlRegDeps ()                            |   |   |
                                                  |       ==>aRegLoad                          |   |   |
                                                  |       ==>iRegLoad                          |   |   |
                                                  |       PassThrough rdi                      |   |   |
                                                  |          ==>acall                          |   |   |
                                                  +----------+---------------------------------+   |   |
                                                             |                                     |   |
                                                             +-------------------------------------+   |
                                                             |                                         |
                                                  +----------v---------------------------------+       |
                                                  | BBStart                                    |       |
                                                  |    GlRegDeps ()                            |       |
                                                  |       PassThrough rdi                      |       |
                                                  |          ==>aconst NULL                    |       |
                                                  |    ==>aRegLoad                             |       |
                                                  |    ==>iRegLoad                             |       |
                                                  | NULLCHK on n191n                           |       |
                                                  |    PassThrough                             |       |
                                                  |       ==>aRegLoad                          |       |
                                                  | BNDCHK                                     |       |
                                                  |    arraylength                             |       |
                                                  |       ==>aRegLoad                          |       |
                                                  |    ==>iRegLoad                             |       |
                                                  | compressedRefs                             |       |
                                                  |    aloadi                                  |       |
                                                  |      aladd                                 |       |
                                                  |        ...                                 |       |
                                                  |    lconst 0                                |       |
                                                  | aRegStore edi                              |       |
                                                  |     ==>aloadi                              |       |
                                                  | BBEnd                                      |       |
                                                  |     GlRegDeps ()                           |       |
                                                  |        PassThrough rdi                     |       |
                                                  |           ==>aloadi                        |       |
                                                  |        ==>aRegLoad                         |       |
                                                  |        ==>iRegLoad                         |       |
                                                  +----------+---------------------------------+       |
                                                             |                                         |
                                                             +-----------------------------------------+
                                                             |
                                                             |
                                                  +----------v---------------------------------+
                                                  | BBStart                                    |
                                                  |    GlRegDeps ()                            |
                                                  |       aRegLoad r9d                         |
                                                  |       iRegLoad ebx                         |
                                                  |       aRegLoad edi                         |
                                                  | treetop                                    |
                                                  |    ==>aRegLoad                             |
                                                  | ResolveAndNULLCHK                          |
                                                  |    iloadi  Point2D.x                       |
                                                  |       ==>aRegLoad                          |
                                                  | ...                                        |
                                                  +--------------------------------------------+

 */
void
TR::TreeLowering::printTT(char *str, TR::TreeTop *tt)
   {
   if (trace())
      {
      TR::TreeTop *prevTT = tt->getPrevTreeTop();
      TR::TreeTop *nextTT = tt->getNextTreeTop();
      TR::Node *ttFirstChild = tt->getNode()->getFirstChild();
      TR::Node *prevTTFirstChild = prevTT->getNode()->getFirstChild();
      TR::Node *nextTTFirstChild = nextTT->getNode()->getFirstChild();

      traceMsg(comp(), "   %s n%dn %s (n%dn %s), PrevTreeTop n%dn %s (n%dn %s), NextTreeTop n%dn %s (n%dn %s)\n", str,
         tt->getNode()->getGlobalIndex(), tt->getNode()->getOpCode().getName(),
         ttFirstChild ? ttFirstChild->getGlobalIndex() : -1,
         ttFirstChild ? ttFirstChild->getOpCode().getName() : "",
         prevTT->getNode()->getGlobalIndex(), prevTT->getNode()->getOpCode().getName(),
         prevTTFirstChild ? prevTTFirstChild->getGlobalIndex() : -1,
         prevTTFirstChild ? prevTTFirstChild->getOpCode().getName() : "",
         nextTT->getNode()->getGlobalIndex(), nextTT->getNode()->getOpCode().getName(),
         nextTTFirstChild ? nextTTFirstChild->getGlobalIndex() : -1,
         nextTTFirstChild ? nextTTFirstChild->getOpCode().getName() : "");
      }
   }

void
TR::TreeLowering::printBlock(char *str, TR::Block *block)
   {
   if (trace())
      {
      traceMsg(comp(), "\n   %s block_%d entry n%dn\n", str, block->getNumber(), block->getEntry()->getNode()->getGlobalIndex());
#if 1
      traceMsg(comp(), "-----------------------------------------------\n");
      TR::TreeTop *stopTree = block->getExit()->getNextTreeTop();
      TR::TreeTop *firstTree = block->getEntry();
      for (TR::TreeTop *tt = firstTree; tt && tt != stopTree; tt = tt->getNextTreeTop())
         {
         //comp()->getDebug()->printWithFixedPrefix(comp()->getOutFile(), tt->getNode(), 1, true, true, "      ");
         comp()->getDebug()->print(comp()->getOutFile(), tt);
         }
      traceMsg(comp(), "\n-----------------------------------------------\n");
#endif
      }
   }

void
TR::TreeLowering::lowerLoadArrayElement(TR::Node *node, TR::TreeTop *tt)
   {
   TR::Compilation *comp = self()->comp();
   TR::Block *originalBlock = tt->getEnclosingBlock();
   TR::Node *elementIndexNode = node->getFirstChild();
   TR::Node *arrayBaseAddressNode = node->getSecondChild();

   TR::CFG *cfg = comp->getFlowGraph();
   cfg->invalidateStructure();

   performTransformation(comp, "%sTransforming loadArrayElement n%dn [%p] in block_%d: children n%dn, n%dn. tt node n%dn, ttBeforeHelerCall node n%dn, ttAfterHelperCall node n%dn\n",
          optDetailString(), node->getGlobalIndex(), node, originalBlock->getNumber(),
          elementIndexNode->getGlobalIndex(), arrayBaseAddressNode->getGlobalIndex(),
          tt->getNode()->getGlobalIndex(), tt->getPrevTreeTop()->getNode()->getGlobalIndex(), tt->getNextTreeTop()->getNode()->getGlobalIndex());


   ///////////////////////////////////////
   // 1. Anchor the call node after the helper call split point
   // to ensure the returned value goes into either a temp or a global register
   TR::TreeTop *anchoredCallTT = TR::TreeTop::create(comp, tt, TR::Node::create(TR::treetop, 1, node));

   // Anchor elementIndex and arrayBaseAddress
   TR::TreeTop *anchoredElementIndexTT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, elementIndexNode));
   TR::TreeTop *anchoredArrayBaseAddressTT = TR::TreeTop::create(comp, anchoredElementIndexTT, TR::Node::create(TR::treetop, 1, arrayBaseAddressNode));

   printTT("anchoredCallTT", anchoredCallTT);
   printTT("anchoredElementIndexTT", anchoredElementIndexTT);
   printTT("anchoredArrayBaseAddressTT", anchoredArrayBaseAddressTT);

   printBlock("before inserting elementLoadTT originalBlock", originalBlock);


   ///////////////////////////////////////
   // 2. Create the new regular array element load node and insert it before anchoredCallTT
   TR::Node *anchoredArrayBaseAddressNode = anchoredArrayBaseAddressTT->getNode()->getFirstChild();
   TR::Node *anchoredElementIndexNode = anchoredElementIndexTT->getNode()->getFirstChild();

   TR::Node *elementAddress = J9::TransformUtil::calculateElementAddress(comp, anchoredArrayBaseAddressNode, anchoredElementIndexNode, TR::Address);
   TR::SymbolReference *elementSymRef = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(TR::Address, anchoredArrayBaseAddressNode);
   TR::Node *elementLoadNode = TR::Node::createWithSymRef(comp->il.opCodeForIndirectArrayLoad(TR::Address), 1, 1, elementAddress, elementSymRef);
   elementLoadNode->copyByteCodeInfo(node);

   if (trace())
      traceMsg(comp, "Created elementLoadNode n%dn\n", elementLoadNode->getGlobalIndex());

   anchoredCallTT->insertBefore(TR::TreeTop::create(comp, TR::Node::createWithSymRef(TR::NULLCHK, 1, 1, TR::Node::create(TR::PassThrough, 1, anchoredArrayBaseAddressNode), comp->getSymRefTab()->findOrCreateNullCheckSymbolRef(comp->getMethodSymbol()))));

   printBlock("after inserting NULLCHK originalBlock", originalBlock);

   int32_t dataWidth = TR::Symbol::convertTypeToSize(TR::Address);
   if (comp->useCompressedPointers())
      dataWidth = TR::Compiler->om.sizeofReferenceField();

   TR::Node *arraylengthNode = TR::Node::create(TR::arraylength, 1, anchoredArrayBaseAddressNode);
   arraylengthNode->setArrayStride(dataWidth);
   anchoredCallTT->insertBefore(TR::TreeTop::create(comp, TR::Node::createWithSymRef(TR::BNDCHK, 2, 2, arraylengthNode, anchoredElementIndexNode, comp->getSymRefTab()->findOrCreateArrayBoundsCheckSymbolRef(comp->getMethodSymbol()))));

   printBlock("after inserting BNDCHK originalBlock", originalBlock);

   if (comp->useCompressedPointers())
      anchoredCallTT->insertBefore(TR::TreeTop::create(comp, TR::Node::createCompressedRefsAnchor(elementLoadNode)));
   else
      anchoredCallTT->insertBefore(TR::TreeTop::create(comp, TR::Node::create(node, TR::treetop, 1, elementLoadNode)));

   printBlock("after inserting array element load originalBlock", originalBlock);


   ///////////////////////////////////////
   // 3. Split the block after the helper call
   printTT("splitPostGRA at tt->getNextTreeTop()", tt->getNextTreeTop());

   TR::Block *blockAfterHelperCall = originalBlock->splitPostGRA(tt->getNextTreeTop(), cfg, true, NULL);

   if (trace())
      traceMsg(comp, "Isolated regular array element load node n%dn and the anchored call node n%dn in block_%d\n",
            elementLoadNode->getGlobalIndex(), anchoredCallTT->getNode()->getGlobalIndex(), blockAfterHelperCall->getNumber());

   printBlock("after splitting the original block originalBlock", originalBlock);
   printBlock("after splitting the original block blockAfterHelperCall", blockAfterHelperCall);


   ///////////////////////////////////////
   // 4. Move the helper call node to the end of the originalBlock
   //
   // As the block is split after the helper call node, it is possible that as part of un-commoning
   // code to store nodes into registers or temp-slots is appended to the original block by the call
   // to splitPostGRA above.  Move the helper call treetop to the end of originalBlock, along with
   // any stores resulting from un-commoning of the nodes in the helper call tree so that it can be
   // split into its own call block.
   TR::TreeTop *originalBlockExit = originalBlock->getExit();
   TR::TreeTop *iterTT = tt->getNextTreeTop();

   if (iterTT != originalBlockExit)
      {
      // Remove TreeTop for call node, and gather it and the treetops for stores that
      // resulted from un-commoning in a TreeTop chain from tt to lastTTForCallBlock
      tt->unlink(false);
      TR::TreeTop *lastTTForCallBlock = tt;

      while (iterTT != originalBlockExit)
         {
         TR::TreeTop *nextTT = iterTT->getNextTreeTop();
         TR::ILOpCodes op = iterTT->getNode()->getOpCodeValue();

         printTT("iterTT", iterTT);

         if ((op == TR::aRegStore || op == TR::astore) && iterTT->getNode()->getFirstChild() == node)
            {
            if (trace())
               traceMsg(comp, "Moving treetop node n%dn [%p] for store of the helper result to the end of block_%d in preparation of final block split\n", iterTT->getNode()->getGlobalIndex(), iterTT->getNode(), originalBlock->getNumber());

            // Remove store node from originalBlock temporarily
            iterTT->unlink(false);
            lastTTForCallBlock->join(iterTT);
            lastTTForCallBlock = iterTT;
            }

         iterTT = nextTT;
         }

      // Insert "tt->iterTT->...->iterTT" before BBEnd
      // Move the treetops that were gathered for the call and any stores of the
      // result to the end of the block in preparation for the split of the call block
      originalBlockExit->getPrevTreeTop()->join(tt);
      lastTTForCallBlock->join(originalBlockExit);
      }

   printBlock("after moving the nodes originalBlock", originalBlock);


   ///////////////////////////////////////
   // 5. Split at the helper call node into its own block
   TR::Block *helperCallBlock = originalBlock->split(tt, cfg);

   helperCallBlock->setIsExtensionOfPreviousBlock(true);

   if (trace())
      traceMsg(comp, "Isolated helper call node n%dn in block_%d\n", node->getGlobalIndex(), helperCallBlock->getNumber());

   printBlock("after split the helper call originalBlock", originalBlock);
   printBlock("after split the helper call helperCallBlock", helperCallBlock);


   ///////////////////////////////////////
   // 6. Create a store node that will be used to save the return value
   // of the helper call or the regular array load. It uses the same register
   // as the anchored node.
   TR::Node *anchoredNode = anchoredCallTT->getNode()->getFirstChild();
   TR::Node *storeNode = TR::TreeLowering::createStoreNodeForAnchoredNode(anchoredNode, TR::Node::aconst(0), "aconst(0)");
   originalBlock->append(TR::TreeTop::create(comp, storeNode));

   if (trace())
      traceMsg(comp, "Append storeNode n%dn %s to block_%d\n", storeNode->getGlobalIndex(), storeNode->getOpCode().getName(), originalBlock->getNumber());

   printBlock("after append storeNode nodes originalBlock", originalBlock);


   ///////////////////////////////////////
   // 7. Create the ificmpeq node that checks classFlags
   TR::SymbolReference *vftSymRef = comp->getSymRefTab()->findOrCreateVftSymbolRef();
   TR::SymbolReference *arrayCompSymRef = comp->getSymRefTab()->findOrCreateArrayComponentTypeSymbolRef();
   TR::SymbolReference *classFlagsSymRef = comp->getSymRefTab()->findOrCreateClassFlagsSymbolRef();

   TR::Node *vft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredArrayBaseAddressNode, vftSymRef);
   TR::Node *arrayCompClass = TR::Node::createWithSymRef(node, TR::aloadi, 1, vft, arrayCompSymRef);
   TR::Node *loadClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, arrayCompClass, classFlagsSymRef);
   TR::Node *isValueTypeNode = TR::Node::create(node, TR::iand, 2, loadClassFlags, TR::Node::iconst(node, J9ClassIsValueType));

   // The branch destination will be set up later when the regular array load block is created
   TR::Node *ifNode = TR::Node::createif(TR::ificmpeq, isValueTypeNode, TR::Node::iconst(node, 0));

   // Copy register dependency to the ificmpeq node that's being appended to the current block
   copyRegisterDependencyBasedOnAnchoredNode(helperCallBlock, ifNode, anchoredNode, storeNode);

   // Append the ificmpeq node that checks classFlags to the original block
   originalBlock->append(TR::TreeTop::create(comp, ifNode));

   if (trace())
      traceMsg(comp, "Append ifNode n%dn to block_%d\n", ifNode->getGlobalIndex(), originalBlock->getNumber());

   printBlock("after append ifNode nodes originalBlock", originalBlock);


   ///////////////////////////////////////
   // 8. Split the regular array element load from the anchored call
   //
   // Store the regular array element load result to the same anchored node register
   TR::Node *storeArrayElementNode = TR::TreeLowering::createStoreNodeForAnchoredNode(anchoredNode, elementLoadNode, "array element load");

   anchoredCallTT->insertBefore(TR::TreeTop::create(comp, storeArrayElementNode));

   printBlock("before split at anchored call blockAfterHelperCall", blockAfterHelperCall);

   TR::Block *blockAfterArrayElementLoad = blockAfterHelperCall->splitPostGRA(anchoredCallTT, cfg, true, NULL);

   if (trace())
      traceMsg(comp, "Isolated the anchored call node n%dn in block_%d\n", anchoredCallTT->getNode()->getGlobalIndex(), blockAfterArrayElementLoad->getNumber());

   printBlock("after split at anchored call blockAfterHelperCall", blockAfterHelperCall);
   printBlock("after split at anchored call blockAfterArrayElementLoad", blockAfterArrayElementLoad);

   // Fix the register load to the stored array element if register is used
   if (blockAfterHelperCall->getExit()->getNode()->getNumChildren() != 0
         && storeArrayElementNode->getOpCodeValue() == TR::aRegStore)
      {
      TR::Node *blkDeps = blockAfterHelperCall->getExit()->getNode()->getFirstChild();

      for (int i = 0; i < blkDeps->getNumChildren(); i++)
         {
         TR::Node *regDep = blkDeps->getChild(i);

         if (trace())
            traceMsg(comp,"blkDeps n%dn [%d] %s %s storeArrayElementNode to %s\n",
                  regDep->getGlobalIndex(), i, regDep->getOpCode().getName(),
                  comp->getDebug()->getGlobalRegisterName(regDep->getGlobalRegisterNumber()),
                  comp->getDebug()->getGlobalRegisterName(storeArrayElementNode->getGlobalRegisterNumber()));

         if (regDep->getOpCodeValue() == TR::aRegLoad &&
             regDep->getGlobalRegisterNumber() == storeArrayElementNode->getGlobalRegisterNumber())
            {
            TR::Node * depNode = TR::Node::create(TR::PassThrough, 1, storeArrayElementNode->getChild(0));
            depNode->setGlobalRegisterNumber(storeArrayElementNode->getGlobalRegisterNumber());
            blkDeps->addChildren(&depNode, 1);

            blkDeps->removeChild(i);
            break;
            }
         }
      }


   ///////////////////////////////////////
   // 9. Set up the edges between the blocks
   ifNode->setBranchDestination(blockAfterHelperCall->getEntry());

   // Add goto block from helper call to the block after the array element load block
   TR::Node *gotoAfterHelperCallNode = TR::Node::create(helperCallBlock->getExit()->getNode(), TR::Goto, 0, blockAfterArrayElementLoad->getEntry());

   copyRegisterDependency(helperCallBlock->getExit()->getNode(), gotoAfterHelperCallNode);

   helperCallBlock->append(TR::TreeTop::create(comp, gotoAfterHelperCallNode));

   cfg->addEdge(originalBlock, blockAfterHelperCall);

   cfg->removeEdge(helperCallBlock, blockAfterHelperCall);

   cfg->addEdge(helperCallBlock, blockAfterArrayElementLoad);
   }

void
TR::TreeLowering::copyRegisterDependencyBasedOnAnchoredNode(TR::Block *fromBlock, TR::Node *toNode, TR::Node *anchoredNode, TR::Node *storeNode)
   {
   if (fromBlock->getExit()->getNode()->getNumChildren() > 0)
      {
      TR::Node *glRegDeps = TR::Node::create(TR::GlRegDeps);
      TR::Node *depNode = NULL;

      if (anchoredNode->getOpCodeValue() == TR::aRegLoad)
         {
         depNode = TR::Node::create(TR::PassThrough, 1, storeNode->getChild(0));
         depNode->setGlobalRegisterNumber(storeNode->getGlobalRegisterNumber());
         glRegDeps->addChildren(&depNode, 1);
         }

      toNode->addChildren(&glRegDeps, 1);

      TR::Node *expectedDeps = fromBlock->getExit()->getNode()->getFirstChild();
      for (int i = 0; i < expectedDeps->getNumChildren(); ++i)
         {
         TR::Node *temp = expectedDeps->getChild(i);
         if (depNode && temp->getGlobalRegisterNumber() == depNode->getGlobalRegisterNumber())
            continue;
         else if (temp->getOpCodeValue() == TR::PassThrough)
            {
            // PassThrough nodes cannot be commoned because doing so does not
            // actually anchor the child, causing it's lifetime to not be extended
            TR::Node *original = temp;
            temp = TR::Node::create(original, TR::PassThrough, 1, original->getFirstChild());
            temp->setLowGlobalRegisterNumber(original->getLowGlobalRegisterNumber());
            temp->setHighGlobalRegisterNumber(original->getHighGlobalRegisterNumber());
            }
         glRegDeps->addChildren(&temp, 1);
         }
      }
   }

void
TR::TreeLowering::copyRegisterDependency(TR::Node *fromNode, TR::Node *toNode)
   {
   if (fromNode->getNumChildren() != 0)
      {
      TR::Node *blkDeps = fromNode->getFirstChild();
      TR::Node *newDeps = TR::Node::create(blkDeps, TR::GlRegDeps);

      for (int i = 0; i < blkDeps->getNumChildren(); i++)
         {
         TR::Node *regDep = blkDeps->getChild(i);

         if (regDep->getOpCodeValue() == TR::PassThrough)
            {
            TR::Node *orig= regDep;
            regDep = TR::Node::create(orig, TR::PassThrough, 1, orig->getFirstChild());
            regDep->setLowGlobalRegisterNumber(orig->getLowGlobalRegisterNumber());
            regDep->setHighGlobalRegisterNumber(orig->getHighGlobalRegisterNumber());
            }

         newDeps->addChildren(&regDep, 1);
         }

      toNode->addChildren(&newDeps, 1);
      }
   }

TR::Node *
TR::TreeLowering::createStoreNodeForAnchoredNode(TR::Node *anchoredNode, TR::Node *nodeToBeStored, const char *msg)
   {
   TR::Compilation *comp = self()->comp();
   TR::Node *storeNode = NULL;

   // After splitPostGRA anchoredNode which was the helper call node
   // should have been transformed into a aRegLoad or aload
   if (anchoredNode->getOpCodeValue() == TR::aRegLoad)
      {
      storeNode = TR::Node::create(TR::aRegStore, 1, nodeToBeStored);
      storeNode->setGlobalRegisterNumber(anchoredNode->getGlobalRegisterNumber());
      if (trace())
         traceMsg(comp, "Storing %s n%dn in register %s storeNode n%dn anchoredNode n%dn\n", msg, nodeToBeStored->getGlobalIndex(), comp->getDebug()->getGlobalRegisterName(anchoredNode->getGlobalRegisterNumber()), storeNode->getGlobalIndex(), anchoredNode->getGlobalIndex());
      }
   else if (anchoredNode->getOpCodeValue() == TR::aload)
      {
      storeNode = TR::Node::create(TR::astore, 0, nodeToBeStored);
      storeNode->setSymbolReference(anchoredNode->getSymbolReference());
      if (trace())
         traceMsg(comp, "Storing %s n%dn to symref %d (%s) storeNode n%dn anchoredNode n%dn\n", msg, nodeToBeStored->getGlobalIndex(), anchoredNode->getSymbolReference()->getReferenceNumber(), anchoredNode->getSymbolReference()->getName(comp->getDebug()), storeNode->getGlobalIndex(), anchoredNode->getGlobalIndex());
      }
   else
      {
      TR_ASSERT_FATAL_WITH_NODE(anchoredNode, false, "Anchored call has been turned into unexpected opcode\n");
      }

   return storeNode;
   }

/*
+-------------------------------------------+        +---------------------------------------------+
| treetop                                   |        |  BBStart                                    |
|    acall  jitStoreFlattenableArrayElement |        |  treetop                                    |
|       aload <value>                       | -----> |     aload <ArrayAddress>                    |
|       iload <index>                       |        |  treetop                                    |
|       aload <arrayAddress>                |        |     aload <index>                           |
| ttAfterArrayElementStore                  |        |  treetop                                    |
+-------------------------------------------+        |     aload <value>                           |
                                                     |  ificmpeq ---------------------------------------------+
                                                     |     iand                                    |          |
                                                     |        iloadi  <isClassFlags>               |          |
                                                     |        ...                                  |          |
                                                     |        iconst 1024                          |          |
                                                     |     iconst 0                                |          |
                                                     |     GlRegDeps ()                            |          |
                                                     |        PassThrough rcx                      |          |
                                                     |           ==>aload                          |          |
                                                     |        PassThrough r8                       |          |
                                                     |           ==>aload                          |          |
                                                     |        PassThrough rdi                      |          |
                                                     |           ==>iload                          |          |
                                                     |  BBEnd                                      |          |
                                                     +---------------------------------------------+          |
                                                     +---------------------------------------------+          |
                                                     |  BBStart                                    |          |
                                                     |  NULLCHK                                    |          |
                                                     |     PassThrough                             |          |
                                                     |        ==>aload                             |          |
                                                     |  treetop                                    |          |
                                                     |     acall  jitStoreFlattenableArrayElement  |          |
                                                     |         ==>aload                            |          |
                                                     |         ==>iload                            |          |
                                                     |         ==>aload                            |          |
                                                     |  ...                                        |          |
                                                     |  goto -->-----------------------------------------+    |
                                                     |     GlRegDeps ()                            |     |    |
                                                     |        PassThrough rcx                      |     |    |
                                                     |           ==>aload                          |     |    |
                                                     |        PassThrough r8                       |     |    |
                                                     |           ==>aload                          |     |    |
                                                     |        PassThrough rdi                      |     |    |
                                                     |           ==>iload                          |     |    |
                                                     |  BBEnd                                      |     |    |
                                                     |     GlRegDeps ()                            |     |    |
                                                     |        PassThrough rcx                      |     |    |
                                                     |           ==>aload                          |     |    |
                                                     |        PassThrough r8                       |     |    |
                                                     |           ==>aload                          |     |    |
                                                     |        PassThrough rdi                      |     |    |
                                                     |           ==>iload                          |     |    |
                                                     |                                             |     |    |
                                                     +----------------------|----------------------+     |    |
                                                                            |                            |    |
                                                                            |                            |    |
                                                                            -----------------------------|-----
                                                                            |                            |
                                                                            |                            |
                                                                            |                            |
                                                     +----------------------v----------------------+     |
                                                     |  BBStart                                    |     |
                                                     |     GlRegDeps ()                            |     |
                                                     |        aRegLoad ecx                         |     |
                                                     |        aRegLoad r8d                         |     |
                                                     |        iRegLoad edi                         |     |
                                                     |  NULLCHK on n82n                            |     |
                                                     |      ...                                    |     |
                                                     |  BNDCHK                                     |     |
                                                     |      ...                                    |     |
                                                     |  treetop                                    |     |
                                                     |      ArrayStoreCHK                          |     |
                                                     |         awrtbari                            |     |
                                                     |         ...                                 |     |
                                                     |  BBEnd                                      |     |
                                                     |      GlRegDeps                              |     |
                                                     +----------------------|----------------------+     |
                                                                            |                            |
                                                                            ------------------------------
                                                                            |
                                                     +----------------------v----------------------+
                                                     | ttAfterArrayElementStore                    |
                                                     +---------------------------------------------+

 */
void
TR::TreeLowering::lowerStoreArrayElement(TR::Node *node, TR::TreeTop *tt)
   {
   TR::Compilation *comp = self()->comp();
   TR::Block *originalBlock = tt->getEnclosingBlock();

   TR::Node *valueNode = node->getFirstChild();
   TR::Node *elementIndexNode = node->getSecondChild();
   TR::Node *arrayBaseAddressNode = node->getThirdChild();

   TR::CFG *cfg = comp->getFlowGraph();
   cfg->invalidateStructure();

   performTransformation(comp, "%sTransforming storeArrayElement n%dn [%p] in block_%d: children (n%dn, n%dn, n%dn) tt node n%dn, ttBeforeHelerCall node n%dn, ttAfterHelperCall node n%dn\n",
          optDetailString(), node->getGlobalIndex(), node, originalBlock->getNumber(),
          valueNode->getGlobalIndex(), elementIndexNode->getGlobalIndex(), arrayBaseAddressNode->getGlobalIndex(),
          tt->getNode()->getGlobalIndex(), tt->getPrevTreeTop()->getNode()->getGlobalIndex(), tt->getNextTreeTop()->getNode()->getGlobalIndex());


   ///////////////////////////////////////
   // 1. Anchor all the children nodes
   TR::TreeTop *anchoredArrayBaseAddressTT = TR::TreeTop::create(comp, tt->getPrevTreeTop(), TR::Node::create(TR::treetop, 1, arrayBaseAddressNode));
   TR::TreeTop *anchoredElementIndexTT = TR::TreeTop::create(comp, anchoredArrayBaseAddressTT, TR::Node::create(TR::treetop, 1, elementIndexNode));
   TR::TreeTop *anchoredValueTT = TR::TreeTop::create(comp, anchoredElementIndexTT, TR::Node::create(TR::treetop, 1, valueNode));

   printTT("anchoredValueTT", anchoredValueTT);
   printTT("anchoredElementIndexTT", anchoredElementIndexTT);
   printTT("anchoredArrayBaseAddressTT", anchoredArrayBaseAddressTT);


   ///////////////////////////////////////
   // 2. Create the new ArrayStoreCHK, BNDCHK, NULLCHK
   TR::Node *anchoredElementIndexNode = anchoredElementIndexTT->getNode()->getFirstChild();
   TR::Node *anchoredArrayBaseAddressNode = anchoredArrayBaseAddressTT->getNode()->getFirstChild();
   TR::Node *anchoredValueNode = anchoredValueTT->getNode()->getFirstChild();

   TR::Node *elementAddress = J9::TransformUtil::calculateElementAddress(comp, anchoredArrayBaseAddressNode, anchoredElementIndexNode, TR::Address);

   TR::SymbolReference *elementSymRef = comp->getSymRefTab()->findOrCreateArrayShadowSymbolRef(TR::Address, anchoredArrayBaseAddressNode);
   TR::Node *elementStoreNode = TR::Node::createWithSymRef(TR::awrtbari, 3, 3, elementAddress, anchoredValueNode, anchoredArrayBaseAddressNode, elementSymRef);

   TR::SymbolReference *arrayStoreCHKSymRef = comp->getSymRefTab()->findOrCreateTypeCheckArrayStoreSymbolRef(comp->getMethodSymbol());
   TR::Node *arrayStoreCHKNode = TR::Node::createWithRoomForThree(TR::ArrayStoreCHK, elementStoreNode, 0, arrayStoreCHKSymRef);

   arrayStoreCHKNode->copyByteCodeInfo(node);

   if (trace())
      traceMsg(comp, "Created arrayStoreCHKNode n%dn\n", arrayStoreCHKNode->getGlobalIndex());

   TR::TreeTop *ttAfterHelperCall = tt->getNextTreeTop();

   printTT("before insert arrayStoreCHKNode ttAfterHelperCall", ttAfterHelperCall);

   ttAfterHelperCall->insertBefore(TR::TreeTop::create(comp, TR::Node::createWithSymRef(TR::NULLCHK, 1, 1, TR::Node::create(TR::PassThrough, 1, anchoredArrayBaseAddressNode), comp->getSymRefTab()->findOrCreateNullCheckSymbolRef(comp->getMethodSymbol()))));

   printBlock("after insert NULLCHK", originalBlock);

   int32_t dataWidth = TR::Symbol::convertTypeToSize(TR::Address);
   if (comp->useCompressedPointers())
      dataWidth = TR::Compiler->om.sizeofReferenceField();

   TR::Node *arraylengthNode = TR::Node::create(TR::arraylength, 1, anchoredArrayBaseAddressNode);
   arraylengthNode->setArrayStride(dataWidth);
   ttAfterHelperCall->insertBefore(TR::TreeTop::create(comp, TR::Node::createWithSymRef(TR::BNDCHK, 2, 2, arraylengthNode, anchoredElementIndexNode, comp->getSymRefTab()->findOrCreateArrayBoundsCheckSymbolRef(comp->getMethodSymbol()))));

   printBlock("after insert BNDCHK", originalBlock);

   TR::TreeTop * arrayStoreCHKTT = NULL;
   arrayStoreCHKTT = ttAfterHelperCall->insertBefore(TR::TreeTop::create(comp, arrayStoreCHKNode));

   if (comp->useCompressedPointers())
      arrayStoreCHKTT = ttAfterHelperCall->insertBefore(TR::TreeTop::create(comp, TR::Node::createCompressedRefsAnchor(elementStoreNode)));

   printBlock("after insert arrayStoreCHKNode", originalBlock);


   ///////////////////////////////////////
   // 3. Split the block after the helper call
   printTT("splitPostGRA at tt->getNextTreeTop()", tt->getNextTreeTop());

   TR::Block *blockAfterHelperCall = originalBlock->splitPostGRA(tt->getNextTreeTop(), cfg, true, NULL);

   printBlock("after splitting the original block", originalBlock);
   printBlock("blockAfterHelperCall after splitting the original block", blockAfterHelperCall);


   ///////////////////////////////////////
   // 4. Move the helper call node to the end of the originalBlock
   //
   // As the block is split after the helper call node, it is possible that as part of un-commoning
   // code to store nodes into registers or temp-slots is appended to the original block by the call
   // to splitPostGRA above.  Move the helper call treetop to the end of originalBlock, along with
   // any stores resulting from un-commoning of the nodes in the helper call tree so that it can be
   // split into its own call block.
   // Remove TreeTop for call node, and gather it and the treetops for stores that
   // resulted from un-commoning in a TreeTop chain from tt to lastTTForCallBlock

   TR::TreeTop *originalBlockExit = originalBlock->getExit();
   if (tt->getNextTreeTop() != originalBlockExit)
      {
      tt->unlink(false);
      originalBlockExit->getPrevTreeTop()->join(tt);
      tt->join(originalBlockExit);
      }

   printBlock("after moving the nodes originalBlock", originalBlock);


   ///////////////////////////////////////
   // 5. Split at the helper call node including the nullchk on value into its own block helperCallBlock

   // Insert NULLCHK for VT
   TR::TreeTop *ttForHelperCallBlock = tt;

   if (!anchoredValueNode->isNonNull())
      {
      TR::Node *passThru  = TR::Node::create(node, TR::PassThrough, 1, anchoredValueNode);
      TR::Node *nullCheck = TR::Node::createWithSymRef(node, TR::NULLCHK, 1, passThru, comp->getSymRefTab()->findOrCreateNullCheckSymbolRef(comp->getMethodSymbol()));
      ttForHelperCallBlock = tt->insertBefore(TR::TreeTop::create(comp, nullCheck));
      }

   TR::Block *helperCallBlock = originalBlock->split(ttForHelperCallBlock, cfg);

   helperCallBlock->setIsExtensionOfPreviousBlock(true);

   if (trace())
      traceMsg(comp, "Isolated helper call node n%dn in block_%d\n", node->getGlobalIndex(), helperCallBlock->getNumber());

   printBlock("original block after split the helper call", originalBlock);
   printBlock("helperCallBlock", helperCallBlock);


   ///////////////////////////////////////
   // 6. Create the ificmpeq node that checks classFlags
   TR::SymbolReference *vftSymRef = comp->getSymRefTab()->findOrCreateVftSymbolRef();
   TR::SymbolReference *arrayCompSymRef = comp->getSymRefTab()->findOrCreateArrayComponentTypeSymbolRef();
   TR::SymbolReference *classFlagsSymRef = comp->getSymRefTab()->findOrCreateClassFlagsSymbolRef();

   TR::Node *vft = TR::Node::createWithSymRef(node, TR::aloadi, 1, anchoredArrayBaseAddressNode, vftSymRef);
   TR::Node *arrayCompClass = TR::Node::createWithSymRef(node, TR::aloadi, 1, vft, arrayCompSymRef);
   TR::Node *loadClassFlags = TR::Node::createWithSymRef(node, TR::iloadi, 1, arrayCompClass, classFlagsSymRef);
   TR::Node *isValueTypeNode = TR::Node::create(node, TR::iand, 2, loadClassFlags, TR::Node::iconst(node, J9ClassIsValueType));

   // The branch destination will be set up later when the regular array element store block is created
   TR::Node *ifNode = TR::Node::createif(TR::ificmpeq, isValueTypeNode, TR::Node::iconst(node, 0));

   // Copy register dependency to the ificmpeq node that's being appended to the current block
   copyRegisterDependency(helperCallBlock->getExit()->getNode(), ifNode);

   // Append the ificmpeq node that checks classFlags to the original block
   originalBlock->append(TR::TreeTop::create(comp, ifNode));

   if (trace())
      traceMsg(comp, "Append ifNode n%dn to block_%d\n", ifNode->getGlobalIndex(), originalBlock->getNumber());


   ///////////////////////////////////////
   // 7. Split after the regular array element store
   printTT("Split at arrayStoreCHKTT->getNextTreeTop()", arrayStoreCHKTT->getNextTreeTop());

   TR::Block *blockAfterArrayElementStore = blockAfterHelperCall->splitPostGRA(arrayStoreCHKTT->getNextTreeTop(), cfg, true, NULL);

   if (trace())
      traceMsg(comp, "Isolated node n%dn in block_%d\n", arrayStoreCHKTT->getNextTreeTop()->getNode()->getGlobalIndex(), blockAfterArrayElementStore->getNumber());

   printBlock("blockAfterHelperCall", blockAfterHelperCall);
   printBlock("blockAfterArrayElementStore", blockAfterArrayElementStore);


   ///////////////////////////////////////
   // 8. Set up the edges between the blocks
   ifNode->setBranchDestination(blockAfterHelperCall->getEntry());

   // Add goto block from helper call to the block after the array element store block
   TR::Node *gotoAfterHelperCallNode = TR::Node::create(helperCallBlock->getExit()->getNode(), TR::Goto, 0, blockAfterArrayElementStore->getEntry());

   copyRegisterDependency(helperCallBlock->getExit()->getNode(), gotoAfterHelperCallNode);

   helperCallBlock->append(TR::TreeTop::create(comp, gotoAfterHelperCallNode));

   cfg->addEdge(originalBlock, blockAfterHelperCall);

   cfg->removeEdge(helperCallBlock, blockAfterHelperCall);

   cfg->addEdge(helperCallBlock, blockAfterArrayElementStore);
   }
