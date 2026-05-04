# Proposal: Query-Time `path_agent_radius` for 3D Navigation

> **Target repo**: `godotengine/godot-proposals`
> **Status**: design draft — not yet prototyped

## Describe the project you are working on

A 3D RTS-style game where unit sizes range from `r ≈ 0.15 m` (infantry)
up to `r ≈ 1.2 m` (tanks, heroes). All units share one terrain and,
ideally, one navmesh.

## Describe the problem or limitation

`NavigationMesh.agent_radius` is a **bake-time** parameter: it is baked
into the geometry via Recast's erosion. `NavigationServer3D.query_path()`
has no notion of agent size. Consequences:

1. One baked navmesh = one footprint. Heterogeneous unit sizes force
   you to bake and ship multiple navmeshes.
2. Returned paths run through polygon vertices, so an agent of the
   matching size still clips walls at convex corners.
3. `NavigationAgent3D` / RVO is local steering — it can deflect around
   obstacles but it does not reroute. Large agents get stuck trying to
   force through gates that were never wide enough.

Note this is separate from `NavigationAgent3D.radius`, which controls
avoidance/steering. This proposal is about the **pathfinding** radius.

## Describe the feature / enhancement

Add a per-query parameter `path_agent_radius` on
`NavigationPathQueryParameters3D`. A single navmesh baked with a small
radius (e.g. 0.05 m) can then be queried with any per-call radius.

The server would:

- **Reject A\* traversals** through portals or polygon interiors that
  can't fit a disk of diameter `2r`, so large agents actually detour.
- **Offset path corners** so the returned polyline keeps clearance `r`
  from the navmesh boundary.

Default `0.0` means "same as today", so the change is additive and
backwards compatible.

### Expected behaviour

Hypothetical map: a wall with two gates (0.6 m and 2.0 m); start left,
goal right.

| Agent radius | Current master | With proposal |
|---|---|---|
| `r = 0.05` | through narrow gate | unchanged |
| `r = 0.5`  | through narrow gate, clips walls | detours to wide gate |
| `r = 1.1`  | through narrow gate, stuck | around the wall's end |

These are target behaviours, not measurements. A PoC is planned.

## Describe how your proposal will work

### 1. Public API

```gdscript
var params := NavigationPathQueryParameters3D.new()
params.path_agent_radius = 0.7   # NEW, default 0.0
NavigationServer3D.query_path(params, result)
```

That is the only new surface. `NavigationMesh.agent_radius` is untouched.

### 2. A\* filter

At sync time, every polygon gets a tiny table of "can a disk of
diameter `d` traverse this polygon from edge `e_i` to edge `e_j`?"
widths. For each neighbour polygon A\* considers, we check two things
(gated behind `r > 0`):

1. **Portal width**: `pathway_start.distance_to(pathway_end) >= 2r`.
   (Uses the shared portion of the shared edge, which can be shorter
   than either polygon's full edge.)
2. **Polygon interior width**: the precomputed table says the current
   polygon can fit diameter `2r` from its entry edge to this exit edge.

The start polygon is always admitted — if the query starts inside a
narrow polygon we still try to return a path, rather than fail.

A\*'s cost/heuristic/funnel stay exactly as today; the table only
gates pass/fail.

#### Computing the table

At sync time we fan-triangulate each polygon from `v_0` into `N-2`
triangles. For each polygon-edge pair `(e_i, e_j)` the estimate is the
min of a per-triangle choke width along the chain of fan triangles
between `e_i` and `e_j`. A natural first-cut per-triangle formula is:

```
For triangle (A, B, C):
    choke(AB) = min(|AB|, dist(C, line AB))
    choke(BC) = min(|BC|, dist(A, line BC))
    choke(CA) = min(|CA|, dist(B, line CA))
```

> Caveats: (1) Demyen's full per-triangle formula splits on whether the
> shared-vertex angle is acute or obtuse; the simplification above is
> not identical to it. (2) Demyen's correctness is proven for
> (constrained) Delaunay triangulations; fan triangulation is a
> different decomposition for `N > 3`. Both simplifications deviate
> from the proven result; §6 measures the sign and magnitude of the
> resulting estimation error and determines whether Demyen's exact
> per-triangle formula and/or CDT are needed.

### 3. Funnel corner offset

Each interior apex of the funnel output sits on a polygon vertex `v`,
with two polygon edges (walls) meeting at `v` with some walkable
interior angle `β`. We shift the apex inward along the **walkable
angle bisector** at `v` by `r / sin(β/2)` — the centre of an
`r`-disk inscribed tangent to both walls at `v`.

This requires the funnel to expose, for each apex, which polygon
vertex it came from; the algorithm already tracks this internally
(apexes are placed on polygon vertices by construction), it just
needs to be surfaced to the offset pass. Start and goal are
user-provided and are left alone.

> Note: using the bisector of the two **path segments** at the apex
> (which is what a naive implementation would reach for) is only
> equivalent to the walkable bisector when the path is tangent to both
> walls at that apex. The funnel does not in general produce
> tangent-to-wall segments — a path can graze a polygon vertex while
> its incoming and outgoing segments point into the interior of the
> walkable region — so the correct offset direction must come from the
> polygon geometry, not from the path segments.

### 4. Expected cost (to be measured)

- **Memory**: at most `3·(N-2)` floats per polygon — ~48 B for `N = 6`.
  A 5 000-polygon map ≈ 240 KB.
- **Sync time**: `O(N)` per polygon; effectively constant.
- **Query time**: no overhead for `r = 0`; for `r > 0`, a few extra
  comparisons per A\* expansion, possibly faster overall because large
  agents prune dead ends.
- **Bake time**: unchanged.

### 5. Out of scope

- No runtime re-erosion of the navmesh.
- No change to `NavigationAgent3D` avoidance / RVO (local steering).
- No special handling of vertical transitions: clearance is computed
  in the plane of each polygon, same as the current funnel.
- No change to `NavigationLink3D` traversal rules.
- Corner offset only applies to `PATH_POSTPROCESSING_CORRIDORFUNNEL`;
  `PATH_POSTPROCESSING_EDGECENTERED` still benefits from A\* filtering
  but is not re-offset.

### 6. Validation plan

- PoC scene: wall with gates of mixed widths, a handful of radius
  presets; visually confirm gate selection and clearance behaviour as
  `r` grows.
- Regression: run existing navigation demos with `r = 0` and verify
  bit-for-bit identical results to master.
- Microbenchmark `query_path()` for `r = 0` vs `r > 0` on a
  multi-thousand-polygon map.
- On generated paths with `r > 0`, sample each returned segment's
  minimum distance to the navmesh boundary and check that it is
  `>= r - ε`. This is the acceptance test for §3's corner-offset
  correctness, including its interaction with adjacent polygons.
- Compare per-polygon traversable widths from the fan-triangulation
  estimate against a CDT reference on synthetic convex polygons
  (spiky interiors, elongated shapes); record the signed error
  distribution to determine whether the estimate is conservative,
  optimistic, or mixed, and by how much.

## If this enhancement will not be used often, can it be worked around with a few lines of script?

No.

- **Multiple baked navmeshes per size bucket**: multiplies memory and
  makes dynamic obstacles hard to keep in sync across variants.
- **Post-process the path in GDScript**: doesn't help — by then A\* has
  already picked a corridor that's too narrow, and pushing points
  outward just moves them into walls. The "which gate to go through"
  decision must happen during A\* expansion, which is C++-only.

## Is there a reason why this should be core and not an add-on in the asset library?

- The change lives inside `NavigationServer3D::query_path()` and the
  A\* / funnel stages. Not reachable from script — an add-on would
  have to reimplement navmesh traversal.
- `NavigationMesh.agent_radius` is already core; having a bake-time
  radius but no query-time radius is an API asymmetry, not a new
  feature category.
- A single `float` defaulting to `0.0` doesn't affect existing projects.

## References

- Demyen, D. *Efficient Triangulation-Based Pathfinding*, M.Sc. thesis,
  University of Alberta, 2006, §4.2.
- Mononen, M. *Simple Stupid Funnel Algorithm*, 2010 — the funnel
  variant Godot currently uses.
