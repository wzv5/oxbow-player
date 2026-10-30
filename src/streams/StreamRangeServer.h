#pragma once

#include "ByteStream.h"
#include "StreamEndpoint.h"
#include "pch.h"

// N145：回环 HTTP Range 桥——把一条可寻位、总长已知的 ByteStream 以 HTTP Range 形式
// 暴露在 127.0.0.1 的临时端口上，供 ffmpeg 作 -i 输入（moov 在尾的 mp4 需要可 seek
// 的输入，管道喂不进去）。
//
// 协议面只做 ffmpeg 真正用到的子集：GET / HEAD、路径精确等于 token、单段 Range（末端
// 钳制、suffix 超长取整文件）、start >= size 与 bytes=-0 落 416、多段与畸形 Range 按
// 无 Range 的 200 全量。每请求一条连接（Connection: close），交付完由本侧发 FIN 收尾。
//
// 服务形态是一个服务线程 + 事件循环多路复用，所有连接并行。两件事由此同时成立：
//   - ByteStream 说"一条流只被单一消费者线程按序驱动 read / seek"，而一次取数只有一条
//     解密流。并行是在这同一条线程里交替的：seek + read 成对出现、中间不处理事件，
//     契约原样成立——既不加锁，也不必每连接各起一条流。
//   - 响应体只在 socket 有空间时才喂，某个对端不再读这条响应只是它自己不推进，占不到
//     别人的执行流。这条界限不是装饰：`bytes=X-` 按 RFC 要答成"X 到文件尾"，首问就是
//     一份几百 MB 的承诺，而 ffmpeg 只要头部那几十 KB 便去 seek——串行会把随后的尾问
//     一起挡在这条兑现不了的承诺后面，症状是"数据早齐了而进程不退出"，退出码与产出
//     字节数却与本地直读一致，从结果面看不出来。串行还非得判"它是不是弃连了"，而实测
//     ffmpeg 的 seek 是先开新一问、旧连接撂着不关也不留错误码——那种判据只能拟合某个
//     客户端的行为，不是协议，换版本换构建即失效。
//
// openStream → 建监听 → 起事件循环全在服务线程，满足端点"绝不在 GUI 线程起流"；
// QTcpServer、各 socket 与响应对象都在这条线程内创建使用，不跨线程携带 QObject。
// 阻塞只剩 openStream 一处（端点契约明允）。stop() 在调用线程非阻塞（置旗 + 流 cancel），
// 服务线程由看门狗观察该旗后退出循环；析构无条件 join。日志只记端口与 token 前 6 位，
// 不打印完整 URL。
class StreamRangeServer final : public QThread {
    Q_OBJECT
  public:
    explicit StreamRangeServer(QObject *parent = nullptr);
    ~StreamRangeServer() override;

    // 发起一次服务：endpoint + identity 存妥后在服务线程内起流。endpoint 不得为空
    // （run 内直接解引用）；端点起出的流必须可寻位（run 内断言）——这两条都由调用方的
    // supportsSecondaryDecode 白名单隐含担保，落空即调用方违约。每取数一实例，
    // 实例不复用（重复 launch 属调用方违约）
    void launch(const std::shared_ptr<StreamEndpoint> &endpoint, const QString &identity);

    // 构造线程非阻塞收口：置取消旗 + cancel 已打开的流；服务线程随即退出循环、
    // 关闭监听与全部连接、释放流（QThread::finished 随达，销毁钩子由调用方接）。
    void stop();

  signals:
    // 监听就绪：url = http://127.0.0.1:<port>/<token>，plainSize = 明文总长
    void ready(const QUrl &url, qint64 plainSize);
    // openStream 失败（错误文案透传）或监听失败；open 期被取消不发
    void failed(const QString &reason);

  protected:
    void run() override;

  private:
    // 服务线程退出（含早退）时摘掉成员侧那份流：run 的局部 shared_ptr 出作用域即
    // 完成实际释放，成员不留一份把流的寿命拖到对象 deleteLater 之后
    void releaseStream();

    std::shared_ptr<StreamEndpoint> m_endpoint; // launch 写入（线程启动前），run 只读
    QString m_identity;
    QString m_token; // 128 位十六进制，launch 期生成
    std::atomic<bool> m_cancel{false};
    QMutex m_streamMutex;                 // 只保护 m_stream 的写入与 stop 侧访问
    std::shared_ptr<ByteStream> m_stream; // run 内 openStream 成功后写入
};
