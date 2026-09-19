# Changelog

Notable changes to `deki-nodegraph`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## Unreleased

### Added
- **Graphs can be authored from the command line** (`--tool` / `--script` /
  MCP), through the same document model and validation as the Node Graph
  window: `graph_add_node` (optionally choosing the node's id and its seeded
  entry's id, so a script can wire what it has just added), `graph_connect`
  (pins by index or label, a state's transitions included), `graph_set_values`,
  `graph_add_child` (e.g. a variable), `graph_get`. Enum fields take their
  names. The canvas rules the window's menus enforce are enforced here.
- `graph_view`: point the open Node Graph window at a canvas and select a
  node - for a screenshot, or to show someone where to look.

## 0.16.0

### Changed
- **Moved into the `DekiNodeGraph` namespace.** Every component was declared at global
  scope, which made its identity a bare class name — the name a scene file
  stores and the name the registry keys on — so two packages defining one name
  collided there with nothing to tell them apart. Each component carries
  `DEKI_FORMER_NAME` with the name it was saved under before, so existing
  scenes load unchanged and are written back qualified on the next save.
  Code naming these types needs the namespace: `using namespace DekiNodeGraph;` or a
  qualified name.
- Enum properties are stored by name rather than by number, so appending to an
  enum or reordering one no longer changes what a saved scene means. Files
  written before this still read.
- `minEngine` 0.16.0. Reflection ABI 17: the package must be rebuilt.

## 0.15.0

### Added
- Unit tests for the graph queries an interpreter walks (`FindNode`,
  `FindFirstOfType`, `Next`) including dangling links, self-links and several
  links on one output pin, and for the compiled-blob loader refusing truncated
  and malformed input rather than reading past the end of it.
