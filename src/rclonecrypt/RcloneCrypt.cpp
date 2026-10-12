// N98 §1：rclone crypt 纯算法层的实现。蓝本为探针 _crypt_probe 的
// Encoding.cpp / Eme.cpp / Cipher.cpp / Sodium.cpp 四文件；判定口径（base32hex
// 接受域、PKCS#7 五分支、错误码顺序）逐一保持，宽松解码会把明文名误判成密文名。

#include "RcloneCrypt.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace {

using RcloneCryptBytes = RcloneCrypt::Bytes;

// ——EME（ECB-Mix-ECB，Halevi–Rogaway 2003）——确定性（同明文同 tweak ⇒ 同密文）
// 是 rclone 名字加密的硬要求，故不用随机 IV。前置：len ∈ {16, 32, …, 2048}。

// GF(2^128) 乘 x，小端字节序：整块左移一位，越界项按 0x87 既约
// （rclone 侧写作 135 & -(in[15]>>7)，同式）
void multByTwo(std::uint8_t b[16]) {
    const std::uint8_t fold = (b[15] >> 7) ? std::uint8_t(135) : std::uint8_t(0);
    std::uint8_t carry = 0;
    for (int i = 0; i < 16; ++i) {
        const std::uint8_t next = b[i] >> 7;
        b[i] = std::uint8_t((b[i] << 1) | carry);
        carry = next;
    }
    b[0] ^= fold;
}

void xorInto(std::uint8_t *dst, const std::uint8_t *src) {
    for (int i = 0; i < 16; ++i)
        dst[i] = std::uint8_t(dst[i] ^ src[i]);
}

void emeTransform(const AesBlock &bc, const std::uint8_t tweak[16], const std::uint8_t *in,
                  std::size_t len, bool decrypt, RcloneCryptBytes &out) {
    const std::size_t m = len / 16;

    // L[j] = multByTwo^(j+1)( AES_enc(K, 0) )——恒用加密方向，与方向无关
    RcloneCryptBytes L(m * 16);
    std::uint8_t acc[16] = {};
    bc.encryptBlock(acc, acc);
    for (std::size_t j = 0; j < m; ++j) {
        multByTwo(acc);
        std::memcpy(L.data() + j * 16, acc, 16);
    }

    auto mix = [&](const std::uint8_t src[16], std::uint8_t dst[16]) {
        if (decrypt)
            bc.decryptBlock(src, dst);
        else
            bc.encryptBlock(src, dst);
    };

    RcloneCryptBytes C(len);
    std::uint8_t tmp[16];
    // PPj = Pj ^ L[j]，再分组运算
    for (std::size_t j = 0; j < m; ++j) {
        std::memcpy(tmp, in + j * 16, 16);
        xorInto(tmp, L.data() + j * 16);
        mix(tmp, C.data() + j * 16);
    }

    // MP = (xorSum C) ^ T；MC = AES_dir(MP)；M = MP ^ MC
    std::uint8_t MP[16], MC[16], M[16];
    std::memcpy(MP, C.data(), 16);
    for (std::size_t j = 1; j < m; ++j)
        xorInto(MP, C.data() + j * 16);
    xorInto(MP, tweak);

    mix(MP, MC);

    std::memcpy(M, MP, 16);
    xorInto(M, MC);
    for (std::size_t j = 1; j < m; ++j) {
        multByTwo(M);
        xorInto(C.data() + j * 16, M);
    }

    // CCC1 = MC ^ T ^ (xorSum_{j>=1} C[j])
    std::uint8_t CCC1[16];
    std::memcpy(CCC1, MC, 16);
    xorInto(CCC1, tweak);
    for (std::size_t j = 1; j < m; ++j)
        xorInto(CCC1, C.data() + j * 16);
    std::memcpy(C.data(), CCC1, 16);

    for (std::size_t j = 0; j < m; ++j) {
        std::memcpy(tmp, C.data() + j * 16, 16);
        mix(tmp, C.data() + j * 16);
        xorInto(C.data() + j * 16, L.data() + j * 16);
    }

    out = std::move(C);
}

// ——名字编码——

constexpr char kBase32Hex[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV"; // RFC4648 hex 表
constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int tableLookup(const char *table, std::size_t tableLen, char c) {
    for (std::size_t i = 0; i < tableLen; ++i) {
        if (table[i] == c)
            return int(i);
    }
    return -1;
}

bool anyPadding(std::string_view s) { return s.find('=') != std::string_view::npos; }

// ASCII 大小写不敏感等值比较（rclone 的 crypt-suffix=="none" 走 strings.EqualFold）
char toLowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLowerAscii(a[i]) != toLowerAscii(b[i]))
            return false;
    }
    return true;
}

} // namespace

// ——名字编码与 PKCS7：RcloneCrypt 的静态成员（实现自写，公开面为测试触达口径）——

std::string RcloneCrypt::base32HexEncode(const Bytes &in) {
    std::string out;
    out.reserve((in.size() * 8 + 4) / 5);
    const std::size_t full = in.size() / 5;
    for (std::size_t g = 0; g < full; ++g) {
        std::uint64_t v = 0;
        for (std::size_t j = 0; j < 5; ++j)
            v = (v << 8) | in[g * 5 + j];
        for (int s = 35; s >= 0; s -= 5)
            out += kBase32Hex[(v >> s) & 0x1F];
    }
    const std::size_t rem = in.size() % 5;
    if (rem) {
        std::uint64_t v = 0;
        for (std::size_t j = 0; j < rem; ++j)
            v = (v << 8) | in[full * 5 + j];
        v <<= (5 - rem) * 8;
        const int chars = int((rem * 8 + 4) / 5);
        for (int c = 0; c < chars; ++c)
            out += kBase32Hex[(v >> (35 - 5 * c)) & 0x1F];
    }
    // 输出小写（rclone 口径）
    for (char &c : out) {
        if (c >= 'A' && c <= 'V')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::expected<RcloneCrypt::Bytes, RcloneCrypt::Error>
RcloneCrypt::base32HexDecode(std::string_view s) {
    if (s.empty())
        return Bytes();
    // 串中出现任何 '=' 判坏（rclone 只显式拒绝末尾 '='，Go 解码器对中间同样判坏）
    if (anyPadding(s))
        return std::unexpected(RcloneCrypt::Error::BadBase32Encoding);
    std::uint64_t g = 0;
    int have = 0;
    Bytes out;
    for (const char ch : s) {
        const char up = ch >= 'a' && ch <= 'v' ? static_cast<char>(ch - 'a' + 'A') : ch;
        const int v = tableLookup(kBase32Hex, sizeof(kBase32Hex) - 1, up);
        if (v < 0)
            return std::unexpected(RcloneCrypt::Error::BadBase32Encoding);
        g = (g << 5) | std::uint64_t(v);
        have += 5;
        if (have >= 8) {
            have -= 8;
            out.push_back(char((g >> have) & 0xFF));
        }
    }
    // 残余位接受域：长度 %8 ∈ {1,3,6}（残余 5/6/7 位）无对应字节数 ⇒ 坏编码；
    // 残余 1/2/3/4 位合法但要求这些位全为 0
    if (have >= 5)
        return std::unexpected(RcloneCrypt::Error::BadBase32Encoding);
    if (have > 0 && (g & ((1u << have) - 1u)) != 0)
        return std::unexpected(RcloneCrypt::Error::BadBase32Encoding);
    return out;
}

std::string RcloneCrypt::base64UrlEncode(const Bytes &in) {
    std::string out;
    const std::size_t full = in.size() / 3;
    for (std::size_t g = 0; g < full; ++g) {
        const std::uint32_t v =
            (std::uint32_t(in[g * 3]) << 16) | (std::uint32_t(in[g * 3 + 1]) << 8) | in[g * 3 + 2];
        out += kBase64Url[(v >> 18) & 63];
        out += kBase64Url[(v >> 12) & 63];
        out += kBase64Url[(v >> 6) & 63];
        out += kBase64Url[v & 63];
    }
    const std::size_t rem = in.size() % 3;
    if (rem) {
        std::uint32_t v = std::uint32_t(in[full * 3]) << 16;
        if (rem == 2)
            v |= std::uint32_t(in[full * 3 + 1]) << 8;
        out += kBase64Url[(v >> 18) & 63];
        out += kBase64Url[(v >> 12) & 63];
        if (rem == 2)
            out += kBase64Url[(v >> 6) & 63];
    }
    return out;
}

std::expected<RcloneCrypt::Bytes, RcloneCrypt::Error>
RcloneCrypt::base64UrlDecode(std::string_view s) {
    if (s.empty())
        return Bytes();
    // 以下四处判坏对应 Go base64.RawURLEncoding 的 CorruptInputError（无填充形态
    // 里 '=' 不是表内字符；len%4==1 无对应字节数；残余高位必须为 0）
    if (anyPadding(s))
        return std::unexpected(RcloneCrypt::Error::BadBase64Encoding);
    if (s.size() % 4 == 1)
        return std::unexpected(RcloneCrypt::Error::BadBase64Encoding);
    std::uint32_t g = 0;
    int have = 0;
    Bytes out;
    for (const char ch : s) {
        const int v = tableLookup(kBase64Url, sizeof(kBase64Url) - 1, ch);
        if (v < 0)
            return std::unexpected(RcloneCrypt::Error::BadBase64Encoding);
        g = (g << 6) | std::uint32_t(v);
        have += 6;
        if (have >= 8) {
            have -= 8;
            out.push_back(char((g >> have) & 0xFF));
        }
    }
    if (have > 0 && (g & ((1u << have) - 1u)) != 0)
        return std::unexpected(RcloneCrypt::Error::BadBase64Encoding);
    return out;
}

// ——PKCS#7（块长恒 16）——

RcloneCrypt::Bytes RcloneCrypt::pkcs7Pad(std::string_view in) {
    Bytes out(in.begin(), in.end());
    const std::size_t pad = 16 - (out.size() % 16);
    out.insert(out.end(), pad, std::uint8_t(pad));
    return out;
}

std::expected<RcloneCrypt::Bytes, RcloneCrypt::Error> RcloneCrypt::pkcs7Unpad(const Bytes &in) {
    if (in.empty())
        return std::unexpected(RcloneCrypt::Error::PaddingNotFound);
    if (in.size() % 16 != 0)
        return std::unexpected(RcloneCrypt::Error::PaddingNotAMultiple);
    const std::size_t pad = in.back();
    if (pad > 16)
        return std::unexpected(RcloneCrypt::Error::PaddingTooLong);
    if (pad == 0)
        return std::unexpected(RcloneCrypt::Error::PaddingTooShort);
    for (std::size_t i = 0; i < pad; ++i) {
        if (in[in.size() - 1 - i] != std::uint8_t(pad))
            return std::unexpected(RcloneCrypt::Error::PaddingNotAllTheSame);
    }
    return RcloneCryptBytes(in.begin(), in.end() - static_cast<std::ptrdiff_t>(pad));
}

namespace {

// ——版本串（rclone --b2-versions 形态：-vYYYY-MM-DD-HHMMSS-fff，23 字符）——

constexpr std::size_t kVersionLen = 23;

bool isDigits(const std::string &s, std::size_t off, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[off + i];
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

int toInt(const std::string &s, std::size_t off, std::size_t n) {
    int v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v = v * 10 + (s[off + i] - '0');
    return v;
}

// 年月日与时分秒各自成位且日历值在域内
bool versionFieldsOk(const std::string &s, std::size_t base) {
    if (base + kVersionLen > s.size())
        return false;
    if (s.compare(base, 2, "-v") != 0)
        return false;
    if (!isDigits(s, base + 2, 4) || s[base + 6] != '-' || !isDigits(s, base + 7, 2) ||
        s[base + 9] != '-' || !isDigits(s, base + 10, 2) || s[base + 12] != '-' ||
        !isDigits(s, base + 13, 6) || s[base + 19] != '-' || !isDigits(s, base + 20, 3))
        return false;
    const int mon = toInt(s, base + 7, 2), day = int(toInt(s, base + 10, 2));
    const int h = toInt(s, base + 13, 2), mi = toInt(s, base + 15, 2), se = toInt(s, base + 17, 2);
    return mon >= 1 && mon <= 12 && day >= 1 && day <= 31 && h < 24 && mi < 60 && se < 60;
}

bool versionMatch(std::string_view name) {
    const std::string s(name);
    for (std::size_t i = 0; i + kVersionLen <= s.size(); ++i) {
        if (s[i] == '-' && s[i + 1] == 'v' && versionFieldsOk(s, i))
            return true;
    }
    return false;
}

// rclone path.Ext 口径的 base/ext；".file"（base 空）时 rclone 交换两者
void splitExt(const std::string &name, std::string &base, std::string &ext) {
    const std::size_t slash = name.find_last_of('/');
    const std::size_t start = (slash == std::string::npos) ? 0 : slash + 1;
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot < start) {
        base = name;
        ext.clear();
        return;
    }
    base = name.substr(0, dot);
    ext = name.substr(dot);
    if (base.empty()) {
        base = ext;
        ext.clear();
    }
}

std::vector<std::string> splitPath(const std::string &p) {
    std::vector<std::string> segs;
    std::size_t pos = 0;
    while (true) {
        const std::size_t slash = p.find('/', pos);
        if (slash == std::string::npos) {
            segs.push_back(p.substr(pos));
            break;
        }
        segs.push_back(p.substr(pos, slash - pos));
        pos = slash + 1;
    }
    return segs;
}

std::string joinPath(const std::vector<std::string> &segs) {
    std::string out;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i)
            out += '/';
        out += segs[i];
    }
    return out;
}

// 版本串剥出：后缀接在扩展名之前（base -v… + ext）；suffixOut 承接剥出的版本串
std::string versionStrip(std::string_view name, std::string &suffixOut) {
    suffixOut.clear();
    const std::string s(name);
    if (!versionMatch(s))
        return s;
    std::string base, ext;
    splitExt(s, base, ext);
    if (base.size() < kVersionLen || !versionFieldsOk(base, base.size() - kVersionLen))
        return s;
    const std::size_t at = base.size() - kVersionLen;
    suffixOut = base.substr(at);
    return base.substr(0, at) + ext;
}

std::string versionAdd(std::string_view name, const std::string &suffixIn) {
    if (suffixIn.empty())
        return std::string(name);
    std::string base, ext;
    splitExt(std::string(name), base, ext);
    return base + suffixIn + ext;
}

// ——obfuscate——
// 逐条对齐 rclone cipher.go 的 obfuscateSegment / deobfuscateSegment（v1.75.2）：
// 前缀数字 = Σ码点 % 256（Go 以 rune 迭代求和）；真正参与轮换的 dir 还要加上
// nameKey 全部字节之和（前缀数字不含它）——这使得混淆的逆变换依赖密钥；
// 非 ASCII 分两档轮换环：Latin-1 补充区 0xA0–0xFF 走 96 元环、≥0x100 按 256 块
// 轮换（结果越出 Unicode 或落进代理区则引用原字符）；无效 UTF-8 整名不轮换，
// 以 "!." 为前缀原样存放。

bool validRune(std::int32_t r) { return r >= 0 && r <= 0x10FFFF && !(r >= 0xD800 && r <= 0xDFFF); }

// 码点 → UTF-8（调用方保证 r 为合法码点）
void appendRune(std::string &out, std::int32_t r) {
    if (r < 0x80) {
        out += char(r);
    } else if (r < 0x800) {
        out += char(0xC0 | (r >> 6));
        out += char(0x80 | (r & 0x3F));
    } else if (r < 0x10000) {
        out += char(0xE0 | (r >> 12));
        out += char(0x80 | ((r >> 6) & 0x3F));
        out += char(0x80 | (r & 0x3F));
    } else {
        out += char(0xF0 | (r >> 18));
        out += char(0x80 | ((r >> 12) & 0x3F));
        out += char(0x80 | ((r >> 6) & 0x3F));
        out += char(0x80 | (r & 0x3F));
    }
}

// 各编码长度对应的最小合法码点：低于它即超长编码。Go 的 utf8 解码（以及
// utf8.ValidString）一律拒绝超长，C0 80 因此不得解出 NUL
std::int32_t utf8MinCodePoint(std::size_t len) {
    switch (len) {
    case 2:
        return 0x80;
    case 3:
        return 0x800;
    case 4:
        return 0x10000;
    default:
        return 0;
    }
}

// 严格 UTF-8 校验（Go utf8.ValidString 口径：拒绝无效首字节、断续字节、截断、
// 超长编码，以及代理区 / 越界码点）——混淆模式靠它决定"整名不轮换、以 !. 前缀
// 原样存放"，放过超长编码会让两侧对同一个磁盘名解出不同的明文
bool utf8Valid(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<std::uint8_t>(s[i]);
        std::size_t adv = 1;
        std::int32_t cp = b;
        if ((b & 0xE0) == 0xC0) {
            adv = 2;
            cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            adv = 3;
            cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            adv = 4;
            cp = b & 0x07;
        } else if (b >= 0x80) {
            return false; // 0x80–0xBF 续字节或 0xF8+ 非法首字节
        }
        if (i + adv > s.size())
            return false;
        for (std::size_t j = 1; j < adv; ++j) {
            const auto c = static_cast<std::uint8_t>(s[i + j]);
            if ((c & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp < utf8MinCodePoint(adv) || !validRune(cp))
            return false;
        i += adv;
    }
    return true;
}

// UTF-8 码点迭代：无效字节（含超长编码）产出 U+FFFD 并前进 1 字节（Go range 语义；
// 编码侧已预检合法，解码侧对上游产出的名字实际不会命中，手改数据按 Go 行为处理）
std::size_t nextRune(std::string_view s, std::size_t i, std::int32_t &cp) {
    const auto b = static_cast<std::uint8_t>(s[i]);
    if (b < 0x80) {
        cp = b; // ASCII：单字节直出
        return 1;
    }
    std::size_t adv = 1;
    std::int32_t v = b;
    if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
        adv = 2;
        v = (b & 0x1F) << 6;
    } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
        adv = 3;
        v = (b & 0x0F) << 12;
    } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
        adv = 4;
        v = (b & 0x07) << 18;
    }
    if (adv > 1) {
        bool ok = true;
        for (std::size_t j = 1; j < adv; ++j) {
            const auto c = static_cast<std::uint8_t>(s[i + j]);
            if ((c & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            v |= (c & 0x3F) << (6 * (adv - 1 - j));
        }
        if (ok && v >= utf8MinCodePoint(adv) && validRune(v)) {
            cp = v;
            return adv;
        }
    }
    cp = 0xFFFD;
    return 1;
}

// ——scrypt——

// N98 §3：keySize = 32 + 32 + 16 = 80；salt 空 ⇒ rclone 内置 salt；口令空不走 scrypt
const RcloneCryptBytes kDefaultSalt = {0xA8, 0x0D, 0xF4, 0x3A, 0x8F, 0xBD, 0x03, 0x08,
                                       0xA7, 0xCA, 0xB8, 0x3E, 0x58, 0x1F, 0x86, 0xB1};

} // namespace

// —————————————————————————— 公开面 ——————————————————————————

const char *RcloneCrypt::errorText(Error e) {
    switch (e) {
    case Error::BadBase32Encoding:
        return "bad base32 filename encoding";
    case Error::BadBase64Encoding:
        return "bad base64 filename encoding";
    case Error::NotAMultipleOfBlocksize:
        return "not a multiple of blocksize";
    case Error::TooShortAfterDecode:
        return "too short after decode";
    case Error::TooLongAfterDecode:
        return "too long after decode";
    case Error::PaddingNotFound:
        return "bad PKCS#7 padding - not padded";
    case Error::PaddingNotAMultiple:
        return "bad PKCS#7 padding - not a multiple of blocksize";
    case Error::PaddingTooLong:
        return "bad PKCS#7 padding - too long";
    case Error::PaddingTooShort:
        return "bad PKCS#7 padding - too short";
    case Error::PaddingNotAllTheSame:
        return "bad PKCS#7 padding - not all the same";
    case Error::NotAnEncryptedFile:
        return "not an encrypted file - does not match suffix";
    case Error::EncryptedFileTooShort:
        return "file is too short to be encrypted";
    case Error::EncryptedFileBadHeader:
        return "file has truncated block header";
    case Error::EncryptedBadMagic:
        return "not an encrypted file - bad magic string";
    case Error::EncryptedBadBlock:
        return "failed to authenticate decrypted block - bad password?";
    }
    return "unknown";
}

std::expected<std::shared_ptr<const RcloneCrypt>, std::string>
RcloneCrypt::create(const Params &p) {
    if (sodium_init() < 0)
        return std::unexpected("sodium_init failed");
    auto c = std::shared_ptr<RcloneCrypt>(new RcloneCrypt());
    c->m_params = p;
    // suffix 定形照 rclone setEncryptedSuffix："none" 大小写不敏感地归空，其余补前导点
    if (equalsIgnoreCase(c->m_params.suffix, "none"))
        c->m_params.suffix.clear();
    else if (!c->m_params.suffix.empty() && c->m_params.suffix.front() != '.')
        c->m_params.suffix.insert(c->m_params.suffix.begin(), '.');

    RcloneCryptBytes key(80, 0);
    if (!c->m_params.password.empty()) {
        const RcloneCryptBytes salt =
            c->m_params.salt.empty()
                ? kDefaultSalt
                : RcloneCryptBytes(c->m_params.salt.begin(), c->m_params.salt.end());
        if (crypto_pwhash_scryptsalsa208sha256_ll(
                reinterpret_cast<const std::uint8_t *>(c->m_params.password.data()),
                c->m_params.password.size(), salt.data(), salt.size(), 16384, 8, 1, key.data(),
                key.size()) != 0)
            return std::unexpected("scrypt failed (out of memory?)");
    }
    c->m_dataKey.assign(key.begin(), key.begin() + 32);
    c->m_nameKey.assign(key.begin() + 32, key.begin() + 64);
    c->m_nameTweak.assign(key.begin() + 64, key.begin() + 80);
    c->m_aes = createAesBlock();
    if (auto err = c->m_aes->init(c->m_nameKey.data(), c->m_nameKey.size()); !err)
        return std::unexpected(err.error());
    return c;
}

std::string RcloneCrypt::encryptSegment(const std::string &plain) const {
    if (plain.empty())
        return {};
    if (m_params.mode == NameMode::Obfuscate) {
        // 无效 UTF-8 整名不轮换："!." 前缀 + 原文（rclone 同口径）
        if (!utf8Valid(plain))
            return std::string("!.") + plain;
        std::int32_t dir = 0;
        for (std::size_t i = 0; i < plain.size();) {
            std::int32_t cp = 0;
            i += nextRune(plain, i, cp);
            dir += cp;
        }
        dir %= 256;
        std::string out = std::to_string(dir) + ".";
        // 前缀数字之后，dir 再加上 nameKey 全部字节之和才是真实轮换量
        std::int32_t aug = dir;
        for (const std::uint8_t k : m_nameKey)
            aug += k;
        for (std::size_t i = 0; i < plain.size();) {
            std::int32_t cp = 0;
            const std::size_t len = nextRune(plain, i, cp);
            if (cp == '!') {
                out += "!!";
            } else if (cp >= '0' && cp <= '9') {
                const int step = (aug % 9) + 1;
                out += char('0' + (cp - '0' + step) % 10);
            } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
                const int step = aug % 25 + 1;
                int pos = cp - 'A';
                if (pos >= 26)
                    pos -= 6;
                pos = (pos + step) % 52;
                if (pos >= 26)
                    pos += 6;
                out += char('A' + pos);
            } else if (cp >= 0xA0 && cp <= 0xFF) {
                // Latin-1 补充区：96 元环轮换
                const int step = (aug % 95) + 1;
                appendRune(out, 0xA0 + (cp - 0xA0 + step) % 96);
            } else if (cp >= 0x100) {
                // 其余 Unicode：按 256 块轮换；结果越出 Unicode 或落进代理区则
                // 引用原字符（'!' + 原文）
                const int step = (aug % 127) + 1;
                const std::int32_t base = cp - cp % 256;
                const std::int32_t nr = base + (cp - base + step) % 256;
                if (!validRune(nr)) {
                    out += '!';
                    out.append(plain, i, len);
                } else {
                    appendRune(out, nr);
                }
            } else {
                out += char(cp);
            }
            i += len;
        }
        return out;
    }
    const RcloneCryptBytes padded = pkcs7Pad(plain);
    RcloneCryptBytes ct;
    emeTransform(*m_aes, m_nameTweak.data(), padded.data(), padded.size(), false, ct);
    return m_params.encoding == NameEncoding::Base32 ? base32HexEncode(ct) : base64UrlEncode(ct);
}

std::expected<std::string, RcloneCrypt::Error>
RcloneCrypt::decryptSegment(const std::string &in) const {
    if (in.empty())
        return std::string();
    if (m_params.mode == NameMode::Obfuscate) {
        // 首个 '.' 切前缀与正文（版本串用 '-' 分隔，不产生额外 '.'）
        const std::size_t dot = in.find('.');
        if (dot == std::string::npos)
            return std::unexpected(Error::NotAnEncryptedFile);
        const std::string num = in.substr(0, dot);
        if (num == "!") { // 字面 '!' 前缀：原串无效 UTF-8，未轮换
            return in.substr(dot + 1);
        }
        // 数字前缀 → 轮换基量，接受域照 Go strconv.Atoi：至多一个符号、必须有至少
        // 一位数字、量值不得溢出。rclone 写出的前缀恒为 dir%256（≤255，至多三位），
        // 18 位上限远在一切真实名之外；旧写法只在 i>18 收口，int32 累加早在第 10 位
        // 就回绕（同一串两侧解出不同明文，且有符号溢出属 UB），空串与光杆符号则被
        // 当成 dir=0 解了下去——Go 在这两处都是 Atoi 报错 ⇒ 非加密文件
        std::size_t at = 0;
        bool neg = false;
        if (at < num.size() && (num[at] == '-' || num[at] == '+')) {
            neg = num[at] == '-';
            ++at;
        }
        const std::size_t digits = num.size() - at;
        if (digits == 0 || digits > 18)
            return std::unexpected(Error::NotAnEncryptedFile);
        std::int64_t dir = 0;
        for (std::size_t i = at; i < num.size(); ++i) {
            const char c = num[i];
            if (c < '0' || c > '9')
                return std::unexpected(Error::NotAnEncryptedFile);
            dir = dir * 10 + (c - '0');
        }
        dir = neg ? -dir : dir;
        // 与编码侧同源：dir 再加上 nameKey 全部字节之和（前缀数字不含它，混淆的
        // 逆变换因此依赖密钥）。商向零取整的 % 语义与 Go 一致
        for (const std::uint8_t k : m_nameKey)
            dir += k;
        const std::string body = in.substr(dot + 1);
        std::string res;
        bool inQuote = false;
        for (std::size_t i = 0; i < body.size();) {
            std::int32_t cp = 0;
            const std::size_t len = nextRune(body, i, cp);
            if (inQuote) {
                appendRune(res, cp); // 引用的下一字符原样（含无效引用场景）
                inQuote = false;
            } else if (cp == '!') {
                inQuote = true;
            } else if (cp >= '0' && cp <= '9') {
                const int step = int(dir % 9) + 1;
                int v = int(cp) - '0' - step;
                if (v < 0)
                    v += 10;
                res += char('0' + v);
            } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
                const int step = int(dir % 25) + 1;
                int pos = cp - 'A';
                if (pos >= 26)
                    pos -= 6;
                pos -= step;
                if (pos < 0)
                    pos += 52;
                if (pos >= 26)
                    pos += 6;
                res += char('A' + pos);
            } else if (cp >= 0xA0 && cp <= 0xFF) {
                const int step = int(dir % 95) + 1;
                std::int32_t nr = cp - 0xA0 - step;
                if (nr < 0)
                    nr += 96;
                appendRune(res, 0xA0 + nr);
            } else if (cp >= 0x100) {
                const int step = int(dir % 127) + 1;
                const std::int32_t base = cp - cp % 256;
                std::int32_t nr = cp - step;
                if (nr < base)
                    nr += 256;
                appendRune(res, nr);
            } else {
                appendRune(res, cp);
            }
            i += len;
        }
        return res;
    }

    // 解码 → 非空且 ≤2048 且 长度为 16 的倍数 → EME 解密 → PKCS7 unpad。三条判据的
    // 次序按 N98 §4 定稿（上游是先判 16 倍数、再判空、再判 2048）：只有错误码归属
    // 有别，两侧的接受域完全一致
    std::expected<Bytes, Error> rawErr = base32HexDecode(in);
    if (m_params.encoding == NameEncoding::Base64)
        rawErr = base64UrlDecode(in);
    if (!rawErr)
        return std::unexpected(rawErr.error());
    const Bytes raw = std::move(*rawErr);
    if (raw.empty())
        return std::unexpected(Error::TooShortAfterDecode);
    if (raw.size() > 2048)
        return std::unexpected(Error::TooLongAfterDecode);
    if (raw.size() % 16 != 0)
        return std::unexpected(Error::NotAMultipleOfBlocksize);

    // EME 结果长度恒与输入相等且 PKCS7 输入合法，此处不可能出错
    RcloneCryptBytes padded;
    emeTransform(*m_aes, m_nameTweak.data(), raw.data(), raw.size(), true, padded);
    auto plain = pkcs7Unpad(padded);
    if (!plain)
        return std::unexpected(plain.error());
    return std::string(plain->begin(), plain->end());
}

std::string RcloneCrypt::encryptPath(const std::string &plain, bool stripVersion) const {
    auto segs = splitPath(plain);
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const bool last = i + 1 == segs.size();
        if (!m_params.dirNameEncrypt && !last)
            continue; // 目录段留明文
        std::string ver;
        if (stripVersion && last && versionMatch(segs[i]))
            segs[i] = versionStrip(segs[i], ver);
        segs[i] = encryptSegment(segs[i]);
        if (stripVersion && last && !ver.empty())
            segs[i] = versionAdd(segs[i], ver);
    }
    return joinPath(segs);
}

std::expected<std::string, RcloneCrypt::Error> RcloneCrypt::decryptPath(const std::string &in,
                                                                        bool stripVersion) const {
    auto segs = splitPath(in);
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const bool last = i + 1 == segs.size();
        std::string ver;
        if (stripVersion && last && versionMatch(segs[i]))
            segs[i] = versionStrip(segs[i], ver);
        if (!m_params.dirNameEncrypt && !last)
            continue;
        auto dec = decryptSegment(segs[i]);
        if (!dec)
            return std::unexpected(dec.error());
        segs[i] = std::move(*dec);
        if (stripVersion && last && !ver.empty())
            segs[i] = versionAdd(segs[i], ver);
    }
    return joinPath(segs);
}

std::string RcloneCrypt::encryptFileName(std::string_view plain) const {
    if (m_params.mode == NameMode::Off)
        return std::string(plain) + m_params.suffix;
    return encryptPath(std::string(plain), true);
}

std::string RcloneCrypt::encryptDirName(std::string_view plain) const {
    if (m_params.mode == NameMode::Off || !m_params.dirNameEncrypt)
        return std::string(plain);
    // 目录名的版本串一并加密，保证"目录名单独加密"与"作为文件父目录加密"逐字相同
    return encryptPath(std::string(plain), false);
}

std::expected<std::string, RcloneCrypt::Error>
RcloneCrypt::decryptFileName(std::string_view in) const {
    if (m_params.mode == NameMode::Off) {
        const std::string s(in);
        // 剥后缀：空 suffix 时只拒空串（Go 的 remainingLength==0 || !HasSuffix 同判）
        std::string out;
        if (m_params.suffix.empty()) {
            out = s;
        } else {
            if (s.size() <= m_params.suffix.size() ||
                s.compare(s.size() - m_params.suffix.size(), m_params.suffix.size(),
                          m_params.suffix) != 0)
                return std::unexpected(Error::NotAnEncryptedFile);
            out = s.substr(0, s.size() - m_params.suffix.size());
        }
        if (out.empty())
            return std::unexpected(Error::NotAnEncryptedFile); // 剥完为空 ⇒ 非加密文件
        // 只剩版本串的名字也按"非加密文件"拒：Go DecryptFileName 的 off 支剥完还过
        // 一道 version.Remove 判空。off 模式自身从不产出带版本尾的名字，收下只会让
        // 列表多出一条打开必失败的行
        if (versionMatch(out)) {
            std::string ver;
            if (versionStrip(out, ver).empty())
                return std::unexpected(Error::NotAnEncryptedFile);
        }
        return out;
    }
    return decryptPath(std::string(in), true);
}

std::expected<std::string, RcloneCrypt::Error>
RcloneCrypt::decryptDirName(std::string_view in) const {
    if (m_params.mode == NameMode::Off || !m_params.dirNameEncrypt)
        return std::string(in);
    auto direct = decryptPath(std::string(in), false);
    if (direct)
        return direct;
    return decryptPath(std::string(in), true); // 遗留目录名（版本串留明文）兜底
}

std::int64_t RcloneCrypt::encryptedSize(std::int64_t plain) {
    const std::int64_t blocks = plain / kBlockDataSize, residue = plain % kBlockDataSize;
    std::int64_t enc = kHeaderSize + blocks * kBlockSize;
    if (residue)
        enc += kBlockOverhead + residue;
    return enc;
}

std::expected<std::int64_t, RcloneCrypt::Error> RcloneCrypt::decryptedSize(std::int64_t enc) {
    const std::int64_t body = enc - kHeaderSize;
    if (body < 0)
        return std::unexpected(Error::EncryptedFileTooShort);
    const std::int64_t blocks = body / kBlockSize, residue = body % kBlockSize;
    std::int64_t out = blocks * kBlockDataSize;
    // 0 < 残余 ≤ 16 ⇒ BadHeader（residue == 0 是整块收尾，合法）
    if (residue) {
        if (residue <= kBlockOverhead)
            return std::unexpected(Error::EncryptedFileBadHeader);
        out += residue - kBlockOverhead;
    }
    return out;
}

void RcloneCrypt::calculateUnderlying(std::int64_t offset, std::int64_t limit,
                                      std::int64_t &underlyingOffset, std::int64_t &underlyingLimit,
                                      std::int64_t &discard, std::int64_t &blocks) {
    blocks = offset / kBlockDataSize;
    discard = offset % kBlockDataSize;
    underlyingOffset = kHeaderSize + blocks * kBlockSize;
    underlyingLimit = -1;
    if (limit < 0)
        return;
    const std::int64_t beyond = limit - (kBlockDataSize - discard);
    std::int64_t toRead = 1;
    if (beyond > 0)
        toRead += beyond / kBlockDataSize + (beyond % kBlockDataSize ? 1 : 0);
    underlyingLimit = toRead * kBlockSize;
}

void RcloneCrypt::nonceAdd(std::uint8_t nonce[24], std::uint64_t x) {
    std::uint64_t carry = x;
    for (std::size_t i = 0; i < 24 && carry; ++i) {
        const std::uint64_t v = nonce[i] + (carry & 0xFF);
        nonce[i] = std::uint8_t(v & 0xFF);
        carry >>= 8;
        carry += v >> 8;
    }
}

std::expected<void, RcloneCrypt::Error> RcloneCrypt::openBlock(const std::uint8_t *box,
                                                               std::size_t boxLen,
                                                               const std::uint8_t nonce[24],
                                                               Bytes &plain) const {
    // 块至少要有 MAC + 1 字节密文：只有 MAC 的 16 字节按截断块头判错（Go fillBuffer
    // 的 n <= blockHeaderSize 同判；空消息验 MAC 会把"截断"报成"坏块"，两类错误在
    // 上游是分工明确的两个码）。经流层走不到这条——尾块 ≤16 字节时 decryptedSize
    // 在 open 期就 BadHeader 了，此处守的是直接调用原语的调用方
    if (boxLen <= kBlockOverhead)
        return std::unexpected(Error::EncryptedFileBadHeader);
    const std::size_t len = boxLen - kBlockOverhead;
    plain.assign(len, 0);
    if (crypto_secretbox_open_detached(plain.data(), box + kBlockOverhead, box,
                                       static_cast<unsigned long long>(len), nonce,
                                       m_dataKey.data()) != 0)
        return std::unexpected(Error::EncryptedBadBlock);
    return {};
}
