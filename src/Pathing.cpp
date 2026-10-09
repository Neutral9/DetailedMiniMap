#include "Pathing.h"

#include <condition_variable>
#include <queue>
#include <thread>

namespace Pathing
{
    namespace
    {
        // one navmesh as the game has it, copied (main thread)
        struct Raw
        {
            RE::FormID                           id = 0;
            std::vector<RE::NiPoint3>            vertices;
            std::vector<RE::BSNavmeshTriangle>   triangles;
            std::vector<RE::BSNavmeshEdgeExtraInfo> links;
        };
        using Raws = std::vector<Raw>;

        // every loaded navmesh as one graph of triangles (the worker builds it from the copies)
        struct Graph
        {
            struct Tri
            {
                std::uint32_t v[3];
                std::int32_t  n[3];  // the triangle across edge k (vertex k to k + 1), -1: none
                RE::NiPoint3  c;     // its centre
            };
            std::vector<RE::NiPoint3>                              verts;
            std::vector<Tri>                                       tris;
            std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid;  // triangles by their centre, kGrid squares

            static constexpr float kGrid = 512.0f;
            static std::int64_t    Key(std::int32_t a_x, std::int32_t a_y) { return (static_cast<std::int64_t>(a_x) << 32) ^ static_cast<std::uint32_t>(a_y); }
        };

        Graph Build(const Raws& a_raws)
        {
            using Flag = RE::BSNavmeshTriangle::TriangleFlag;
            Graph                                                  g;
            struct Base
            {
                std::uint32_t vertex, triangle, count;
            };
            std::unordered_map<RE::FormID, Base>                   bases;  // navmesh -> its first vertex, first triangle, triangles
            std::uint32_t                                          vb = 0, tb = 0;
            for (const auto& m : a_raws) {
                bases[m.id] = { vb, tb, static_cast<std::uint32_t>(m.triangles.size()) };
                vb += static_cast<std::uint32_t>(m.vertices.size());
                tb += static_cast<std::uint32_t>(m.triangles.size());
            }
            g.verts.reserve(vb);
            g.tris.reserve(tb);
            for (const auto& m : a_raws) {
                const auto vbase = bases[m.id].vertex, tbase = bases[m.id].triangle;
                g.verts.insert(g.verts.end(), m.vertices.begin(), m.vertices.end());
                const auto count = m.triangles.size();
                for (const auto& t : m.triangles) {
                    Graph::Tri tri{};
                    bool       ok = !t.triangleFlags.all(Flag::kDeleted);
                    for (int k = 0; k < 3; ++k) {
                        ok = ok && t.vertices[k] < m.vertices.size();
                        tri.v[k] = vbase + (t.vertices[k] < m.vertices.size() ? t.vertices[k] : 0);
                        tri.n[k] = -1;
                    }
                    for (int k = 0; ok && k < 3; ++k) {
                        const auto across = t.triangles[k];
                        if (t.triangleFlags.all(static_cast<Flag>(1 << k))) {
                            // into another navmesh (the next cell's): through its link
                            if (across < m.links.size() && m.links[across].type.get() == RE::EDGE_EXTRA_INFO_TYPE::kPortal) {
                                const auto& portal = m.links[across].portal;
                                if (const auto other = bases.find(portal.otherMeshID); other != bases.end() && portal.triangle < other->second.count) {
                                    tri.n[k] = static_cast<std::int32_t>(other->second.triangle + portal.triangle);
                                }
                            }
                        } else if (across != 0xFFFF && across < count) {
                            tri.n[k] = static_cast<std::int32_t>(tbase + across);
                        }
                    }
                    const auto& a = g.verts[tri.v[0]];
                    const auto& b = g.verts[tri.v[1]];
                    const auto& c = g.verts[tri.v[2]];
                    tri.c = { (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
                    if (!ok) {
                        tri.n[0] = tri.n[1] = tri.n[2] = -1;
                        tri.c = { FLT_MAX, FLT_MAX, FLT_MAX };  // never found, never entered
                    }
                    g.tris.push_back(tri);
                }
            }
            for (std::uint32_t i = 0; i < g.tris.size(); ++i) {
                const auto& c = g.tris[i].c;
                if (c.x != FLT_MAX) {
                    g.grid[Graph::Key(static_cast<std::int32_t>(std::floor(c.x / Graph::kGrid)), static_cast<std::int32_t>(std::floor(c.y / Graph::kGrid)))].push_back(i);
                }
            }
            return g;
        }

        float Area2(const RE::NiPoint3& a, const RE::NiPoint3& b, const RE::NiPoint3& c)  // twice the signed area (x, y)
        {
            return (c.x - a.x) * (b.y - a.y) - (b.x - a.x) * (c.y - a.y);
        }

        bool Inside(const Graph& a_g, const Graph::Tri& a_t, const RE::NiPoint3& a_p)
        {
            const auto& a = a_g.verts[a_t.v[0]];
            const auto& b = a_g.verts[a_t.v[1]];
            const auto& c = a_g.verts[a_t.v[2]];
            const float d0 = Area2(a, b, a_p), d1 = Area2(b, c, a_p), d2 = Area2(c, a, a_p);
            return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
        }

        // the triangle a point stands on (the nearest in height of those under it), else the nearest by its centre
        // the triangle nearest a point among every loaded one (far off the navmesh - deep water, a cliff: the way
        // starts from the nearest ground NPCs walk, however far)
        std::int32_t Nearest(const Graph& a_g, const RE::NiPoint3& a_p)
        {
            std::int32_t best = -1;
            float        bestD = FLT_MAX;
            for (std::size_t i = 0; i < a_g.tris.size(); ++i) {
                const auto& c = a_g.tris[i].c;
                const float  d = (c.x - a_p.x) * (c.x - a_p.x) + (c.y - a_p.y) * (c.y - a_p.y) + (c.z - a_p.z) * (c.z - a_p.z);
                if (d < bestD) {
                    bestD = d;
                    best = static_cast<std::int32_t>(i);
                }
            }
            return best;
        }

        std::int32_t Find(const Graph& a_g, const RE::NiPoint3& a_p, float a_reach)
        {
            const auto   gx = static_cast<std::int32_t>(std::floor(a_p.x / Graph::kGrid)), gy = static_cast<std::int32_t>(std::floor(a_p.y / Graph::kGrid));
            const int    ring = static_cast<int>(std::ceil(a_reach / Graph::kGrid));
            std::int32_t on = -1, closest = -1;
            float        onDz = 400.0f, closestD = a_reach * a_reach;
            for (int dx = -ring; dx <= ring; ++dx) {
                for (int dy = -ring; dy <= ring; ++dy) {
                    const auto it = a_g.grid.find(Graph::Key(gx + dx, gy + dy));
                    if (it == a_g.grid.end()) {
                        continue;
                    }
                    for (const auto i : it->second) {
                        const auto& t = a_g.tris[i];
                        const float dz = std::abs(t.c.z - a_p.z);
                        if (std::abs(dx) <= 1 && std::abs(dy) <= 1 && dz < onDz && Inside(a_g, t, a_p)) {
                            on = static_cast<std::int32_t>(i);
                            onDz = dz;
                        }
                        const float d = (t.c.x - a_p.x) * (t.c.x - a_p.x) + (t.c.y - a_p.y) * (t.c.y - a_p.y) + dz * dz;
                        if (d < closestD) {
                            closestD = d;
                            closest = static_cast<std::int32_t>(i);
                        }
                    }
                }
            }
            return on >= 0 ? on : closest;
        }

        // A* over the triangles; the goal not reached (another island, outside the loaded part): to the triangle
        // that came nearest to it
        std::vector<std::int32_t> Search(const Graph& a_g, std::int32_t a_start, std::int32_t a_goal, const RE::NiPoint3& a_to)
        {
            const auto         n = a_g.tris.size();
            std::vector<float> cost(n, FLT_MAX);
            std::vector<std::int32_t> from(n, -1);
            std::vector<std::uint8_t> done(n, 0);
            using Entry = std::pair<float, std::int32_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
            const auto h = [&](std::int32_t a_t) { return a_g.tris[a_t].c.GetDistance(a_to); };
            cost[a_start] = 0.0f;
            open.emplace(h(a_start), a_start);
            std::int32_t best = a_start;
            float        bestH = h(a_start);
            int          expanded = 0;
            while (!open.empty() && expanded < 150000) {
                const auto t = open.top().second;
                open.pop();
                if (done[t]) {
                    continue;
                }
                done[t] = 1;
                ++expanded;
                if (const float d = h(t); d < bestH) {
                    bestH = d;
                    best = t;
                }
                if (t == a_goal) {
                    best = t;
                    break;
                }
                for (const auto next : a_g.tris[t].n) {
                    if (next < 0 || done[next] || a_g.tris[next].c.x == FLT_MAX) {
                        continue;
                    }
                    const float c = cost[t] + a_g.tris[t].c.GetDistance(a_g.tris[next].c);
                    if (c < cost[next]) {
                        cost[next] = c;
                        from[next] = t;
                        open.emplace(c + h(next), next);
                    }
                }
            }
            std::vector<std::int32_t> out;
            for (auto t = best; t >= 0; t = from[t]) {
                out.push_back(t);
            }
            std::ranges::reverse(out);
            return out;
        }

        // the corridor pulled tight: the corners of the edges it passes (the funnel, "string pulling")
        std::vector<RE::NiPoint3> Pull(const Graph& a_g, const std::vector<std::int32_t>& a_corridor, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
        {
            struct Portal
            {
                RE::NiPoint3 left, right;
            };
            std::vector<Portal> portals{ { a_from, a_from } };
            for (std::size_t i = 0; i + 1 < a_corridor.size(); ++i) {
                const auto& t = a_g.tris[a_corridor[i]];
                for (int k = 0; k < 3; ++k) {
                    if (t.n[k] == a_corridor[i + 1]) {
                        const auto& p = a_g.verts[t.v[k]];
                        const auto& q = a_g.verts[t.v[(k + 1) % 3]];
                        portals.push_back(Area2(t.c, p, q) > 0.0f ? Portal{ p, q } : Portal{ q, p });
                        break;
                    }
                }
            }
            portals.push_back({ a_to, a_to });
            const auto same = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::abs(a.x - b.x) < 0.01f && std::abs(a.y - b.y) < 0.01f; };
            std::vector<RE::NiPoint3> out{ a_from };
            RE::NiPoint3 apex = a_from, left = a_from, right = a_from;
            std::size_t  apexAt = 0, leftAt = 0, rightAt = 0;
            for (std::size_t i = 1, guard = 0; i < portals.size() && guard < portals.size() * 8; ++i, ++guard) {
                const auto& L = portals[i].left;
                const auto& R = portals[i].right;
                if (Area2(apex, right, R) <= 0.0f) {
                    if (same(apex, right) || Area2(apex, left, R) > 0.0f) {
                        right = R;
                        rightAt = i;
                    } else {
                        out.push_back(left);
                        apex = left;
                        apexAt = leftAt;
                        right = left = apex;
                        rightAt = leftAt = apexAt;
                        i = apexAt;
                        continue;
                    }
                }
                if (Area2(apex, left, L) >= 0.0f) {
                    if (same(apex, left) || Area2(apex, right, L) < 0.0f) {
                        left = L;
                        leftAt = i;
                    } else {
                        out.push_back(right);
                        apex = right;
                        apexAt = rightAt;
                        right = left = apex;
                        rightAt = leftAt = apexAt;
                        i = apexAt;
                        continue;
                    }
                }
            }
            if (out.empty() || !same(out.back(), a_to)) {
                out.push_back(a_to);
            }
            return out;
        }

        // ---- the worker
        std::mutex                lock;
        std::condition_variable   wake;
        std::shared_ptr<Raws>     pendingRaws;  // new navmeshes to build the graph from
        bool                      hasJob = false;
        RE::NiPoint3              jobFrom;
        std::vector<RE::NiPoint3> jobTo;
        std::vector<Found>        result;
        std::uint64_t             generation = 0;  // Clear() bumps it: a search under way is thrown away

        void Work()
        {
            Graph graph;
            for (;;) {
                std::shared_ptr<Raws>     raws;
                RE::NiPoint3              from;
                std::vector<RE::NiPoint3> to;
                bool                      job = false;
                std::uint64_t             gen;
                {
                    std::unique_lock guard(lock);
                    wake.wait(guard, [] { return hasJob || pendingRaws; });
                    raws = std::exchange(pendingRaws, nullptr);
                    job = std::exchange(hasJob, false);
                    from = jobFrom;
                    to = jobTo;
                    gen = generation;
                }
                if (raws) {
                    const auto start = std::chrono::steady_clock::now();
                    graph = Build(*raws);
                    logger::info("pathing: {} navmeshes, {} triangles, built in {:.1f} ms", raws->size(), graph.tris.size(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
                if (!job || graph.tris.empty()) {
                    continue;
                }
                // a path to every target, one after the other
                std::vector<Found> paths;
                auto               start = Find(graph, from, 1024.0f);
                if (start < 0) {
                    start = Nearest(graph, from);  // off the navmesh: from the nearest loaded ground, however far
                }
                for (const auto& target : to) {
                    Found found{ target, {} };
                    if (start >= 0) {
                        const auto goal = Find(graph, target, 512.0f);
                        const auto corridor = Search(graph, start, goal, target);
                        // the target reached: to it; else to the nearest the loaded navmesh came
                        const auto end = !corridor.empty() && corridor.back() == goal ? target : graph.tris[corridor.back()].c;
                        found.path = Pull(graph, corridor, from, end);
                    }
                    paths.push_back(std::move(found));
                }
                std::scoped_lock guard(lock);
                if (gen == generation) {
                    result = std::move(paths);
                }
            }
        }

        void StartWorker()
        {
            static std::once_flag once;
            std::call_once(once, [] { std::thread(Work).detach(); });
        }

        // main thread: what the copies were taken from (a navmesh and its size)
        std::vector<std::pair<const void*, std::size_t>> taken;
    }

    void Update(RE::PlayerCharacter* a_player)
    {
        const auto here = a_player ? a_player->GetParentCell() : nullptr;
        if (!here) {
            return;
        }
        std::vector<RE::TESObjectCELL*> cells;
        if (here->IsInteriorCell()) {
            cells.push_back(here);
        } else if (const auto tes = RE::TES::GetSingleton(); tes && tes->gridCells) {
            const auto grid = tes->gridCells;
            for (std::uint32_t x = 0; x < grid->length; ++x) {
                for (std::uint32_t y = 0; y < grid->length; ++y) {
                    if (const auto cell = grid->GetCell(x, y); cell && cell->IsAttached()) {
                        cells.push_back(cell);
                    }
                }
            }
        }
        std::vector<RE::NavMesh*>                        meshes;
        std::vector<std::pair<const void*, std::size_t>> now;
        for (const auto cell : cells) {
            const auto navs = cell->GetRuntimeData().navMeshes;
            for (std::uint32_t i = 0; navs && i < navs->navMeshes.size(); ++i) {
                const auto mesh = navs->navMeshes[i].get();
                // sanity: absurd sizes are a misread layout
                if (mesh && mesh->vertices.size() < 65535 && mesh->triangles.size() < 65535) {
                    meshes.push_back(mesh);
                    now.emplace_back(mesh, mesh->triangles.size());
                }
            }
        }
        if (now == taken) {
            return;
        }
        taken = std::move(now);
        auto raws = std::make_shared<Raws>();
        for (const auto mesh : meshes) {
            Raw raw;
            raw.id = mesh->GetFormID();
            raw.vertices.reserve(mesh->vertices.size());
            for (const auto& v : mesh->vertices) {
                raw.vertices.push_back(v.location);
            }
            raw.triangles.assign(mesh->triangles.begin(), mesh->triangles.end());
            raw.links.assign(mesh->extraEdgeInfo.begin(), mesh->extraEdgeInfo.end());
            raws->push_back(std::move(raw));
        }
        StartWorker();
        {
            std::scoped_lock guard(lock);
            pendingRaws = std::move(raws);
        }
        wake.notify_one();
    }

    void Request(const RE::NiPoint3& a_from, const std::vector<RE::NiPoint3>& a_to)
    {
        if (taken.empty() || a_to.empty()) {
            return;
        }
        {
            std::scoped_lock guard(lock);
            hasJob = true;
            jobFrom = a_from;
            jobTo = a_to;
        }
        wake.notify_one();
    }

    std::vector<Found> Paths()
    {
        std::scoped_lock guard(lock);
        return result;
    }

    void Clear()
    {
        taken.clear();
        std::scoped_lock guard(lock);
        ++generation;
        result.clear();
        hasJob = false;
    }
}
