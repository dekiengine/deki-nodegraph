#include "deki-nodegraph/NodeFactory.h"

namespace DekiNodeGraph {
namespace SceneFormat {

NodeFactory& NodeFactory::Instance() {
    static NodeFactory instance;
    return instance;
}

}  // namespace SceneFormat
}  // namespace DekiNodeGraph
