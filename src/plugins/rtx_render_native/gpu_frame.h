#pragma once
#include <QSize>
#include <QString>
#include <memory>
namespace usdcc::rtx {
class GpuFrame {
public:
    std::shared_ptr<GpuFrame> cleanFrame;
    virtual ~GpuFrame() = default;
    virtual QSize size() const = 0;
    virtual bool copied() const = 0;
    virtual void save(const QString& file) const = 0;
};
}
