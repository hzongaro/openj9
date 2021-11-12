#include "compile/Compilation.hpp"
#include "optimizer/ForceHelperTransform.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"

#define OPT_DETAILS "O^O Force Helper Transform:  "

ForceHelperTransform::ForceHelperTransform(TR::OptimizationManager *manager)
   : TR::Optimization(manager),
     _helperCallsToBeFolded(trMemory())
   {
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
   TR::NodeChecklist visited(comp());

   for (TR::TreeTop *tt = comp()->getStartTree(); tt; tt = tt->getNextTreeTop())
      {
      _curTree = tt;
      visitNode(tt->getNode(), visited);
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
