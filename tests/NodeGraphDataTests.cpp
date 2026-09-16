// The graph queries an interpreter walks, and the loader's refusal of junk.
//
// Graph::FindNode / FindFirstOfType / Next are the whole traversal API: an
// interpreter finds its entry node by type, then follows Next() from pin to
// pin. Their edge cases are the ones a malformed or hand-edited graph
// produces -- a link to a node that is not there, two links off one output
// pin, a self-link -- and each of those has a defined answer that a rewrite
// must not change.
//
// LoadFromMemory parses a compiled blob on the device, where the input is
// whatever was flashed. It must return nullptr rather than reach into a
// truncated buffer, and it must not leak the graph it had half-built.

#include <gtest/gtest.h>

#include <NodeGraphData.h>

#include <cstdint>
#include <vector>

// The package's types moved into its namespace; tests name them unqualified.
using namespace DekiNodeGraph;

namespace
{

using Graph = NodeGraphData::Graph;
using Node = NodeGraphData::NodeInstance;
using Link = NodeGraphData::Link;

Node MakeNode(uint32_t id, uint32_t typeId)
{
    Node n;
    n.id = id;
    n.typeId = typeId;
    return n;
}

// A -> B -> C, plus an unconnected D of the same type as B.
Graph Chain()
{
    Graph g;
    g.nodes = {MakeNode(1, 100), MakeNode(2, 200), MakeNode(3, 300), MakeNode(4, 200)};
    g.links = {Link{1, 0, 2, 0}, Link{2, 0, 3, 0}};
    return g;
}

}  // namespace

TEST(NodeGraphQueries, FindNodeReturnsTheNodeWithThatId)
{
    const Graph g = Chain();
    const Node* n = g.FindNode(2);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->id, 2u);
    EXPECT_EQ(n->typeId, 200u);
}

TEST(NodeGraphQueries, FindNodeReturnsNullForAnIdThatIsNotThere)
{
    const Graph g = Chain();
    EXPECT_EQ(g.FindNode(99), nullptr);
    EXPECT_EQ(g.FindNode(0), nullptr);
}

TEST(NodeGraphQueries, FindFirstOfTypeReturnsTheEarliestMatch)
{
    // Two nodes share type 200; an interpreter looking for its entry node
    // takes the first, so document order is the tie-break and is load-bearing.
    const Graph g = Chain();
    const Node* n = g.FindFirstOfType(200);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->id, 2u) << "later node won; document order is not being honoured";
}

TEST(NodeGraphQueries, FindFirstOfTypeReturnsNullForAnAbsentType)
{
    const Graph g = Chain();
    EXPECT_EQ(g.FindFirstOfType(999), nullptr);
}

TEST(NodeGraphQueries, NextFollowsTheLinkLeavingAPin)
{
    const Graph g = Chain();
    const Node* b = g.Next(1, 0);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, 2u);

    const Node* c = g.Next(2, 0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->id, 3u);
}

TEST(NodeGraphQueries, NextReturnsNullAtTheEndOfAChain)
{
    const Graph g = Chain();
    EXPECT_EQ(g.Next(3, 0), nullptr) << "the last node has no outgoing link";
}

TEST(NodeGraphQueries, NextDistinguishesPins)
{
    // Pin number is half the key. A node with two outputs must not have one
    // pin answer for the other, which is what makes a branch node work.
    Graph g;
    g.nodes = {MakeNode(1, 100), MakeNode(2, 200), MakeNode(3, 300)};
    g.links = {Link{1, 0, 2, 0}, Link{1, 1, 3, 0}};

    ASSERT_NE(g.Next(1, 0), nullptr);
    EXPECT_EQ(g.Next(1, 0)->id, 2u);
    ASSERT_NE(g.Next(1, 1), nullptr);
    EXPECT_EQ(g.Next(1, 1)->id, 3u);
    EXPECT_EQ(g.Next(1, 2), nullptr) << "a pin with no link must not fall through to another";
}

TEST(NodeGraphQueries, NextTakesTheFirstOfSeveralLinksOnOnePin)
{
    // The editor does not produce this, but a hand-edited or migrated graph
    // can. Documented behaviour is first-wins, and it has to stay decidable
    // rather than becoming "whichever the container happens to yield".
    Graph g;
    g.nodes = {MakeNode(1, 100), MakeNode(2, 200), MakeNode(3, 300)};
    g.links = {Link{1, 0, 2, 0}, Link{1, 0, 3, 0}};

    const Node* n = g.Next(1, 0);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->id, 2u);
}

TEST(NodeGraphQueries, NextReturnsNullWhenTheLinkPointsAtAMissingNode)
{
    // A dangling link is the shape a partial delete leaves behind. Next() has
    // to answer null rather than hand back a pointer into nothing.
    Graph g;
    g.nodes = {MakeNode(1, 100)};
    g.links = {Link{1, 0, 42, 0}};
    EXPECT_EQ(g.Next(1, 0), nullptr);
}

TEST(NodeGraphQueries, ASelfLinkResolvesToTheNodeItself)
{
    // Not useful, but it must terminate rather than recurse: Next() is one
    // step, so a caller's own loop guard is what stops a cycle.
    Graph g;
    g.nodes = {MakeNode(1, 100)};
    g.links = {Link{1, 0, 1, 0}};
    const Node* n = g.Next(1, 0);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->id, 1u);
}

TEST(NodeGraphQueries, AnEmptyGraphAnswersNullToEverything)
{
    const Graph g;
    EXPECT_EQ(g.FindNode(1), nullptr);
    EXPECT_EQ(g.FindFirstOfType(1), nullptr);
    EXPECT_EQ(g.Next(1, 0), nullptr);
}

TEST(NodeGraphLoad, RefusesAnEmptyBuffer)
{
    EXPECT_EQ(NodeGraphData::LoadFromMemory(nullptr, 0), nullptr);

    const uint8_t byte = 0x80;
    EXPECT_EQ(NodeGraphData::LoadFromMemory(&byte, 0), nullptr);
}

TEST(NodeGraphLoad, RefusesABlobWhoseRootIsNotAMap)
{
    // 0x90 is a MessagePack fixarray of length 0. The root has to be a map.
    const uint8_t notAMap[] = {0x90};
    EXPECT_EQ(NodeGraphData::LoadFromMemory(notAMap, sizeof(notAMap)), nullptr);

    const uint8_t anInteger[] = {0x2a};  // positive fixint 42
    EXPECT_EQ(NodeGraphData::LoadFromMemory(anInteger, sizeof(anInteger)), nullptr);
}

TEST(NodeGraphLoad, RefusesATruncatedBlobWithoutReadingPastTheEnd)
{
    // A fixmap claiming one pair, with nothing after it. Under ASan or on a
    // device this is the case that reads off the end if the parser trusts the
    // declared size.
    const uint8_t truncated[] = {0x81};
    EXPECT_EQ(NodeGraphData::LoadFromMemory(truncated, sizeof(truncated)), nullptr);

    // A map claiming 15 pairs with one truncated key.
    const uint8_t truncatedKey[] = {0x8f, 0xa5, 'n', 'o', 'd'};
    EXPECT_EQ(NodeGraphData::LoadFromMemory(truncatedKey, sizeof(truncatedKey)), nullptr);
}

TEST(NodeGraphLoad, RefusesRandomBytesOfEveryLength)
{
    // Not a fuzzer, but enough to catch a parser that dereferences before it
    // bounds-checks: every prefix of a byte pattern with no valid structure.
    const uint8_t junk[] = {0xde, 0xad, 0xbe, 0xef, 0xff, 0x00, 0x81, 0xc1,
                            0xdd, 0xff, 0xff, 0xff, 0xff, 0xa0, 0x7f, 0xcb};
    for (size_t n = 1; n <= sizeof(junk); ++n)
        EXPECT_EQ(NodeGraphData::LoadFromMemory(junk, n), nullptr) << "accepted junk of length " << n;
}
