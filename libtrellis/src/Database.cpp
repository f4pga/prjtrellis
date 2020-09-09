#include "Database.hpp"
#include "Chip.hpp"
#include "Tile.hpp"
#include "Util.hpp"
#include "BitDatabase.hpp"
#include <iostream>
#include <boost/optional.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <stdexcept>
#include <mutex>


namespace pt = boost::property_tree;

namespace Trellis {
static string db_root = "";
static pt::ptree devices_info;

// Cache Tilegrid data, to save time parsing it again
static map<string, pt::ptree> tilegrid_cache;
#ifndef NO_THREADS
static mutex tilegrid_cache_mutex;
#endif

void load_database(string root) {
    db_root = root;
    pt::read_json(root + "/" + "devices.json", devices_info);
}

// Iterate through all family and device permutations
// T should return true in case of a match
template<typename T>
boost::optional<DeviceLocator> find_device_generic(T f) {
    for (const pt::ptree::value_type &family : devices_info.get_child("families")) {
        for (const pt::ptree::value_type &dev : family.second.get_child("devices")) {
            bool res = f(dev.first, dev.second);
            if (res)
                return boost::make_optional(DeviceLocator{family.first, dev.first});
        }
    }
    return boost::optional<DeviceLocator>();
}

DeviceLocator find_device_by_name(string name) {
    auto found = find_device_generic([name](const string &n, const pt::ptree &p) -> bool {
        UNUSED(p);
        return n == name;
    });
    if (!found)
        throw runtime_error("no device in database with name " + name);
    return *found;
}

// Hex is not allowed in JSON, to avoid an ugly decimal integer use a string instead
// But we need to parse this back to a uint32_t
uint32_t parse_uint32(string str) {
    return uint32_t(strtoul(str.c_str(), nullptr, 0));
}

DeviceLocator find_device_by_idcode(uint32_t idcode) {
    auto found = find_device_generic([idcode](const string &n, const pt::ptree &p) -> bool {
        UNUSED(n);
        return parse_uint32(p.get<string>("idcode")) == idcode;
    });
    if (!found)
        throw runtime_error("no device in database with IDCODE " + uint32_to_hexstr(idcode));
    return *found;
}

ChipInfo get_chip_info(const DeviceLocator &part) {
    pt::ptree dev = devices_info.get_child("families").get_child(part.family).get_child("devices").get_child(
            part.device);
    ChipInfo ci;
    ci.family = part.family;
    ci.name = part.device;
    ci.num_frames = dev.get<int>("frames");
    ci.bits_per_frame = dev.get<int>("bits_per_frame");
    ci.pad_bits_after_frame = dev.get<int>("pad_bits_after_frame");
    ci.pad_bits_before_frame = dev.get<int>("pad_bits_before_frame");
    ci.idcode = parse_uint32(dev.get<string>("idcode"));
    ci.max_row = dev.get<int>("max_row");
    ci.max_col = dev.get<int>("max_col");
    ci.col_bias = dev.get<int>("col_bias");
    return ci;
}

Ecp5GlobalsInfo get_global_info_ecp5(const DeviceLocator &part) {
    string glbdata_path = db_root + "/" + part.family + "/" + part.device + "/globals.json";
    pt::ptree glb_parsed;
    pt::read_json(glbdata_path, glb_parsed);
    Ecp5GlobalsInfo glbs;
    for (const pt::ptree::value_type &quad : glb_parsed.get_child("quadrants")) {
        GlobalRegion rg;
        rg.name = quad.first;
        rg.x0 = quad.second.get<int>("x0");
        rg.x1 = quad.second.get<int>("x1");
        rg.y0 = quad.second.get<int>("y0");
        rg.y1 = quad.second.get<int>("y1");
        glbs.quadrants.push_back(rg);
    }
    for (const pt::ptree::value_type &tap : glb_parsed.get_child("taps")) {
        TapSegment ts;
        assert(tap.first[0] == 'C');
        ts.tap_col = stoi(tap.first.substr(1));
        ts.lx0 = tap.second.get<int>("lx0");
        ts.lx1 = tap.second.get<int>("lx1");
        ts.rx0 = tap.second.get<int>("rx0");
        ts.rx1 = tap.second.get<int>("rx1");
        glbs.tapsegs.push_back(ts);
    }
    for (const pt::ptree::value_type &spine : glb_parsed.get_child("spines")) {
        SpineSegment ss;
        ss.quadrant = spine.first.substr(0, 2);
        ss.tap_col = stoi(spine.first.substr(2));
        ss.spine_row = spine.second.get<int>("y");
        ss.spine_col = spine.second.get<int>("x");
        glbs.spinesegs.push_back(ss);
    }
    return glbs;
}

MachXO2GlobalsInfo get_global_info_machxo2(const DeviceLocator &part) {
    string glbdata_path = db_root + "/" + part.family + "/" + part.device + "/globals.json";
    pt::ptree glb_parsed;
    pt::read_json(glbdata_path, glb_parsed);
    MachXO2GlobalsInfo glbs;

    for (const pt::ptree::value_type &lr : glb_parsed.get_child("lr-conns")) {
        LeftRightConn lrc;
        lrc.name = lr.first;
        lrc.row = lr.second.get<int>("row");

        auto rs = lr.second.get_child("row-span").begin();
        lrc.row_span.first = rs->second.get_value<int>();
        rs++;
        lrc.row_span.second = rs->second.get_value<int>();

        glbs.lr_conns.push_back(lrc);
    }

    // The column is a string the in JSON file so the JSON is easier to read
    // (because JSON doesn't have comments). Columns need to be parsed in
    // order.
    int col_no = 0;
    for (const pt::ptree::value_type &ud : glb_parsed.get_child("ud-conns")) {
        std::vector<int> udc;

        assert(col_no == std::stoi(ud.first));

        for(const pt::ptree::value_type &glb_no : ud.second.get_child(""))
        {
            udc.push_back(glb_no.second.get<int>(""));
        }

        glbs.ud_conns.push_back(udc);
        col_no++;
    }

    col_no = 0;
    for (const pt::ptree::value_type &spans : glb_parsed.get_child("branch-spans")) {
        std::vector<pair<int, int> > sp;

        assert(col_no == std::stoi(spans.first));

        for(auto global_no : glbs.ud_conns[col_no])
        {
          std::pair<int, int> sr;

          string global_key = std::to_string(global_no);
          auto curr_sr = spans.second.get_child(global_key).begin();

          sr.first = curr_sr->second.get_value<int>();
          curr_sr++;
          sr.second = curr_sr->second.get_value<int>();

          sp.push_back(sr);
        }

        glbs.branch_spans.push_back(sp);
        col_no++;
    }

    for (const pt::ptree::value_type &dccs : glb_parsed.get_child("missing-dccs")) {
        MissingDccs missing;

        missing.row = std::stoi(dccs.first);

        for(const pt::ptree::value_type &dcc_no : dccs.second.get_child(""))
        {
            missing.missing.push_back(dcc_no.second.get<int>(""));
        }

        glbs.missing_dccs.push_back(missing);
    }

    return glbs;
}

vector<TileInfo> get_device_tilegrid(const DeviceLocator &part) {
    vector <TileInfo> tilesInfo;
    assert(db_root != "");
    string tilegrid_path = db_root + "/" + part.family + "/" + part.device + "/tilegrid.json";
    {
        ChipInfo info = get_chip_info(part);
#ifndef NO_THREADS
        lock_guard <mutex> lock(tilegrid_cache_mutex);
#endif
        if (tilegrid_cache.find(part.device) == tilegrid_cache.end()) {
            pt::ptree tg_parsed;
            pt::read_json(tilegrid_path, tg_parsed);
            tilegrid_cache[part.device] = tg_parsed;
        }
        const pt::ptree &tg = tilegrid_cache[part.device];

        for (const pt::ptree::value_type &tile : tg) {
            TileInfo ti;
            ti.family = part.family;
            ti.device = part.device;
            ti.max_col = info.max_col;
            ti.max_row = info.max_row;
            ti.col_bias = info.col_bias;

            ti.name = tile.first;
            ti.num_frames = size_t(tile.second.get<int>("cols"));
            ti.bits_per_frame = size_t(tile.second.get<int>("rows"));
            ti.bit_offset = size_t(tile.second.get<int>("start_bit"));
            ti.frame_offset = size_t(tile.second.get<int>("start_frame"));
            ti.type = tile.second.get<string>("type");
            for (const pt::ptree::value_type &site : tile.second.get_child("sites")) {
                SiteInfo si;
                si.type = site.second.get<string>("name");
                si.col = site.second.get<int>("pos_col");
                si.row = site.second.get<int>("pos_row");
                ti.sites.push_back(si);
            }
            tilesInfo.push_back(ti);
        }
    }
    return tilesInfo;
}

static unordered_map<TileLocator, shared_ptr<TileBitDatabase>> bitdb_store;
#ifndef NO_THREADS
static mutex bitdb_store_mutex;
#endif

shared_ptr<TileBitDatabase> get_tile_bitdata(const TileLocator &tile) {
#ifndef NO_THREADS
    lock_guard <mutex> bitdb_store_lg(bitdb_store_mutex);
#endif
    if (bitdb_store.find(tile) == bitdb_store.end()) {
        assert(!db_root.empty());
        string bitdb_path = db_root + "/" + tile.family + "/tiledata/" + tile.tiletype + "/bits.db";
        shared_ptr <TileBitDatabase> bitdb{new TileBitDatabase(bitdb_path)};
        bitdb_store[tile] = bitdb;
        return bitdb;
    } else {
        return bitdb_store.at(tile);
    }
}

}
