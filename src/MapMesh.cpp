#include "MapMesh.h"

#include "Fog.h"
#include "Settings.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <queue>
#include <thread>
#include <unordered_set>

namespace MapMesh
{
    namespace
    {
        constexpr float       kRoadsEvery = 1.0f / 3.0f;     // seconds: the loaded cells' road triangles are looked at this often
        constexpr double      kWorkMs = 0.4;                 // main thread: what gathering the map may take in a frame (ms)
        constexpr std::size_t kUploadBytes = 6u << 20;       // render thread: geometry sent to the GPU in one frame at most
        constexpr std::size_t kKeptTriangles = 8'000'000;    // placed on the map at most: what is loaded always, what was left behind until this
        constexpr std::size_t kModelBytes = 512u << 20;      // GPU memory the models no longer placed may keep (the longest unused go first)
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

        // decoded triangles: x, y, z a vertex, 3 indices a triangle, the box they fill
        struct Geometry
        {
            std::vector<float>         verts;
            std::vector<std::uint32_t> indices;
            float                      minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX, minZ = FLT_MAX, maxZ = -FLT_MAX;
            std::size_t                triangles = 0;
            bool                       swapped = false;  // read in the other format than its flag says
        };

        // ---- what the map holds. A model is one mesh's geometry in its own space, shared by every reference showing it
        // (a wall piece placed 300 times is decoded and sent to the GPU once, and found again when the cell comes back);
        // a mesh with no copy of its data in memory (the landscape, some others) is read back from the GPU into a model
        // of its own, in world space. An instance is a model placed: by its shape's world transform, or as it is
        enum ModelState : int
        {
            kDecoding = 0,  // on the worker thread, or waiting for its GPU read
            kReady = 1,     // decoded: goes up to the GPU (render thread, a frame's budget at a time)
            kFailed = 2     // its data made no sense: never drawn
        };

        struct Model
        {
            std::uint64_t key = 0;
            int           kind = kObjects;
            int           state = kDecoding;
            Geometry      geo;            // until it is on the GPU
            std::size_t   triangles = 0;
            std::size_t   bytes = 0;      // on the GPU
            ID3D11Buffer* vb = nullptr;
            ID3D11Buffer* ib = nullptr;
            std::vector<std::uint32_t> placed;  // the slots of its instances (the pictures draw a model's together)
            std::uint64_t lastUsed = 0;   // the sweep pass it was last placed or let go in
            float         extent[6]{};    // the detailed log: its geometry as decoded (min x y z, max x y z), and in which format
            bool          swapped = false;
            std::string   from;           // ...from which mesh
        };

        struct Instance
        {
            Model*        model = nullptr;
            float         row[3][4]{};    // its space -> world
            float         box[6]{};       // world space, from its shape's bound: min x y z, max x y z
            RE::FormID    owner = 0;      // the reference, or the cell (the landscape, water)
            RE::FormID    space = 0;
            std::size_t   triangles = 0;  // the shape's count (the model's may not be known yet)
            float         shownAt = 0.0f; // Now() it first went on a picture (0: not yet): it fades in from there
            std::uint64_t lastSeen = 0;   // the sweep pass it was last found loaded in
            std::uint32_t serial = 0;     // which use of its slot this is (a handle names slot and serial)
            bool          baked = false;  // its model read back from the GPU in world space (moved: taken again)
        };

        // a game buffer held while we use it: the shape alone does not keep it (a block of landscape let go frees its
        // buffers while we may still copy them a few frames later)
        struct BufferRef
        {
            ID3D11Buffer* p = nullptr;

            BufferRef() = default;
            explicit BufferRef(ID3D11Buffer* a_p) :
                p(a_p)
            {
                if (p) {
                    p->AddRef();
                }
            }
            BufferRef(BufferRef&& a_o) noexcept :
                p(std::exchange(a_o.p, nullptr)) {}
            BufferRef& operator=(BufferRef&& a_o) noexcept
            {
                if (this != &a_o) {
                    reset();
                    p = std::exchange(a_o.p, nullptr);
                }
                return *this;
            }
            BufferRef(const BufferRef&) = delete;
            BufferRef& operator=(const BufferRef&) = delete;
            ~BufferRef() { reset(); }
            void reset()
            {
                if (p) {
                    std::exchange(p, nullptr)->Release();
                }
            }
        };

        // a mesh to read from the GPU into its model (render thread); the shape and its buffers are kept alive meanwhile
        struct GpuJob
        {
            std::uint64_t                 key = 0;  // the model
            RE::NiPointer<RE::BSTriShape> keep;
            RE::NiTransform               world;
            RE::NiBound                   bound;
            std::uint32_t                 stride = 0, vertexCount = 0, triangleCount = 0;
            bool                          full = false;
            BufferRef                     vb;
            BufferRef                     ib;
            std::vector<std::uint16_t>    rawIndices;  // the shape's own copy when there (taken now: it may go)
        };

        std::mutex                                                 meshLock;  // models' state, instances, GPU jobs (the render thread reads back, uploads, draws)
        std::unordered_map<std::uint64_t, std::unique_ptr<Model>> models;     // added and removed on the main thread only
        std::vector<Instance>                                      instances(1);  // by slot (0: none), a free one without a model; added and removed on the main thread only
        std::vector<std::uint32_t>                                 freeSlots;
        std::uint32_t                                              instanceSerial = 0;

        // an instance by its handle (slot and serial): nullptr once it is gone (its slot may hold another by then)
        Instance* InstanceAt(std::uint64_t a_handle)
        {
            const auto slot = static_cast<std::uint32_t>(a_handle & 0xFFFFFFFF);
            const auto serial = static_cast<std::uint32_t>(a_handle >> 32);
            return slot < instances.size() && instances[slot].model && instances[slot].serial == serial ? &instances[slot] : nullptr;
        }
        std::atomic<std::uint64_t>                                 geometryVersion = 1;  // what the map holds changed: the pictures are drawn again
        std::deque<GpuJob>                                         jobs;
        // GPU reads started (buffers copied), read a frame or two later
        struct InFlight
        {
            GpuJob        job;
            ID3D11Buffer* vbCopy = nullptr;
            ID3D11Buffer* ibCopy = nullptr;
        };
        std::vector<InFlight>                                      inFlight;
        std::vector<RE::NiPointer<RE::BSTriShape>>                 graveyard;  // shapes done with, released on the main thread
        std::vector<IUnknown*>                                     releaseQueue;
        std::uint64_t                                              pass = 1;   // the sweep's passes over the loaded references
        std::uint64_t                                              gpuSerial = 0;
        float                                                      sinceRoads = kRoadsEvery;
        // the detailed log's numbers (main thread; the render thread's are atomics)
        struct Perf
        {
            float                     since = 0.0f;  // seconds since the last report
            int                       frames = 0;
            double                    mainMs = 0.0, mainMax = 0.0;  // the minimap's whole update a frame, the gathering in it
            double                    workMs = 0.0, workMax = 0.0;
            int                       refs = 0, models = 0, passes = 0;  // references taken again, models made, sweeps done
            std::atomic<int>          renders = 0, redraws = 0, uploads = 0;
            std::atomic<std::int64_t> renderUs = 0, renderMaxUs = 0;
            bool                      environment = false;  // logged once
        } perf;
        std::atomic<std::size_t>                                   gpuRead = 0, gpuFailed = 0;

        // the on-screen panel's numbers: a second's worth, taken into liveStats once a second (main thread; the render
        // thread's are atomics)
        struct LiveAcc
        {
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
            int                                   frames = 0;
            double                                updateMs = 0.0, updateMax = 0.0, workMs = 0.0, workMax = 0.0;
            int                                   refs = 0, models = 0, passes = 0;
            std::atomic<int>                      redraws = 0, reused = 0, uploads = 0;
            std::atomic<std::int64_t>             renderUs = 0, renderMaxUs = 0;
            std::atomic<std::size_t>              drawnInstances = 0, drawnTriangles = 0, drawCalls = 0;
        } live;
        std::mutex statsLock;
        Stats      liveStats;

        RE::FormID mapSpace = 0;         // the world space (or the room) the map shows
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
            // no reference: the game's distant objects (LOD) - their see-through cards are the trees' billboards
            if (!a_owner) {
                return true;
            }
            const auto base = a_owner->GetBaseObject();
            if (base && (base->Is(RE::FormType::Tree) || base->Is(RE::FormType::Flora) || base->Is(RE::FormType::Grass))) {
                return true;
            }
            // a mod's trees and plants made as statics: by their model's path
            const auto model = base ? base->As<RE::TESModel>() : nullptr;
            if (!model || !model->GetModel()) {
                return false;
            }
            std::string path = model->GetModel();
            std::ranges::transform(path, path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return std::ranges::any_of(std::array<std::string_view, 7>{ "tree", "plant", "shrub", "bush", "fern", "grass", "foliage" },
                [&](std::string_view a_word) { return path.contains(a_word); });
        }

        // ---- the mesh filter: Data/SKSE/Plugins/DetailedMiniMap/MeshFilter.json - two lists of words looked for in a
        // mesh's name and its model's path (any case): "Blacklist" leaves such meshes off the map ("@rule": one of the
        // mod's own rules, on - a rule not listed is off; no file: all on), "Whitelist" keeps them whatever the
        // blacklist says ("word = water": drawn as water). Read when a game is loaded (main thread)
        struct FilterWord
        {
            std::string word;  // lower case (a rule: its name)
            bool        keep = false;
            bool        water = false;
            bool        rule = false;
        };
        std::vector<FilterWord> filter;
        bool                    filterRead = false;
        bool                    filterFound = false;  // the file was there

        std::vector<FilterWord> ReadFilter()
        {
            std::vector<FilterWord> out;
            const auto              path = std::filesystem::current_path() / "Data/SKSE/Plugins/DetailedMiniMap/MeshFilter.json";
            std::ifstream           file(path);
            filterFound = false;
            if (!file.is_open()) {
                return out;
            }
            // comments (// and /* */) allowed; a broken file: the mod's defaults, said in the log
            const auto json = nlohmann::json::parse(file, nullptr, false, true);
            if (json.is_discarded() || !json.is_object()) {
                logger::warn("MeshFilter.json: not valid JSON - the built-in filter instead");
                return out;
            }
            filterFound = true;
            const auto trim = [](std::string_view a_s) {
                const auto first = a_s.find_first_not_of(" \t\r\n");
                const auto last = a_s.find_last_not_of(" \t\r\n");
                return first == std::string_view::npos ? std::string() : std::string(a_s.substr(first, last - first + 1));
            };
            const auto lower = [](std::string a_s) {
                std::ranges::transform(a_s, a_s.begin(), [](char a_c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c))); });
                return a_s;
            };
            // a section by its name, any case
            const auto section = [&](std::string_view a_name) -> const nlohmann::json* {
                for (const auto& [key, value] : json.items()) {
                    if (lower(key) == a_name && value.is_array()) {
                        return &value;
                    }
                }
                return nullptr;
            };
            for (const auto [name, keep] : { std::pair{ "blacklist", false }, std::pair{ "whitelist", true } }) {
                const auto list = section(name);
                for (std::size_t i = 0; list && i < list->size(); ++i) {
                    if (!(*list)[i].is_string()) {
                        continue;
                    }
                    auto       text = trim((*list)[i].get<std::string>());
                    FilterWord f;
                    f.keep = keep;
                    if (!keep && text.starts_with('@')) {  // a rule (the blacklist only)
                        f.rule = true;
                        text = trim(std::string_view(text).substr(1));
                    }
                    if (const auto eq = text.find('='); eq != std::string::npos) {  // "word = water" (the whitelist)
                        f.water = keep && lower(trim(std::string_view(text).substr(eq + 1))) == "water";
                        text = trim(std::string_view(text).substr(0, eq));
                    }
                    if (!text.empty()) {
                        f.word = lower(std::move(text));
                        out.push_back(std::move(f));
                    }
                }
            }
            return out;
        }

        void ReadFilterOnce()
        {
            if (!filterRead) {
                filter = ReadFilter();
                filterRead = true;
            }
        }

        // one of the mod's own rules: on while the file lists it (no file: every rule on)
        bool Rule(std::string_view a_name)
        {
            ReadFilterOnce();
            return !filterFound || std::ranges::any_of(filter, [&](const FilterWord& a_f) { return a_f.rule && a_f.word == a_name; });
        }

        // the reference whose 3D a shape is part of (the root's user data)
        RE::TESObjectREFR* OwnerOf(const RE::NiAVObject* a_obj)
        {
            for (auto at = a_obj; at; at = at->parent) {
                if (const auto ref = at->GetUserData()) {
                    return ref;
                }
            }
            return nullptr;
        }

        const char* ModelOf(RE::TESObjectREFR* a_owner)
        {
            const auto base = a_owner ? a_owner->GetBaseObject() : nullptr;
            const auto model = base ? base->As<RE::TESModel>() : nullptr;
            return model ? model->GetModel() : nullptr;
        }

        // a backdrop, not a thing: the editor's markers folder (Markers\BlackPlane01 - a black sheet Bethesda sets behind
        // the void in caves and keeps, thousands of units across: on a map it covers everything)
        bool IsBackdrop(std::string_view a_name)
        {
            const auto lower = [](char a_c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c))); };
            std::string name(a_name.size(), ' ');
            std::ranges::transform(a_name, name.begin(), lower);
            return name.starts_with("markers\\") || name.starts_with("meshes\\markers\\") || name.find("blackplane") != std::string::npos;
        }

        // what the filter's words say of a mesh: the first word found (a kept one before a left-off one), nullptr:
        // nothing (no file: the effects folder left off, as the shipped file has it)
        const FilterWord* Filtered(RE::TESObjectREFR* a_owner, const RE::NiAVObject* a_shape)
        {
            ReadFilterOnce();
            static const std::vector<FilterWord> fallback{ { "effects\\" } };
            const auto&                          words = filterFound ? filter : fallback;
            const auto                           contains = [](std::string_view a_s, std::string_view a_word) {
                return !a_s.empty() && !std::ranges::search(a_s, a_word, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }).empty();
            };
            const std::string_view name(a_shape->name.c_str());
            const auto             path = ModelOf(a_owner);
            const std::string_view model(path ? path : "");
            const FilterWord*      found = nullptr;
            for (const auto& f : words) {
                if (!f.rule && (contains(name, f.word) || contains(model, f.word))) {
                    if (f.keep) {
                        return &f;
                    }
                    found = found ? found : &f;
                }
            }
            return found;
        }

        // the mod's own rules for what is no solid ground or wall (each one a "@rule" of the filter): see-through (alpha
        // blended - smoke, light shafts, cobwebs: nothing solid is drawn so), refracting (the shimmer over a fire), a decal
        // (blood, dirt), an effect's shader (fire, magic, glow - anything but the lit and the water ones)
        bool LeftOffByRules(const RE::BSShaderProperty* a_shader, const RE::NiAlphaProperty* a_alpha)
        {
            using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
            if (Rule("effectshader") && !netimmerse_cast<const RE::BSLightingShaderProperty*>(a_shader)) {
                return true;
            }
            if (Rule("seethrough") && a_alpha && a_alpha->GetAlphaBlending()) {
                return true;
            }
            if (Rule("refraction") && a_shader->flags.any(Flag::kRefraction)) {
                return true;
            }
            return Rule("decal") && a_shader->flags.any(Flag::kDecal, Flag::kDynamicDecal);
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
            int hidden = 0, actors = 0, foliage = 0, shader = 0, other = 0, markers = 0, items = 0, tiny = 0, filtered = 0;
        };
        CollectStats collectStats;  // of the last harvest (main thread)
        std::string* collectTrace = nullptr;  // the detailed log's diagnosis: every step of a collection written down

        void Trace(const RE::NiAVObject* a_obj, int a_depth, std::string_view a_what)
        {
            if (collectTrace) {
                *collectTrace += std::format("\n      {}{} '{}': {}", std::string(static_cast<std::size_t>(a_depth) * 2, ' '), a_obj ? (const_cast<RE::NiAVObject*>(a_obj)->AsNode() ? "node" : "mesh") : "null", a_obj ? a_obj->name.c_str() : "", a_what);
            }
        }

        // water the game hides while the camera does not see it (its planes are culled and shown again all the time):
        // a water mesh, or a node right above water meshes - hidden or not it is the same water on a map
        bool IsWaterShape(const RE::NiAVObject* a_obj)
        {
            const auto geom = const_cast<RE::NiAVObject*>(a_obj)->AsGeometry();
            return geom && netimmerse_cast<RE::BSWaterShaderProperty*>(geom->GetGeometryRuntimeData().shaderProperty.get());
        }

        bool Hidden(RE::NiAVObject* a_obj)
        {
            if (!a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                return false;
            }
            if (IsWaterShape(a_obj)) {
                return false;
            }
            if (const auto node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child && IsWaterShape(child.get())) {
                        return false;
                    }
                }
            }
            return true;
        }

        void Collect(RE::NiAVObject* a_obj, std::vector<RE::BSTriShape*>& a_out, int a_depth, RE::TESObjectREFR* a_owner = nullptr)
        {
            if (!a_obj || a_depth > 64) {
                Trace(a_obj, a_depth, "null or too deep");
                return;
            }
            if (const auto ref = a_obj->GetUserData()) {
                if (ref->Is(RE::FormType::ActorCharacter)) {
                    ++collectStats.actors;
                    Trace(a_obj, a_depth, "an actor");
                    return;
                }
                // what the game never draws: triggers, occlusion planes, room and multibound boxes (all a
                // primitive), markers - their debug shapes are huge flat sheets on a map
                const auto base = ref->GetBaseObject();
                if (ref->extraList.HasType<RE::ExtraPrimitive>() || (base && base->IsMarker()) || (base && IsBackdrop(ModelOf(ref) ? ModelOf(ref) : ""))) {
                    ++collectStats.markers;
                    Trace(a_obj, a_depth, "a marker or primitive");
                    return;
                }
                // loose items and whatever the game made at runtime (dropped things, arrows, spawned clutter): noise
                // on a map, and they come and go - a cell holding them would be rebuilt again and again
                if (ref->IsDynamicForm() || (base && IsLooseItem(base))) {
                    ++collectStats.items;
                    Trace(a_obj, a_depth, std::format("an item or made at runtime ({:08X})", ref->GetFormID()));
                    return;
                }
                a_owner = ref;
            }
            // not drawn either: the black planes behind load doors
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kNotVisible) || std::string_view(a_obj->name.c_str()).starts_with("DoorToBlack") || IsBackdrop(a_obj->name.c_str())) {
                ++collectStats.markers;
                Trace(a_obj, a_depth, "not visible");
                return;
            }
            if (Hidden(a_obj)) {
                ++collectStats.hidden;
                Trace(a_obj, a_depth, "hidden");
                return;
            }
            if (const auto node = a_obj->AsNode()) {
                // a switch shows one of its children; the others are never updated (no bound, a stale transform) and
                // decode into garbage - huge sheets
                const auto  switchNode = netimmerse_cast<RE::NiSwitchNode*>(node);
                const auto& children = node->GetChildren();
                Trace(a_obj, a_depth, std::format("{} children{}", children.size(), switchNode ? std::format(", a switch on {}", switchNode->index) : ""));
                // every slot of the array: its size() counts the children only, and empty slots may lie before them (a
                // mesh edited in NifSkope: the rest were never looked at)
                std::int32_t slot = -1;
                for (const auto& child : children) {
                    ++slot;
                    if (!child) {
                        continue;
                    }
                    if (switchNode && slot != switchNode->index) {
                        ++collectStats.hidden;
                        continue;
                    }
                    Collect(child.get(), a_out, a_depth + 1, a_owner);
                }
                return;
            }
            const auto geom = a_obj->AsGeometry();
            const auto tri = geom ? geom->AsTriShape() : nullptr;
            // never updated (the engine does not draw it): nothing to check its data against
            if (tri && a_obj->worldBound.radius <= 0.0f) {
                ++collectStats.hidden;
                Trace(a_obj, a_depth, "no bound");
                return;
            }
            if (!tri || netimmerse_cast<RE::BSDynamicTriShape*>(tri)) {
                ++collectStats.other;
                Trace(a_obj, a_depth, "not a static tri shape");
                return;
            }
            const auto& rd = geom->GetGeometryRuntimeData();
            if (rd.skinInstance || !rd.rendererData || !rd.shaderProperty) {
                ++collectStats.shader;
                Trace(a_obj, a_depth, "skinned, or no data or shader");
                return;
            }
            // the filter's words first: left off, or kept whatever the rules below say
            const auto rule = Filtered(a_owner, a_obj);
            if (rule && !rule->keep) {
                ++collectStats.filtered;
                Trace(a_obj, a_depth, std::format("the filter: '{}'", rule->word));
                return;
            }
            const bool kept = rule != nullptr;
            // water is taken too (drawn as water); the rest by the mod's rules (each a "@rule" of the filter)
            const bool water = (kept && rule->water) || netimmerse_cast<RE::BSWaterShaderProperty*>(rd.shaderProperty.get());
            if (!kept && !water && LeftOffByRules(rd.shaderProperty.get(), rd.alphaProperty.get())) {
                ++collectStats.shader;
                Trace(a_obj, a_depth, "a rule (effect shader, see-through, refraction, decal)");
                return;
            }
            // trees, bushes, grass: a canopy over the ground
            if (!kept && !water && Rule("foliage") && rd.alphaProperty && rd.alphaProperty->GetAlphaTesting() && IsFoliage(a_owner, rd.shaderProperty.get())) {
                ++collectStats.foliage;
                Trace(a_obj, a_depth, "foliage");
                return;
            }
            // small things (a plate, a cup, a coin purse): under a pixel on any map, yet often most of a city cell's triangles.
            // A small part of a thing big enough to show is kept while it is a fair share of it (a mannequin's helmet,
            // a statue's head); a building's nails and rivets still go
            const auto  whole = a_owner ? a_owner->Get3D() : nullptr;
            const float wholeRadius = whole ? whole->worldBound.radius : 0.0f;
            const bool  partOfMore = wholeRadius >= kTinyRadius * 2.0f && a_obj->worldBound.radius >= wholeRadius * 0.2f;
            if (!kept && !water && Rule("tiny") && a_obj->worldBound.radius < kTinyRadius && !partOfMore) {
                ++collectStats.tiny;
                Trace(a_obj, a_depth, "tiny");
                return;
            }
            Trace(a_obj, a_depth, "taken");
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
            const auto    step = std::max<std::uint32_t>(1, a_count / 65536);  // all of them: the farthest one says how far they reach
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

        std::atomic<int> failLogs = 0;  // details of meshes that do not decode, the first few of a session
        thread_local int rejectedIndex = 0, rejectedEdge = 0;  // triangles the last Decode on this thread threw away (bad index / too long)
        int readLogs = 0;  // details of GPU reads, the first few of a session
        int flatLandLogs = 0;  // placeholder land blocks left out (for the log)
        std::atomic<int> formatSwapped = 0;  // meshes whose vertex format flag was wrong (for the log)
        std::atomic<std::size_t> strayTriangles = 0;  // triangles dropped for a vertex outside the mesh bound (for the log)

        // decode one mesh into a_out - in world space, or (a_local) in its own (checked in the world all the same: the
        // shape's transform and bound say whether the data makes sense); false = it does not either way. Any thread
        bool Decode(const char* a_name, const std::uint8_t* a_data, std::uint32_t a_stride, std::uint32_t a_vertexCount, bool a_full, const std::uint16_t* a_indices,
            std::uint32_t a_triangleCount, const RE::NiTransform& a_world, const RE::NiBound& a_bound, Geometry& a_out, bool a_logFailure, bool a_local)
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
            // both fit: the mesh's bound was made from its true vertices, so they reach exactly its rim (cover 1) - a
            // misread set falls short of it or pokes out past it (a carved panel read as halves: 1.07, its float
            // reading 1.00); only where that does not tell them apart, the wider spread wins
            const float flaggedOff = std::abs(flagged.cover - 1.0f), otherOff = std::abs(other.cover - 1.0f);
            const bool  otherBetter = std::abs(flaggedOff - otherOff) > 0.02f ? otherOff < flaggedOff : other.spread > flagged.spread * 1.5f;
            if (!flagged.Good() || (other.Good() && otherBetter)) {
                a_full = !a_full;
                ++formatSwapped;
                a_out.swapped = true;
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
                const auto p = ReadPosition(a_data + static_cast<std::size_t>(v) * a_stride, a_full);
                const auto w = a_world * p;
                const auto s = a_local ? p : w;  // what is kept
                world[v] = w;
                a_out.verts.insert(a_out.verts.end(), { s.x, s.y, s.z });
                if (!std::isfinite(w.x) || !std::isfinite(w.y) || !std::isfinite(w.z) || (reach < FLT_MAX && w.GetDistance(a_bound.center) > reach)) {
                    stray[v] = 1;
                    continue;
                }
                a_out.minX = std::min(a_out.minX, s.x);
                a_out.minY = std::min(a_out.minY, s.y);
                a_out.maxX = std::max(a_out.maxX, s.x);
                a_out.maxY = std::max(a_out.maxY, s.y);
                a_out.minZ = std::min(a_out.minZ, s.z);
                a_out.maxZ = std::max(a_out.maxZ, s.z);
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
            if (const auto rule = Filtered(OwnerOf(a_shape), a_shape); rule && rule->water) {  // "+word = water" (a waterfall's mesh)
                return kWater;
            }
            return IsLand(a_shape) ? kLand : kObjects;
        }

        // an exterior cell by its grid square (an interior one by its form)
        constexpr RE::FormID ExteriorId(std::int32_t a_x, std::int32_t a_y)
        {
            return 0xFF000000u | ((static_cast<RE::FormID>(a_x + 2048) & 0xFFF) << 12) | (static_cast<RE::FormID>(a_y + 2048) & 0xFFF);
        }

        // is the square of exterior cell (x, y) within a_radius of the point
        bool CellWithin(std::int32_t a_x, std::int32_t a_y, const RE::NiPoint3& a_pos, float a_radius)
        {
            const float nx = std::clamp(a_pos.x, a_x * kCellSize, (a_x + 1) * kCellSize) - a_pos.x;
            const float ny = std::clamp(a_pos.y, a_y * kCellSize, (a_y + 1) * kCellSize) - a_pos.y;
            return nx * nx + ny * ny <= a_radius * a_radius;
        }

        // ---- models: found by what a mesh is - its name, counts, format and a sample of its data (the same mesh, by
        // another reference or loaded again after its cell came back, finds its model). Shared ones are odd keys, a
        // GPU read's own an even one
        std::uint64_t ModelKey(RE::BSTriShape* a_shape, int a_kind)
        {
            const auto&   rd = a_shape->GetGeometryRuntimeData();
            const auto&   counts = a_shape->GetTrishapeRuntimeData();
            const auto    data = rd.rendererData;
            const auto    stride = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(rd.vertexDesc) & 0xF) * 4;
            std::uint64_t h = std::hash<std::string_view>{}(a_shape->name.c_str());
            h = Mix(h ^ counts.vertexCount);
            h = Mix(h ^ counts.triangleCount);
            h = Mix(h ^ std::bit_cast<std::uint64_t>(rd.vertexDesc));
            h = Mix(h ^ static_cast<std::uint64_t>(a_kind));
            if (data && data->rawVertexData && stride >= 8) {
                const auto step = std::max<std::uint32_t>(1, counts.vertexCount / 16);
                for (std::uint32_t v = 0; v < counts.vertexCount; v += step) {
                    std::uint64_t word = 0;
                    std::memcpy(&word, data->rawVertexData + static_cast<std::size_t>(v) * stride, 8);
                    h = Mix(h ^ word);
                }
            }
            if (data && data->rawIndexData) {
                const auto step = std::max<std::uint32_t>(1, counts.triangleCount / 16);
                for (std::uint32_t t = 0; t < counts.triangleCount; t += step) {
                    std::uint64_t word = 0;
                    std::memcpy(&word, data->rawIndexData + static_cast<std::size_t>(t) * 3, 6);
                    h = Mix(h ^ word);
                }
            }
            return h | 1;
        }

        // ---- the worker: decodes the meshes kept in memory into their models' own space, the nearest first. The
        // shape is held meanwhile (its data with it) and let go on the main thread
        struct DecodeJob
        {
            std::uint64_t                 key = 0;
            RE::NiPointer<RE::BSTriShape> keep;
            const std::uint8_t*           data = nullptr;
            const std::uint16_t*          indices = nullptr;
            std::uint32_t                 stride = 0, vertexCount = 0, triangleCount = 0;
            bool                          full = false;
            RE::NiTransform               world;
            RE::NiBound                   bound;
            float                         distance = 0.0f;
            std::uint64_t                 generation = 0;
        };
        struct Decoded
        {
            std::uint64_t                 key = 0;
            RE::NiPointer<RE::BSTriShape> keep;
            Geometry                      geo;
            bool                          ok = false;
            std::uint64_t                 generation = 0;
        };
        struct Nearer
        {
            bool operator()(const DecodeJob& a, const DecodeJob& b) const { return a.distance > b.distance; }
        };
        std::mutex                                                          workLock;
        std::condition_variable                                             workWake;
        std::priority_queue<DecodeJob, std::vector<DecodeJob>, Nearer>      workQueue;
        std::vector<Decoded>                                                workDone;
        std::uint64_t                                                       workGeneration = 1;  // everything dropped: what was under way is thrown away

        void DecodeWorker()
        {
            for (;;) {
                DecodeJob job;
                {
                    std::unique_lock guard(workLock);
                    workWake.wait(guard, [] { return !workQueue.empty(); });
                    job = workQueue.top();
                    workQueue.pop();
                    if (job.generation != workGeneration) {
                        workDone.push_back({ job.key, std::move(job.keep), {}, false, job.generation });  // let go on the main thread
                        continue;
                    }
                }
                Decoded out{ job.key, std::move(job.keep), {}, false, job.generation };
                out.ok = Decode(out.keep ? out.keep->name.c_str() : "", job.data, job.stride, job.vertexCount, job.full, job.indices, job.triangleCount, job.world, job.bound,
                    out.geo, false, true);
                std::scoped_lock guard(workLock);
                workDone.push_back(std::move(out));
            }
        }

        void StartWorker()
        {
            static std::once_flag once;
            std::call_once(once, [] { std::thread(DecodeWorker).detach(); });
        }

        // main thread: the decoded models in
        void TakeDecoded()
        {
            std::vector<Decoded> done;
            {
                std::scoped_lock guard(workLock);
                done.swap(workDone);
            }
            if (done.empty()) {
                return;
            }
            std::scoped_lock guard(meshLock);
            for (auto& d : done) {
                const auto it = models.find(d.key);
                if (d.generation != workGeneration || it == models.end() || it->second->state != kDecoding) {
                    continue;  // dropped meanwhile
                }
                auto& m = *it->second;
                if (d.ok && d.geo.triangles > 0) {
                    m.extent[0] = d.geo.minX, m.extent[1] = d.geo.minY, m.extent[2] = d.geo.minZ;
                    m.extent[3] = d.geo.maxX, m.extent[4] = d.geo.maxY, m.extent[5] = d.geo.maxZ;
                    m.swapped = d.geo.swapped;
                    m.from = d.keep ? d.keep->name.c_str() : "";
                    m.geo = std::move(d.geo);
                    m.triangles = m.geo.triangles;
                    m.state = kReady;
                } else {
                    m.state = kFailed;
                }
            }
            ++geometryVersion;
        }  // the shapes held go with done

        // ---- placing: a shape's model (made when new) and an instance of it
        struct Placed
        {
            Model* model = nullptr;
            bool   world = false;  // read back from the GPU: already in world space
        };

        // main thread, under meshLock
        Placed ModelFor(RE::BSTriShape* a_shape, int a_kind, float a_distance)
        {
            const auto& rd = a_shape->GetGeometryRuntimeData();
            const auto  data = rd.rendererData;
            const auto& counts = a_shape->GetTrishapeRuntimeData();
            const auto  stride = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(rd.vertexDesc) & 0xF) * 4;
            const bool  noCounts = counts.vertexCount == 0 || counts.triangleCount == 0;  // a multi-part shape: the counts come from its GPU buffers
            if (!data || stride < 8 || (noCounts && (!data->vertexBuffer || !data->indexBuffer))) {
                return {};
            }
            const bool full = rd.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
            // its copy in memory: decoded on the worker into a shared model - checked here first (cheap), data that
            // fits its bound neither way is read back from the GPU instead
            if (!noCounts && a_kind == kObjects && data->rawVertexData && data->rawIndexData) {
                const auto key = ModelKey(a_shape, a_kind);
                if (const auto it = models.find(key); it != models.end()) {
                    return { it->second.get(), false };
                }
                if (Measure(data->rawVertexData, stride, counts.vertexCount, full, a_shape->world, a_shape->worldBound).Good() ||
                    Measure(data->rawVertexData, stride, counts.vertexCount, !full, a_shape->world, a_shape->worldBound).Good()) {
                    auto m = std::make_unique<Model>();
                    m->key = key;
                    m->kind = a_kind;
                    m->triangles = counts.triangleCount;
                    DecodeJob job;
                    job.key = key;
                    job.keep.reset(a_shape);
                    job.data = data->rawVertexData;
                    job.indices = data->rawIndexData;
                    job.stride = stride;
                    job.vertexCount = counts.vertexCount;
                    job.triangleCount = counts.triangleCount;
                    job.full = full;
                    job.world = a_shape->world;
                    job.bound = a_shape->worldBound;
                    job.distance = a_distance;
                    {
                        std::scoped_lock guard(workLock);
                        job.generation = workGeneration;
                        workQueue.push(std::move(job));
                    }
                    workWake.notify_one();
                    ++perf.models;
                    ++live.models;
                    return { models.emplace(key, std::move(m)).first->second.get(), false };
                }
            }
            if (!data->vertexBuffer || (!data->indexBuffer && !data->rawIndexData)) {
                return {};
            }
            // a model of its own, read back from the GPU in world space
            auto m = std::make_unique<Model>();
            m->key = (Mix(++gpuSerial) << 1) | 2;
            m->kind = a_kind;
            m->triangles = counts.triangleCount;
            GpuJob job;
            job.key = m->key;
            job.keep.reset(a_shape);
            job.world = a_shape->world;
            job.bound = a_shape->worldBound;
            job.stride = stride;
            job.vertexCount = counts.vertexCount;
            job.triangleCount = counts.triangleCount;
            job.full = full;
            job.vb = BufferRef(reinterpret_cast<ID3D11Buffer*>(data->vertexBuffer));
            if (!noCounts && data->rawIndexData) {
                job.rawIndices.assign(data->rawIndexData, data->rawIndexData + static_cast<std::size_t>(counts.triangleCount) * 3);
            } else {
                job.ib = BufferRef(reinterpret_cast<ID3D11Buffer*>(data->indexBuffer));
            }
            jobs.push_back(std::move(job));
            ++perf.models;
            ++live.models;
            const auto key = m->key;
            return { models.emplace(key, std::move(m)).first->second.get(), true };
        }

        // main thread, under meshLock: a shape's model placed
        std::uint64_t Place(const Placed& a_p, RE::BSTriShape* a_shape, RE::FormID a_owner, RE::FormID a_space)
        {
            Instance in;
            in.model = a_p.model;
            in.baked = a_p.world;
            a_p.model->lastUsed = pass;
            if (a_p.world) {
                in.row[0][0] = in.row[1][1] = in.row[2][2] = 1.0f;
            } else {
                const auto& t = a_shape->world;
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        in.row[i][j] = t.rotate.entry[i][j] * t.scale;
                    }
                }
                in.row[0][3] = t.translate.x;
                in.row[1][3] = t.translate.y;
                in.row[2][3] = t.translate.z;
            }
            const auto& b = a_shape->worldBound;
            in.box[0] = b.center.x - b.radius;
            in.box[1] = b.center.y - b.radius;
            in.box[2] = b.center.z - b.radius;
            in.box[3] = b.center.x + b.radius;
            in.box[4] = b.center.y + b.radius;
            in.box[5] = b.center.z + b.radius;
            in.owner = a_owner;
            in.space = a_space;
            in.triangles = a_shape->GetTrishapeRuntimeData().triangleCount;
            in.lastSeen = pass;
            in.serial = ++instanceSerial;
            std::uint32_t slot;
            if (freeSlots.empty()) {
                slot = static_cast<std::uint32_t>(instances.size());
                instances.push_back(in);
            } else {
                slot = freeSlots.back();
                freeSlots.pop_back();
                instances[slot] = in;
            }
            a_p.model->placed.push_back(slot);
            return (static_cast<std::uint64_t>(in.serial) << 32) | slot;
        }

        // main thread, under meshLock
        void Unplace(std::uint64_t a_handle)
        {
            const auto in = InstanceAt(a_handle);
            if (!in) {
                return;
            }
            const auto slot = static_cast<std::uint32_t>(a_handle & 0xFFFFFFFF);
            auto&      placed = in->model->placed;
            if (const auto it = std::ranges::find(placed, slot); it != placed.end()) {
                *it = placed.back();
                placed.pop_back();
            }
            in->model->lastUsed = pass;
            *in = {};
            freeSlots.push_back(slot);
        }

        // under meshLock: a model the pictures can draw now (or never will: nothing to wait for)
        bool Drawable(const Model* a_m)
        {
            return a_m->state == kFailed || a_m->vb != nullptr;
        }

        // a placed thing taken again (a reference's 3D loaded anew, a landscape block swapped) that was on the map
        // already does not fade in again: an old instance of the same model at the same place hands its time on
        void Inherit(const std::vector<std::uint64_t>& a_new, const std::vector<std::uint64_t>& a_old)  // under meshLock
        {
            for (const auto n : a_new) {
                auto& in = *InstanceAt(n);
                for (const auto o : a_old) {
                    const auto oldIn = InstanceAt(o);
                    if (!oldIn || oldIn->shownAt <= 0.0f || oldIn->model != in.model) {
                        continue;
                    }
                    const auto& old = *oldIn;
                    if (std::abs(old.row[0][3] - in.row[0][3]) < 1.0f && std::abs(old.row[1][3] - in.row[1][3]) < 1.0f && std::abs(old.row[2][3] - in.row[2][3]) < 1.0f) {
                        in.shownAt = old.shownAt;
                        break;
                    }
                }
            }
        }

        // ---- references: each one's meshes taken when its 3D comes in or changes - a cheap look at its tree (which
        // nodes and shapes, hidden or not, a switch's choice) says whether anything did
        std::uint64_t TreeHash(RE::NiAVObject* a_obj, int a_depth = 0)
        {
            if (!a_obj || a_depth > 64) {
                return 0;
            }
            std::uint64_t h = reinterpret_cast<std::uintptr_t>(a_obj);
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kNotVisible) || Hidden(a_obj)) {
                return Mix(h ^ 1);
            }
            if (const auto node = a_obj->AsNode()) {
                if (const auto sw = netimmerse_cast<RE::NiSwitchNode*>(node)) {
                    h = Mix(h ^ static_cast<std::uint64_t>(sw->index + 7));
                }
                for (const auto& child : node->GetChildren()) {
                    h = Mix(h + TreeHash(child.get(), a_depth + 1));
                }
                return h;
            }
            return Mix(h);
        }

        // what is never on the map: characters, markers and primitives, loose items and whatever the game made at
        // runtime (they come and go)
        bool NeverOnMap(RE::TESObjectREFR* a_ref)
        {
            if (a_ref->Is(RE::FormType::ActorCharacter) || a_ref->IsDynamicForm() || a_ref->extraList.HasType<RE::ExtraPrimitive>()) {
                return true;
            }
            const auto base = a_ref->GetBaseObject();
            return !base || base->IsMarker() || IsLooseItem(base) || IsBackdrop(ModelOf(a_ref) ? ModelOf(a_ref) : "");
        }

        struct RefRec
        {
            RE::NiAVObject*            root = nullptr;
            std::uint64_t              hash = 0;
            std::vector<std::uint64_t> placed;
            std::vector<std::uint64_t> leaving;  // the instances it had, on the map until the new ones can be drawn
            std::vector<RE::BSTriShape*> shapes;  // placed[i] came from shapes[i] (alive while the tree hashes the same)
            std::vector<RE::NiTransform> worlds;  // ...where it was then
        };
        std::unordered_map<RE::FormID, RefRec> refRecs;   // main thread
        std::unordered_set<RE::FormID>         refsLeaving; // ...those with old instances still up

        void ForgetRef(RE::FormID a_id)
        {
            const auto it = refRecs.find(a_id);
            if (it == refRecs.end()) {
                return;
            }
            {
                std::scoped_lock guard(meshLock);
                for (const auto i : it->second.placed) {
                    Unplace(i);
                }
                for (const auto i : it->second.leaving) {
                    Unplace(i);
                }
            }
            refRecs.erase(it);
            refsLeaving.erase(a_id);
            ++geometryVersion;
        }

        // the detailed log: for a reference not all of whose meshes made it onto the map (or a big one: a building), once
        // per model - what each mesh is and why it was left out
        std::unordered_set<std::string> diagnosed;

        void CountMeshes(RE::NiAVObject* a_obj, int& a_count, int a_depth = 0)
        {
            if (!a_obj || a_depth > 64) {
                return;
            }
            if (const auto node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    CountMeshes(child.get(), a_count, a_depth + 1);
                }
            } else if (a_obj->AsGeometry()) {
                ++a_count;
            }
        }

        void ListMeshes(RE::NiAVObject* a_obj, std::string& a_out, int a_depth = 0)
        {
            if (!a_obj || a_depth > 64) {
                return;
            }
            const auto flags = a_obj->GetFlags().underlying();
            if (const auto node = a_obj->AsNode()) {
                a_out += std::format("\n    node '{}' flags {:08X}{}", a_obj->name.c_str(), flags, a_obj->GetUserData() ? " (a reference)" : "");
                for (const auto& child : node->GetChildren()) {
                    ListMeshes(child.get(), a_out, a_depth + 1);
                }
                return;
            }
            const auto geom = a_obj->AsGeometry();
            if (!geom) {
                return;
            }
            const auto tri = geom->AsTriShape();
            const auto& rd = geom->GetGeometryRuntimeData();
            const auto  data = rd.rendererData;
            std::string what = std::format("\n    mesh '{}' flags {:08X} r {:.0f}", a_obj->name.c_str(), flags, a_obj->worldBound.radius);
            if (!tri) {
                what += " - not a tri shape";
            } else {
                const auto& c = tri->GetTrishapeRuntimeData();
                const auto  stride = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(rd.vertexDesc) & 0xF) * 4;
                what += std::format(" verts {} tris {} stride {} data {} indices {} vb {} skin {} shader {} alpha {}", c.vertexCount, c.triangleCount, stride,
                    data && data->rawVertexData ? "yes" : "no", data && data->rawIndexData ? "yes" : "no", data && data->vertexBuffer ? "yes" : "no", rd.skinInstance ? "yes" : "no",
                    rd.shaderProperty ? "yes" : "none", rd.alphaProperty ? (rd.alphaProperty->GetAlphaBlending() ? "blend" : rd.alphaProperty->GetAlphaTesting() ? "test" : "off") : "none");
                if (data && data->rawVertexData && c.vertexCount && stride >= 8) {
                    const bool full = rd.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
                    const auto f = Measure(data->rawVertexData, stride, c.vertexCount, full, tri->world, tri->worldBound);
                    const auto o = Measure(data->rawVertexData, stride, c.vertexCount, !full, tri->world, tri->worldBound);
                    what += std::format(" fit {}: inside {:.2f} cover {:.2f} spread {:.2f}, other: inside {:.2f} cover {:.2f} spread {:.2f}", full ? "float" : "half", f.inside,
                        f.cover, f.spread, o.inside, o.cover, o.spread);
                }
            }
            a_out += what;
        }

        void Diagnose(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root, std::size_t a_taken, std::size_t a_placed, const CollectStats& a_left)
        {
            int meshes = 0;
            CountMeshes(a_root, meshes);
            const bool big = a_root->worldBound.radius > 1500.0f;
            if ((a_placed >= static_cast<std::size_t>(meshes) && !big) || diagnosed.size() >= 300) {
                return;
            }
            const auto path = ModelOf(a_ref);
            const std::string key = path ? path : std::format("{:08X}", a_ref->GetFormID());
            if (!diagnosed.insert(key).second) {
                return;
            }
            std::string list;
            ListMeshes(a_root, list);
            std::string                  trace;
            std::vector<RE::BSTriShape*> again;
            const auto                   stats = collectStats;
            collectTrace = &trace;
            Collect(a_root, again, 0);
            collectTrace = nullptr;
            collectStats = stats;
            list += "\n    collected again, step by step:" + trace;
            logger::info("map 3d: '{}' ({:08X}, r {:.0f}): {} meshes, {} taken, {} placed - left out: hidden {}, filtered {}, tiny {}, foliage {}, shader {}, other {}, "
                         "markers {}, items {}, actors {}{}",
                key, a_ref->GetFormID(), a_root->worldBound.radius, meshes, a_taken, a_placed, a_left.hidden, a_left.filtered, a_left.tiny, a_left.foliage, a_left.shader,
                a_left.other, a_left.markers, a_left.items, a_left.actors, list);
        }

        // a shape moved from where it was (a door swung, a gate let down): more than a hair
        bool Moved(const RE::NiTransform& a_was, const RE::NiTransform& a_now)
        {
            if (std::abs(a_was.translate.x - a_now.translate.x) > 0.5f || std::abs(a_was.translate.y - a_now.translate.y) > 0.5f ||
                std::abs(a_was.translate.z - a_now.translate.z) > 0.5f || std::abs(a_was.scale - a_now.scale) > 0.001f) {
                return true;
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    if (std::abs(a_was.rotate.entry[i][j] - a_now.rotate.entry[i][j]) > 0.002f) {
                        return true;
                    }
                }
            }
            return false;
        }

        // under meshLock: the instances of a reference whose tree is unchanged follow its shapes where they moved - its
        // place written anew, nothing taken again (false: one baked in world space moved, it is taken again)
        bool Follow(RefRec& a_rec, bool& a_moved)
        {
            for (std::size_t k = 0; k < a_rec.placed.size(); ++k) {
                const auto in = InstanceAt(a_rec.placed[k]);
                if (!in) {
                    continue;
                }
                in->lastSeen = pass;
                if (k >= a_rec.shapes.size()) {
                    continue;
                }
                const auto  shape = a_rec.shapes[k];
                const auto& t = shape->world;
                if (!Moved(a_rec.worlds[k], t)) {
                    continue;
                }
                if (in->baked) {
                    return false;
                }
                a_rec.worlds[k] = t;
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        in->row[i][j] = t.rotate.entry[i][j] * t.scale;
                    }
                }
                in->row[0][3] = t.translate.x;
                in->row[1][3] = t.translate.y;
                in->row[2][3] = t.translate.z;
                const auto& b = shape->worldBound;
                in->box[0] = b.center.x - b.radius;
                in->box[1] = b.center.y - b.radius;
                in->box[2] = b.center.z - b.radius;
                in->box[3] = b.center.x + b.radius;
                in->box[4] = b.center.y + b.radius;
                in->box[5] = b.center.z + b.radius;
                a_moved = true;
            }
            return true;
        }

        // main thread: a reference looked at - taken again when its 3D changed, moved along when it moved, let go when
        // it is switched off; its 3D merely unloaded (left behind) it stays on the map. True: something changed
        bool TakeRef(RE::TESObjectREFR* a_ref, RE::FormID a_space, const RE::NiPoint3& a_player)
        {
            const auto id = a_ref->GetFormID();
            if (NeverOnMap(a_ref) || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->IsMarkedForDeletion()) {
                const bool had = refRecs.contains(id);
                ForgetRef(id);
                return had;
            }
            const auto root = a_ref->Get3D();
            const auto rec = refRecs.find(id);
            if (!root) {
                return false;
            }
            const auto hash = TreeHash(root);
            if (rec != refRecs.end() && rec->second.root == root && rec->second.hash == hash) {
                bool moved = false;
                bool kept;
                {
                    std::scoped_lock guard(meshLock);
                    kept = Follow(rec->second, moved);
                }
                if (kept) {
                    if (moved) {
                        ++geometryVersion;
                    }
                    return moved;
                }
            }
            std::vector<RE::BSTriShape*> shapes;
            collectStats = {};
            Collect(root, shapes, 0);
            const auto                 left = collectStats;
            const float                distance = a_ref->GetPosition().GetDistance(a_player);
            std::vector<std::uint64_t>   placed;
            std::vector<RE::BSTriShape*> from;
            std::vector<RE::NiTransform> worlds;
            auto&                        r = refRecs[id];
            {
                std::scoped_lock guard(meshLock);
                for (const auto shape : shapes) {
                    if (const auto p = ModelFor(shape, KindOf(shape), distance); p.model) {
                        placed.push_back(Place(p, shape, id, a_space));
                        from.push_back(shape);
                        worlds.push_back(shape->world);
                    }
                }
                Inherit(placed, r.placed);
                // what it had stays up until the new instances can be drawn (a mesh read back from the GPU takes a few
                // frames: no blink meanwhile); an older set still waiting goes now
                for (const auto i : r.leaving) {
                    Unplace(i);
                }
                r.leaving = std::move(r.placed);
                if (!r.leaving.empty()) {
                    refsLeaving.insert(id);
                }
            }
            if (Settings::Map().debugLog) {
                Diagnose(a_ref, root, shapes.size(), placed.size(), left);
            }
            r.root = root;
            r.hash = hash;
            r.placed = std::move(placed);
            r.shapes = std::move(from);
            r.worlds = std::move(worlds);
            ++perf.refs;
            ++live.refs;
            ++geometryVersion;
            return true;
        }

        // ---- a cell's own shapes, no reference's: the landscape, water planes and a few others. Taken by part (the
        // landscape by its block and place); a part gone leaves only once the new ones are read, so the ground never
        // shows a hole while a block is swapped for another
        // the game's distant land and water (its LOD, what it draws past the loaded cells): a stand-in ground under what is
        // seen far off - big rocks and buildings load in further than the cells round them. Drawn where no cell's own
        // landscape is yet; that one fades in over it as its cell comes
        constexpr RE::FormID kLodLand = 0xFE7FFF01, kLodWater = 0xFE7FFF02;  // their owners (no form has these)

        struct LooseRec
        {
            std::uint64_t                                   hash = 0;
            std::unordered_map<std::uint64_t, std::uint64_t> parts;    // part -> instance
            std::vector<std::uint64_t>                      leaving;  // replaced, until the new parts are in
        };
        std::unordered_map<RE::FormID, LooseRec> looseRecs;  // main thread, by cell

        // the shapes under a cell's tree that belong to no reference (a reference's subtree is skipped whole)
        void CollectLoose(RE::NiAVObject* a_obj, std::vector<RE::BSTriShape*>& a_out, int a_depth)
        {
            if (!a_obj || a_depth > 64 || a_obj->GetUserData()) {
                return;
            }
            const auto node = a_obj->AsNode();
            if (!node) {
                Collect(a_obj, a_out, a_depth);
                return;
            }
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kNotVisible) || Hidden(a_obj)) {
                return;
            }
            const auto  sw = netimmerse_cast<RE::NiSwitchNode*>(node);
            const auto& children = node->GetChildren();
            std::int32_t slot = -1;  // every slot (see Collect)
            for (const auto& child : children) {
                ++slot;
                if (child && (!sw || slot == sw->index)) {
                    CollectLoose(child.get(), a_out, a_depth + 1);
                }
            }
        }

        std::uint64_t LooseHash(RE::NiAVObject* a_obj, int a_depth)
        {
            if (!a_obj || a_depth > 64 || a_obj->GetUserData()) {
                return 0;
            }
            std::uint64_t h = reinterpret_cast<std::uintptr_t>(a_obj);
            if (a_obj->GetFlags().any(RE::NiAVObject::Flag::kNotVisible) || Hidden(a_obj)) {
                return Mix(h ^ 1);
            }
            if (const auto node = a_obj->AsNode()) {
                if (const auto sw = netimmerse_cast<RE::NiSwitchNode*>(node)) {
                    h = Mix(h ^ static_cast<std::uint64_t>(sw->index + 7));
                }
                for (const auto& child : node->GetChildren()) {
                    h = Mix(h + LooseHash(child.get(), a_depth + 1));
                }
            }
            return Mix(h);
        }

        // a loose shape's part: the landscape by its block and place, anything else by its name, counts and place
        std::uint64_t PartKey(RE::BSTriShape* a_shape)
        {
            const auto& c = a_shape->worldBound.center;
            return Mix(ShapeHash(a_shape) ^ Mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(c.x / 64.0f))) * 31 +
                                                static_cast<std::uint64_t>(static_cast<std::int64_t>(std::floor(c.y / 64.0f)))));
        }

        bool IsLodLand(const RE::NiAVObject* a_obj)
        {
            return std::string_view(a_obj->name.c_str()).starts_with("Land");
        }

        // the trees' distant look: the game's tree LOD, DynDOLOD's ("treepassthru" billboards, "objpassthru" 3D trees) -
        // never on the map
        bool IsLodTree(RE::BSGeometry* a_geom)
        {
            std::string name = a_geom->name.c_str();
            std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.contains("tree") || name.contains("passthru")) {
                return true;
            }
            if (const auto rtti = a_geom->GetRTTI(); rtti && rtti->GetName() && std::string_view(rtti->GetName()).contains("Tree")) {
                return true;
            }
            const auto& rd = a_geom->GetGeometryRuntimeData();
            return rd.shaderProperty && rd.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
        }

        void CollectLod(RE::NiAVObject* a_obj, std::vector<RE::BSTriShape*>& a_out, bool a_hidden, int a_depth)
        {
            if (!a_obj || a_depth > 64) {
                return;
            }
            const bool off = a_hidden || a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden, RE::NiAVObject::Flag::kNotVisible);
            if (const auto node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    CollectLod(child.get(), a_out, off, a_depth + 1);
                }
                return;
            }
            const auto geom = a_obj->AsGeometry();
            const auto tri = geom ? geom->AsTriShape() : nullptr;
            if (!tri) {
                return;
            }
            if (IsLodLand(tri)) {
                if (tri->worldBound.radius > 0.0f && geom->GetGeometryRuntimeData().rendererData) {
                    a_out.push_back(tri);  // shown or not
                }
            } else if (!off && !IsLodTree(tri)) {
                Collect(tri, a_out, a_depth);
            }
        }

        std::uint64_t LodHash(RE::NiAVObject* a_obj, bool a_hidden, int a_depth)
        {
            if (!a_obj || a_depth > 64) {
                return 0;
            }
            const bool    off = a_hidden || a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden, RE::NiAVObject::Flag::kNotVisible);
            std::uint64_t h = reinterpret_cast<std::uintptr_t>(a_obj);
            if (const auto node = a_obj->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    h = Mix(h + LodHash(child.get(), off, a_depth + 1));
                }
                return Mix(h);
            }
            return IsLodLand(a_obj) || !off ? Mix(h) : 0;  // land: there or not; an object: shown or not
        }

        // main thread: a cell's loose shapes looked at - a_water: the water planes lying in it (they hang under the
        // water system, not under a cell)
        void TakeLoose(RE::FormID a_cell, RE::NiAVObject* a_root, const std::vector<RE::BSTriShape*>& a_water, RE::FormID a_space, const RE::NiPoint3& a_player, int a_kind = -1)
        {
            const bool    lod = a_cell == kLodLand;
            std::uint64_t hash = lod ? LodHash(a_root, false, 0) : LooseHash(a_root, 0);
            for (const auto w : a_water) {
                hash = Mix(hash + reinterpret_cast<std::uintptr_t>(w));
            }
            auto& rec = looseRecs[a_cell];
            if (rec.hash == hash && hash != 0) {
                for (const auto& [part, i] : rec.parts) {
                    if (const auto in = InstanceAt(i)) {
                        in->lastSeen = pass;
                    }
                }
                return;
            }
            rec.hash = hash;
            std::vector<RE::BSTriShape*> shapes;
            if (lod) {
                CollectLod(a_root, shapes, false, 0);
            } else {
                CollectLoose(a_root, shapes, 0);
            }
            for (const auto w : a_water) {
                Collect(w, shapes, 0);
            }
            std::unordered_map<std::uint64_t, std::uint64_t> parts;
            std::scoped_lock                                 guard(meshLock);
            for (const auto shape : shapes) {
                const auto key = PartKey(shape);
                if (parts.contains(key)) {
                    continue;
                }
                if (const auto it = rec.parts.find(key); it != rec.parts.end()) {
                    parts.emplace(key, it->second);  // the same as before
                    rec.parts.erase(it);
                    continue;
                }
                const auto distance = shape->worldBound.center.GetDistance(a_player);
                const int kind = lod ? (IsLodLand(shape) ? kLand : kObjects) : a_kind >= 0 ? a_kind : KindOf(shape);
                if (const auto p = ModelFor(shape, kind, distance); p.model) {
                    parts.emplace(key, Place(p, shape, a_cell, a_space));
                }
            }
            for (const auto& [key, i] : rec.parts) {
                // a distant land block once read stays: the game hides (and lets go of) those out of the camera's sight -
                // the map keeps them all round, the finer lying over the coarser
                if (const auto in = InstanceAt(i); lod && in && in->model->kind == kLand) {
                    parts.emplace(key, i);
                    continue;
                }
                rec.leaving.push_back(i);
            }
            rec.parts = std::move(parts);
            ++geometryVersion;
        }

        // main thread: the parts replaced go once every new part of their cell is read (or could not be); a new one
        // where an old one of its kind lay takes its time on (no fade in again)
        void SettleLoose()
        {
            std::scoped_lock guard(meshLock);
            for (auto it = refsLeaving.begin(); it != refsLeaving.end();) {
                const auto rec = refRecs.find(*it);
                if (rec != refRecs.end()) {
                    const bool waiting = std::ranges::any_of(rec->second.placed, [](std::uint64_t a_h) {
                        const auto in = InstanceAt(a_h);
                        return in && !Drawable(in->model);
                    });
                    if (waiting) {
                        ++it;
                        continue;
                    }
                    for (const auto i : rec->second.leaving) {
                        Unplace(i);
                    }
                    rec->second.leaving.clear();
                    ++geometryVersion;
                }
                it = refsLeaving.erase(it);
            }
            for (auto& [cell, rec] : looseRecs) {
                if (rec.leaving.empty()) {
                    continue;
                }
                const bool waiting = std::ranges::any_of(rec.parts, [](const auto& a_p) {
                    const auto in = InstanceAt(a_p.second);
                    return in && !Drawable(in->model);
                });
                if (waiting) {
                    continue;
                }
                for (const auto& [key, n] : rec.parts) {
                    const auto newIn = InstanceAt(n);
                    if (!newIn || newIn->shownAt > 0.0f) {
                        continue;
                    }
                    auto& in = *newIn;
                    for (const auto o : rec.leaving) {
                        const auto oldIn = InstanceAt(o);
                        if (!oldIn || oldIn->shownAt <= 0.0f || oldIn->model->kind != in.model->kind) {
                            continue;
                        }
                        const auto& b = oldIn->box;
                        const float w = std::min(b[3], in.box[3]) - std::max(b[0], in.box[0]);
                        const float h = std::min(b[4], in.box[4]) - std::max(b[1], in.box[1]);
                        const float area = std::min((b[3] - b[0]) * (b[4] - b[1]), (in.box[3] - in.box[0]) * (in.box[4] - in.box[1]));
                        if (w > 0.0f && h > 0.0f && area > 0.0f && w * h >= area * 0.5f) {
                            in.shownAt = oldIn->shownAt;
                            break;
                        }
                    }
                }
                for (const auto o : rec.leaving) {
                    Unplace(o);
                }
                rec.leaving.clear();
                ++geometryVersion;
            }
        }

        // ---- what is looked at, and when: the map is still unless the game says otherwise. A reference whose 3D the
        // game loaded is taken at once and looked at again a few times (its tree may still be filling in); one
        // activated (a door, a lever) is followed a while with whatever can move near it (the gate the lever lets
        // down); a cell come in is gone over at once and twice more. A pass over everything loaded only now and then
        // (kPassEvery): what changes with no word from the game (a part shown or hidden by a quest) - spread over
        // frames like the rest
        constexpr float kPassEvery = 45.0f;     // seconds between the passes over everything loaded
        constexpr float kCellsEvery = 0.25f;    // the loaded cells looked at this often (which came in)
        constexpr float kWatchStep = 0.25f;     // a reference followed is looked at this often...
        constexpr int   kWatchLoaded = 8;       // ...this many times after its 3D loaded (or after it last moved)
        constexpr int   kWatchActivated = 24;   // ...after it, or something near it, was activated
        constexpr float kActivatedNear = 2000.0f;  // what can move near a thing activated

        struct Watch
        {
            float next = 0.0f;  // Now() it is looked at
            int   left = 0;     // times still
        };

        struct Sweep
        {
            RE::FormID                          space = 0;
            std::vector<RE::FormID>             known;   // the loaded cells (outside also the world space's persistent one)
            std::unordered_set<RE::NiAVObject*> roots;   // their trees
            std::vector<RE::FormID>             cells;   // to go over: cells[cell] on, a frame's share at a time
            std::size_t                         cell = 0, ref = 0;
            bool                                looseDone = false;
            std::vector<std::pair<float, RE::FormID>> again;    // a cell come in, gone over again at that Now()
            std::unordered_map<RE::FormID, Watch>     watched;  // references followed
            float                                     nextPass = 0.0f, nextCells = 0.0f;
        };
        Sweep sweep;

        float Now();

        // a cell to go over (once, however often asked before it is)
        void Queue(RE::FormID a_cell)
        {
            if (sweep.cell >= sweep.cells.size()) {
                sweep.cells.clear();
                sweep.cell = sweep.ref = 0;
                sweep.looseDone = false;
            }
            if (std::find(sweep.cells.begin() + static_cast<std::ptrdiff_t>(sweep.cell), sweep.cells.end(), a_cell) == sweep.cells.end()) {
                sweep.cells.push_back(a_cell);
            }
        }

        void Track(RE::FormID a_ref, int a_times)
        {
            auto& w = sweep.watched[a_ref];
            if (w.left < a_times) {
                w.left = a_times;
            }
            if (w.next <= 0.0f) {
                w.next = Now() + kWatchStep;
            }
        }

        std::mutex              eventLock;
        std::vector<RE::FormID> loadedRefs;     // 3D came or went (the game's event; any thread)
        std::vector<RE::FormID> activatedRefs;  // activated (the game's event; any thread)

        struct LoadSink : RE::BSTEventSink<RE::TESObjectLoadedEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                if (a_event) {
                    std::scoped_lock guard(eventLock);
                    if (loadedRefs.size() < 50'000) {  // the map hidden a long while: the cells are gone over anyway
                        loadedRefs.push_back(a_event->formID);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        struct ActivateSink : RE::BSTEventSink<RE::TESActivateEvent>
        {
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (a_event && a_event->objectActivated) {
                    std::scoped_lock guard(eventLock);
                    if (activatedRefs.size() < 1'000) {
                        activatedRefs.push_back(a_event->objectActivated->GetFormID());
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void ListenForLoads()
        {
            static LoadSink     loads;
            static ActivateSink activations;
            static bool         added = false;
            if (!added) {
                if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
                    events->AddEventSink<RE::TESObjectLoadedEvent>(&loads);
                    events->AddEventSink<RE::TESActivateEvent>(&activations);
                    added = true;
                }
            }
        }

        // what can move by itself once set going: doors, gates and levers, platforms, chests' lids
        bool CanMove(RE::TESObjectREFR* a_ref)
        {
            const auto base = a_ref->GetBaseObject();
            if (!base) {
                return false;
            }
            switch (base->GetFormType()) {
            case RE::FormType::Door:
            case RE::FormType::Activator:
            case RE::FormType::TalkingActivator:
            case RE::FormType::MovableStatic:
            case RE::FormType::Furniture:
            case RE::FormType::Container:
                return true;
            default:
                return false;
            }
        }

        std::vector<RE::TESObjectCELL*> LoadedCells(RE::PlayerCharacter* a_player)
        {
            std::vector<RE::TESObjectCELL*> out;
            const auto                      here = a_player->GetParentCell();
            if (!here) {
                return out;
            }
            if (here->IsInteriorCell()) {
                out.push_back(here);
            } else if (const auto tes = RE::TES::GetSingleton(); tes && tes->gridCells) {
                const auto grid = tes->gridCells;
                for (std::uint32_t x = 0; x < grid->length; ++x) {
                    for (std::uint32_t y = 0; y < grid->length; ++y) {
                        if (const auto cell = grid->GetCell(x, y); cell && cell->IsAttached()) {
                            out.push_back(cell);
                        }
                    }
                }
            }
            return out;
        }

        RE::FormID CellId(RE::TESObjectCELL* a_cell)
        {
            if (a_cell->IsInteriorCell()) {
                return a_cell->GetFormID();
            }
            const auto coords = a_cell->GetCoordinates();
            return coords ? ExteriorId(coords->cellX, coords->cellY) : a_cell->GetFormID();
        }

        // main thread, under meshLock: what was left behind goes once too much is placed (the oldest first), and models
        // no longer placed once they hold too much GPU memory
        void Evict()
        {
            std::size_t total = 0;
            for (const auto& in : instances) {
                total += in.model ? in.triangles : 0;
            }
            if (total > kKeptTriangles) {
                std::vector<std::pair<std::uint64_t, std::uint64_t>> old;  // last seen, handle
                for (std::uint32_t slot = 1; slot < instances.size(); ++slot) {
                    const auto& in = instances[slot];
                    if (in.model && in.lastSeen + 1 < pass) {
                        old.emplace_back(in.lastSeen, (static_cast<std::uint64_t>(in.serial) << 32) | slot);
                    }
                }
                std::ranges::sort(old);
                for (const auto& [seen, handle] : old) {
                    if (total <= kKeptTriangles) {
                        break;
                    }
                    total -= InstanceAt(handle)->triangles;
                    Unplace(handle);
                }
                ++geometryVersion;
            }
            std::size_t bytes = 0;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> unused;
            for (const auto& [key, m] : models) {
                if (m->placed.empty()) {
                    bytes += m->bytes;
                    unused.emplace_back(m->lastUsed, key);
                }
            }
            if (bytes <= kModelBytes) {
                return;
            }
            std::ranges::sort(unused);
            for (const auto& [used, key] : unused) {
                if (bytes <= kModelBytes) {
                    break;
                }
                auto& m = *models[key];
                bytes -= m.bytes;
                for (const auto p : { m.vb, m.ib }) {
                    if (p) {
                        releaseQueue.push_back(p);
                    }
                }
                models.erase(key);
            }
        }

        // the detailed log: the reference picked in the console (a click with the console open) - all the map knows of
        // it: taken or not and why, each instance, its model's state and size, where it is placed
        RE::FormID inspected = 0;
        std::atomic<RE::FormID> highlighted = 0;  // ...drawn red on the map (the render thread reads it)

        void Inspect(RE::TESObjectREFR* a_ref)
        {
            const auto  id = a_ref->GetFormID();
            const auto  root = a_ref->Get3D();
            const auto  base = a_ref->GetBaseObject();
            const auto  path = ModelOf(a_ref);
            const auto  cell = a_ref->GetParentCell();
            std::string out = std::format("map 3d: inspect {:08X} '{}' base {:08X} type {} cell {:08X}{}; never on the map {}, disabled {}, deleted {}, 3D {}", id,
                path ? path : "", base ? base->GetFormID() : 0, base ? static_cast<int>(base->GetFormType()) : -1, cell ? cell->GetFormID() : 0,
                cell && cell->IsAttached() ? " (attached)" : " (not attached)", NeverOnMap(a_ref), a_ref->IsDisabled(), a_ref->IsDeleted(), root ? "loaded" : "none");
            const auto rec = refRecs.find(id);
            if (rec == refRecs.end()) {
                out += "\n    not taken (no record)";
            } else {
                const auto& r = rec->second;
                out += std::format("\n    record: root {} (now {}), hash {:016X} (now {:016X}), {} placed, {} leaving, followed {}", static_cast<const void*>(r.root),
                    static_cast<const void*>(root), r.hash, root ? TreeHash(root) : 0, r.placed.size(), r.leaving.size(), sweep.watched.contains(id));
                std::scoped_lock guard(meshLock);
                for (const auto h : r.placed) {
                    const auto in = InstanceAt(h);
                    if (!in) {
                        out += std::format("\n    instance {:016X}: gone", h);
                        continue;
                    }
                    const auto m = in->model;
                    out += std::format("\n    instance {:016X}: at {:.0f} {:.0f} {:.0f}, box z {:.0f}..{:.0f}, scale row {:.2f} {:.2f} {:.2f}, space {:08X}, shown at {:.1f}, baked {}; "
                                       "model {:016X} kind {} state {} triangles {} (geo {}) bytes {} on GPU {} placed {}",
                        h, in->row[0][3], in->row[1][3], in->row[2][3], in->box[2], in->box[5], in->row[0][0], in->row[1][1], in->row[2][2], in->space, in->shownAt, in->baked,
                        m->key, m->kind, m->state, m->triangles, m->geo.triangles, m->bytes, m->vb != nullptr, m->placed.size());
                    out += std::format(" - decoded from '{}' {}, extent x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f}", m->from, m->swapped ? "swapped" : "as flagged", m->extent[0], m->extent[3], m->extent[1], m->extent[4], m->extent[2], m->extent[5]);
                }
            }
            if (root) {
                std::string                  list;
                std::string                  trace;
                std::vector<RE::BSTriShape*> again;
                ListMeshes(root, list);
                const auto stats = collectStats;
                collectTrace = &trace;
                Collect(root, again, 0);
                collectTrace = nullptr;
                collectStats = stats;
                out += list + "\n    collected now, step by step:" + trace;
                // each mesh decoded again here, as the worker would: what is kept of it
                for (const auto shape : again) {
                    const auto& rd = shape->GetGeometryRuntimeData();
                    const auto& c = shape->GetTrishapeRuntimeData();
                    const auto  data = rd.rendererData;
                    const auto  stride = static_cast<std::uint32_t>(std::bit_cast<std::uint64_t>(rd.vertexDesc) & 0xF) * 4;
                    if (!data || !data->rawVertexData || !data->rawIndexData || stride < 8 || !c.vertexCount) {
                        out += std::format("\n    decode '{}': no data in memory (read back from the GPU)", shape->name.c_str());
                        continue;
                    }
                    Geometry   geo;
                    const auto swappedBefore = formatSwapped.load();
                    const auto strayBefore = strayTriangles.load();
                    const bool ok = Decode(shape->name.c_str(), data->rawVertexData, stride, c.vertexCount, rd.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
                        data->rawIndexData, c.triangleCount, shape->world, shape->worldBound, geo, false, true);
                    out += std::format("\n    decode (key {:016X}) '{}': {}, format {}, {} of {} triangles kept (bad index {}, too long {}, a vertex out of the bound {}), extent x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f}",
                        ModelKey(shape, KindOf(shape)), shape->name.c_str(), ok ? "ok" : "FAILED", formatSwapped.load() != swappedBefore ? "swapped" : "as flagged", geo.triangles, c.triangleCount, rejectedIndex,
                        rejectedEdge, strayTriangles.load() - strayBefore, geo.minX, geo.maxX, geo.minY, geo.maxY, geo.minZ, geo.maxZ);
                    if (geo.triangles <= 200) {  // a small mesh: every triangle, in its own space
                        for (std::size_t t = 0; t + 2 < geo.indices.size(); t += 3) {
                            const auto v = [&](std::uint32_t a_i) { return std::format("({:.0f} {:.0f} {:.0f})", geo.verts[a_i * 3], geo.verts[a_i * 3 + 1], geo.verts[a_i * 3 + 2]); };
                            out += std::format("\n        tri {}: {} {} {}", t / 3, v(geo.indices[t]), v(geo.indices[t + 1]), v(geo.indices[t + 2]));
                        }
                    }
                }
            }
            // what stands near it that the map does not show (or shows only in part): the piece missing may be another
            if (cell) {
                const auto at = a_ref->GetPosition();
                cell->ForEachReference([&](RE::TESObjectREFR* a_near) {
                    if (!a_near || a_near == a_ref || a_near->GetPosition().GetDistance(at) > 700.0f || !a_near->Get3D() || NeverOnMap(a_near)) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    int meshes = 0;
                    CountMeshes(a_near->Get3D(), meshes);
                    const auto   it = refRecs.find(a_near->GetFormID());
                    const size_t placed = it == refRecs.end() ? 0 : it->second.placed.size();
                    if (it == refRecs.end() || placed == 0) {
                        const auto p = ModelOf(a_near);
                        out += std::format("\n    near, not on the map: {:08X} '{}' ({} meshes{}{})", a_near->GetFormID(), p ? p : "", meshes, it == refRecs.end() ? ", not taken" : ", nothing placed",
                            a_near->IsDisabled() ? ", disabled" : "");
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            logger::info("{}", out);
        }

        // main thread: the loaded cells looked at - those come in gone over at once (and again in a moment: a cell's
        // landscape and water come a little after it)
        void LookAtCells(RE::PlayerCharacter* a_player, float a_now)
        {
            std::vector<RE::FormID> now;
            sweep.roots.clear();
            for (const auto cell : LoadedCells(a_player)) {
                now.push_back(cell->GetFormID());
                if (const auto data = cell->GetRuntimeData().loadedData; data && data->cell3D) {
                    sweep.roots.insert(data->cell3D.get());
                }
            }
            if (const auto here = a_player->GetParentCell(); here && !here->IsInteriorCell()) {
                if (const auto world = a_player->GetWorldspace(); world && world->persistentCell) {
                    now.push_back(world->persistentCell->GetFormID());  // big buildings, quest places
                }
            }
            bool came = false;
            for (const auto id : now) {
                if (std::ranges::find(sweep.known, id) == sweep.known.end()) {
                    Queue(id);
                    sweep.again.emplace_back(a_now + 1.5f, id);
                    sweep.again.emplace_back(a_now + 5.0f, id);
                    came = true;
                }
            }
            sweep.known = std::move(now);
            for (auto it = sweep.again.begin(); it != sweep.again.end();) {
                if (it->first > a_now) {
                    ++it;
                    continue;
                }
                if (std::ranges::find(sweep.known, it->second) != sweep.known.end()) {
                    Queue(it->second);
                }
                it = sweep.again.erase(it);
            }
            if (came) {
                std::scoped_lock guard(meshLock);
                Evict();
            }
        }

        // main thread: the game's distant land and water taken again when they change (it swaps their blocks as one goes)
        void LookAtLod(RE::PlayerCharacter* a_player, RE::FormID a_space)
        {
            const auto here = a_player->GetParentCell();
            const auto tes = RE::TES::GetSingleton();
            if (!tes || !here || here->IsInteriorCell()) {
                return;
            }
            const auto player = a_player->GetPosition();
            if (tes->lodLandRoot) {
                TakeLoose(kLodLand, tes->lodLandRoot, {}, a_space, player, kLand);
            }
            if (tes->objLODWaterRoot) {
                TakeLoose(kLodWater, tes->objLODWaterRoot, {}, a_space, player, kWater);
            }
            if (Settings::Map().debugLog) {
                static std::size_t landParts = 0, waterParts = 0;
                const auto         land = looseRecs[kLodLand].parts.size(), water = looseRecs[kLodWater].parts.size();
                if (land != landParts || water != waterParts) {
                    landParts = land;
                    waterParts = water;
                    // every mesh in the distant land's tree: shown or hidden by the game (on itself or above), where (in
                    // cells), its name - what the map takes is the shown ones
                    struct Group
                    {
                        int                        count = 0;
                        float                      x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
                        std::map<std::string, int> names;
                    };
                    Group                                           shown, hidden;
                    std::function<void(RE::NiAVObject*, bool, int)> walk = [&](RE::NiAVObject* a_obj, bool a_hidden, int a_depth) {
                        if (!a_obj || a_depth > 64) {
                            return;
                        }
                        const bool off = a_hidden || a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden, RE::NiAVObject::Flag::kNotVisible);
                        if (const auto node = a_obj->AsNode()) {
                            for (const auto& child : node->GetChildren()) {
                                walk(child.get(), off, a_depth + 1);
                            }
                            return;
                        }
                        if (!a_obj->AsGeometry()) {
                            return;
                        }
                        auto&       g = off ? hidden : shown;
                        const auto& c = a_obj->worldBound.center;
                        ++g.count;
                        g.x0 = std::min(g.x0, c.x / kCellSize);
                        g.y0 = std::min(g.y0, c.y / kCellSize);
                        g.x1 = std::max(g.x1, c.x / kCellSize);
                        g.y1 = std::max(g.y1, c.y / kCellSize);
                        std::string name = a_obj->name.c_str();
                        name = name.substr(0, name.find_first_of("0123456789"));  // by kind, not by number
                        ++g.names[std::format("{} r{:.0f}", name, std::round(a_obj->worldBound.radius / 1000.0f) * 1000.0f)];
                    };
                    walk(tes->lodLandRoot, false, 0);
                    const auto describe = [](const Group& a_g) {
                        std::string out = std::format("{} meshes, cells x {:.1f}..{:.1f} y {:.1f}..{:.1f}:", a_g.count, a_g.x0, a_g.x1, a_g.y0, a_g.y1);
                        for (const auto& [n, k] : a_g.names) {
                            out += std::format(" '{}' x{}", n, k);
                        }
                        return out;
                    };
                    const auto at = a_player->GetPosition();
                    logger::info("map 3d: the distant land: {} blocks, the distant water: {}; the character in cell {:.1f} {:.1f}\n    shown: {}\n    hidden: {}", land, water,
                        at.x / kCellSize, at.y / kCellSize, describe(shown), describe(hidden));
                }
            }
        }

        // main thread: a pass over everything loaded
        void StartPass()
        {
            for (const auto id : sweep.known) {
                Queue(id);
            }
            ++pass;
            ++perf.passes;
            ++live.passes;
            std::scoped_lock guard(meshLock);
            Evict();
        }

        // main thread: a thing activated - it, and whatever near it can move, followed a while
        void Activated(RE::TESObjectREFR* a_ref)
        {
            Track(a_ref->GetFormID(), kWatchActivated);
            const auto at = a_ref->GetPosition();
            for (const auto id : sweep.known) {
                const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
                if (!cell || !cell->IsAttached()) {
                    continue;
                }
                if (const auto c = cell->IsInteriorCell() ? nullptr : cell->GetCoordinates()) {
                    const float x = std::clamp(at.x, c->cellX * kCellSize, (c->cellX + 1) * kCellSize);
                    const float y = std::clamp(at.y, c->cellY * kCellSize, (c->cellY + 1) * kCellSize);
                    if (std::hypot(x - at.x, y - at.y) > kActivatedNear) {
                        continue;
                    }
                }
                cell->ForEachReference([&](RE::TESObjectREFR* a_near) {
                    if (a_near && a_near != a_ref && CanMove(a_near) && a_near->GetPosition().GetDistance(at) <= kActivatedNear && refRecs.contains(a_near->GetFormID())) {
                        Track(a_near->GetFormID(), kWatchActivated);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        }

        // the water planes lying in a cell, outside every cell's tree
        std::vector<RE::BSTriShape*> WaterOf(RE::FormID a_cell, bool a_inside)
        {
            std::vector<RE::BSTriShape*> out;
            const auto                   ws = RE::TESWaterSystem::GetSingleton();
            if (!ws) {
                return out;
            }
            RE::BSSpinLockGuard guard(ws->lock);
            for (const auto& object : ws->waterObjects) {
                const auto shape = object ? object->shape.get() : nullptr;
                const auto tri = shape ? shape->AsTriShape() : nullptr;
                if (!tri) {
                    continue;
                }
                bool inTree = false;
                for (RE::NiAVObject* o = tri; o && !inTree; o = o->parent) {
                    inTree = sweep.roots.contains(o);
                }
                const auto& c = tri->worldBound.center;
                const auto  id = a_inside ? a_cell : ExteriorId(static_cast<std::int32_t>(std::floor(c.x / kCellSize)), static_cast<std::int32_t>(std::floor(c.y / kCellSize)));
                if (!inTree && id == a_cell) {
                    out.push_back(tri);
                }
            }
            return out;
        }

        // main thread: everything other than this world space (or room) goes from the map; the models stay (a city
        // entered again finds its meshes)
        void LeaveSpace(RE::FormID a_space)
        {
            std::scoped_lock guard(meshLock);
            for (std::uint32_t slot = 1; slot < instances.size(); ++slot) {
                const auto& in = instances[slot];
                if (in.model && in.space != a_space) {
                    Unplace((static_cast<std::uint64_t>(in.serial) << 32) | slot);
                }
            }
            refRecs.clear();
            refsLeaving.clear();
            looseRecs.clear();
            ++geometryVersion;
        }

        // main thread, every frame: a frame's share of the gathering (kWorkMs)
        void Work(RE::PlayerCharacter* a_player, RE::FormID a_space)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(kWorkMs));
            const auto over = [&] { return std::chrono::steady_clock::now() >= deadline; };
            const auto player = a_player->GetPosition();
            const auto here = a_player->GetParentCell();
            const bool inside = here && here->IsInteriorCell();
            if (a_space != sweep.space) {
                LeaveSpace(a_space);
                sweep = {};
                sweep.space = a_space;
            }
            TakeDecoded();
            SettleLoose();
            const float now = Now();
            if (now >= sweep.nextCells) {
                sweep.nextCells = now + kCellsEvery;
                LookAtCells(a_player, now);
                if (Settings::Map().look.distant) {
                    LookAtLod(a_player, a_space);
                }
            }
            if (now >= sweep.nextPass) {
                sweep.nextPass = now + kPassEvery;
                StartPass();
            }

            // the detailed log: the reference picked in the console, inspected once
            if (Settings::Map().debugLog) {
                const auto picked = RE::Console::GetSelectedRef();
                const auto pickedId = picked ? picked->GetFormID() : 0;
                if (pickedId != inspected) {
                    inspected = pickedId;
                    highlighted = pickedId;
                    ++geometryVersion;
                    if (picked) {
                        Inspect(picked.get());
                    }
                }
            } else if (inspected != 0) {
                inspected = 0;
                highlighted = 0;
                ++geometryVersion;
            }

            // a reference of this space's loaded cells
            const auto ours = [&](RE::TESObjectREFR* a_ref) {
                const auto cell = a_ref ? a_ref->GetParentCell() : nullptr;
                return cell && cell->IsAttached() && (inside ? cell == here : (!cell->IsInteriorCell() && a_ref->GetWorldspace() == a_player->GetWorldspace()));
            };

            // what the game said loaded or unloaded, or was activated: first
            std::vector<RE::FormID> events, activated;
            {
                std::scoped_lock guard(eventLock);
                events.swap(loadedRefs);
                activated.swap(activatedRefs);
            }
            for (const auto id : activated) {
                if (const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id); ours(ref)) {
                    Activated(ref);
                }
            }
            std::size_t next = 0;
            for (; next < events.size() && !over(); ++next) {
                const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(events[next]);
                if (!ours(ref)) {
                    continue;
                }
                TakeRef(ref, a_space, player);
                if (ref->Get3D()) {
                    Track(events[next], kWatchLoaded);
                }
            }
            if (next < events.size()) {
                std::scoped_lock guard(eventLock);
                loadedRefs.insert(loadedRefs.begin(), events.begin() + static_cast<std::ptrdiff_t>(next), events.end());
            }

            // the references followed: those due looked at again - one that changed followed on
            for (auto it = sweep.watched.begin(); it != sweep.watched.end() && !over();) {
                auto& w = it->second;
                if (w.next > now) {
                    ++it;
                    continue;
                }
                const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(it->first);
                if (!ours(ref)) {
                    it = sweep.watched.erase(it);
                    continue;
                }
                if (TakeRef(ref, a_space, player)) {
                    w.left = std::max(w.left, kWatchLoaded);
                } else {
                    --w.left;
                }
                w.next = now + kWatchStep;
                it = w.left > 0 ? std::next(it) : sweep.watched.erase(it);
            }

            // the cells to go over, on from where it stopped
            while (!over() && sweep.cell < sweep.cells.size()) {
                const auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(sweep.cells[sweep.cell]);
                const auto data = cell ? cell->GetRuntimeData().loadedData : nullptr;
                if (!cell) {
                    ++sweep.cell;
                    sweep.ref = 0;
                    sweep.looseDone = false;
                    continue;
                }
                if (!sweep.looseDone) {
                    sweep.looseDone = true;
                    if (data && data->cell3D) {
                        const auto id = CellId(cell);
                        TakeLoose(id, data->cell3D.get(), WaterOf(id, inside), a_space, player);
                    }
                    continue;
                }
                bool        finished = true;
                std::size_t index = 0;
                cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                    const auto at = index++;
                    if (at < sweep.ref) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    if (over()) {
                        finished = false;
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    if (a_ref) {
                        TakeRef(a_ref, a_space, player);
                    }
                    sweep.ref = at + 1;
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (finished) {
                    ++sweep.cell;
                    sweep.ref = 0;
                    sweep.looseDone = false;
                }
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            perf.workMs += ms;
            perf.workMax = std::max(perf.workMax, ms);
            live.workMs += ms;
            live.workMax = std::max(live.workMax, ms);
        }

        // main thread, under meshLock: everything gone (the map off, the filter changed)
        void DropAllGeometry()
        {
            for (auto& [key, m] : models) {
                for (const auto p : { m->vb, m->ib }) {
                    if (p) {
                        releaseQueue.push_back(p);
                    }
                }
            }
            models.clear();
            instances.assign(1, {});
            freeSlots.clear();
            for (auto& job : jobs) {
                graveyard.push_back(std::move(job.keep));
            }
            jobs.clear();
            {
                std::scoped_lock guard(workLock);
                ++workGeneration;
            }
            refRecs.clear();
            refsLeaving.clear();
            looseRecs.clear();
            sweep = {};
            ++geometryVersion;
        }

        // ---- roads: the navmesh's "preferred" triangles (orange in the Creation Kit) - the paths NPCs keep to, laid
        // along the roads. Taken per exterior cell from the game while it is loaded, kept for the world space. Drawn as
        // triangles into a mask of the picture (coverage and the road's height, exact at any zoom), which the shader
        // turns into a road on whatever surface lies there
        using RoadKey = std::pair<RE::FormID, RE::FormID>;  // world space, exterior cell
        std::map<RoadKey, std::vector<RE::NiPoint3>> roads;  // main thread: 3 corners a triangle, world space
        bool                                         roadsChanged = true;
        std::map<RoadKey, std::uint64_t>             roadSigs;  // per cell, its navmeshes as last read (unchanged: not read again)

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
                roadSigs.erase(it->first);
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
                    // its navmeshes the same as last time: nothing to read
                    std::uint64_t sig = 1469598103934665603ull;
                    for (const auto& ptr : navs->navMeshes) {
                        const auto mesh = ptr.get();
                        sig = Mix(sig ^ reinterpret_cast<std::uintptr_t>(mesh));
                        sig = Mix(sig ^ (mesh ? (static_cast<std::uint64_t>(mesh->triangles.size()) << 32 | mesh->vertices.size()) : 0));
                    }
                    const RoadKey key{ a_space, ExteriorId(coords->cellX, coords->cellY) };
                    if (const auto it = roadSigs.find(key); it != roadSigs.end() && it->second == sig) {
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
                    roadSigs[key] = sig;
                    auto& have = roads[key];
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

        // render thread, under meshLock: queued meshes read back from their GPU buffers into their models without waiting
        // for the GPU - the buffers are copied this frame and read a frame or two later, once the copies are done (a read
        // right after the copy would wait for everything the GPU has queued: a stall a frame)
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
                if (SUCCEEDED(vr) && job.rawIndices.empty() && p.ibCopy) {
                    ir = a_context->Map(p.ibCopy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &imap);
                    if (ir == DXGI_ERROR_WAS_STILL_DRAWING) {
                        a_context->Unmap(p.vbCopy, 0);
                        ++it;
                        continue;
                    }
                }
                const auto found = models.find(job.key);
                Model*     m = found != models.end() && found->second->state == kDecoding ? found->second.get() : nullptr;  // still wanted
                bool       ok = false;
                if (SUCCEEDED(vr)) {
                    D3D11_BUFFER_DESC vd{};
                    p.vbCopy->GetDesc(&vd);
                    const std::uint16_t* indices = job.rawIndices.empty() ? nullptr : job.rawIndices.data();
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
                    if (m && indices && vd.ByteWidth >= static_cast<std::size_t>(job.vertexCount) * job.stride) {
                        Geometry geo;
                        ok = Decode(job.keep ? job.keep->name.c_str() : "", static_cast<const std::uint8_t*>(vmap.pData), job.stride, job.vertexCount, job.full, indices,
                            job.triangleCount, job.world, job.bound, geo, true, false);
                        const bool huge = ok && (geo.maxX - geo.minX > 3000.0f || geo.maxY - geo.minY > 3000.0f);  // bigger than a land block
                        if (readLogs < 10 || (huge && readLogs < 60)) {
                            ++readLogs;
                            const auto& b = job.bound;
                            logger::info("map 3d: GPU read '{}': {} verts, {} tris, stride {}, vb {} bytes, indices from {} -> {} -> {} triangles kept "
                                         "({} bad index, {} too long); bound r {:.0f} at ({:.0f} {:.0f} {:.0f}), mesh x {:.0f}..{:.0f} y {:.0f}..{:.0f}",
                                job.keep ? job.keep->name.c_str() : "", job.vertexCount, job.triangleCount, job.stride, vd.ByteWidth,
                                !job.rawIndices.empty() ? "the CPU copy" : "the GPU", ok ? "decoded" : "NOT decoded", geo.triangles, rejectedIndex, rejectedEdge, b.radius,
                                b.center.x, b.center.y, b.center.z, geo.minX, geo.maxX, geo.minY, geo.maxY);
                        }
                        // a cell without landscape data gets a perfectly flat block at the world's default height: a
                        // placeholder nobody walks on, a huge empty sheet on the map
                        const bool placeholder = ok && m->kind == kLand && geo.maxZ - geo.minZ < 1.0f;
                        if (placeholder && flatLandLogs < 20) {
                            ++flatLandLogs;
                            logger::info("map 3d: flat placeholder land '{}' at z {:.0f}, x {:.0f}..{:.0f} y {:.0f}..{:.0f} - left out", job.keep ? job.keep->name.c_str() : "",
                                geo.minZ, geo.minX, geo.maxX, geo.minY, geo.maxY);
                        }
                        ok = ok && geo.triangles > 0 && !placeholder;
                        if (ok) {
                            m->geo = std::move(geo);
                            m->triangles = m->geo.triangles;
                        }
                    }
                    if (SUCCEEDED(ir)) {
                        a_context->Unmap(p.ibCopy, 0);
                    }
                    a_context->Unmap(p.vbCopy, 0);
                }
                if (m) {
                    m->state = ok ? kReady : kFailed;
                    ++geometryVersion;
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
                logger::info("map 3d: GPU reads done - {} meshes read, {} could not be decoded, {} stray triangles so far", gpuRead.load(), gpuFailed.load(),
                    strayTriangles.load());
            }

            // new copies, read in a later frame
            const auto copyOf = [&](ID3D11Buffer* a_src) -> ID3D11Buffer* {
                if (!a_src) {
                    return nullptr;
                }
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
                p.vbCopy = copyOf(p.job.vb.p);
                p.ibCopy = p.job.rawIndices.empty() ? copyOf(p.job.ib.p) : nullptr;
                p.job.vb.reset();  // copied: the game's own may go now
                p.job.ib.reset();
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
    float4 shade;      // x depth shade, y interior, z style (0 colour, 1 vanilla), w the diagnostics' colours
    float4 occluder;   // the character on this picture: px centre, px radii (z = 0: no occlusion cut)
    float4 occluder2;  // x: the character's depth on this picture, y: feet height to keep, z: the cut takes the objects only
    float4 timing;     // x now (s), y fade-in time (s)
    float4 roadBox;    // x widened by this many world units, w on
    float4 roadLook;   // rgb
    float4 water;      // rgb, a on
    float4 under;      // x the water's level, y 0..1 the character under it (the water not drawn, what lies below its level tinted)
    float4 landOrigin;    // x, y: the cell at texel 0, 0 of the land mask
    float4 fog;           // the fog of war: rgb, a how thick (0: none)
    float4 fogMap;        // xy: the world point at the fog texture's corner, z: world units a texel, w: the edge's noise
    float4 fogView;       // xy: the picture's size (px)
};
cbuffer K : register(b1)
{
    float4 kind;    // x: 0 objects, 1 the landscape, 2 water
};
Texture2D<float2> roadMask : register(t0);  // per pixel of this picture: a road drawn there, its height
Texture2D<float>  fogTex : register(t1);    // the fog of war round the character: how far each square was seen
Texture2D<float>  landMask : register(t2);  // 32 x 32 exterior cells round the character: 1 where their own landscape is in

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

// ---- the fog of war: how far a world point has been seen (Fog: a texture of squares round the character), the squares
// eased between (no diamonds of a plain blend), the edge broken up by noise
float FogTexel(int2 p)
{
    return all(p >= 0) && all(p < 512) ? fogTex.Load(int3(p, 0)) : 0.0;
}

float SeenAt(float2 xy)
{
    float k = fogMap.w;
    xy += (float2(Noise(xy / 900.0), Noise(xy / 900.0 + 17.3)) - 0.5) * fogMap.z * 1.5 * k;
    float2 t = (xy - fogMap.xy) / fogMap.z - 0.5;
    float2 i = floor(t);
    float2 f = t - i;
    f = f * f * (3.0 - 2.0 * f);
    int2  p = int2(i);
    float v = lerp(lerp(FogTexel(p), FogTexel(p + int2(1, 0)), f.x), lerp(FogTexel(p + int2(0, 1)), FogTexel(p + int2(1, 1)), f.x), f.y);
    v += (Noise(xy / 240.0) - 0.5) * 0.2 * k;
    return smoothstep(0.1, 0.8, v);
}

// a surface under the fog: its colour gone towards the fog's, as opaque as it is thick
float4 Fogged(float4 c, float2 xy)
{
    if (fog.a <= 0.0) {
        return c;
    }
    float f = (1.0 - SeenAt(xy)) * fog.a;
    return float4(lerp(c.rgb, fog.rgb, f), lerp(c.a, 1.0, f));
}

// the fog over what nothing was drawn on (the picture's empty background): a triangle over the whole picture, each
// pixel's world point taken at the character's height
float4 VSFog(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSFog(float4 pos : SV_Position) : SV_Target
{
    if (fog.a <= 0.0) {
        discard;
    }
    float2 ndc = float2(pos.x / fogView.x * 2.0 - 1.0, 1.0 - pos.y / fogView.y * 2.0);
    float2 rhs = float2(ndc.x - row0.z * player.z - row0.w, ndc.y - row1.z * player.z - row1.w);
    float  det = row0.x * row1.y - row0.y * row1.x;
    if (abs(det) < 1e-24) {
        discard;
    }
    float2 xy = float2(rhs.x * row1.y - row0.y * rhs.y, row0.x * rhs.y - rhs.x * row1.x) / det;
    float  f = (1.0 - SeenAt(xy)) * fog.a;
    if (f <= 0.003) {
        discard;
    }
    return float4(fog.rgb, f);
}

struct VSOut
{
    float4                pos : SV_Position;
    float3                world : TEXCOORD0;
    float                 depth : TEXCOORD1;     // along the view
    nointerpolation float fadeFrom : TEXCOORD2;  // Now() its instance went on the map while it fades in (0: not fading)
    nointerpolation float mark : TEXCOORD3;      // the detailed log: the reference picked in the console, drawn red
    nointerpolation float lod : TEXCOORD4;       // the game's distant land or water
};

// a world point onto the picture
VSOut Project(float3 w, float fadeFrom)
{
    VSOut  o;
    float4 q = float4(w, 1.0);
    o.pos = float4(dot(row0, q), dot(row1, q), dot(row2, q), dot(row3, q));
    o.depth = o.pos.z;
    o.world = w;
    o.fadeFrom = fadeFrom;
    o.mark = 0.0;
    o.lod = 0.0;
    return o;
}

// a model's vertex placed by its instance (its space -> the world: three rows)
struct VSIn
{
    float3 p : POSITION;
    float4 r0 : ROW0;
    float4 r1 : ROW1;
    float4 r2 : ROW2;
    float4 extra : EXTRA;  // x: fadeFrom
};

VSOut VS(VSIn v)
{
    float4 q = float4(v.p, 1.0);
    // the distant land a little lower: the cells' own landscape fading in over it lies on top
    VSOut  o = Project(float3(dot(v.r0, q), dot(v.r1, q), dot(v.r2, q) - 48.0 * v.extra.w), v.extra.x);
    o.mark = v.extra.y;
    o.lod = v.extra.z;
    return o;
}

// the road triangles: in world space already
VSOut VSRoad(float3 p : POSITION)
{
    return Project(p, 0.0);
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

// the character under water: what lies below the water's level tinted by it, evenly (the water's own meshes, one
// over another in places, are not drawn then - they would add up unevenly)
float3 Under(float3 col, float z, bool isWater, float3 tint)
{
    if (isWater || under.y <= 0.0 || z > under.x) {
        return col;
    }
    return lerp(col, tint, 0.2 * under.y);
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
    if (under.y > 0.5 && i.world.z > under.x - 8.0) {
        discard;  // under water: no roads up on the shore
    }
    if (occluder2.z < 0.5 && i.world.z > player.z + player.w + 60.0) {
        discard;  // cut away with what stands over the character (inside)
    }
    return float2(1.0, i.world.z);
}

float4 PS(VSOut i) : SV_Target
{
    float3 w = i.world;
    // the distant land and water: not where a cell's own landscape is in
    if (i.lod > 0.5) {
        int2 c = int2(floor(w.xy / 4096.0)) - int2(landOrigin.xy);
        if (all(c >= 0) && all(c < 32) && landMask.Load(int3(c, 0)) > 0.5) {
            discard;
        }
    }
    // the cut, 1: inside, everything over the head (a ceiling would hide the whole room); outside the objects only (an
    // underground world's roof), the landscape and the distant land stay
    if (w.z > player.z + player.w && (occluder2.z < 0.5 || (kind.x < 0.5 && i.lod < 0.5))) {
        if (i.mark > 0.5) {
            return float4(0.2, 0.4, 1.0, 1.0);  // picked, but cut away over the head: blue
        }
        discard;
    }
    // the cut, 2: exactly what covers the character on this picture - inside their outline on screen, nearer to the
    // view than they are and above their feet (a roof, a crown, a bridge, an arch): a tunnel down to them
    float occRim = 0.0;
    if (occluder.z > 0.0 && w.z > occluder2.y) {
        float r = length((i.pos.xy - occluder.xy) / occluder.zw);
        if (i.depth < occluder2.x) {
            if (r < 1.0) {
                if (i.mark > 0.5) {
                    return float4(0.2, 1.0, 0.3, 1.0);  // picked, but cut away over the character: green
                }
                discard;
            }
            occRim = 1.0 - smoothstep(1.0, 1.12, r);  // a bright edge around the tunnel
        }
    }
    // the character under water: only what lies under its surface - what stands over it (ice floes, boats, the
    // shore, bridges) fades out as they dive (the water's own meshes go too, by their kind)
    float above = under.y > 0.0 && kind.x < 1.5 && w.z > under.x - 8.0 ? 1.0 - under.y : 1.0;
    if (above <= 0.01) {
        discard;
    }
    // a mesh new to the map (a cell just loaded, a house, water arriving late) fades in
    float fade = i.fadeFrom > 0.0 ? saturate((timing.x - i.fadeFrom) / timing.y) : 1.0;
    if (fade <= 0.003) {
        if (i.mark > 0.5) {
            return float4(1.0, 0.9, 0.1, 1.0);  // picked, but not faded in: yellow
        }
        discard;
    }
    float3 nc = cross(ddx(w), ddy(w));
    if (i.mark > 0.5 && (any(isnan(w)) || any(isinf(w)))) {
        return float4(0.0, 1.0, 1.0, 1.0);  // picked: a vertex not a number - cyan
    }
    if (i.mark > 0.5 && dot(nc, nc) < 1e-12) {
        return float4(1.0, 0.0, 1.0, 1.0);  // picked: no slope to take a normal from - magenta
    }
    float3 n = normalize(nc);
    float  up = abs(n.z);
    float  flat = smoothstep(0.55, 0.85, up);
    float3 L = normalize(float3(-0.45, 0.55, 0.7));
    float  light = 0.55 + 0.6 * abs(dot(n, L));
    // the diagnostics: each surface coloured by what it is - objects red, the landscape green, water blue, the game's distant
    // land and objects (LOD) yellow - so a screenshot tells what covers the map
    if (shade.w > 0.5) {
        float3 c = i.lod > 0.5 ? float3(1.0, 0.85, 0.1) : kind.x > 1.5 ? float3(0.2, 0.4, 1.0) : kind.x > 0.5 ? float3(0.2, 0.85, 0.3) : float3(0.95, 0.25, 0.2);
        return float4(c * light, 1.0);
    }
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
        col = Under(col, w.z, isWater, float3(0.17, 0.20, 0.21));
        col = i.mark > 0.5 ? lerp(col, float3(1.0, 0.1, 0.1), 0.75) : col;  // picked in the console: tinted red, through the same cuts
    } else {
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
        col = Under(col, w.z, isWater, water.rgb);
        col = i.mark > 0.5 ? lerp(col, float3(1.0, 0.1, 0.1), 0.75) : col;
    }
    // one way out: the fog of war over it (its code once in the shader)
    return Fogged(float4(col, look.a * fade * above * (isWater ? 1.0 - under.y : 1.0)), w.xy);  // water: gone from under it
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
            float under[4];
            float         landOrigin[4];  // x, y: the cell at texel 0, 0 of the land mask
            float         fog[4];
            float         fogMap[4];
            float         fogView[4];
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
            ID3D11VertexShader*      vsRoad = nullptr;     // ...their triangles in world space already
            ID3D11InputLayout*       roadLayout = nullptr;
            ID3D11Buffer*            instanceVb = nullptr;  // the instances drawn: placed and faded per instance
            UINT                     instanceCapacity = 0;
            ID3D11BlendState*        roadBlend = nullptr;  // max: of two roads on a pixel (a bridge over one) the upper
            ID3D11Buffer*            roadVb = nullptr;     // every road triangle of the world space shown
            UINT                     roadCorners = 0;
            std::uint32_t            roadUploaded = 0;
            Target                   targets[2];  // the minimap's picture, the local map's (both may be up in one frame: one fading out)
            // the fog of war: its shaders, the texture round the character (shared by both pictures)
            ID3D11VertexShader*       vsFog = nullptr;
            ID3D11PixelShader*        psFog = nullptr;
            ID3D11DepthStencilState*  noDepth = nullptr;
            ID3D11BlendState*         fogBlend = nullptr;  // over the empty only: what is drawn stays (it was fogged as drawn)
            ID3D11Texture2D*          fogTex = nullptr;
            ID3D11ShaderResourceView* fogSrv = nullptr;
            ID3D11Texture2D*          landTex = nullptr;  // 32 x 32 exterior cells round the character: their own landscape in
            ID3D11ShaderResourceView* landSrv = nullptr;
            Fog::Window               fogWindow;
            std::vector<std::uint8_t> fogTexels;
            ID3D11Texture2D*          fogProbe = nullptr;  // the detailed log: one texel of the fog read back (staging)
            bool                      probePending = false;
            ID3D11Texture2D*          picProbe[2]{};       // the detailed log: each picture read back (staging), to count what it holds
            int                       picProbeSize[2][2]{};
            bool                      picPending[2]{};
            std::string               adapter;             // the GPU, its feature level
        } r;

        // the shaders are compiled on a thread of their own as soon as the game's data is loaded (half a second of
        // D3DCompile on the render thread was a freeze on the first frame of the map)
        struct Compiled
        {
            std::atomic_bool done = false;
            bool             ok = false;
            ID3DBlob*        blobs[6]{};  // VS, PS, PSRoad, VSRoad, VSFog, PSFog
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
                    const char* stages[6][2] = { { "VS", "vs_5_0" }, { "PS", "ps_5_0" }, { "PSRoad", "ps_5_0" }, { "VSRoad", "vs_5_0" }, { "VSFog", "vs_5_0" }, { "PSFog", "ps_5_0" } };
                    for (int i = 0; compiled.ok && i < 6; ++i) {
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
            // a model's vertices (slot 0), its instances' rows and fade (slot 1, one a instance)
            const D3D11_INPUT_ELEMENT_DESC elements[]{
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "ROW", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "ROW", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "ROW", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
                { "EXTRA", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            };
            const D3D11_INPUT_ELEMENT_DESC road{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
            bool ok = SUCCEEDED(a_device->CreateVertexShader(blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), nullptr, &r.vs)) &&
                      SUCCEEDED(a_device->CreatePixelShader(blobs[1]->GetBufferPointer(), blobs[1]->GetBufferSize(), nullptr, &r.ps)) &&
                      SUCCEEDED(a_device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)), blobs[0]->GetBufferPointer(), blobs[0]->GetBufferSize(), &r.layout)) &&
                      SUCCEEDED(a_device->CreatePixelShader(blobs[2]->GetBufferPointer(), blobs[2]->GetBufferSize(), nullptr, &r.psRoad)) &&
                      SUCCEEDED(a_device->CreateVertexShader(blobs[3]->GetBufferPointer(), blobs[3]->GetBufferSize(), nullptr, &r.vsRoad)) &&
                      SUCCEEDED(a_device->CreateInputLayout(&road, 1, blobs[3]->GetBufferPointer(), blobs[3]->GetBufferSize(), &r.roadLayout)) &&
                      SUCCEEDED(a_device->CreateVertexShader(blobs[4]->GetBufferPointer(), blobs[4]->GetBufferSize(), nullptr, &r.vsFog)) &&
                      SUCCEEDED(a_device->CreatePixelShader(blobs[5]->GetBufferPointer(), blobs[5]->GetBufferSize(), nullptr, &r.psFog));
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
            // the fog's pass: no depth, and only where nothing was drawn (the colour added as much as the picture is empty)
            D3D11_DEPTH_STENCIL_DESC nd{};
            nd.DepthEnable = FALSE;
            nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            nd.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ok = ok && SUCCEEDED(a_device->CreateDepthStencilState(&nd, &r.noDepth));
            D3D11_BLEND_DESC fb{};
            fb.RenderTarget[0].BlendEnable = TRUE;
            fb.RenderTarget[0].SrcBlend = fb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_DEST_ALPHA;
            fb.RenderTarget[0].DestBlend = fb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            fb.RenderTarget[0].BlendOp = fb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            fb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ok = ok && SUCCEEDED(a_device->CreateBlendState(&fb, &r.fogBlend));
            D3D11_TEXTURE2D_DESC fd{};
            fd.Width = fd.Height = Fog::kTex;
            fd.MipLevels = fd.ArraySize = 1;
            fd.Format = DXGI_FORMAT_R8_UNORM;
            fd.SampleDesc.Count = 1;
            fd.Usage = D3D11_USAGE_DEFAULT;
            fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            r.fogTexels.assign(static_cast<std::size_t>(Fog::kTex) * Fog::kTex, 0);
            D3D11_SUBRESOURCE_DATA fdata{ r.fogTexels.data(), Fog::kTex, 0 };
            // the cells' mask (the distant land not drawn where a cell's own is in): without it the distant land is drawn everywhere
            {
                D3D11_TEXTURE2D_DESC ld = fd;
                ld.Width = ld.Height = 32;
                if (FAILED(a_device->CreateTexture2D(&ld, nullptr, &r.landTex)) || FAILED(a_device->CreateShaderResourceView(r.landTex, nullptr, &r.landSrv))) {
                    logger::warn("map 3d: the land mask could not be made");
                }
            }
            // the fog's texture: without it the map draws on, without the fog
            if (FAILED(a_device->CreateTexture2D(&fd, &fdata, &r.fogTex)) || FAILED(a_device->CreateShaderResourceView(r.fogTex, nullptr, &r.fogSrv))) {
                logger::warn("map 3d: the fog of war's texture could not be made: no fog drawn");
                if (r.fogTex) {
                    r.fogTex->Release();
                    r.fogTex = nullptr;
                }
            }

            r.ready = ok;
            if (ok) {
                {
                    // the GPU and its feature level, for reports from machines the map draws nothing on
                    IDXGIDevice*  dxgi = nullptr;
                    IDXGIAdapter* adapter = nullptr;
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(a_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
                        char name[128]{};
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                        r.adapter = std::format("'{}' (vendor {:04X}, device {:04X}, {} MB), feature level {:X}", name, desc.VendorId, desc.DeviceId, desc.DedicatedVideoMemory >> 20,
                            static_cast<unsigned>(a_device->GetFeatureLevel()));
                    }
                    for (IUnknown* p : std::initializer_list<IUnknown*>{ adapter, dxgi }) {
                        if (p) {
                            p->Release();
                        }
                    }
                }
                logger::info("map 3d: renderer ready ({}x MSAA), the fog of war's texture {}", r.samples, r.fogTex ? "made" : "MISSING");
            } else {
                logger::warn("map 3d: renderer FAILED - no map picture");
            }
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
            ID3D11Buffer*             vb[2]{};
            UINT                      vbStride[2]{}, vbOffset[2]{};
            ID3D11Buffer*             ib = nullptr;
            DXGI_FORMAT               ibFormat{};
            UINT                      ibOffset = 0;
            ID3D11VertexShader*       vs = nullptr;
            ID3D11PixelShader*        ps = nullptr;
            ID3D11GeometryShader*     gs = nullptr;
            ID3D11Buffer*             vsCb = nullptr;
            ID3D11Buffer*             psCb = nullptr;
            ID3D11Buffer*             psCb1 = nullptr;
            ID3D11ShaderResourceView* psSrv[3]{};
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
                c->IAGetVertexBuffers(0, 2, vb, vbStride, vbOffset);
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
                c->PSGetShaderResources(0, 3, psSrv);
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
                c->IASetVertexBuffers(0, 2, vb, vbStride, vbOffset);
                c->IASetIndexBuffer(ib, ibFormat, ibOffset);
                c->VSSetShader(vs, nullptr, 0);
                c->PSSetShader(ps, nullptr, 0);
                c->GSSetShader(gs, nullptr, 0);
                c->VSSetConstantBuffers(0, 1, &vsCb);
                c->PSSetConstantBuffers(0, 1, &psCb);
                c->PSSetConstantBuffers(1, 1, &psCb1);
                c->PSSetShaderResources(0, 3, psSrv);
                c->RSSetState(raster);
                c->RSSetViewports(viewportCount, viewports);
                c->OMSetRenderTargets(1, &rtv, dsv);
                c->OMSetBlendState(blend, blendFactor, sampleMask);
                c->OMSetDepthStencilState(depth, stencilRef);
                for (IUnknown* p : std::initializer_list<IUnknown*>{ layout, vb[0], vb[1], ib, vs, ps, gs, vsCb, psCb, psCb1, psSrv[0], psSrv[1], psSrv[2], raster, rtv, dsv, blend, depth }) {
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

        // the on-screen panel's numbers, once a second while the detailed log is on (main thread)
        void TakeLive()
        {
            const auto now = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(now - live.since).count();
            if (seconds < 1.0f) {
                return;
            }
            Stats st;
            if (Settings::Map().debugLog) {
                st.valid = true;
                const float frames = static_cast<float>(std::max(live.frames, 1));
                st.fps = static_cast<float>(live.frames) / seconds;
                st.frameMs = seconds * 1000.0f / frames;
                st.updateMs = static_cast<float>(live.updateMs) / frames;
                st.updateMax = static_cast<float>(live.updateMax);
                st.workMs = static_cast<float>(live.workMs) / frames;
                st.workMax = static_cast<float>(live.workMax);
                st.redraws = live.redraws.load();
                st.reused = live.reused.load();
                st.renderMs = st.redraws ? static_cast<float>(live.renderUs.load()) / 1000.0f / static_cast<float>(st.redraws) : 0.0f;
                st.renderMax = static_cast<float>(live.renderMaxUs.load()) / 1000.0f;
                st.refsTaken = live.refs;
                st.modelsMade = live.models;
                st.uploads = live.uploads.load();
                st.passes = live.passes;
                st.drawnInstances = live.drawnInstances.load();
                st.drawnTriangles = live.drawnTriangles.load();
                st.drawCalls = live.drawCalls.load();
                st.refs = refRecs.size();
                std::size_t cpuBytes = refRecs.size() * 64;
                {
                    std::scoped_lock guard(meshLock);
                    for (const auto& in : instances) {
                        if (in.model) {
                            ++st.placed;
                            st.placedTriangles += in.triangles;
                        }
                    }
                    cpuBytes += instances.capacity() * sizeof(Instance);
                    std::size_t gpuBytes = 0;
                    for (const auto& [key, m] : models) {
                        ++st.models;
                        st.shared += (key & 1) ? 1 : 0;
                        st.onGpu += m->vb ? 1 : 0;
                        st.decoding += m->state == kDecoding ? 1 : 0;
                        st.failed += m->state == kFailed ? 1 : 0;
                        gpuBytes += m->bytes;
                        cpuBytes += sizeof(Model) + m->geo.verts.capacity() * sizeof(float) + m->geo.indices.capacity() * sizeof(std::uint32_t) +
                                    m->placed.capacity() * sizeof(std::uint32_t);
                    }
                    gpuBytes += static_cast<std::size_t>(r.instanceCapacity) * 64 + roadVerts.size() * sizeof(float);
                    std::size_t pictures = 0;
                    for (const auto& t : r.targets) {
                        // colour and depth multisampled, the resolved copy, the road mask
                        pictures += static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height) * (8u * r.samples + 4u + 8u);
                    }
                    st.gpuMB = static_cast<float>(gpuBytes) / 1048576.0f;
                    st.picturesMB = static_cast<float>(pictures) / 1048576.0f;
                    st.gpuReads = jobs.size() + inFlight.size();
                }
                {
                    std::scoped_lock guard(workLock);
                    st.decodeQueue = workQueue.size();
                }
                {
                    std::scoped_lock guard(eventLock);
                    st.events = loadedRefs.size();
                }
                st.cpuMB = static_cast<float>(cpuBytes) / 1048576.0f;
            }
            {
                std::scoped_lock guard(statsLock);
                liveStats = st;
            }
            live.since = now;
            live.frames = live.refs = live.models = live.passes = 0;
            live.updateMs = live.updateMax = live.workMs = live.workMax = 0.0;
            live.redraws = live.reused = live.uploads = 0;
            live.renderUs = live.renderMaxUs = 0;
        }

        // the detailed log: the biggest things on the map round the character, as decoded (the model's extent placed by its
        // instance) beside the game's own bound - a mesh read wrong lies far bigger than its bound and covers the map
        void LogBiggest()
        {
            const auto player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            const auto at = player->GetPosition();
            struct Big
            {
                float      area = 0.0f, ratio = 0.0f;
                float      box[6]{};  // as decoded, in the world
                float      bound[6]{};
                RE::FormID owner = 0;
                int        kind = 0;
                std::string from;
            };
            std::vector<Big> all;
            {
                std::scoped_lock guard(meshLock);
                for (const auto& in : instances) {
                    const auto m = in.model;
                    if (!m || m->state == kFailed || in.space != mapSpace || m->triangles == 0) {
                        continue;
                    }
                    const float cx = (in.box[0] + in.box[3]) * 0.5f - at.x, cy = (in.box[1] + in.box[4]) * 0.5f - at.y;
                    if (cx * cx + cy * cy > 30000.0f * 30000.0f) {
                        continue;
                    }
                    Big b;
                    for (int k = 0; k < 3; ++k) {
                        b.box[k] = FLT_MAX;
                        b.box[k + 3] = -FLT_MAX;
                    }
                    for (int c = 0; c < 8; ++c) {
                        const float p[3]{ m->extent[(c & 1) ? 3 : 0], m->extent[(c & 2) ? 4 : 1], m->extent[(c & 4) ? 5 : 2] };
                        for (int k = 0; k < 3; ++k) {
                            const float w = in.row[k][0] * p[0] + in.row[k][1] * p[1] + in.row[k][2] * p[2] + in.row[k][3];
                            b.box[k] = std::min(b.box[k], w);
                            b.box[k + 3] = std::max(b.box[k + 3], w);
                        }
                    }
                    b.area = (b.box[3] - b.box[0]) * (b.box[4] - b.box[1]);
                    b.ratio = b.area / std::max((in.box[3] - in.box[0]) * (in.box[4] - in.box[1]), 1.0f);
                    std::copy(std::begin(in.box), std::end(in.box), b.bound);
                    b.owner = in.owner;
                    b.kind = m->kind;
                    b.from = m->from;
                    all.push_back(std::move(b));
                }
            }
            const auto line = [&](const Big& a_b) {
                const auto  ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_b.owner);
                const char* model = ref ? ModelOf(ref) : nullptr;
                return std::format("\n    {:08X} '{}' mesh '{}' ({}): decoded x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f}, the game's bound x {:.0f}..{:.0f} y {:.0f}..{:.0f} z {:.0f}..{:.0f} "
                                   "- {:.1f} times its bound's area",
                    a_b.owner, model ? model : "-", a_b.from, a_b.kind == kLand ? "landscape" : a_b.kind == kWater ? "water" : "objects", a_b.box[0], a_b.box[3], a_b.box[1], a_b.box[4],
                    a_b.box[2], a_b.box[5], a_b.bound[0], a_b.bound[3], a_b.bound[1], a_b.bound[4], a_b.bound[2], a_b.bound[5], a_b.ratio);
            };
            std::string text = std::format("map 3d: round the character ({:.0f} {:.0f} {:.0f}), {} things on the map; the biggest as decoded:", at.x, at.y, at.z, all.size());
            std::ranges::sort(all, [](const Big& a, const Big& b) { return a.area > b.area; });
            for (std::size_t i = 0; i < all.size() && i < 12; ++i) {
                text += line(all[i]);
            }
            // read far bigger than the game has them (more than 4 times the area): the likely culprits of a covered map
            std::ranges::sort(all, [](const Big& a, const Big& b) { return a.ratio > b.ratio; });
            text += "\n  decoded far bigger than their bound:";
            int odd = 0;
            for (std::size_t i = 0; i < all.size() && odd < 12 && all[i].ratio > 4.0f; ++i, ++odd) {
                text += line(all[i]);
            }
            if (odd == 0) {
                text += " none";
            }
            logger::info("{}", text);
        }

        // the detailed log: the fog on the GPU since the last report
        // the detailed log: what each picture holds - how much of it something was drawn on, and its average colour
        struct PicDiag
        {
            std::atomic<int>   covered = -1;  // per mille of its pixels drawn on (-1: not read yet)
            std::atomic<int>   rgb = 0;
            std::atomic<int>   w = 0, h = 0;
            std::atomic<float> rows[4] = {};  // the view's first row and the depth row's z (NaN: a broken view)
        } picDiag[2];

        void PictureProbe(ID3D11Device* a_device, ID3D11DeviceContext* a_context, Target& a_t, int a_slot)
        {
            auto&       probe = r.picProbe[a_slot];
            auto&       size = r.picProbeSize[a_slot];
            auto&       pending = r.picPending[a_slot];
            if (probe && (size[0] != a_t.width || size[1] != a_t.height)) {
                probe->Release();
                probe = nullptr;
                pending = false;
            }
            if (!probe) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = static_cast<UINT>(a_t.width);
                d.Height = static_cast<UINT>(a_t.height);
                d.MipLevels = d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(a_device->CreateTexture2D(&d, nullptr, &probe))) {
                    return;
                }
                size[0] = a_t.width;
                size[1] = a_t.height;
            }
            if (pending) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(a_context->Map(probe, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                    std::uint64_t drawn = 0, total = 0, sum[3]{};
                    for (int y = 0; y < size[1]; y += 4) {
                        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
                        for (int x = 0; x < size[0]; x += 4) {
                            const auto* p = row + x * 4;
                            ++total;
                            if (p[3] > 8) {
                                ++drawn;
                                sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                            }
                        }
                    }
                    a_context->Unmap(probe, 0);
                    pending = false;
                    auto& d = picDiag[a_slot];
                    d.covered = total ? static_cast<int>(drawn * 1000 / total) : 0;
                    d.rgb = drawn ? static_cast<int>(((sum[0] / drawn) << 16) | ((sum[1] / drawn) << 8) | (sum[2] / drawn)) : 0;
                    d.w = size[0];
                    d.h = size[1];
                }
                return;
            }
            static std::chrono::steady_clock::time_point next[2]{};
            if (std::chrono::steady_clock::now() < next[a_slot]) {
                return;
            }
            next[a_slot] = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            a_context->CopyResource(probe, a_t.resolved);
            pending = true;
        }

        struct FogDiag
        {
            std::atomic<int> wanted = 0, off = 0, drawn = 0, uploads = 0;
            std::atomic<int> cpu = -1, gpu = -1;  // the texel under the character: as sent up, as the GPU holds it (-1: not read)
            std::atomic<int> x = 0, y = 0;        // ...where in the window
        } fogDiag;

        // the texel under the character read back from the fog's texture, a frame later (no stall): what the shader reads
        void FogProbe(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const View& a_view)
        {
            if (!r.fogProbe) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = d.Height = 1;
                d.MipLevels = d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(a_device->CreateTexture2D(&d, nullptr, &r.fogProbe))) {
                    return;
                }
            }
            if (r.probePending) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(a_context->Map(r.fogProbe, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
                    fogDiag.gpu = *static_cast<const std::uint8_t*>(mapped.pData);
                    a_context->Unmap(r.fogProbe, 0);
                    r.probePending = false;
                }
                return;
            }
            static auto next = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() < next || r.fogWindow.unit <= 0.0f) {
                return;
            }
            next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            const int x = static_cast<int>(std::floor(a_view.player.x / r.fogWindow.unit)) - r.fogWindow.ox;
            const int y = static_cast<int>(std::floor(a_view.player.y / r.fogWindow.unit)) - r.fogWindow.oy;
            fogDiag.x = x;
            fogDiag.y = y;
            if (x < 0 || y < 0 || x >= Fog::kTex || y >= Fog::kTex) {
                fogDiag.cpu = fogDiag.gpu = -2;  // the character outside the window
                return;
            }
            fogDiag.cpu = r.fogTexels[static_cast<std::size_t>(y) * Fog::kTex + x];
            const D3D11_BOX box{ static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + 1), static_cast<UINT>(y + 1), 1 };
            a_context->CopySubresourceRegion(r.fogProbe, 0, 0, 0, 0, r.fogTex, 0, &box);
            r.probePending = true;
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
            std::size_t placed = 0, triangles = 0, shared = 0, onGpu = 0, waiting = 0, failed = 0, bytes = 0, queued = 0;
            {
                std::scoped_lock guard(meshLock);
                for (const auto& in : instances) {
                    if (in.model) {
                        ++placed;
                        triangles += in.triangles;
                    }
                }
                for (const auto& [key, m] : models) {
                    shared += (key & 1) ? 1 : 0;
                    onGpu += m->vb ? 1 : 0;
                    waiting += m->state == kDecoding ? 1 : 0;
                    failed += m->state == kFailed ? 1 : 0;
                    bytes += m->bytes;
                }
                queued = jobs.size() + inFlight.size();
            }
            const int   renders = perf.renders.exchange(0), redraws = perf.redraws.exchange(0), uploads = perf.uploads.exchange(0);
            const auto  renderUs = perf.renderUs.exchange(0), renderMaxUs = perf.renderMaxUs.exchange(0);
            const float frames = static_cast<float>(std::max(perf.frames, 1));
            logger::info("perf: {:.0f} s, {} frames ({:.0f} fps): minimap update {:.2f} ms a frame (max {:.2f}), gathering {:.2f} ms a frame (max {:.2f}); "
                         "{} references taken, {} models made, {} sweeps; pictures drawn in {} of {} frames, {:.2f} ms CPU each (max {:.2f}), {} models sent up; "
                         "{} placed ({}k triangles), {} models ({} shared, {} on the GPU, {} MB, {} being read, {} failed), GPU reads waiting {}",
                perf.since, perf.frames, frames / perf.since, perf.mainMs / frames, perf.mainMax, perf.workMs / frames, perf.workMax, perf.refs, perf.models, perf.passes,
                redraws, renders, redraws ? renderUs / 1000.0 / redraws : 0.0, renderMaxUs / 1000.0, uploads, placed, triangles / 1000, models.size(), shared, onGpu,
                bytes >> 20, waiting, failed, queued);
            logger::info("fog (gpu): texture {}; pictures with the fog {}, without {}; drawn with it {}, {} uploads; the window: space {:08X}, square {} {} at {:.0f} units, version {}; "
                         "the square under the character ({} {}): sent up {}, the GPU holds {} (-1 not read yet, -2 outside the window)",
                r.fogTex ? "made" : "MISSING", fogDiag.wanted.exchange(0), fogDiag.off.exchange(0), fogDiag.drawn.exchange(0), fogDiag.uploads.exchange(0), r.fogWindow.space,
                r.fogWindow.ox, r.fogWindow.oy, r.fogWindow.unit, r.fogWindow.version, fogDiag.x.load(), fogDiag.y.load(), fogDiag.cpu.load(), fogDiag.gpu.load());
            logger::info("{}", Fog::Report());
            for (int slot = 0; slot < 2; ++slot) {
                const auto& d = picDiag[slot];
                const int   rgb = d.rgb;
                logger::info("map 3d: the {} picture {}x{}: {} drawn on{} (average colour {:02X}{:02X}{:02X}); its view {:.6f} {:.6f} {:.6f} {:.4f}; the GPU {}", slot ? "local map's" : "minimap's",
                    d.w.load(), d.h.load(), d.covered < 0 ? std::string("not read yet") : std::format("{:.1f}%", d.covered / 10.0f), d.covered == 0 ? " - NOTHING" : "", (rgb >> 16) & 0xFF,
                    (rgb >> 8) & 0xFF, rgb & 0xFF, d.rows[0].load(), d.rows[1].load(), d.rows[2].load(), d.rows[3].load(), r.adapter.empty() ? std::string("?") : r.adapter);
            }
            LogBiggest();
            perf.since = 0.0f;
            perf.frames = perf.refs = perf.models = perf.passes = 0;
            perf.mainMs = perf.mainMax = perf.workMs = perf.workMax = 0.0;
        }
    }

    Stats Live()
    {
        std::scoped_lock guard(statsLock);
        return liveStats;
    }

    void NoteFrame(float a_ms)
    {
        perf.mainMs += a_ms;
        perf.mainMax = std::max(perf.mainMax, static_cast<double>(a_ms));
        ++live.frames;
        live.updateMs += a_ms;
        live.updateMax = std::max(live.updateMax, static_cast<double>(a_ms));
        TakeLive();
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
                {
                    std::scoped_lock guard(meshLock);
                    DropAllGeometry();
                }
                roads.clear();
                roadSigs.clear();
                roadsChanged = true;
                logger::info("map 3d: map off, geometry unloaded");
            }
            if (a_radius < 0.0f) {
                TakeDecoded();  // the shapes the worker still held go (what it decoded is thrown away: everything was dropped)
            }
            sinceRoads = kRoadsEvery;
            return;
        }
        hasGeometry = true;
        const auto& s = Settings::Map();
        const auto  space = SpaceOf(a_player);
        if (!space) {
            return;
        }
        StartWorker();
        ListenForLoads();
        {
            std::scoped_lock guard(meshLock);
            graveyard.clear();  // shapes the render thread is done with
        }
        Work(a_player, space);
        const auto cell = a_player->GetParentCell();
        const bool outdoors = cell && !cell->IsInteriorCell();
        // the roads: a few times a second (a cell's navmesh comes in a moment after the cell), a bit farther than the
        // map shows
        if (s.look.roads && (sinceRoads += a_delta) >= kRoadsEvery) {
            sinceRoads = 0.0f;
            HarvestRoads(a_player, space, a_radius * 1.25f + kCellSize * 0.5f);
        }
        PublishRoadMesh(space, s.look.roads && outdoors);
        mapSpace = space;
        Report(a_delta);
    }

    RE::FormID CurrentSpace()
    {
        return mapSpace;
    }

    void ReloadFilter()
    {
        auto       now = ReadFilter();
        const auto same = [](const FilterWord& a, const FilterWord& b) { return a.word == b.word && a.keep == b.keep && a.water == b.water && a.rule == b.rule; };
        const bool changed = !filterRead || !std::ranges::equal(now, filter, same);
        filter = std::move(now);
        filterRead = true;
        logger::info("map 3d: mesh filter - {} words", filter.size());
        if (changed) {
            std::scoped_lock guard(meshLock);
            DropAllGeometry();  // everything read again by the new words
        }
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
        auto&      t = r.targets[std::clamp(a_view.slot, 0, 1)];
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

        // which instances this picture draws: of this space, in range of the character, on the picture and, for objects,
        // bigger than a pixel
        const auto visible = [&](const Instance& a_in) {
            const auto m = a_in.model;
            if (a_in.space != a_view.space || !m || m->state == kFailed || (!a_view.distant && (a_in.owner == kLodLand || a_in.owner == kLodWater)) || (m->kind == kWater && (a_view.water[3] < 0.5f || a_view.under[1] >= 0.99f))) {
                return false;
            }
            const auto& b = a_in.box;
            const float nx = std::clamp(a_view.player.x, b[0], b[3]) - a_view.player.x;
            const float ny = std::clamp(a_view.player.y, b[1], b[4]) - a_view.player.y;
            if (nx * nx + ny * ny > a_view.range * a_view.range) {
                return false;
            }
            const float c[3]{ (b[0] + b[3]) * 0.5f, (b[1] + b[4]) * 0.5f, (b[2] + b[5]) * 0.5f };
            const float h[3]{ (b[3] - b[0]) * 0.5f, (b[4] - b[1]) * 0.5f, (b[5] - b[2]) * 0.5f };
            const auto& w = a_view.rows;
            const float cx = w[0][0] * c[0] + w[0][1] * c[1] + w[0][2] * c[2] + w[0][3];
            const float cy = w[1][0] * c[0] + w[1][1] * c[1] + w[1][2] * c[2] + w[1][3];
            const float ex = std::abs(w[0][0]) * h[0] + std::abs(w[0][1]) * h[1] + std::abs(w[0][2]) * h[2];
            const float ey = std::abs(w[1][0]) * h[0] + std::abs(w[1][1]) * h[1] + std::abs(w[1][2]) * h[2];
            if (std::abs(cx) - ex > 1.0f || std::abs(cy) - ey > 1.0f) {
                return false;  // off the picture
            }
            return m->kind != kObjects || ex * a_view.width >= 1.0f || ey * a_view.height >= 1.0f;  // under a pixel across: not drawn
        };

        // the fog of war round the character: up where it changed (the window moved: all of it)
        // the detailed log: what the pictures did with the fog
        ++(a_view.fog[3] > 0.0f ? fogDiag.wanted : fogDiag.off);
        if (Settings::Map().debugLog) {
            auto& d = picDiag[std::clamp(a_view.slot, 0, 1)];
            d.rows[0] = a_view.rows[0][0];
            d.rows[1] = a_view.rows[1][1];
            d.rows[2] = a_view.rows[2][2];
            d.rows[3] = a_view.rows[2][3];
        }
        const bool fogOn = a_view.fog[3] > 0.0f && r.fogTex && r.fogSrv;
        if (fogOn) {
            int rect[4]{};
            ++fogDiag.drawn;
            if (Fog::Take(a_view.space, a_view.player.x, a_view.player.y, r.fogWindow, r.fogTexels, rect)) {
                const D3D11_BOX box{ static_cast<UINT>(rect[0]), static_cast<UINT>(rect[1]), 0, static_cast<UINT>(rect[2]), static_cast<UINT>(rect[3]), 1 };
                context->UpdateSubresource(r.fogTex, 0, &box, r.fogTexels.data() + static_cast<std::size_t>(rect[1]) * Fog::kTex + rect[0], Fog::kTex, 0);
                ++fogDiag.uploads;
            }
            if (Settings::Map().debugLog) {
                FogProbe(device, context, a_view);
            }
        }

        // the picture depends on the view and on what the map holds: while neither changes (and nothing is fading in
        // or waiting to go up) the last one is shown again - a map standing still costs nothing
        std::vector<std::uint32_t> key;
        {
            const auto add = [&](const float* a_p, std::size_t a_n) {
                for (std::size_t i = 0; i < a_n; ++i) {
                    key.push_back(std::bit_cast<std::uint32_t>(a_p[i]));
                }
            };
            add(&a_view.rows[0][0], 16);
            const float values[]{ a_view.player.x, a_view.player.y, a_view.player.z, a_view.cut, a_view.cutObjects ? 1.0f : 0.0f, a_view.range, a_view.fadeTime };
            add(values, std::size(values));
            for (const auto* a : { a_view.ground, a_view.geometry, a_view.lines, a_view.shade, a_view.occluder, a_view.occluder2, a_view.roads, a_view.water, a_view.under, a_view.fog }) {
                add(a, 4);
            }
            key.insert(key.end(), { static_cast<std::uint32_t>(a_view.width), static_cast<std::uint32_t>(a_view.height), a_view.space, r.roadUploaded, a_view.distant ? 1u : 0u });
            if (fogOn) {
                key.insert(key.end(), { static_cast<std::uint32_t>(r.fogWindow.version), static_cast<std::uint32_t>(r.fogWindow.ox), static_cast<std::uint32_t>(r.fogWindow.oy),
                                        std::bit_cast<std::uint32_t>(a_view.fogNoise) });
            }
        }

        // per model the instances drawn (the GPU's copy of a model made here first, a frame's budget of bytes)
        struct InstanceData
        {
            float row[3][4];
            float extra[4];  // x: Now() it went on the map while it is still fading in (0: not fading), y: picked in the console, z: the game's distant land or water
        };
        struct Batch
        {
            Model* model;
            UINT   start = 0, count = 0;
        };
        std::vector<InstanceData>        drawn;
        std::vector<Batch>               batches[kKinds];
        std::unique_lock                 guard(meshLock);
        for (const auto object : releaseQueue) {
            object->Release();
        }
        releaseQueue.clear();
        ReadBack(device, context);
        const float now = Now();
        const float fadeTime = std::max(a_view.fadeTime, 0.01f);
        bool        settled = true;
        {
            std::size_t sent = 0;
            for (auto& [mkey, m] : models) {
                if (m->state != kReady || m->vb || m->placed.empty()) {
                    continue;
                }
                const std::size_t bytes = m->geo.verts.size() * sizeof(float) + m->geo.indices.size() * sizeof(std::uint32_t);
                if (sent > 0 && sent + bytes > kUploadBytes) {
                    settled = false;  // the rest next frame
                    break;
                }
                D3D11_BUFFER_DESC      vd{ static_cast<UINT>(m->geo.verts.size() * sizeof(float)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
                D3D11_SUBRESOURCE_DATA vdata{ m->geo.verts.data(), 0, 0 };
                D3D11_BUFFER_DESC      idesc{ static_cast<UINT>(m->geo.indices.size() * sizeof(std::uint32_t)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
                D3D11_SUBRESOURCE_DATA idata{ m->geo.indices.data(), 0, 0 };
                if (FAILED(device->CreateBuffer(&vd, &vdata, &m->vb)) || FAILED(device->CreateBuffer(&idesc, &idata, &m->ib))) {
                    for (auto& p : { &m->vb, &m->ib }) {
                        if (*p) {
                            (*p)->Release();
                            *p = nullptr;
                        }
                    }
                    m->state = kFailed;
                    logger::warn("map 3d: could not upload a model ({} triangles)", m->triangles);
                    continue;
                }
                sent += bytes;
                m->bytes = bytes;
                m->geo = {};  // the GPU has it now
                ++perf.uploads;
                ++live.uploads;
                ++geometryVersion;
            }
        }
        // the same view of the same map as last time, nothing fading in or waiting to go up: the last picture again
        // (not even the instances looked over)
        const std::uint64_t version = geometryVersion.load();
        if (t.drawn && t.lastSettled && settled && version == t.lastSig && key == t.lastKey) {
            guard.unlock();
            ++perf.renders;
            ++live.reused;
            if (Settings::Map().debugLog) {
                PictureProbe(device, context, t, std::clamp(a_view.slot, 0, 1));
            }
            return t.srv;
        }
        // per model its instances on this picture (a model keeps the list of its own: drawn together, nothing sorted)
        for (const auto& [mkey, owned] : models) {
            const auto m = owned.get();
            if (m->placed.empty() || m->state == kFailed) {
                continue;
            }
            Batch b{ m, static_cast<UINT>(drawn.size()), 0 };
            for (const auto slot : m->placed) {
                auto& in = instances[slot];
                if (!visible(in)) {
                    continue;
                }
                if (!m->vb) {
                    settled = false;  // not up yet: drawn once it is
                    break;
                }
                if (in.shownAt <= 0.0f) {
                    in.shownAt = std::max(now, 1.0e-3f);
                }
                InstanceData d{};
                std::memcpy(d.row, in.row, sizeof(d.row));
                if (now - in.shownAt < fadeTime) {
                    d.extra[0] = in.shownAt;
                    settled = false;
                }
                if (in.owner != 0 && in.owner == highlighted.load()) {
                    d.extra[1] = 1.0f;
                }
                if (in.owner == kLodLand || in.owner == kLodWater) {
                    d.extra[2] = 1.0f;
                    if (in.model->kind == kLand) {
                        // a block's level by its size (4, 8, 16, 32 cells across): a coarser one lies lower
                        const float half = (in.box[3] - in.box[0]) * 0.5f;
                        d.extra[3] = half < 20000.0f ? 1.0f : half < 40000.0f ? 2.5f : half < 75000.0f ? 5.0f : 10.0f;
                    }
                }
                drawn.push_back(d);
                ++b.count;
            }
            if (b.count) {
                batches[std::clamp(m->kind, 0, kKinds - 1)].push_back(b);
            }
        }
        t.lastKey = std::move(key);
        t.lastSig = version;
        t.lastSettled = settled;

        StateBackup backup;
        backup.Save(context);

        // the instances' data: one buffer for all, grown when too small
        if (!drawn.empty() && drawn.size() > r.instanceCapacity) {
            if (r.instanceVb) {
                r.instanceVb->Release();
                r.instanceVb = nullptr;
            }
            const UINT        capacity = static_cast<UINT>(std::max<std::size_t>(drawn.size() * 3 / 2, 4096));
            D3D11_BUFFER_DESC id{ capacity * static_cast<UINT>(sizeof(InstanceData)), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
            r.instanceCapacity = SUCCEEDED(device->CreateBuffer(&id, nullptr, &r.instanceVb)) ? capacity : 0;
        }
        bool instancesUp = false;
        if (!drawn.empty() && r.instanceVb) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(r.instanceVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, drawn.data(), drawn.size() * sizeof(InstanceData));
                context->Unmap(r.instanceVb, 0);
                instancesUp = true;
            }
        }

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
        c.occluder2[2] = a_view.cutObjects ? 1.0f : 0.0f;
        c.timing[0] = now;
        c.timing[1] = fadeTime;
        const bool roadsOn = r.roadCorners > 0 && a_view.roads[3] > 0.0f;
        c.roadBox[0] = std::max(a_view.roads[3] - 1.0f, 0.0f);  // widened by this many world units either side
        c.roadBox[3] = roadsOn ? 1.0f : 0.0f;
        std::memcpy(c.roadLook, a_view.roads, sizeof(c.roadLook));
        std::memcpy(c.water, a_view.water, sizeof(c.water));
        std::memcpy(c.under, a_view.under, sizeof(c.under));
        if (fogOn) {
            std::memcpy(c.fog, a_view.fog, sizeof(c.fog));
            c.fogMap[0] = static_cast<float>(r.fogWindow.ox) * r.fogWindow.unit;
            c.fogMap[1] = static_cast<float>(r.fogWindow.oy) * r.fogWindow.unit;
            c.fogMap[2] = std::max(r.fogWindow.unit, 1.0f);
            c.fogMap[3] = a_view.fogNoise;
        }
        c.fogView[0] = static_cast<float>(a_view.width);
        c.fogView[1] = static_cast<float>(a_view.height);
        {
            // the cells round the character whose own landscape is on this picture, faded in: the distant land gives way there
            const int ox = static_cast<int>(std::floor(a_view.player.x / kCellSize)) - 16, oy = static_cast<int>(std::floor(a_view.player.y / kCellSize)) - 16;
            c.landOrigin[0] = static_cast<float>(ox);
            c.landOrigin[1] = static_cast<float>(oy);
            std::uint8_t mask[32 * 32]{};
            for (const auto& in : instances) {
                if (!in.model || in.model->kind != kLand || in.owner == kLodLand || in.space != a_view.space || in.shownAt <= 0.0f || now - in.shownAt < fadeTime) {
                    continue;
                }
                const int cx = static_cast<int>(std::floor((in.box[0] + in.box[3]) * 0.5f / kCellSize)) - ox;
                const int cy = static_cast<int>(std::floor((in.box[1] + in.box[4]) * 0.5f / kCellSize)) - oy;
                if (cx >= 0 && cx < 32 && cy >= 0 && cy < 32) {
                    mask[cy * 32 + cx] = 255;
                }
            }
            if (r.landTex) {
                context->UpdateSubresource(r.landTex, 0, nullptr, mask, 32, 0);
            }
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(r.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &c, sizeof(c));
            context->Unmap(r.cb, 0);
        }

        const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(a_view.width), static_cast<float>(a_view.height), 0.0f, 1.0f };
        context->RSSetViewports(1, &viewport);
        context->RSSetState(r.raster);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->GSSetShader(nullptr, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &r.cb);
        context->PSSetConstantBuffers(0, 1, &r.cb);
        constexpr UINT kStride = sizeof(float) * 3, kOffset = 0;

        // first the roads into the road mask (a pixel: covered, the road's height - the upper of two), exact at any zoom
        const float none[4]{ 0.0f, -1.0e6f, 0.0f, 0.0f };
        context->ClearRenderTargetView(t.roadMaskRtv, none);
        if (roadsOn) {
            context->IASetInputLayout(r.roadLayout);
            context->VSSetShader(r.vsRoad, nullptr, 0);
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
        context->IASetInputLayout(r.layout);
        context->VSSetShader(r.vs, nullptr, 0);
        context->PSSetShader(r.ps, nullptr, 0);
        context->PSSetConstantBuffers(1, 1, &r.kindCb);
        ID3D11ShaderResourceView* const resources[3]{ t.roadMaskSrv, r.fogSrv, r.landSrv };
        context->PSSetShaderResources(0, 3, resources);
        std::size_t drawnTriangles = 0, drawCalls = 0;
        if (instancesUp) {
            constexpr UINT kInstanceStride = sizeof(InstanceData);
            context->IASetVertexBuffers(1, 1, &r.instanceVb, &kInstanceStride, &kOffset);
            // objects first, then the landscape, then water (land above the water level hides it: the shore draws
            // itself); each with its own look (b1 says which)
            for (int k = 0; k < kKinds; ++k) {
                if (batches[k].empty()) {
                    continue;
                }
                const float              kindValue[4]{ static_cast<float>(k), 0.0f, 0.0f, 0.0f };
                D3D11_MAPPED_SUBRESOURCE kindMap{};
                if (SUCCEEDED(context->Map(r.kindCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &kindMap))) {
                    std::memcpy(kindMap.pData, kindValue, sizeof(kindValue));
                    context->Unmap(r.kindCb, 0);
                }
                for (const auto& b : batches[k]) {
                    context->IASetVertexBuffers(0, 1, &b.model->vb, &kStride, &kOffset);
                    context->IASetIndexBuffer(b.model->ib, DXGI_FORMAT_R32_UINT, 0);
                    context->DrawIndexedInstanced(static_cast<UINT>(b.model->triangles * 3), b.count, 0, 0, b.start);
                    drawnTriangles += b.model->triangles * b.count;
                    ++drawCalls;
                }
            }
        }
        // the fog over the empty background (what was drawn was fogged as it was)
        if (fogOn && r.vsFog && r.psFog) {
            context->OMSetBlendState(r.fogBlend, nullptr, 0xFFFFFFFF);
            context->OMSetDepthStencilState(r.noDepth, 0);
            context->IASetInputLayout(nullptr);
            context->VSSetShader(r.vsFog, nullptr, 0);
            context->PSSetShader(r.psFog, nullptr, 0);
            context->Draw(3, 0);
            ++drawCalls;
        }
        guard.unlock();

        if (r.samples > 1) {
            context->ResolveSubresource(t.resolved, 0, t.color, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        } else {
            context->CopyResource(t.resolved, t.color);
        }
        if (Settings::Map().debugLog) {
            PictureProbe(device, context, t, std::clamp(a_view.slot, 0, 1));
        }
        backup.Restore(context);
        t.drawn = true;
        const auto us = static_cast<std::int64_t>(MsSince(renderStart) * 1000.0);
        ++perf.renders;
        ++perf.redraws;
        perf.renderUs += us;
        ++live.redraws;
        live.renderUs += us;
        for (auto most = live.renderMaxUs.load(); us > most && !live.renderMaxUs.compare_exchange_weak(most, us);) {
        }
        live.drawnInstances = drawn.size();
        live.drawnTriangles = drawnTriangles;
        live.drawCalls = drawCalls;
        for (auto most = perf.renderMaxUs.load(); us > most && !perf.renderMaxUs.compare_exchange_weak(most, us);) {
        }
        return t.srv;
    }
}
