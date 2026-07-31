// Grid A* for NukeTilemap. Requests snapshot the cost grid on the game thread and solve on
// the nuke::Jobs pool, so a solve never locks against the live map.
#include <NukeTilemap/Tilemap.h>
#include <API/Model/Jobs.h>

#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <vector>
#include <cmath>
#include <cstring>

namespace nuke {

namespace {

struct PathRequest
{
	int w = 0, h = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::vector<uint16_t> grid;              // cost snapshot (0 = blocked)
	std::atomic<bool> done{ false };
	bool   found = false;
	double cost = 0;
	std::vector<std::pair<int, int>> points;
};

boost::mutex g_pathMutex;   // registry only — the solve itself touches just its own request
std::map<long long, std::shared_ptr<PathRequest>> g_paths;
long long g_nextPathId = 1;

// Binary-heap A*. Grid costs are per cell entered (100 = normal), diagonals pay cost × √2.
// Fills r.points with START..GOAL inclusive, collinear runs compressed. False = no route.
bool Solve(PathRequest& r)
{
	const int w = r.w, h = r.h;
	const size_t n = (size_t)w * h;
	auto idx = [&](int x, int y) { return (size_t)y * w + x; };
	auto blocked = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h || r.grid[idx(x, y)] == 0; };
	if (blocked(r.x0, r.y0) || blocked(r.x1, r.y1)) return false;
	if (r.x0 == r.x1 && r.y0 == r.y1) { r.points.push_back({ r.x0, r.y0 }); r.cost = 0; return true; }

	// g/parent/closed share ONE arena: concurrent solves would serialize on the allocator lock.
	const uint32_t kInf = 0xFFFFFFFFu;
	std::unique_ptr<uint8_t[]> arena(new uint8_t[n * (sizeof(uint32_t) + sizeof(int32_t) + 1)]);
	uint32_t* g      = (uint32_t*)arena.get();
	int32_t*  parent = (int32_t*)(arena.get() + n * sizeof(uint32_t));
	uint8_t*  closed = arena.get() + n * (sizeof(uint32_t) + sizeof(int32_t));
	for (size_t i = 0; i < n; ++i) { g[i] = kInf; parent[i] = -1; }
	std::memset(closed, 0, n);
	struct Node { uint32_t f; uint32_t i; };
	struct Less { bool operator()(const Node& a, const Node& b) const { return a.f > b.f; } };
	std::vector<Node> heap;
	heap.reserve(4096);
	auto hpush = [&](Node nd) { heap.push_back(nd); std::push_heap(heap.begin(), heap.end(), Less{}); };
	auto hpop  = [&]() { std::pop_heap(heap.begin(), heap.end(), Less{}); Node nd = heap.back(); heap.pop_back(); return nd; };

	// Octile heuristic scaled to the 100-per-cell base (admissible: 100 = min cell cost).
	auto heur = [&](int x, int y) -> uint32_t
	{
		const int dx = std::abs(x - r.x1), dy = std::abs(y - r.y1);
		const int lo = dx < dy ? dx : dy, hi = dx < dy ? dy : dx;
		return (uint32_t)(lo * 141 + (hi - lo) * 100);
	};

	g[idx(r.x0, r.y0)] = 0;
	hpush({ heur(r.x0, r.y0), (uint32_t)idx(r.x0, r.y0) });

	static const int DX[8] = { 1, -1, 0,  0,  1,  1, -1, -1 };
	static const int DY[8] = { 0,  0, 1, -1,  1, -1,  1, -1 };

	const size_t goal = idx(r.x1, r.y1);
	while (!heap.empty())
	{
		const Node nd = hpop();
		const size_t ci = nd.i;
		if (closed[ci]) continue;
		closed[ci] = 1;
		if (ci == goal) break;
		const int cx = (int)(ci % w), cy = (int)(ci / w);
		for (int d = 0; d < 8; ++d)
		{
			const int nx = cx + DX[d], ny = cy + DY[d];
			if (blocked(nx, ny)) continue;
			if (d >= 4 && (blocked(cx + DX[d], cy) || blocked(cx, cy + DY[d])))
				continue;   // no corner cutting: both orthogonal neighbours must be open
			const size_t ni = idx(nx, ny);
			if (closed[ni]) continue;
			const uint32_t step = (d >= 4) ? (uint32_t)(r.grid[ni] * 141 / 100) : r.grid[ni];
			const uint32_t ng = g[ci] + step;
			if (ng >= g[ni]) continue;
			g[ni] = ng;
			parent[ni] = (int32_t)ci;
			hpush({ ng + heur(nx, ny), (uint32_t)ni });
		}
	}
	if (g[goal] == kInf) return false;

	std::vector<std::pair<int, int>> rev;
	for (int32_t i = (int32_t)goal; i != -1; i = parent[i])
		rev.push_back({ (int)(i % w), (int)(i / w) });
	r.points.reserve(rev.size());
	for (size_t i = rev.size(); i-- > 0; )
	{
		const auto& p = rev[i];
		const size_t k = r.points.size();
		if (k >= 2)
		{
			const auto& a = r.points[k - 2];
			const auto& b = r.points[k - 1];
			if ((b.first - a.first) == (p.first - b.first) && (b.second - a.second) == (p.second - b.second))
			{ r.points.back() = p; continue; }   // same direction: extend the run
		}
		r.points.push_back(p);
	}
	r.cost = g[goal] / 100.0;
	return true;
}

std::shared_ptr<PathRequest> FindReq(double id)
{
	boost::mutex::scoped_lock lock(g_pathMutex);
	auto it = g_paths.find((long long)id);
	return it != g_paths.end() ? it->second : nullptr;
}

}  // namespace

double Tilemap::FindPath(int x0, int y0, int x1, int y1)
{
	const uint16_t* grid = CostGrid();   // game thread: lazy rebuild is safe here
	if (!grid || width <= 0 || height <= 0) return 0;
	if (!InBounds(x0, y0) || !InBounds(x1, y1)) return 0;

	auto req = std::make_shared<PathRequest>();
	req->w = width; req->h = height;
	req->x0 = x0; req->y0 = y0; req->x1 = x1; req->y1 = y1;
	req->grid.assign(grid, grid + (size_t)width * height);   // snapshot: the solve is lock-free

	long long id;
	{
		boost::mutex::scoped_lock lock(g_pathMutex);
		id = g_nextPathId++;
		g_paths[id] = req;
	}
	Jobs::Schedule([req]()
	{
		req->found = Solve(*req);
		req->done.store(true, std::memory_order_release);
	});
	return (double)id;
}

bool Tilemap::PathReady(double id)
{
	auto r = FindReq(id);
	return r && r->done.load(std::memory_order_acquire);
}

bool Tilemap::PathFound(double id)
{
	auto r = FindReq(id);
	return r && r->done.load(std::memory_order_acquire) && r->found;
}

int Tilemap::PathLength(double id)
{
	auto r = FindReq(id);
	return (r && r->done.load(std::memory_order_acquire)) ? (int)r->points.size() : 0;
}

int Tilemap::PathX(double id, int i)
{
	auto r = FindReq(id);
	if (!r || !r->done.load(std::memory_order_acquire) || i < 0 || i >= (int)r->points.size()) return -1;
	return r->points[i].first;
}

int Tilemap::PathY(double id, int i)
{
	auto r = FindReq(id);
	if (!r || !r->done.load(std::memory_order_acquire) || i < 0 || i >= (int)r->points.size()) return -1;
	return r->points[i].second;
}

double Tilemap::PathCost(double id)
{
	auto r = FindReq(id);
	return (r && r->done.load(std::memory_order_acquire)) ? r->cost : 0;
}

void Tilemap::PathRelease(double id)
{
	boost::mutex::scoped_lock lock(g_pathMutex);
	g_paths.erase((long long)id);   // shared_ptr: a still-running solve finishes on its own copy
}

bool Tilemap::PathFindSync(int x0, int y0, int x1, int y1,
                           std::vector<std::pair<int, int>>& out, double* totalCost)
{
	const uint16_t* grid = CostGrid();
	if (!grid || !InBounds(x0, y0) || !InBounds(x1, y1)) return false;
	PathRequest r;
	r.w = width; r.h = height;
	r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
	r.grid.assign(grid, grid + (size_t)width * height);
	const bool ok = Solve(r);
	out = std::move(r.points);
	if (totalCost) *totalCost = r.cost;
	return ok;
}

}  // namespace nuke
