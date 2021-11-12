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
#if !defined(J9_FORCEHELPERTRANSFORM)
#define J9_FORCEHELPERTRANSFORM

#include "il/Node.hpp"
#include "infra/List.hpp"
#include "optimizer/Optimization.hpp"
#include "optimizer/Optimization_inlines.hpp"
#include "optimizer/OptimizationManager.hpp"

class ForceHelperTransform : public TR::Optimization
{
public:
   ForceHelperTransform(TR::OptimizationManager *manager);
   virtual int32_t perform();
   virtual const char * optDetailString() const throw();
   virtual void doDelayedTransformations();

   static TR::Optimization *create(TR::OptimizationManager *manager)
      {
      return new (manager->allocator()) ForceHelperTransform(manager);
      }

protected:
   void visitNode(TR::Node *node, TR::NodeChecklist &visited);
   void postProcess(TR::TreeTop *tree, TR::Node *callNode, flags8_t flags);

   struct ValueTypesHelperCallTransform {
      TR_ALLOC(TR_Memory::ValuePropagation)
      TR::TreeTop *_tree;
      TR::Node *_callNode;
      flags8_t _flags;
      bool _isLoad;
      bool _requiresStoreCheck;
      ValueTypesHelperCallTransform(TR::TreeTop *tree, TR::Node *callNode, flags8_t flags)
         : _tree(tree), _callNode(callNode), _flags(flags) {} // _isLoad(isLoad), _requiresStoreCheck(requiresStoreCheck) {}
      enum // flag bits
         {
         IsArrayLoad        = 0x01,
         IsArrayStore       = 0x02,
         RequiresStoreCheck = 0x04,
         IsRefCompare       = 0x08,
         InsertDebugCounter = 0x10,
         RequiresBoundCheck = 0x20,
         RequiresStoreAndNullCheck = 0x40,
         Unused1            = 0x80,
         };
   };

   TR::TreeTop *_curTree;
   List<ValueTypesHelperCallTransform> _helperCallsToBeFolded;
};
#endif
