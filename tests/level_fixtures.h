// Scenes, worlds and texture directories made for the custom level tests, shaped like the game's own; none of its data is used.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "jpt_hash.h"

namespace fixtures {

inline std::string U32(uint32_t v) {
    std::string out;
    for (int i = 0; i < 4; ++i) out += char(v >> (8 * i));
    return out;
}

inline std::string F32(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return U32(bits);
}

using Sections = std::vector<std::pair<std::string, std::string>>;

inline std::string ContainerHead(uint32_t total, uint32_t sections, uint32_t symbols, uint32_t symbol_bytes, uint32_t symbols_at) {
    return U32(0x0ACEBABE) + U32(total) + U32(sections) + U32(symbols) + U32(symbol_bytes) + U32(symbols_at) + U32(0) + U32(0) + U32(10) + std::string(12, '\0');
}

// A scene or a save: sections under their names.
inline std::string Container(const Sections& sections) {
    std::string table, body, symbols;
    const uint32_t start = uint32_t(0x30 + sections.size() * 32);
    for (size_t i = 0; i < sections.size(); ++i) {
        table += U32(uint32_t(0x1000 + i)) + U32(7) + U32(uint32_t(start + body.size())) + U32(uint32_t(sections[i].second.size())) + std::string(16, '\0');
        body += sections[i].second;
        symbols += U32(uint32_t(0x1000 + i)) + U32(uint32_t(sections[i].first.size() + 1)) + U32(1) + sections[i].first + '\0';
    }
    const uint32_t symbols_at = uint32_t(start + body.size());
    return ContainerHead(uint32_t(symbols_at + symbols.size()), uint32_t(sections.size()), uint32_t(sections.size()), uint32_t(symbols.size()), symbols_at) + table + body + symbols;
}

struct MeshShape {
    std::string name = "Crate-00", texture = "Map\\w\\crate.bmp", bump = {};
    uint32_t polygons = 1;
    bool curved = false, model = false, curved_property = false;
    float size = 1.0f; // of its points
    uint32_t corners = 3; // of its first polygon
    bool bad_index = false, bad_surface = false;
    int animated = -1; // the surface it animates
    std::string unused = {}; // a second surface that no polygon has
    const char* type = nullptr; // its class
    int bound = -1;             // BoundVol of a trigger
    std::string mesh = {}, physics = {}; // of other objects, which it takes instead of its own
    bool skeleton = false;      // with joints, over a mesh stored as the engine builds it
    int dinosaur = -1;
};

struct PlacedObject {
    std::string name, type;
    float x = 0, y = 0, z = 0;
};

struct WorldShape {
    std::string terrain; // "x" for x.wtd; empty: no object is the terrain
    bool optimized = true;
    std::vector<MeshShape> meshes;
    std::vector<int> actions;
    std::vector<std::string> levels; // LOAD_LEVEL targets
    std::vector<std::string> maps;   // the pictures of ATX map actions
    std::vector<PlacedObject> placed;
    std::vector<std::pair<std::string, int>> hints; // a trigger and the hint its Action00 sets
    std::vector<std::pair<std::string, std::string>> scripted;         // an object and the class its Script names
    std::vector<std::pair<std::string, std::string>> scripted_actions; // a trigger and the class its Action00 (99) names
    std::vector<std::string> frames; // of an animating mesh
    uint32_t animals = 0;
    int trigger_actions = 0;       // of one trigger
    std::string expression;        // of one trigger
    std::vector<std::string> models; // of one object
    std::string models_type = "Compound";
    int models_named = -1;         // the number all of them have, instead of one after another
    uint32_t skeleton_polygons = 1;
    bool plain_meshes = false;     // stored unbuilt, without joints
    uint32_t more_objects = 0;
    std::vector<int> links;        // of a skeleton's first joint
    int joints = 4;                // of the skeleton
    std::string substitute;        // of the skeleton: an object
    std::vector<std::pair<std::string, std::string>> magnets;
    uint32_t meeting = 0;          // faces of one more mesh that meet in a point
    bool meeting_skeleton = false, planted = false;
    int normals = 0;
};

class WorldBuilder {
public:
    explicit WorldBuilder(const WorldShape& shape) {
        const uint32_t header = Add(".header", U32(0x0ACEBABE) + U32(15 | (shape.optimized ? 0x80000000u : 0)));
        (void)header;
        if (!shape.terrain.empty()) Object("Terrain-00", 0, {{"Class", Text("TerrainPlacement")}, {"File", Text(shape.terrain)}});
        for (const MeshShape& mesh : shape.meshes) Mesh(mesh, shape.plain_meshes);
        for (uint32_t i = 0; i < shape.more_objects; ++i) Object("Many-01", 0, {});
        for (uint32_t i = 0; i < shape.animals; ++i) Object("Raptor-0" + std::to_string(i), 0, {{"Class", Text("CAnimal")}});
        for (const int action : shape.actions) Object("Trigger-00", 0, {{"Class", Text("CLocationTrigger")}, {"ActionType", Whole(action)}});
        if (shape.trigger_actions || !shape.expression.empty()) {
            std::vector<std::pair<std::string, Value>> values = {{"Class", Text("CLocationTrigger")}, {"FireExpression", Text(shape.expression)}};
            for (int i = 0; i < shape.trigger_actions; ++i) values.push_back({Numbered("Action", i), {6, U32(0)}});
            Object("Many-00", 0, values);
        }
        if (!shape.models.empty()) {
            std::vector<std::pair<std::string, Value>> values;
            for (size_t i = 0; i < shape.models.size(); ++i) values.push_back({Numbered("Model", shape.models_named < 0 ? int(i) : shape.models_named), Text(shape.models[i])});
            values.push_back({"Type", Text(shape.models_type)});
            Object("Compound-00", 0, values);
        }
        if (!shape.links.empty() || !shape.substitute.empty()) {
            MeshShape body;
            body.model = true;
            body.polygons = shape.skeleton_polygons;
            std::vector<std::pair<std::string, Value>> values = {{"NumJoints", Whole(shape.joints)}}, joint;
            if (!shape.substitute.empty()) values.push_back({"A00", Text(shape.substitute)});
            Object("Skeleton-00", ModelObject(body), values);
            for (size_t i = 0; i < shape.links.size(); ++i) joint.push_back({Numbered("A", int(i)), Whole(shape.links[i])});
            Object("$JSkeleton00", 0, joint);
        }
        for (const auto& [master, slave] : shape.magnets) Object("Magnet-00", 0, {{"Class", Text("CMagnet")}, {"MasterObject", Text(master)}, {"SlaveObject", Text(slave)}});
        if (shape.meeting) {
            std::vector<std::pair<std::string, Value>> values;
            if (shape.meeting_skeleton) values.push_back({"NumJoints", Whole(1)});
            if (shape.normals) values.push_back({"Normals", Whole(shape.normals)});
            if (shape.planted) values.push_back({"Planted", Flag(true)});
            Object("Fan-00", FanObject(shape.meeting), values);
        }
        for (const std::string& level : shape.levels) Object("Leave-00", 0, {{"Class", Text("CLocationTrigger")}, {"ActionType", Whole(16)}, {"LevelName", Text(level)}});
        for (const std::string& map : shape.maps) Object("Map-00", 0, {{"Class", Text("CLocationTrigger")}, {"ActionType", Whole(102)}, {"Sample", Text(map)}});
        for (const PlacedObject& o : shape.placed) Object(o.name, 0, {{"Class", Text(o.type)}}, o.x, o.y, o.z);
        for (const auto& [name, hint] : shape.hints)
            Object(name, 0, {{"Class", Text("CLocationTrigger")}, {"Action00", Group({{"ActionType", Whole(28)}, {"HintID", Whole(hint)}})}});
        for (const auto& [name, script] : shape.scripted) Object(name, 0, {{"Class", Text("CInstance")}, {"Script", Text(script)}});
        for (const auto& [name, script] : shape.scripted_actions)
            Object(name, 0, {{"Class", Text("CLocationTrigger")}, {"Action00", Group({{"ActionType", Whole(99)}, {"ScriptAT", Text(script)}})}});
        if (!shape.frames.empty()) {
            std::vector<std::pair<std::string, Value>> values;
            for (size_t i = 0; i < shape.frames.size(); ++i) values.push_back({Numbered("Anim", int(i)), Text(shape.frames[i])});
            Object("Screen-00", MeshObject(MeshShape{}), values);
        }
    }

    std::string Bytes() {
        const uint32_t table = Add(".values", ValueTable());
        std::string region = U32(uint32_t(objects_.size() / 44)) + objects_ + U32(table);
        Add(".region", region);
        std::string entries, body, symbols;
        const uint32_t start = uint32_t(0x30 + sections_.size() * 32);
        for (const Section& s : sections_) {
            entries += U32(s.name) + U32(7) + U32(uint32_t(start + body.size())) + U32(uint32_t(s.bytes.size())) + std::string(12, '\0') + U32(s.handle);
            body += s.bytes;
        }
        for (const auto& [text, handle] : symbols_) symbols += U32(handle) + U32(uint32_t(text.size() + 1)) + U32(1) + text + '\0';
        const uint32_t at = uint32_t(start + body.size());
        return ContainerHead(uint32_t(at + symbols.size()), uint32_t(sections_.size()), uint32_t(symbols_.size()), uint32_t(symbols.size()), at) + entries + body + symbols;
    }

private:
    struct Value {
        int type;
        std::string bytes;
    };
    static std::string Numbered(const char* prefix, int i) { return std::string(prefix) + char('0' + i / 10) + char('0' + i % 10); }
    static Value Text(const std::string& text) { return {5, text + '\0'}; }
    static Value Whole(int v) { return {3, U32(uint32_t(v))}; }
    static Value Flag(bool v) { return {1, std::string(1, char(v))}; }
    static std::string Handle(uint32_t h) { return U32(h) + U32(1); }

    uint32_t Symbol(const std::string& text) {
        const auto it = symbols_.find(text);
        if (it != symbols_.end()) return it->second;
        return symbols_[text] = uint32_t(0x100 + symbols_.size());
    }

    uint32_t Add(const std::string& name, const std::string& bytes) {
        const uint32_t handle = uint32_t(0x5000 + sections_.size());
        sections_.push_back({Symbol(name), handle, bytes});
        return handle;
    }

    uint32_t Name(const std::string& text) {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == text) return uint32_t(0x900 + i);
        names_.push_back(text);
        return uint32_t(0x900 + names_.size() - 1);
    }

    uint32_t Record(const std::string& name, const Value& value) {
        const uint32_t key = 0x20000 + record_count_++;
        records_ += std::string(1, char(value.type)) + '\1' + '\0' + Handle(0) + Handle(Name(name)) + Handle(0) + value.bytes + Handle(key);
        return key;
    }

    Value Group(const std::vector<std::pair<std::string, Value>>& values) {
        std::string children;
        for (const auto& [key, value] : values) children += Handle(Record(key, value));
        return {6, U32(uint32_t(values.size())) + children};
    }

    void Object(const std::string& name, uint32_t section, const std::vector<std::pair<std::string, Value>>& values, float x = 0, float y = 0, float z = 0) {
        const uint32_t attributes = Record(name, Group(values));
        objects_ += U32(section) + U32(Symbol(name)) + F32(x) + F32(y) + F32(z) + std::string(12, '\0') + F32(1.0f) + Handle(attributes);
    }

    // Points, vertices, pointers and polygons, one triangle after another.
    uint32_t MeshObject(const MeshShape& m) {
        const uint32_t material = m.unused.empty() ? Add("material", U32(1) + U32(Symbol(m.texture)) + U32(Symbol("")) + U32(Symbol(m.bump)) + std::string(32, '\0'))
                                                   : Add("material", U32(2) + U32(Symbol(m.texture)) + U32(Symbol(m.unused)) + U32(Symbol("")) + U32(Symbol("")) + U32(Symbol(m.bump)) + U32(Symbol("")) + std::string(32, '\0'));
        const uint32_t points = m.polygons * 3;
        std::string at, vertices, pointers, polygons;
        for (uint32_t i = 0; i < points; ++i) {
            at += F32(m.size * float(i)) + F32(m.size * float(i % 2)) + F32(0.5f);
            vertices += U32(i) + std::string(12, '\0') + F32(float(i % 3 == 1)) + F32(float(i % 3 == 2)) + std::string(8, '\0');
            pointers += U32(m.bad_index && !i ? points : i);
        }
        for (uint32_t i = 0; i < m.polygons; ++i) polygons += U32(i ? 3 : m.corners) + U32(i * 3) + std::string(16, '\0') + U32(m.bad_surface ? 7 : 0) + U32(m.curved ? 4 : 0) + std::string(8, '\0');
        const std::string mesh = U32(material) + std::string(20, '\0') + U32(points) + U32(points) + U32(points) + U32(0) + U32(m.polygons) + at + vertices + pointers + polygons;
        const uint32_t geometry = Add("geometry", mesh);
        return Add("object", U32(Symbol(m.name)) + U32(geometry) + std::string(20, '\0'));
    }

    uint32_t ModelObject(const MeshShape& m) {
        const uint32_t material = Add("material", U32(1) + U32(Symbol(m.texture)) + U32(Symbol("")) + U32(Symbol(m.bump)) + std::string(32, '\0'));
        std::string points, faces, corners, ids;
        for (uint32_t i = 0; i < m.polygons * 3; ++i) points += F32(m.size * float(i)) + F32(0.25f) + F32(0.5f), corners += F32(0.25f) + F32(0.5f);
        for (uint32_t i = 0; i < m.polygons; ++i) faces += U32(i * 3) + U32(i * 3 + 1) + U32(i * 3 + 2), ids += U32(0);
        const std::string geometry = std::string(12, '\0') + U32(m.polygons * 3) + U32(m.polygons) + U32(0) + U32(0) + std::string(12, '\0') + points + faces + std::string(m.polygons * 12, '\0');
        const std::string mapping = U32(material) + U32(m.polygons * 3) + U32(m.polygons) + corners + faces + ids;
        const uint32_t shape = Add("geometry", geometry), map = Add("mapping", mapping);
        return Add("object", U32(Symbol(m.name)) + U32(shape) + U32(map) + std::string(16, '\0'));
    }

    // Faces around one point, each turned a little further.
    uint32_t FanObject(uint32_t count) {
        std::string points = F32(0) + F32(0) + F32(0), faces, corners, ids;
        for (uint32_t i = 0; i <= count; ++i) {
            const float a = 6.2831853f * float(i) / float(count + 1);
            points += F32(std::cos(a)) + F32(std::sin(a)) + F32(i % 2 ? 1.0f : 0.2f);
        }
        for (uint32_t i = 0; i < count; ++i) faces += U32(0) + U32(i + 1) + U32(i + 2);
        const std::string geometry = std::string(12, '\0') + U32(count + 2) + U32(count) + U32(0) + U32(0) + std::string(12, '\0') + points + faces + std::string(count * 12, '\0');
        return Add("object", U32(Symbol("Fan-00")) + U32(Add("geometry", geometry)) + std::string(20, '\0'));
    }

    void Mesh(const MeshShape& m, bool plain) {
        std::vector<std::pair<std::string, Value>> values = {{"Class", Text(m.type ? m.type : m.model ? "CAnimal" : "CInstance")}};
        if (m.model || m.skeleton) values.push_back({"NumJoints", Whole(4)});
        if (m.bound >= 0) values.push_back({"BoundVol", Whole(m.bound)});
        if (!m.mesh.empty()) values.push_back({"Mesh", Text(m.mesh)});
        if (!m.physics.empty()) values.push_back({"Physics", Text(m.physics)});
        if (m.dinosaur >= 0) values.push_back({"Dinosaur", Whole(m.dinosaur)});
        if (m.curved_property) values.push_back({"Curved", Flag(true)});
        if (m.animated >= 0) values.push_back({"AnimSubMaterial", Whole(m.animated)});
        Object(m.name, m.model || plain ? ModelObject(m) : MeshObject(m), values);
    }

    std::string ValueTable() const {
        const std::string profile = std::string("CHandle") + '\0' + std::string(13, '\0');
        std::string out = std::string("values") + '\0' + profile + "symbols" + '\0' + U32(uint32_t(names_.size()));
        for (size_t i = 0; i < names_.size(); ++i) out += names_[i] + '\0' + Handle(uint32_t(0x900 + i));
        return out + profile + U32(record_count_) + records_;
    }

    struct Section {
        uint32_t name, handle;
        std::string bytes;
    };
    std::vector<Section> sections_;
    std::map<std::string, uint32_t> symbols_;
    std::vector<std::string> names_;
    std::string records_, objects_;
    uint32_t record_count_ = 0;
};

inline std::string MakeWorld(const WorldShape& shape) { return WorldBuilder(shape).Bytes(); }

// A world with one crate that names its terrain.
inline std::string World(const std::string& terrain) {
    WorldShape shape;
    shape.terrain = terrain;
    shape.meshes.push_back({"Crate-00", "Map\\" + terrain + "\\crate.bmp"});
    return MakeWorld(shape);
}

inline uint32_t TextureHash(std::string name, float diffuse = 1.0f) {
    for (char& c : name) c = c == '\\' ? '/' : char(c >= 'A' && c <= 'Z' ? c + 32 : c);
    const std::string end = F32(diffuse);
    return jpt_crc32(jpt_crc32(0, name.data(), name.size()), end.data(), end.size());
}

struct PidEntry {
    uint64_t hash = 0;
    bool bump = false;
    uint32_t colour = 0x123456, width = 128, height = 64, palette = 0xFFFFFFFF, offset = 0;
};

inline std::string TextureSet(const std::vector<PidEntry>& entries) {
    std::string out = U32(304) + U32(16) + U32(32) + U32(uint32_t(entries.size())) + U32(32) + U32(0) + U32(0) + U32(0);
    for (const PidEntry& e : entries)
        out += U32(56) + U32(e.offset) + U32(e.width) + U32(e.height) + U32(512) + U32(16) + U32(e.colour) + U32(0) + U32(e.bump) + U32(0) + U32(e.palette) + U32(0) + U32(uint32_t(e.hash)) + U32(uint32_t(e.hash >> 32));
    return out;
}

// The set of World(terrain).
inline std::string Set(const std::string& terrain) { return TextureSet({{TextureHash("Map\\" + terrain + "\\crate.bmp")}}); }

// The key of the curved bump map of a MeshShape's polygon, over a texture of 128 x 64.
inline uint32_t CurvedKey(const MeshShape& m, uint32_t polygon, uint32_t texture) {
    const auto hundredths = [](double v) { return uint32_t(int32_t(v * 100.0)); };
    std::string data = U32(texture) + U32(0) + U32(0) + U32(hundredths(128)) + U32(hundredths(64)) + U32((polygon + 1) * texture) + U32(3 * texture);
    for (uint32_t i = polygon * 3; i < polygon * 3 + 3; ++i) data += U32(hundredths(double(m.size * float(i)))) + U32(hundredths(double(m.size * float(i % 2)))) + U32(hundredths(0.5));
    return jpt_crc32(0, data.data(), data.size());
}

inline std::string HeaderSection(const std::string& world, uint32_t version) {
    const std::string path = "c:\\tres cd\\data\\" + world;
    return U32(version) + path + std::string(0x500 - path.size(), '\0') + U32(1) + std::string(12, 'h');
}

inline std::string Save(const std::string& world, const std::string& scene, uint32_t version = 22) {
    const auto text = [](std::string s, size_t n) { return s.resize(n, '\0'), s; };
    return Container({{"SaveGameName", text("Quick Save", 21)}, {"SaveSCNName", text(scene, 20)}, {"SaveBMP", "BM"}, {"Header", HeaderSection(world, version)}, {"Partitions", std::string(17, 'p')},
                      {"Hierarchy", std::string(16, 'h')}, {"00A1B2C3", std::string(64, 'o')}});
}

} // namespace fixtures
