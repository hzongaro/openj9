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
#include "compile/Compilation.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "optimizer/ForceHelperTransform.hpp"
#include "optimizer/TransformUtil.hpp"

#define OPT_DETAILS "O^O Force Helper Transform:  "

ForceHelperTransform::ForceHelperTransform(TR::OptimizationManager *manager)
   : TR::Optimization(manager),
     _helperCallsToBeFolded(trMemory())
   {
   }

const char *
ForceHelperTransform::optDetailString() const throw()
   {
   return "O^O Force Helper Transform: ";
   }

static bool owningMethodDoesNotContainStoreChecks(ForceHelperTransform *fht, TR::Node *node)
   {
   TR::ResolvedMethodSymbol *method = fht->comp()->getOwningMethodSymbol(node->getOwningMethod());
   if (method && method->skipArrayStoreChecks())
      return true;
   return false;
   }

static bool owningMethodDoesNotContainBoundChecks(ForceHelperTransform *fht, TR::Node *node)
   {
   TR::ResolvedMethodSymbol *method = fht->comp()->getOwningMethodSymbol(node->getOwningMethod());
   if (method && method->skipBoundChecks())
      return true;
   return false;
   }

int32_t ForceHelperTransform::perform()
   {
   if (trace())
      {
      traceMsg(comp(), "Starting ForceHelperTransform\n");
      comp()->dumpMethodTrees("Trees at start of ForceHelperTransform");
      }

   TR::NodeChecklist visited(comp());

   for (TR::TreeTop *tt = comp()->getStartTree(); tt; tt = tt->getNextTreeTop())
      {
      _curTree = tt;
      visitNode(tt->getNode(), visited);
      }

   doDelayedTransformations();

   if (trace())
      {
      traceMsg(comp(), "Ending ForceHelperTransform\n");
      comp()->dumpMethodTrees("Trees at end of ForceHelperTransform");
      }

   return 1;
   }


void ForceHelperTransform::visitNode(TR::Node * node, TR::NodeChecklist &visited)
   {
   if (visited.contains(node))
      {
      return;
      }

   visited.add(node);

   if (node->getOpCode().isCall())
      {
      // IL Generation only uses the <objectInequalityComparison> non-helper today,
      // but we should be prepared for <objectEqualityComparisonSymbol> as well.
      const bool isObjectEqualityCompare =
         comp()->getSymRefTab()->isNonHelper(
            node->getSymbolReference(),
            TR::SymbolReferenceTable::objectEqualityComparisonSymbol);

      const bool isObjectInequalityCompare =
         comp()->getSymRefTab()->isNonHelper(
            node->getSymbolReference(),
            TR::SymbolReferenceTable::objectInequalityComparisonSymbol);

      if (isObjectEqualityCompare || isObjectInequalityCompare)
         {
         TR::ILOpCode acmpOp = isObjectEqualityCompare ? comp()->il.opCodeForCompareEquals(TR::Address)
                                                       : comp()->il.opCodeForCompareNotEquals(TR::Address);

         if (performTransformation(
               comp(),
               "%sChanging n%un from %s to %s\n",
               OPT_DETAILS,
               node->getGlobalIndex(),
               comp()->getSymRefTab()->getNonHelperSymbolName(isObjectEqualityCompare ? TR::SymbolReferenceTable::objectEqualityComparisonSymbol
                                                                                      : TR::SymbolReferenceTable::objectInequalityComparisonSymbol),
               acmpOp.getName()))
            {
            // Replace the non-helper equality/inequality comparison with an address comparison
            TR::Node::recreate(node, acmpOp.getOpCodeValue());
            }
         }

      // Check for call to jit{Load|Store}FlattenableArrayElement helpers
      const bool isLoadFlattenableArrayElement =
                    node->getOpCode().isCall()
                    && node->getSymbolReference()
                       == comp()->getSymRefTab()->findOrCreateLoadFlattenableArrayElementSymbolRef();
   
      const bool isStoreFlattenableArrayElement =
                    node->getOpCode().isCall()
                    && node->getSymbolReference()
                       == comp()->getSymRefTab()->findOrCreateStoreFlattenableArrayElementSymbolRef();
   
      if (isLoadFlattenableArrayElement || isStoreFlattenableArrayElement)
         {
         flags8_t flagsForTransform(isLoadFlattenableArrayElement ? ValueTypesHelperCallTransform::IsArrayLoad
                                                                  : ValueTypesHelperCallTransform::IsArrayStore);
   
         if (isStoreFlattenableArrayElement && !owningMethodDoesNotContainStoreChecks(this, node))
            {
            TR::Node *storeValueNode = node->getChild(0);

            // If storing to an array whose component type is or might be a value type
            // and the value that's being assigned is or might be null, both a run-time
            // NULLCHK of the value is required (guarded by a check of whether the
            // component type is a value type) and an ArrayStoreCHK are required;
            // otherwise, only the ArrayStoreCHK is required.
            //
            if (!storeValueNode->isNonNull())
               {
               flagsForTransform.set(ValueTypesHelperCallTransform::RequiresStoreAndNullCheck);
               }
            else
               {
               flagsForTransform.set(ValueTypesHelperCallTransform::RequiresStoreCheck);
               }
            }

         if (!owningMethodDoesNotContainBoundChecks(this, node))
            {
            flagsForTransform.set(ValueTypesHelperCallTransform::RequiresBoundCheck);
            }

         _helperCallsToBeFolded.add(
               new (trStackMemory()) ValueTypesHelperCallTransform(_curTree, node, flagsForTransform));

         }
      }

   for (int i = 0; i < node->getNumChildren(); i++)
      {
      visitNode(node->getChild(i), visited);
      }
   }

void ForceHelperTransform::doDelayedTransformations()
   {
      // Process transformations for calls to value types helpers or non-helpers
   ListIterator<ValueTypesHelperCallTransform> valueTypesHelperCallsToBeFolded(&_helperCallsToBeFolded);

   for (ValueTypesHelperCallTransform *callToTransform = valueTypesHelperCallsToBeFolded.getFirst();
        callToTransform != NULL;
        callToTransform = valueTypesHelperCallsToBeFolded.getNext())
      {
      TR::TreeTop *callTree = callToTransform->_tree;
      TR::Node *callNode = callToTransform->_callNode;

      const bool isLoad = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::IsArrayLoad);
      const bool isStore = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::IsArrayStore);
      const bool isCompare = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::IsRefCompare);
      const bool needsStoreCheck = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::RequiresStoreCheck);
      const bool needsStoreAndNullCheck = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::RequiresStoreAndNullCheck);
      const bool needsBoundCheck = callToTransform->_flags.testAny(ValueTypesHelperCallTransform::RequiresBoundCheck);

      // performTransformation was already checked for comparison non-helper call
      // Only need to check for array element load or store helper calls
      if (!isCompare && !performTransformation(
                            comp(),
                            "%s Replacing n%dn from acall of <jit%sFlattenableArrayElement> to aloadi\n",
                            OPT_DETAILS,
                            callNode->getGlobalIndex(),
                            isLoad ? "Load" : "Store"))
         {
         continue;
         }

      // Insert dynamic debug counter to describe successful transformation of value type helper or non-helper call
      if (callToTransform->_flags.testAny(ValueTypesHelperCallTransform::InsertDebugCounter))
         {
         const char *operationName = isLoad ? "aaload" : (isStore ? "aastore" : "acmp");

         const char *counterName = TR::DebugCounter::debugCounterName(comp(), "vt-helper/vp-xformed/%s/(%s)/bc=%d",
                                                               operationName, comp()->signature(), callNode->getByteCodeIndex());
         TR::DebugCounter::prependDebugCounter(comp(), counterName, callTree);
         }

      // Transformation for comparison was already handled.  Just needed post-processing to be able to insert debug counter
      if (isCompare)
         {
         continue;
         }

      TR_ASSERT_FATAL_WITH_NODE(callNode, !comp()->requiresSpineChecks(), "Cannot handle VP yet for jit{Load|Store}FlattenableArrayElement if SpineCHKs are required\n");

      int opIndex = 0;

      TR::Node *valueNode = isLoad ? NULL : callNode->getChild(opIndex++);
      TR::Node *indexNode = callNode->getChild(opIndex++);
      TR::Node *arrayRefNode = callNode->getChild(opIndex);

      TR::Node *elementAddressNode = J9::TransformUtil::calculateElementAddress(comp(), arrayRefNode, indexNode, TR::Address);

      if (needsBoundCheck)
         {
         const int32_t width = comp()->useCompressedPointers() ? TR::Compiler->om.sizeofReferenceField()
                                                               : TR::Symbol::convertTypeToSize(TR::Address);

         TR::Node *arrayLengthNode = TR::Node::create(callNode, TR::arraylength, 1, arrayRefNode);
         arrayLengthNode->setArrayStride(width);

         TR::Node *bndChkNode = TR::Node::createWithSymRef(TR::BNDCHK, 2, 2, arrayLengthNode, indexNode,
                                             comp()->getSymRefTab()->findOrCreateArrayBoundsCheckSymbolRef(comp()->getMethodSymbol()));
         callTree->insertBefore(TR::TreeTop::create(comp(), bndChkNode));

         // This might be the first time the array bounds check symbol reference is used
         // Need to ensure aliasing for them is correctly constructed
         //
         optimizer()->setAliasSetsAreValid(false);
         }

      TR::SymbolReference *elementSymRef = comp()->getSymRefTab()->findOrCreateArrayShadowSymbolRef(TR::Address, arrayRefNode);

      if (isLoad)
         {
         const TR::ILOpCodes loadOp = comp()->il.opCodeForIndirectArrayLoad(TR::Address);

         TR::Node *elementLoadNode = TR::Node::recreateWithoutProperties(callNode, loadOp, 1, elementAddressNode, elementSymRef);

         if (comp()->useCompressedPointers())
            {
            TR::Node *compressNode = TR::Node::createCompressedRefsAnchor(elementLoadNode);
            callTree->insertBefore(TR::TreeTop::create(comp(), compressNode));
            }
         }
      else
         {
         TR::Node *oldAnchorNode = callTree->getNode();

         TR_ASSERT_FATAL_WITH_NODE(oldAnchorNode, (oldAnchorNode->getNumChildren() == 1) && oldAnchorNode->getFirstChild() == callNode, "Expected call node n%un for jitStoreFlattenableArrayElement was anchored under node n%un\n", callNode->getGlobalIndex(), oldAnchorNode->getGlobalIndex());

         TR::Node *elementStoreNode = TR::Node::recreateWithoutProperties(callNode, TR::awrtbari, 3, elementAddressNode,
                                                   valueNode, arrayRefNode, elementSymRef);

         if (needsStoreCheck || needsStoreAndNullCheck)
            {
            TR::ResolvedMethodSymbol *methodSym = comp()->getMethodSymbol();
            TR::SymbolReference *storeCheckSymRef = comp()->getSymRefTab()->findOrCreateTypeCheckArrayStoreSymbolRef(methodSym);
            TR::Node *storeCheckNode = TR::Node::createWithRoomForThree(TR::ArrayStoreCHK, elementStoreNode, 0, storeCheckSymRef);
            storeCheckNode->setByteCodeInfo(elementStoreNode->getByteCodeInfo());
            callTree->setNode(storeCheckNode);

            if (needsStoreAndNullCheck)
               {
               TR::SymbolReference *nonNullableArrayNullStoreCheckSymRef = comp()->getSymRefTab()->findOrCreateNonNullableArrayNullStoreCheckSymbolRef();
               TR::Node *nullCheckNode = TR::Node::createWithSymRef(TR::call, 2, 2, valueNode, arrayRefNode, nonNullableArrayNullStoreCheckSymRef);
               nullCheckNode->setByteCodeInfo(elementStoreNode->getByteCodeInfo());
               callTree->insertBefore(TR::TreeTop::create(comp(), TR::Node::create(TR::treetop, 1,  nullCheckNode)));
               }

            // This might be the first time the various checking symbol references are used
            // Need to ensure aliasing for them is correctly constructed
            //
            optimizer()->setAliasSetsAreValid(false);
            }
         else
            {
            callTree->setNode(TR::Node::create(TR::treetop, 1, elementStoreNode));
            }

         // The old anchor node is no longer needed.  Remove what was previously a child
         // call node from it.
         oldAnchorNode->removeAllChildren();
         if (comp()->useCompressedPointers())
            {
            TR::Node *compressNode = TR::Node::createCompressedRefsAnchor(elementStoreNode);
            callTree->insertAfter(TR::TreeTop::create(comp(), compressNode));
            }
         }

      // The indexNode, arrayRefNode and valueNode (if any), were referenced by the
      // original callNode.  Now that the call node has been recreated with either
      // an aloadi, awrtbari or ArrayStoreCHK, we need to decrement their references.
      if (valueNode != NULL)
         {
         valueNode->recursivelyDecReferenceCount();
         }

      indexNode->recursivelyDecReferenceCount();
      arrayRefNode->recursivelyDecReferenceCount();
      }

   _helperCallsToBeFolded.deleteAll();
   }

