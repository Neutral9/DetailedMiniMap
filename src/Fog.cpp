#include "Fog.h"

#include "Settings.h"

namespace Fog
{
    namespace
    {
        constexpr float kUnitOutside = 512.0f;  // world units a square: outside (a cell is 8 a side)
        constexpr float kUnitInside = 192.0f;   // ...inside (rooms are small)
        constexpr int   kBlock = 64;            // squares a side of a block (kept, packed and saved as one)

        using Block = std::array<std::uint8_t, kBlock * kBlock>;

        struct Space
        {
            bool                                              inside = false;
            std::unordered_map<std::uint64_t, std::unique_ptr<Block>> blocks;
            int                                               dirty[4]{ 0, 0, 0, 0 };  // squares changed since the window took them (x0, y0, x1, y1)
            bool                                              changed = false;
        };

        std::mutex                                lock;
        std::unordered_map<RE::FormID, Space>     spaces;
        std::uint64_t                             version = 1;
        std::uint32_t                             generation = 1;
        std::atomic_bool                          miscShown = true;  // the journal's Miscellaneous entry active (as a new game has it)
        std::unordered_map<RE::FormID, float>     searched;  // container -> the game's day it was looked into (under the lock)
        // the detailed log's counts since its last report
        struct Counts
        {
            std::uint64_t updates = 0, skipped = 0, revealing = 0, squares = 0, takes = 0, whole = 0, parts = 0;
            RE::FormID    space = 0;  // the last one revealed in
        } counts;
        constexpr float                           kSearchedDays = 10.0f;  // as long as the game takes to refill one (a cell's reset)

        int FloorDiv(int a_v, int a_d)
        {
            return a_v >= 0 ? a_v / a_d : -((-a_v + a_d - 1) / a_d);
        }

        std::uint64_t BlockKey(int a_bx, int a_by)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_bx)) << 32) | static_cast<std::uint32_t>(a_by);
        }

        float UnitOf(bool a_inside)
        {
            return a_inside ? kUnitInside : kUnitOutside;
        }

        // a square's value (0 where nothing was ever seen)
        std::uint8_t At(const Space& a_s, int a_x, int a_y)
        {
            const int  bx = FloorDiv(a_x, kBlock), by = FloorDiv(a_y, kBlock);
            const auto it = a_s.blocks.find(BlockKey(bx, by));
            return it == a_s.blocks.end() ? 0 : (*it->second)[(a_y - by * kBlock) * kBlock + (a_x - bx * kBlock)];
        }

        std::uint8_t& Cell(Space& a_s, int a_x, int a_y)
        {
            const int bx = FloorDiv(a_x, kBlock), by = FloorDiv(a_y, kBlock);
            auto&     block = a_s.blocks[BlockKey(bx, by)];
            if (!block) {
                block = std::make_unique<Block>();
                block->fill(0);
            }
            return (*block)[(a_y - by * kBlock) * kBlock + (a_x - bx * kBlock)];
        }

        void MarkDirty(Space& a_s, int a_x, int a_y)
        {
            if (!a_s.changed) {
                a_s.dirty[0] = a_s.dirty[2] = a_x;
                a_s.dirty[1] = a_s.dirty[3] = a_y;
                a_s.changed = true;
            }
            a_s.dirty[0] = std::min(a_s.dirty[0], a_x);
            a_s.dirty[1] = std::min(a_s.dirty[1], a_y);
            a_s.dirty[2] = std::max(a_s.dirty[2], a_x + 1);
            a_s.dirty[3] = std::max(a_s.dirty[3], a_y + 1);
        }

        // ---- the co-save: a record a space - its form, inside or not, its blocks (each run-length packed: a value, then
        // how many of it as a varint)
        constexpr std::uint32_t kUniqueId = 'DMMF';
        constexpr std::uint32_t kRecord = 'FOGS';
        constexpr std::uint32_t kVersion = 1;
        constexpr std::uint32_t kMiscRecord = 'MISC';
        constexpr std::uint32_t kSearchedRecord = 'SRCH';

        void PutVarint(std::vector<std::uint8_t>& a_out, std::uint32_t a_v)
        {
            while (a_v >= 0x80) {
                a_out.push_back(static_cast<std::uint8_t>(a_v | 0x80));
                a_v >>= 7;
            }
            a_out.push_back(static_cast<std::uint8_t>(a_v));
        }

        bool GetVarint(const std::uint8_t*& a_p, const std::uint8_t* a_end, std::uint32_t& a_v)
        {
            a_v = 0;
            for (int shift = 0; a_p < a_end && shift < 35; shift += 7) {
                const std::uint8_t b = *a_p++;
                a_v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
                if (!(b & 0x80)) {
                    return true;
                }
            }
            return false;
        }

        void Pack(const Block& a_b, std::vector<std::uint8_t>& a_out)
        {
            for (std::size_t i = 0; i < a_b.size();) {
                std::size_t j = i + 1;
                while (j < a_b.size() && a_b[j] == a_b[i]) {
                    ++j;
                }
                a_out.push_back(a_b[i]);
                PutVarint(a_out, static_cast<std::uint32_t>(j - i));
                i = j;
            }
        }

        bool Unpack(const std::uint8_t*& a_p, const std::uint8_t* a_end, Block& a_b)
        {
            for (std::size_t i = 0; i < a_b.size();) {
                if (a_p >= a_end) {
                    return false;
                }
                const std::uint8_t value = *a_p++;
                std::uint32_t      run = 0;
                if (!GetVarint(a_p, a_end, run) || run == 0 || i + run > a_b.size()) {
                    return false;
                }
                std::fill_n(a_b.begin() + i, run, value);
                i += run;
            }
            return true;
        }

        void OnSave(SKSE::SerializationInterface* a_intfc)
        {
            std::scoped_lock          guard(lock);
            std::size_t               total = 0, written = 0;
            std::vector<std::uint8_t> data;
            for (const auto& [id, s] : spaces) {
                data.clear();
                std::uint32_t count = 0;
                std::vector<std::uint8_t> packed;
                for (const auto& [key, block] : s.blocks) {
                    if (std::ranges::all_of(*block, [](std::uint8_t v) { return v == 0; })) {
                        continue;
                    }
                    const auto bx = static_cast<std::int32_t>(key >> 32), by = static_cast<std::int32_t>(key & 0xFFFFFFFFu);
                    for (const auto v : { static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by) }) {
                        PutVarint(packed, (v << 1) ^ static_cast<std::uint32_t>(static_cast<std::int32_t>(v) >> 31));  // zigzag
                    }
                    Pack(*block, packed);
                    ++count;
                }
                if (!count) {
                    continue;
                }
                data.push_back(s.inside ? 1 : 0);
                PutVarint(data, count);
                data.insert(data.end(), packed.begin(), packed.end());
                const std::uint32_t form = id;
                if (!a_intfc->OpenRecord(kRecord, kVersion) || !a_intfc->WriteRecordData(form) ||
                    !a_intfc->WriteRecordData(data.data(), static_cast<std::uint32_t>(data.size()))) {
                    logger::error("fog: could not write the fog of {:08X}", id);
                    continue;
                }
                total += data.size() + 4;
                ++written;
            }
            {
                // the containers looked into: a form and a day each (only the plugins' own: the game's made ones change)
                std::vector<std::pair<RE::FormID, float>> list;
                for (const auto& [ref, day] : searched) {
                    if ((ref >> 24) != 0xFF) {
                        list.emplace_back(ref, day);
                    }
                }
                const auto count = static_cast<std::uint32_t>(list.size());
                bool       ok = a_intfc->OpenRecord(kSearchedRecord, 1) && a_intfc->WriteRecordData(count);
                for (const auto& [ref, day] : list) {
                    ok = ok && a_intfc->WriteRecordData(ref) && a_intfc->WriteRecordData(day);
                }
                if (!ok) {
                    logger::error("co-save: the containers looked into could not be written");
                }
            }
            const std::uint8_t misc = miscShown ? 1 : 0;
            if (!a_intfc->OpenRecord(kMiscRecord, 1) || !a_intfc->WriteRecordData(misc)) {
                logger::error("co-save: the journal's Miscellaneous state could not be written");
            }
            logger::info("fog: saved {} spaces, {} bytes", written, total);
        }

        void OnLoad(SKSE::SerializationInterface* a_intfc)
        {
            std::scoped_lock guard(lock);
            spaces.clear();
            ++version;
            ++generation;
            miscShown = true;
            searched.clear();
            std::uint32_t type = 0, ver = 0, length = 0;
            std::size_t   loaded = 0;
            while (a_intfc->GetNextRecordInfo(type, ver, length)) {
                if (type == kSearchedRecord && length >= 4) {
                    std::uint32_t count = 0;
                    a_intfc->ReadRecordData(count);
                    for (std::uint32_t i = 0; i < count && (i + 1) * 8 + 4 <= length; ++i) {
                        RE::FormID ref = 0, id = 0;
                        float      day = 0.0f;
                        a_intfc->ReadRecordData(ref);
                        a_intfc->ReadRecordData(day);
                        if (a_intfc->ResolveFormID(ref, id)) {
                            searched[id] = day;
                        }
                    }
                    continue;
                }
                if (type == kMiscRecord && length >= 1) {
                    std::uint8_t misc = 1;
                    a_intfc->ReadRecordData(misc);
                    miscShown = misc != 0;
                    continue;
                }
                if (type != kRecord || ver != kVersion || length < 5) {
                    continue;
                }
                std::uint32_t form = 0;
                a_intfc->ReadRecordData(form);
                std::vector<std::uint8_t> data(length - 4);
                if (a_intfc->ReadRecordData(data.data(), static_cast<std::uint32_t>(data.size())) != data.size()) {
                    continue;
                }
                RE::FormID id = 0;
                if (!a_intfc->ResolveFormID(form, id)) {
                    continue;  // its plugin gone
                }
                const std::uint8_t* p = data.data();
                const std::uint8_t* end = p + data.size();
                Space               s;
                s.inside = *p++ != 0;
                std::uint32_t count = 0;
                bool          ok = GetVarint(p, end, count);
                for (std::uint32_t i = 0; ok && i < count; ++i) {
                    std::uint32_t zx = 0, zy = 0;
                    ok = GetVarint(p, end, zx) && GetVarint(p, end, zy);
                    const auto bx = static_cast<std::int32_t>((zx >> 1) ^ (0u - (zx & 1))), by = static_cast<std::int32_t>((zy >> 1) ^ (0u - (zy & 1)));
                    auto       block = std::make_unique<Block>();
                    ok = ok && Unpack(p, end, *block);
                    if (ok) {
                        s.blocks[BlockKey(bx, by)] = std::move(block);
                    }
                }
                if (!ok) {
                    logger::warn("fog: the fog of {:08X} could not be read", id);
                }
                spaces[id] = std::move(s);
                ++loaded;
            }
            logger::info("fog: loaded {} spaces", loaded);
        }

        void OnRevert(SKSE::SerializationInterface*)
        {
            std::scoped_lock guard(lock);
            miscShown = true;
            searched.clear();
            spaces.clear();
            ++version;
            ++generation;
        }
    }

    void Register()
    {
        const auto s = SKSE::GetSerializationInterface();
        s->SetUniqueID(kUniqueId);
        s->SetSaveCallback(OnSave);
        s->SetLoadCallback(OnLoad);
        s->SetRevertCallback(OnRevert);
    }

    bool MiscShown()
    {
        return miscShown;
    }

    void NoteMiscShown(bool a_shown)
    {
        miscShown = a_shown;
    }

    // the game's day now (0 before a game is up)
    float Today()
    {
        const auto calendar = RE::Calendar::GetSingleton();
        return calendar ? calendar->GetDaysPassed() : 0.0f;
    }

    bool Searched(RE::FormID a_ref)
    {
        std::scoped_lock guard(lock);
        const auto       it = searched.find(a_ref);
        return it != searched.end() && Today() - it->second < kSearchedDays;
    }

    void Clear()
    {
        std::scoped_lock guard(lock);
        spaces.clear();
        ++version;
        ++generation;  // the pictures' window taken whole again
        logger::info("fog: cleared from the menu");
    }

    void NoteSearched(RE::FormID a_ref)
    {
        const float      today = Today();
        std::scoped_lock guard(lock);
        searched[a_ref] = today;
    }

    void Update(RE::PlayerCharacter* a_player, float a_delta)
    {
        const auto cell = a_player ? a_player->GetParentCell() : nullptr;
        if (!cell || a_delta <= 0.0f) {
            std::scoped_lock guard(lock);
            ++counts.skipped;
            return;
        }
        const auto& m = Settings::Map();
        const bool  inside = cell->IsInteriorCell();
        const auto  world = inside ? nullptr : a_player->GetWorldspace();
        const auto  id = inside || !world ? cell->GetFormID() : world->GetFormID();
        const float unit = UnitOf(inside);
        const float radius = std::max(inside ? m.fogRadiusInside : m.fogRadius, unit);
        const float inner = radius * 0.5f;
        // how much a square clears a frame: all of it over the time set
        const float fade = m.look.fogFade;
        const int   step = fade <= 0.01f ? 255 : std::max(1, static_cast<int>(std::ceil(255.0f * a_delta / fade)));
        const auto  pos = a_player->GetPosition();

        std::scoped_lock guard(lock);
        auto&            s = spaces[id];
        ++counts.updates;
        counts.space = id;
        s.inside = inside;
        const int  x0 = static_cast<int>(std::floor((pos.x - radius) / unit)), x1 = static_cast<int>(std::floor((pos.x + radius) / unit));
        const int  y0 = static_cast<int>(std::floor((pos.y - radius) / unit)), y1 = static_cast<int>(std::floor((pos.y + radius) / unit));
        bool       any = false;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) * unit - pos.x, dy = (static_cast<float>(y) + 0.5f) * unit - pos.y;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d >= radius) {
                    continue;
                }
                // all of it near, less towards the edge (gone over again nearer: the rest)
                const float t = std::clamp((d - inner) / (radius - inner), 0.0f, 1.0f);
                const int   target = static_cast<int>(255.0f * (1.0f - t * t * (3.0f - 2.0f * t)));
                if (target <= 0 || At(s, x, y) >= target) {
                    continue;
                }
                auto& v = Cell(s, x, y);
                v = static_cast<std::uint8_t>(std::min(target, v + step));
                MarkDirty(s, x, y);
                any = true;
                ++counts.squares;
            }
        }
        if (any) {
            ++version;
            ++counts.revealing;
        }
    }

    float Seen(RE::FormID a_space, float a_x, float a_y)
    {
        std::scoped_lock guard(lock);
        const auto       it = spaces.find(a_space);
        if (it == spaces.end()) {
            return 0.0f;
        }
        const auto& s = it->second;
        const float unit = UnitOf(s.inside);
        const float fx = a_x / unit - 0.5f, fy = a_y / unit - 0.5f;
        const int   x = static_cast<int>(std::floor(fx)), y = static_cast<int>(std::floor(fy));
        const float tx = fx - static_cast<float>(x), ty = fy - static_cast<float>(y);
        const float top = std::lerp(static_cast<float>(At(s, x, y)), static_cast<float>(At(s, x + 1, y)), tx);
        const float bottom = std::lerp(static_cast<float>(At(s, x, y + 1)), static_cast<float>(At(s, x + 1, y + 1)), tx);
        return std::lerp(top, bottom, ty) / 255.0f;
    }

    std::string Report()
    {
        const auto& m = Settings::Map();
        const auto  player = RE::PlayerCharacter::GetSingleton();
        const auto  cell = player ? player->GetParentCell() : nullptr;
        const bool  inside = cell && cell->IsInteriorCell();
        const auto  world = cell && !inside ? player->GetWorldspace() : nullptr;
        const auto  id = !cell ? 0 : inside || !world ? cell->GetFormID() : world->GetFormID();
        const auto  pos = player ? player->GetPosition() : RE::NiPoint3{};
        const float seen = cell ? Seen(id, pos.x, pos.y) : -1.0f;
        std::scoped_lock guard(lock);
        const auto       it = spaces.find(id);
        std::size_t      blocks = 0, kept = 0;
        for (const auto& [sid, s] : spaces) {
            kept += s.blocks.size();
        }
        if (it != spaces.end()) {
            blocks = it->second.blocks.size();
        }
        auto text = std::format("fog (cpu): on {}, minimap {}, local map {}, radius {:.0f} / {:.0f} inside, opacity {:.2f}, clears in {:.2f} s; the character in {:08X} ({}) at {:.0f} {:.0f}, "
                                "seen there {:.2f}; {} spaces, {} blocks ({} here); since the last report: {} updates ({} skipped: no cell or no time), {} revealing {} squares (last in {:08X}), "
                                "{} takes ({} whole, {} in part)",
            m.fog, m.fogMinimap, m.fogLocal, m.fogRadius, m.fogRadiusInside, m.look.fogOpacity, m.look.fogFade, id, inside ? "inside" : "outside", pos.x, pos.y, seen,
            spaces.size(), kept, blocks, counts.updates, counts.skipped, counts.revealing, counts.squares, counts.space, counts.takes, counts.whole, counts.parts);
        counts = {};
        return text;
    }

    bool Take(RE::FormID a_space, float a_x, float a_y, Window& a_io, std::vector<std::uint8_t>& a_texels, int (&a_rect)[4])
    {
        std::scoped_lock guard(lock);
        const auto       it = spaces.find(a_space);
        const bool       inside = it != spaces.end() ? it->second.inside : a_io.space == a_space && a_io.unit == kUnitInside;
        const float      unit = UnitOf(inside);
        // the window: round the character, by whole blocks (it moves seldom)
        const int ox = FloorDiv(static_cast<int>(std::floor(a_x / unit)), kBlock) * kBlock - kTex / 2 + kBlock / 2;
        const int oy = FloorDiv(static_cast<int>(std::floor(a_y / unit)), kBlock) * kBlock - kTex / 2 + kBlock / 2;
        a_texels.resize(static_cast<std::size_t>(kTex) * kTex);
        ++counts.takes;
        const bool whole = a_io.space != a_space || a_io.ox != ox || a_io.oy != oy || a_io.unit != unit || a_io.generation != generation;
        if (!whole && a_io.version == version) {
            return false;
        }
        int rect[4]{ 0, 0, kTex, kTex };
        if (!whole) {
            if (it == spaces.end() || !it->second.changed) {
                a_io.version = version;
                return false;
            }
            const auto& d = it->second.dirty;
            rect[0] = std::clamp(d[0] - ox, 0, kTex);
            rect[1] = std::clamp(d[1] - oy, 0, kTex);
            rect[2] = std::clamp(d[2] - ox, 0, kTex);
            rect[3] = std::clamp(d[3] - oy, 0, kTex);
        }
        if (it != spaces.end()) {
            it->second.changed = false;
        }
        a_io = { a_space, ox, oy, unit, version, generation };
        if (rect[2] <= rect[0] || rect[3] <= rect[1]) {
            return false;
        }
        ++(whole ? counts.whole : counts.parts);
        // a block at a time: found once, its rows copied
        for (int by = FloorDiv(oy + rect[1], kBlock); by <= FloorDiv(oy + rect[3] - 1, kBlock); ++by) {
            for (int bx = FloorDiv(ox + rect[0], kBlock); bx <= FloorDiv(ox + rect[2] - 1, kBlock); ++bx) {
                const Block* block = nullptr;
                if (it != spaces.end()) {
                    const auto found = it->second.blocks.find(BlockKey(bx, by));
                    block = found != it->second.blocks.end() ? found->second.get() : nullptr;
                }
                const int x0 = std::max(bx * kBlock - ox, rect[0]), x1 = std::min((bx + 1) * kBlock - ox, rect[2]);
                const int y0 = std::max(by * kBlock - oy, rect[1]), y1 = std::min((by + 1) * kBlock - oy, rect[3]);
                for (int y = y0; y < y1; ++y) {
                    auto* row = a_texels.data() + static_cast<std::size_t>(y) * kTex + x0;
                    if (block) {
                        std::memcpy(row, block->data() + (oy + y - by * kBlock) * kBlock + (ox + x0 - bx * kBlock), static_cast<std::size_t>(x1 - x0));
                    } else {
                        std::memset(row, 0, static_cast<std::size_t>(x1 - x0));
                    }
                }
            }
        }
        std::copy(std::begin(rect), std::end(rect), a_rect);
        return true;
    }
}
