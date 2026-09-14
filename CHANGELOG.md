# Changelog

Notable changes to `deki-nodegraph`. Engine and editor changes are in the
[engine changelog](https://github.com/dekiengine/deki-engine/blob/master/CHANGELOG.md).

A package's `minEngine` names the engine version it needs. Before 1.0 a
breaking change bumps the minor across the editor, the engine and every
package together, so a package with no changes of its own is still released
alongside one that has them.

## 0.15.0

### Added
- Unit tests for the graph queries an interpreter walks (`FindNode`,
  `FindFirstOfType`, `Next`) including dangling links, self-links and several
  links on one output pin, and for the compiled-blob loader refusing truncated
  and malformed input rather than reading past the end of it.
