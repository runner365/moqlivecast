#ifndef RTCP_TCC_FB_HPP
#define RTCP_TCC_FB_HPP

#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <iostream>
#include <assert.h>

#include "rtprtcp_pub.hpp"
#include "rtcp_fb_pub.hpp"

namespace cpp_streamer
{
/* RTP Extensions for Transport-wide Congestion Control
 * draft-holmer-rmcat-transport-wide-cc-extensions-01
 *
 *   0               1               2               3
 *   0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|  FMT=15 |    PT=205     |           length              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                     SSRC of packet sender                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                      SSRC of media source                     |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |      base sequence number     |      packet status count      |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                 reference time                | fb pkt. count |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |          packet chunk         |         packet chunk          |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  .                                                               .
 *  .                                                               .
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |         packet chunk          |  recv delta   |  recv delta   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  .                                                               .
 *  .                                                               .
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |           recv delta          |  recv delta   | zero padding  |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * */

/*
 * 说明：
 *  - 这里实现的是一个「简化版」的 TCC FB 包，仅满足当前工程使用需求。
 *  - 结构和字段布局与草案保持一致，但对 packet chunk / recv delta 的编码采用
 *    非压缩的简单实现，便于后续在此基础上做增强。
 */

class RtcpTccFbPacket
{
public:
    class RcvDeltaInfo
    {
    public:
        RcvDeltaInfo(int16_t delta_ms, uint16_t wide_seq)
            : delta_ms_(delta_ms), wide_seq_(wide_seq)
        {
        }
        ~RcvDeltaInfo() = default;

    public:
        uint16_t delta_ms_ = 0; // 接收时间增量，单位为毫秒
        uint16_t wide_seq_ = 0; // transport-wide 序
    };
public:
    // Accessors for parsed/serialized fields
    uint32_t GetSenderSsrc() const { return sender_ssrc_; }
    uint32_t GetMediaSsrc() const { return media_ssrc_; }
    uint16_t GetBaseSeq() const { return base_seq_; }
    uint16_t GetPacketStatusCount() const { return packet_status_cnt_; }
    uint32_t GetReferenceTime() const { return reference_time_; }
    uint8_t  GetFbPktCount() const { return fb_pkt_count_; }

    const std::vector<RcvDeltaInfo>& GetRecvDeltas() const { return recv_deltas_; }
    const std::vector<uint16_t>& GetPacketChunks() const { return packet_chunks_; }

    class RunLengthChunk
    {
    public:
        RunLengthChunk() {
            data_[0] = 0;
            data_[1] = 0;
        }
        ~RunLengthChunk() = default;

        // T(0)|S(1)|RLE(5)|RunLength(9)
        // T: Type, 0=Run Length Chunk, 1=Status Vector Chunk
        // S: Status, 0=not received, 1=received all
        // RLE: Receive Delta Length, when S==1, it means the length of recv_delta;
        //      when S==0, it means nothing
        // RunLength: number of packets represented by this chunk
    public:
        void SetStatus(uint8_t status) {
            status_ = status & 0x01;
            // on bit 2
            data_[0] |= status_ << 6;
        }
        void SetRecvDeltaLength(uint16_t length) {
            // bit 9 to bit 13
            recv_delta_length_ = length & 0x1F;
            data_[0] |= (recv_delta_length_ & 0x1F) << 1;
        }
        void SetRunLength(uint16_t run_length) {
            run_length_ = run_length & 0x1FF;
            data_[0] = (data_[0] & 0xFE) | ((run_length_ >> 8) & 0x01);
            data_[1] = static_cast<uint8_t>(run_length_ & 0xFF);
        }
        uint16_t GetChunkData() const {
            return (static_cast<uint16_t>(data_[0]) << 8) | static_cast<uint16_t>(data_[1]);
        }
        
    private:
        uint8_t status_ = 0; // 0=not received, 1=received all
        // Receive Delta Length: 
        // when status==1, it means the length of recv_delta; 
        // when status==0, it means nothing
        uint8_t recv_delta_length_ = 0;
        uint16_t run_length_ = 0;

    private:
        uint8_t data_[2] = {0};
    };

public:
    class StatusVectorChunk
    {
    public:
        StatusVectorChunk() {
            data_[0] = 0x80;
            data_[1] = 0;
        }
        ~StatusVectorChunk() = default;

        // T(1)|S(1)|Symbol List(14)
        // T: Type, 0=Run Length Chunk, 1=Status Vector Chunk, so it must be 1
        // S: Symbol, 0=one bit per packet, 1=two bits per packet
        // Symbol List: when S==0, each bit represents one packet status;
        //              when S==1, each 2 bits represent one packet status
    public:
        void SetSymbol(uint8_t symbol) {
            symbol_ = symbol & 0x01;
            // on bit 2
            data_[0] |= symbol_ << 6;
        }
        void SetSymbolList(uint16_t symbol_list) {
            symbol_list_ = symbol_list & 0x3FFF;
            data_[0] |= (symbol_list_ >> 8) & 0x3F;
            data_[1] = static_cast<uint8_t>(symbol_list_ & 0xFF);
        }
        uint16_t GetChunkData() const {
            return (static_cast<uint16_t>(data_[0]) << 8) | static_cast<uint16_t>(data_[1]);
        }

    private:
        uint8_t symbol_ = 0; // 0=one bit per packet, 1=two bits per packet
        uint16_t symbol_list_ = 0; // 14 bits

    private:
        uint8_t data_[2] = {0};
    };

public:
    static constexpr size_t kTccFbPacketMaxSize = 1350;

    RtcpTccFbPacket()
    {
        Reset();
    }

    ~RtcpTccFbPacket() = default;

public:
    bool Serial(uint8_t* buffer, size_t& len) {
        if (recv_deltas_.size() < 2) {
            std::cout << "\r\n***** not enough packets to serial TCC FB *****" << std::endl;
            return false;
        }
        RtcpFbCommonHeader* rtcp_header = (RtcpFbCommonHeader*)buffer;
        rtcp_header->version = 2;
        rtcp_header->padding = 0;
        rtcp_header->fmt = 15; // TCC FB
        rtcp_header->packet_type = 205; // Transport Layer FB
        rtcp_header->length = 0; // to be filled later

        uint8_t* p = buffer + sizeof(RtcpFbCommonHeader);
        RtcpFbHeader* fb_header = (RtcpFbHeader*)p;
        fb_header->media_ssrc = htonl(media_ssrc_);
        fb_header->sender_ssrc = htonl(sender_ssrc_);
        p += sizeof(RtcpFbHeader);

        //set base seq and pkt status count
        uint16_t base_seq_net = htons(base_seq_);
        memcpy(p, &base_seq_net, sizeof(uint16_t));
        p += sizeof(uint16_t);
        uint16_t pkt_status_cnt_net = htons(static_cast<uint16_t>(recv_deltas_.size()));
        memcpy(p, &pkt_status_cnt_net, sizeof(uint16_t));
        p += sizeof(uint16_t);

        //set reference time (only low 24bit used) and fb pkt count
        uint32_t ref_time_and_count = (reference_time_ << 8) | fb_pkt_count_;
        ref_time_and_count = htonl(ref_time_and_count);
        memcpy(p, &ref_time_and_count, sizeof(uint32_t));
        p += sizeof(uint32_t);

        len = sizeof(RtcpFbCommonHeader) + sizeof(RtcpFbHeader) + 8; // up to fb pkt count

        if (!SerialPacketChunks(p, len)) {
            std::cout << "\r\n***** failed to serial TCC FB packet chunks *****" << std::endl;
            return false;
        }

        if (!SerialRecvDelta(p, len)) {
            std::cout << "\r\n***** failed to serial TCC FB recv deltas *****" << std::endl;
            return false;
        }

        // zero padding to 32-bit boundary after deltas
        // RTCP P-bit semantics: if padding is present, the last byte of the packet
        // contains a count of how many padding bytes should be ignored.
        size_t pad = (4 - (len % 4)) % 4;
        if (pad) {
            // mark header P-bit
            rtcp_header->padding = 1;
            // write padding bytes: pad-1 zeros, then 1 byte with padding count
            if (pad > 1) {
                memset(p, 0, pad - 1);
                p += (pad - 1);
                len += (pad - 1);
            }
            // final padding count byte
            *p = static_cast<uint8_t>(pad);
            p += 1;
            len += 1;
        }

        // Fill RTCP header length: number of 32-bit words minus one (including header)
        size_t rtcp_len_words_minus1 = (len / 4) - 1;
        rtcp_header->length = htons(static_cast<uint16_t>(rtcp_len_words_minus1));
        return true;
    }
    // 解析已有的 TCC FB 数据（data 指向 RTCP 头部第一个字节）
    static RtcpTccFbPacket* Parse(uint8_t* data, size_t len)
    {
        if (!data || len < sizeof(RtcpFbCommonHeader) + sizeof(RtcpFbHeader) + 8) {
            return nullptr;
        }

        auto* pkt = new RtcpTccFbPacket();

        // Parse common header
        RtcpFbCommonHeader* rtcp_header = (RtcpFbCommonHeader*)data;
        if (rtcp_header->version != 2 || rtcp_header->packet_type != 205 || rtcp_header->fmt != 15) {
            delete pkt;
            return nullptr;
        }

        // Total length in bytes from header (length is number of 32-bit words minus one)
        uint16_t words_minus1 = ntohs(rtcp_header->length);
        size_t total_len = (words_minus1 + 1) * 4;
        if (total_len > len) {
            delete pkt;
            return nullptr;
        }

        // Handle RTCP padding
        if (rtcp_header->padding) {
            if (total_len == 0) { delete pkt; return nullptr; }
            uint8_t padding_count = data[total_len - 1];
            if (padding_count == 0 || padding_count > total_len) {
                delete pkt;
                return nullptr;
            }
            total_len -= padding_count; // exclude padding bytes from parse range
        }

        uint8_t* p = data + sizeof(RtcpFbCommonHeader);
        uint8_t* end = data + total_len;

        // FB header (sender_ssrc, media_ssrc)
        if (p + sizeof(RtcpFbHeader) > end) { delete pkt; return nullptr; }
        RtcpFbHeader* fb_header = (RtcpFbHeader*)p;
        pkt->sender_ssrc_ = ntohl(fb_header->sender_ssrc);
        pkt->media_ssrc_  = ntohl(fb_header->media_ssrc);
        p += sizeof(RtcpFbHeader);

        // base_seq and packet_status_cnt
        if (p + 4 > end) { delete pkt; return nullptr; }
        pkt->base_seq_ = ntohs(*(uint16_t*)p); p += 2;
        pkt->packet_status_cnt_ = ntohs(*(uint16_t*)p); p += 2;

        // reference time (low 24 bits) and fb pkt count
        if (p + 4 > end) { delete pkt; return nullptr; }
        uint32_t ref_and_cnt = ntohl(*(uint32_t*)p); p += 4;
        pkt->fb_pkt_count_   = (uint8_t)(ref_and_cnt & 0xFF);
        pkt->reference_time_ = (ref_and_cnt >> 8) & 0x00FFFFFF;

        // Parse packet chunks
        // Our serializer writes only StatusVectorChunk with S=0 (one bit per packet), 14 symbols per chunk
        // Continue reading 16-bit chunks until we've accounted for packet_status_cnt_ symbols
        uint16_t symbols_needed = pkt->packet_status_cnt_;
        uint16_t symbols_collected = 0;
        std::vector<uint16_t> chunks;
        while (symbols_collected < symbols_needed) {
            if (p + 2 > end) { delete pkt; return nullptr; }
            uint16_t chunk_net; memcpy(&chunk_net, p, 2); p += 2;
            uint16_t chunk = ntohs(chunk_net);
            bool is_svc = (chunk & 0x8000) != 0;
            bool two_bits = (chunk & 0x4000) != 0;
            if (!is_svc || two_bits) {
                // Unsupported chunk type in this simplified parser
                delete pkt;
                return nullptr;
            }
            uint16_t symbols = (chunk & 0x3FFF);
            (void)symbols;
            chunks.push_back(chunk);
            pkt->packet_chunks_.push_back(chunk);
            symbols_collected += 14;
        }

        // Now parse recv deltas: our serializer stores each delta as 16-bit signed (network order)
        // The number of deltas equals the count of received packets (bits set to 1 across symbols)
        size_t receive_bits = 0;
        (void)receive_bits;
        for (uint16_t chunk : chunks) {
            uint16_t symbols = chunk & 0x3FFF;
            for (int b = 13; b >= 0; --b) { // Keep same bit order as serialization final flush (left shift then fill)
                if (symbols & (1u << b)) receive_bits++;
            }
        }

        // The symbols_needed may exceed exact 14*N; only first symbols_needed bits are valid
        // Count received bits only among the first symbols_needed positions
        size_t valid_receive_bits = 0;
        {
            uint16_t processed = 0;
            for (uint16_t chunk : chunks) {
                uint16_t symbols = chunk & 0x3FFF;
                for (int b = 13; b >= 0; --b) {
                    if (processed >= symbols_needed) break;
                    if (symbols & (1u << b)) valid_receive_bits++;
                    processed++;
                }
                if (processed >= symbols_needed) break;
            }
        }

        // Read that many deltas
        for (size_t i = 0; i < valid_receive_bits; ++i) {
            if (p + 2 > end) { delete pkt; return nullptr; }
            uint16_t net; memcpy(&net, p, 2); p += 2;
            int16_t d = (int16_t)ntohs(net);
            // wide seq reconstruction: walk chunks and accumulate gaps
            pkt->recv_deltas_.push_back(RcvDeltaInfo(d, 0)); // seqs filled later
        }

        // Reconstruct transport-wide sequence numbers based on base_seq_ and chunk bitmaps
        {
            uint16_t seq = pkt->base_seq_;
            size_t delta_index = 0;
            uint16_t processed = 0;
            for (uint16_t chunk : chunks) {
                uint16_t symbols = chunk & 0x3FFF;
                for (int b = 13; b >= 0; --b) {
                    if (processed >= symbols_needed) break;
                    bool received = (symbols & (1u << b)) != 0;
                    if (received) {
                        if (delta_index < pkt->recv_deltas_.size()) {
                            pkt->recv_deltas_[delta_index].wide_seq_ = seq;
                            delta_index++;
                        }
                    }
                    // advance to next seq regardless (received or lost)
                    seq = (uint16_t)(seq + 1);
                    processed++;
                }
                if (processed >= symbols_needed) break;
            }
        }

        return pkt;
    }

    // 是否已经接近 MTU，需要立即发送
    bool IsFullRtcp() const {
        const size_t kMaxRecvRtpPacketNumber = 300;
        if (recv_deltas_.size() >= kMaxRecvRtpPacketNumber) {
            return true;
        }
        return false;
    }

    size_t PacketCount() const { return recv_deltas_.size(); }

    int64_t OldestPacketTimeMs() const
    {
        return has_first_packet_ ? base_time_ms_ : 0;
    }

    // 插入一个接收到的 RTP 包
    // wideSeq: transport-wide seq
    // now_ms : 当前接收时间（毫秒）
    // return: 
    // < 0 on error
    // == 0, on success
    // return values:
    //  < 0: error
    //   0: inserted into current feedback batch
    //   1: exceeded delta range, batch should be flushed and a new batch started with this packet (caller should react)
    int InsertPacket(int wideSeq, int64_t now_ms) {
        if (wideSeq < 0 || wideSeq > 0xFFFF) {
            return -1;
        }

        if (!has_first_packet_) {
            has_first_packet_ = true;
            base_seq_     = (uint16_t)wideSeq;
            base_time_ms_ = now_ms;
            last_wide_seq_ = (uint16_t)wideSeq;

            // 参考时间按照草案单位（1/64s）做一个近似换算，仅保留 24bit
            reference_time_ = (uint32_t)((now_ms / 16) & 0x00FFFFFF);
            return 0;
        } else {
            if (wideSeq == last_wide_seq_) {
                return 0;
            }
        }

        if (last_wide_seq_ != 0) {
            if (SeqLowerThan((uint16_t)wideSeq, last_wide_seq_)) {
                // 乱序，暂不处理
                return -1;
            }
        }
        // 简化实现：每个包一个 recv delta，单位毫秒
        int64_t delta_ms = now_ms - base_time_ms_;
        const int64_t max_delta = 5*1000; // 5 秒以内的包才记录
        if (delta_ms < -max_delta || delta_ms > max_delta) {
            // 提示上层：当前批次应当 flush，随后用该包开启新批次
            // 不在本批次内写入该包，改由外层在新批次调用 InsertPacket。
            return 1;
        }
        base_time_ms_ = now_ms;

        recv_deltas_.push_back(RcvDeltaInfo(static_cast<int16_t>(delta_ms), static_cast<uint16_t>(wideSeq)));

        last_wide_seq_ = (uint16_t)wideSeq;

        return 0;
    }

    void SetSsrc(uint32_t sender_ssrc, uint32_t media_ssrc) {
        sender_ssrc_ = sender_ssrc;
        media_ssrc_  = media_ssrc;
    }

    void SetFbPktCount(uint8_t cnt) { fb_pkt_count_ = cnt; }

    void Reset() {
        sender_ssrc_        = 0;
        media_ssrc_         = 0;
        base_seq_           = 0;
        packet_status_cnt_  = 0;
        reference_time_     = 0;
        fb_pkt_count_       = 0;
        base_time_ms_       = 0;
        has_first_packet_   = false;
        packet_chunks_.clear();
        recv_deltas_.clear();
    }

private:
    bool SerialPacketChunks(uint8_t*& p, size_t& len) {
        // packet chunks
        // set all packets as StatusVectorChunk with one bit per packet
        auto last_pkt_info = recv_deltas_[0];
        size_t idx = 1;
        uint16_t current_chunk = 1;
        uint16_t packets_in_chunk = 1;
        uint8_t* start_p = p;
        size_t deltas_size = recv_deltas_.size();
        
        // std::cout << "\r\n***** SerialPacketChunks, deltas_size: " << deltas_size << std::endl;
        // std::cout << "***** start_p: " << static_cast<void*>(start_p) << std::endl;
        while (idx < deltas_size) {
            uint16_t gap = 0;
            // std::cout << "***** processing idx: " << idx << std::endl;
            // std::cout << "***** p: " << static_cast<void*>(p) << std::endl;

            auto current_pkt_info = recv_deltas_[idx];

            if (current_pkt_info.wide_seq_ < last_pkt_info.wide_seq_) {
                gap = 0xffff + current_pkt_info.wide_seq_ + 1 - last_pkt_info.wide_seq_;
            } else {
                gap = current_pkt_info.wide_seq_ - last_pkt_info.wide_seq_;
            }

            if (gap == 1) {
                // received
                current_chunk = (current_chunk << 1) | 0x01;
                packets_in_chunk++;
            } else {
                // lost packets
                if (packets_in_chunk + gap < 14) {
                    for (uint16_t i = 0; i < gap - 1; i++) {
                        current_chunk = (current_chunk << 1);
                        packets_in_chunk++;
                    }
                    // received current packet
                    current_chunk = (current_chunk << 1) | 0x01;
                    packets_in_chunk++;
                } else {
                    uint16_t space_left = 14 - packets_in_chunk;
                    for (uint16_t i = 0; i < space_left - 1; i++) {
                        current_chunk = (current_chunk << 1);
                        packets_in_chunk++;
                    }
                    // flush current chunk
                    StatusVectorChunk svc;
                    svc.SetSymbol(0); // one bit per packet
                    svc.SetSymbolList(current_chunk);
                    uint16_t chunk_data = svc.GetChunkData();
                    uint16_t chunk_data_net = htons(chunk_data);
                    memcpy(p, &chunk_data_net, sizeof(uint16_t));
                    p += sizeof(uint16_t);
                    len += sizeof(uint16_t);

                    // reset
                    current_chunk = 0;
                    packets_in_chunk = 1;
                    // process the remaining lost packets
                    uint16_t remaining_lost = gap - space_left;
                    while (remaining_lost >= 14) {
                        // flush a full lost chunk
                        StatusVectorChunk lost_svc;
                        lost_svc.SetSymbol(0); // one bit per packet
                        lost_svc.SetSymbolList(0x0000); // all lost
                        uint16_t lost_chunk_data = lost_svc.GetChunkData();
                        uint16_t lost_chunk_data_net = htons(lost_chunk_data);
                        memcpy(p, &lost_chunk_data_net, sizeof(uint16_t));
                        p += sizeof(uint16_t);
                        len += sizeof(uint16_t);
                        remaining_lost -= 14;
                    }
                    // process remaining lost packets
                    current_chunk = 0;
                    packets_in_chunk = 0;
                    for (uint16_t i = 0; i < remaining_lost - 1; i++) {
                        current_chunk = (current_chunk << 1);
                        packets_in_chunk++;
                    }
                    // received current packet
                    current_chunk = (current_chunk << 1) | 0x01;
                    packets_in_chunk++;

                }
            }
            
            int64_t offset = p - start_p;
            assert(offset <= 512); // max 256 chunks
            if (packets_in_chunk == 14) {
                // flush current chunk
                StatusVectorChunk svc;
                svc.SetSymbol(0); // one bit per packet
                svc.SetSymbolList(current_chunk);
                uint16_t chunk_data = svc.GetChunkData();
                uint16_t chunk_data_net = htons(chunk_data);
                memcpy(p, &chunk_data_net, sizeof(uint16_t));
                p += sizeof(uint16_t);
                len += sizeof(uint16_t);

                // reset
                current_chunk = 0;
                packets_in_chunk = 0;
            } else {
                if (packets_in_chunk > 14) {
                    std::cout << "\r\n***** packets_in_chunk(too large): " << packets_in_chunk << std::endl;
                }
                assert(packets_in_chunk < 14);
            }
            // advance to next packet info
            last_pkt_info = current_pkt_info;
            idx++;
        }
        // flush remaining chunk
        if (packets_in_chunk > 1) {
            StatusVectorChunk svc;
            svc.SetSymbol(0); // one bit per packet
            //remain chunk must be shifted
            current_chunk = current_chunk << (14 - packets_in_chunk);
            svc.SetSymbolList(current_chunk);
            uint16_t chunk_data = svc.GetChunkData();
            uint16_t chunk_data_net = htons(chunk_data);
            memcpy(p, &chunk_data_net, sizeof(uint16_t));
            p += sizeof(uint16_t);
            len += sizeof(uint16_t);
        }

        // 位图计数断言：统计写出的 chunk 中的“收到”位（1）的数量应等于 recv_deltas_.size()
        size_t ones_count = 0;
        uint8_t* q = start_p;
        while (q < p) {
            uint16_t chunk_net;
            memcpy(&chunk_net, q, sizeof(uint16_t));
            uint16_t chunk = ntohs(chunk_net);
            // 仅处理 StatusVectorChunk (T=1, bit15=1) 且 S=0 (bit14=0)
            bool is_svc = (chunk & 0x8000) != 0;
            bool symbol_two_bits = (chunk & 0x4000) != 0; // S=1 表示两位/包（当前实现未使用）
            if (is_svc && !symbol_two_bits) {
                uint16_t symbols = chunk & 0x3FFF;
                // 统计 14 位内的 1 的个数
                for (int b = 0; b < 14; ++b) {
                    if (symbols & (1u << b)) ones_count++;
                }
            }
            q += 2;
        }
        assert(ones_count <= recv_deltas_.size());
        return true;
    }
    // Serialize recv delta list (each delta as 16-bit signed, network byte order)
    bool SerialRecvDelta(uint8_t*& p, size_t& len) {
        for (const auto& info : recv_deltas_) {
            int16_t d = info.delta_ms_;
            // std::cout << "*** detal:" << d << "\r\n";
            uint16_t net = htons(static_cast<uint16_t>(d));
            memcpy(p, &net, sizeof(uint16_t));
            p += sizeof(uint16_t);
            len += sizeof(uint16_t);
        }
        return true;
    }
private:
    uint32_t sender_ssrc_       = 0;
    uint32_t media_ssrc_        = 0;
    uint16_t base_seq_          = 0;
    uint16_t packet_status_cnt_ = 0;
    uint32_t reference_time_    = 0; // 仅使用低 24bit
    uint8_t  fb_pkt_count_      = 0;

    std::vector<RcvDeltaInfo>  recv_deltas_;// deltas, seqs
    std::vector<uint16_t> packet_chunks_;

    // 仅用于计算 delta
    int64_t base_time_ms_     = 0;
    bool    has_first_packet_ = false;

    uint16_t last_wide_seq_  = 0;
};

}
#endif


