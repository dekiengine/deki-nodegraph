#include "deki-nodegraph/NodeFactory.h"

namespace DekiNodeGraph
{
namespace SceneFormat
{

NodeFactory& NodeFactory::Instance()
{
    static NodeFactory s_Instance;
    return s_Instance;
}

}  // namespace SceneFormat
}  // namespace DekiNodeGraph
