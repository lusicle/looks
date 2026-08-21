// Minimal intrusive COM pointer shared by the platform TUs - no ATL.
// Move-only: a copyable raw-Release holder is a double-release.
#pragma once

namespace looks::platform {

template <typename T>
class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    T** put() {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

}  // namespace looks::platform
