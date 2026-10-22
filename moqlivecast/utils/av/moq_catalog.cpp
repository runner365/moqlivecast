#include "moq_catalog.hpp"

#include "utils/json.hpp"

namespace cpp_streamer {

bool HexDecode(const std::string &in, std::vector<uint8_t> *out) {
    if (!out) return false;
    if (in.size() % 2 != 0) return false;
    out->clear();
    out->reserve(in.size() / 2);
    for (size_t i = 0; i < in.size(); i += 2) {
        int hi = 0, lo = 0;
        for (int k = 0; k < 2; k++) {
            const char c = in[i + static_cast<size_t>(k)];
            int v = -1;
            if (c >= '0' && c <= '9')      v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) return false;
            if (k == 0) hi = v; else lo = v;
        }
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool ParseMoqCatalog(const std::string &json, MoqCatalogInfo *out) {
    if (!out) return false;
    nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    out->renditions.clear();
    out->clock_wall = 0;
    out->clock_timescale = 0;

    auto cit = j.find("clock");
    if (cit != j.end() && cit->is_object()) {
        out->clock_wall = cit->value("wall", static_cast<uint64_t>(0));
        out->clock_timescale = cit->value("timescale", static_cast<uint64_t>(0));
    }

    /* 只认 video / audio 两节；其余扩展节一律忽略。 */
    static const char *const kKinds[] = {"video", "audio"};
    for (size_t k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        const std::string kind = kKinds[k];
        auto sit = j.find(kind);
        if (sit == j.end() || !sit->is_object()) continue;
        auto rit = sit->find("renditions");
        if (rit == sit->end() || !rit->is_object()) continue;

        for (auto it = rit->begin(); it != rit->end(); ++it) {
            if (!it.value().is_object()) continue;

            MoqCatalogRendition r;
            r.name = it.key();
            r.kind = kind;
            r.codec = it.value().value("codec", std::string());
            r.width = it.value().value("codedWidth", static_cast<uint32_t>(0));
            r.height = it.value().value("codedHeight", static_cast<uint32_t>(0));
            r.sample_rate = it.value().value("sampleRate", static_cast<uint32_t>(0));
            r.channels = it.value().value("numberOfChannels",
                                          static_cast<uint32_t>(0));
            r.bitrate = it.value().value("bitrate", static_cast<uint32_t>(0));

            auto dit = it.value().find("description");
            if (dit != it.value().end() && dit->is_string()) {
                HexDecode(dit->get<std::string>(), &r.config);
            }

            r.usable = (kind == "video" && r.codec.compare(0, 5, "avc1.") == 0) ||
                       (kind == "audio" && r.codec.compare(0, 8, "mp4a.40.") == 0);
            out->renditions[r.name] = std::move(r);
        }
    }
    return true;
}

} /* namespace cpp_streamer */
