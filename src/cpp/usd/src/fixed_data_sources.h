#pragma once

// Hydra data sources that hold what they're given: what OpenUSD's hd/retainedDataSource.h has,
// for usdcc's code and tests. That header doesn't compile as C++20 with GCC: it writes a
// constructor as HdRetainedTypedSampledDataSource<bool>(...), which C++20 forbids.

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace usdcc::usd::hd {

// HD_DECLARE_DATASOURCE names these without OpenUSD's namespace.
using PXR_NS::HdDataSourceAllocator;
using PXR_NS::HdDataSourceBase;
using PXR_NS::HdDataSourceBaseHandle;

// A value of T that's the same at every time.
template <typename T>
class FixedTypedDataSource : public PXR_NS::HdTypedSampledDataSource<T> {
public:
    HD_DECLARE_DATASOURCE(FixedTypedDataSource<T>);
    using Time = PXR_NS::HdSampledDataSource::Time;

    PXR_NS::VtValue GetValue(Time) override { return PXR_NS::VtValue(m_value); }
    T GetTypedValue(Time) override { return m_value; }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time>*) override { return false; }

private:
    explicit FixedTypedDataSource(T value) : m_value(std::move(value)) {}
    T m_value;
};

// A value of any type that's the same at every time.
class FixedSampledDataSource : public PXR_NS::HdSampledDataSource {
public:
    HD_DECLARE_DATASOURCE(FixedSampledDataSource);

    PXR_NS::VtValue GetValue(Time) override { return m_value; }
    bool GetContributingSampleTimesForInterval(Time, Time, std::vector<Time>*) override { return false; }

private:
    explicit FixedSampledDataSource(PXR_NS::VtValue value) : m_value(std::move(value)) {}
    PXR_NS::VtValue m_value;
};

// Named children.
class FixedContainerDataSource : public PXR_NS::HdContainerDataSource {
public:
    HD_DECLARE_DATASOURCE(FixedContainerDataSource);
    using Children = std::vector<std::pair<PXR_NS::TfToken, PXR_NS::HdDataSourceBaseHandle>>;

    PXR_NS::TfTokenVector GetNames() override {
        PXR_NS::TfTokenVector names;
        names.reserve(m_children.size());
        for (const auto& [name, child] : m_children) names.push_back(name);
        return names;
    }
    PXR_NS::HdDataSourceBaseHandle Get(const PXR_NS::TfToken& name) override {
        const auto it = std::find_if(m_children.begin(), m_children.end(), [&name](const auto& child) { return child.first == name; });
        return it == m_children.end() ? nullptr : it->second;
    }

private:
    explicit FixedContainerDataSource(Children children = {}) : m_children(std::move(children)) {}
    Children m_children;
};

}  // namespace usdcc::usd::hd
