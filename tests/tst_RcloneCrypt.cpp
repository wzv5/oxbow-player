// RcloneCrypt 算法层单元测试（N98 §9）：向量数据复用 rclone（MIT）
// backend/crypt/cipher_test.go 与 backend/crypt/pkcs7/pkcs7_test.go 的成套期望值
// （Go 代码一行不搬，单测内注明出处「文件 + 符号名」）。覆盖：scrypt 密钥向量
// （TestKey）、编码逐长度期望（TestEncodeFileNameBase32/64）、零密钥名字加密
// （TestEncryptSegmentBase32/64）、解码错误码表（TestDecryptSegmentBase32）、
// 三模式名字层 + 版本串（TestStandard/NonStandardEncrypt/DecryptFileName、
// TestVersionedDirName）、尺寸换算两张表（TestEncryptedSize/TestDecryptedSize）、
// calculateUnderlying 全表（TestDecrypterCalculateUnderlying）、nonce 算术链
// （TestNonceIncrement/TestNonceAdd）、PKCS7 边界与五错误分支（pkcs7_test.go 的
// TestPad/TestUnpad 在块长 16 上的投影）、内容层黄金密文（TestEncryptData 的
// file0/file1/file16，经 RcloneCryptByteStream 验证解密/头校验/坏块语义）。
// secretbox 独立向量（spec §9 的 nacl/secretbox_test.go）不在本机 rclone 检出内，
// 由黄金密文向量代偿——它们本身就是 rclone 对 secretbox 落地的端到端校验。
// 多块流行为（跨块读、边界 seek、pass_bad_blocks 只归零坏块）用 RcloneCryptFixture.h
// 的正向封装合成真密文覆盖，不依赖外部数据目录。
// 一类一可执行（R5）：ctest --test-dir build -C Debug -R RcloneCrypt 跑本套件。

#include "pch.h"

#include <QtTest/QtTest>

#include "RcloneCryptFixture.h"
#include "rclonecrypt/RcloneCrypt.h"
#include "streams/RcloneCryptByteStream.h"

using Bytes = RcloneCrypt::Bytes;

namespace {

// 零密钥标准模式（测试向量均为零密钥口径，TestKey 的首条性质）
std::shared_ptr<const RcloneCrypt>
zeroCipher(RcloneCrypt::NameEncoding enc = RcloneCrypt::NameEncoding::Base32,
           bool dirNameEncrypt = true, RcloneCrypt::NameMode mode = RcloneCrypt::NameMode::Standard,
           const std::string &suffix = ".bin", bool passBadBlocks = false) {
    RcloneCrypt::Params p;
    p.mode = mode;
    p.encoding = enc;
    p.dirNameEncrypt = dirNameEncrypt;
    p.suffix = suffix;
    p.passBadBlocks = passBadBlocks;
    auto c = RcloneCrypt::create(p);
    if (!c)
        qFatal("zeroCipher: create failed: %s", c.error().c_str());
    return *c;
}

QString toQ(const std::string &s) { return QString::fromStdString(s); }

Bytes strBytes(const char *s, std::size_t len) { return Bytes(s, s + len); }

// 内存流：黄金密文、多块推进与坏块语义验证用
class MemStream final : public ByteStream {
  public:
    explicit MemStream(QByteArray data) : m_data(std::move(data)) {}

    qint64 read(char *out, qint64 maxBytes) override {
        if (m_pos >= qint64(m_data.size()))
            return kEof;
        const qint64 n = qMin<qint64>(maxBytes, qint64(m_data.size()) - m_pos);
        std::memcpy(out, m_data.constData() + m_pos, std::size_t(n));
        m_pos += n;
        return n;
    }
    bool isSeekable() const override { return true; }
    qint64 seek(qint64 to) override {
        m_pos = to;
        return to;
    }
    qint64 size() const override { return qint64(m_data.size()); }
    Stats stats() const override { return {}; }

  private:
    QByteArray m_data;
    qint64 m_pos = 0;
};

std::shared_ptr<ByteStream> makeMemStream(const Bytes &data) {
    return std::make_shared<MemStream>(
        QByteArray(reinterpret_cast<const char *>(data.data()), int(data.size())));
}

Bytes hexBytes(const char *hexPairs) {
    Bytes out;
    const QList<QByteArray> parts = QByteArray(hexPairs).split(' ');
    for (const QByteArray &p : parts) {
        if (p.isEmpty())
            continue;
        out.push_back(std::uint8_t(QString::fromLatin1(p).toUInt(nullptr, 16)));
    }
    return out;
}

Bytes memBytes(const QByteArray &a) {
    return Bytes(reinterpret_cast<const std::uint8_t *>(a.constData()),
                 reinterpret_cast<const std::uint8_t *>(a.constData() + a.size()));
}

// TestEncryptData 的三份黄金密文（cipher_test.go · file0 / file1 / file16）
constexpr const char *kFile0Hex =
    "0x52 0x43 0x4c 0x4f 0x4e 0x45 0x00 0x00 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 "
    "0x09 0x0a 0x0b 0x0c 0x0d 0x0e 0x0f 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18";
constexpr const char *kFile1Hex =
    "0x52 0x43 0x4c 0x4f 0x4e 0x45 0x00 0x00 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 "
    "0x09 0x0a 0x0b 0x0c 0x0d 0x0e 0x0f 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 "
    "0x09 0x5b 0x44 0x6c 0xd6 0x23 0x7b 0xbc 0xb0 0x8d 0x09 0xfb 0x52 0x4c 0xe5 0x65 "
    "0xAA";
constexpr const char *kFile16Hex =
    "0x52 0x43 0x4c 0x4f 0x4e 0x45 0x00 0x00 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 "
    "0x09 0x0a 0x0b 0x0c 0x0d 0x0e 0x0f 0x10 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 "
    "0xb9 0xc4 0x55 0x2a 0x27 0x10 0x06 0x29 0x18 0x96 0x0a 0x3e 0x60 0x8c 0x29 0xb9 "
    "0xaa 0x8a 0x5e 0x1e 0x16 0x5b 0x6d 0x07 0x5d 0xe4 0xe9 0xbb 0x36 0x7f 0xd6 0xd4";

} // namespace

class TestRcloneCrypt : public QObject {
    Q_OBJECT

  private slots:
    // TestKey（cipher_test.go）：scrypt 三件套 + salt 大小写敏感 + 空口令全零
    void keyVectors();
    // TestEncodeFileNameBase32 / Base64：编码逐长度期望（0..16 字节）
    void encodeFileNameBase32();
    void encodeFileNameBase64();
    // TestEncryptSegmentBase32 / Base64：零密钥名字加密（EME+PKCS7+编码三段合一）
    void encryptSegmentBase32();
    void encryptSegmentBase64();
    // TestDecryptSegmentBase32 / Base64：解码错误码表
    void decryptSegmentErrors();
    // TestStandardEncryptFileNameBase32 / Base64：整名加密（目录名开 / 关 + 版本串）
    void standardEncryptFileName();
    // TestNonStandardEncryptFileName：off 与 obfuscate（含版本串与 Unicode 向量）
    void nonStandardEncryptFileName();
    // TestNonStandardDecryptFileName：off / obfuscate 的解码判定与后缀
    void nonStandardDecryptFileName();
    // TestEncDecMatches：三模式往返不变式
    void encDecMatches();
    // TestVersionedDirName：目录名与同名文件的版本形态差异 + 遗留名兜底
    void versionedDirName();
    // TestEncryptedSize / TestDecryptedSize：尺寸换算两张表
    void sizeTables();
    // TestDecrypterCalculateUnderlying：明文 (offset,limit) → 底层四元组全表
    void calculateUnderlyingTable();
    // TestNonceIncrement / TestNonceAdd：24 字节小端 nonce 算术
    void nonceArithmetic();
    // pkcs7_test.go · TestPad / TestUnpad 在块长 16 上的投影（含五个错误分支）
    void pkcs7Vectors();
    // TestEncryptData 黄金密文（file0/file1/file16）+ 流层：头校验、坏块语义
    void goldenCiphertextViaStream();
    // 混淆前缀的接受域（N98 变更记录：Atoi 语义收口——空 / 光杆符号 / 超范围皆拒）
    void obfuscatePrefixDomain();
    // off 模式的版本串与 suffix 定形（纯版本串判非加密、NONE 大小写不敏感）
    void offModeSuffixAndVersionRules();
    // 名字层长度上限：2032 字节明文 ⇒ 2048 字节分组（EME m=128）往返
    void nameLengthUpperBound();
    // openBlock 原语边界：只有 MAC 的 16 字节块按截断块头判错
    void openBlockRejectsMacOnlyChunk();
    // 合成多块密文：顺序读尽、跨块边界 seek、seek 到总长与越界
    void multiBlockStreamReadAndSeek();
    // 合成多块密文：坏块报错 / pass_bad_blocks 只归零坏块且流不中断
    void multiBlockBadBlockSemantics();
};

void TestRcloneCrypt::keyVectors() {
    // TestKey 的四组三段向量（dataKey / nameKey / nameTweak）+ 空口令全零性质
    struct Row {
        const char *password;
        const char *salt;
        const char *dataKeyHex;
        const char *nameKeyHex;
        const char *tweakHex;
    };
    const Row rows[] = {
        {"potato", "", "7455C71AB17C865B8471F47B79ACB07EB31D5678B80C7E2EAF4FC8066A9EE468",
         "765DA27AB15D77F95796711F7B93AD63BBB484072E7180A8D17A9BBEC14270D0",
         "C18D5932F55B2828C5E1E87215520310"},
        {"Potato", "", "AEEA6AD347DF75B963CE12F57623E946D42ED8BF3E928B39243794133E5EF75E",
         "54F7026E8AFC560A86636AAB2C9C5162E51A122351836EAF50420F981C860A19",
         "F8C1B6272D529B4A8FDAEB424A28DDF3"},
        {"potato", "sausage", "8E9B6B99F8690467A071F9CB92D0AA787F8FF178BEC96F999FD5206E644A1B50",
         "3EA95EF681782DC9D9955D225BFD442C6F5D6897B029015C6F462E2A9DAE2CE3",
         "F17FD7141D65274F363FC2A04DD2148A"},
        {"potato", "Sausage", "DA818C67EF110FC8D5C8624B7FE29E3535B08D798489ACCBA0FF0272031A5E64",
         "02817E7BEA99815AD02DB96448B028277C20B4D4A468AD4E5C290F79EFEEDB3B",
         "9AB50B3DCB605955A54DE6B6470323E2"},
    };
    auto hex = [](const Bytes &b) {
        QString s;
        for (std::uint8_t byte : b)
            s += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
        return s;
    };
    for (const Row &row : rows) {
        RcloneCrypt::Params p;
        p.password = row.password;
        p.salt = row.salt;
        auto c = RcloneCrypt::create(p);
        if (!c)
            QFAIL(qPrintable(
                QStringLiteral("Key should derive: %1").arg(QString::fromStdString(c.error()))));
        QCOMPARE(hex((*c)->dataKey()), QLatin1String(row.dataKeyHex));
        QCOMPARE(hex((*c)->nameKey()), QLatin1String(row.nameKeyHex));
        QCOMPARE(hex((*c)->nameTweak()), QLatin1String(row.tweakHex));
    }
    // 空口令 ⇒ 三段全零，不走 scrypt
    auto zero = zeroCipher();
    QCOMPARE(zero->dataKey(), Bytes(32, 0));
    QCOMPARE(zero->nameKey(), Bytes(32, 0));
    QCOMPARE(zero->nameTweak(), Bytes(16, 0));
}

void TestRcloneCrypt::encodeFileNameBase32() {
    // cipher_test.go · TestEncodeFileNameBase32（base32hex 小写去尾）
    const std::pair<std::string, std::string> rows[] = {
        {"", ""},
        {"1", "64"},
        {"12", "64p0"},
        {"123", "64p36"},
        {"1234", "64p36d0"},
        {"12345", "64p36d1l"},
        {"123456", "64p36d1l6o"},
        {"1234567", "64p36d1l6org"},
        {"12345678", "64p36d1l6orjg"},
        {"123456789", "64p36d1l6orjge8"},
        {"1234567890", "64p36d1l6orjge9g"},
        {"12345678901", "64p36d1l6orjge9g64"},
        {"123456789012", "64p36d1l6orjge9g64p0"},
        {"1234567890123", "64p36d1l6orjge9g64p36"},
        {"12345678901234", "64p36d1l6orjge9g64p36d0"},
        {"123456789012345", "64p36d1l6orjge9g64p36d1l"},
        {"1234567890123456", "64p36d1l6orjge9g64p36d1l6o"},
    };
    for (const auto &row : rows) {
        const Bytes in(row.first.begin(), row.first.end());
        QCOMPARE(QString::fromStdString(RcloneCrypt::base32HexEncode(in)),
                 QString::fromStdString(row.second));
        auto back = RcloneCrypt::base32HexDecode(row.second);
        QVERIFY(bool(back));
        QVERIFY(*back == in);
    }
}

void TestRcloneCrypt::encodeFileNameBase64() {
    // cipher_test.go · TestEncodeFileNameBase64（url-safe 无填充）
    const std::pair<std::string, std::string> rows[] = {
        {"", ""},
        {"1", "MQ"},
        {"12", "MTI"},
        {"123", "MTIz"},
        {"1234", "MTIzNA"},
        {"12345", "MTIzNDU"},
        {"123456", "MTIzNDU2"},
        {"1234567", "MTIzNDU2Nw"},
        {"12345678", "MTIzNDU2Nzg"},
        {"123456789", "MTIzNDU2Nzg5"},
        {"1234567890", "MTIzNDU2Nzg5MA"},
        {"12345678901", "MTIzNDU2Nzg5MDE"},
        {"123456789012", "MTIzNDU2Nzg5MDEy"},
        {"1234567890123", "MTIzNDU2Nzg5MDEyMw"},
        {"12345678901234", "MTIzNDU2Nzg5MDEyMzQ"},
        {"123456789012345", "MTIzNDU2Nzg5MDEyMzQ1"},
        {"1234567890123456", "MTIzNDU2Nzg5MDEyMzQ1Ng"},
    };
    for (const auto &row : rows) {
        const Bytes in(row.first.begin(), row.first.end());
        QCOMPARE(QString::fromStdString(RcloneCrypt::base64UrlEncode(in)),
                 QString::fromStdString(row.second));
        auto back = RcloneCrypt::base64UrlDecode(row.second);
        QVERIFY(bool(back));
        QVERIFY(*back == in);
    }
}

void TestRcloneCrypt::encryptSegmentBase32() {
    // cipher_test.go · TestEncryptSegmentBase32（零密钥；无版本串单段名经
    // encryptFileName 等价触达——后者此时就是 encryptSegment）
    auto c = zeroCipher();
    const std::pair<std::string, std::string> rows[] = {
        {"", ""},
        {"1", "p0e52nreeaj0a5ea7s64m4j72s"},
        {"12", "l42g6771hnv3an9cgc8cr2n1ng"},
        {"123", "qgm4avr35m5loi1th53ato71v0"},
        {"1234", "8ivr2e9plj3c3esisjpdisikos"},
        {"12345", "rh9vu63q3o29eqmj4bg6gg7s44"},
        {"123456", "bn717l3alepn75b2fb2ejmi4b4"},
        {"1234567", "n6bo9jmb1qe3b1ogtj5qkf19k8"},
        {"12345678", "u9t24j7uaq94dh5q53m3s4t9ok"},
        {"123456789", "37hn305g6j12d1g0kkrl7ekbs4"},
        {"1234567890", "ot8d91eplaglb62k2b1trm2qv0"},
        {"12345678901", "h168vvrgb53qnrtvvmb378qrcs"},
        {"123456789012", "s3hsdf9e29ithrqbjqu01t8q2s"},
        {"1234567890123", "cf3jimlv1q2oc553mv7s3mh3eo"},
        {"12345678901234", "moq0uqdlqrblrc5pa5u5c7hq9g"},
        {"123456789012345", "eeam3li4rnommi3a762h5n7meg"},
        {"1234567890123456", "mijbj0frqf6ms7frcr6bd9h0env53jv96pjaaoirk7forcgpt70g"},
    };
    for (const auto &row : rows) {
        QCOMPARE(toQ(c->encryptFileName(row.first)), QString::fromStdString(row.second));
        auto back = c->decryptFileName(row.second);
        QVERIFY2(bool(back), row.second.c_str());
        QCOMPARE(toQ(*back), QString::fromStdString(row.first));
        // base32 大小写不敏感（TestEncryptSegmentBase32 的 caseInsensitive 支路）
        auto upper = c->decryptFileName(QString::fromStdString(row.second).toUpper().toStdString());
        QVERIFY2(bool(upper), row.second.c_str());
        QCOMPARE(toQ(*upper), QString::fromStdString(row.first));
    }
}

void TestRcloneCrypt::encryptSegmentBase64() {
    // cipher_test.go · TestEncryptSegmentBase64（零密钥）
    auto c = zeroCipher(RcloneCrypt::NameEncoding::Base64);
    const std::pair<std::string, std::string> rows[] = {
        {"", ""},
        {"1", "yBxRX25ypgUVyj8MSxJnFw"},
        {"12", "qQUDHOGN_jVdLIMQzYrhvA"},
        {"123", "1CxFf2Mti1xIPYlGruDh-A"},
        {"1234", "RL-xOTmsxsG7kuTy2XJUxw"},
        {"12345", "3FP_GHoeBJdq0yLgaED8IQ"},
        {"123456", "Xc4T1Gqrs3OVYnrE6dpEWQ"},
        {"1234567", "uZeEzssOnDWHEOzLqjwpog"},
        {"12345678", "8noiTP5WkkbEuijsPhOpxQ"},
        {"123456789", "GeNxgLA0wiaGAKU3U7qL4Q"},
        {"1234567890", "x1DUhdmqoVWYVBLD3dha-A"},
        {"12345678901", "iEyP_3BZR6vvv_2WM6NbZw"},
        {"123456789012", "4OPGvS4SZdjvS568APUaFw"},
        {"1234567890123", "Y8c5Wr8OhYYUo7fPwdojdg"},
        {"12345678901234", "tjQPabXW112wuVF8Vh46TA"},
        {"123456789012345", "c5Vh1kTd8WtIajmFEtz2dA"},
        {"1234567890123456", "tKa5gfvTzW4d-2bMtqYgdf5Rz-k2ZqViW6HfjbIZ6cE"},
    };
    for (const auto &row : rows) {
        QCOMPARE(toQ(c->encryptFileName(row.first)), QString::fromStdString(row.second));
        auto back = c->decryptFileName(row.second);
        QVERIFY2(bool(back), row.second.c_str());
        QCOMPARE(toQ(*back), QString::fromStdString(row.first));
    }
}

void TestRcloneCrypt::decryptSegmentErrors() {
    // cipher_test.go · TestDecryptSegmentBase32 / TestDecryptSegmentBase64 的错误码表
    auto b32 = zeroCipher();
    auto b64 = zeroCipher(RcloneCrypt::NameEncoding::Base64);
    const std::string longName32(3328, 'a');
    const std::string longName64(2816, 'a');

    QCOMPARE(b32->decryptFileName("64=").error(), RcloneCrypt::Error::BadBase32Encoding);
    QCOMPARE(b32->decryptFileName("!").error(), RcloneCrypt::Error::BadBase32Encoding);
    QCOMPARE(b32->decryptFileName(longName32).error(), RcloneCrypt::Error::TooLongAfterDecode);
    // 编码后长度非 16 倍数 / padding 非法——期望码经公开编码面构造
    const std::string enc1 = RcloneCrypt::base32HexEncode(Bytes{'a'});
    QCOMPARE(b32->decryptFileName(enc1).error(), RcloneCrypt::Error::NotAMultipleOfBlocksize);
    const std::string s15 = "123456789abcdef";
    const std::string enc15 = RcloneCrypt::base32HexEncode(strBytes(s15.c_str(), 15));
    QCOMPARE(b32->decryptFileName(enc15).error(), RcloneCrypt::Error::NotAMultipleOfBlocksize);
    const std::string s16 = "123456789abcdef0";
    const std::string enc16 = RcloneCrypt::base32HexEncode(strBytes(s16.c_str(), 16));
    QCOMPARE(b32->decryptFileName(enc16).error(), RcloneCrypt::Error::PaddingTooLong);

    // base64 的解码失败另立错误码（上游此处直接外抛 stdlib 的 CorruptInputError，
    // 没有具名错误）——否则文案会指着 base32 说事
    QCOMPARE(b64->decryptFileName("6H=").error(), RcloneCrypt::Error::BadBase64Encoding);
    QCOMPARE(b64->decryptFileName("!").error(), RcloneCrypt::Error::BadBase64Encoding);
    QCOMPARE(b64->decryptFileName("MQ=").error(), RcloneCrypt::Error::BadBase64Encoding);
    QCOMPARE(b64->decryptFileName("M").error(), RcloneCrypt::Error::BadBase64Encoding);
    QCOMPARE(b64->decryptFileName(longName64).error(), RcloneCrypt::Error::TooLongAfterDecode);
}

void TestRcloneCrypt::standardEncryptFileName() {
    // cipher_test.go · TestStandardEncryptFileNameBase32（目录名开 / 关两态 + 版本串）
    auto on = zeroCipher();
    auto off = zeroCipher(RcloneCrypt::NameEncoding::Base32, false);
    QCOMPARE(toQ(on->encryptFileName("1")), QStringLiteral("p0e52nreeaj0a5ea7s64m4j72s"));
    QCOMPARE(toQ(on->encryptFileName("1/12")),
             QStringLiteral("p0e52nreeaj0a5ea7s64m4j72s/l42g6771hnv3an9cgc8cr2n1ng"));
    QCOMPARE(toQ(on->encryptFileName("1/12/123")),
             QStringLiteral("p0e52nreeaj0a5ea7s64m4j72s/l42g6771hnv3an9cgc8cr2n1ng/"
                            "qgm4avr35m5loi1th53ato71v0"));
    // 版本串留明文、接在扩展名之前
    QCOMPARE(toQ(on->encryptFileName("1-v2001-02-03-040506-123")),
             QStringLiteral("p0e52nreeaj0a5ea7s64m4j72s-v2001-02-03-040506-123"));
    QCOMPARE(toQ(on->encryptFileName("1/12-v2001-02-03-040506-123")),
             QStringLiteral("p0e52nreeaj0a5ea7s64m4j72s/"
                            "l42g6771hnv3an9cgc8cr2n1ng-v2001-02-03-040506-123"));
    // 目录名加密关闭：目录段留明文
    QCOMPARE(toQ(off->encryptFileName("1/12")), QStringLiteral("1/l42g6771hnv3an9cgc8cr2n1ng"));
    QCOMPARE(toQ(off->encryptFileName("1/12/123")),
             QStringLiteral("1/12/qgm4avr35m5loi1th53ato71v0"));

    // base64 行（TestStandardEncryptFileNameBase64）
    auto on64 = zeroCipher(RcloneCrypt::NameEncoding::Base64);
    auto off64 = zeroCipher(RcloneCrypt::NameEncoding::Base64, false);
    QCOMPARE(toQ(on64->encryptFileName("1/12/123")),
             QStringLiteral("yBxRX25ypgUVyj8MSxJnFw/qQUDHOGN_jVdLIMQzYrhvA/"
                            "1CxFf2Mti1xIPYlGruDh-A"));
    QCOMPARE(toQ(off64->encryptFileName("1/12")), QStringLiteral("1/qQUDHOGN_jVdLIMQzYrhvA"));

    // TestStandardDecryptFileNameBase32：解码 + 目录名开关两态
    QCOMPARE(toQ(on->decryptFileName("p0e52nreeaj0a5ea7s64m4j72s/l42g6771hnv3an9cgc8cr2n1ng/"
                                     "qgm4avr35m5loi1th53ato71v0")
                     .value()),
             QStringLiteral("1/12/123"));
    QCOMPARE(toQ(off->decryptFileName("1/l42g6771hnv3an9cgc8cr2n1ng").value()),
             QStringLiteral("1/12"));
}

void TestRcloneCrypt::nonStandardEncryptFileName() {
    // cipher_test.go · TestNonStandardEncryptFileName
    // Off 模式（默认 .bin / 自定义 .jpg / 字面 none）
    auto off = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off);
    QCOMPARE(toQ(off->encryptFileName("1/12/123")), QStringLiteral("1/12/123.bin"));
    auto offJpg =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, ".jpg");
    QCOMPARE(toQ(offJpg->encryptFileName("1/12/123")), QStringLiteral("1/12/123.jpg"));
    auto offNone =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, "none");
    QCOMPARE(toQ(offNone->encryptFileName("1/12/123")), QStringLiteral("1/12/123"));

    // Obfuscate 模式（零密钥）
    auto ob = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(toQ(ob->encryptFileName("1/12/123/!hello")),
             QStringLiteral("49.6/99.23/150.890/53.!!lipps"));
    QCOMPARE(toQ(ob->encryptFileName("1/12/123/!hello-v2001-02-03-040506-123")),
             QStringLiteral("49.6/99.23/150.890/53-v2001-02-03-040506-123.!!lipps"));
    QCOMPARE(toQ(ob->encryptFileName("1/12/123/hello-v2001-02-03-040506-123.txt")),
             QStringLiteral("49.6/99.23/150.890/162.uryyB-v2001-02-03-040506-123.GKG"));
    // 非 ASCII：Latin-1 环与 Unicode 256 块环
    QCOMPARE(toQ(ob->encryptFileName("\u00a1")), QStringLiteral("161.\u00e4"));
    QCOMPARE(toQ(ob->encryptFileName("\u03a0")), QStringLiteral("160.\u03c2"));
    // 目录名加密关闭：目录段原样
    auto obNoDir =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, false, RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(toQ(obNoDir->encryptFileName("1/12/123/!hello")),
             QStringLiteral("1/12/123/53.!!lipps"));
    QCOMPARE(toQ(obNoDir->encryptFileName("\u00a1")), QStringLiteral("161.\u00e4"));
}

void TestRcloneCrypt::nonStandardDecryptFileName() {
    // cipher_test.go · TestNonStandardDecryptFileName（base32 行；base64 行同构）
    auto off = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off);
    QCOMPARE(toQ(off->decryptFileName("1/12/123.bin").value()), QStringLiteral("1/12/123"));
    QCOMPARE(off->decryptFileName("1/12/123.bix").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(off->decryptFileName(".bin").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    // off 模式：只剥后缀，版本串原样保留
    QCOMPARE(toQ(off->decryptFileName("1/12/123-v2001-02-03-040506-123.bin").value()),
             QStringLiteral("1/12/123-v2001-02-03-040506-123"));
    QCOMPARE(toQ(off->decryptFileName("1/12/123-v1970-01-01-010101-123-v2001-02-03-040506-123.bin")
                     .value()),
             QStringLiteral("1/12/123-v1970-01-01-010101-123-v2001-02-03-040506-123"));
    auto offJpg =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, ".jpg");
    QCOMPARE(toQ(offJpg->decryptFileName("1/12/123.jpg").value()), QStringLiteral("1/12/123"));
    auto offNone =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, "none");
    QCOMPARE(toQ(offNone->decryptFileName("1/12/123").value()), QStringLiteral("1/12/123"));

    auto ob = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(toQ(ob->decryptFileName("!.hello").value()), QStringLiteral("hello"));
    QCOMPARE(ob->decryptFileName("hello").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(toQ(ob->decryptFileName("161.\u00e4").value()), QStringLiteral("\u00a1"));
    QCOMPARE(toQ(ob->decryptFileName("160.\u03c2").value()), QStringLiteral("\u03a0"));
    auto obNoDir =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, false, RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(toQ(obNoDir->decryptFileName("1/12/123/53.!!lipps").value()),
             QStringLiteral("1/12/123/!hello"));
}

void TestRcloneCrypt::encDecMatches() {
    // cipher_test.go · TestEncDecMatches：三模式往返不变式（base32 / base64）
    for (RcloneCrypt::NameEncoding enc :
         {RcloneCrypt::NameEncoding::Base32, RcloneCrypt::NameEncoding::Base64}) {
        const std::pair<RcloneCrypt::NameMode, std::string> rows[] = {
            {RcloneCrypt::NameMode::Standard, "1/2/3/4"},
            {RcloneCrypt::NameMode::Off, "1/2/3/4"},
            {RcloneCrypt::NameMode::Obfuscate, "1/2/3/4/!hello\u03a0"},
            {RcloneCrypt::NameMode::Obfuscate, "Avatar The Last Airbender"},
        };
        for (const auto &row : rows) {
            auto c = zeroCipher(enc, true, row.first);
            auto back = c->decryptFileName(c->encryptFileName(row.second));
            QVERIFY2(bool(back), row.second.c_str());
            QCOMPARE(toQ(*back), QString::fromStdString(row.second));
        }
    }
}

void TestRcloneCrypt::versionedDirName() {
    // cipher_test.go · TestVersionedDirName：目录名整段加密（版本串一并），与
    // "作为文件父目录加密"逐字相同；文件名保留版本串明文；遗留名兜底解码
    const std::string dir = "dir-v2001-02-03-040506-123";
    for (RcloneCrypt::NameMode mode :
         {RcloneCrypt::NameMode::Standard, RcloneCrypt::NameMode::Obfuscate}) {
        auto c = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, mode);
        const std::string encryptedDir = c->encryptDirName(dir);
        const std::string encryptedFile = c->encryptFileName(dir + "/file.txt");
        // EncryptDirName(dir) == path.Dir(EncryptFileName(dir + "/file.txt"))
        QCOMPARE(QString::fromStdString(encryptedDir),
                 QString::fromStdString(encryptedFile.substr(0, encryptedFile.find_last_of('/'))));
        // 目录名往返
        QCOMPARE(toQ(c->decryptDirName(encryptedDir).value()), toQ(dir));
        // 同名文件保留版本串明文（与目录形态不同），且仍可解回
        const std::string fileForm = c->encryptFileName(dir);
        QVERIFY(encryptedDir != fileForm);
        QVERIFY(fileForm.find("-v2001-02-03-040506-123") != std::string::npos);
        QCOMPARE(toQ(c->decryptFileName(fileForm).value()), toQ(dir));
        // 遗留目录名（文件形态的密文名）按文件形态兜底解出
        QCOMPARE(toQ(c->decryptDirName(fileForm).value()), toQ(dir));
        // 两种形态都解不开的名字仍是错误
        QVERIFY(!c->decryptDirName("!!!-v2001-02-03-040506-123"));
    }
}

void TestRcloneCrypt::sizeTables() {
    // cipher_test.go · TestEncryptedSize / TestDecryptedSize
    const auto encryptedSize = RcloneCrypt::encryptedSize;
    const auto decryptedSize = RcloneCrypt::decryptedSize;
    const std::pair<std::int64_t, std::int64_t> enc[] = {
        {0, 32},
        {1, 32 + 16 + 1},
        {65536, 32 + 16 + 65536},
        {65537, 32 + 16 + 65536 + 16 + 1},
        {std::int64_t(1) << 20, 32 + 16 * (16 + 65536)},
        {(std::int64_t(1) << 20) + 65535, 32 + 16 * (16 + 65536) + 16 + 65535},
        {std::int64_t(1) << 30, 32 + std::int64_t(16384) * (16 + 65536)},
        {(std::int64_t(1) << 40) + 1, 32 + std::int64_t(16777216) * (16 + 65536) + 16 + 1},
    };
    for (const auto &row : enc) {
        QCOMPARE(encryptedSize(row.first), row.second);
        QCOMPARE(*decryptedSize(row.second), row.first);
    }
    // TestDecryptedSize 的错误行
    QCOMPARE(decryptedSize(0).error(), RcloneCrypt::Error::EncryptedFileTooShort);
    QCOMPARE(decryptedSize(1).error(), RcloneCrypt::Error::EncryptedFileTooShort);
    QCOMPARE(decryptedSize(7).error(), RcloneCrypt::Error::EncryptedFileTooShort);
    QCOMPARE(decryptedSize(32 + 1).error(), RcloneCrypt::Error::EncryptedFileBadHeader);
    QCOMPARE(decryptedSize(32 + 16).error(), RcloneCrypt::Error::EncryptedFileBadHeader);
    QCOMPARE(decryptedSize(32 + 16 + 65536 + 1).error(),
             RcloneCrypt::Error::EncryptedFileBadHeader);
    QCOMPARE(decryptedSize(32 + 16 + 65536 + 16).error(),
             RcloneCrypt::Error::EncryptedFileBadHeader);
}

void TestRcloneCrypt::calculateUnderlyingTable() {
    // cipher_test.go · TestDecrypterCalculateUnderlying 全表
    const auto calculateUnderlying = RcloneCrypt::calculateUnderlying;
    constexpr std::int64_t hdr = 32, blk = 65552, data = 65536;
    struct Row {
        std::int64_t offset, limit, wantOffset, wantLimit, wantDiscard, wantBlocks;
    };
    const Row rows[] = {
        {0, -1, hdr, -1, 0, 0},
        {1, -1, hdr, -1, 1, 0},
        {data - 1, -1, hdr, -1, data - 1, 0},
        {data, -1, hdr + blk, -1, 0, 1},
        {data + 1, -1, hdr + blk, -1, 1, 1},
        {0, 1, hdr, blk, 0, 0},
        {1, 1, hdr, blk, 1, 0},
        {data - 1, 1, hdr, blk, data - 1, 0},
        {data, 1, hdr + blk, blk, 0, 1},
        {data + 1, 1, hdr + blk, blk, 1, 1},
        {0, 100, hdr, blk, 0, 0},
        {1, 100, hdr, blk, 1, 0},
        {data - 1, 100, hdr, 2 * blk, data - 1, 0},
        {data, 100, hdr + blk, blk, 0, 1},
        {data + 1, 100, hdr + blk, blk, 1, 1},
        {0, data - 1, hdr, blk, 0, 0},
        {1, data - 1, hdr, blk, 1, 0},
        {data - 1, data - 1, hdr, 2 * blk, data - 1, 0},
        {data, data - 1, hdr + blk, blk, 0, 1},
        {data + 1, data - 1, hdr + blk, blk, 1, 1},
        {0, data, hdr, blk, 0, 0},
        {1, data, hdr, 2 * blk, 1, 0},
        {data - 1, data, hdr, 2 * blk, data - 1, 0},
        {data, data, hdr + blk, blk, 0, 1},
        {data + 1, data, hdr + blk, 2 * blk, 1, 1},
        {0, data + 1, hdr, 2 * blk, 0, 0},
        {1, data + 1, hdr, 2 * blk, 1, 0},
        {data - 1, data + 1, hdr, 2 * blk, data - 1, 0},
        {data, data + 1, hdr + blk, 2 * blk, 0, 1},
        {data + 1, data + 1, hdr + blk, 2 * blk, 1, 1},
    };
    for (const Row &row : rows) {
        std::int64_t uo = 0, ul = 0, discard = 0, blocks = 0;
        calculateUnderlying(row.offset, row.limit, uo, ul, discard, blocks);
        QCOMPARE(uo, row.wantOffset);
        QCOMPARE(ul, row.wantLimit);
        QCOMPARE(discard, row.wantDiscard);
        QCOMPARE(blocks, row.wantBlocks);
    }
}

void TestRcloneCrypt::nonceArithmetic() {
    // cipher_test.go · TestNonceIncrement / TestNonceAdd（0xFF 连珠、跨 8 字节
    // 进位、uint64 上限行、24 字节全溢出）
    auto makeNonce = [](std::initializer_list<int> bytes) {
        Bytes n(24, 0);
        std::size_t i = 0;
        for (int b : bytes)
            n[i++] = std::uint8_t(b);
        return n;
    };
    Bytes n = makeNonce({0x00});
    RcloneCrypt::nonceAdd(n.data(), 1);
    QVERIFY(n == makeNonce({0x01}));
    n = makeNonce({0xFF, 0xFF, 0xFF});
    RcloneCrypt::nonceAdd(n.data(), 1);
    QVERIFY(n == makeNonce({0x00, 0x00, 0x00, 0x01}));
    // TestNonceIncrement 末行：24 字节全 0xFF + 1 ⇒ 归零
    n = Bytes(24, 0xFF);
    RcloneCrypt::nonceAdd(n.data(), 1);
    QVERIFY(n == Bytes(24, 0));
    // TestNonceAdd 的 uint64 上限行（跨 8 字节进位）
    n = makeNonce({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    RcloneCrypt::nonceAdd(n.data(), 0xFFFFFFFFFFFFFFFFull);
    QVERIFY(n == makeNonce({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}));
    n = makeNonce({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    RcloneCrypt::nonceAdd(n.data(), 0xFFFFFFFFFFFFFFFFull);
    // 2^80-1 + 2^64-1 = 2^80 + 2^64 - 2：byte0=0xFE、bytes1-7=0xFF、byte8+=0x00、
    // byte9+=0x00、byte10=0x01（bit 80）
    QVERIFY(n == makeNonce({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01}));
    // 24 字节全 0xFF + uint64 上限：进位链被 0xFF 全段吸收，byte8 起全 0
    // （2^192-1 + 2^64-1 ≡ 2^64-2 (mod 2^192)）
    n = Bytes(24, 0xFF);
    RcloneCrypt::nonceAdd(n.data(), 0xFFFFFFFFFFFFFFFFull);
    QVERIFY(n ==
            makeNonce({0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
}

void TestRcloneCrypt::pkcs7Vectors() {
    // pkcs7_test.go · TestPad / TestUnpad（本实现块长恒 16）
    const auto pkcs7Pad = RcloneCrypt::pkcs7Pad;
    const auto pkcs7Unpad = RcloneCrypt::pkcs7Unpad;
    // 补齐：空 ⇒ 整块 16；整块收尾再补整块
    QVERIFY(pkcs7Pad("") == Bytes(16, 16));
    QVERIFY(pkcs7Pad("a") ==
            strBytes("a\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f", 16));
    QVERIFY(
        pkcs7Pad("abcdefghijklmnop") ==
        strBytes("abcdefghijklmnop\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10\x10",
                 32));
    // 去补的五个错误分支（Empty / 非 16 倍数 / 末字节 >16 / 末字节 ==0 / 不全等）
    QCOMPARE(pkcs7Unpad(Bytes()).error(), RcloneCrypt::Error::PaddingNotFound);
    QCOMPARE(pkcs7Unpad(Bytes(15, 'x')).error(), RcloneCrypt::Error::PaddingNotAMultiple);
    Bytes tooLong = Bytes(16, 0x08);
    tooLong.back() = 0xFF;
    QCOMPARE(pkcs7Unpad(tooLong).error(), RcloneCrypt::Error::PaddingTooLong);
    Bytes alsoTooLong = Bytes(16, 0x08);
    alsoTooLong.back() = 0x11;
    QCOMPARE(pkcs7Unpad(alsoTooLong).error(), RcloneCrypt::Error::PaddingTooLong);
    Bytes tooShort = Bytes(16, 0x08);
    tooShort.back() = 0x00;
    QCOMPARE(pkcs7Unpad(tooShort).error(), RcloneCrypt::Error::PaddingTooShort);
    Bytes mixed = Bytes(16, 0x05);
    mixed[11] = 0x04; // 末 5 字节不完全相同
    QCOMPARE(pkcs7Unpad(mixed).error(), RcloneCrypt::Error::PaddingNotAllTheSame);
    Bytes mixed2(16, 0x08);
    mixed2[8] = 0x07; // TestUnpad 的 \x07\x08… 行在 16 上的投影
    QCOMPARE(pkcs7Unpad(mixed2).error(), RcloneCrypt::Error::PaddingNotAllTheSame);
    // 正常去补
    auto padded = pkcs7Pad("123456");
    auto back = pkcs7Unpad(padded);
    QVERIFY(bool(back));
    QVERIFY(*back == strBytes("123456", 6));
}

void TestRcloneCrypt::goldenCiphertextViaStream() {
    // cipher_test.go · TestEncryptData 的黄金密文 + TestNewDecrypter 错误路径 +
    // TestDecrypterRead 截断行为 + passBadBlocks（TestDecrypterRead 末段）
    const Bytes file0 = hexBytes(kFile0Hex);
    const Bytes file1 = hexBytes(kFile1Hex);
    const Bytes file16 = hexBytes(kFile16Hex);
    auto c = zeroCipher();

    // 黄金密文解密：file1 ⇒ {1}、file16 ⇒ {1..16}；读尽后再读 ⇒ kEof
    const std::pair<Bytes, Bytes> goldens[] = {
        {file1, Bytes{0x01}},
        {file16, Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}},
    };
    for (const auto &golden : goldens) {
        auto s = RcloneCryptByteStream::open(c, makeMemStream(golden.first));
        QVERIFY2(bool(s), "golden stream should open");
        QCOMPARE((*s)->size(), qint64(golden.second.size()));
        Bytes out(golden.second.size() + 16, 0);
        const qint64 got = (*s)->read(reinterpret_cast<char *>(out.data()), qint64(out.size()));
        QCOMPARE(got, qint64(golden.second.size()));
        out.resize(std::size_t(got));
        QVERIFY(out == golden.second);
        QCOMPARE((*s)->read(reinterpret_cast<char *>(out.data()), 16), ByteStream::kEof);
    }

    // 不足 32 字节 ⇒ TooShort；magic 不符 ⇒ BadMagic（TestNewDecrypter 错误路径）
    auto tooShort =
        RcloneCryptByteStream::open(c, makeMemStream(Bytes(file1.begin(), file1.begin() + 31)));
    QCOMPARE(tooShort.error(), QStringLiteral("file is too short to be encrypted"));
    Bytes badMagic = file0;
    badMagic[3] ^= 0x01;
    auto bad = RcloneCryptByteStream::open(c, makeMemStream(badMagic));
    QCOMPARE(bad.error(), QStringLiteral("not an encrypted file - bad magic string"));

    // 截断行为（TestDecrypterRead）：恰 32 字节 = 合法空文件（读出即 kEof）；
    // 头 + 残块 ≤ MAC 边界 ⇒ decryptedSize 判 BadHeader，open 即失败
    {
        auto ok = RcloneCryptByteStream::open(c, makeMemStream(file0));
        QVERIFY(bool(ok));
        char buf[16];
        QCOMPARE((*ok)->read(buf, 16), ByteStream::kEof);
    }
    {
        auto truncated = RcloneCryptByteStream::open(
            c, makeMemStream(Bytes(file16.begin(), file16.begin() + 48)));
        QCOMPARE(truncated.error(), QStringLiteral("file has truncated block header"));
    }

    // 坏块：篡改任一密文字节 ⇒ 读错误；pass_bad_blocks ⇒ 该块归零、流不中断
    Bytes corrupted = file16;
    corrupted.back() ^= 0xFF;
    auto badBlock = RcloneCryptByteStream::open(c, makeMemStream(corrupted));
    QVERIFY(bool(badBlock));
    char buf[64];
    QCOMPARE((*badBlock)->read(buf, 64), ByteStream::kError);

    auto passing = zeroCipher(RcloneCrypt::NameEncoding::Base32, true,
                              RcloneCrypt::NameMode::Standard, ".bin", true);
    auto passStream = RcloneCryptByteStream::open(passing, makeMemStream(corrupted));
    QVERIFY(bool(passStream));
    QCOMPARE((*passStream)->read(buf, 64), qint64(16));
    QVERIFY(std::all_of(buf, buf + 16, [](char ch) { return ch == 0; }));
    // 坏块之后流不中断的另一面：size 声明与位置守恒
    QCOMPARE((*passStream)->size(), qint64(16));
}

void TestRcloneCrypt::obfuscatePrefixDomain() {
    // 混淆前缀的接受域对齐 Go strconv.Atoi（cipher.go · deobfuscateSegment）：至多
    // 一个符号、至少一位数字、量值不溢出。旧写法 int32 累加在第 10 位就回绕（同一串
    // 两侧解出不同明文），空与光杆符号前缀更被当成 dir=0 一路解了下去
    auto ob = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Obfuscate);
    QCOMPARE(ob->decryptFileName(".hello").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(ob->decryptFileName("-.hello").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(ob->decryptFileName("+.hello").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(ob->decryptFileName("1x.hello").error(), RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(ob->decryptFileName(std::string(19, '9') + ".hello").error(),
             RcloneCrypt::Error::NotAnEncryptedFile);

    // 17 位前缀在 int64 里成立，步进必须按 64 位取模。期望值独立算：把 A-Za-z 看成
    // 0..51 的 52 元环，逆向一格即减步进（旧写法此处步进已回绕，必判不过）
    const std::int64_t big = 12345678901234567ll;
    const int step = int(big % 25) + 1;
    const int ordinalOfB = 26 + ('b' - 'a');
    const int back = (ordinalOfB - step + 52) % 52;
    const QChar expected = back < 26 ? QChar('A' + back) : QChar('a' + back - 26);
    const QString input = QString::number(qint64(big)) + QStringLiteral(".b");
    QCOMPARE(toQ(ob->decryptFileName(input.toStdString()).value()), QString(expected));

    // rclone 自己产出的名字（前缀恒为 dir%256，至多三位）往返不变
    QCOMPARE(toQ(ob->decryptFileName(ob->encryptFileName("hello.txt")).value()),
             QStringLiteral("hello.txt"));
}

void TestRcloneCrypt::offModeSuffixAndVersionRules() {
    auto off = zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off);
    // 剥完后缀只剩版本串 ⇒ 非加密文件（Go DecryptFileName 的 off 支在剥完后还过
    // 一道 version.Remove 判空）；off 模式自身从不产出这种名，收下只会多一行必失败的条目
    QCOMPARE(off->decryptFileName("-v2001-02-03-040506-123.bin").error(),
             RcloneCrypt::Error::NotAnEncryptedFile);
    // 版本串前有实名的照常解，且版本串留在明文名里（off 模式不剥它）
    QCOMPARE(toQ(off->decryptFileName("abc-v2001-02-03-040506-123.bin").value()),
             QStringLiteral("abc-v2001-02-03-040506-123"));
    // 空 suffix 形态下裸版本串同样拒
    auto offNone =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, "none");
    QCOMPARE(offNone->decryptFileName("-v2001-02-03-040506-123").error(),
             RcloneCrypt::Error::NotAnEncryptedFile);
    QCOMPARE(toQ(offNone->decryptFileName("a.bin").value()), QStringLiteral("a.bin"));
    // "none" 大小写不敏感（Go 的 strings.EqualFold）——此前 "NONE" 会被当普通后缀
    auto offUpperNone =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, "NONE");
    QCOMPARE(toQ(offUpperNone->encryptFileName("a/b.mkv")), QStringLiteral("a/b.mkv"));
    // 无点的后缀补前导点（Go setEncryptedSuffix）
    auto offBareBin =
        zeroCipher(RcloneCrypt::NameEncoding::Base32, true, RcloneCrypt::NameMode::Off, "bin");
    QCOMPARE(toQ(offBareBin->encryptFileName("a")), QStringLiteral("a.bin"));
    QCOMPARE(toQ(offBareBin->decryptFileName("a.bin").value()), QStringLiteral("a"));
}

void TestRcloneCrypt::nameLengthUpperBound() {
    // 可解名的上界：解码侧要求 raw ≤2048 且为 16 的倍数 ⇒ 明文最长 2032，
    // 重加密正好落在 EME 的 m=128（128 × 16 字节）上限
    auto c = zeroCipher();
    const std::string plain(2032, 'x');
    const std::string encoded = c->encryptFileName(plain);
    const Bytes raw = *RcloneCrypt::base32HexDecode(encoded);
    QCOMPARE(int(raw.size()), 2048);
    QCOMPARE(toQ(c->decryptFileName(encoded).value()), QString::fromStdString(plain));
}

void TestRcloneCrypt::openBlockRejectsMacOnlyChunk() {
    // Go fillBuffer 判的是 n <= blockHeaderSize（"块头 + 1 字节密文"才成块）：
    // 只有 MAC 的 16 字节按截断块头判错，不走空消息验签（那会报成坏块）
    auto c = zeroCipher();
    const Bytes nonce(24, 0);
    Bytes macOnly(16, 0);
    Bytes out;
    QCOMPARE(c->openBlock(macOnly.data(), macOnly.size(), nonce.data(), out).error(),
             RcloneCrypt::Error::EncryptedFileBadHeader);
    // 17 字节（MAC + 1 字节密文）进入正常验签路径：零密钥下 MAC 不符 ⇒ 坏块
    macOnly.push_back(0x00);
    QCOMPARE(c->openBlock(macOnly.data(), macOnly.size(), nonce.data(), out).error(),
             RcloneCrypt::Error::EncryptedBadBlock);
}

void TestRcloneCrypt::multiBlockStreamReadAndSeek() {
    // 合成三块真密文（64 KiB + 64 KiB + 1000 明文）：§5 的读侧口径——顺序读尽、
    // 块边界 seek、seek 到总长与越界、二次 seek 回退——全部在此落，不靠外部数据
    const auto c = zeroCipher();
    Bytes fileNonce(24);
    for (std::size_t i = 0; i < fileNonce.size(); ++i)
        fileNonce[i] = std::uint8_t(i + 1);
    const std::vector<Bytes> blocks{RcloneCryptFixture::pattern(65536, 0),
                                    RcloneCryptFixture::pattern(65536, 1),
                                    RcloneCryptFixture::pattern(1000, 2)};
    const Bytes sealed = RcloneCryptFixture::sealFile(c->dataKey(), fileNonce, blocks);
    const Bytes plain = RcloneCryptFixture::concatBlocks(blocks);

    auto opened = RcloneCryptByteStream::open(c, makeMemStream(sealed));
    QVERIFY2(bool(opened), "crypt stream should open over sealed data");
    const auto stream = *opened;
    QCOMPARE(stream->size(), qint64(plain.size()));

    // 顺序读尽：4 KiB 一片，累计与明文逐字相同，读尽后恒 kEof
    Bytes got;
    std::vector<char> chunk(4096);
    while (true) {
        const qint64 n = stream->read(chunk.data(), qint64(chunk.size()));
        if (n == ByteStream::kEof)
            break;
        QVERIFY2(n > 0, "sequential read must not error");
        got.insert(got.end(), chunk.begin(), chunk.begin() + std::size_t(n));
    }
    QVERIFY(got == plain);
    QCOMPARE(stream->read(chunk.data(), 16), ByteStream::kEof);

    // 块边界与尾段：seek 恒返回目标偏移，同偏移读出的字节与明文一致
    const qint64 offsets[] = {
        0, 65535, 65536, 65537, 131071, 131072, qint64(plain.size()) - 3, qint64(plain.size())};
    for (qint64 off : offsets) {
        QCOMPARE(stream->seek(off), off);
        const qint64 want = qMin<qint64>(8, qint64(plain.size()) - off);
        if (want <= 0) {
            QCOMPARE(stream->read(chunk.data(), 8), ByteStream::kEof); // 恰好 EOF 也是这条规则
            continue;
        }
        const qint64 n = stream->read(chunk.data(), want);
        QCOMPARE(n, want);
        // 逐字节比较一律走 memcmp：读缓冲是 char（有符号），明文是 uint8_t，
        // std::equal 的逐元素 == 会把 -16 与 0xF0 判成不等（模式里含 ≥0x80 的字节）
        QVERIFY(std::memcmp(chunk.data(), plain.data() + std::size_t(off), std::size_t(n)) == 0);
    }

    // 越界 seek 恒成功返回目标（N98 §5 定案：钳位是 mpv 侧的事，本层零分支防御），
    // 随后的读按 kEof 收束
    QCOMPARE(stream->seek(qint64(plain.size()) + 4096), qint64(plain.size()) + 4096);
    QCOMPARE(stream->read(chunk.data(), 8), ByteStream::kEof);

    // 二次 seek 回退：块缓存作废后重取，同偏移仍是同一串字节
    QCOMPARE(stream->seek(10), qint64(10));
    QCOMPARE(stream->read(chunk.data(), 5), qint64(5));
    QVERIFY(std::memcmp(chunk.data(), plain.data() + 10, 5) == 0);

    // 单次 read 跨 0/1 块界
    QCOMPARE(stream->seek(65530), qint64(65530));
    const qint64 crossed = stream->read(chunk.data(), 20);
    QCOMPARE(crossed, qint64(20));
    QVERIFY(std::memcmp(chunk.data(), plain.data() + 65530, 20) == 0);
}

void TestRcloneCrypt::multiBlockBadBlockSemantics() {
    const auto c = zeroCipher();
    const Bytes fileNonce(24, 0xAB);
    const std::vector<Bytes> blocks{RcloneCryptFixture::pattern(65536, 0),
                                    RcloneCryptFixture::pattern(65536, 1),
                                    RcloneCryptFixture::pattern(1000, 2)};
    const Bytes sealed = RcloneCryptFixture::sealFile(c->dataKey(), fileNonce, blocks);
    const Bytes plain = RcloneCryptFixture::concatBlocks(blocks);
    // 篡改块 1 的密文区：块 1 起点 = 32 + 65552，跳过 16 字节 MAC 再偏 100
    Bytes corrupted = sealed;
    corrupted[std::size_t(32 + 65552 + 16 + 100)] ^= 0xFF;

    // 不开 pass_bad_blocks：好块照常交付，命中坏块即报错；坏块不污染流
    auto strict = RcloneCryptByteStream::open(c, makeMemStream(corrupted));
    QVERIFY(bool(strict));
    const auto strictStream = *strict;
    std::vector<char> chunk(4096);
    QCOMPARE(strictStream->seek(65526), qint64(65526));
    QCOMPARE(strictStream->read(chunk.data(), 10), qint64(10));         // 块 0 的尾部仍解得开
    QCOMPARE(strictStream->read(chunk.data(), 10), ByteStream::kError); // 一脚踏进块 1
    QCOMPARE(strictStream->seek(0), qint64(0));
    QCOMPARE(strictStream->read(chunk.data(), 8), qint64(8));
    QVERIFY(std::memcmp(chunk.data(), plain.data(), 8) == 0);

    // pass_bad_blocks：只有坏块归零，块 0 与块 2 照常，流一路走到 EOF
    auto passing = zeroCipher(RcloneCrypt::NameEncoding::Base32, true,
                              RcloneCrypt::NameMode::Standard, ".bin", true);
    auto opened = RcloneCryptByteStream::open(passing, makeMemStream(corrupted));
    QVERIFY(bool(opened));
    const auto passStream = *opened;
    Bytes got;
    while (true) {
        const qint64 n = passStream->read(chunk.data(), qint64(chunk.size()));
        if (n == ByteStream::kEof)
            break;
        QVERIFY2(n > 0, "pass_bad_blocks must not break the stream");
        got.insert(got.end(), chunk.begin(), chunk.begin() + std::size_t(n));
    }
    QCOMPARE(int(got.size()), int(plain.size()));
    QVERIFY(std::equal(got.begin(), got.begin() + 65536, plain.begin()));
    QVERIFY(
        std::all_of(got.begin() + 65536, got.begin() + 131072, [](char ch) { return ch == 0; }));
    QVERIFY(std::equal(got.begin() + 131072, got.end(), plain.begin() + 131072));
}

QTEST_GUILESS_MAIN(TestRcloneCrypt)
#include "tst_RcloneCrypt.moc"
