#include "MapMesh.h"

#include "Settings.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <thread>

namespace MapMesh
{
    namespace
    {
        constexpr float       kHarvestEvery = 1.0f / 3.0f;   // seconds: the loaded cells' meshes are looked at this often
        constexpr std::size_t kUploadPerFrame = 1'500'000;   // triangles sent to the GPU in one frame
        constexpr std::size_t kKeptTriangles = 6'000'000;    // in memory at most: the loaded cells (always kept) and a few left behind
        constexpr std::size_t kBuildPerFrame = 40'000;       // triangles copied in one frame at most (about 2 ms): a cell arriving with many is spread over frames
        constexpr float       kCellSize = 4096.0f;           // an exterior cell
        constexpr float       kTinyRadius = 20.0f;           // meshes smaller than this (bound radius) are left off the map

        // what a mesh is drawn as (the shader's b1)
        enum Kind : int
        {
            kObjects = 0,  // buildings, rocks, furniture...
            kLand = 1,     // the landscape
            kWater = 2,    // lakes, rivers, the sea
            kKinds
        };

        // ---- geometry: per loaded cell one mesh from the meshes the game keeps a CPU copy of, plus one per mesh read
        // back from its GPU buffers (the landscape, and anything whose copy does not decode); world space
        struct CellMesh
        {
            RE::FormID                 cell = 0, space = 0;
            std::uint64_t              signature = 0;
            std::uint64_t              serial = 0;  // the build it came from (GPU reads arriving late check it)
            std::vector<float>         verts;  // x, y, z
            std::vector<std::uint32_t> indices;
            float                      minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
            std::size_t                triangles = 0;
            int                        kind = kObjects;  // which look it is drawn with
            std::uint64_t              lastSeen = 0;
            ID3D11Buffer*              vb = nullptr;
            ID3D11Buffer*              ib = nullptr;
            float                      shownAt = 0.0f;  // Now() it first went on the map (0: not yet): it fades in from there
            // part 0: the set it was built from - each shape by name and counts, sorted (what came, what went); with the
            // detailed log on also the names
            std::vector<std::uint64_t>                          shapes;
            std::vector<std::pair<std::uint64_t, std::string>> names;
        };
        std::uint64_t MeshKey(RE::FormID a_cell, std::uint32_t a_part) { return (static_cast<std::uint64_t>(a_cell) << 32) | a_part; }

        // a mesh to read from the GPU (render thread); the shape is kept alive meanwhile
        struct GpuJob
        {
            std::uint64_t                 key = 0;
            RE::FormID                    cell = 0, space = 0;
            RE::NiPointer<RE::BSTriShape> keep;
            RE::NiTransform               world;
            RE::NiBound                   bound;
            std::uint32_t                 stride = 0, vertexCount = 0, triangleCount = 0;
            bool                          full = false;
            ID3D11Buffer*                 vb = nullptr;
            ID3D11Buffer*                 ib = nullptr;
            const std::uint16_t*          rawIndices = nullptr;  // used when there (the shape's own copy)
            int                           kind = kObjects;
            std::uint64_t                 lastSeen = 0;
            bool                          staged = false;  // part of a rebuild: goes to staging, not straight on the map
            std::uint64_t                 serial = 0;      // the build it belongs to
        };

        std::mutex                                         meshLock;  // meshes, jobs, staging (the render thread uploads and draws)
        std::map<std::uint64_t, std::unique_ptr<CellMesh>> meshes;
        std::deque<GpuJob>                                 jobs;
        // a rebuilt cell waits here until all its parts are read: the old one stays on the map meanwhile (no blink)
        struct Staged
        {
            std::uint64_t                                       sig = 0;
            std::uint64_t                                       serial = 0;
            std::map<std::uint64_t, std::unique_ptr<CellMesh>> parts;
        };
        std::unordered_map<RE::FormID, Staged>             staging;
        // GPU reads started (buffers copied), read a frame or two later
        struct InFlight
        {
            GpuJob        job;
            ID3D11Buffer* vbCopy = nullptr;
            ID3D11Buffer* ibCopy = nullptr;
        };
        std::vector<InFlight>                              inFlight;
        std::uint64_t                                      buildSerial = 0;
        std::vector<RE::NiPointer<RE::BSTriShape>>         graveyard;  // shapes done with, released on the main thread
        std::vector<IUnknown*>                             releaseQueue;
        std::uint64_t                                      tick = 0;
        float                                              sinceHarvest = kHarvestEvery;
        // the detailed log's numbers (main thread; the render thread's are atomics)
        struct Perf
        {
            float                     since = 0.0f;  // seconds since the last report
            int                       frames = 0;
            double                    mainMs = 0.0, mainMax = 0.0;  // the minimap's whole update a frame, the harvest in it
            double                    harvestMs = 0.0, harvestMax = 0.0;
            int                       harvests = 0;
            int                       builds = 0, rebuilds = 0, carried = 0;  // cells built, built again, old land / water kept
            std::atomic<int>          renders = 0, redraws = 0;
            std::atomic<std::int64_t> renderUs = 0, renderMaxUs = 0;
            bool                      environment = false;  // logged once
        } perf;
        std::size_t                                        gpuRead = 0, gpuFailed = 0;
        // main thread
        std::unordered_map<RE::FormID, std::uint64_t>      lastSignature;  // per cell, the set seen at the last harvest
        std::unordered_map<RE::FormID, int>                unsettled;      // per cell, harvests its set has differed from the one on the map
        std::unordered_map<RE::FormID, std::uint64_t>      lastCheck;      // per cell, the harvest it was last looked at
        std::unordered_map<RE::FormID, int>                shrinking;      // per cell on the map, harvests its set has only lost shapes (hidden for a moment, or gone)
        std::unordered_map<RE::FormID, std::uint64_t>      lastBuilt;      // per cell, the harvest it was last built at

        RE::FormID mapSpace = 0;        // the world space (or the room) the map shows
        bool       hasGeometry = false;  // something gathered since the map was last turned off

        RE::FormID SpaceOf(RE::PlayerCharacter* a_player)
        {
            const auto cell = a_player->GetParentCell();
            if (!cell) {
                return 0;
            }
            if (cell->IsInteriorCell()) {
                return cell->GetFormID();
            }
            const auto world = a_player->GetWorldspace();
            return world ? world->GetFormID() : cell->GetFormID();
        }

        // ---- harvesting
        float HalfToFloat(std::uint16_t a_h)
        {
            const std::uint32_t sign = (a_h & 0x8000u) << 16;
            std::uint32_t       exp = (a_h >> 10) & 0x1F;
            std::uint32_t       mant = a_h & 0x3FF;
            std::uint32_t       bits;
            if (exp == 0) {
                if (mant == 0) {
                    bits = sign;
                } else {  // subnormal
                    exp = 127 - 15 + 1;
                    while ((mant & 0x400) == 0) {
                        mant <<= 1;
                        --exp;
                    }
                    mant &= 0x3FF;
                    bits = sign | (exp << 23) | (mant << 13);
                }
            } else if (exp == 31) {
                bits = sign | 0x7F800000u | (mant << 13);
            } else {
                bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
            }
            return std::bit_cast<float>(bits);
        }

        // foliage: a tree's or a plant's cards (leaves, fern fronds, grass) - a mess of quads on a map
        bool IsFoliage(RE::TESObjectREFR* a_owner, const RE::BSShaderProperty* a_shader)
        {
            if (a_shader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim)) {
                return true;
            }
            const auto base = a_owner ? a_owner->GetBaseObject() : nullptr;
            return base && (base->Is(RE::FormType::Tree) || base->Is(RE::FormType::Flora) || base->Is(RE::FormType::Grass));
        }

        bool IsLooseItem(const RE::TESBoundObject* a_base)
        {
            switch (a_base->GetFormType()) {
            case RE::FormType::Misc:
            case RE::FormType::Weapon:
            case RE::FormType::Armor:
            case RE::FormType::AlchemyItem:
            case RE::FormType::Ingredient:
            case RE::FormType::Book:
            case RE::FormType::KeyMaster:
            case RE::FormType::SoulGem:
            case RE::FormType::Ammo:
            case RE::FormType::Scroll:
            case RE::FormType::Projectile:
                return true;
            default:
                return false;
            }
        }

        // the drawable static geometry under a_obj (water too): no actors, nothing hidden, no effects / particles,
        // no foliage (alpha-tested planks, thatch, fences stay)
        struct CollectStats  // what was left out (for the log)
        {
            int hidden = 0, actors = 0, foliage = 0, shader = 0, other = 0, markers = 0, items = 0, tiny = 0;
        };
        CollectStats collectStats;  // of the last harvest (main thread)

        void Collect(RE::NiAVObject* a_obj, std::vector<RE::BSTriShape*>& a_out, int a_depth, RE::TESObjectREFR* a_owner = nullptr)
        {
            if (!a_obj || a_depth > 64) {
                return;
            }
            if (const auto ref = a_obj->GetUserData()) {
                if (ref->Is(RE::FormType::ActorCharacter)) {
                    ++collectStats.actors;
                    return;
                }
                // what the game never draws: triggers, occlusion planes, room and multibound boxes (all a
                // primitive), markers - their debug shapes are huge flat sheets on a map
                const auto base = ref->GetBaseObject();
                if (ref->extraList.HasType<RE::ExtraPrimitive>() || (base && base->IsMarker())) {
                    ++collectStats.markers;
                    return;
                }
                // loose items and whatever the game made at runtime (dropped things, arrows, spawned clutter): noise
                // on a map, and they come and go - a cell holding them would be rebuilt again and again
                if (ref->IsDynamicForm() || (base && IsLooseItem(base))) {
                    ++collectStats.items;
                    return;
                }
                a_owner = ref;
            }
            // not drawn either: the black planes behind load doors
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kNotVisible) || std::string_view(a_obj->name.c_str()).starts_with("DoorToBlack")) {
                ++collectStats.markers;
                return;
            }
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                ++collectStats.hidden;
                return;
            }
            if (const auto node = a_obj->AsNode()) {
                // a switch shows one of its children; the others are never updated (no bound, a stale transform) and
                // decode into garbage - huge sheets
                const auto  switchNode = netimmerse_cast<RE::NiSwitchNode*>(node);
                const auto& children = node->GetChildren();
                for (std::uint32_t i = 0; i < children.size(); ++i) {
                    if (switchNode && static_cast<std::int32_t>(i) != switchNode->index) {
                        ++collectStats.hidden;
                        continue;
                    }
                    Collect(children[static_cast<std::uint16_t>(i)].get(), a_out, a_depth + 1, a_owner);
                }
                return;
            }
            const auto geom = a_obj->AsGeometry();
            const auto tri = geom ? geom->AsTriShape() : nullptr;
            // never updated (the engine does not draw it): nothing to check its data against
            if (tri && a_obj->worldBound.radius <= 0.0f) {
                ++collectStats.hidden;
                return;
            }
            if (!tri || netimmerse_cast<RE::BSDynamicTriShape*>(tri)) {
                ++collectStats.other;
                return;
            }
            const auto& rd = geom->GetGeometryRuntimeData();
            // water is taken too (drawn as water), every other non-lighting shader is an effect
            const bool water = rd.shaderProperty && netimmerse_cast<RE::BSWaterShaderProperty*>(rd.shaderProperty.get());
            if (rd.skinInstance || !rd.rendererData || !rd.shaderProperty || (!water && !netimmerse_cast<RE::BSLightingShaderProperty*>(rd.shaderProperty.get()))) {
                ++collectStats.shader;
                return;
            }
            if (!water && rd.alphaProperty && rd.alphaProperty->GetAlphaTesting() && IsFoliage(a_owner, rd.shaderProperty.get())) {
                ++collectStats.foliage;
                return;
            }
            // small things (a plate, a cup, a coin purse): under a pixel on any map, yet often most of a city cell's triangles
            if (!water && a_obj->worldBound.radius < kTinyRadius) {
                ++collectStats.tiny;
                return;
            }
            a_out.push_back(tri);
        }

        std::uint64_t Mix(std::uint64_t a_x)  // splitmix64
        {
            a_x += 0x9E3779B97F4A7C15ull;
            a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
            a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
            return a_x ^ (a_x >> 31);
        }

        bool IsLand(RE::BSTriShape* a_shape);  // below

        // a shape by its name and counts. Not where it stands: a pushed bucket or an opened door is the same map, and
        // havok settles things a little differently each time
        std::uint64_t ShapeHash(RE::BSTriShape* a_shape)
        {
            // the landscape: by its block and place only - the game (or a renderer mod) swaps a block for one with other
            // counts as the character moves, the same ground
            if (IsLand(a_shape)) {
                const auto& c = a_shape->worldBound.center;
                std::uint64_t h = std::hash<std::string_view>{}(a_shape->name.c_str());
                h = Mix(h ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(c.x / 256.0f))));
                return Mix(h ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(c.y / 256.0f))));
            }
            const auto&   c = a_shape->GetTrishapeRuntimeData();
            std::uint64_t h = std::hash<std::string_view>{}(a_shape->name.c_str());
            h = Mix(h ^ c.triangleCount);
            return Mix(h ^ c.vertexCount);
        }

        std::vector<std::uint64_t> ShapeHashes(const std::vector<RE::BSTriShape*>& a_shapes)  // sorted
        {
            std::vector<std::uint64_t> out;
            out.reserve(a_shapes.size());
            for (const auto shape : a_shapes) {
                out.push_back(ShapeHash(shape));
            }
            std::ranges::sort(out);
            return out;
        }

        // the set of meshes of a cell, in any order: it changes when something comes or goes
        std::uint64_t Signature(const std::vector<std::uint64_t>& a_hashes)
        {
            std::uint64_t s = Mix(a_hashes.size());
            for (const auto h : a_hashes) {
                s += h;
            }
            return s;
        }

        RE::NiPoint3 ReadPosition(const std::uint8_t* a_p, bool a_full)
        {
            if (a_full) {
                RE::NiPoint3 p;
                std::memcpy(&p.x, a_p, sizeof(float) * 3);
                return p;
            }
            std::uint16_t h[3];
            std::memcpy(h, a_p, sizeof(h));
            return { HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]) };
        }

        struct Fit
        {
            float inside = 0.0f;  // share of the vertices inside the mesh's world bound
            float cover = 0.0f;   // how far out they reach, in bound radii
            float spread = 0.0f;  // the size of their box, in bound diameters: misread data collapses into a line or a dot
            bool  Good() const { return inside >= 0.97f && cover >= 0.3f && spread >= 0.2f; }
        };

        // the decoded vertices put into the world must fill the mesh's own world bound: that checks the format and
        // the transform at once
        Fit Measure(const std::uint8_t* a_data, std::uint32_t a_stride, std::uint32_t a_count, bool a_full, const RE::NiTransform& a_world, const RE::NiBound& a_bound)
        {
            if (a_bound.radius <= 0.0f) {
                return { 1.0f, 1.0f, 1.0f };  // nothing to check against
            }
            const float   r = a_bound.radius * 1.05f + 4.0f;
            const auto    step = std::max<std::uint32_t>(1, a_count / 256);
            std::uint32_t total = 0, inside = 0;
            float         reach = 0.0f;
            RE::NiPoint3  lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (std::uint32_t v = 0; v < a_count; v += step) {
                const auto p = a_world * ReadPosition(a_data + static_cast<std::size_t>(v) * a_stride, a_full);
                ++total;
                if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
                    const float d = p.GetDistance(a_bound.center);
                    if (d <= r) {
                        ++inside;
                        reach = std::max(reach, d);
                        lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                        hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
                    }
                }
            }
            const float extent = inside ? std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z }) : 0.0f;
            return { total ? static_cast<float>(inside) / total : 0.0f, reach / a_bound.radius, extent / (2.0f * a_bound.radius) };
        }

        int failLogs = 0;  // details of meshes that do not decode, the first few of a session
        int rejectedIndex = 0, rejectedEdge = 0;  // triangles the last Decode threw away (bad index / too long)
        int readLogs = 0;  // details of GPU reads, the first few of a session
        int flatLandLogs = 0;  // placeholder land blocks left out (for the log)
        int formatSwapped = 0;  // meshes whose vertex format flag was wrong (for the log)
        std::size_t strayTriangles = 0;  // triangles dropped for a vertex outside the mesh bound (for the log)

        // decode one mesh into a_out (world space); false = its data does not make sense either way
        bool Decode(const char* a_name, const std::uint8_t* a_data, std::uint32_t a_stride, std::uint32_t a_vertexCount, bool a_full, const std::uint16_t* a_indices,
            std::uint32_t a_triangleCount, const RE::NiTransform& a_world, const RE::NiBound& a_bound, CellMesh& a_out, bool a_logFailure)
        {
            // both formats measured, the one whose vertices fill the bound better wins (the flag is not to be trusted:
            // float positions read as halves still land inside the bound, as a 5-unit line)
            const auto flagged = Measure(a_data, a_stride, a_vertexCount, a_full, a_world, a_bound);
            const auto other = Measure(a_data, a_stride, a_vertexCount, !a_full, a_world, a_bound);
            if (!flagged.Good() && !other.Good()) {
                if (a_logFailure && failLogs < 60) {
                    ++failLogs;
                    const auto p0 = ReadPosition(a_data, a_full);
                    logger::info("map 3d: '{}' ({} verts, {} tris, stride {}, flag {}) does not fit its bound r {:.0f} at ({:.0f} {:.0f} {:.0f}): "
                                 "inside {:.2f}/{:.2f}, reach {:.2f}/{:.2f}, spread {:.2f}/{:.2f}; v0 ({:.1f} {:.1f} {:.1f}), node at ({:.0f} {:.0f} {:.0f}) x{:.2f}",
                        a_name ? a_name : "", a_vertexCount, a_triangleCount, a_stride, a_full ? "float" : "half", a_bound.radius, a_bound.center.x,
                        a_bound.center.y, a_bound.center.z, flagged.inside, other.inside, flagged.cover, other.cover, flagged.spread, other.spread, p0.x, p0.y,
                        p0.z, a_world.translate.x, a_world.translate.y, a_world.translate.z, a_world.scale);
                }
                return false;
            }
            if (!flagged.Good() || (other.Good() && other.spread > flagged.spread * 1.5f)) {
                a_full = !a_full;
                ++formatSwapped;
            }
            rejectedIndex = rejectedEdge = 0;
            const auto                base = static_cast<std::uint32_t>(a_out.verts.size() / 3);
            const float               maxEdge = a_bound.radius > 0.0f ? a_bound.radius * 2.2f + 4.0f : FLT_MAX;
            // every vertex of a good mesh lies inside its bound: one outside is misread data, and a triangle using it
            // stretches into a sheet that runs far up and down through the map
            const float               reach = a_bound.radius > 0.0f ? a_bound.radius * 1.05f + 4.0f : FLT_MAX;
            std::vector<RE::NiPoint3> world(a_vertexCount);
            std::vector<std::uint8_t> stray(a_vertexCount, 0);
            for (std::uint32_t v = 0; v < a_vertexCount; ++v) {
                const auto w = a_world * ReadPosition(a_data + static_cast<std::size_t>(v) * a_stride, a_full);
                world[v] = w;
                a_out.verts.insert(a_out.verts.end(), { w.x, w.y, w.z });
                if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z) || (reach < FLT_MAX && w.GetDistance(a_bound.center) > reach)) {
                    stray[v] = 1;
                    continue;
                }
                a_out.minX = std::min(a_out.minX, w.x);
                a_out.minY = std::min(a_out.minY, w.y);
                a_out.maxX = std::max(a_out.maxX, w.x);
                a_out.maxY = std::max(a_out.maxY, w.y);
                a_out.minZ = std::min(a_out.minZ, w.z);
                a_out.maxZ = std::max(a_out.maxZ, w.z);
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(a_triangleCount) * 3; i += 3) {
                const auto a = a_indices[i], b = a_indices[i + 1], c = a_indices[i + 2];
                if (a >= a_vertexCount || b >= a_vertexCount || c >= a_vertexCount || a == b || b == c || a == c) {
                    ++rejectedIndex;
                    continue;
                }
                if (stray[a] || stray[b] || stray[c]) {
                    ++strayTriangles;
                    continue;
                }
                // a triangle longer than the whole mesh is garbage
                if (world[a].GetDistance(world[b]) > maxEdge || world[b].GetDistance(world[c]) > maxEdge || world[a].GetDistance(world[c]) > maxEdge) {
                    ++rejectedEdge;
                    continue;
                }
                a_out.indices.insert(a_out.indices.end(), { base + a, base + b, base + c });
            }
            a_out.triangles = a_out.indices.size() / 3;
            return true;
        }

        struct BuildStats
        {
            std::size_t cpu = 0, gpu = 0;  // meshes from the CPU copy / to read from the GPU (for the log)
        };

        // the landscape: a cell quadrant "Block (x, y)" that belongs to no reference
        bool IsLand(RE::BSTriShape* a_shape)
        {
            if (!std::string_view(a_shape->name.c_str()).starts_with("Block (")) {
                return false;
            }
            for (RE::NiAVObject* o = a_shape; o; o = o->parent) {
                if (o->GetUserData()) {
                    return false;
                }
            }
            return true;
        }

        int KindOf(RE::BSTriShape* a_shape)
        {
            const auto shader = a_shape->GetGeometryRuntimeData().shaderProperty.get();
            if (shader && netimmerse_cast<RE::BSWaterShaderProperty*>(shader)) {
                return kWater;
            }
            return IsLand(a_shape) ? kLand : kObjects;
        }

        // a cell being built: its shapes copied a few at a time, a frame's budget of triangles each frame (a city cell of
        // a couple of million triangles at once was a tenth of a second's freeze); the shapes are held meanwhile
        struct Pending
        {
            RE::FormID                                 id = 0;
            std::string                                what;  // for the log
            std::uint64_t                              sig = 0;
            std::vector<RE::NiPointer<RE::BSTriShape>> shapes;
            std::size_t                                next = 0;  // the next shape to copy
            std::uint32_t                              part = 1;  // the next GPU read's part number
            std::unique_ptr<CellMesh>                  mesh;
            std::vector<GpuJob>                        jobs;
            BuildStats                                 stats;
        };
        std::deque<Pending> pending;  // main thread

        // a build begun: the cell's mesh (part 0) with its set, the shapes held
        Pending BeginBuild(RE::FormID a_id, RE::FormID a_space, std::string a_what, const std::vector<RE::BSTriShape*>& a_shapes, std::vector<std::uint64_t> a_hashes)
        {
            Pending p;
            p.id = a_id;
            p.what = std::move(a_what);
            p.mesh = std::make_unique<CellMesh>();
            auto& m = *p.mesh;
            m.cell = a_id;
            m.space = a_space;
            m.signature = p.sig = Signature(a_hashes);
            m.shapes = std::move(a_hashes);
            if (Settings::Map().debugLog) {
                for (const auto shape : a_shapes) {
                    m.names.emplace_back(ShapeHash(shape), shape->name.c_str());
                }
            }
            m.serial = ++buildSerial;
            p.shapes.reserve(a_shapes.size());
            for (const auto shape : a_shapes) {
                p.shapes.emplace_back(shape);
            }
            return p;
        }

        // one shape into the build: from its CPU copy into part 0, else a GPU read of its own
        void BuildShape(Pending& a_p, RE::BSTriShape* a_shape)
        {
            auto&       m = *a_p.mesh;
            const auto& rd = a_shape->GetGeometryRuntimeData();
            const auto  data = rd.rendererData;
            const auto& counts = a_shape->GetTrishapeRuntimeData();
            const auto  stride = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(rd.vertexDesc) & 0xF) * 4;
            const bool  noCounts = counts.vertexCount == 0 || counts.triangleCount == 0;  // a multi-part shape: the counts come from its GPU buffers
            if (!data || stride < 8 || (noCounts && (!data->vertexBuffer || !data->indexBuffer))) {
                return;
            }
            const bool full = rd.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
            const int  kind = KindOf(a_shape);  // the landscape and water: always meshes of their own (their own look)
            if (!noCounts && kind == kObjects && data->rawVertexData && data->rawIndexData &&
                Decode(a_shape->name.c_str(), data->rawVertexData, stride, counts.vertexCount, full, data->rawIndexData, counts.triangleCount, a_shape->world, a_shape->worldBound, m,
                    false)) {
                ++a_p.stats.cpu;
                return;
            }
            if (!data->vertexBuffer || (!data->indexBuffer && !data->rawIndexData)) {
                return;
            }
            GpuJob job;
            job.key = MeshKey(m.cell, a_p.part++);
            job.cell = m.cell;
            job.space = m.space;
            job.keep.reset(a_shape);
            job.world = a_shape->world;
            job.bound = a_shape->worldBound;
            job.stride = stride;
            job.vertexCount = counts.vertexCount;
            job.triangleCount = counts.triangleCount;
            job.full = full;
            job.vb = reinterpret_cast<ID3D11Buffer*>(data->vertexBuffer);
            job.ib = reinterpret_cast<ID3D11Buffer*>(data->indexBuffer);
            job.rawIndices = noCounts ? nullptr : data->rawIndexData;
            job.kind = kind;
            job.serial = m.serial;
            a_p.jobs.push_back(std::move(job));
            ++a_p.stats.gpu;
        }

        void Drop(CellMesh& a_mesh)  // under meshLock: GPU buffers go on the render thread
        {
            if (a_mesh.vb) {
                releaseQueue.push_back(a_mesh.vb);
            }
            if (a_mesh.ib) {
                releaseQueue.push_back(a_mesh.ib);
            }
            a_mesh.vb = nullptr;
            a_mesh.ib = nullptr;
        }

        void DropMeshes(RE::FormID a_cell)  // under meshLock: the parts on the map only
        {
            const auto lo = meshes.lower_bound(MeshKey(a_cell, 0));
            const auto hi = meshes.lower_bound(MeshKey(a_cell + 1, 0));
            for (auto it = lo; it != hi; ++it) {
                Drop(*it->second);
            }
            meshes.erase(lo, hi);
        }

        void DropJobs(RE::FormID a_cell)  // under meshLock: its pending GPU reads
        {
            for (auto it = jobs.begin(); it != jobs.end();) {
                if (it->cell == a_cell) {
                    graveyard.push_back(std::move(it->keep));
                    it = jobs.erase(it);
                } else {
                    ++it;
                }
            }
        }

        void DropCell(RE::FormID a_cell)  // under meshLock: every part of the cell, a rebuild in progress, its pending reads too
        {
            DropMeshes(a_cell);
            staging.erase(a_cell);
            std::erase_if(pending, [&](const Pending& a_p) { return a_p.id == a_cell; });
            DropJobs(a_cell);
            lastSignature.erase(a_cell);
            unsettled.erase(a_cell);
            lastCheck.erase(a_cell);
            shrinking.erase(a_cell);
            lastBuilt.erase(a_cell);
        }

        void EvictOver()  // main thread, under meshLock: the cells longest out of sight go once there are too many triangles
        {
            std::size_t total = 0;
            for (const auto& [key, m] : meshes) {
                total += m->triangles;
            }
            while (total > kKeptTriangles && !meshes.empty()) {
                auto oldest = meshes.begin();
                for (auto it = meshes.begin(); it != meshes.end(); ++it) {
                    if (it->second->lastSeen < oldest->second->lastSeen) {
                        oldest = it;
                    }
                }
                if (oldest->second->lastSeen == tick) {
                    break;  // everything left is loaded right now
                }
                const auto cell = oldest->second->cell;
                for (auto it = meshes.lower_bound(MeshKey(cell, 0)); it != meshes.end() && it->second->cell == cell; ++it) {
                    total -= it->second->triangles;
                }
                DropCell(cell);
            }
        }

        // a group replaced by a new build of it: what is the same as before (a part of the same kind, size and place;
        // the landscape and water by place alone) stays as it was on the map - no fade again; only what is new fades in
        // (a house, water that came later)
        struct WasShown
        {
            std::uint32_t part;
            int           kind;
            std::size_t   triangles;
            float         box[6];
            float         at;
        };

        std::vector<WasShown> ShownOf(RE::FormID a_cell)  // under meshLock
        {
            std::vector<WasShown> out;
            for (auto it = meshes.lower_bound(MeshKey(a_cell, 0)); it != meshes.end() && it->second->cell == a_cell; ++it) {
                const auto& m = *it->second;
                if (m.shownAt > 0.0f) {
                    out.push_back({ static_cast<std::uint32_t>(it->first & 0xFFFFFFFF), m.kind, m.triangles, { m.minX, m.minY, m.minZ, m.maxX, m.maxY, m.maxZ }, m.shownAt });
                }
            }
            return out;
        }

        void Inherit(CellMesh& a_mesh, std::uint32_t a_part, const std::vector<WasShown>& a_old)
        {
            const float box[6]{ a_mesh.minX, a_mesh.minY, a_mesh.minZ, a_mesh.maxX, a_mesh.maxY, a_mesh.maxZ };
            for (const auto& o : a_old) {
                // part 0 holds the many small things together: kept as it was whatever changed in it
                bool same = a_part == 0 ? o.part == 0 : o.part != 0 && o.kind == a_mesh.kind && (a_mesh.kind != kObjects || o.triangles == a_mesh.triangles);
                for (int i = 0; same && a_part != 0 && i < 6; ++i) {
                    same = std::abs(o.box[i] - box[i]) < 2.0f;
                }
                if (same) {
                    a_mesh.shownAt = o.at;
                    return;
                }
            }
        }

        // main thread, every frame: rebuilds whose every part is read take the old cell's place in one go
        void SwapStaged()
        {
            std::scoped_lock guard(meshLock);
            for (auto it = staging.begin(); it != staging.end();) {
                const auto id = it->first;
                const auto mine = [&](const GpuJob& a_job) { return a_job.cell == id && a_job.serial == it->second.serial; };
                if (std::ranges::any_of(jobs, mine) || std::ranges::any_of(inFlight, [&](const InFlight& a_p) { return mine(a_p.job); })) {
                    ++it;
                    continue;
                }
                const auto old = ShownOf(id);
                // the landscape or the water of the old build stays when the new one came without (a read that
                // failed, a block hidden for a moment): never a hole in the ground
                auto&         parts = it->second.parts;
                std::uint32_t carried = 0x40000000;
                for (const int kind : { kLand, kWater }) {
                    if (std::ranges::any_of(parts, [&](const auto& a_p) { return a_p.second->kind == kind; })) {
                        continue;
                    }
                    for (auto m = meshes.lower_bound(MeshKey(id, 0)); m != meshes.end() && m->second->cell == id;) {
                        if (m->second->kind == kind) {
                            parts[MeshKey(id, carried++)] = std::move(m->second);
                            m = meshes.erase(m);
                            ++perf.carried;
                        } else {
                            ++m;
                        }
                    }
                }
                DropMeshes(id);
                for (auto& [key, part] : parts) {
                    part->lastSeen = tick;
                    Inherit(*part, static_cast<std::uint32_t>(key & 0xFFFFFFFF), old);
                    meshes[key] = std::move(part);
                }
                it = staging.erase(it);
            }
        }

        // ---- what is gathered: the cells the game has loaded within the radius the minimap shows. A group is a cell:
        // an interior cell by its form, an exterior cell by its grid square. References whose 3D hangs outside every
        // cell's tree (persistent ones: big buildings, quest places) and the water planes go into the group of the cell
        // they stand in. Cells left behind stay in memory (back at once) until the triangle cap lets the oldest go
        constexpr std::uint64_t kRecheckTicks = 5;  // harvests between two checks of a cell that has not changed
        constexpr int           kGoneHarvests = 30;       // a cell on the map that only lost shapes is built again once they stayed gone this long (10 s)
        constexpr int           kUnsettledHarvests = 9;   // ...one whose set keeps changing: built anyway after this long (3 s)
        constexpr std::uint64_t kRebuildTicks = 9;        // a cell is built again at most this often (3 s)

        constexpr RE::FormID ExteriorId(std::int32_t a_x, std::int32_t a_y)
        {
            return 0xFF000000u | ((static_cast<RE::FormID>(a_x + 2048) & 0xFFF) << 12) | (static_cast<RE::FormID>(a_y + 2048) & 0xFFF);
        }
        std::pair<std::int32_t, std::int32_t> CoordsOf(RE::FormID a_id)
        {
            return { static_cast<std::int32_t>((a_id >> 12) & 0xFFF) - 2048, static_cast<std::int32_t>(a_id & 0xFFF) - 2048 };
        }

        // is the square of exterior cell (x, y) within a_radius of the point
        bool CellWithin(std::int32_t a_x, std::int32_t a_y, const RE::NiPoint3& a_pos, float a_radius)
        {
            const float nx = std::clamp(a_pos.x, a_x * kCellSize, (a_x + 1) * kCellSize) - a_pos.x;
            const float ny = std::clamp(a_pos.y, a_y * kCellSize, (a_y + 1) * kCellSize) - a_pos.y;
            return nx * nx + ny * ny <= a_radius * a_radius;
        }

        void DropAllGeometry()  // under meshLock
        {
            for (auto& [id, m] : meshes) {
                Drop(*m);
            }
            meshes.clear();
            staging.clear();
            pending.clear();
            for (auto& job : jobs) {
                graveyard.push_back(std::move(job.keep));
            }
            jobs.clear();
            lastSignature.clear();
            unsettled.clear();
            lastCheck.clear();
            shrinking.clear();
            lastBuilt.clear();
        }

        // a_extra: a harvest only to go on building what the last one left over (next frame) - a cell's set is not
        // judged settled by it (two frames in a row prove nothing)
        void Harvest(RE::PlayerCharacter* a_player, RE::FormID a_space, float a_radius, bool a_extra)
        {
            if (!a_extra) {
                ++tick;
            }
            const auto here = a_player->GetParentCell();
            if (!here) {
                return;
            }
            const auto pos = a_player->GetPosition();
            const bool inside = here->IsInteriorCell();

            // the cells wanted now (loaded, in the radius), and every loaded cell's tree
            std::vector<std::pair<RE::FormID, RE::TESObjectCELL*>> live;
            std::unordered_set<RE::NiAVObject*>                    roots;
            std::vector<RE::TESObjectCELL*>                        attached;  // every loaded cell (in the radius or not)
            if (inside) {
                live.emplace_back(here->GetFormID(), here);
                attached.push_back(here);
                if (const auto data = here->GetRuntimeData().loadedData; data && data->cell3D) {
                    roots.insert(data->cell3D.get());
                }
            } else if (const auto tes = RE::TES::GetSingleton(); tes && tes->gridCells) {
                const auto grid = tes->gridCells;
                for (std::uint32_t x = 0; x < grid->length; ++x) {
                    for (std::uint32_t y = 0; y < grid->length; ++y) {
                        const auto cell = grid->GetCell(x, y);
                        const auto coords = cell && cell->IsAttached() ? cell->GetCoordinates() : nullptr;
                        if (!coords) {
                            continue;
                        }
                        attached.push_back(cell);
                        if (const auto data = cell->GetRuntimeData().loadedData; data && data->cell3D) {
                            roots.insert(data->cell3D.get());
                        }
                        if (CellWithin(coords->cellX, coords->cellY, pos, a_radius)) {
                            live.emplace_back(ExteriorId(coords->cellX, coords->cellY), cell);
                        }
                    }
                }
            }

            // every cell the game has loaded stays in memory (seen now), in the scan or not: only those it let go of may
            // be dropped for room - a cell out of the scan for a moment is not built all over again when it comes back
            {
                std::scoped_lock guard(meshLock);
                for (const auto cell : attached) {
                    const auto coords = inside ? nullptr : cell->GetCoordinates();
                    const auto id = inside ? cell->GetFormID() : coords ? ExteriorId(coords->cellX, coords->cellY) : 0;
                    for (auto p = meshes.lower_bound(MeshKey(id, 0)); id != 0 && p != meshes.end() && p->second->cell == id; ++p) {
                        p->second->lastSeen = tick;
                    }
                }
            }

            // which cells are looked at this time: new ones, changed ones, and the rest every few harvests
            const auto due = [&](RE::FormID a_id) {
                std::scoped_lock guard(meshLock);
                const auto it = meshes.find(MeshKey(a_id, 0));
                if (it == meshes.end() || it->second->signature != lastSignature[a_id] || tick - lastCheck[a_id] >= kRecheckTicks) {
                    return true;
                }
                for (auto p = it; p != meshes.end() && p->second->cell == a_id; ++p) {
                    p->second->lastSeen = tick;
                }
                return false;
            };

            // one cell's meshes: kept while the same, rebuilt when they changed
            const auto process = [&](RE::FormID a_id, const std::string& a_what, const std::vector<RE::BSTriShape*>& a_shapes) {
                lastCheck[a_id] = tick;
                if (a_shapes.empty()) {
                    return;  // not loaded yet (a cell at the edge of the loaded ones)
                }
                auto       hashes = ShapeHashes(a_shapes);
                const auto sig = Signature(hashes);
                // a cell finishes loading over a few frames and some meshes flicker: a new set is built once it is seen
                // twice in a row (so a cell fades in whole, not in pieces); one that never settles is built anyway
                // after a few harvests
                const bool                                         settled = a_extra ? lastSignature[a_id] == sig : std::exchange(lastSignature[a_id], sig) == sig;
                bool                                               rebuild = false;  // a cell on the map built again (for the log: what changed)
                std::vector<std::uint64_t>                         oldShapes;
                std::vector<std::pair<std::uint64_t, std::string>> oldNames;
                {
                    std::scoped_lock guard(meshLock);
                    const auto it = meshes.find(MeshKey(a_id, 0));
                    const bool onMap = it != meshes.end();
                    auto&      wait = unsettled[a_id];
                    if (!a_extra) {
                        wait = !onMap || it->second->signature != sig ? wait + 1 : 0;
                    }
                    // the cell stays as it is on the map this time
                    const auto keep = [&] {
                        for (auto p = it; p != meshes.end() && p->second->cell == a_id; ++p) {
                            p->second->lastSeen = tick;
                        }
                        for (auto& job : jobs) {
                            if (job.cell == a_id) {
                                job.lastSeen = tick;
                            }
                        }
                    };
                    // this very set is being rebuilt already: wait for it
                    if (const auto st = staging.find(a_id); st != staging.end() && st->second.sig == sig) {
                        keep();
                        return;
                    }
                    if (std::ranges::any_of(pending, [&](const Pending& a_p) { return a_p.id == a_id && a_p.sig == sig; })) {
                        keep();
                        return;
                    }
                    if (onMap && it->second->signature == sig) {
                        shrinking.erase(a_id);
                        keep();
                        return;
                    }
                    if (onMap) {
                        // a cell on the map: only lost shapes (nothing new) is most often something hidden for a moment
                        // (faded out by distance, culled, a mod switching it) - the map stays as it is until they have
                        // been gone a good while. Anything new: built again once it settled, never more often than
                        // every few seconds (a cell whose set keeps changing does not rebuild over and over)
                        const bool fewer = std::ranges::includes(it->second->shapes, hashes);
                        auto&      shrink = shrinking[a_id];
                        shrink = !fewer ? 0 : a_extra ? shrink : shrink + 1;
                        const bool wanted = fewer ? shrink >= kGoneHarvests : settled || wait >= kUnsettledHarvests;
                        if (!wanted || tick - lastBuilt[a_id] < kRebuildTicks) {
                            keep();
                            return;
                        }
                        rebuild = true;
                        if (Settings::Map().debugLog) {
                            oldShapes = it->second->shapes;
                            oldNames = it->second->names;
                        }
                    } else if (!settled && wait < 3) {
                        return;
                    }
                }
                if (rebuild && Settings::Map().debugLog) {
                    // what came and what went, by name
                    std::vector<std::uint64_t> added, gone;
                    std::ranges::set_difference(hashes, oldShapes, std::back_inserter(added));
                    std::ranges::set_difference(oldShapes, hashes, std::back_inserter(gone));
                    const auto list = [&](const std::vector<std::uint64_t>& a_which, bool a_new) {
                        std::string out;
                        for (std::size_t i = 0; i < a_which.size() && i < 8; ++i) {
                            std::string name = "?";
                            if (a_new) {
                                const auto s = std::ranges::find_if(a_shapes, [&](RE::BSTriShape* a_s) { return ShapeHash(a_s) == a_which[i]; });
                                name = s != a_shapes.end() ? (*s)->name.c_str() : "?";
                            } else if (const auto n = std::ranges::find_if(oldNames, [&](const auto& a_n) { return a_n.first == a_which[i]; }); n != oldNames.end()) {
                                name = n->second;
                            }
                            out += (out.empty() ? "'" : ", '") + name + "'";
                        }
                        return a_which.size() > 8 ? out + ", ..." : out;
                    };
                    logger::info("map 3d: {} built again, {} harvests after the last build: {} new [{}], {} gone [{}]", a_what, tick - lastBuilt[a_id], added.size(),
                        list(added, true), gone.size(), list(gone, false));
                }
                rebuild ? ++perf.rebuilds : ++perf.builds;
                lastBuilt[a_id] = tick;
                shrinking.erase(a_id);
                // copied over the next frames (StepBuilds); a build of this cell still under way is replaced
                std::erase_if(pending, [&](const Pending& a_p) { return a_p.id == a_id; });
                pending.push_back(BeginBuild(a_id, a_space, a_what, a_shapes, std::move(hashes)));
            };

            // the cells due now: their trees, plus the references hanging outside every tree
            std::map<RE::FormID, std::vector<RE::BSTriShape*>> shapes;
            for (const auto& [id, cell] : live) {
                if (!due(id)) {
                    continue;
                }
                const auto data = cell->GetRuntimeData().loadedData;
                if (const auto root = data ? data->cell3D.get() : nullptr) {  // no 3D yet: nothing to take
                    Collect(root, shapes[id], 0);
                }
            }
            if (!shapes.empty()) {
                collectStats = {};
                // the references whose 3D hangs outside the cells' trees (the large ones: city walls, keeps, big rocks),
                // taken from the cells' own lists and the world's persistent ones - by where they stand on the ground,
                // not by a distance from the character (in 3D that misses everything below a character high above)
                std::unordered_set<RE::TESObjectREFR*> taken;
                const auto outside = [&](RE::TESObjectREFR* a_ref) {
                    const auto root = a_ref ? a_ref->Get3D() : nullptr;
                    if (!root || a_ref->IsDisabled()) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    // first where it stands (cheap): most references are in cells not due this time
                    const auto p = a_ref->GetPosition();
                    const auto id = inside ? here->GetFormID() :
                                             ExteriorId(static_cast<std::int32_t>(std::floor(p.x / kCellSize)), static_cast<std::int32_t>(std::floor(p.y / kCellSize)));
                    const auto it = shapes.find(id);
                    if (it == shapes.end() || !taken.insert(a_ref).second) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    for (RE::NiAVObject* o = root; o; o = o->parent) {
                        if (roots.contains(o)) {
                            return RE::BSContainer::ForEachResult::kContinue;  // in a cell's tree: gathered with it
                        }
                    }
                    Collect(root, it->second, 0, a_ref);
                    return RE::BSContainer::ForEachResult::kContinue;
                };
                // every loaded cell's list, not only the due ones: a reference listed in one cell may stand in its
                // neighbour - read only when that one is due, the neighbour's set flipped between two versions
                for (const auto cell : attached) {
                    cell->ForEachReference(outside);
                }
                if (const auto world = a_player->GetWorldspace(); world && world->persistentCell && !inside) {
                    world->persistentCell->ForEachReference(outside);
                }
                // the water planes hang under the water system, not under the cells: each into the cell it lies in
                if (const auto ws = RE::TESWaterSystem::GetSingleton()) {
                    RE::BSSpinLockGuard guard(ws->lock);
                    for (const auto& object : ws->waterObjects) {
                        const auto shape = object ? object->shape.get() : nullptr;
                        if (!shape) {
                            continue;
                        }
                        bool inTree = false;
                        for (RE::NiAVObject* o = shape; o && !inTree; o = o->parent) {
                            inTree = roots.contains(o);
                        }
                        const auto& c = shape->worldBound.center;
                        const auto  id = inside ? here->GetFormID() :
                                                  ExteriorId(static_cast<std::int32_t>(std::floor(c.x / kCellSize)), static_cast<std::int32_t>(std::floor(c.y / kCellSize)));
                        if (const auto it = shapes.find(id); !inTree && it != shapes.end()) {
                            Collect(shape, it->second, 0);
                        }
                    }
                }
                for (const auto& [id, list] : shapes) {
                    const auto [x, y] = CoordsOf(id);
                    process(id, inside ? std::format("cell {:08X}", id) : std::format("cell ({}, {})", x, y), list);
                }
            }

            // another world space or room: what was gathered there goes
            std::scoped_lock        guard(meshLock);
            std::vector<RE::FormID> gone;
            for (const auto& [key, m] : meshes) {
                if ((key & 0xFFFFFFFF) == 0 && m->space != a_space) {  // one look per cell, at part 0
                    gone.push_back(m->cell);
                }
            }
            for (const auto id : gone) {
                DropCell(id);
            }
        }

        // a build done: onto the map, or (a cell already on it) aside until its GPU reads are in
        void Commit(Pending& a_p)
        {
            auto& m = a_p.mesh;
            m->lastSeen = tick;
            logger::info("map 3d: {}: {} meshes - {} from the CPU copy ({} triangles), {} to read from the GPU", a_p.what, a_p.shapes.size(), a_p.stats.cpu, m->triangles,
                a_p.stats.gpu);
            std::scoped_lock guard(meshLock);
            const auto       id = a_p.id;
            const bool       replace = meshes.contains(MeshKey(id, 0));
            staging.erase(id);
            DropJobs(id);
            const auto serial = m->serial;
            if (replace) {
                auto& st = staging[id];
                st.sig = a_p.sig;
                st.serial = serial;
                st.parts[MeshKey(id, 0)] = std::move(m);
            } else {
                meshes[MeshKey(id, 0)] = std::move(m);
            }
            for (auto& job : a_p.jobs) {
                job.lastSeen = tick;
                job.staged = replace;
                jobs.push_back(std::move(job));
            }
            EvictOver();
        }

        // main thread, every frame: the builds under way go on, a frame's budget of triangles at a time
        void StepBuilds()
        {
            std::size_t done = 0;
            while (!pending.empty() && done < kBuildPerFrame) {
                auto& p = pending.front();
                while (p.next < p.shapes.size() && done < kBuildPerFrame) {
                    const auto shape = p.shapes[p.next++].get();
                    done += shape->GetTrishapeRuntimeData().triangleCount + 64;
                    BuildShape(p, shape);
                }
                if (p.next < p.shapes.size()) {
                    break;
                }
                Commit(p);
                pending.pop_front();
            }
        }

        // ---- roads: the navmesh's "preferred" triangles (orange in the Creation Kit) - the paths NPCs keep to, laid
        // along the roads. Taken per exterior cell from the game while it is loaded, kept for the world space. Drawn as
        // triangles into a mask of the picture (coverage and the road's height, exact at any zoom), which the shader
        // turns into a road on whatever surface lies there
        using RoadKey = std::pair<RE::FormID, RE::FormID>;  // world space, exterior cell
        std::map<RoadKey, std::vector<RE::NiPoint3>> roads;  // main thread: 3 corners a triangle, world space
        bool                                         roadsChanged = true;

        // what was last published (main thread)
        RE::FormID roadSpace = 0;
        bool       roadOutside = false;

        // the road triangles for the GPU (main thread publishes, render thread uploads)
        std::mutex         roadLock;
        std::vector<float> roadVerts;  // x, y, z a corner, 3 corners a triangle
        std::uint32_t      roadVersion = 0;
        bool               roadActive = false;

        void HarvestRoads(RE::PlayerCharacter* a_player, RE::FormID a_space, float a_radius)
        {
            const auto here = a_player->GetParentCell();
            const auto tes = RE::TES::GetSingleton();
            const bool outside = here && !here->IsInteriorCell() && tes && tes->gridCells;

            // another world space, or inside: the roads go
            for (auto it = roads.begin(); it != roads.end();) {
                if (outside && it->first.first == a_space) {
                    ++it;
                    continue;
                }
                it = roads.erase(it);
                roadsChanged = true;
            }
            if (!outside) {
                return;  // inside the preferred triangles are a dungeon's main path, not a road
            }

            using Flag = RE::BSNavmeshTriangle::TriangleFlag;
            const auto  pos = a_player->GetPosition();
            const auto  grid = tes->gridCells;
            std::size_t added = 0, triangles = 0;
            for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
                for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
                    const auto cell = grid->GetCell(gx, gy);
                    const auto coords = cell && cell->IsAttached() ? cell->GetCoordinates() : nullptr;
                    const auto navs = coords ? cell->GetRuntimeData().navMeshes : nullptr;
                    if (!navs || !CellWithin(coords->cellX, coords->cellY, pos, a_radius)) {
                        continue;
                    }
                    // looked at every harvest while loaded: a cell just come in may not have its navmesh yet
                    std::vector<RE::NiPoint3> corners;
                    std::size_t               navTriangles = 0;
                    for (const auto& ptr : navs->navMeshes) {
                        const auto mesh = ptr.get();
                        // sanity: absurd sizes are a misread layout
                        if (!mesh || mesh->vertices.size() >= 65535 || mesh->triangles.size() >= 65535) {
                            continue;
                        }
                        const auto count = mesh->vertices.size();
                        navTriangles += mesh->triangles.size();
                        for (const auto& t : mesh->triangles) {
                            if (!t.triangleFlags.all(Flag::kPreferred) || t.triangleFlags.all(Flag::kDeleted) || t.vertices[0] >= count ||
                                t.vertices[1] >= count || t.vertices[2] >= count) {
                                continue;
                            }
                            for (const auto v : t.vertices) {
                                corners.push_back(mesh->vertices[v].location);
                            }
                        }
                    }
                    if (navTriangles == 0) {
                        continue;  // its navmesh not loaded yet
                    }
                    auto& have = roads[{ a_space, ExteriorId(coords->cellX, coords->cellY) }];
                    if (have.size() != corners.size() || (!corners.empty() && std::memcmp(have.data(), corners.data(), corners.size() * sizeof(RE::NiPoint3)) != 0)) {
                        triangles += corners.size() / 3;
                        have = std::move(corners);
                        roadsChanged = true;
                        ++added;
                    }
                }
            }
            if (added) {
                logger::info("map roads: {} cells from the game, {} road triangles", added, triangles);
            }
        }

        // the road triangles of the world space shown, for the GPU (published when they change, not when the map moves)
        void PublishRoadMesh(RE::FormID a_space, bool a_outside)
        {
            if (!roadsChanged && a_space == roadSpace && a_outside == roadOutside) {
                return;
            }
            roadsChanged = false;
            roadSpace = a_space;
            roadOutside = a_outside;
            std::vector<float> verts;
            if (a_outside) {
                for (const auto& [key, list] : roads) {
                    if (key.first != a_space) {
                        continue;
                    }
                    for (const auto& p : list) {
                        verts.insert(verts.end(), { p.x, p.y, p.z });
                    }
                }
            }
            std::scoped_lock guard(roadLock);
            roadVerts = std::move(verts);
            roadActive = a_outside && !roadVerts.empty();
            ++roadVersion;
        }

        // a read finished: the build it belongs to is still the one on the map (or being staged)
        bool StillWanted(const GpuJob& a_job)
        {
            if (!a_job.staged) {
                const auto it = meshes.find(MeshKey(a_job.cell, 0));
                return it != meshes.end() && it->second->serial == a_job.serial;
            }
            const auto st = staging.find(a_job.cell);
            return st != staging.end() && st->second.serial == a_job.serial;
        }

        // render thread, under meshLock: queued meshes read back from their GPU buffers without waiting for the GPU - the
        // buffers are copied this frame and read a frame or two later, once the copies are done (a read right after
        // the copy would wait for everything the GPU has queued: a stall a frame)
        void ReadBack(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
        {
            constexpr std::size_t kPerFrame = 48;
            // the copies started earlier: read those done, the rest stay for the next frame
            std::size_t done = 0;
            for (auto it = inFlight.begin(); it != inFlight.end();) {
                auto&                    p = *it;
                auto&                    job = p.job;
                D3D11_MAPPED_SUBRESOURCE vmap{}, imap{};
                const HRESULT            vr = p.vbCopy ? a_context->Map(p.vbCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &vmap) : E_FAIL;
                if (vr == DXGI_ERROR_WAS_STILL_DRAWING) {
                    ++it;
                    continue;
                }
                HRESULT ir = E_FAIL;
                if (SUCCEEDED(vr) && !job.rawIndices && p.ibCopy) {
                    ir = a_context->Map(p.ibCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &imap);
                    if (ir == DXGI_ERROR_WAS_STILL_DRAWING) {
                        a_context->Unmap(p.vbCopy, 0);
                        ++it;
                        continue;
                    }
                }
                bool ok = false;
                if (SUCCEEDED(vr)) {
                    D3D11_BUFFER_DESC vd{};
                    p.vbCopy->GetDesc(&vd);
                    const std::uint16_t* indices = job.rawIndices;
                    std::size_t          indexBytes = static_cast<std::size_t>(job.triangleCount) * 6;
                    if (!indices && SUCCEEDED(ir)) {
                        D3D11_BUFFER_DESC id{};
                        p.ibCopy->GetDesc(&id);
                        if (job.triangleCount == 0) {  // a multi-part shape: all of its index buffer
                            job.triangleCount = id.ByteWidth / 6;
                            indexBytes = static_cast<std::size_t>(job.triangleCount) * 6;
                        }
                        indices = id.ByteWidth >= indexBytes ? static_cast<const std::uint16_t*>(imap.pData) : nullptr;
                    }
                    if (job.vertexCount == 0) {
                        job.vertexCount = std::min<std::uint32_t>(vd.ByteWidth / job.stride, 65536);
                    }
                    if (indices && vd.ByteWidth >= static_cast<std::size_t>(job.vertexCount) * job.stride && StillWanted(job)) {
                        auto m = std::make_unique<CellMesh>();
                        m->cell = job.cell;
                        m->space = job.space;
                        m->lastSeen = job.lastSeen;
                        m->kind = job.kind;
                        ok = Decode(job.keep ? job.keep->name.c_str() : "", static_cast<const std::uint8_t*>(vmap.pData), job.stride, job.vertexCount, job.full, indices, job.triangleCount, job.world, job.bound, *m, true);
                        const bool huge = ok && (m->maxX - m->minX > 3000.0f || m->maxY - m->minY > 3000.0f);  // bigger than a land block
                        if (readLogs < 10 || (huge && readLogs < 60)) {
                            ++readLogs;
                            const auto& b = job.bound;
                            logger::info("map 3d: GPU read '{}': {} verts, {} tris, stride {}, vb {} bytes, indices from {} -> {} -> {} triangles kept "
                                         "({} bad index, {} too long); bound r {:.0f} at ({:.0f} {:.0f} {:.0f}), mesh x {:.0f}..{:.0f} y {:.0f}..{:.0f}",
                                job.keep ? job.keep->name.c_str() : "", job.vertexCount, job.triangleCount, job.stride, vd.ByteWidth,
                                job.rawIndices ? "the CPU copy" : "the GPU", ok ? "decoded" : "NOT decoded", m->triangles, rejectedIndex, rejectedEdge, b.radius,
                                b.center.x, b.center.y, b.center.z, m->minX, m->maxX, m->minY, m->maxY);
                        }
                        // a cell without landscape data gets a perfectly flat block at the world's default height: a
                        // placeholder nobody walks on, a huge empty sheet on the map
                        const bool placeholder = ok && m->kind == kLand && m->maxZ - m->minZ < 1.0f;
                        if (placeholder && flatLandLogs < 20) {
                            ++flatLandLogs;
                            logger::info("map 3d: flat placeholder land '{}' at z {:.0f}, x {:.0f}..{:.0f} y {:.0f}..{:.0f} - left out", job.keep ? job.keep->name.c_str() : "",
                                m->minZ, m->minX, m->maxX, m->minY, m->maxY);
                        }
                        if (ok && m->triangles > 0 && !placeholder) {
                            if (!job.staged) {
                                meshes[job.key] = std::move(m);
                            } else {
                                staging[job.cell].parts[job.key] = std::move(m);
                            }
                        }
                    }
                    if (SUCCEEDED(ir)) {
                        a_context->Unmap(p.ibCopy, 0);
                    }
                    a_context->Unmap(p.vbCopy, 0);
                }
                ok ? ++gpuRead : ++gpuFailed;
                if (p.vbCopy) {
                    p.vbCopy->Release();
                }
                if (p.ibCopy) {
                    p.ibCopy->Release();
                }
                graveyard.push_back(std::move(job.keep));
                it = inFlight.erase(it);
                ++done;
            }
            if (done && jobs.empty() && inFlight.empty()) {
                logger::info("map 3d: GPU reads done - {} meshes read, {} could not be decoded, {} stray triangles so far", gpuRead, gpuFailed, strayTriangles);
            }

            // new copies, read in a later frame
            const auto copyOf = [&](ID3D11Buffer* a_src) -> ID3D11Buffer* {
                D3D11_BUFFER_DESC d{};
                a_src->GetDesc(&d);
                D3D11_BUFFER_DESC s{ d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
                ID3D11Buffer*     out = nullptr;
                if (FAILED(a_device->CreateBuffer(&s, nullptr, &out))) {
                    return nullptr;
                }
                a_context->CopyResource(out, a_src);
                return out;
            };
            while (!jobs.empty() && inFlight.size() < kPerFrame) {
                InFlight p{ std::move(jobs.front()) };
                jobs.pop_front();
                p.vbCopy = copyOf(p.job.vb);
                p.ibCopy = p.job.rawIndices ? nullptr : copyOf(p.job.ib);
                inFlight.push_back(std::move(p));
            }
        }

        // seconds since the plugin started: the clock of the fade-in (CPU stamps, shader compares)
        float Now()
        {
            static const auto start = std::chrono::steady_clock::now();
            return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        }

        // ---- the renderer (render thread only)
        constexpr char kShader[] = R"(
cbuffer C : register(b0)
{
    float4 row0, row1, row2, row3;  // world point -> clip space (orthographic)
    float4 player;     // xyz, w = cut height
    float4 groundLook;    // rgb, a opacity
    float4 geometryLook;
    float4 lines;      // x grid size, y contour step (0 = off), z strength
    float4 shade;      // x depth shade, y interior, z style (0 colour, 1 vanilla)
    float4 occluder;   // the character on this picture: px centre, px radii (z = 0: no occlusion cut)
    float4 occluder2;  // x: the character's depth on this picture, y: feet height to keep
    float4 timing;     // x now (s), y fade-in time (s)
    float4 roadBox;    // x widened by this many world units, w on
    float4 roadLook;   // rgb
    float4 water;      // rgb, a on
};
cbuffer K : register(b1)
{
    float4 kind;    // x: 0 objects, 1 the landscape, 2 water; y: Now() this mesh went on the map (0: long ago)
};
Texture2D<float2> roadMask : register(t0);  // per pixel of this picture: a road drawn there, its height

float Hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float Noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

struct VSOut
{
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float  depth : TEXCOORD1;  // along the view
};

VSOut VS(float3 p : POSITION)
{
    VSOut  o;
    float4 q = float4(p, 1.0);
    o.pos = float4(dot(row0, q), dot(row1, q), dot(row2, q), dot(row3, q));
    o.depth = o.pos.z;
    o.world = p;
    return o;
}

// strokes: parallel lines across u, about 1.5 px wide and a_px apart on screen, anchored in the world (they
// do not swim when the map moves); the spacing snaps to powers of two and the two nearest blend as the zoom changes
float StrokeAt(float u, float spacing, float pix)
{
    float d = abs(frac(u / spacing + 0.5) - 0.5) * spacing / pix;  // px to the nearest line
    return 1.0 - smoothstep(0.35, 1.1, d);
}

float Strokes(float u, float v, float pix, float px)
{
    float lv = log2(max(px * pix, 1e-3));
    float s0 = exp2(floor(lv));
    float b = frac(lv);
    float wobble = sin(v / (s0 * 5.0)) * s0 * 0.12;
    return lerp(StrokeAt(u + wobble, s0, pix), StrokeAt(u + wobble, s0 * 2.0, pix), b);
}

// water: wavy lines along the shore direction nobody knows - so along x, bent
float WaterLines(float3 w, float pix)
{
    float u = w.y + sin(w.x / 260.0) * 45.0 + sin(w.x / 97.0 + 1.3) * 12.0;
    return Strokes(u, w.x, pix, 9.0) * (0.45 + 0.55 * Noise(w.xy / 300.0));
}

// the road mask at a pixel: a road there at the surface's height (not one under a roof, nor a bridge's above)
float RoadAt(int2 p, float z)
{
    float2 r = roadMask.Load(int3(p, 0));
    return r.x * (1.0 - smoothstep(60.0, 140.0, abs(z - r.y)));
}

static const float2 kAround[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
                                   float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071) };

// a road on the pixel, the road widened by a_r px
float RoadFill(int2 p, float z, float a_r)
{
    float f = RoadAt(p, z);
    if (a_r >= 1.0) {
        [unroll] for (int k = 0; k < 8; ++k) {
            f = max(f, RoadAt(p + int2(round(kAround[k] * a_r)), z));
        }
    }
    return f;
}

// the road pass: the navmesh's road triangles into the road mask
float2 PSRoad(VSOut i) : SV_Target
{
    if (i.world.z > player.z + player.w + 60.0) {
        discard;  // cut away with what stands over the character
    }
    return float2(1.0, i.world.z);
}

float4 PS(VSOut i) : SV_Target
{
    float3 w = i.world;
    // the cut, 1: inside, everything over the head (a ceiling would hide the whole room)
    if (w.z > player.z + player.w) {
        discard;
    }
    // the cut, 2: exactly what covers the character on this picture - inside their outline on screen, nearer to the
    // view than they are and above their feet (a roof, a crown, a bridge, an arch): a tunnel down to them
    float occRim = 0.0;
    if (occluder.z > 0.0 && w.z > occluder2.y) {
        float r = length((i.pos.xy - occluder.xy) / occluder.zw);
        if (i.depth < occluder2.x) {
            if (r < 1.0) {
                discard;
            }
            occRim = 1.0 - smoothstep(1.0, 1.12, r);  // a bright edge around the tunnel
        }
    }
    // a mesh new to the map (a cell just loaded, a house, water arriving late) fades in
    float fade = kind.y > 0.0 ? saturate((timing.x - kind.y) / timing.y) : 1.0;
    if (fade <= 0.003) {
        discard;
    }
    float3 n = normalize(cross(ddx(w), ddy(w)));
    float  up = abs(n.z);
    float  flat = smoothstep(0.55, 0.85, up);
    float3 L = normalize(float3(-0.45, 0.55, 0.7));
    float  light = 0.55 + 0.6 * abs(dot(n, L));
    // world units per pixel, for lines of a steady width on screen (taken here: no gradients inside branches)
    float3 fw = float3(fwidth(w.x), fwidth(w.y), fwidth(w.z));
    float  pix = max(max(fw.x, fw.y), 1e-4);

    bool  isWater = kind.x > 1.5;
    // ground: the landscape; inside (no landscape there) the flat floors
    float isGround = isWater ? 0.0 : kind.x > 0.5 ? 1.0 : shade.y > 0.5 ? step(0.5, flat) : 0.0;
    float4 look = isGround > 0.5 || isWater ? groundLook : geometryLook;

    // lines on the ground: a grid and height lines, like a survey map
    float lineW = pix * 1.2 + 0.01;
    float grid = 0.0, contour = 0.0;
    if (lines.x > 0.0) {
        float2 g = abs(frac(w.xy / lines.x + 0.5) - 0.5) * lines.x;
        grid = (1.0 - saturate(min(g.x, g.y) / lineW)) * flat * isGround;
    }
    if (lines.y > 0.0) {
        float h = abs(frac(w.z / lines.y + 0.5) - 0.5) * lines.y;
        contour = (1.0 - saturate(h / (fw.z * 1.5 + 0.01))) * isGround;
    }

    // a road: from the picture's road mask (the road triangles drawn into it - exact at any zoom), only where the
    // surface is at the road's height (not a roof over it) and not too steep; widened by roadBox.x world units; its
    // border softened over a pixel (half its own, half the ring around)
    float onRoad = roadBox.w * (isWater ? 0.0 : 1.0) * smoothstep(0.3, 0.6, up);
    float roadFill = 0.0;
    if (onRoad > 0.0) {
        int2  rp = int2(i.pos.xy);
        float widen = min(roadBox.x / pix, 8.0);
        float around = 0.0;
        [unroll] for (int k = 0; k < 8; ++k) {
            around += RoadFill(rp + int2(round(kAround[k])), w.z, widen);
        }
        roadFill = (RoadFill(rp, w.z, widen) * 0.5 + around * 0.0625) * onRoad;
    }

    float dz = w.z - player.z;
    float depthMul = dz < 0.0 ? lerp(1.0, 1.0 - shade.x, saturate(-dz / 1500.0)) : 1.0 + 0.2 * saturate(dz / 400.0);
    float rim = saturate(1.0 - (player.z + player.w - w.z) / 25.0);

    float3 col;
    if (shade.z > 0.5) {
        // the vanilla local map: one dark sepia palette whatever the colours set - the floor dark, what stands on it
        // lighter, roads and the cut's edges lightest, water a dull slate; a little grain like the game's map
        float tone;
        if (isWater) {
            tone = 0.16 * (0.9 + 0.3 * WaterLines(w, pix));
        } else {
            tone = (isGround > 0.5 ? 0.30 : 0.56) * light;
            tone -= (grid * 0.05 + contour * 0.08) * lines.z;
            tone = lerp(tone, 0.62 * light, roadFill * 0.8);
        }
        tone = (tone + rim * 0.30 + occRim * 0.35) * depthMul;
        tone *= 0.93 + 0.07 * Noise(w.xy / 37.0);
        float t = saturate(tone);
        col = lerp(float3(0.045, 0.042, 0.036), float3(0.93, 0.87, 0.73), t * t * (3.0 - 2.0 * t));
        if (isWater) {
            col = lerp(col, float3(0.17, 0.20, 0.21), 0.55);
        }
        return float4(col, look.a * fade);
    }
    if (isWater) {
        col = water.rgb * (0.9 + 0.25 * WaterLines(w, pix));
    } else {
        col = look.rgb * light;
        col += grid * float3(0.10, 0.15, 0.22) * lines.z;
        col += contour * float3(0.12, 0.22, 0.30) * lines.z;
        col = lerp(col, roadLook.rgb * light, roadFill * 0.9);
    }
    // a bright rim where the cut opens things up
    col += rim * float3(0.35, 0.45, 0.6);
    col += occRim * float3(0.45, 0.65, 0.85);
    // below the character it gets darker with depth, above slightly lighter
    col *= depthMul;
    return float4(col, look.a * fade);
}
)";

        struct Constants
        {
            float row[4][4];
            float player[4];
            float ground[4];
            float geometry[4];
            float lines[4];
            float shade[4];
            float occluder[4];
            float occluder2[4];
            float timing[4];
            float roadBox[4];
            float roadLook[4];
            float water[4];
        };
        static_assert(sizeof(Constants) % 16 == 0);

        // the minimap's picture
        struct Target
        {
            int                       width = 0, height = 0;
            ID3D11Texture2D*          color = nullptr;  // multisampled
            ID3D11RenderTargetView*   rtv = nullptr;
            ID3D11Texture2D*          depth = nullptr;
            ID3D11DepthStencilView*   dsv = nullptr;
            ID3D11Texture2D*          resolved = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11Texture2D*          roadMask = nullptr;  // the road mask: covered, the road's height
            ID3D11RenderTargetView*   roadMaskRtv = nullptr;
            ID3D11ShaderResourceView* roadMaskSrv = nullptr;
            // the last picture drawn: shown again as it is while nothing about it changes
            std::vector<std::uint32_t> lastKey;
            std::uint64_t             lastSig = 0;
            bool                      lastSettled = false;  // nothing was fading in or waiting to go up
            bool                      drawn = false;

            void Release()
            {
                for (IUnknown* p : std::initializer_list<IUnknown*>{ srv, resolved, dsv, depth, rtv, color, roadMaskSrv, roadMaskRtv, roadMask }) {
                    if (p) {
                        p->Release();
                    }
                }
                *this = {};
            }
        };

        struct Renderer
        {
            bool                     tried = false, ready = false;
            UINT                     samples = 1;
            ID3D11VertexShader*      vs = nullptr;
            ID3D11PixelShader*       ps = nullptr;
            ID3D11InputLayout*       layout = nullptr;
            ID3D11Buffer*            cb = nullptr;
            ID3D11Buffer*            kindCb = nullptr;  // b1: which kind of mesh is being drawn
            ID3D11RasterizerState*   raster = nullptr;
            ID3D11DepthStencilState* depthState = nullptr;
            ID3D11BlendState*        blend = nullptr;
            ID3D11PixelShader*       psRoad = nullptr;     // the roads into the picture's road mask
            ID3D11BlendState*        roadBlend = nullptr;  // max: of two roads on a pixel (a bridge over one) the upper
            ID3D11Buffer*            roadVb = nullptr;     // every road triangle of the world space shown
            UINT                     roadCorners = 0;
            std::uint32_t            roadUploaded = 0;
            Target                   targets[2];  // the minimap's picture, the local map's (both may be up in one frame: one fading out)
        } r;

        // the shaders are compiled on a thread of their own as soon as the game's data is loaded (half a second of
        // D3DCompile on the render thread was a freeze on the first frame of the map)
        struct Compiled
        {
            std::atomic_bool done = false;
            bool             ok = false;
            ID3DBlob*        blobs[3]{};  // VS, PS, PSRoad
        };
        Compiled       compiled;
        std::once_flag compileOnce;

        void StartCompile()
        {
            std::call_once(compileOnce, [] {
                std::thread([] {
                    const auto start = std::chrono::steady_clock::now();
                    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
                    const auto compile = module ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
                    compiled.ok = compile != nullptr;
                    if (!compile) {
                        logger::error("map 3d: d3dcompiler_47.dll not found");
                    }
                    const char* stages[3][2] = { { "VS", "vs_5_0" }, { "PS", "ps_5_0" }, { "PSRoad", "ps_5_0" } };
                    for (int i = 0; compiled.ok && i < 3; ++i) {
                        ID3DBlob* errors = nullptr;
                        if (FAILED(compile(kShader, sizeof(kShader) - 1, "MapMesh", nullptr, nullptr, stages[i][0], stages[i][1], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &compiled.blobs[i], &errors))) {
                            logger::error("map 3d: shader {}: {}", stages[i][0], errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
                            compiled.ok = false;
                        }
                        if (errors) {
                            errors->Release();
                        }
                    }
                    logger::info("map 3d: shaders compiled in {:.0f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count());
                    compiled.done = true;
                }).detach();
            });
        }

        bool InitRenderer(ID3D11Device* a_device)
        {
            StartCompile();
            if (!compiled.done) {
                return false;  // not yet: the map shows once they are
            }
            r.tried = true;
            auto& blobs = compiled.blobs;
            if (!compiled.ok) {
                for (auto& b : blobs) {
                    if (b) {
                        b->Release();
                        b = nullptr;
                    }
                }
                return false;
            }
            UINT quality = 0;
            if (SUCCEEDED(a_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &quality)) && quality > 0) {
                r.samples = 4;
            }
            const D3D11_INPUT_ELEMENT_DESC element{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
            bool ok = SUCCEEDED(a_device->CreateVertexShader(blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), nullptr, &r.vs)) &&
                      SUCCEEDED(a_device->CreatePixelShader(blobs[1]->GetBufferPointer(), blobs[1]->GetBufferSize(), nullptr, &r.ps)) &&
                      SUCCEEDED(a_device->CreateInputLayout(&element, 1, blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), &r.layout)) &&
                      SUCCEEDED(a_device->CreatePixelShader(blobs[2]->GetBufferPointer(), blobs[2]->GetBufferSize(), nullptr, &r.psRoad));
            for (auto& b : blobs) {
                b->Release();
                b = nullptr;
            }

            D3D11_BUFFER_DESC cbDesc{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
            ok = ok && SUCCEEDED(a_device->CreateBuffer(&cbDesc, nullptr, &r.cb));
            D3D11_BUFFER_DESC kindDesc{ 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
            ok = ok && SUCCEEDED(a_device->CreateBuffer(&kindDesc, nullptr, &r.kindCb));
            D3D11_RASTERIZER_DESC rs{};
            rs.FillMode = D3D11_FILL_SOLID;
            rs.CullMode = D3D11_CULL_NONE;
            rs.DepthClipEnable = TRUE;
            rs.MultisampleEnable = TRUE;
            ok = ok && SUCCEEDED(a_device->CreateRasterizerState(&rs, &r.raster));
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = TRUE;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            ds.DepthFunc = D3D11_COMPARISON_LESS;
            ok = ok && SUCCEEDED(a_device->CreateDepthStencilState(&ds, &r.depthState));
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable = FALSE;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ok = ok && SUCCEEDED(a_device->CreateBlendState(&bd, &r.blend));
            D3D11_BLEND_DESC rb{};
            rb.RenderTarget[0].BlendEnable = TRUE;
            rb.RenderTarget[0].SrcBlend = rb.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            rb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
            rb.RenderTarget[0].SrcBlendAlpha = rb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            rb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
            rb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
            ok = ok && SUCCEEDED(a_device->CreateBlendState(&rb, &r.roadBlend));

            r.ready = ok;
            logger::info("map 3d: renderer {} ({}x MSAA)", ok ? "ready" : "FAILED", r.samples);
            return ok;
        }

        bool EnsureTarget(ID3D11Device* a_device, Target& a_t, int a_w, int a_h)
        {
            if (a_t.srv && a_t.width == a_w && a_t.height == a_h) {
                return true;
            }
            a_t.Release();
            const bool           ms = r.samples > 1;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(a_w);
            d.Height = static_cast<UINT>(a_h);
            d.MipLevels = d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = r.samples;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            bool ok = SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &a_t.color)) && SUCCEEDED(a_device->CreateRenderTargetView(a_t.color, nullptr, &a_t.rtv));
            d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ok = ok && SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &a_t.depth));
            D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsv.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
            ok = ok && SUCCEEDED(a_device->CreateDepthStencilView(a_t.depth, &dsv, &a_t.dsv));
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ok = ok && SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &a_t.resolved)) && SUCCEEDED(a_device->CreateShaderResourceView(a_t.resolved, nullptr, &a_t.srv));
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            // the road mask: per pixel whether a road is there and its height (no multisampling: read by pixel)
            d.Format = DXGI_FORMAT_R32G32_FLOAT;
            ok = ok && SUCCEEDED(a_device->CreateTexture2D(&d, nullptr, &a_t.roadMask)) && SUCCEEDED(a_device->CreateRenderTargetView(a_t.roadMask, nullptr, &a_t.roadMaskRtv)) &&
                 SUCCEEDED(a_device->CreateShaderResourceView(a_t.roadMask, nullptr, &a_t.roadMaskSrv));
            if (!ok) {
                a_t.Release();
                return false;
            }
            a_t.width = a_w;
            a_t.height = a_h;
            return true;
        }

        // the pipeline state we touch, put back afterwards
        struct StateBackup
        {
            ID3D11InputLayout*        layout = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY  topology{};
            ID3D11Buffer*             vb = nullptr;
            UINT                      vbStride = 0, vbOffset = 0;
            ID3D11Buffer*             ib = nullptr;
            DXGI_FORMAT               ibFormat{};
            UINT                      ibOffset = 0;
            ID3D11VertexShader*       vs = nullptr;
            ID3D11PixelShader*        ps = nullptr;
            ID3D11GeometryShader*     gs = nullptr;
            ID3D11Buffer*             vsCb = nullptr;
            ID3D11Buffer*             psCb = nullptr;
            ID3D11Buffer*             psCb1 = nullptr;
            ID3D11ShaderResourceView* psSrv = nullptr;
            ID3D11RasterizerState*    raster = nullptr;
            D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            ID3D11RenderTargetView*   rtv = nullptr;
            ID3D11DepthStencilView*   dsv = nullptr;
            ID3D11BlendState*         blend = nullptr;
            float                     blendFactor[4]{};
            UINT                      sampleMask = 0;
            ID3D11DepthStencilState*  depth = nullptr;
            UINT                      stencilRef = 0;

            void Save(ID3D11DeviceContext* c)
            {
                c->IAGetInputLayout(&layout);
                c->IAGetPrimitiveTopology(&topology);
                c->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
                UINT n = 0;
                c->VSGetShader(&vs, nullptr, &n);
                n = 0;
                c->PSGetShader(&ps, nullptr, &n);
                n = 0;
                c->GSGetShader(&gs, nullptr, &n);
                c->VSGetConstantBuffers(0, 1, &vsCb);
                c->PSGetConstantBuffers(0, 1, &psCb);
                c->PSGetConstantBuffers(1, 1, &psCb1);
                c->PSGetShaderResources(0, 1, &psSrv);
                c->RSGetState(&raster);
                c->RSGetViewports(&viewportCount, viewports);
                c->OMGetRenderTargets(1, &rtv, &dsv);
                c->OMGetBlendState(&blend, blendFactor, &sampleMask);
                c->OMGetDepthStencilState(&depth, &stencilRef);
            }

            void Restore(ID3D11DeviceContext* c)
            {
                c->IASetInputLayout(layout);
                c->IASetPrimitiveTopology(topology);
                c->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
                c->IASetIndexBuffer(ib, ibFormat, ibOffset);
                c->VSSetShader(vs, nullptr, 0);
                c->PSSetShader(ps, nullptr, 0);
                c->GSSetShader(gs, nullptr, 0);
                c->VSSetConstantBuffers(0, 1, &vsCb);
                c->PSSetConstantBuffers(0, 1, &psCb);
                c->PSSetConstantBuffers(1, 1, &psCb1);
                c->PSSetShaderResources(0, 1, &psSrv);
                c->RSSetState(raster);
                c->RSSetViewports(viewportCount, viewports);
                c->OMSetRenderTargets(1, &rtv, dsv);
                c->OMSetBlendState(blend, blendFactor, sampleMask);
                c->OMSetDepthStencilState(depth, stencilRef);
                for (IUnknown* p : std::initializer_list<IUnknown*>{ layout, vb, ib, vs, ps, gs, vsCb, psCb, psCb1, psSrv, raster, rtv, dsv, blend, depth }) {
                    if (p) {
                        p->Release();
                    }
                }
            }
        };
    }

    namespace
    {
        double MsSince(std::chrono::steady_clock::time_point a_from)
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a_from).count();
        }

        // what a report needs to be read: the game, the mods that matter, the settings that cost
        void LogEnvironment()
        {
            const auto  ini = RE::INISettingCollection::GetSingleton();
            const auto  grids = ini ? ini->GetSetting("uGridsToLoad:General") : nullptr;
            const auto  plugin = SKSE::PluginDeclaration::GetSingleton();
            const auto& s = Settings::Map();
            logger::info("diagnostics: Detailed MiniMap {}, Skyrim {}; Community Shaders {}, ENB {}; uGridsToLoad {}; {}x MSAA; minimap {:.0f} px, range {:.0f} / {:.0f} "
                         "inside, tilt {:.0f}, style {}, roads {}, water {}, local map {}",
                plugin ? plugin->GetVersion().string() : "?", REL::Module::get().version().string(), GetModuleHandleW(L"CommunityShaders.dll") ? "yes" : "no",
                std::filesystem::exists("enbseries.ini") ? "yes" : "no", grids ? grids->GetUnsignedInteger() : 0u, r.samples, s.minimapSize, s.minimapRange,
                s.minimapRangeInside, s.minimapTilt, s.look.style, s.look.roads, s.look.water, s.localMap);
        }

        // the detailed log: every 10 seconds what the map cost and did
        void Report(float a_delta)
        {
            if (!Settings::Map().debugLog) {
                perf.since = 0.0f;
                return;
            }
            if (!perf.environment) {
                perf.environment = true;
                LogEnvironment();
            }
            perf.since += a_delta;
            ++perf.frames;
            if (perf.since < 10.0f) {
                return;
            }
            std::size_t cells = 0, parts = 0, triangles = 0, waiting = 0;
            {
                std::scoped_lock guard(meshLock);
                for (const auto& [key, m] : meshes) {
                    ++parts;
                    triangles += m->triangles;
                    cells += (key & 0xFFFFFFFF) == 0 ? 1 : 0;
                }
                waiting = jobs.size() + inFlight.size();
            }
            const int   renders = perf.renders.exchange(0), redraws = perf.redraws.exchange(0);
            const auto  renderUs = perf.renderUs.exchange(0), renderMaxUs = perf.renderMaxUs.exchange(0);
            const float frames = static_cast<float>(std::max(perf.frames, 1));
            logger::info("perf: {:.0f} s, {} frames ({:.0f} fps): minimap update {:.2f} ms a frame (max {:.2f}); harvests {}, {:.2f} ms each (max {:.2f}); "
                         "pictures drawn in {} of {} frames, {:.2f} ms CPU each (max {:.2f}); cells built {}, built again {}, old land / water kept {}; "
                         "in memory {} cells, {} parts, {}k triangles; GPU reads waiting {} ({} read, {} failed so far)",
                perf.since, perf.frames, frames / perf.since, perf.mainMs / frames, perf.mainMax, perf.harvests, perf.harvests ? perf.harvestMs / perf.harvests : 0.0,
                perf.harvestMax, redraws, renders, redraws ? renderUs / 1000.0 / redraws : 0.0, renderMaxUs / 1000.0, perf.builds, perf.rebuilds, perf.carried, cells,
                parts, triangles / 1000, waiting, gpuRead, gpuFailed);
            perf.since = 0.0f;
            perf.frames = perf.harvests = perf.builds = perf.rebuilds = perf.carried = 0;
            perf.mainMs = perf.mainMax = perf.harvestMs = perf.harvestMax = 0.0;
        }
    }

    void NoteFrame(float a_ms)
    {
        perf.mainMs += a_ms;
        perf.mainMax = std::max(perf.mainMax, static_cast<double>(a_ms));
        if (a_ms > 8.0f) {
            logger::info("perf: a slow minimap update, {:.1f} ms", a_ms);
        }
    }

    void Update(RE::PlayerCharacter* a_player, float a_delta, float a_radius)
    {
        // nothing on screen: no work at all; the map off (< 0): the memory goes too
        if (a_radius <= 0.0f) {
            if (a_radius < 0.0f && hasGeometry) {
                hasGeometry = false;
                std::scoped_lock guard(meshLock);
                DropAllGeometry();
                roads.clear();
                roadsChanged = true;
                logger::info("map 3d: map off, geometry unloaded");
            }
            sinceHarvest = kHarvestEvery;  // the first update once shown again harvests at once
            return;
        }
        hasGeometry = true;
        const auto& s = Settings::Map();
        const auto  space = SpaceOf(a_player);
        if (!space) {
            return;
        }
        {
            std::scoped_lock guard(meshLock);
            graveyard.clear();  // shapes the render thread is done with
        }
        SwapStaged();
        const auto cell = a_player->GetParentCell();
        const bool outdoors = cell && !cell->IsInteriorCell();
        // the meshes are picked up 3 times a second, so what loads in while running shows at once; scanned a bit
        // farther than the map shows, so a cell is in (and faded in) before it comes into view
        const float scan = a_radius * 1.25f + kCellSize * 0.5f;
        if ((sinceHarvest += a_delta) >= kHarvestEvery) {
            const auto start = std::chrono::steady_clock::now();
            sinceHarvest = 0.0f;
            Harvest(a_player, space, scan, false);
            if (s.look.roads) {
                HarvestRoads(a_player, space, scan);
            }
            const double ms = MsSince(start);
            ++perf.harvests;
            perf.harvestMs += ms;
            perf.harvestMax = std::max(perf.harvestMax, ms);
            if (ms > 6.0) {
                logger::info("perf: a slow harvest, {:.1f} ms", ms);
            }
        }
        // the cells being built: a frame's share
        if (!pending.empty()) {
            const auto start = std::chrono::steady_clock::now();
            StepBuilds();
            if (const double ms = MsSince(start); ms > 6.0) {
                logger::info("perf: a slow build step, {:.1f} ms", ms);
            }
        }
        PublishRoadMesh(space, s.look.roads && outdoors);
        mapSpace = space;
        Report(a_delta);
    }

    RE::FormID CurrentSpace()
    {
        return mapSpace;
    }

    void Prepare()
    {
        StartCompile();
    }

    void* Render(const View& a_view)
    {
        const auto device = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
        const auto data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
        const auto context = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
        if (!device || !context || a_view.width < 8 || a_view.height < 8) {
            return nullptr;
        }
        if (!r.tried) {
            InitRenderer(device);
        }
        if (!r.ready) {
            return nullptr;
        }
        const auto renderStart = std::chrono::steady_clock::now();
        auto& t = r.targets[std::clamp(a_view.slot, 0, 1)];
        if (!EnsureTarget(device, t, a_view.width, a_view.height)) {
            return nullptr;
        }

        // the road triangles: up again when they change (a cell's roads come in, another world space)
        {
            std::scoped_lock guard(roadLock);
            if (roadVersion != r.roadUploaded) {
                if (r.roadVb) {
                    r.roadVb->Release();
                    r.roadVb = nullptr;
                }
                r.roadCorners = 0;
                if (roadActive) {
                    D3D11_BUFFER_DESC      rd{ static_cast<UINT>(roadVerts.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
                    D3D11_SUBRESOURCE_DATA rdata{ roadVerts.data(), 0, 0 };
                    if (SUCCEEDED(device->CreateBuffer(&rd, &rdata, &r.roadVb))) {
                        r.roadCorners = static_cast<UINT>(roadVerts.size() / 3);
                    }
                }
                r.roadUploaded = roadVersion;
            }
        }

        // which meshes this picture draws: in range of the character, on the picture and, for objects, bigger than
        // a pixel
        const auto visible = [&](const CellMesh& a_m) {
            if (a_m.space != a_view.space || (a_m.indices.empty() && !a_m.ib) || (a_m.kind == kWater && a_view.water[3] < 0.5f)) {
                return false;  // another world space; no triangles (a cell's empty part 0); water off
            }
            const float nx = std::clamp(a_view.player.x, a_m.minX, a_m.maxX) - a_view.player.x;
            const float ny = std::clamp(a_view.player.y, a_m.minY, a_m.maxY) - a_view.player.y;
            if (nx * nx + ny * ny > a_view.range * a_view.range) {
                return false;
            }
            const float c[3]{ (a_m.minX + a_m.maxX) * 0.5f, (a_m.minY + a_m.maxY) * 0.5f, (a_m.minZ + a_m.maxZ) * 0.5f };
            const float h[3]{ (a_m.maxX - a_m.minX) * 0.5f, (a_m.maxY - a_m.minY) * 0.5f, (a_m.maxZ - a_m.minZ) * 0.5f };
            const auto& w = a_view.rows;
            const float cx = w[0][0] * c[0] + w[0][1] * c[1] + w[0][2] * c[2] + w[0][3];
            const float cy = w[1][0] * c[0] + w[1][1] * c[1] + w[1][2] * c[2] + w[1][3];
            const float ex = std::abs(w[0][0]) * h[0] + std::abs(w[0][1]) * h[1] + std::abs(w[0][2]) * h[2];
            const float ey = std::abs(w[1][0]) * h[0] + std::abs(w[1][1]) * h[1] + std::abs(w[1][2]) * h[2];
            if (std::abs(cx) - ex > 1.0f || std::abs(cy) - ey > 1.0f) {
                return false;  // off the picture
            }
            return a_m.kind != kObjects || ex * a_view.width >= 1.0f || ey * a_view.height >= 1.0f;  // under a pixel across: not drawn
        };

        // the picture depends on the view and on the meshes it draws: while neither changes (and nothing is fading
        // in or waiting to go up) the last one is shown again - a map standing still costs nothing
        std::vector<std::uint32_t> key;
        {
            const auto add = [&](const float* a_p, std::size_t a_n) {
                for (std::size_t i = 0; i < a_n; ++i) {
                    key.push_back(std::bit_cast<std::uint32_t>(a_p[i]));
                }
            };
            add(&a_view.rows[0][0], 16);
            const float values[]{ a_view.player.x, a_view.player.y, a_view.player.z, a_view.cut, a_view.range, a_view.fadeTime };
            add(values, std::size(values));
            for (const auto* a : { a_view.ground, a_view.geometry, a_view.lines, a_view.shade, a_view.occluder, a_view.occluder2, a_view.roads, a_view.water }) {
                add(a, 4);
            }
            key.insert(key.end(), { static_cast<std::uint32_t>(a_view.width), static_cast<std::uint32_t>(a_view.height), a_view.space, r.roadUploaded });
        }
        std::uint64_t sig = 1469598103934665603ull;
        bool          settled = true;
        {
            std::scoped_lock guard(meshLock);
            for (const auto object : releaseQueue) {
                object->Release();
            }
            releaseQueue.clear();
            ReadBack(device, context);
            const float now = Now();
            const float fadeTime = std::max(a_view.fadeTime, 0.01f);
            for (const auto& [id, m] : meshes) {
                if (!visible(*m)) {
                    continue;
                }
                for (const auto v : { id, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(m->vb)), static_cast<std::uint64_t>(m->triangles) }) {
                    sig = (sig ^ v) * 1099511628211ull;
                }
                if (!m->vb || m->shownAt <= 0.0f || now - m->shownAt < fadeTime) {
                    settled = false;
                }
            }
        }
        if (t.drawn && t.lastSettled && settled && sig == t.lastSig && key == t.lastKey) {
            ++perf.renders;
            return t.srv;
        }
        t.lastKey = std::move(key);
        t.lastSig = sig;
        t.lastSettled = settled;

        StateBackup backup;
        backup.Save(context);

        Constants c{};
        std::memcpy(c.row, a_view.rows, sizeof(c.row));
        c.player[0] = a_view.player.x;
        c.player[1] = a_view.player.y;
        c.player[2] = a_view.player.z;
        c.player[3] = a_view.cut;
        std::memcpy(c.ground, a_view.ground, sizeof(c.ground));
        std::memcpy(c.geometry, a_view.geometry, sizeof(c.geometry));
        std::memcpy(c.lines, a_view.lines, sizeof(c.lines));
        std::memcpy(c.shade, a_view.shade, sizeof(c.shade));
        std::memcpy(c.occluder, a_view.occluder, sizeof(c.occluder));
        std::memcpy(c.occluder2, a_view.occluder2, sizeof(c.occluder2));
        c.timing[0] = Now();
        c.timing[1] = std::max(a_view.fadeTime, 0.01f);
        const bool roadsOn = r.roadCorners > 0 && a_view.roads[3] > 0.0f;
        c.roadBox[0] = std::max(a_view.roads[3] - 1.0f, 0.0f);  // widened by this many world units either side
        c.roadBox[3] = roadsOn ? 1.0f : 0.0f;
        std::memcpy(c.roadLook, a_view.roads, sizeof(c.roadLook));
        std::memcpy(c.water, a_view.water, sizeof(c.water));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &c, sizeof(c));
            context->Unmap(r.cb, 0);
        }

        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_view.width), static_cast<float>(a_view.height), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
        context->RSSetState(r.raster);
        context->IASetInputLayout(r.layout);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(r.vs, nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &r.cb);
        context->PSSetConstantBuffers(0, 1, &r.cb);
        constexpr UINT kStride = sizeof(float) * 3, kOffset = 0;

        // first the roads into the road mask (a pixel: covered, the road's height - the upper of two), exact at any zoom
        const float none[4]{ 0.0f, -1.0e6f, 0.0f, 0.0f };
        context->ClearRenderTargetView(t.roadMaskRtv, none);
        if (roadsOn) {
            context->OMSetRenderTargets(1, &t.roadMaskRtv, nullptr);
            context->OMSetBlendState(r.roadBlend, nullptr, 0xFFFFFFFF);
            context->PSSetShader(r.psRoad, nullptr, 0);
            context->IASetVertexBuffers(0, 1, &r.roadVb, &kStride, &kOffset);
            context->Draw(r.roadCorners, 0);
        }

        const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
        context->ClearRenderTargetView(t.rtv, clear);
        context->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        context->OMSetRenderTargets(1, &t.rtv, t.dsv);
        context->OMSetBlendState(r.blend, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(r.depthState, 0);
        context->PSSetShader(r.ps, nullptr, 0);
        context->PSSetConstantBuffers(1, 1, &r.kindCb);
        context->PSSetShaderResources(0, 1, &t.roadMaskSrv);

        {
            std::scoped_lock guard(meshLock);
            std::size_t      uploaded = 0;
            // objects first, then the landscape, then water (land above the water level hides it: the shore draws
            // itself); each with its own look (b1 says which)
            const float now = Now();
            const float fadeTime = std::max(a_view.fadeTime, 0.01f);
            for (int pass = 0; pass < kKinds; ++pass) {
                // b1: the kind, and when the mesh being drawn went on the map while it is still fading in
                float      kindValue[4]{ static_cast<float>(pass), 0.0f, 0.0f, 0.0f };
                const auto setFade = [&](float a_at) {
                    kindValue[1] = a_at;
                    D3D11_MAPPED_SUBRESOURCE kindMap{};
                    if (SUCCEEDED(context->Map(r.kindCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &kindMap))) {
                        std::memcpy(kindMap.pData, kindValue, sizeof(kindValue));
                        context->Unmap(r.kindCb, 0);
                    }
                };
                setFade(0.0f);
                for (auto& [id, m] : meshes) {
                    if (m->kind != pass || !visible(*m)) {
                        continue;
                    }
                    if (!m->vb) {
                        // something already on the map (a rebuilt cell's unchanged part) goes up at once: never a gap
                        if (uploaded > kUploadPerFrame && m->shownAt <= 0.0f) {
                            continue;
                        }
                        D3D11_BUFFER_DESC      vd{ static_cast<UINT>(m->verts.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
                        D3D11_SUBRESOURCE_DATA vdata{ m->verts.data(), 0, 0 };
                        D3D11_BUFFER_DESC      idesc{ static_cast<UINT>(m->indices.size() * sizeof(std::uint32_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
                        D3D11_SUBRESOURCE_DATA idata{ m->indices.data(), 0, 0 };
                        if (FAILED(device->CreateBuffer(&vd, &vdata, &m->vb)) || FAILED(device->CreateBuffer(&idesc, &idata, &m->ib))) {
                            Drop(*m);
                            m->indices.clear();  // never again
                            logger::warn("map 3d: could not upload a mesh of cell {:08X}", m->cell);
                            continue;
                        }
                        uploaded += m->triangles;
                        // the GPU has it now
                        std::vector<float>().swap(m->verts);
                        std::vector<std::uint32_t>().swap(m->indices);
                    }
                    if (m->shownAt <= 0.0f) {
                        m->shownAt = std::max(now, 1.0e-3f);
                    }
                    const float fadeFrom = now - m->shownAt < fadeTime ? m->shownAt : 0.0f;
                    if (fadeFrom != kindValue[1]) {
                        setFade(fadeFrom);
                    }
                    context->IASetVertexBuffers(0, 1, &m->vb, &kStride, &kOffset);
                    context->IASetIndexBuffer(m->ib, DXGI_FORMAT_R32_UINT, 0);
                    context->DrawIndexed(static_cast<UINT>(m->triangles * 3), 0, 0);
                }
            }
        }

        if (r.samples > 1) {
            context->ResolveSubresource(t.resolved, 0, t.color, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        } else {
            context->CopyResource(t.resolved, t.color);
        }
        backup.Restore(context);
        t.drawn = true;
        const auto us = static_cast<std::int64_t>(MsSince(renderStart) * 1000.0);
        ++perf.renders;
        ++perf.redraws;
        perf.renderUs += us;
        for (auto most = perf.renderMaxUs.load(); us > most && !perf.renderMaxUs.compare_exchange_weak(most, us);) {
        }
        return t.srv;
    }
}
