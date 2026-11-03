/**
 * @file ip_utils.c
 * @brief IP ve ICMP Protokol Yardımcıları (IP Utilities) Gerçeklemesi.
 *
 * RFC 791 (IPv4), RFC 792 (ICMP) ve RFC 1071 (Internet Checksum) standartlarına
 * uygun olarak ham paket işleme ve metin formatlama işlevlerini yürütür.
 */

#include "ip_utils.h"

#include <stdio.h>
#include <string.h>

/** Standart IPv4 başlık uzunluğu (seçenekler hariç, bayt) */
#define IPV4_HDR_LEN 20

/** Standart ICMP başlık uzunluğu (bayt) */
#define ICMP_HDR_LEN 8

/**
 * @brief RFC 1071 standardına uygun 16-bit Internet Checksum hesaplar.
 */
uint16_t ip_utils_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i = 0;

    while (i + 1 < len) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
        i += 2;
    }
    if (i < len) {
        sum += (uint32_t)data[i] << 8;
    }
    while (sum > 0xFFFF) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/**
 * @brief 16-bit tamsayıyı hedef belleğe Big-Endian (Network Byte Order) olarak yazar.
 */
static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/**
 * @brief IPv4 sözde başlığı (Pseudo-Header) ve Layer 4 yükü üzerinden RFC 1071 sağlama toplamı hesaplar.
 *
 * TCP (RFC 793) ve UDP (RFC 768) için kullanılır:
 * Pseudo-Header = [Src IP (4B)] + [Dst IP (4B)] + [0x00 (1B)] + [Protocol (1B)] + [L4 Length (2B)]
 *
 * @param src_ip Ağ bayt sırasında kaynak IPv4 adresi işaretçisi (4 bayt).
 * @param dst_ip Ağ bayt sırasında hedef IPv4 adresi işaretçisi (4 bayt).
 * @param proto L4 Protokol numarası (6: TCP, 17: UDP).
 * @param l4_data L4 başlık ve verisini içeren bellek işaretçisi.
 * @param l4_len L4 başlık ve verisinin toplam bayt uzunluğu.
 * @return uint16_t 16-bit 1's complement sağlama toplamı.
 */
static uint16_t ip_utils_l4_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                                     uint8_t proto, const uint8_t *l4_data, size_t l4_len)
{
    uint32_t sum = 0;

    /* 1. Kaynak ve Hedef IP (8 bayt) */
    for (size_t i = 0; i < 4; i += 2) {
        sum += ((uint32_t)src_ip[i] << 8) | src_ip[i + 1];
        sum += ((uint32_t)dst_ip[i] << 8) | dst_ip[i + 1];
    }

    /* 2. Zero (1B) + Proto (1B) */
    sum += proto;

    /* 3. L4 Length (2B) */
    sum += (uint32_t)l4_len;

    /* 4. L4 Başlığı ve Veri Yükü */
    size_t i = 0;
    while (i + 1 < l4_len) {
        sum += ((uint32_t)l4_data[i] << 8) | l4_data[i + 1];
        i += 2;
    }
    if (i < l4_len) {
        sum += (uint32_t)l4_data[i] << 8;
    }

    /* 5. Taşan bitleri (carry) katla */
    while (sum > 0xFFFF) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

void ip_utils_update_checksums(uint8_t *pkt, size_t len)
{
    if (!pkt || len < IPV4_HDR_LEN) {
        return;
    }

    const unsigned ihl = (unsigned)(pkt[0] & 0x0F) * 4u;
    if (ihl < IPV4_HDR_LEN || len < ihl) {
        return;
    }

    /* 1. IPv4 Header Checksum sıfırla ve yeniden hesapla (RFC 791) */
    pkt[10] = 0;
    pkt[11] = 0;
    const uint16_t ip_csum = ip_utils_checksum(pkt, ihl);
    put_be16(pkt + 10, ip_csum);

    const uint8_t proto = pkt[9];
    const size_t l4_len = len - ihl;
    uint8_t *l4_data = pkt + ihl;
    const uint8_t *src_ip = pkt + 12;
    const uint8_t *dst_ip = pkt + 16;

    /* 2. Protokol ICMP ise ICMP Checksum sıfırla ve yeniden hesapla (RFC 792) */
    if (proto == 1 && l4_len >= ICMP_HDR_LEN) {
        l4_data[2] = 0;
        l4_data[3] = 0;
        const uint16_t icmp_csum = ip_utils_checksum(l4_data, l4_len);
        put_be16(l4_data + 2, icmp_csum);
    }
    /* 3. Protokol TCP ise TCP Checksum sıfırla ve yeniden hesapla (RFC 793) */
    else if (proto == 6 && l4_len >= 20) {
        /* TCP Checksum alanı TCP başlığının 16. baytındadır (offset 16-17) */
        l4_data[16] = 0;
        l4_data[17] = 0;
        const uint16_t tcp_csum = ip_utils_l4_checksum(src_ip, dst_ip, proto, l4_data, l4_len);
        put_be16(l4_data + 16, tcp_csum);
    }
    /* 4. Protokol UDP ise UDP Checksum sıfırla ve yeniden hesapla (RFC 768) */
    else if (proto == 17 && l4_len >= 8) {
        /* UDP Checksum alanı UDP başlığının 6. baytındadır (offset 6-7) */
        l4_data[6] = 0;
        l4_data[7] = 0;
        uint16_t udp_csum = ip_utils_l4_checksum(src_ip, dst_ip, proto, l4_data, l4_len);
        /* RFC 768: Checksum 0 çıkarsa 0xFFFF olarak gönderilmelidir (0x0000 checksum yok demektir) */
        if (udp_csum == 0) {
            udp_csum = 0xFFFF;
        }
        put_be16(l4_data + 6, udp_csum);
    }
}

/**
 * @brief IPv4 ICMP Echo Request paketi üretir.
 */
size_t ip_utils_build_icmp_echo(uint8_t *buf, size_t buf_len,
                                uint32_t src_be, uint32_t dst_be,
                                uint16_t ip_id, uint16_t icmp_id, uint16_t seq,
                                const uint8_t *payload, size_t payload_len)
{
    if (!buf || (payload_len > 0 && !payload)) {
        return 0;
    }
    if (payload_len > 65535u - IPV4_HDR_LEN - ICMP_HDR_LEN) {
        return 0;
    }

    const size_t total = IPV4_HDR_LEN + ICMP_HDR_LEN + payload_len;
    if (buf_len < total) {
        return 0;
    }

    memset(buf, 0, total);

    /* --- IPv4 Başlığı --- */
    buf[0] = 0x45;                      /* Version 4, IHL 5 (20 bayt) */
    put_be16(buf + 2, (uint16_t)total); /* Toplam uzunluk */
    put_be16(buf + 4, ip_id);           /* Paket Kimliği (Identification) */
    buf[8] = 64;                        /* TTL = 64 */
    buf[9] = 1;                         /* Protokol: 1 (ICMP) */
    memcpy(buf + 12, &src_be, 4);       /* Kaynak IP */
    memcpy(buf + 16, &dst_be, 4);       /* Hedef IP */
    put_be16(buf + 10, ip_utils_checksum(buf, IPV4_HDR_LEN)); /* IP Checksum */

    /* --- ICMP Başlığı ve Yükü --- */
    uint8_t *icmp = buf + IPV4_HDR_LEN;
    icmp[0] = 8;                         /* Type 8: Echo Request */
    icmp[1] = 0;                         /* Code 0 */
    put_be16(icmp + 4, icmp_id);         /* Identifier */
    put_be16(icmp + 6, seq);             /* Sequence Number */
    if (payload_len > 0) {
        memcpy(icmp + ICMP_HDR_LEN, payload, payload_len);
    }
    put_be16(icmp + 2, ip_utils_checksum(icmp, ICMP_HDR_LEN + payload_len)); /* ICMP Checksum */

    return total;
}

size_t ip_utils_build_icmp_reply(uint8_t *buf, size_t buf_len,
                                 uint32_t src_be, uint32_t dst_be,
                                 uint16_t ip_id, uint16_t icmp_id, uint16_t seq,
                                 const uint8_t *payload, size_t payload_len)
{
    if (!buf || (payload_len > 0 && !payload)) {
        return 0;
    }
    if (payload_len > 65535u - IPV4_HDR_LEN - ICMP_HDR_LEN) {
        return 0;
    }

    const size_t total = IPV4_HDR_LEN + ICMP_HDR_LEN + payload_len;
    if (buf_len < total) {
        return 0;
    }

    memset(buf, 0, total);

    /* --- IPv4 Başlığı --- */
    buf[0] = 0x45;                      /* Version 4, IHL 5 (20 bayt) */
    put_be16(buf + 2, (uint16_t)total); /* Toplam uzunluk */
    put_be16(buf + 4, ip_id);           /* Paket Kimliği (Identification) */
    buf[8] = 64;                        /* TTL = 64 */
    buf[9] = 1;                         /* Protokol: 1 (ICMP) */
    memcpy(buf + 12, &src_be, 4);       /* Kaynak IP (Gateway) */
    memcpy(buf + 16, &dst_be, 4);       /* Hedef IP (ESP32 WAN IP) */
    put_be16(buf + 10, ip_utils_checksum(buf, IPV4_HDR_LEN)); /* IP Checksum */

    /* --- ICMP Başlığı ve Yükü (Type 0 = Echo Reply) --- */
    uint8_t *icmp = buf + IPV4_HDR_LEN;
    icmp[0] = 0;                         /* Type 0: Echo Reply */
    icmp[1] = 0;                         /* Code 0 */
    put_be16(icmp + 4, icmp_id);         /* Identifier */
    put_be16(icmp + 6, seq);             /* Sequence Number */
    if (payload_len > 0) {
        memcpy(icmp + ICMP_HDR_LEN, payload, payload_len);
    }
    put_be16(icmp + 2, ip_utils_checksum(icmp, ICMP_HDR_LEN + payload_len)); /* ICMP Checksum */

    return total;
}

/**
 * @brief Ham IPv4 paketini ayrıştırarak okunabilir tek satırlık özete dönüştürür.
 */
void ip_utils_describe(const uint8_t *pkt, size_t len, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';

    if (!pkt || len < IPV4_HDR_LEN || (pkt[0] >> 4) != 4) {
        snprintf(out, out_len, "geçersiz IP (%u byte)", (unsigned)len);
        return;
    }

    const unsigned ihl = (unsigned)(pkt[0] & 0x0F) * 4u;
    if (ihl < IPV4_HDR_LEN || len < ihl) {
        snprintf(out, out_len, "geçersiz IPv4 başlık");
        return;
    }

    const unsigned proto = pkt[9];
    const unsigned total = ((unsigned)pkt[2] << 8) | pkt[3];

    /* ICMP Özel Ayrıştırma */
    if (proto == 1 && len >= ihl + ICMP_HDR_LEN) {
        const uint8_t *icmp = pkt + ihl;
        const char *kind = "icmp";
        if (icmp[0] == 8) {
            kind = "echo";
        } else if (icmp[0] == 0) {
            kind = "echo-reply";
        }
        const unsigned id = ((unsigned)icmp[4] << 8) | icmp[5];
        const unsigned seq = ((unsigned)icmp[6] << 8) | icmp[7];
        snprintf(out, out_len,
                 "%s %u.%u.%u.%u > %u.%u.%u.%u id=%u seq=%u",
                 kind,
                 pkt[12], pkt[13], pkt[14], pkt[15],
                 pkt[16], pkt[17], pkt[18], pkt[19],
                 id, seq);
        return;
    }

    /* Genel IPv4 Ayrıştırma */
    snprintf(out, out_len,
             "IPv4 %u.%u.%u.%u > %u.%u.%u.%u proto=%u len=%u",
             pkt[12], pkt[13], pkt[14], pkt[15],
             pkt[16], pkt[17], pkt[18], pkt[19],
             proto, total);
}

bool ip_utils_parse(const uint8_t *pkt, size_t len, ip_packet_info_t *info)
{
    if (!info) {
        return false;
    }
    memset(info, 0, sizeof(*info));

    if (!pkt || len < IPV4_HDR_LEN) {
        return false;
    }

    info->version = pkt[0] >> 4;
    if (info->version != 4) {
        return false;
    }

    info->ihl_bytes = (pkt[0] & 0x0F) * 4u;
    if (info->ihl_bytes < IPV4_HDR_LEN || len < info->ihl_bytes) {
        return false;
    }

    info->total_len = ((uint16_t)pkt[2] << 8) | pkt[3];
    info->ip_id = ((uint16_t)pkt[4] << 8) | pkt[5];
    info->ttl = pkt[8];
    info->protocol = pkt[9];

    memcpy(&info->src_ip, pkt + 12, 4);
    memcpy(&info->dst_ip, pkt + 16, 4);

    snprintf(info->src_str, sizeof(info->src_str), "%u.%u.%u.%u",
             pkt[12], pkt[13], pkt[14], pkt[15]);
    snprintf(info->dst_str, sizeof(info->dst_str), "%u.%u.%u.%u",
             pkt[16], pkt[17], pkt[18], pkt[19]);

    switch (info->protocol) {
    case 1:
        info->proto_name = "ICMP";
        info->is_icmp = true;
        if (len >= info->ihl_bytes + ICMP_HDR_LEN) {
            const uint8_t *icmp = pkt + info->ihl_bytes;
            info->icmp_type = icmp[0];
            info->icmp_code = icmp[1];
            info->icmp_id = ((uint16_t)icmp[4] << 8) | icmp[5];
            info->icmp_seq = ((uint16_t)icmp[6] << 8) | icmp[7];

            if (info->icmp_type == 8) {
                info->type_desc = "Echo Request (Ping)";
            } else if (info->icmp_type == 0) {
                info->type_desc = "Echo Reply (Ping Cevabi)";
            } else if (info->icmp_type == 3) {
                info->type_desc = "Hedefe Ulasilamadi (Unreachable)";
            } else {
                info->type_desc = "Diger ICMP";
            }
        } else {
            info->type_desc = "Eksik ICMP Basligi";
        }
        break;
    case 6:
        info->proto_name = "TCP";
        info->type_desc = "TCP Iletim Paketi";
        if (len >= info->ihl_bytes + 4) {
            const uint8_t *tcp = pkt + info->ihl_bytes;
            info->src_port = ((uint16_t)tcp[0] << 8) | tcp[1];
            info->dst_port = ((uint16_t)tcp[2] << 8) | tcp[3];
        }
        break;
    case 17:
        info->proto_name = "UDP";
        info->type_desc = "UDP Datagram";
        if (len >= info->ihl_bytes + 4) {
            const uint8_t *udp = pkt + info->ihl_bytes;
            info->src_port = ((uint16_t)udp[0] << 8) | udp[1];
            info->dst_port = ((uint16_t)udp[2] << 8) | udp[3];
        }
        break;
    default:
        info->proto_name = "Bilinmeyen";
        info->type_desc = "Bilinmeyen Ust Protokol";
        break;
    }

    return true;
}
