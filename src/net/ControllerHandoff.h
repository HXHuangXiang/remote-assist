#pragma once

namespace remote_assist {

// 单控制端会话的交接状态。WebSocket handler 销毁与输入状态释放来自不同线程：
// 只有旧会话已脱离且所有遗留 keyup/mouseup 已完成时，才能允许新控制端接管。
// 该类由 WsBroadcaster 的 clientMu_ 保护，刻意不自行加锁，避免与 socket 生命周期
// 锁产生反向锁序。
class ControllerHandoff {
public:
    // 仅在没有活动/关闭中的会话且不存在待释放输入时允许建立控制端。
    constexpr bool TryAttach() {
        if (!CanAccept()) {
            return false;
        }
        attached_ = true;
        closing_ = false;
        return true;
    }

    // 关闭 handler 前先调用，使采集和发送路径不再把该连接当作可用控制端，并
    // 阻止新控制端在遗留输入释放前接入。
    constexpr void BeginInputCleanup() {
        closing_ = true;
        inputCleanupPending_ = true;
    }

    // 仅在 Input::ReleaseAll 已将所有状态成功注入 keyup/mouseup 后调用。
    // 若 handler 已先脱离，此调用会立即重新开放接管；反之仍会等 Detach。
    constexpr void CompleteInputCleanup() { inputCleanupPending_ = false; }

    // handler 栈上的 WebSocket 即将销毁。输入清理可能早于或晚于该步骤完成。
    constexpr void Detach() {
        attached_ = false;
        closing_ = false;
    }

    constexpr bool CanAccept() const { return !attached_ && !inputCleanupPending_; }
    // 输入已经释放但旧 handler 尚在析构时，同样属于不可接管的短暂交接期。调用方
    // 应把它作为可重试状态，而非“另一位控制者仍在正常使用”。
    constexpr bool IsInputCleanupPending() const { return closing_ || inputCleanupPending_; }
    // 正在关闭的连接不再应接收视频帧，也不应使 CaptureLoop 继续编码。
    constexpr bool HasActiveController() const { return attached_ && !closing_; }

private:
    bool attached_ = false;
    bool closing_ = false;
    bool inputCleanupPending_ = false;
};

}  // namespace remote_assist
