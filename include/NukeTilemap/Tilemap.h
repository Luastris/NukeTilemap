#pragma once
#ifndef NUKE_TILEMAP_H
#define NUKE_TILEMAP_H
// NukeTilemap — grid-world module: one component owns the whole map as flat per-layer
// arrays. The map lies in the atom's LOCAL XY plane; cell (0,0) is its lower-left corner.

#include <API/Model/Component.h>
#include <API/Model/Vector.h>
#include <reflect/Reflect.h>
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
  #ifdef NUKETILEMAP_EXPORTS
  #define NUKETILEMAP_API __declspec(dllexport)
  #else
  #define NUKETILEMAP_API __declspec(dllimport)
  #endif
#else
  #define NUKETILEMAP_API __attribute__((visibility("default")))
#endif

namespace nuke {

class Texture;
class iRender;

// One tile definition from the .nutile asset. `walk`: 0 = impassable, 100 = normal,
// bigger = slower. Visuals come from `cells` (grid indices) or `rects` (pixel rects, win).
struct NUKETILEMAP_API TileDef
{
	// One free-form atlas region. `rot` = stored rotated 90° CW in the page (un-rotated via UVs).
	struct Rect { int x = 0, y = 0, w = 0, h = 0; bool rot = false; };

	uint16_t              id = 0;
	std::string           name;
	std::vector<int>      cells;      // atlas cell indices (several = visual variants)
	std::vector<Rect>     rects;      // explicit pixel rects (override cells when present)
	uint16_t              walk = 100;
	uint32_t              flags = 0;
};

// The parsed .nutile asset: texture atlas + grid + defs. Cached per path and shared;
// `version` bumps on InvalidateTileSet so live maps rebake.
struct NUKETILEMAP_API TileSet
{
	std::string            path;       // content-relative source (.nutile)
	std::string            texPath;    // content-relative texture the defs draw from
	Texture*               tex = nullptr;
	// Optional normal map: present -> tiles draw through the LIT sprite pipeline.
	std::string            normalPath;
	Texture*               normalTex = nullptr;
	bool                   normalFlipY = true;
	int                    cols = 1, rows = 1;   // atlas grid
	std::vector<TileDef>   defs;                 // indexed lookup below
	int                    version = 0;
	const TileDef* Find(uint16_t id) const;
};

// Re-parse a loaded tile set IN PLACE: the cached object keeps its address, version bumps,
// live tilemaps rebake next frame. No-op if the path isn't loaded.
NUKETILEMAP_API void InvalidateTileSet(const std::string& contentRel);

// Flag name -> stable bit for this session (max 32). Registered by the .nutile parser.
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
	// ORDER defines setIndex — reordering renumbers ids on the map, so keep it stable.
	[[nuke::prop(asset="file:.nutile", label="Tile Sets", tip="The map's .nutile defs assets. A cell's global tile id = (set index << 12) | tile id — build it with TileId(set, id). Order defines the set index: KEEP IT STABLE (reordering renumbers existing cells).")]] std::vector<std::string> tilesets;
	[[nuke::prop(hidden)]] std::string tileset;   // legacy single-set prop, merged in as set 0
	[[nuke::prop(hidden)]] std::string data;      // packed layers (RLE+CBOR+base64)

	struct Layer
	{
		std::string           name;
		std::vector<uint16_t> id;        // width*height, 0 = empty
		std::vector<uint8_t>  variant;   // visual variant (mapgen rolls it)
		std::vector<uint8_t>  user;      // free per-cell byte for the game
	};

	Tilemap();

	void Init(Atom* parent) override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
	void Destroy() override;
	void OnRender(iRender* r, RenderPhase phase) override;   // chunked draw (Opaque phase)
	void OnBeforeSave() override;                            // layers -> `data` prop

	// (Re)create the map: drops all layers/cells.
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
	// Compose a GLOBAL tile id from (set index, local id); by-name searches all sets. -1 = none.
	[[nuke::func]] int    TileId(int setIndex, int localId);
	[[nuke::func]] int    TileIdByName(const std::string& name);
	[[nuke::func]] int    SetCount();
	// Over the LAYER STACK: blocked if any layer's tile has walk 0, else the max walk value.
	[[nuke::func]] bool   Walkable(int x, int y);
	[[nuke::func]] double MoveCost(int x, int y);                   // 0 = blocked
	[[nuke::func]] bool   HasFlag(int x, int y, const std::string& flag);   // any layer's tile
	// Grid <-> world (the atom's local XY plane; cell centres).
	[[nuke::func]] Vector3 CellToWorld(int x, int y);
	[[nuke::func]] int     WorldToCellX(const Vector3& world);
	[[nuke::func]] int     WorldToCellY(const Vector3& world);
	// Intersect the map plane with a world ray; PickedX/Y read back the LAST hit (-1 = miss).
	[[nuke::func]] bool PickCell(const Vector3& origin, const Vector3& dir);
	[[nuke::func]] int  PickedX();
	[[nuke::func]] int  PickedY();

	// Async 8-way A* on the nuke::Jobs pool; SNAPSHOTS the cost grid at request time.
	// Poll PathReady, read the waypoints, then PathRelease the id (results are held until then).
	[[nuke::func]] double FindPath(int x0, int y0, int x1, int y1);   // -> request id (0 = rejected)
	[[nuke::func]] bool   PathReady(double id);    // solved (found or not) — results readable
	[[nuke::func]] bool   PathFound(double id);    // a route exists
	[[nuke::func]] int    PathLength(double id);   // waypoint count (incl. start + goal)
	[[nuke::func]] int    PathX(double id, int i);
	[[nuke::func]] int    PathY(double id, int i);
	[[nuke::func]] double PathCost(double id);     // total move cost along the route
	[[nuke::func]] void   PathRelease(double id);

	// Synchronous solve on the calling thread: fills `out` with (x, y) waypoints.
	bool PathFindSync(int x0, int y0, int x1, int y1,
	                  std::vector<std::pair<int, int>>& out, double* totalCost = nullptr);

	// Native bulk access (no reflection on hot paths).
	void            SetTiles(int layer, const uint16_t* ids, size_t count);   // row-major fill
	Layer*          GetLayer(int index);
	const uint16_t* CostGrid();          // width*height, lazily rebuilt (0 = blocked)
	const TileSet*  Defs();              // set 0 (legacy accessor; null until resolved)
	const TileSet*  SetAt(int i);        // resolved set by index (null if absent)
	const TileDef*  DefFor(uint16_t globalId);   // def behind a global id (null = empty/unknown)

private:
	std::vector<Layer>    layers;
	std::vector<uint16_t> cost;          // combined move-cost cache
	bool                  costDirty = true;

	// Render bake: sprite-batch vertex runs (9 floats/vert), index = layerIndex*setCount + setIndex.
	struct ChunkBake { std::vector<std::vector<float>> layerVerts; bool dirty = true; };
	std::vector<ChunkBake> chunks;
	int  chunksX = 0, chunksY = 0;
	double bakedPos[3] = { 0, 0, 0 };    // transform snapshot: a moved/rotated map rebakes
	double bakedQuat[4] = { 0, 0, 0, 1 };

	std::vector<const TileSet*> sets;    // resolved from `tilesets` (lazy)
	std::vector<std::string>    setsLoadedFrom;
	std::vector<int>            bakedSetVersions;  // rebake everything when a set hot-reloads
	int            texRetryCounter = 0;  // throttles the unresolved-texture re-lookup

	bool decoded = false;                // `data` prop decoded into layers (lazy after load)
	int  pickedX = -1, pickedY = -1;     // last PickCell hit

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
