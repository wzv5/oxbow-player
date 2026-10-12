// N98 §8：AES 抽象的共享落点——两后端共用的密钥长度校验单点。
// 平台差异全部住在 rclonecrypt_aes_cng.cpp / rclonecrypt_aes_openssl.cpp 两个
// 后端 TU 内，本文件不含任何平台分支。

#include "RcloneCryptAes.h"

bool aesKeyLenValid(std::size_t keyLen) { return keyLen == 16 || keyLen == 24 || keyLen == 32; }
