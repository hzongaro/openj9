#if !defined(J9_FORCEHELPERTRANSFORM)
#define J9_FORCEHELPERTRANSFORM

#include "il/Node.hpp"
#include "infra/List.hpp"
#include "optimizer/Optimization.hpp"
#include "optimizer/OptimizationManager.hpp"

class ForceHelperTransform : public TR::Optimization
{
public:
   ForceHelperTransform(TR::OptimizationManager *manager);
   int32_t perform();

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
