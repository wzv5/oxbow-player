#pragma once

#include "pch.h"

#include "ScriptPortStub.h"

// 发送记账：按 script-message-to 的线格式拆出的 (owner, name, args)
struct SendRecord {
    QString owner;
    QString name;
    QStringList args;
};

// MpvApiPort 的录制替身：每次 command 记一条账并放行一个令牌，取一条消费一条；
// 返回值可配置，用来模拟 script-message-to 找不到目标 client 的失败码。
// 令牌按发送次数计数，故 takeOne 之前的失败发送同样要消费掉，别留给下一个用例。
class RecordingPort : public ScriptPortStub {
  public:
    int command(const QStringList &cmd) override {
        SendRecord record;
        record.owner = cmd.value(1);
        record.name = cmd.value(2);
        record.args = cmd.mid(3);
        {
            QMutexLocker lock(&m_mutex);
            m_records.append(record);
        }
        m_sent.release();
        return m_rc; // 命令在工作线程落这里，返回值跨线程读
    }

    SendRecord takeOne() {
        m_sent.acquire();
        QMutexLocker lock(&m_mutex);
        return m_records.takeFirst();
    }

    // 有界取：不发送就不该无限等下去
    bool tryTakeOne(SendRecord *out, int ms) {
        if (!m_sent.tryAcquire(1, ms))
            return false;
        QMutexLocker lock(&m_mutex);
        *out = m_records.takeFirst();
        return true;
    }

    int recordCount() const {
        QMutexLocker lock(&m_mutex);
        return m_records.size();
    }

    void setReturnCode(int rc) { m_rc = rc; }

  private:
    mutable QMutex m_mutex;
    QList<SendRecord> m_records;
    QSemaphore m_sent;
    std::atomic_int m_rc = 0;
};
