// Tilemap implementation — data, defs, chunked render bake, serialization. See the header
// for the model. Everything here is engine-API only (no renderer/backend types beyond the
// iRender seam), so the module stays renderer-agnostic.
#include <NukeTilemap/Tilemap.h>

#include <interface/AppInstance.h>
#include <API/Model/Atom.h>
#include <API/Model/Texture.h>
#include <API/Model/resdb.h>
#include <API/Model/Transform.h>
#include <render/irender.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cmath>
#include <map>
#include <iostream>

using json = nlohmann::json;

namespace nuke {

// ---- flags ---------------------------------------------------------------------------------

uint32_t TileFlagMask(const std::string& name)
{
	static std::map<std::string, uint32_t> reg;
	static uint32_t next = 1;
	if (name.empty()) return 0;
	auto it = reg.find(name);
	if (it != reg.end()) return it->second;
	if (next == 0) { std::cout << "[NukeTilemap]\tflag overflow (32 max): " << name << std::endl; return 0; }
	uint32_t m = next; next <<= 1;
	reg[name] = m;
	return m;
}

// ---- tileset cache -------------------------------------------------------------------------

const TileDef* TileSet::Find(uint16_t id) const
{
	for (const TileDef& d : defs) if (d.id == id) return &d;
	return nullptr;
}

// .nutile files parse once per path; every Tilemap referencing the same set shares it.
// The cache lives for the session (defs are tiny; textures are ResDB's problem). Entries
// re-parse IN PLACE on InvalidateTileSet (std::map nodes are address-stable).
static std::map<std::string, TileSet> g_tilesets;

// Parse the file into `ts` (keeps path/version; resets everything else).
static bool ParseTileSetInto(TileSet& ts, const std::string& rel)
{
	std::string text;
	if (!AppInstance::GetSingleton()->ReadContent(rel, text))
	{
		std::cout << "[NukeTilemap]\ttileset not found: " << rel << std::endl;
		return false;
	}
	json j = json::parse(text, nullptr, false, true);
	if (j.is_discarded() || !j.is_object())
	{
		std::cout << "[NukeTilemap]\ttileset parse error: " << rel << std::endl;
		return false;
	}
	ts.path    = rel;
	ts.tex     = nullptr;   // re-resolves lazily (the texture path may have changed)
	ts.normalTex = nullptr;
	ts.defs.clear();
	ts.texPath    = j.value("texture", std::string());
	ts.normalPath = j.value("normal", std::string());
	ts.normalFlipY = !j.value("normalDx", false);   // default = OpenGL green convention
	ts.cols    = std::max(1, j.value("cols", 1));
	ts.rows    = std::max(1, j.value("rows", 1));
	if (j.contains("tiles") && j["tiles"].is_array())
		for (const json& t : j["tiles"])
		{
			TileDef d;
			d.id   = (uint16_t)t.value("id", 0);
			d.name = t.value("name", std::string());
			d.walk = (uint16_t)t.value("walk", 100);
			if (t.contains("cells") && t["cells"].is_array())
				for (const json& c : t["cells"]) d.cells.push_back(c.get<int>());
			// An EXPLICIT empty "cells": [] = an INVISIBLE cost-only def (occupancy blockers:
			// walk/flags apply, nothing bakes). Absent key keeps the cell-0 default.
			if (d.cells.empty() && !t.contains("cells")) d.cells.push_back(0);
			// Free-form atlas rects: [[x,y,w,h], [x,y,w,h,1], ...] — a 5th element = rotated 90° CW.
			if (t.contains("rects") && t["rects"].is_array())
				for (const json& rj : t["rects"])
					if (rj.is_array() && rj.size() >= 4)
					{
						TileDef::Rect rr;
						rr.x = rj[0].get<int>(); rr.y = rj[1].get<int>();
						rr.w = rj[2].get<int>(); rr.h = rj[3].get<int>();
						rr.rot = rj.size() >= 5 && rj[4].get<int>() != 0;
						if (rr.w > 0 && rr.h > 0) d.rects.push_back(rr);
					}
			if (t.contains("flags") && t["flags"].is_array())
				for (const json& f : t["flags"]) d.flags |= TileFlagMask(f.get<std::string>());
			if (d.id != 0) ts.defs.push_back(d);   // 0 is reserved: "empty"
		}
	std::cout << "[NukeTilemap]\ttileset '" << rel << "': " << ts.defs.size() << " defs, atlas "
	          << ts.cols << "x" << ts.rows << " from '" << ts.texPath << "'" << std::endl;
	return true;
}

static const TileSet* LoadTileSet(const std::string& rel)
{
	auto it = g_tilesets.find(rel);
	if (it != g_tilesets.end()) return &it->second;
	TileSet& ts = g_tilesets[rel];
	if (!ParseTileSetInto(ts, rel)) return nullptr;
	return &ts;
}

void InvalidateTileSet(const std::string& contentRel)
{
	auto it = g_tilesets.find(contentRel);
	if (it == g_tilesets.end()) return;   // never loaded: nothing references it yet
	ParseTileSetInto(it->second, contentRel);
	++it->second.version;                 // live tilemaps rebake on their next frame
}

// ---- base64 (the data prop must be valid JSON text) ------------------------------------------

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string B64Encode(const std::vector<uint8_t>& in)
{
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	for (size_t i = 0; i < in.size(); i += 3)
	{
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < in.size()) v |= (uint32_t)in[i + 1] << 8;
		if (i + 2 < in.size()) v |= (uint32_t)in[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += (i + 1 < in.size()) ? kB64[(v >> 6) & 63] : '=';
		out += (i + 2 < in.size()) ? kB64[v & 63] : '=';
	}
	return out;
}

static std::vector<uint8_t> B64Decode(const std::string& in)
{
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::vector<uint8_t> out;
	out.reserve(in.size() / 4 * 3);
	uint32_t acc = 0; int bits = 0;
	for (char c : in)
	{
		int v = val(c);
		if (v < 0) continue;   // '=' padding / whitespace
		acc = (acc << 6) | (uint32_t)v; bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((acc >> bits) & 0xFF)); }
	}
	return out;
}

// ---- RLE over uint16 ids (maps are extremely uniform — grass compresses to nothing) ----------

static void RleEncode16(const std::vector<uint16_t>& in, std::vector<uint8_t>& out)
{
	size_t i = 0;
	while (i < in.size())
	{
		uint16_t v = in[i];
		size_t run = 1;
		while (i + run < in.size() && in[i + run] == v && run < 0xFFFF) ++run;
		out.push_back((uint8_t)(run & 0xFF)); out.push_back((uint8_t)(run >> 8));
		out.push_back((uint8_t)(v & 0xFF));   out.push_back((uint8_t)(v >> 8));
		i += run;
	}
}

static bool RleDecode16(const uint8_t* p, size_t n, std::vector<uint16_t>& out, size_t expect)
{
	out.clear(); out.reserve(expect);
	for (size_t i = 0; i + 4 <= n; i += 4)
	{
		size_t   run = (size_t)p[i] | ((size_t)p[i + 1] << 8);
		uint16_t v   = (uint16_t)p[i + 2] | ((uint16_t)p[i + 3] << 8);
		for (size_t k = 0; k < run && out.size() < expect; ++k) out.push_back(v);
	}
	return out.size() == expect;
}

// ---- component -----------------------------------------------------------------------------

Tilemap::Tilemap() : Component("Tilemap") {}

void Tilemap::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	decoded = false;   // props (incl. `data`) are already loaded when Init runs — decode lazily
}

void Tilemap::Destroy() {}

void Tilemap::Setup(int w, int h, double cellSizeWorld)
{
	width = std::max(1, w); height = std::max(1, h);
	if (cellSizeWorld > 0.0) cellSize = cellSizeWorld;
	layers.clear();
	cost.clear(); costDirty = true;
	chunks.clear(); chunksX = chunksY = 0;
	decoded = true;   // a fresh map IS the live state now
}

int Tilemap::AddLayer(const std::string& name)
{
	EnsureDecoded();
	for (size_t i = 0; i < layers.size(); ++i)
		if (layers[i].name == name) return (int)i;
	Layer l; l.name = name;
	l.id.assign((size_t)width * height, 0);
	l.variant.assign((size_t)width * height, 0);
	l.user.assign((size_t)width * height, 0);
	layers.push_back(std::move(l));
	chunks.clear();   // per-chunk layer runs grew — rebuild the bake containers
	return (int)layers.size() - 1;
}

int Tilemap::LayerIndex(const std::string& name)
{
	EnsureDecoded();
	for (size_t i = 0; i < layers.size(); ++i)
		if (layers[i].name == name) return (int)i;
	return -1;
}

int Tilemap::LayerCount() { EnsureDecoded(); return (int)layers.size(); }

void Tilemap::SetTile(int layer, int x, int y, int id) { SetTileV(layer, x, y, id, 0); }

void Tilemap::SetTileV(int layer, int x, int y, int id, int variant)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !InBounds(x, y)) return;
	const size_t i = (size_t)y * width + x;
	layers[layer].id[i]      = (uint16_t)id;
	layers[layer].variant[i] = (uint8_t)variant;
	MarkCell(x, y);
}

int Tilemap::Tile(int layer, int x, int y)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !InBounds(x, y)) return 0;
	return layers[layer].id[(size_t)y * width + x];
}

int Tilemap::Variant(int layer, int x, int y)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !InBounds(x, y)) return 0;
	return layers[layer].variant[(size_t)y * width + x];
}

void Tilemap::SetUser(int layer, int x, int y, int v)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !InBounds(x, y)) return;
	layers[layer].user[(size_t)y * width + x] = (uint8_t)v;
}

int Tilemap::User(int layer, int x, int y)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !InBounds(x, y)) return 0;
	return layers[layer].user[(size_t)y * width + x];
}

void Tilemap::Fill(int layer, int id)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size()) return;
	std::fill(layers[layer].id.begin(), layers[layer].id.end(), (uint16_t)id);
	std::fill(layers[layer].variant.begin(), layers[layer].variant.end(), (uint8_t)0);
	costDirty = true;
	for (ChunkBake& c : chunks) c.dirty = true;
}

void Tilemap::SetTiles(int layer, const uint16_t* ids, size_t count)
{
	EnsureDecoded();
	if (layer < 0 || layer >= (int)layers.size() || !ids) return;
	const size_t n = std::min(count, layers[layer].id.size());
	std::memcpy(layers[layer].id.data(), ids, n * sizeof(uint16_t));
	costDirty = true;
	for (ChunkBake& c : chunks) c.dirty = true;
}

Tilemap::Layer* Tilemap::GetLayer(int index)
{
	EnsureDecoded();
	return (index >= 0 && index < (int)layers.size()) ? &layers[index] : nullptr;
}

const TileSet* Tilemap::Defs() { EnsureSets(); return sets.empty() ? nullptr : sets[0]; }

// Combined move cost: blocked if ANY layer's tile is walk 0; else the max walk over the
// stack (empty cells contribute nothing; a fully empty cell = 100, plain ground).
void Tilemap::RebuildCost()
{
	EnsureSets();
	cost.assign((size_t)width * height, 100);
	if (sets.empty()) { costDirty = false; return; }
	for (const Layer& l : layers)
		for (size_t i = 0; i < l.id.size() && i < cost.size(); ++i)
		{
			if (!l.id[i] || cost[i] == 0) continue;
			const TileDef* d = DefFor(l.id[i]);
			if (!d) continue;
			if (d->walk == 0)          cost[i] = 0;
			else if (d->walk > cost[i]) cost[i] = d->walk;
		}
	costDirty = false;
}

const uint16_t* Tilemap::CostGrid()
{
	EnsureDecoded();
	if (costDirty || cost.size() != (size_t)width * height) RebuildCost();
	return cost.data();
}

bool Tilemap::Walkable(int x, int y)
{
	if (!InBounds(x, y)) return false;
	return CostGrid()[(size_t)y * width + x] != 0;
}

double Tilemap::MoveCost(int x, int y)
{
	if (!InBounds(x, y)) return 0;
	return CostGrid()[(size_t)y * width + x];
}

bool Tilemap::HasFlag(int x, int y, const std::string& flag)
{
	EnsureDecoded(); EnsureSets();
	if (sets.empty() || !InBounds(x, y)) return false;
	const uint32_t m = TileFlagMask(flag);
	const size_t i = (size_t)y * width + x;
	for (const Layer& l : layers)
		if (l.id[i])
			if (const TileDef* d = DefFor(l.id[i]))
				if (d->flags & m) return true;
	return false;
}

// Grid <-> world over the atom's LOCAL XY plane: cell (0,0)'s lower-left corner sits at the
// atom's position; +x cells go along the atom's right, +y along its up.
Vector3 Tilemap::CellToWorld(int x, int y)
{
	if (!transform) return Vector3(0, 0, 0);
	Vector3 p = transform->globalPosition(), R = transform->right(), U = transform->up();
	const double cx = (x + 0.5) * cellSize, cy = (y + 0.5) * cellSize;
	return Vector3(p.x + R.x * cx + U.x * cy,
	               p.y + R.y * cx + U.y * cy,
	               p.z + R.z * cx + U.z * cy);
}

int Tilemap::WorldToCellX(const Vector3& world)
{
	if (!transform || cellSize <= 0.0) return -1;
	Vector3 p = transform->globalPosition(), R = transform->right();
	const double d = (world.x - p.x) * R.x + (world.y - p.y) * R.y + (world.z - p.z) * R.z;
	return (int)std::floor(d / cellSize);
}

int Tilemap::WorldToCellY(const Vector3& world)
{
	if (!transform || cellSize <= 0.0) return -1;
	Vector3 p = transform->globalPosition(), U = transform->up();
	const double d = (world.x - p.x) * U.x + (world.y - p.y) * U.y + (world.z - p.z) * U.z;
	return (int)std::floor(d / cellSize);
}

// Ray -> cell (6.7): intersect the map's plane (atom local XY: normal = right × up), then
// project the hit into cell coords. Hits outside the grid or behind the origin = miss.
bool Tilemap::PickCell(const Vector3& origin, const Vector3& dir)
{
	pickedX = pickedY = -1;
	if (!transform || cellSize <= 0.0) return false;
	Vector3 p = transform->globalPosition(), R = transform->right(), U = transform->up();
	const double nx = R.y * U.z - R.z * U.y;
	const double ny = R.z * U.x - R.x * U.z;
	const double nz = R.x * U.y - R.y * U.x;
	const double denom = nx * dir.x + ny * dir.y + nz * dir.z;
	if (std::abs(denom) < 1e-9) return false;   // ray parallel to the map plane
	const double t = ((p.x - origin.x) * nx + (p.y - origin.y) * ny + (p.z - origin.z) * nz) / denom;
	if (t <= 0.0) return false;                 // plane behind the ray
	const Vector3 hit(origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t);
	const int cx = WorldToCellX(hit), cy = WorldToCellY(hit);
	if (!InBounds(cx, cy)) return false;
	pickedX = cx; pickedY = cy;
	return true;
}

int Tilemap::PickedX() { return pickedX; }
int Tilemap::PickedY() { return pickedY; }

// ---- serialization (the hidden `data` prop) ---------------------------------------------------

void Tilemap::OnBeforeSave()
{
	if (!decoded) return;   // never touched since load — the prop already holds the state
	json root;
	root["ver"] = 1;
	root["w"] = width; root["h"] = height;
	json jl = json::array();
	for (const Layer& l : layers)
	{
		std::vector<uint8_t> rle;
		RleEncode16(l.id, rle);
		json e;
		e["name"]    = l.name;
		e["id"]      = json::binary(rle);
		e["variant"] = json::binary(std::vector<uint8_t>(l.variant.begin(), l.variant.end()));
		e["user"]    = json::binary(std::vector<uint8_t>(l.user.begin(), l.user.end()));
		jl.push_back(e);
	}
	root["layers"] = jl;
	data = B64Encode(json::to_cbor(root));
}

void Tilemap::EnsureDecoded()
{
	if (decoded) return;
	decoded = true;
	layers.clear(); chunks.clear(); chunksX = chunksY = 0; costDirty = true;
	if (data.empty()) return;
	json root = json::from_cbor(B64Decode(data), true, false);
	if (root.is_discarded() || !root.is_object())
	{
		std::cout << "[NukeTilemap]\tmap data decode FAILED (corrupt blob) — empty map" << std::endl;
		return;
	}
	// The blob's dimensions win (props may have been edited independently in the file).
	width  = root.value("w", width);
	height = root.value("h", height);
	const size_t n = (size_t)width * height;
	if (root.contains("layers") && root["layers"].is_array())
		for (const json& e : root["layers"])
		{
			Layer l;
			l.name = e.value("name", std::string());
			if (e.contains("id") && e["id"].is_binary())
			{
				const auto& b = e["id"].get_binary();
				if (!RleDecode16(b.data(), b.size(), l.id, n))
					std::cout << "[NukeTilemap]\tlayer '" << l.name << "' id RLE mismatch" << std::endl;
			}
			l.id.resize(n, 0);
			auto bytes = [&](const char* key, std::vector<uint8_t>& out)
			{
				out.assign(n, 0);
				if (e.contains(key) && e[key].is_binary())
				{
					const auto& b = e[key].get_binary();
					std::memcpy(out.data(), b.data(), std::min(b.size(), n));
				}
			};
			bytes("variant", l.variant);
			bytes("user",    l.user);
			layers.push_back(std::move(l));
		}
}

// ---- rendering -------------------------------------------------------------------------------

void Tilemap::EnsureSets()
{
	// Effective path list: `tilesets`; a pre-multiset map carries only the legacy `tileset`
	// prop — merged in as set 0 so old maps and old game code keep working unchanged.
	std::vector<std::string> want = tilesets;
	if (want.empty() && !tileset.empty()) want.push_back(tileset);
	if ((int)want.size() > kMaxSets)
	{
		static bool warned = false;
		if (!warned) { warned = true; std::cout << "[NukeTilemap]\tmore than " << kMaxSets
			<< " tile sets on one map - extras IGNORED (global id space is (set<<12)|id)" << std::endl; }
		want.resize(kMaxSets);
	}

	// Texture may resolve late (content registers after the component on cold boot).
	// GuidForContentPath: texPath is content-relative, ResDB keys are scan/import forms.
	// The lookup has a linear normalized fallback -> THROTTLED retry (a permanently missing
	// texture must not cost a registry sweep every frame), and the warning fires ONCE per set.
	auto resolveTex = [this](const std::string& path, bool first)
	{
		auto it = g_tilesets.find(path);
		if (it == g_tilesets.end()) return;
		TileSet* ts = &it->second;
		if (!ts->tex)
		{
			std::string guid = ResDB::getSingleton()->GuidForContentPath(ts->texPath);
			if (!guid.empty()) ts->tex = ResDB::getSingleton()->GetTexture(guid);
			if (!ts->tex && first)
				std::cout << "[NukeTilemap]\ttileset texture UNRESOLVED: '" << ts->texPath
				          << "' (its tiles will not draw until it registers; retrying quietly)" << std::endl;
		}
		if (!ts->normalPath.empty() && !ts->normalTex)
		{
			std::string guid = ResDB::getSingleton()->GuidForContentPath(ts->normalPath);
			if (!guid.empty()) ts->normalTex = ResDB::getSingleton()->GetTexture(guid);
		}
	};

	if (want != setsLoadedFrom)
	{
		sets.clear();
		for (const std::string& p : want) sets.push_back(p.empty() ? nullptr : LoadTileSet(p));
		setsLoadedFrom = want;
		texRetryCounter = 0;
		for (ChunkBake& c : chunks) c.dirty = true;   // a new set list invalidates every bake
		costDirty = true;
	}
	// Throttled late-texture retry for any still-unresolved set (diffuse or normal).
	bool missing = false;
	for (const TileSet* s : sets)
		if (s && (!s->tex || (!s->normalPath.empty() && !s->normalTex))) { missing = true; break; }
	if (missing)
	{
		++texRetryCounter;
		if (texRetryCounter == 1 || (texRetryCounter % 120) == 0)
			for (size_t i = 0; i < sets.size(); ++i)
				if (sets[i] && !sets[i]->tex) resolveTex(setsLoadedFrom[i], texRetryCounter == 1);
	}
}

const TileSet* Tilemap::SetAt(int i)
{
	EnsureSets();
	return (i >= 0 && i < (int)sets.size()) ? sets[i] : nullptr;
}

const TileDef* Tilemap::DefFor(uint16_t globalId)
{
	if (!globalId) return nullptr;
	const int si = globalId >> kSetShift, li = globalId & kMaxLocalId;
	if (si >= (int)sets.size() || !sets[si]) return nullptr;
	return sets[si]->Find((uint16_t)li);
}

int Tilemap::TileId(int setIndex, int localId)
{
	if (setIndex < 0 || setIndex >= kMaxSets || localId < 1 || localId > kMaxLocalId) return -1;
	return (setIndex << kSetShift) | localId;
}

int Tilemap::TileIdByName(const std::string& name)
{
	EnsureSets();
	for (size_t si = 0; si < sets.size(); ++si)
		if (sets[si])
			for (const TileDef& d : sets[si]->defs)
				if (d.name == name) return ((int)si << kSetShift) | d.id;
	return -1;
}

int Tilemap::SetCount()
{
	EnsureSets();
	return (int)sets.size();
}

void Tilemap::EnsureChunks()
{
	const int nx = (width  + kChunk - 1) / kChunk;
	const int ny = (height + kChunk - 1) / kChunk;
	if (nx == chunksX && ny == chunksY && chunks.size() == (size_t)nx * ny) return;
	chunksX = nx; chunksY = ny;
	chunks.assign((size_t)nx * ny, ChunkBake{});
}

void Tilemap::MarkCell(int x, int y)
{
	costDirty = true;
	EnsureChunks();
	const int cx = x / kChunk, cy = y / kChunk;
	if (cx >= 0 && cy >= 0 && cx < chunksX && cy < chunksY)
		chunks[(size_t)cy * chunksX + cx].dirty = true;
}

// Bake one chunk: per layer, a quad per non-empty cell in the sprite batch's exact vertex
// layout (9 floats: pos, uv, tint). World-space verts snapshot the atom's transform — a
// moved/rotated map rebakes everything (maps are static in practice).
void Tilemap::BakeChunk(int cx, int cy)
{
	ChunkBake& ch = chunks[(size_t)cy * chunksX + cx];
	const size_t setCount = sets.size();
	ch.layerVerts.assign(layers.size() * std::max<size_t>(1, setCount), {});
	if (setCount == 0) { ch.dirty = false; return; }

	Vector3 P = transform->globalPosition(), R = transform->right(), U = transform->up();
	const double cs = cellSize;

	const int x0 = cx * kChunk, y0 = cy * kChunk;
	const int x1 = std::min(x0 + kChunk, width), y1 = std::min(y0 + kChunk, height);

	for (size_t li = 0; li < layers.size(); ++li)
	{
		const Layer& l = layers[li];
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
			{
				const size_t i = (size_t)y * width + x;
				const uint16_t id = l.id[i];
				if (!id) continue;
				// Global id -> owning set (its texture is this quad's run) + its local def.
				const size_t si = (size_t)(id >> kSetShift);
				if (si >= setCount) continue;
				const TileSet* set = sets[si];
				if (!set || !set->tex) continue;
				const float du = 1.0f / set->cols, dv = 1.0f / set->rows;
				const TileDef* d = set->Find((uint16_t)(id & kMaxLocalId));
				if (!d) continue;
				if (d->rects.empty() && d->cells.empty()) continue;   // invisible cost-only def
				std::vector<float>& out = ch.layerVerts[li * setCount + si];
				// Per-corner UVs: TL/TR/BR/BL of the tile's VISUAL. Grid cells and free-form
				// rects both reduce to these four pairs; a rotated rect permutes them.
				float uTL, vTL, uTR, vTR, uBR, vBR, uBL, vBL;
				if (!d->rects.empty())
				{
					const TileDef::Rect& rc = d->rects[l.variant[i] % d->rects.size()];
					const float tw = (float)set->tex->width, th = (float)set->tex->height;
					if (tw <= 0 || th <= 0) continue;
					if (!rc.rot)
					{
						const float u0 = rc.x / tw, v0 = rc.y / th;
						const float u1 = (rc.x + rc.w) / tw, v1 = (rc.y + rc.h) / th;
						uTL = u0; vTL = v0; uTR = u1; vTR = v0; uBR = u1; vBR = v1; uBL = u0; vBL = v1;
					}
					else
					{
						// Stored rotated 90° CW: the page rect spans w=rc.h, h=rc.w. Original
						// pixel (px,py) sits at page (x + h - py, y + px) — invert per corner.
						const float pu0 = rc.x / tw, pu1 = (rc.x + rc.h) / tw;
						const float pv0 = rc.y / th, pv1 = (rc.y + rc.w) / th;
						uTL = pu1; vTL = pv0; uTR = pu1; vTR = pv1; uBR = pu0; vBR = pv1; uBL = pu0; vBL = pv0;
					}
				}
				else
				{
					const int cell = d->cells[l.variant[i] % d->cells.size()];
					const int cc = cell % set->cols, cr = cell / set->cols;
					const float u0 = cc * du, v0 = cr * dv, u1 = u0 + du, v1 = v0 + dv;
					uTL = u0; vTL = v0; uTR = u1; vTR = v0; uBR = u1; vBR = v1; uBL = u0; vBL = v1;
				}

				// Cell corners on the atom's local XY plane (world space).
				const double bx = x * cs, by = y * cs;
				auto corner = [&](double ox, double oy, float u, float v)
				{
					out.push_back((float)(P.x + R.x * (bx + ox) + U.x * (by + oy)));
					out.push_back((float)(P.y + R.y * (bx + ox) + U.y * (by + oy)));
					out.push_back((float)(P.z + R.z * (bx + ox) + U.z * (by + oy)));
					out.push_back(u); out.push_back(v);
					out.push_back(1); out.push_back(1); out.push_back(1); out.push_back(1);
				};
				// Two CCW triangles; vTL = top of the atlas region = the cell's TOP edge (+y).
				corner(0,  cs, uTL, vTL); corner(cs, cs, uTR, vTR); corner(cs, 0, uBR, vBR);
				corner(0,  cs, uTL, vTL); corner(cs, 0,  uBR, vBR); corner(0,  0, uBL, vBL);
			}
	}
	ch.dirty = false;
}

void Tilemap::OnRender(iRender* r, RenderPhase phase)
{
	if (phase != RenderPhase::Opaque || !r || !transform) return;
	EnsureDecoded();
	EnsureSets();
	bool anyTex = false;
	for (const TileSet* s : sets) if (s && s->tex) { anyTex = true; break; }
	if (!anyTex || layers.empty()) return;
	EnsureChunks();

	// Transform snapshot: baked verts are world-space, so a moved/rotated map rebakes.
	{
		Vector3 p = transform->globalPosition();
		Quaternion q = transform->globalRotation();
		if (p.x != bakedPos[0] || p.y != bakedPos[1] || p.z != bakedPos[2] ||
		    q.x != bakedQuat[0] || q.y != bakedQuat[1] || q.z != bakedQuat[2] || q.w != bakedQuat[3])
		{
			bakedPos[0] = p.x; bakedPos[1] = p.y; bakedPos[2] = p.z;
			bakedQuat[0] = q.x; bakedQuat[1] = q.y; bakedQuat[2] = q.z; bakedQuat[3] = q.w;
			for (ChunkBake& c : chunks) c.dirty = true;
		}
	}
	// Tile-set hot-reload (the .nutile editor saved): new defs/atlas -> rebake + recost.
	{
		bool bumped = bakedSetVersions.size() != sets.size();
		if (!bumped)
			for (size_t i = 0; i < sets.size(); ++i)
				if (sets[i] && sets[i]->version != bakedSetVersions[i]) { bumped = true; break; }
		if (bumped)
		{
			bakedSetVersions.resize(sets.size());
			for (size_t i = 0; i < sets.size(); ++i) bakedSetVersions[i] = sets[i] ? sets[i]->version : -1;
			costDirty = true;
			for (ChunkBake& c : chunks) c.dirty = true;
		}
	}

	// Frustum cull per chunk: the chunk's 4 plane corners against the camera's clip volume
	// (same clip-space test World's FrustumCull uses; the plane has no thickness).
	float view[16], proj[16];
	r->getViewProj(view, proj);
	Vector3 P = transform->globalPosition(), R = transform->right(), U = transform->up();
	auto visible = [&](int cx, int cy) -> bool
	{
		const double xA = cx * kChunk * cellSize, yA = cy * kChunk * cellSize;
		const double xB = std::min((cx + 1) * kChunk, width) * cellSize;
		const double yB = std::min((cy + 1) * kChunk, height) * cellSize;
		bool anyIn = false;
		int outL = 0, outR = 0, outB = 0, outT = 0, outN = 0, outF = 0;
		const double xs[2] = { xA, xB }, ys[2] = { yA, yB };
		for (int a = 0; a < 2; ++a)
			for (int b = 0; b < 2; ++b)
			{
				const double wx = P.x + R.x * xs[a] + U.x * ys[b];
				const double wy = P.y + R.y * xs[a] + U.y * ys[b];
				const double wz = P.z + R.z * xs[a] + U.z * ys[b];
				// row-major v*M like the engine's cull: clip = world * view * proj
				float vx = (float)(wx * view[0] + wy * view[4] + wz * view[8]  + view[12]);
				float vy = (float)(wx * view[1] + wy * view[5] + wz * view[9]  + view[13]);
				float vz = (float)(wx * view[2] + wy * view[6] + wz * view[10] + view[14]);
				float cxn = vx * proj[0] + vy * proj[4] + vz * proj[8]  + proj[12];
				float cyn = vx * proj[1] + vy * proj[5] + vz * proj[9]  + proj[13];
				float czn = vx * proj[2] + vy * proj[6] + vz * proj[10] + proj[14];
				float cw  = vx * proj[3] + vy * proj[7] + vz * proj[11] + proj[15];
				if (cxn < -cw) ++outL; else if (cxn > cw) ++outR;
				if (cyn < -cw) ++outB; else if (cyn > cw) ++outT;
				if (czn < 0)   ++outN; else if (czn > cw) ++outF;
				if (cxn >= -cw && cxn <= cw && cyn >= -cw && cyn <= cw && czn >= 0 && czn <= cw) anyIn = true;
			}
		if (anyIn) return true;
		// all 4 corners outside ONE plane -> definitely invisible; else conservative-visible
		return !(outL == 4 || outR == 4 || outB == 4 || outT == 4 || outN == 4 || outF == 4);
	};

	// Layer sweeps preserve paint order (terrain under floor under things) across chunks;
	// inside a layer, one run per SET texture (consecutive same-texture runs = one draw).
	const size_t setCount = sets.size();
	int visChunks = 0, sentVerts = 0;
	for (size_t li = 0; li < layers.size(); ++li)
		for (size_t si = 0; si < setCount; ++si)
		{
			const TileSet* set = sets[si];
			if (!set || !set->tex) continue;
			for (int cy = 0; cy < chunksY; ++cy)
				for (int cx = 0; cx < chunksX; ++cx)
				{
					if (!visible(cx, cy)) continue;
					if (si == 0) ++visChunks;
					ChunkBake& ch = chunks[(size_t)cy * chunksX + cx];
					if (ch.dirty) BakeChunk(cx, cy);
					const size_t bi = li * setCount + si;
					if (bi < ch.layerVerts.size() && !ch.layerVerts[bi].empty())
					{
						const int n = (int)(ch.layerVerts[bi].size() / 9);
						if (set->normalTex)   // normal map present -> lit pipeline (Lambert)
							r->drawSpriteRunLit(set->tex, set->normalTex, ch.layerVerts[bi].data(), n,
							                    set->normalFlipY);
						else
							r->drawSpriteRun(set->tex, ch.layerVerts[bi].data(), n);
						sentVerts += n;
					}
				}
		}
	// Draw-path diagnostics (NUKE_TM_DIAG=1): what the cull kept and what reached the batch.
	static const bool diag = []{ const char* e = std::getenv("NUKE_TM_DIAG"); return e && *e == '1'; }();
	if (diag)
	{
		static int frame = 0;
		if ((frame++ % 60) == 0)
			std::cout << "[NukeTilemap]\tDIAG chunks " << visChunks << "/" << (chunksX * chunksY)
			          << " visible, verts " << sentVerts << ", sets " << setCount << std::endl;
	}
}

}  // namespace nuke
