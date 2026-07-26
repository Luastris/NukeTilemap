#pragma once
#ifndef NUKE_TILEMAP_H
#define NUKE_TILEMAP_H
// NukeTilemap — the grid-world module (colony sims / roguelikes / strategy, Phase 6.4).
//
// A SEPARATE module by design: most projects never need a tilemap, so it ships only with
// the games that do. A native game module links NukeTilemap.lib and includes this header
// for direct C++ access (the hot path — mapgen fills, A* reads); scripts reach the same
// component through reflection (Lua `atom:getComponent("Tilemap")`, C# `GetComponent<Tilemap>()`).
//
// Model:
//  - ONE component owns the whole map as FLAT arrays — a cell is an index, never an object.
//  - N named LAYERS (terrain/floor/things/zones...), each: uint16 tile id (0 = empty),
//    uint8 visual variant, uint8 free user byte (plant growth, wall HP bucket, ...).
//  - Tile DEFS live in a `.nutile` JSON asset (texture atlas + per-tile cells/walk/flags);
//    a cell stores only the id — walkability/cost are functions of the def stack.
//  - The map lies in the atom's LOCAL XY plane (like Canvas): rotate the atom to lay it
//    flat on the ground (X+90) or keep it vertical for a 2D side view. `cellSize` world
//    units per cell; cell (0,0) is the map's lower-left corner at the atom's position.
//  - RENDERING: 32×32-cell chunks baked into sprite-batch vertex runs (one draw per layer
//    per atlas), frustum-culled per chunk, dirty-rebaked on SetTile. Zero new PSOs.
//  - SERIALIZATION: layers pack RLE+CBOR+base64 into the hidden `data` prop on save
//    (Component::OnBeforeSave) and decode on load — savegames carry the live map.

#include <API/Model/Component.h>
#include <API/Model/Vector.h>
#include <reflect/Reflect.h>
#include <string>
#include <vector>
#include <cstdint>

#ifdef NUKETILEMAP_EXPORTS
#define NUKETILEMAP_API __declspec(dllexport)
#else
#define NUKETILEMAP_API __declspec(dllimport)
#endif

namespace nuke {

class Texture;
class iRender;

// One tile definition from the .nutile asset. `walk`: 0 = impassable, 100 = normal,
// bigger = slower (RimWorld-style path cost). `flags` = bitmask over TileFlagMask names.
// Visuals come from EITHER `cells` (uniform cols×rows grid indices) or `rects` (explicit
// atlas PIXEL rects — free-form packed atlases, e.g. imported libgdx TexturePacker pages).
// When `rects` is non-empty it wins; each entry is one visual variant.
struct NUKETILEMAP_API TileDef
{
	// One free-form atlas region. `rot` = stored rotated 90° clockwise in the page
	// (TexturePacker "rotate: true"); the bake un-rotates it via the UVs.
	struct Rect { int x = 0, y = 0, w = 0, h = 0; bool rot = false; };

	uint16_t              id = 0;
	std::string           name;
	std::vector<int>      cells;      // atlas cell indices (several = visual variants)
	std::vector<Rect>     rects;      // explicit pixel rects (override cells when present)
	uint16_t              walk = 100;
	uint32_t              flags = 0;
};

// The parsed .nutile asset: texture atlas + grid + defs. Loaded once per path (cached in
// the module); tilemaps share it. `version` bumps on InvalidateTileSet (editor hot-reload)
// so live maps know to rebake.
struct NUKETILEMAP_API TileSet
{
	std::string            path;       // content-relative source (.nutile)
	std::string            texPath;    // content-relative texture the defs draw from
	Texture*               tex = nullptr;
	// Optional NORMAL map ("normal" in the .nutile — imported atlases come in diffuse+normal
	// pairs): present -> the set's tiles draw through the LIT sprite pipeline (Lambert from
	// the scene lights). "normalDx": true = DirectX green convention (default = OpenGL, flip).
	std::string            normalPath;
	Texture*               normalTex = nullptr;
	bool                   normalFlipY = true;
	int                    cols = 1, rows = 1;   // atlas grid
	std::vector<TileDef>   defs;                 // indexed lookup below
	int                    version = 0;
	const TileDef* Find(uint16_t id) const;
};

// Re-parse a loaded tile set IN PLACE (the .nutile editor calls this after saving): the
// cached TileSet object keeps its address, its version bumps, and every live Tilemap
// referencing it rebakes chunks + costs on its next frame. No-op if the path isn't loaded.
NUKETILEMAP_API void InvalidateTileSet(const std::string& contentRel);

// Dynamic flag registry: "wall"/"buildable"/"mine"/... -> a stable bit for this session.
// Game code compares masks; the .nutile parser registers names on sight (max 32).
NUKETILEMAP_API uint32_t TileFlagMask(const std::string& name);

class NUKETILEMAP_API Tilemap : public Component
{
	NUKE_CLASS(Tilemap, Component, "World")
public:
	static const int kChunk = 32;   // cells per chunk side (render bake granularity)

	static const int kSetShift   = 12;     // global tile id = (setIndex << 12) | localId
	static const int kMaxSets    = 16;     // 16 sets × 4095 local ids per map
	static const int kMaxLocalId = 4095;

	[[nuke::prop(label="Width",  min=1, tip="Map width in cells (use Setup to change at runtime)")]]  int width  = 64;
	[[nuke::prop(label="Height", min=1, tip="Map height in cells (use Setup to change at runtime)")]] int height = 64;
	[[nuke::prop(label="Cell Size", tip="World units per cell")]] double cellSize = 1.0;
	// MULTIPLE tile sets, mixed freely in any layer: a cell's global id = (setIndex<<12)|localId
	// (set 0's globals EQUAL its local ids, so single-set maps/code keep working unchanged).
	// ORDER defines setIndex — reordering renumbers ids on the map, so keep it stable.
	[[nuke::prop(asset="file:.nutile", label="Tile Sets", tip="The map's .nutile defs assets. A cell's global tile id = (set index << 12) | tile id — build it with TileId(set, id). Order defines the set index: KEEP IT STABLE (reordering renumbers existing cells).")]] std::vector<std::string> tilesets;
	// Legacy single-set prop (pre-multiset maps): merged in as set 0 when `tilesets` is empty.
	[[nuke::prop(hidden)]] std::string tileset;
	// The packed map (layers as RLE+CBOR+base64). Written by OnBeforeSave, decoded on load.
	[[nuke::prop(hidden)]] std::string data;

	struct Layer
	{
		std::string           name;
		std::vector<uint16_t> id;        // width*height, 0 = empty
		std::vector<uint8_t>  variant;   // visual variant (mapgen rolls it)
		std::vector<uint8_t>  user;      // free per-cell byte for the game
	};

	Tilemap();

	// --- lifecycle (Component) ---
	void Init(Atom* parent) override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
	void Destroy() override;
	void OnRender(iRender* r, RenderPhase phase) override;   // chunked draw (Opaque phase)
	void OnBeforeSave() override;                            // layers -> `data` prop

	// --- reflected API (scripts get it via the component handle; C++ calls it directly) ---
	// (Re)create the map: drops all layers/cells. Mapgen calls this first.
	[[nuke::func]] void   Setup(int w, int h, double cellSizeWorld);
	[[nuke::func]] int    AddLayer(const std::string& name);       // -> layer index
	[[nuke::func]] int    LayerIndex(const std::string& name);     // -1 if absent
	[[nuke::func]] int    LayerCount();
	[[nuke::func]] void   SetTile(int layer, int x, int y, int id);
	[[nuke::func]] void   SetTileV(int layer, int x, int y, int id, int variant);
	[[nuke::func]] int    Tile(int layer, int x, int y);            // 0 = empty / out of bounds
	[[nuke::func]] int    Variant(int layer, int x, int y);
	[[nuke::func]] void   SetUser(int layer, int x, int y, int v);  // the free per-cell byte
	[[nuke::func]] int    User(int layer, int x, int y);
	[[nuke::func]] void   Fill(int layer, int id);                  // whole layer
	// Multi-set helpers: compose a GLOBAL tile id from (set index, local id in that set's
	// .nutile); by-name looks the tile up across all sets (first match wins). -1 = not found.
	[[nuke::func]] int    TileId(int setIndex, int localId);
	[[nuke::func]] int    TileIdByName(const std::string& name);
	[[nuke::func]] int    SetCount();
	// Walkability/cost over the LAYER STACK: blocked if any layer's tile has walk 0; else
	// the max walk value (empty cells contribute nothing; all-empty = 100).
	[[nuke::func]] bool   Walkable(int x, int y);
	[[nuke::func]] double MoveCost(int x, int y);                   // 0 = blocked
	[[nuke::func]] bool   HasFlag(int x, int y, const std::string& flag);   // any layer's tile
	// Grid <-> world (the atom's local XY plane; cell centres).
	[[nuke::func]] Vector3 CellToWorld(int x, int y);
	[[nuke::func]] int     WorldToCellX(const Vector3& world);
	[[nuke::func]] int     WorldToCellY(const Vector3& world);
	// Ray -> cell (6.7): intersect the map plane with a world ray (Camera.ScreenRayOrigin/Dir
	// pair) and return the hit cell. PickCell answers "did it hit the map at all"; the X/Y
	// getters return the LAST PickCell hit (-1 when it missed) — one intersection, two reads.
	[[nuke::func]] bool PickCell(const Vector3& origin, const Vector3& dir);
	[[nuke::func]] int  PickedX();
	[[nuke::func]] int  PickedY();

	// --- pathfinding (6.5): async A* over the cost grid, solved on the nuke::Jobs pool ---
	// FindPath SNAPSHOTS the cost grid at request time (thread-free solve; the result
	// reflects the map as of the request — re-path when the world changes underneath you).
	// 8-way with a no-corner-cut rule (a diagonal step needs BOTH orthogonal neighbours
	// walkable), octile heuristic, collinear waypoints compressed. Poll PathReady, read the
	// waypoints, then PathRelease the id (results are held until released).
	[[nuke::func]] double FindPath(int x0, int y0, int x1, int y1);   // -> request id (0 = rejected)
	[[nuke::func]] bool   PathReady(double id);    // solved (found or not) — results readable
	[[nuke::func]] bool   PathFound(double id);    // a route exists
	[[nuke::func]] int    PathLength(double id);   // waypoint count (incl. start + goal)
	[[nuke::func]] int    PathX(double id, int i);
	[[nuke::func]] int    PathY(double id, int i);
	[[nuke::func]] double PathCost(double id);     // total move cost along the route
	[[nuke::func]] void   PathRelease(double id);

	// Native synchronous solve (game modules on their own worker/budget): fills `out` with
	// (x, y) waypoints. Same rules as FindPath, no registry round-trip.
	bool PathFindSync(int x0, int y0, int x1, int y1,
	                  std::vector<std::pair<int, int>>& out, double* totalCost = nullptr);

	// --- native bulk (game modules; no reflection on hot paths) ---
	void            SetTiles(int layer, const uint16_t* ids, size_t count);   // row-major fill
	Layer*          GetLayer(int index);
	const uint16_t* CostGrid();          // width*height, lazily rebuilt (0 = blocked) — A* reads this
	const TileSet*  Defs();              // set 0 (legacy accessor; null until resolved)
	const TileSet*  SetAt(int i);        // resolved set by index (null if absent)
	const TileDef*  DefFor(uint16_t globalId);   // def behind a global id (null = empty/unknown)

private:
	std::vector<Layer>    layers;
	std::vector<uint16_t> cost;          // combined move-cost cache
	bool                  costDirty = true;

	// Render bake: per (chunk, layer, set) vertex runs in the sprite batch layout
	// (9 floats/vert) — index = layerIndex * setCount + setIndex (one run per texture).
	struct ChunkBake { std::vector<std::vector<float>> layerVerts; bool dirty = true; };
	std::vector<ChunkBake> chunks;
	int  chunksX = 0, chunksY = 0;
	double bakedPos[3] = { 0, 0, 0 };    // transform snapshot: a moved/rotated map rebakes
	double bakedQuat[4] = { 0, 0, 0, 1 };

	std::vector<const TileSet*> sets;    // resolved from `tilesets` (lazy; re-resolves on change)
	std::vector<std::string>    setsLoadedFrom;
	std::vector<int>            bakedSetVersions;  // rebake everything when a set hot-reloads
	int            texRetryCounter = 0;  // throttle the unresolved-texture re-lookup (not every frame)

	bool decoded = false;                // `data` prop decoded into layers (lazy after load)
	int  pickedX = -1, pickedY = -1;     // last PickCell hit (reflected getters)

	void EnsureDecoded();
	void EnsureSets();
	void EnsureChunks();
	void MarkCell(int x, int y);         // dirty the chunk + cost for a cell
	void BakeChunk(int cx, int cy);
	void RebuildCost();
	bool InBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
};

}  // namespace nuke

#endif // !NUKE_TILEMAP_H
