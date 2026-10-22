#ifndef MOQ_CATALOG_HPP
#define MOQ_CATALOG_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cpp_streamer {

/* catalog.json 里一条 rendition 的信息（只收本服务端能落地成 FLV 的两种
 * 编解码器：H.264 / AAC）。字段含义见 moq_lite_session.cpp 的 OnCatalogFrame
 * 注释：description 是 **hex** 的 avcC / AudioSpecificConfig 原字节。 */
struct MoqCatalogRendition {
    std::string name;      /* rendition key == track 名 */
    std::string kind;      /* "video" | "audio" */
    std::string codec;     /* "avc1.PPCCLL" / "mp4a.40.2" ... */
    std::vector<uint8_t> config;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bitrate = 0;
    /* 本服务端能把它拼成 FLV 序列头并送给 MediaStreamManager。 */
    bool usable = false;
};

struct MoqCatalogInfo {
    std::map<std::string, MoqCatalogRendition> renditions;  /* 按名字升序 */
    uint64_t clock_wall = 0;
    uint64_t clock_timescale = 0;
};

/* 解析一份 catalog JSON。失败（不是对象 / 语法错误）返回 false；
 * 未知字段、未知 rendition 一律忽略（与参考实现的容错规则一致）。 */
bool ParseMoqCatalog(const std::string &json, MoqCatalogInfo *out);

/* hex（无前缀、无分隔符）解码。奇数长度或含非 hex 字符返回 false。 */
bool HexDecode(const std::string &in, std::vector<uint8_t> *out);

} /* namespace cpp_streamer */

#endif /* MOQ_CATALOG_HPP */
