#include "level_guide.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <tuple>
#include <initializer_list>
#include <utility>

namespace jpt {
namespace {

// The constants of tools/minimap_map.py and tools/minimap_bounds.py.
constexpr int kMaskRows = 256, kMaxCols = 640;
constexpr double kMargin = 0.05, kLandStamp = 26.0, kDryCore = 14.0, kPlane = 150.0, kPatchMax = 150.0, kLandPatch = 70.0, kGateMax = 25.0;
constexpr double kSubmerged = 0.5, kLandmarkMin = 8.0, kLinearMin = 2.0, kGateMin = 3.0, kLandmarkMax = 300.0, kMinWidth = 2.0, kWallAspect = 4.0;
constexpr double kWallMaxWidth = 6.0, kSolidFill = 0.45, kHollowMin = 15.0, kVerticalNz = 0.3, kSegmentMin = 1.5, kMergeAngle = 0.14, kMergeGap = 2.5;
constexpr double kMergeSide = 1.0, kVehicleMerge = 3.0;
constexpr int kLandMinCells = 150;
constexpr int kLand = 1, kWater = 2;
constexpr int kBuilding = 0, kWall = 1, kRoad = 2, kGate = 3;
constexpr int kSetHint = 28, kLoadLevel = 16, kEndGame = 31;
constexpr size_t kTargetsMax = 2;

using Point = std::pair<double, double>;
using Quad = std::vector<Point>;

struct Object {
    unsigned index = 0;
    std::string name, type;
    bool has_values = false;
    const void* values = nullptr;
    double pos[3] = {}, rot[3] = {}, scale = 1.0;
};

// Python's sum() of floats (3.12 and later): compensated, as the tools that made the campaign's maps add them.
double PySum(std::initializer_list<double> values) {
    double total = 0.0, c = 0.0;
    for (const double x : values) {
        const double t = total + x;
        c += std::fabs(total) >= std::fabs(x) ? (total - t) + x : (x - t) + total;
        total = t;
    }
    return c != 0.0 && std::isfinite(c) ? total + c : total;
}

double SumX(const std::vector<std::pair<double, double>>& q) { return PySum({q[0].first, q[1].first, q[2].first, q[3].first}); }
double SumY(const std::vector<std::pair<double, double>>& q) { return PySum({q[0].second, q[1].second, q[2].second, q[3].second}); }

double Rounded(double v, int places) {
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", places, v);
    return std::strtod(text, nullptr);
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Has(const std::string& text, const char* part) { return Lower(text).find(part) != std::string::npos; }
bool Starts(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

bool Search(const std::regex& re, const std::string& text) { return std::regex_search(text, re); }

const std::regex& HelperName() {
    static const std::regex r("invis|physics|bbox|box", std::regex::icase);
    return r;
}
bool IsHelperName(const std::string& name) {
    static const std::regex no("^F?No[A-Z]");
    return Search(HelperName(), name) || Search(no, name);
}
const std::regex kNatural("rock|boulder|cliff|canyon|island|ridge|mound|hill|mntn|mtn|swamp", std::regex::icase);
const std::regex kRoadTerrain("road|street|path", std::regex::icase);
const std::regex kBuiltTerrain("smmtcon|walkway|prkng|plza|porch|slab|pndwall|wallbase|compflr|hackflr|loshack|biolb|secgate|bnkr|flr", std::regex::icase);
const std::regex kGateName("gate", std::regex::icase);
const std::regex kLinearName("wall|fence|barrier|wwy|walkway|bridge|pier|dock|ramp", std::regex::icase);
const std::regex kVehicleBody("^P(HJeepCrsh(ed|Suvy)|HunterJeep(Crashed|Suvy)?|HjeepOpenBody(Hack)?|HjeepSLDseatBody|truckBody|MercedesUnimoBody)\\d*-\\d+$");
const std::regex kVehicleLarge("truckBody|MercedesUnimoBody");
const std::regex kVehicleMedium("CrshSuvy|JeepSuvy|JeepCrshed");

bool IsLand(const Object& o) {
    static const char* const kHelpers[] = {"CBackdrop", "CSky", "CEntityWater", "CParticles", "CWaterDisturbance", "CMagnet", "CMuzzleFlash", "CAnimal", "CGun",
                                           "CLightDirectional", "CStartTrigger", "CLocationTrigger", "CObjectTrigger", "CCreatureTrigger", "CCollisionTrigger",
                                           "CSequenceTrigger", "CVariableTrigger", "CTimerTrigger", "CBooleanTrigger", "TerrainPlacement", "Player",
                                           "Player Settings", "AI Command"};
    static const char* const kNotLevel[] = {"BackDrop", "Ocean", "TrnObj_Ocean", "TOcean", "AIBox", "AIShore", "AIConcPier", "Trig_", "Trig", "Teleport", "Occlusion"};
    if (o.name.empty()) return false;
    for (const char* prefix : kNotLevel)
        if (Starts(o.name, prefix)) return false;
    for (const char* helper : kHelpers)
        if (o.type == helper) return false;
    return true;
}

class Reader {
public:
    explicit Reader(const jpt_groff_t* g) : g_(g) {}

    std::string Text(const void* group, const char* key) const {
        jpt_groff_value_t v;
        return jpt_groff_value_named(g_, group, key, &v) && v.type == JPT_GROFF_VALUE_TEXT ? v.text : "";
    }
    bool Number(const void* group, const char* key, int& out) const {
        jpt_groff_value_t v;
        if (!jpt_groff_value_named(g_, group, key, &v) || (v.type != JPT_GROFF_VALUE_INT && v.type != JPT_GROFF_VALUE_CHAR && v.type != JPT_GROFF_VALUE_BOOL)) return false;
        out = v.number;
        return true;
    }
    template <typename F> void Each(const void* group, F f) const {
        jpt_groff_value_t v;
        for (unsigned i = 0; jpt_groff_value_at(g_, group, i, &v); i++) f(v);
    }

private:
    const jpt_groff_t* g_;
};

bool IsWaterSurface(const Reader& r, const Object& o) {
    if (o.type == "CEntityWater") return true;
    if (o.type != "CInstance") return false;
    bool ocean = false;
    r.Each(o.values, [&](const jpt_groff_value_t& v) {
        if (v.key && Starts(v.key, "Anim") && v.type == JPT_GROFF_VALUE_TEXT && Has(v.text, "ocean")) ocean = true;
    });
    return ocean;
}

bool IsWaterTerrain(const Reader& r, const Object& o) {
    return o.type == "CTerrainObj" && !Has(o.name, "sound") && Has(r.Text(o.values, "SoundMaterial"), "water");
}

bool IsSeaTile(const Object& o) { return (o.type == "CTerrainObj" || o.type == "CInstance") && Has(o.name, "ocean") && !Has(o.name, "sound"); }
bool IsLandObject(const Reader& r, const Object& o) { return IsLand(o) && !IsSeaTile(o) && !IsWaterSurface(r, o) && !IsWaterTerrain(r, o); }
bool IsTerrainPatch(const Reader& r, const Object& o) { return o.type == "CTerrainObj" && !IsSeaTile(o) && !IsWaterTerrain(r, o) && IsLand(o); }

int LandmarkKind(const Object& o) {
    const std::string& name = o.name;
    if (name.empty() || IsHelperName(name)) return -1;
    if (o.type == "CTerrainObj") {
        if (Has(name, "sound")) return -1;
        if (Search(kRoadTerrain, name)) return kRoad;
        return Search(kBuiltTerrain, name) ? kBuilding : -1;
    }
    if (o.type.empty()) {
        if (name[0] != 'S' && name[0] != 'F') return -1;
    } else if (o.type != "CInstance" || std::strchr("VTABROo$W", name[0]) || Starts(name, "BackDrop")) {
        return -1;
    }
    if (Search(kNatural, name)) return -1;
    return Search(kGateName, name) && !Search(kLinearName, name) ? kGate : kBuilding;
}

struct Matrix {
    double m[3][3];
};

Matrix Rotation(double rx, double ry, double rz) {
    const auto mul = [](const Matrix& a, const Matrix& b) {
        Matrix out{};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        return out;
    };
    const Matrix x{{{1, 0, 0}, {0, std::cos(rx), std::sin(rx)}, {0, -std::sin(rx), std::cos(rx)}}};
    const Matrix y{{{std::cos(ry), 0, -std::sin(ry)}, {0, 1, 0}, {std::sin(ry), 0, std::cos(ry)}}};
    const Matrix z{{{std::cos(rz), std::sin(rz), 0}, {-std::sin(rz), std::cos(rz), 0}, {0, 0, 1}}};
    return mul(mul(x, y), z);
}

struct Vec3 {
    double x, y, z;
};

std::vector<Vec3> WorldPoints(const Object& o, const float* points, unsigned count) {
    const Matrix m = Rotation(o.rot[0], o.rot[1], o.rot[2]);
    std::vector<Vec3> out;
    out.reserve(count);
    for (unsigned i = 0; i < count; i++) {
        const double v[3] = {double(points[3 * i]) * o.scale, double(points[3 * i + 1]) * o.scale, double(points[3 * i + 2]) * o.scale};
        out.push_back({o.pos[0] + m.m[0][0] * v[0] + m.m[0][1] * v[1] + m.m[0][2] * v[2], o.pos[1] + m.m[1][0] * v[0] + m.m[1][1] * v[1] + m.m[1][2] * v[2],
                       o.pos[2] + m.m[2][0] * v[0] + m.m[2][1] * v[1] + m.m[2][2] * v[2]});
    }
    return out;
}

// An object's geometry as the tools read it: a built mesh's points and triangles, else the stored points.
struct Shape {
    std::vector<Vec3> world;
    std::vector<std::array<uint32_t, 3>> triangles;
    bool mesh = false;
};

Shape ShapeOf(const jpt_groff_t* g, const Object& o, bool mesh_first) {
    Shape s;
    float* points = nullptr;
    uint32_t* triangles = nullptr;
    unsigned count = 0, faces = 0;
    if (mesh_first && jpt_groff_object_mesh(g, o.index, &points, &count, &triangles, &faces)) {
        s.mesh = true;
        for (unsigned i = 0; i < faces; i++) s.triangles.push_back({triangles[3 * i], triangles[3 * i + 1], triangles[3 * i + 2]});
    } else if (!jpt_groff_object_points(g, o.index, &points, &count)) {
        return s;
    }
    s.world = WorldPoints(o, points, count);
    std::free(points);
    std::free(triangles);
    return s;
}

double Cross(const Point& o, const Point& a, const Point& b) { return (a.first - o.first) * (b.second - o.second) - (a.second - o.second) * (b.first - o.first); }

std::vector<Point> ConvexHull(std::vector<Point> pts) {
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() <= 2) return pts;
    std::vector<Point> lower, upper;
    for (const Point& p : pts) {
        while (lower.size() >= 2 && Cross(lower[lower.size() - 2], lower.back(), p) <= 0) lower.pop_back();
        lower.push_back(p);
    }
    for (auto it = pts.rbegin(); it != pts.rend(); ++it) {
        while (upper.size() >= 2 && Cross(upper[upper.size() - 2], upper.back(), *it) <= 0) upper.pop_back();
        upper.push_back(*it);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

// The footprint and its larger side; empty when it has no area.
std::vector<Point> HullOf(const std::vector<Vec3>& world, double* extent) {
    std::vector<Point> xy;
    xy.reserve(world.size());
    for (const Vec3& p : world) xy.push_back({p.x, p.y});
    std::vector<Point> hull = ConvexHull(xy);
    *extent = 0.0;
    if (hull.size() < 3) return {};
    double lx = hull[0].first, hx = lx, ly = hull[0].second, hy = ly;
    for (const Point& p : hull) lx = std::min(lx, p.first), hx = std::max(hx, p.first), ly = std::min(ly, p.second), hy = std::max(hy, p.second);
    *extent = std::max(hx - lx, hy - ly);
    return hull;
}

double PolygonArea(const std::vector<Point>& poly) {
    double sum = 0.0;
    for (size_t i = 0; i < poly.size(); i++) {
        const Point &a = poly[i], &b = poly[(i + 1) % poly.size()];
        sum += a.first * b.second - b.first * a.second;
    }
    return std::fabs(sum) / 2;
}

Quad OrientedRect(const std::vector<Point>& hull) {
    bool found = false;
    double best_area = 0, bx0 = 0, by0 = 0, bux = 0, buy = 0, bu0 = 0, bu1 = 0, bv0 = 0, bv1 = 0;
    for (size_t i = 0; i < hull.size(); i++) {
        const double x0 = hull[i].first, y0 = hull[i].second, x1 = hull[(i + 1) % hull.size()].first, y1 = hull[(i + 1) % hull.size()].second;
        const double length = std::hypot(x1 - x0, y1 - y0);
        if (length == 0.0) continue;
        const double ux = (x1 - x0) / length, uy = (y1 - y0) / length;
        double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
        for (size_t k = 0; k < hull.size(); k++) {
            const double u = (hull[k].first - x0) * ux + (hull[k].second - y0) * uy, v = -(hull[k].first - x0) * uy + (hull[k].second - y0) * ux;
            if (!k) u0 = u1 = u, v0 = v1 = v;
            u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
        }
        const double area = (u1 - u0) * (v1 - v0);
        if (!found || area < best_area - 1e-9) found = true, best_area = area, bx0 = x0, by0 = y0, bux = ux, buy = uy, bu0 = u0, bu1 = u1, bv0 = v0, bv1 = v1;
    }
    Quad out;
    for (const auto& [u, v] : {std::pair{bu0, bv0}, {bu1, bv0}, {bu1, bv1}, {bu0, bv1}}) out.push_back({bx0 + bux * u - buy * v, by0 + buy * u + bux * v});
    return out;
}

double Dist(const Point& a, const Point& b) { return std::hypot(a.first - b.first, a.second - b.second); }
std::pair<double, double> RectSides(const Quad& q) { return {Dist(q[0], q[1]), Dist(q[0], q[3])}; }

Quad WidenRect(const Quad& q) {
    const double cx = SumX(q) / 4, cy = SumY(q) / 4;
    double ux = q[1].first - q[0].first, uy = q[1].second - q[0].second, vx = q[3].first - q[0].first, vy = q[3].second - q[0].second;
    const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
    if (lu >= kMinWidth && lv >= kMinWidth) return q;
    const double wu = std::max(lu, kMinWidth) / 2, wv = std::max(lv, kMinWidth) / 2;
    ux = ux / std::max(lu, 1e-9) * wu, uy = uy / std::max(lu, 1e-9) * wu;
    vx = vx / std::max(lv, 1e-9) * wv, vy = vy / std::max(lv, 1e-9) * wv;
    return {{cx - ux - vx, cy - uy - vy}, {cx + ux - vx, cy + uy - vy}, {cx + ux + vx, cy + uy + vy}, {cx - ux + vx, cy - uy + vy}};
}

bool TriangleContains(double px, double py, const Point& a, const Point& b, const Point& c) {
    const double d = (b.second - c.second) * (a.first - c.first) + (c.first - b.first) * (a.second - c.second);
    if (std::fabs(d) < 1e-12) return false;
    const double l1 = ((b.second - c.second) * (px - c.first) + (c.first - b.first) * (py - c.second)) / d;
    const double l2 = ((c.second - a.second) * (px - c.first) + (a.first - c.first) * (py - c.second)) / d;
    return l1 >= -1e-9 && l2 >= -1e-9 && 1 - l1 - l2 >= -1e-9;
}

using Tri2 = std::array<Point, 3>;

double CoveredFraction(const std::vector<Tri2>& tris, const std::vector<Point>& hull) {
    const double area = PolygonArea(hull);
    if (area <= 0) return 0.0;
    double x0 = hull[0].first, y0 = hull[0].second;
    for (const Point& p : hull) x0 = std::min(x0, p.first), y0 = std::min(y0, p.second);
    const double step = std::max(0.8, std::sqrt(area / 5000));
    std::set<std::pair<long long, long long>> covered;
    for (const Tri2& t : tris) {
        const double lx = std::min({t[0].first, t[1].first, t[2].first}), hx = std::max({t[0].first, t[1].first, t[2].first});
        const double ly = std::min({t[0].second, t[1].second, t[2].second}), hy = std::max({t[0].second, t[1].second, t[2].second});
        for (long long i = (long long)((lx - x0) / step); i < (long long)((hx - x0) / step) + 2; i++)
            for (long long j = (long long)((ly - y0) / step); j < (long long)((hy - y0) / step) + 2; j++)
                if (!covered.count({i, j}) && TriangleContains(x0 + (double(i) + 0.5) * step, y0 + (double(j) + 0.5) * step, t[0], t[1], t[2])) covered.insert({i, j});
    }
    return double(covered.size()) * step * step / area;
}

std::vector<std::pair<Point, Point>> WallLines(const Shape& s) {
    std::set<std::pair<Point, Point>> lines;
    for (const auto& t : s.triangles) {
        const Vec3 &pa = s.world[t[0]], &pb = s.world[t[1]], &pc = s.world[t[2]];
        const double ux = pb.x - pa.x, uy = pb.y - pa.y, uz = pb.z - pa.z, vx = pc.x - pa.x, vy = pc.y - pa.y, vz = pc.z - pa.z;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length < 1e-9 || std::fabs(nz) / length > kVerticalNz) continue;
        const std::pair<const Vec3*, const Vec3*> edges[3] = {{&pa, &pb}, {&pb, &pc}, {&pa, &pc}};
        size_t best = 0;
        for (size_t e = 1; e < 3; e++)
            if (std::hypot(edges[e].first->x - edges[e].second->x, edges[e].first->y - edges[e].second->y) >
                std::hypot(edges[best].first->x - edges[best].second->x, edges[best].first->y - edges[best].second->y))
                best = e;
        const Vec3 &p = *edges[best].first, &q = *edges[best].second;
        if (std::hypot(p.x - q.x, p.y - q.y) < kSegmentMin) continue;
        Point a{Rounded(p.x, 1), Rounded(p.y, 1)}, b{Rounded(q.x, 1), Rounded(q.y, 1)};
        if (b < a) std::swap(a, b);
        lines.insert({a, b});
    }
    return {lines.begin(), lines.end()};
}

Quad LineQuad(Point p, Point q) {
    const double length = std::hypot(q.first - p.first, q.second - p.second), ux = (q.first - p.first) / length, uy = (q.second - p.second) / length;
    const double pad = std::max(0.0, (kMinWidth - length) / 2), h = kMinWidth / 2;
    p = {p.first - ux * pad, p.second - uy * pad};
    q = {q.first + ux * pad, q.second + uy * pad};
    return {{p.first - uy * h, p.second + ux * h}, {q.first - uy * h, q.second + ux * h}, {q.first + uy * h, q.second - ux * h}, {p.first + uy * h, p.second - ux * h}};
}

struct Frame {
    double cx, cy, ux, uy, hl, hw;
};

Frame PieceFrame(const Quad& q) {
    const auto [a, b] = RectSides(q);
    const Point e = a >= b ? Point{q[1].first - q[0].first, q[1].second - q[0].second} : Point{q[3].first - q[0].first, q[3].second - q[0].second};
    double length = std::hypot(e.first, e.second);
    if (length == 0.0) length = 1.0;
    return {SumX(q) / 4, SumY(q) / 4, e.first / length, e.second / length,
            std::max(a, b) / 2, std::min(a, b) / 2};
}

struct WallGroup {
    double cx, cy, ux, uy, u0, u1, v0, v1;

    explicit WallGroup(const Quad& q) {
        const Frame f = PieceFrame(q);
        cx = f.cx, cy = f.cy, ux = f.ux, uy = f.uy, u0 = -f.hl, u1 = f.hl, v0 = -f.hw, v1 = f.hw;
    }
    bool Accepts(const Quad& q, double extent[4]) const {
        const Frame f = PieceFrame(q);
        const double sine = std::fabs(ux * f.uy - uy * f.ux), cosine = std::sqrt(std::max(0.0, 1 - sine * sine));
        const double du = (f.cx - cx) * ux + (f.cy - cy) * uy, dv = -(f.cx - cx) * uy + (f.cy - cy) * ux;
        const double along = f.hl * cosine + f.hw * sine, across = f.hw * cosine + f.hl * sine;
        extent[0] = du - along, extent[1] = du + along, extent[2] = dv - across, extent[3] = dv + across;
        return !(sine > kMergeAngle || std::max(u0 - extent[1], extent[0] - u1) > kMergeGap || std::max(v0 - extent[3], extent[2] - v1) > kMergeSide);
    }
    void Take(const double e[4]) { u0 = std::min(u0, e[0]), u1 = std::max(u1, e[1]), v0 = std::min(v0, e[2]), v1 = std::max(v1, e[3]); }
    Quad ToQuad() const {
        const auto at = [this](double u, double v) { return Point{cx + ux * u - uy * v, cy + uy * u + ux * v}; };
        return {at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)};
    }
};

std::vector<Quad> MergeWalls(std::vector<Quad> quads) {
    std::vector<WallGroup> groups;
    for (int round = 0; round < 6; round++) {
        std::vector<Quad> ordered = quads;
        const auto key = [](const Quad& q) {
            const auto [a, b] = RectSides(q);
            return std::make_tuple(-Rounded(std::max(a, b), 3), Rounded(SumX(q), 3), Rounded(SumY(q), 3));
        };
        std::stable_sort(ordered.begin(), ordered.end(), [&key](const Quad& a, const Quad& b) { return key(a) < key(b); });
        groups.clear();
        for (const Quad& q : ordered) {
            double extent[4];
            bool joined = false;
            for (WallGroup& g : groups)
                if (g.Accepts(q, extent)) {
                    g.Take(extent);
                    joined = true;
                    break;
                }
            if (!joined) groups.emplace_back(q);
        }
        if (groups.size() == quads.size()) break;
        quads.clear();
        for (const WallGroup& g : groups) quads.push_back(g.ToQuad());
    }
    std::vector<Quad> out;
    for (const WallGroup& g : groups) out.push_back(g.ToQuad());
    return out;
}

class Grid {
public:
    Grid(int across, int down, double min_x, double min_y, double max_x, double max_y)
        : cols(across), rows(down), land(size_t(across) * down), dry(size_t(across) * down), plane(size_t(across) * down), plane_z(size_t(across) * down),
          min_x_(min_x), max_y_(max_y), cw_((max_x - min_x) / across), ch_((max_y - min_y) / down) {}

    int cols, rows;

    std::pair<int, int> CellOf(double x, double y) const { return {int((x - min_x_) / cw_), int((max_y_ - y) / ch_)}; }
    bool Inside(int c, int r) const { return c >= 0 && c < cols && r >= 0 && r < rows; }
    Point Centre(int c, int r) const { return {min_x_ + (c + 0.5) * cw_, max_y_ - (r + 0.5) * ch_}; }

    void StampDisk(std::vector<uint8_t>& mask, double x, double y, double radius) const {
        const auto [cx, cy] = CellOf(x, y);
        const int rc = int(std::ceil(radius / cw_)) + 1, rr = int(std::ceil(radius / ch_)) + 1;
        for (int r = std::max(0, cy - rr); r <= std::min(rows - 1, cy + rr); r++)
            for (int c = std::max(0, cx - rc); c <= std::min(cols - 1, cx + rc); c++) {
                const Point p = Centre(c, r);
                if ((p.first - x) * (p.first - x) + (p.second - y) * (p.second - y) <= radius * radius) mask[size_t(r) * cols + c] = 1;
            }
    }

    std::vector<std::pair<int, int>> CoverTriangles(std::vector<uint8_t>& mask, const std::vector<Tri2>& tris) const {
        std::map<std::pair<int, int>, int> counts;
        std::vector<std::pair<int, int>> order;
        const auto add = [&](std::pair<int, int> cell, int n, bool set) {
            auto it = counts.find(cell);
            if (it == counts.end()) {
                counts[cell] = n;
                order.push_back(cell);
            } else {
                it->second = set ? n : it->second + n;
            }
        };
        for (const Tri2& t : tris) {
            const auto [c0, r1] = CellOf(std::min({t[0].first, t[1].first, t[2].first}), std::min({t[0].second, t[1].second, t[2].second}));
            const auto [c1, r0] = CellOf(std::max({t[0].first, t[1].first, t[2].first}), std::max({t[0].second, t[1].second, t[2].second}));
            bool hit = false;
            for (int r = std::max(0, r0); r <= std::min(rows - 1, r1); r++)
                for (int col = std::max(0, c0); col <= std::min(cols - 1, c1); col++) {
                    const Point p = Centre(col, r);
                    int n = 0;
                    for (int dx = -1; dx <= 1; dx++)
                        for (int dy = -1; dy <= 1; dy++) n += TriangleContains(p.first + dx * cw_ / 3, p.second + dy * ch_ / 3, t[0], t[1], t[2]);
                    if (n) {
                        add({col, r}, n, false);
                        hit = true;
                    }
                }
            if (!hit) {
                const auto [col, r] = CellOf((t[0].first + t[1].first + t[2].first) / 3, (t[0].second + t[1].second + t[2].second) / 3);
                if (Inside(col, r)) add({col, r}, 9, true);
            }
        }
        std::vector<std::pair<int, int>> cells;
        for (const auto& cell : order)
            if (counts[cell] >= 4) cells.push_back(cell);
        if (cells.empty() && !counts.empty()) {
            std::pair<int, int> best = counts.begin()->first;
            for (const auto& [cell, n] : counts)
                if (n > counts[best]) best = cell;
            cells.push_back(best);
        }
        for (const auto& [col, r] : cells) mask[size_t(r) * cols + col] = 1;
        return cells;
    }

    std::vector<std::pair<int, int>> CoverHull(std::vector<uint8_t>& mask, const std::vector<Point>& hull) const {
        double lx = hull[0].first, hx = lx, ly = hull[0].second, hy = ly;
        for (const Point& p : hull) lx = std::min(lx, p.first), hx = std::max(hx, p.first), ly = std::min(ly, p.second), hy = std::max(hy, p.second);
        auto [c0, r1] = CellOf(lx, ly);
        auto [c1, r0] = CellOf(hx, hy);
        c0 = std::max(0, c0), c1 = std::min(cols - 1, c1), r0 = std::max(0, r0), r1 = std::min(rows - 1, r1);
        std::vector<std::array<double, 3>> edges;
        for (size_t i = 0; i < hull.size(); i++) {
            const Point &a = hull[i], &b = hull[(i + 1) % hull.size()];
            const double nx = b.second - a.second, ny = a.first - b.first;
            edges.push_back({nx, ny, nx * a.first + ny * a.second});
        }
        const double hw = cw_ / 2, hh = ch_ / 2;
        std::vector<std::pair<int, int>> cells;
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                const Point p = Centre(c, r);
                bool all = true;
                for (const auto& e : edges)
                    if (!(e[0] * p.first + e[1] * p.second - (std::fabs(e[0]) * hw + std::fabs(e[1]) * hh) <= e[2] + 1e-9)) {
                        all = false;
                        break;
                    }
                if (all) {
                    mask[size_t(r) * cols + c] = 1;
                    cells.push_back({c, r});
                }
            }
        return cells;
    }

    std::vector<uint8_t> land, dry, plane;
    std::vector<double> plane_z;

private:
    double min_x_, max_y_, cw_, ch_;
};

void DropSmallBlobs(std::vector<uint8_t>& cells, int cols, int rows, size_t minimum) {
    std::vector<uint8_t> seen(cells.size());
    for (int y0 = 0; y0 < rows; y0++)
        for (int x0 = 0; x0 < cols; x0++) {
            if (!cells[size_t(y0) * cols + x0] || seen[size_t(y0) * cols + x0]) continue;
            std::deque<std::pair<int, int>> queue{{x0, y0}};
            std::vector<std::pair<int, int>> blob;
            seen[size_t(y0) * cols + x0] = 1;
            while (!queue.empty()) {
                const auto [x, y] = queue.front();
                queue.pop_front();
                blob.push_back({x, y});
                const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : dirs) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (nx >= 0 && nx < cols && ny >= 0 && ny < rows && !seen[size_t(ny) * cols + nx] && cells[size_t(ny) * cols + nx]) {
                        seen[size_t(ny) * cols + nx] = 1;
                        queue.push_back({nx, ny});
                    }
                }
            }
            if (blob.size() < minimum)
                for (const auto& [x, y] : blob) cells[size_t(y) * cols + x] = 0;
        }
}

void AddRuns(const std::vector<uint8_t>& cells, int cols, int rows, int kind, std::vector<GuideRun>& runs) {
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols;) {
            if (!cells[size_t(y) * cols + x]) {
                x++;
                continue;
            }
            const int start = x;
            while (x < cols && cells[size_t(y) * cols + x]) x++;
            runs.push_back({start, x - start, y, kind});
        }
}

std::vector<Tri2> Flat(const Shape& s) {
    std::vector<Tri2> out;
    for (const auto& t : s.triangles) out.push_back({Point{s.world[t[0]].x, s.world[t[0]].y}, Point{s.world[t[1]].x, s.world[t[1]].y}, Point{s.world[t[2]].x, s.world[t[2]].y}});
    return out;
}

std::vector<std::pair<int, Quad>> LandmarkPieces(const jpt_groff_t* g, const Object& o, int kind) {
    const Shape s = ShapeOf(g, o, true);
    if (s.world.empty()) return {};
    double extent;
    const std::vector<Point> hull = HullOf(s.world, &extent);
    if (hull.empty()) return {};
    const Quad quad = OrientedRect(hull);
    const auto [a, b] = RectSides(quad);
    const double length = std::max(a, b), width = std::max(std::min(a, b), 0.01);
    const bool linear = Search(kLinearName, o.name);
    const double floor = kind == kGate ? kGateMin : linear ? kLinearMin : kLandmarkMin;
    if (length < floor || length > kLandmarkMax) return {};
    if (kind == kGate && length > kGateMax) kind = kBuilding;
    if (s.mesh && kind == kBuilding && length >= kHollowMin && CoveredFraction(Flat(s), hull) < kSolidFill) {
        const auto lines = WallLines(s);
        if (!lines.empty()) {
            std::vector<std::pair<int, Quad>> out;
            for (const auto& [p, q] : lines) out.push_back({kWall, LineQuad(p, q)});
            return out;
        }
    }
    if (kind == kBuilding && width <= kWallMaxWidth && (length / width >= kWallAspect || linear)) kind = kWall;
    return {{kind, WidenRect(quad)}};
}

GuideVehicle VehicleMarker(const jpt_groff_t* g, const Object& o) {
    const Shape s = ShapeOf(g, o, true);
    double extent;
    const std::vector<Point> hull = s.world.empty() ? std::vector<Point>{} : HullOf(s.world, &extent);
    GuideVehicle v;
    if (!hull.empty()) {
        const Frame f = PieceFrame(OrientedRect(hull));
        v.x = f.cx, v.y = f.cy, v.ax = f.ux, v.ay = f.uy;
    } else {
        v.x = o.pos[0], v.y = o.pos[1], v.ax = std::cos(o.rot[2]), v.ay = std::sin(o.rot[2]);
    }
    v.size = Search(kVehicleLarge, o.name) ? 2 : Search(kVehicleMedium, o.name) ? 1 : 0;
    return v;
}

void BuildMap(const jpt_groff_t* g, const Reader& r, const std::vector<Object>& objects, LevelGuide& guide) {
    bool any = false;
    double lx = 0, hx = 0, ly = 0, hy = 0;
    for (const Object& o : objects) {
        if (!IsLand(o)) continue;
        if (!any) lx = hx = o.pos[0], ly = hy = o.pos[1];
        any = true;
        lx = std::min(lx, o.pos[0]), hx = std::max(hx, o.pos[0]), ly = std::min(ly, o.pos[1]), hy = std::max(hy, o.pos[1]);
    }
    if (!any) {
        guide.no_map = "the world has no playable object";
        return;
    }
    const double margin = kMargin * std::max(hx - lx, hy - ly);
    guide.min_x = std::floor(lx - margin), guide.min_y = std::floor(ly - margin), guide.max_x = std::ceil(hx + margin), guide.max_y = std::ceil(hy + margin);
    const double span_x = guide.max_x - guide.min_x, span_y = guide.max_y - guide.min_y;
    if (!(span_x > 0) || !(span_y > 0)) {
        guide.no_map = "the playable objects stand in one line";
        return;
    }
    const int rows = kMaskRows, cols = std::max(1, std::min(kMaxCols, int(std::nearbyint(rows * span_x / span_y))));
    Grid grid(cols, rows, guide.min_x, guide.min_y, guide.max_x, guide.max_y);
    std::vector<uint8_t> auth(size_t(cols) * rows);

    for (const Object& o : objects) {
        const bool surface = IsWaterSurface(r, o), terrain = !surface && IsWaterTerrain(r, o);
        if (!surface && !terrain) continue;
        const Shape s = ShapeOf(g, o, true);
        if (s.world.empty()) continue;
        double extent;
        const std::vector<Point> hull = HullOf(s.world, &extent);
        if (hull.empty() || (terrain && extent > kPatchMax)) continue;
        const bool is_plane = surface && extent > kPlane;
        if (is_plane) {
            const auto cells = s.mesh ? grid.CoverTriangles(grid.plane, Flat(s)) : grid.CoverHull(grid.plane, hull);
            for (const auto& [c, row] : cells) grid.plane_z[size_t(row) * cols + c] = o.pos[2];
        } else if (s.mesh) {
            grid.CoverTriangles(auth, Flat(s));
        } else {
            grid.CoverHull(auth, hull);
        }
    }

    for (const Object& o : objects) {
        if (!IsLandObject(r, o)) continue;
        guide.playable++;
        const auto [c, row] = grid.CellOf(o.pos[0], o.pos[1]);
        const bool under = grid.Inside(c, row) && grid.plane[size_t(row) * cols + c];
        if (under && o.pos[2] < grid.plane_z[size_t(row) * cols + c] - kSubmerged) continue;
        grid.StampDisk(grid.land, o.pos[0], o.pos[1], kLandStamp);
        if (under) grid.StampDisk(grid.dry, o.pos[0], o.pos[1], kDryCore);
    }
    for (const Object& o : objects) {
        if (!IsTerrainPatch(r, o)) continue;
        const Shape s = ShapeOf(g, o, false);
        if (s.world.empty()) continue;
        double extent;
        const std::vector<Point> hull = HullOf(s.world, &extent);
        if (!hull.empty() && extent <= kLandPatch) {
            grid.CoverHull(grid.land, hull);
            grid.CoverHull(grid.dry, hull);
        }
    }
    DropSmallBlobs(grid.land, cols, rows, kLandMinCells);
    std::vector<uint8_t> water(size_t(cols) * rows);
    for (size_t i = 0; i < water.size(); i++) water[i] = auth[i] || (grid.plane[i] && !grid.dry[i]);

    std::vector<std::pair<int, Quad>> pieces;
    for (const Object& o : objects) {
        const int kind = LandmarkKind(o);
        if (kind < 0) continue;
        for (auto& piece : LandmarkPieces(g, o, kind)) pieces.push_back(std::move(piece));
    }
    std::vector<Quad> walls;
    std::vector<std::pair<int, Quad>> all;
    for (const auto& [kind, q] : pieces)
        if (kind == kWall) walls.push_back(q);
    for (const Quad& q : MergeWalls(walls)) {
        const auto [a, b] = RectSides(q);
        all.push_back({std::min(a, b) <= kWallMaxWidth ? kWall : kBuilding, q});
    }
    for (const auto& piece : pieces)
        if (piece.first != kWall) all.push_back(piece);
    using Key = std::tuple<int, double, double, std::vector<Point>>;
    std::set<Key> unique;
    for (const auto& [kind, q] : all) {
        std::vector<Point> rounded;
        for (const Point& p : q) rounded.push_back({Rounded(p.first, 1), Rounded(p.second, 1)});
        unique.insert({kind, SumY(rounded), SumX(rounded), rounded});
    }
    for (const auto& [kind, sy, sx, pts] : unique) {
        GuideLandmark l;
        l.kind = kind;
        for (int k = 0; k < 4; k++) l.x[k] = pts[k].first, l.y[k] = pts[k].second;
        guide.landmarks.push_back(l);
    }

    std::vector<GuideVehicle> bodies;
    for (const Object& o : objects)
        if (std::regex_match(o.name, kVehicleBody)) bodies.push_back(VehicleMarker(g, o));
    std::sort(bodies.begin(), bodies.end(), [](const GuideVehicle& a, const GuideVehicle& b) {
        return std::tie(a.x, a.y, a.ax, a.ay, a.size) < std::tie(b.x, b.y, b.ax, b.ay, b.size);
    });
    for (GuideVehicle& v : bodies) v.x = Rounded(v.x, 1), v.y = Rounded(v.y, 1), v.ax = Rounded(v.ax, 3), v.ay = Rounded(v.ay, 3);
    for (const GuideVehicle& v : bodies)
        if (std::all_of(guide.vehicles.begin(), guide.vehicles.end(), [&v](const GuideVehicle& k) { return std::hypot(v.x - k.x, v.y - k.y) >= kVehicleMerge; }))
            guide.vehicles.push_back(v);

    AddRuns(grid.land, cols, rows, kLand, guide.runs);
    AddRuns(water, cols, rows, kWater, guide.runs);
    std::stable_sort(guide.runs.begin(), guide.runs.end(), [](const GuideRun& a, const GuideRun& b) { return std::tie(a.row, a.kind, a.start) < std::tie(b.row, b.kind, b.start); });
    guide.cols = cols, guide.rows = rows;
    guide.map = !guide.runs.empty();
    if (!guide.map) guide.no_map = "no land or water could be told from the world";
}

// The triggers' actions: Action01, Action02, ... when it has them, else the trigger's own ActionType.
template <typename F> void EachAction(const Reader& r, const Object& o, F f) {
    std::vector<std::pair<std::string, const void*>> numbered;
    r.Each(o.values, [&](const jpt_groff_value_t& v) {
        if (v.type == JPT_GROFF_VALUE_GROUP && v.key && std::strlen(v.key) == 8 && Starts(v.key, "Action") && std::isdigit((unsigned char)v.key[6]) &&
            std::isdigit((unsigned char)v.key[7]))
            numbered.push_back({v.key, v.group});
    });
    std::stable_sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    int type;
    if (!numbered.empty()) {
        for (const auto& [key, group] : numbered)
            if (r.Number(group, "ActionType", type)) f(type, group);
    } else if (r.Number(o.values, "ActionType", type)) {
        f(type, o.values);
    }
}

// Each hint leads to the triggers that set the next one, the last to the level's exits: the campaign's objectives follow its
// hints the same way (src/host/objective_policy.c).
void BuildObjectives(const Reader& r, const std::vector<Object>& objects, LevelGuide& guide) {
    std::map<int, std::set<std::string>> setters;
    std::set<std::string> exits;
    for (const Object& o : objects) {
        if (o.type.size() < 7 || o.type.compare(o.type.size() - 7, 7, "Trigger") || o.name.empty()) continue;
        EachAction(r, o, [&](int type, const void* group) {
            int hint;
            if (type == kSetHint && r.Number(group, "HintID", hint) && hint >= 0) setters[hint].insert(o.name);
            if (type == kLoadLevel || type == kEndGame) exits.insert(o.name);
        });
    }
    const auto first = [](const std::set<std::string>& names) {
        std::vector<std::string> out;
        for (const std::string& n : names)
            if (out.size() < kTargetsMax) out.push_back(n);
        return out;
    };
    if (setters.empty()) {
        if (!exits.empty()) guide.objectives.push_back({-1, first(exits)});
        return;
    }
    for (auto it = setters.begin(); it != setters.end(); ++it) {
        const auto next = std::next(it);
        guide.objectives.push_back({it->first, next != setters.end() ? first(next->second) : first(exits)});
    }
}

} // namespace

LevelGuide BuildLevelGuide(const jpt_groff_t* g) {
    LevelGuide guide;
    const Reader r(g);
    std::vector<Object> objects;
    const unsigned count = jpt_groff_object_count(g);
    for (unsigned i = 0; i < count; i++) {
        jpt_groff_object_t raw;
        jpt_groff_object(g, i, &raw);
        Object o;
        o.index = i;
        o.name = raw.name;
        o.type = raw.type;
        o.has_values = raw.has_values != 0;
        o.values = raw.values;
        for (int k = 0; k < 3; k++) o.pos[k] = Rounded(raw.position[k], 4), o.rot[k] = Rounded(raw.rotation[k], 6);
        o.scale = Rounded(raw.scale, 6);
        objects.push_back(std::move(o));
    }
    guide.objects = count;
    BuildMap(g, r, objects, guide);
    BuildObjectives(r, objects, guide);
    return guide;
}

std::string LevelGuideText(const LevelGuide& guide, const std::string& world, bool in_game) {
    std::string out = "level-guide " + std::to_string(kLevelGuideVersion) + "\n";
    char line[256];
    out += std::string("world ") + (in_game ? "game " : "package ") + world + "\n";
    if (guide.map) {
        std::snprintf(line, sizeof line, "map %.0f %.0f %.0f %.0f %d %d\n", guide.min_x, guide.min_y, guide.max_x, guide.max_y, guide.cols, guide.rows);
        out += line;
        for (const GuideRun& r : guide.runs) {
            std::snprintf(line, sizeof line, "run %d %d %d %d\n", r.start, r.length, r.row, r.kind);
            out += line;
        }
        for (const GuideLandmark& l : guide.landmarks) {
            std::snprintf(line, sizeof line, "landmark %d %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n", l.kind, l.x[0], l.y[0], l.x[1], l.y[1], l.x[2], l.y[2], l.x[3], l.y[3]);
            out += line;
        }
        for (const GuideVehicle& v : guide.vehicles) {
            std::snprintf(line, sizeof line, "vehicle %.1f %.1f %.3f %.3f %d\n", v.x, v.y, v.ax, v.ay, v.size);
            out += line;
        }
    } else {
        out += "nomap " + guide.no_map + "\n";
    }
    for (const GuideObjective& o : guide.objectives) {
        out += "objective " + std::to_string(o.hint);
        for (const std::string& t : o.targets) out += " " + t;
        out += "\n";
    }
    return out;
}

std::string LevelGuideName(const std::string& scene_file) {
    const size_t slash = scene_file.find_last_of("/\\");
    return Lower(slash == std::string::npos ? scene_file : scene_file.substr(slash + 1)) + ".txt";
}

std::string LevelGuideSummary(const LevelGuide& guide) {
    std::string out = guide.map ? "minimap " + std::to_string(guide.cols) + "x" + std::to_string(guide.rows) + ", " + std::to_string(guide.landmarks.size()) + " landmark(s), " +
                                      std::to_string(guide.vehicles.size()) + " vehicle(s)"
                                : "no minimap (" + guide.no_map + ")";
    if (guide.objectives.empty()) return out + ", no objectives (no hint and no exit trigger)";
    return out + ", " + std::to_string(guide.objectives.size()) + " objective(s) from " + (guide.objectives[0].hint < 0 ? "its exit" : "its hints");
}

} // namespace jpt
